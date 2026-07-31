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
GROUP = 128


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "ple-zh-s42"
    os.makedirs(OUT, exist_ok=True)

    ck_path = RUNS / f"{tag}.pt"
    if not ck_path.exists():
        print(f"Model not found: {ck_path}")
        sys.exit(1)

    ck = torch.load(ck_path, map_location="cpu", weights_only=False)
    cfg = Config(**ck["cfg"])
    model = TinyLM(cfg)
    model.load_state_dict(ck["state"])
    model.eval()

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
        if quant:
            packed, scales, dq = quant_pack(t)
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
