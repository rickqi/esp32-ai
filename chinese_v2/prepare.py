"""
chinese_v2 data preparation — clean medical corpus from ModelScope.

Downloads zjydiary/Medical (encyclopedia + textbooks) and builds the
character-level training data for the v2 independent environment.

Usage:
    uv run python chinese_v2/prepare.py --download     # fetch data (30-60 min)
    uv run python chinese_v2/prepare.py                # build corpus from cached files
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from chinese_v2.env import DATA_DIR, PROJECT_ROOT  # noqa: E402
from chinese.tokenizer import CharTokenizer  # noqa: E402

RAW_DIR = DATA_DIR / "raw"
CORPUS = DATA_DIR / "corpus.txt"
TOKENIZER_JSON = DATA_DIR / "tokenizer.json"
MAX_CHARS = 100_000_000  # 100M chars cap


# ---------------------------------------------------------------------------
#  Download via modelscope CLI
# ---------------------------------------------------------------------------

def download():
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    os.system(f"pip install modelscope -q 2>nul || pip3 install modelscope -q")
    cmd = (f"modelscope download --dataset zjydiary/Medical --local_dir "
           f"{RAW_DIR / 'zjydiary_Medical'}")
    print(f"running: {cmd}")
    os.system(cmd)
    print(f"downloaded to {RAW_DIR / 'zjydiary_Medical'}")


# ---------------------------------------------------------------------------
#  Build corpus from downloaded json files
# ---------------------------------------------------------------------------

def clean_text(text):
    """Strip LaTeX / table / noise from medical text."""
    text = re.sub(r"\\[a-zA-Z]+(\{[^}]*\})?", " ", text)  # latex commands
    text = re.sub(r"\$[^$]*\$", " ", text)                 # math
    text = re.sub(r"<[^>]+>", " ", text)                   # html
    text = re.sub(r"!\[.*?\]\(.*?\)", " ", text)           # images
    text = re.sub(r"[|]{2,}", " ", text)                   # table pipes
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def extract_json_text(path, text_key):
    """Extract text field from a JSON lines / array file."""
    texts = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(d, dict):
                for k in (text_key, "text", "content", "answer", "instruction"):
                    if k in d and isinstance(d[k], str):
                        texts.append(clean_text(d[k]))
                        break
            elif isinstance(d, str):
                texts.append(clean_text(d))
    return texts


def build_corpus(max_chars=MAX_CHARS):
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    all_texts = []
    # Scan downloaded data dir for json files
    src = RAW_DIR / "zjydiary_Medical"
    if not src.exists():
        print(f"data not downloaded: {src}\n  run: uv run python chinese_v2/prepare.py --download")
        sys.exit(1)

    n_json = 0
    for f in src.rglob("*.json"):
        n_json += 1
        texts = extract_json_text(f, "text")
        all_texts.extend(texts)
        print(f"  {f.relative_to(src)}: {len(texts)} entries")
    print(f"total json files: {n_json}, text entries: {len(all_texts)}")

    # Write capped corpus
    total = 0
    with open(CORPUS, "w", encoding="utf-8") as out:
        for t in all_texts:
            if total >= max_chars:
                break
            piece = t[: max_chars - total]
            if total > 0:
                out.write("\n\n")
            out.write(piece)
            total += len(piece)
    print(f"corpus: {total:,} chars -> {CORPUS}")


# ---------------------------------------------------------------------------
#  Tokenize (reuse CharTokenizer + SFT markers)
# ---------------------------------------------------------------------------

def tokenize(vocab_size=8000):
    corpus = CORPUS.read_text(encoding="utf-8")
    tok = CharTokenizer()
    tok.train(corpus, vocab_size=vocab_size, min_freq=2)
    tok.save(str(TOKENIZER_JSON))
    print(f"tokenizer: {tok.vocab_size} chars -> {TOKENIZER_JSON}")

    # Encode (chunked, memory-safe)
    n = len(corpus)
    enc = DATA_DIR / "train.bin"
    out = np.memmap(str(enc), dtype=np.uint16, mode="w+", shape=(n,))
    CHUNK = 5_000_000
    pos = 0
    for i in range(0, n, CHUNK):
        ids = tok.encode(corpus[i:i + CHUNK], bos=False, eos=False)
        out[pos:pos + len(ids)] = ids
        pos += len(ids)
    out.flush()
    del out
    import gc
    gc.collect()

    full = np.fromfile(str(enc), dtype=np.uint16)
    n_val = max(1, int(n * 0.01))
    full[-n_val:].tofile(str(DATA_DIR / "val.bin"))
    full[:-n_val].tofile(str(enc))
    print(f"train {len(full) - n_val:,} / val {n_val:,} tokens")


def main():
    ap = argparse.ArgumentParser(description="chinese_v2 medical data prep")
    ap.add_argument("--download", action="store_true", help="download from ModelScope")
    ap.add_argument("--build-only", action="store_true", help="only build corpus (skip tokenize)")
    ap.add_argument("--vocab-size", type=int, default=8000)
    args = ap.parse_args()

    if args.download:
        download()
    build_corpus()
    if not args.build_only:
        tokenize(args.vocab_size)


if __name__ == "__main__":
    main()
