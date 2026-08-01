"""
Export the Chinese PLE model to flat binary for the C inference runtime.

Same format as src/export.py (PLE1 magic, group-wise int4 + fp16 scales,
golden logits reference) but loads from runs_chinese/ and writes to
firmware/model_chinese/ so the English artifact is untouched.

Usage:
    uv run python chinese/export.py ple-zh-s42
"""

import os
import struct
import sys
from pathlib import Path

import numpy as np
import torch

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC = PROJECT_ROOT / "src"
sys.path.insert(0, str(SRC))
sys.path.insert(0, str(PROJECT_ROOT))

from model import Config, TinyLM  # noqa: E402
from export import quant_pack  # noqa: E402  (exact same packing as English)

RUNS = PROJECT_ROOT / "runs_chinese"
OUT = PROJECT_ROOT / "firmware" / "model_chinese"
MAGIC = 0x504C4531  # "PLE1"
# group=32 for fine-grained quantization — SFT models are 4-bit sensitive;
# group=128 collapses generations (verified on zh5-med). 8-bit fully preserves.
GROUP = 32


def resolve_env(runs_dir, out_dir):
    """Switch to an alternate environment (e.g. chinese_v2)."""
    global RUNS, OUT
    if runs_dir:
        RUNS = Path(runs_dir)
    if out_dir:
        OUT = Path(out_dir)


def find_ckpt(tag):
    """Check runs_dir/ and runs_dir/sft/ for the checkpoint."""
    for base in (RUNS, RUNS / "sft"):
        p = base / f"{tag}.pt"
        if p.exists():
            return p
    return RUNS / f"{tag}.pt"


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Export Chinese model to flat binary")
    ap.add_argument("tag", nargs="?", default="ple-zh-s42")
    ap.add_argument("--runs-dir", default=None, help="Env runs dir (e.g. runs_v2)")
    ap.add_argument("--out-dir", default=None, help="Env out dir (e.g. firmware/model_v2)")
    args = ap.parse_args()
    tag = args.tag
    resolve_env(args.runs_dir, args.out_dir)
    os.makedirs(OUT, exist_ok=True)

    ck_path = find_ckpt(tag)
    if not ck_path.exists():
        print(f"Model not found: {ck_path}")
        sys.exit(1)

    ck = torch.load(ck_path, map_location="cpu", weights_only=False)
    cfg = Config(**ck["cfg"])
    model = TinyLM(cfg)
    model.load_state_dict(ck["state"])
    model.eval()

    # NaN/Inf cleanup: replace any corrupted weights with 0 (defensive —
    # training noise can inject NaN that would poison quant scales / norms).
    cleaned = 0
    for name, w in model.named_parameters():
        bad = torch.isnan(w.data) | torch.isinf(w.data)
        if bad.any():
            n = int(bad.sum().item())
            print(f"  [clean] {name}: {n} NaN/Inf -> 0")
            w.data[bad] = 0.0
            cleaned += n
    if cleaned:
        print(f"  cleaned {cleaned} corrupted values total")
    else:
        print("  no NaN/Inf in weights")

    plan = []
    sd = model.state_dict()

    def add(name, quant):
        plan.append((name, sd[name], quant))

    add("tok_emb.weight", True)           # tied: input embed + output head
    add("ple_model_proj.weight", True)
    add("ple_proj_norm.weight", False)
    add("ple_table.weight", True)
    for i in range(cfg.n_layers):
        p = f"blocks.{i}."
        add(p + "attn_norm.weight", False)
        add(p + "attn.qkv.weight", True)
        add(p + "attn.proj.weight", True)
        add(p + "ffn_norm.weight", False)
        add(p + "ffn.gate.weight", True)
        add(p + "ffn.up.weight", True)
        add(p + "ffn.down.weight", True)
        add(p + "ple_gate.weight", True)
        add(p + "ple_proj.weight", True)
        add(p + "ple_norm.weight", False)
    add("out_norm.weight", False)

    dq_sd = {k: v.clone() for k, v in sd.items()}
    blobs = []
    for name, t, quant in plan:
        # NaN/Inf cleanup: unused vocab rows (e.g. untrained token embeddings) can
        # carry NaN, which quantizes into NaN fp16 scales and poisons on-device
        # inference (observed: model_v2 tok_emb had 276 NaN scales -> block loops).
        if torch.isnan(t).any() or torch.isinf(t).any():
            print(f"  cleanup NaN/Inf in {name}: {torch.isnan(t).sum().item()} NaN, "
                  f"{torch.isinf(t).sum().item()} Inf")
            t = torch.nan_to_num(t, nan=0.0, posinf=0.0, neginf=0.0)
        if quant:
            # NOTE: must pass group=GROUP explicitly -- src/export.py's quant_pack
            # defaults to its own GROUP=128, which would pack with the wrong
            # group size vs the 32 written to the header, corrupting the layout
            # (C side then misparses scales -> NaN on device).
            packed, scales, dq = quant_pack(t, group=GROUP)
            dq_sd[name] = dq
            blobs.append(("Q", name, t.shape, packed, scales))
        else:
            blobs.append(("F", name, t.shape, t.contiguous().numpy().astype(np.float32), None))

    path = OUT / "model.bin"
    with open(path, "wb") as f:
        f.write(struct.pack("<I", MAGIC))
        for v in [cfg.vocab_size, cfg.d_model, cfg.n_layers, cfg.n_heads,
                  cfg.ffn_hidden, cfg.ple_dim, cfg.seq_len, GROUP]:
            f.write(struct.pack("<i", v))
        f.write(struct.pack("<f", cfg.rope_theta))
        for entry in blobs:
            kind = entry[0]
            if kind == "Q":
                _, _, _, packed, scales = entry
                f.write(struct.pack("<i", GROUP))
                f.write(packed.tobytes())
                f.write(scales.tobytes())
            else:
                _, _, _, arr, _ = entry
                f.write(arr.tobytes())
    size = os.path.getsize(path)
    print(f"wrote {path}  ({size/1e6:.2f} MB)  {len(plan)} tensors")

    if "head.weight" in dq_sd:
        dq_sd["head.weight"] = dq_sd["tok_emb.weight"]

    gold = TinyLM(cfg)
    gold.load_state_dict(dq_sd)
    gold.eval()
    # Chinese-ish fixed prompt: a few token ids within the Chinese vocab
    prompt = [2, 10, 100, 500, 1000, 2000, 3000, 4000]
    ids = torch.tensor([prompt])
    with torch.no_grad():
        logits, _ = gold(ids)
    last = logits[0, -1].numpy().astype(np.float32)
    np.savez(OUT / "golden.npz",
             prompt=np.array(prompt, dtype=np.int32), logits=last)
    with open(OUT / "golden.txt", "w") as gf:
        gf.write(f"{len(prompt)}\n")
        gf.write(" ".join(str(t) for t in prompt) + "\n")
        gf.write("\n".join(f"{v:.6f}" for v in last) + "\n")
    top5 = last.argsort()[-5:][::-1]
    print(f"golden: prompt={prompt}")
    print(f"golden: last-pos top5 token ids = {top5.tolist()}")
    print(f"golden: logit range [{last.min():.3f}, {last.max():.3f}]")


if __name__ == "__main__":
    main()
