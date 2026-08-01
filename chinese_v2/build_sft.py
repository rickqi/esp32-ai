"""
chinese_v2 SFT data builder — sample zjydiary finetune instructions.

Samples N entries from finetune/train_zh_0.json (1.95M medical instruction
pairs) and encodes them into the sft_train.py format with shifted loss
labels (only assistant/answer tokens count).

Usage:
    uv run python chinese_v2/build_sft.py --count 30000
    uv run python chinese_v2/build_sft.py --count 30000 --val-count 3000
"""

import argparse
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from chinese.tokenizer import CharTokenizer  # noqa: E402

DATA_DIR = Path("/mnt/d/codes/esp32-ai/data_v2")
FINETUNE = DATA_DIR / "raw" / "zjydiary_Medical" / "finetune" / "train_zh_0.json"
OUT_DIR = DATA_DIR / "sft"
PAD, UNK, BOS, EOS = 0, 1, 2, 3


def encode_instruction(tok, question, answer):
    """Encode into [BOS user Q end assist A end EOS] with shifted labels."""
    tok_user, tok_assist, tok_end = tok.USER, tok.ASSIST, tok.END

    def ids_of(text):
        ids = []
        i = 0
        while i < len(text):
            if text.startswith("<user>", i):
                ids.append(tok_user); i += len("<user>")
            elif text.startswith("<assistant>", i):
                ids.append(tok_assist); i += len("<assistant>")
            elif text.startswith("<end>", i):
                ids.append(tok_end); i += len("<end>")
            else:
                ids.append(tok.stoi.get(text[i], UNK)); i += 1
        return ids

    prefix = ids_of(f"<user>{question}<end><assistant>")
    answer_ids = ids_of(answer)
    input_ids = [BOS] + prefix + answer_ids + [EOS]
    n_pref = len(prefix) + 1
    labels = [-100] * (n_pref - 1) + answer_ids + [EOS] + [-100]
    assert len(labels) == len(input_ids)
    return input_ids, labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=30000)
    ap.add_argument("--val-count", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    tok = CharTokenizer.load(str(DATA_DIR / "tokenizer.json"))
    print(f"tokenizer: {tok.vocab_size} (user={tok.USER})")

    # Sample lines from finetune jsonl
    rng = random.Random(args.seed)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    samples = []
    with open(FINETUNE, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("instruction", "") or "").strip()
            a = (d.get("output", "") or "").strip()
            if len(q) < 5 or len(a) < 30:
                continue
            samples.append((q, a))
            if len(samples) >= args.count * 2:
                break

    print(f"collected {len(samples)} valid samples, sampling...")
    rng.shuffle(samples)
    picked = samples[:args.count + args.val_count]
    val = picked[:args.val_count]
    train = picked[args.val_count:]

    def encode_list(items):
        enc = []
        for q, a in items:
            input_ids, labels = encode_instruction(tok, q, a)
            enc.append({"input_ids": input_ids, "labels": labels})
        return enc

    train_enc = encode_list(train)
    val_enc = encode_list(val)

    with open(OUT_DIR / "sft_train.json", "w", encoding="utf-8") as f:
        json.dump({"data": train_enc, "format": "char-level",
                   "special_tokens": {"user": tok.USER, "assistant": tok.ASSIST,
                                      "end": tok.END}}, f, ensure_ascii=False)
    with open(OUT_DIR / "sft_val.json", "w", encoding="utf-8") as f:
        json.dump({"data": val_enc, "format": "char-level"}, f, ensure_ascii=False)

    print(f"train: {len(train_enc)}, val: {len(val_enc)} -> {OUT_DIR}")
    # sample check
    s = train_enc[0]
    print(f"  sample: len={len(s['input_ids'])}, loss_positions="
          f"{sum(1 for v in s['labels'] if v != -100)}")


if __name__ == "__main__":
    main()
