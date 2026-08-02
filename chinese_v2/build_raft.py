"""
RAFT (Retrieval-Augmented Fine-Tuning) data builder for v2.

Training sample (P3 top2 format, matches firmware rag_augment_prompt):
    input  = <user> E1 \n E2 \n QUESTION <end> <assistant>
    target = ANSWER
where E1/E2 are answer-only evidence slices from the same KB entry
(self-grounded, like the answer-only index docs).  This teaches the model
to "copy from evidence after a question" -- the exact distribution the
firmware injects (Top-2 docs + user question).

Usage:
    uv run python chinese_v2/build_raft.py --count 20000 --top2
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


def encode_raft(tok, question, answer, top2=False, prefix_marker=False):
    """input = BOS <user> E1 \n E2 \n QUESTION <end> <assistant> ; target = ANSWER EOS.

    P3 (format alignment): top2=True matches the firmware's rag_augment_prompt()
    Top-2 injection exactly: two evidence docs (answer[:50] each, like the
    answer-only index docs) joined with '\n', then the question, then the
    SFT markers.  Training on this distribution removes the train/infer
    format mismatch that limited raft1's paraphrase fidelity.

    prefix_marker=True (deprecated): wraps evidence in [证据]...[/证据] -- the
    earlier P3 experiment (139d590) showed this REGRESSES because the firmware
    prompt template does not use the marker; kept for reference only.
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

    if top2:
        e1 = answer[:EVIDENCE_CHARS]
        e2 = answer[EVIDENCE_CHARS:2 * EVIDENCE_CHARS] or answer[:EVIDENCE_CHARS]
        prefix = ids_of(f"<user>{e1}\n{e2}\n{question}<end><assistant>")
    elif prefix_marker:
        evidence = f"[证据]{answer[:EVIDENCE_CHARS]}[/证据]"
        prefix = ids_of(f"<user>{evidence}<end><assistant>")
    else:
        evidence = answer[:EVIDENCE_CHARS]      # self-grounded evidence
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
                    help="P3(deprecated): wrap evidence in [证据]...[/证据] marker")
    ap.add_argument("--top2", action="store_true",
                    help="P3: align with firmware Top-2 injection (E1\\nE2\\nQUESTION)")
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
            input_ids, labels = encode_raft(tok, q, a,
                                            top2=args.top2,
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
