"""
chinese_v2 SFT data builder — multi-source merge.

Merges three instruction sources into the v2 SFT pool:
  A. zjydiary finetune     (1.95M medical QA, JSONL instruction/output)
  B. BenTsao 本草           (8.6K structured medical QA, JSONL)
  C. HuatuoGPT2-SFT-GPT4   (142K multi-turn, JSON array human/gpt)

Sampling is stratified so no single source dominates; output has shifted
loss labels (only assistant/answer tokens count).

Usage:
    uv run python chinese_v2/build_sft.py --zjydiary 30000 --benchao 8000 --huatuogpt2 20000
"""

import argparse
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from chinese.tokenizer import CharTokenizer  # noqa: E402

DATA_DIR = Path("/mnt/d/codes/esp32-ai/data_v2")
RAW = DATA_DIR / "raw"
OUT_DIR = DATA_DIR / "sft"
PAD, UNK, BOS, EOS = 0, 1, 2, 3


def encode_instruction(tok, question, answer):
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


def load_zjydiary(sample_n, rng):
    """JSONL instruction/output."""
    out = []
    p = RAW / "zjydiary_Medical" / "finetune" / "train_zh_0.json"
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("instruction", "") or "").strip()
            a = (d.get("output", "") or "").strip()
            if len(q) >= 5 and len(a) >= 30:
                out.append((q, a))
    rng.shuffle(out)
    print(f"zjydiary: collected {len(out)}, sample {sample_n}")
    return out[:sample_n]


def load_benchao(sample_n, rng):
    """JSONL instruction/output (BenTsao 本草)."""
    out = []
    p = RAW / "benchao" / "llama_data.json"
    if not p.exists():
        print("benchao missing, skip")
        return []
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("instruction", "") or "").strip()
            a = (d.get("output", "") or "").strip()
            if len(q) >= 5 and len(a) >= 30:
                out.append((q, a))
    rng.shuffle(out)
    print(f"benchao: collected {len(out)}, sample {sample_n}")
    return out[:sample_n]


def load_huatuogpt2(sample_n, rng):
    """JSON array {conversations: [{from: human/gpt, value}]}."""
    out = []
    p = RAW / "huatuogpt2" / "HuatuoGPT2-GPT4-SFT-140K.json"
    if not p.exists():
        print("huatuogpt2 missing, skip")
        return []
    d = json.load(open(p, encoding="utf-8"))
    for item in d:
        convs = item.get("conversations", [])
        turns = []
        for c in convs:
            f = c.get("from", "")
            v = c.get("value", "")
            if isinstance(v, list):
                v = "\n".join(str(x) for x in v)
            v = str(v or "").strip()
            if v:
                turns.append((f, v))
        human = next((v for f, v in turns if f == "human"), "")
        gpt = next((v for f, v in turns if f in ("gpt", "assistant")), "")
        if len(human) >= 5 and len(gpt) >= 30:
            out.append((human, gpt))
    rng.shuffle(out)
    print(f"huatuogpt2: collected {len(out)}, sample {sample_n}")
    return out[:sample_n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zjydiary", type=int, default=30000)
    ap.add_argument("--benchao", type=int, default=8000)
    ap.add_argument("--huatuogpt2", type=int, default=20000)
    ap.add_argument("--val-count", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    tok = CharTokenizer.load(str(DATA_DIR / "tokenizer.json"))
    print(f"tokenizer: {tok.vocab_size} (user={tok.USER})\n")
    rng = random.Random(args.seed)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    all_samples = []
    all_samples += load_zjydiary(args.zjydiary, rng)
    all_samples += load_benchao(args.benchao, rng)
    all_samples += load_huatuogpt2(args.huatuogpt2, rng)
    print(f"\ntotal: {len(all_samples)}")

    rng.shuffle(all_samples)
    val = all_samples[:args.val_count]
    train = all_samples[args.val_count:]

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
    s = train_enc[0]
    print(f"  sample: len={len(s['input_ids'])}, loss_positions="
          f"{sum(1 for v in s['labels'] if v != -100)}")


if __name__ == "__main__":
    main()
