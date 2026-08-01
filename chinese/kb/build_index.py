"""
Build compressed inverted index for ESP32 device-side RAG.

Format (binary, designed for PSRAM):
  header:  magic "RAG1" | n_docs u32 | n_terms u32 | vocab_size u32
  doc_blob:  concat of per-doc utf8 bytes, with offsets table
  doc_off:   n_docs+1 u32 offsets into doc_blob
  inverted:  for each term (char id) a sorted u32 doc-id list
             (packed as n_terms*2 header + concatenated ids)

Docs: "question<sep>answer-truncated", each <= MAX_DOC_CHARS.
Terms: character-level (reuse v2 tokenizer char ids where possible).

Usage:
    uv run python chinese/kb/build_index.py --sample 30000 --out data_v2/kb/index.bin
"""

import argparse
import json
import random
import struct
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT))
from chinese.tokenizer import CharTokenizer  # noqa: E402

MAX_DOC_CHARS = 50   # question(15) + answer(35)
SEP = "\x01"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kb", default="/mnt/d/codes/esp32-ai/data_v2/kb/format_data.jsonl")
    ap.add_argument("--sample", type=int, default=10000)
    ap.add_argument("--tokenizer", default="/mnt/d/codes/esp32-ai/data_v2/tokenizer.json")
    ap.add_argument("--out", default="/mnt/d/codes/esp32-ai/data_v2/kb/index.bin")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    tok = CharTokenizer.load(args.tokenizer)
    char2id = tok.stoi
    print(f"tokenizer vocab: {tok.vocab_size}")

    # Load + sample KB entries
    entries = []
    with open(args.kb, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("question", "") or "").strip()
            a = (d.get("answer", "") or "").strip()
            if len(q) >= 3 and len(a) >= 20:
                entries.append((q, a))
    print(f"kb valid entries: {len(entries)}")

    rng = random.Random(args.seed)
    rng.shuffle(entries)
    entries = entries[:args.sample]
    print(f"sampled: {len(entries)}")

    # Build docs: store as uint16 char-ids (compact, 2 bytes/char vs 3 bytes utf8)
    doc_off = [0]
    doc_ids = []  # flat uint16 char ids
    for q, a in entries:
        doc = (q[:15] + SEP + a[:35])[:MAX_DOC_CHARS]
        ids = [char2id.get(c, tok.UNK) for c in doc]
        doc_ids.extend(ids)
        doc_off.append(doc_off[-1] + len(ids))

    # Build inverted index (char -> doc list)
    inverted = {}  # char_id -> list of doc_id
    for di, doc in enumerate(docs_str(entries)):
        seen = set()
        for ch in doc:
            cid = char2id.get(ch)
            if cid is None or cid in seen:
                continue
            seen.add(cid)
            inverted.setdefault(cid, []).append(di)

    terms = sorted(inverted.keys())
    print(f"terms: {len(terms)}")

    # Serialize: magic | n_docs | n_terms | vocab | doc_off | doc_ids | inverted
    out = bytearray()
    out += b"RAG1"
    out += struct.pack("<III", len(entries), len(terms), tok.vocab_size)
    out += struct.pack(f"<{len(doc_off)}I", *doc_off)          # fixed size first
    out += struct.pack(f"<{len(doc_ids)}H", *doc_ids)          # then flat char ids
    # inverted: per term: char_id + count + doc ids (uint16)
    for t in terms:
        ids = inverted[t]
        out += struct.pack("<IH", t, len(ids))
        out += struct.pack(f"<{len(ids)}H", *ids)

    Path(args.out).write_bytes(bytes(out))
    print(f"index: {len(out)/1e6:.2f} MB -> {args.out}")
    avg_len = doc_off[-1] / len(entries)
    print(f"avg doc chars: {avg_len:.0f}, terms: {len(terms)}")


def docs_str(entries):
    for q, a in entries:
        yield (q[:15] + SEP + a[:35])[:MAX_DOC_CHARS]


if __name__ == "__main__":
    main()
