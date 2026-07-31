"""
SFT inference demo — ask the fine-tuned Chinese model a question.

Builds the instruction prompt (<user> Q <end><assistant>) and generates
the answer with the SFT checkpoint.

Usage:
    uv run python chinese/sft/sft_generate.py "甲状腺切除是否会造成晕倒"
    uv run python chinese/sft/sft_generate.py --tag zh2-sft
"""

import argparse
import sys
from pathlib import Path

import torch

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
SRC = PROJECT_ROOT / "src"
sys.path.insert(0, str(SRC))
sys.path.insert(0, str(PROJECT_ROOT))

from model import Config, TinyLM  # noqa: E402
from chinese.tokenizer import CharTokenizer  # noqa: E402

RUNS_SFT = PROJECT_ROOT / "runs_chinese" / "sft"
TOKENIZER = PROJECT_ROOT / "data_chinese" / "tokenizer.json"


def main():
    ap = argparse.ArgumentParser(description="Ask the SFT Chinese model a question")
    ap.add_argument("question", nargs="?", default="甲状腺切除是否会造成晕倒",
                    help="Question text")
    ap.add_argument("--tag", default="zh2-sft")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--max-new-tokens", type=int, default=200)
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--top-k", type=int, default=40)
    ap.add_argument("--repetition-penalty", type=float, default=1.3,
                    help="penalize already-generated tokens (>=1.0)")
    ap.add_argument("--device", default=None)
    args = ap.parse_args()

    device = args.device or ("cuda" if torch.cuda.is_available() else "cpu")
    torch.manual_seed(args.seed)

    tok = CharTokenizer.load(str(TOKENIZER))
    ck_path = RUNS_SFT / f"ple-{args.tag}-s{args.seed}.pt"
    if not ck_path.exists():
        print(f"Model not found: {ck_path}")
        sys.exit(1)
    ck = torch.load(ck_path, map_location=device, weights_only=False)
    cfg = Config(**ck["cfg"])
    model = TinyLM(cfg).to(device)
    model.load_state_dict(ck["state"])
    model.eval()

    # Build instruction prompt: BOS <user> Q <end> <assistant> (match training)
    prompt_text = f"<user>{args.question}<end><assistant>"
    ids = [tok.BOS]
    i = 0
    while i < len(prompt_text):
        if prompt_text.startswith("<user>", i):
            ids.append(tok.USER); i += len("<user>")
        elif prompt_text.startswith("<assistant>", i):
            ids.append(tok.ASSIST); i += len("<assistant>")
        elif prompt_text.startswith("<end>", i):
            ids.append(tok.END); i += len("<end>")
        else:
            ids.append(tok.stoi.get(prompt_text[i], tok.UNK))
            i += 1
    idx = torch.tensor([ids], dtype=torch.long, device=device)

    print(f"Q: {args.question}")
    print(f"A: ", end="", flush=True)

    generated = []
    with torch.no_grad():
        for _ in range(args.max_new_tokens):
            idx_c = idx[:, -cfg.seq_len:]
            logits, _ = model(idx_c)
            logits = logits[:, -1, :] / args.temperature
            # repetition penalty
            if args.repetition_penalty > 1.0 and generated:
                for tid in set(generated[-50:]):
                    logits[0, tid] /= args.repetition_penalty
            if args.top_k:
                v, _ = torch.topk(logits, min(args.top_k, logits.size(-1)))
                logits[logits < v[:, [-1]]] = -float("inf")
            probs = torch.softmax(logits, dim=-1)
            next_id = torch.multinomial(probs, 1)
            if next_id.item() in (tok.EOS, tok.END):
                break
            idx = torch.cat([idx, next_id], dim=1)
            ch = tok.itos.get(next_id.item(), "")
            generated.append(next_id.item())
            print(ch, end="", flush=True)
    print()
    print(f"\n--- {len(generated)} chars ---")


if __name__ == "__main__":
    main()
