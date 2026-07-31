"""
Chinese-model post-training 4-bit quantization check.

Reuses src/quantize.py's exact group-wise int4 scheme but points at the
Chinese data/runs dirs so the English pipeline is untouched.

Usage:
    uv run python chinese/quantize.py --tag zh --seed 42
"""

import argparse
import math
import os
import sys
from pathlib import Path

import numpy as np
import torch

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC = PROJECT_ROOT / "src"
sys.path.insert(0, str(SRC))
sys.path.insert(0, str(PROJECT_ROOT))

from model import Config, TinyLM  # noqa: E402
from quantize import quantize_groupwise  # noqa: E402

DATA = PROJECT_ROOT / "data_chinese"
RUNS = PROJECT_ROOT / "runs_chinese"


def get_device():
    if torch.backends.mps.is_available():
        return "mps"
    if torch.cuda.is_available():
        return "cuda"
    return "cpu"


def load(path, device):
    ck = torch.load(path, map_location=device, weights_only=False)
    m = TinyLM(Config(**ck["cfg"])).to(device)
    m.load_state_dict(ck["state"])
    m.eval()
    return m


def quantize_model(model, bits=4, group=64, quant_table=True, fp16_scales=False):
    n_q = 0
    with torch.no_grad():
        for name, p in model.named_parameters():
            if p.ndim < 2:
                continue
            if "table" in name and not quant_table:
                continue
            p.copy_(quantize_groupwise(p.data, bits, group, fp16_scales))
            n_q += p.numel()
    return n_q


@torch.no_grad()
def val_loss(model, device, iters=60, bs=16, sl=256, seed=1234):
    data = np.memmap(str(DATA / "val.bin"), dtype=np.uint16, mode="r")
    rng = np.random.default_rng(seed)
    losses = []
    for _ in range(iters):
        ix = rng.integers(0, len(data) - sl - 1, bs)
        x = torch.from_numpy(np.stack([data[i:i+sl] for i in ix]).astype(np.int64)).to(device)
        y = torch.from_numpy(np.stack([data[i+1:i+1+sl] for i in ix]).astype(np.int64)).to(device)
        losses.append(model(x, y)[1].item())
    return sum(losses) / len(losses)


def main():
    ap = argparse.ArgumentParser(description="Quantize Chinese model (4-bit PTQ check)")
    ap.add_argument("--bits", type=int, default=4)
    ap.add_argument("--group", type=int, default=64)
    ap.add_argument("--tag", default="zh")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--fp16-scales", action="store_true")
    args = ap.parse_args()
    device = get_device()

    path = RUNS / f"ple-{args.tag}-s{args.seed}.pt"
    if not path.exists():
        print(f"Model not found: {path}")
        sys.exit(1)

    m = load(path, device)
    fp = val_loss(m, device)
    print(f"ple fp32 val {fp:.4f} (ppl {math.exp(fp):.2f})")

    m = load(path, device)
    nq = quantize_model(m, args.bits, args.group, quant_table=True,
                        fp16_scales=args.fp16_scales)
    q = val_loss(m, device)
    print(f"ple {args.bits}-bit val {q:.4f} (ppl {math.exp(q):.2f}) | "
          f"deg {q-fp:+.4f} | quantized {nq/1e6:.1f}M params")


if __name__ == "__main__":
    main()
