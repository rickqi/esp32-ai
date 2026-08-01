"""
Chinese model inference demo — load the trained checkpoint and generate text
using the character-level tokenizer.

Usage:
    uv run python chinese/generate.py
    uv run python chinese/generate.py "从前有一个"
    uv run python chinese/generate.py --tag zh --seed 42 --max-new-tokens 200
"""

import argparse
import sys
from pathlib import Path

import torch

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC = PROJECT_ROOT / "src"
sys.path.insert(0, str(SRC))
sys.path.insert(0, str(PROJECT_ROOT))

from model import Config, TinyLM  # noqa: E402
from chinese.tokenizer import CharTokenizer  # noqa: E402

RUNS = PROJECT_ROOT / "runs_chinese"
TOKENIZER = PROJECT_ROOT / "data_chinese" / "tokenizer.json"


def main():
    ap = argparse.ArgumentParser(description="Generate Chinese text with the trained model")
    ap.add_argument("prompt", nargs="?", default="本报告",
                    help="Chinese prompt text (default: 本报告)")
    ap.add_argument("--tag", default="zh")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--max-new-tokens", type=int, default=150)
    ap.add_argument("--temperature", type=float, default=0.8)
    ap.add_argument("--top-k", type=int, default=40)
    ap.add_argument("--device", default=None)
    args = ap.parse_args()

    device = args.device or ("cuda" if torch.cuda.is_available() else "cpu")
    torch.manual_seed(args.seed)

    # Load tokenizer + model
    tok = CharTokenizer.load(str(TOKENIZER))
    ck_path = RUNS / f"ple-{args.tag}-s{args.seed}.pt"
    if not ck_path.exists():
        print(f"Model not found: {ck_path}")
        sys.exit(1)
    ck = torch.load(ck_path, map_location=device, weights_only=False)
    cfg = Config(**ck["cfg"])
    model = TinyLM(cfg).to(device)
    model.load_state_dict(ck["state"])
    model.eval()

    print(f"Model: {cfg.vocab_size} vocab, {cfg.d_model} dim, {cfg.n_layers} layers")
    print(f"Vocab size (tokenizer): {tok.vocab_size}")
    print(f"Prompt: {args.prompt!r}")

    # Encode prompt
    ids = tok.encode(args.prompt, bos=True, eos=False)
    idx = torch.tensor([ids], dtype=torch.long, device=device)

    print("\n--- 生成结果 ---")
    print(args.prompt, end="", flush=True)

    generated = []
    with torch.no_grad():
        for _ in range(args.max_new_tokens):
            idx_c = idx[:, -cfg.seq_len:]
            logits, _ = model(idx_c)
            logits = logits[:, -1, :] / args.temperature
            if args.top_k:
                v, _ = torch.topk(logits, min(args.top_k, logits.size(-1)))
                logits[logits < v[:, [-1]]] = -float("inf")
            probs = torch.softmax(logits, dim=-1)
            next_id = torch.multinomial(probs, 1)
            # EOS check
            if next_id.item() == tok.EOS:
                break
            idx = torch.cat([idx, next_id], dim=1)
            ch = tok.itos.get(next_id.item(), "")
            generated.append(ch)
            print(ch, end="", flush=True)

    print("\n\n--- 统计 ---")
    print(f"生成字符数: {len(generated)}")
    print(f"总序列长: {idx.shape[1]}")


if __name__ == "__main__":
    main()
