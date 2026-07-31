"""
SFT training for the Chinese ESP32 model — Phase 1 of the SFT plan.

Continues from the pretrained checkpoint (runs_chinese/ple-zh2-s42.pt),
expands the vocab rows 5901 -> 5904 for the 3 SFT markers, and fine-tunes
with instruction loss masking (loss only on <|assistant|> answers).

Isolated from the English pipeline. Uses the same PLE architecture
(src/model.py) and char-level tokenizer (chinese/tokenizer.py).

Usage:
    uv run python chinese/sft/sft_train.py \
        --resume runs_chinese/ple-zh2-s42.pt \
        --sft-data data_chinese/sft/processed/sft_train.json \
        --val-data data_chinese/sft/processed/sft_val.json \
        --steps 3000 --tag zh2-sft
"""

import argparse
import json
import math
import os
import random
import sys
import time
from pathlib import Path

import numpy as np
import torch

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
SRC = PROJECT_ROOT / "src"
sys.path.insert(0, str(SRC))
sys.path.insert(0, str(PROJECT_ROOT))

from model import Config, TinyLM  # noqa: E402

DATA_CHINESE = PROJECT_ROOT / "data_chinese"
RUNS_SFT = PROJECT_ROOT / "runs_chinese" / "sft"


# ---------------------------------------------------------------------------
#  Dataset
# ---------------------------------------------------------------------------

class InstructionDataset(torch.utils.data.Dataset):
    """SFT samples with loss masks: loss only on assistant-answer tokens."""

    def __init__(self, data_path, seq_len=256):
        with open(data_path, encoding="utf-8") as f:
            d = json.load(f)
        self.samples = d["data"]
        self.seq_len = seq_len
        # Pre-truncate/pad nothing here — collate does it per batch

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, i):
        s = self.samples[i]
        return s["input_ids"], s["labels"]


class ContinuationDataset(torch.utils.data.Dataset):
    """Plain-text continuation chunks (anti-forgetting mix)."""

    def __init__(self, text_path, seq_len=256, seed=0):
        text = open(text_path, encoding="utf-8").read()
        self.seq_len = seq_len
        # Build a long list of char ids (SFT markers get UNK via tokenizer)
        from chinese.tokenizer import CharTokenizer
        tok = CharTokenizer.load(str(DATA_CHINESE / "tokenizer.json"))
        self.ids = []
        for ch in text:
            self.ids.append(tok.stoi.get(ch, tok.UNK))
        self.rng = random.Random(seed)

    def __len__(self):
        return max(1, len(self.ids) // self.seq_len)

    def __getitem__(self, i):
        start = self.rng.randint(0, len(self.ids) - self.seq_len - 1)
        chunk = self.ids[start:start + self.seq_len]
        return chunk, chunk  # labels = input for pure continuation


def collate_fn(batch, seq_len, pad_id, ignore=-100):
    """Pad a batch of (input_ids, labels) to fixed length."""
    xs, ys = [], []
    for x, y in batch:
        x = x[:seq_len]
        y = y[:seq_len]
        # truncate but keep at least the tail of answer? simple: truncate head if too long
        if len(x) < seq_len:
            x = x + [pad_id] * (seq_len - len(x))
            y = y + [ignore] * (seq_len - len(y))
        else:
            x = x[:seq_len]
            y = y[:seq_len]
        xs.append(x)
        ys.append(y)
    return (torch.tensor(xs, dtype=torch.long),
            torch.tensor(ys, dtype=torch.long))


class MixedSampler(torch.utils.data.Sampler):
    """Sample with instruction_ratio from SFT data, rest from continuation."""

    def __init__(self, n_inst, n_cont, ratio, seed=0):
        self.n_inst, self.n_cont = n_inst, n_cont
        self.ratio = ratio
        self.rng = random.Random(seed)

    def __iter__(self):
        for _ in range(self.n_inst):
            if self.rng.random() < self.ratio:
                yield self.rng.randint(0, self.n_inst - 1)
            else:
                yield self.rng.randint(0, max(0, self.n_cont - 1)) if self.n_cont else 0

    def __len__(self):
        return self.n_inst


# ---------------------------------------------------------------------------
#  Checkpoint helpers
# ---------------------------------------------------------------------------

def expand_vocab_rows(state, old_vocab, new_vocab):
    """Expand row-counted tensors (tok_emb/head/ple_table) old->new rows.

    New rows keep their random-init values (they were never trained). Existing
    rows are copied as-is, so char ids stay aligned.
    """
    new_state = {}
    for k, v in state.items():
        if v.ndim >= 2 and v.shape[0] == old_vocab and new_vocab > old_vocab:
            if k in ("tok_emb.weight", "head.weight", "ple_table.weight"):
                pad = torch.randn(new_vocab - old_vocab, *v.shape[1:]) * 0.02
                new_state[k] = torch.cat([v, pad], dim=0)
                print(f"  expanded {k}: {list(v.shape)} -> {list(new_state[k].shape)}")
                continue
        new_state[k] = v
    return new_state


def load_pretrained(path, new_vocab):
    """Load pretrained ckpt and rebuild the model with expanded vocab."""
    ck = torch.load(path, map_location="cpu", weights_only=False)
    old_cfg = ck["cfg"]
    new_cfg = Config(**{**old_cfg, "vocab_size": new_vocab})
    model = TinyLM(new_cfg)
    state = expand_vocab_rows(ck["state"], old_cfg["vocab_size"], new_vocab)
    model.load_state_dict(state, strict=False)
    print(f"Loaded pretrained: {path} (vocab {old_cfg['vocab_size']} -> {new_vocab})")
    return model, new_cfg


# ---------------------------------------------------------------------------
#  Training loop
# ---------------------------------------------------------------------------

def get_device():
    if torch.backends.mps.is_available():
        return "mps"
    if torch.cuda.is_available():
        return "cuda"
    return "cpu"


def lr_at(step, total, peak, warmup):
    if step < warmup:
        return peak * (step + 1) / warmup
    p = (step - warmup) / max(1, total - warmup)
    return 0.1 * peak + 0.9 * peak * 0.5 * (1 + math.cos(math.pi * p))


@torch.no_grad()
def evaluate(model, val_ds, device, seq_len, pad_id, iters=40):
    model.eval()
    loader = torch.utils.data.DataLoader(
        val_ds, batch_size=8, shuffle=False,
        collate_fn=lambda b: collate_fn(b, seq_len, pad_id))
    losses = []
    for i, (x, y) in enumerate(loader):
        if i >= iters:
            break
        x, y = x.to(device), y.to(device)
        logits, _ = model(x)
        loss = torch.nn.functional.cross_entropy(
            logits.view(-1, logits.size(-1)), y.reshape(-1), ignore_index=-100)
        losses.append(loss.item())
    model.train()
    return sum(losses) / max(1, len(losses))


def main():
    ap = argparse.ArgumentParser(description="SFT the Chinese PLE model")
    ap.add_argument("--resume", default="runs_chinese/ple-zh2-s42.pt",
                    help="pretrained checkpoint")
    ap.add_argument("--sft-data", default="data_chinese/sft/processed/sft_train.json")
    ap.add_argument("--val-data", default="data_chinese/sft/processed/sft_val.json")
    ap.add_argument("--cont-data", default="data_chinese/sft/split/train.txt")
    ap.add_argument("--steps", type=int, default=3000)
    ap.add_argument("--batch-size", type=int, default=16)
    ap.add_argument("--seq-len", type=int, default=256)
    ap.add_argument("--lr", type=float, default=5e-4)
    ap.add_argument("--warmup", type=int, default=100)
    ap.add_argument("--eval-every", type=int, default=250)
    ap.add_argument("--instruction-ratio", type=float, default=0.8)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--tag", default="zh2-sft")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    device = get_device()
    RUNS_SFT.mkdir(parents=True, exist_ok=True)

    # Tokenizer + vocab
    from chinese.tokenizer import CharTokenizer
    tok = CharTokenizer.load(str(DATA_CHINESE / "tokenizer.json"))
    new_vocab = tok.vocab_size
    print(f"Vocab: {new_vocab} (user={tok.USER}, assist={tok.ASSIST}, end={tok.END})")

    # Model from pretrained, expanded head
    model, cfg = load_pretrained(args.resume, new_vocab)
    model.to(device)
    print(f"Model: {cfg.vocab_size} vocab, {cfg.d_model} dim, {cfg.n_layers} layers, "
          f"total params: {sum(p.numel() for p in model.parameters()):,}")

    # Optimizer
    decay, no_decay = [], []
    for n, p in model.named_parameters():
        (no_decay if p.ndim < 2 or "table" in n or "tok_emb" in n else decay).append(p)
    opt = torch.optim.AdamW(
        [{"params": decay, "weight_decay": 0.1}, {"params": no_decay, "weight_decay": 0.0}],
        lr=args.lr, betas=(0.9, 0.95))

    # Datasets
    inst_ds = InstructionDataset(args.sft_data, args.seq_len)
    val_ds = InstructionDataset(args.val_data, args.seq_len)
    cont_ds = ContinuationDataset(args.cont_data, args.seq_len) if os.path.exists(args.cont_data) else None
    print(f"SFT samples: {len(inst_ds)}, val: {len(val_ds)}, "
          f"continuation: {len(cont_ds) if cont_ds else 0}")

    pad_id = tok.PAD

    # Mixed batch function
    def make_batch():
        x_b, y_b = [], []
        for _ in range(args.batch_size):
            if cont_ds is not None and random.random() > args.instruction_ratio:
                x, y = cont_ds[random.randrange(len(cont_ds))]
                x = x[:args.seq_len]
                y = y[:args.seq_len]
                if len(x) < args.seq_len:
                    x = x + [pad_id] * (args.seq_len - len(x))
                    y = y + [-100] * (args.seq_len - len(y))
            else:
                x, y = inst_ds[random.randrange(len(inst_ds))]
                x = x[:args.seq_len]
                y = y[:args.seq_len]
                if len(x) < args.seq_len:
                    x = x + [pad_id] * (args.seq_len - len(x))
                    y = y + [-100] * (args.seq_len - len(y))
            x_b.append(x)
            y_b.append(y)
        return (torch.tensor(x_b, dtype=torch.long).to(device),
                torch.tensor(y_b, dtype=torch.long).to(device))

    # Train
    name = f"ple-{args.tag}-s{args.seed}"
    history = []
    t0 = time.time()
    best_val = float("inf")

    print(f"\n{'='*60}\nTraining {name} ({args.steps} steps)\n{'='*60}")
    for step in range(args.steps):
        lr = lr_at(step, args.steps, args.lr, args.warmup)
        for g in opt.param_groups:
            g["lr"] = lr

        x, y = make_batch()
        logits, _ = model(x)
        loss = torch.nn.functional.cross_entropy(
            logits.view(-1, logits.size(-1)), y.reshape(-1), ignore_index=-100)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()

        if step % args.eval_every == 0 or step == args.steps - 1:
            vl = evaluate(model, val_ds, device, args.seq_len, pad_id)
            best_val = min(best_val, vl)
            history.append({"step": step, "train": loss.item(), "val": vl})
            print(f"{name} step {step:5d} | train {loss.item():.4f} | "
                  f"val {vl:.4f} | ppl {math.exp(vl):7.1f} | {time.time()-t0:5.0f}s",
                  flush=True)

    # Save
    pt_path = RUNS_SFT / f"{name}.pt"
    torch.save({"cfg": cfg.__dict__, "state": model.state_dict()}, pt_path)
    json_path = RUNS_SFT / f"{name}.json"
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump({"name": name, "steps": args.steps, "lr": args.lr,
                   "instruction_ratio": args.instruction_ratio,
                   "resume": args.resume, "history": history,
                   "best_val": best_val, "final_val": history[-1]["val"]},
                  f, indent=2)

    print(f"\n{'='*60}")
    print(f"Done. Model: {pt_path}")
    print(f"Best val: {best_val:.4f} (ppl {math.exp(best_val):.1f})")
    print(f"Wall: {time.time()-t0:.0f}s")
    print(f"{'='*60}")
    print(f"\nNext: uv run python chinese/quantize.py --tag {args.tag} --seed {args.seed}")
    print(f"      uv run python chinese/export.py {name}")


if __name__ == "__main__":
    main()
