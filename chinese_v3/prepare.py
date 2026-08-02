"""
chinese_v3 data preparation — 8192-char tokenizer + SFT data build.

V3 uses an 8,192-char tokenizer (wider than v2's 6,594) and reuses the
high-quality SFT pools (zjydiary / BenTsao / HuatuoGPT2) which were themselves
Qwen/DeepSeek-generated. Logits distillation against the live Qwen3-0.6B
teacher happens in distill_train.py.

Usage:
    python3 chinese_v3/prepare.py --tokenizer-only       # train 8192 tokenizer
    python3 chinese_v3/prepare.py --build-sft --count 50000
"""

import argparse
import json
import random
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

DATA = PROJECT_ROOT / "data_v3"
V2_DATA = PROJECT_ROOT / "data_v2"
OUT = DATA / "sft"
PAD, UNK, BOS, EOS = 0, 1, 2, 3


def train_tokenizer():
    """Train an 8192-char tokenizer on the v2 medical corpus (reuse)."""
    from chinese.tokenizer import CharTokenizer
    DATA.mkdir(parents=True, exist_ok=True)
    corpus_path = V2_DATA / "corpus.txt"
    if not corpus_path.exists():
        print(f"v2 corpus missing: {corpus_path}")
        sys.exit(1)
    corpus = corpus_path.read_text(encoding="utf-8")
    tok = CharTokenizer()
    tok.train(corpus, vocab_size=8192, min_freq=1)  # wider vocab than v2's 6594
    tok.save(str(DATA / "tokenizer.json"))
    print(f"v3 tokenizer: {tok.vocab_size} chars -> {DATA/'tokenizer.json'}")
    return tok


def encode_instruction(tok, q, a):
    u, e, ast = tok.USER, tok.END, tok.ASSIST

    def ids_of(text):
        ids = []
        i = 0
        while i < len(text):
            if text.startswith("<user>", i):
                ids.append(u); i += 5
            elif text.startswith("<assistant>", i):
                ids.append(ast); i += 10
            elif text.startswith("<end>", i):
                ids.append(e); i += 4
            else:
                ids.append(tok.stoi.get(text[i], UNK)); i += 1
        return ids

    prefix = ids_of(f"<user>{q}<end><assistant>")
    ans = ids_of(a)
    input_ids = [BOS] + prefix + ans + [EOS]
    n_pref = len(prefix) + 1
    labels = [-100] * (n_pref - 1) + ans + [EOS] + [-100]
    assert len(labels) == len(input_ids)
    return input_ids, labels


def build_sft(count, seed=42):
    tok = None
    from chinese.tokenizer import CharTokenizer
    tok = CharTokenizer.load(str(DATA / "tokenizer.json"))
    rng = random.Random(seed)
    OUT.mkdir(parents=True, exist_ok=True)

    # Reuse the three high-quality pools (Qwen/DeepSeek-generated QA)
    pools = []
    # zjydiary finetune
    zj = V2_DATA / "raw" / "zjydiary_Medical" / "finetune" / "train_zh_0.json"
    with open(zj, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("instruction", "") or "").strip()
            a = (d.get("output", "") or "").strip()
            if len(q) >= 5 and len(a) >= 30:
                pools.append((q, a))
    print(f"zjydiary: {len(pools)}")
    # BenTsao
    bt = V2_DATA / "raw" / "benchao" / "llama_data.json"
    if bt.exists():
        with open(bt, encoding="utf-8", errors="replace") as f:
            for line in f:
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue
                q = (d.get("instruction", "") or "").strip()
                a = (d.get("output", "") or "").strip()
                if len(q) >= 5 and len(a) >= 30:
                    pools.append((q, a))
    print(f"+BenTsao: total {len(pools)}")
    # HuatuoGPT2
    ht = V2_DATA / "raw" / "huatuogpt2" / "HuatuoGPT2-GPT4-SFT-140K.json"
    if ht.exists():
        d = json.load(open(ht, encoding="utf-8"))
        for item in d:
            convs = item.get("conversations", [])
            turns = []
            for c in convs:
                v = c.get("value", "")
                if isinstance(v, list):
                    v = "\n".join(str(x) for x in v)
                turns.append((c.get("from", ""), str(v or "").strip()))
            human = next((v for f, v in turns if f == "human"), "")
            gpt = next((v for f, v in turns if f in ("gpt", "assistant")), "")
            if len(human) >= 5 and len(gpt) >= 30:
                pools.append((human, gpt))
    print(f"+HuatuoGPT2: total {len(pools)}")

    rng.shuffle(pools)
    picked = pools[:count]
    # split
    n_val = max(1000, count // 20)
    val, train = picked[:n_val], picked[n_val:]

    def encode(items):
        enc = []
        for q, a in items:
            input_ids, labels = encode_instruction(tok, q, a)
            enc.append({"input_ids": input_ids, "labels": labels})
        return enc

    train_enc = encode(train)
    val_enc = encode(val)
    with open(OUT / "sft_train.json", "w", encoding="utf-8") as f:
        json.dump({"data": train_enc, "format": "char-level",
                   "special_tokens": {"user": tok.USER, "assistant": tok.ASSIST,
                                      "end": tok.END}}, f, ensure_ascii=False)
    with open(OUT / "sft_val.json", "w", encoding="utf-8") as f:
        json.dump({"data": val_enc, "format": "char-level"}, f, ensure_ascii=False)
    print(f"v3 SFT: train {len(train_enc)}, val {len(val_enc)} -> {OUT}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tokenizer-only", action="store_true")
    ap.add_argument("--build-sft", action="store_true")
    ap.add_argument("--count", type=int, default=50000)
    args = ap.parse_args()

    if args.tokenizer_only or not args.build_sft:
        train_tokenizer()
    if args.build_sft:
        build_sft(args.count)


if __name__ == "__main__":
    main()
