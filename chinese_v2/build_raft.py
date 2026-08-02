"""
RAFT (Retrieval-Augmented Fine-Tuning) data builder for v2.

Training sample:  input = <user>[evidence]<end><assistant>  target = answer
where [evidence] is the truncated answer from the same KB entry (self-grounded).
This teaches the model to "copy from evidence" — the key enabler for small
models to actually use retrieved context in RAG.

Usage:
    uv run python chinese_v2/build_raft.py --count 20000
"""

import argparse
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from chinese.tokenizer import CharTokenizer  # noqa: E402

DATA_DIR = Path("/mnt/d/codes/esp32-ai/data_v2")
OUT_DIR = DATA_DIR / "sft"
KB = DATA_DIR / "kb" / "format_data.jsonl"
PAD, UNK, BOS, EOS = 0, 1, 2, 3

EVIDENCE_CHARS = 60   # evidence = answer[:60]
MAX_ANSWER = 150      # target answer capped


def resolve_env(data_dir):
    """Switch to an alternate environment (e.g. data_v3)."""
    global DATA_DIR, OUT_DIR, KB
    if data_dir:
        DATA_DIR = Path(data_dir)
    OUT_DIR = DATA_DIR / "sft"
    KB = DATA_DIR / "kb" / "format_data.jsonl"


def encode_raft(tok, evidence, answer, prefix_marker=False):
    """input = BOS <user> EVIDENCE <end> <assistant> ; target = ANSWER EOS.

    P3: prefix_marker=True wraps evidence in [证据]...[/证据] so the model
    explicitly learns "bracketed content is to be faithfully reproduced",
    reducing free-form drift during RAG reproduction.
    """
    u, a, e = tok.USER, tok.ASSIST, tok.END

    def ids_of(text):
        ids = []
        i = 0
        while i < len(text):
            if text.startswith("<user>", i):
                ids.append(u); i += 5
            elif text.startswith("<assistant>", i):
                ids.append(a); i += 10
            elif text.startswith("<end>", i):
                ids.append(e); i += 4
            else:
                ids.append(tok.stoi.get(text[i], UNK)); i += 1
        return ids

    if prefix_marker:
        evidence = f"[证据]{evidence}[/证据]"
    prefix = ids_of(f"<user>{evidence}<end><assistant>")
    ans = ids_of(answer)
    input_ids = [BOS] + prefix + ans + [EOS]
    n_pref = len(prefix) + 1
    labels = [-100] * (n_pref - 1) + ans + [EOS] + [-100]
    assert len(labels) == len(input_ids)
    return input_ids, labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--val-count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--data-dir", default=None, help="Env data dir (e.g. data_v3)")
    ap.add_argument("--prefix-marker", action="store_true",
                    help="P3: wrap evidence in [证据]...[/证据] marker")
    args = ap.parse_args()
    resolve_env(args.data_dir)

    tok = CharTokenizer.load(str(DATA_DIR / "tokenizer.json"))
    print(f"tokenizer: {tok.vocab_size}")

    entries = []
    with open(KB, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("question", "") or "").strip()
            a = (d.get("answer", "") or "").strip()
            if len(q) >= 3 and len(a) >= 30:
                entries.append((q, a))
    print(f"kb entries: {len(entries)}")

    rng = random.Random(args.seed)
    rng.shuffle(entries)
    picked = entries[:args.count + args.val_count]
    val = picked[:args.val_count]
    train = picked[args.val_count:]

    def build(items):
        enc = []
        for q, a in items:
            evidence = a[:EVIDENCE_CHARS]      # self-grounded evidence
            answer = a[:MAX_ANSWER]
            input_ids, labels = encode_raft(tok, evidence, answer,
                                            prefix_marker=args.prefix_marker)
            enc.append({"input_ids": input_ids, "labels": labels})
        return enc

    train_enc = build(train)
    val_enc = build(val)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    with open(OUT_DIR / "raft_train.json", "w", encoding="utf-8") as f:
        json.dump({"data": train_enc, "format": "char-level",
                   "special_tokens": {"user": tok.USER, "assistant": tok.ASSIST,
                                      "end": tok.END}}, f, ensure_ascii=False)
    with open(OUT_DIR / "raft_val.json", "w", encoding="utf-8") as f:
        json.dump({"data": val_enc, "format": "char-level"}, f, ensure_ascii=False)

    print(f"RAFT train: {len(train_enc)}, val: {len(val_enc)} -> {OUT_DIR}")
    s = train_enc[0]
    print(f"  sample len={len(s['input_ids'])}, loss={sum(1 for v in s['labels'] if v != -100)}")


if __name__ == "__main__":
    main()
