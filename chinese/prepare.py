"""
Chinese data preparation for ESP32-LLM.

Extracts clean Chinese text from D:\\docs\\source (insurance/business documents),
builds a character-level tokenizer, and generates train.bin / val.bin
for training the tiny PLE model.

Usage:
    uv run python chinese/prepare.py
    uv run python chinese/prepare.py --source D:/docs/source --vocab-size 8000
"""

import argparse
import os
import re
import sys
from pathlib import Path

import numpy as np

# Add project root to path
HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
sys.path.insert(0, str(PROJECT_ROOT))

from chinese.tokenizer import CharTokenizer  # noqa: E402


# ---------------------------------------------------------------------------
#  Text extraction & cleaning
# ---------------------------------------------------------------------------

# OCR artifacts: ![](page=0,bbox=[...])
RE_OCR_IMG = re.compile(r"!\[\]\(page=[^)]+\)")

# Markdown images: ![alt](url)
RE_MD_IMG = re.compile(r"!\[.*?\]\([^)]+\)")

# Markdown links: [text](url)
RE_MD_LINK = re.compile(r"\[([^\]]*)\]\([^)]+\)")

# HTML tags
RE_HTML = re.compile(r"<[^>]+>")

# Multiple newlines
RE_MULTI_NL = re.compile(r"\n{3,}")

# Multiple spaces
RE_MULTI_SPACE = re.compile(r"[ \t]{2,}")

# Horizontal rules
RE_HR = re.compile(r"---+\n?")

# Table separators: |---|---|
RE_TABLE_SEP = re.compile(r"\|[\s\-:]+\|")


def count_chinese(text: str) -> int:
    """Count CJK chars without materialising a match list (memory-safe on
    100MB+ strings; re.findall would build a ~2.5GB list for 81M matches)."""
    return sum(1 for ch in text if "\u4e00" <= ch <= "\u9fff")


def clean_markdown(text: str) -> str:
    """Strip Markdown/HTML/OCR/processing-log formatting, return clean Chinese text."""
    # Strip YAML frontmatter (--- ... ---)
    text = re.sub(r"^---\n.*?\n---\n", "", text, flags=re.DOTALL)
    # Remove OCR image references
    text = RE_OCR_IMG.sub("", text)
    text = RE_MD_IMG.sub("", text)
    text = RE_HR.sub("", text)
    text = RE_TABLE_SEP.sub("", text)
    text = RE_HTML.sub("", text)
    text = RE_MD_LINK.sub(r"\1", text)
    text = re.sub(r"^#{1,6}\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"^[\*\-\+]\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"^>\s?", "", text, flags=re.MULTILINE)

    # Remove lines that look like processing logs / file paths / system output
    lines = text.split("\n")
    clean_lines = []
    for line in lines:
        s = line.strip()
        if not s:
            continue
        # File paths
        if re.match(r"^[A-Za-z]:[\\/]", s):
            continue
        # Emoji-leading lines (OCR pipeline status)
        if re.search(r"[\U0001F300-\U0001F9FF]", s[:1]):
            continue
        # OCR processing log lines
        if re.match(r"^(glm ocr|处理用时|正在提取|💾|📁|📄|📂|输出文件|开始处理)", s):
            continue
        # Pure separator lines
        if re.match(r"^={10,}$", s) or re.match(r"^[\-]{10,}$", s):
            continue
        # Pure number lines
        if re.match(r"^[\d\s\.\,\%]+$", s):
            continue
        # Python traceback / script paths
        if re.match(r"^(Traceback|File \")", s):
            continue
        # Timestamp-only lines
        if re.match(r"^\d{4}[-/]\d{2}[-/]\d{2}\s*\d{2}:\d{2}", s):
            continue
        # Python stdout captured in logs
        if re.match(r"^(Processing|Process finished)", s):
            continue

        # ---- low-quality table/data-dump lines ----
        n = len(s)
        # Digit-heavy rows (data exports / ICD code lists)
        digit_ratio = sum(c.isdigit() for c in s) / max(n, 1)
        if digit_ratio > 0.35:
            continue
        # Pipe-separated table rows
        if s.count("|") >= 3:
            continue
        # Long space-separated code/number runs (e.g. "120 124 221 116 623 160 322 .000000")
        if re.match(r"^[\d\s.+\-]+$", s) and n > 20:
            continue
        # DLP-style audit rows: contains both "|" and file extensions
        if "\\" in s and s.count("/") + s.count("\\") >= 2:
            continue

        clean_lines.append(s)

    text = "\n".join(clean_lines)
    text = RE_MULTI_NL.sub("\n\n", text)
    text = RE_MULTI_SPACE.sub(" ", text)
    return text.strip()


def _scan_files(source_dir: Path, min_file_bytes: int = 500, min_chinese: int = 20,
                exclude_dirs: tuple = ("DLP", "DLP案件反馈")):
    """First pass: rank all .md/.txt files by Chinese character count (desc).

    Files under any excluded directory name are skipped (DLP data dumps are
    table-heavy and pollute language-model training).
    """
    stats = []
    for fpath in source_dir.rglob("*"):
        if fpath.suffix.lower() not in (".md", ".txt"):
            continue
        # Skip files inside excluded directories
        if any(ex in fpath.parts for ex in exclude_dirs):
            continue
        try:
            if fpath.stat().st_size < min_file_bytes:
                continue
        except OSError:
            continue
        try:
            raw = fpath.read_text(encoding="utf-8", errors="replace")
        except Exception:
            try:
                raw = fpath.read_text(encoding="gbk", errors="replace")
            except Exception:
                continue
        zh = count_chinese(raw)
        if zh >= min_chinese:
            stats.append((zh, str(fpath)))
    stats.sort(key=lambda x: -x[0])  # Chinese-rich files first
    return stats


def extract_text(source_dir: str, out_path: Path, min_file_bytes: int = 500,
                 max_chars: int = 100_000_000, max_per_file: int = 2_000_000,
                 exclude_dirs: tuple = ("DLP", "DLP案件反馈")):
    """Walk source dir, clean each .md/.txt, write to out_path, stop at cap.

    Files are processed in order of Chinese content (richest first) so the
    capped corpus maximises Chinese text. Each file contributes at most
    `max_per_file` chars to keep the corpus diverse. Streams to disk —
    never holds the whole corpus in memory.
    Returns (total_chars, chinese_chars, files_used).
    """
    source = Path(source_dir)
    if not source.is_dir():
        print(f"Source directory not found: {source}")
        sys.exit(1)

    print("  scanning files by Chinese content...")
    ranked = _scan_files(source, min_file_bytes, exclude_dirs=exclude_dirs)
    print(f"  found {len(ranked)} files with Chinese content")

    total = 0
    chinese_total = 0
    files_used = 0
    skipped_bad = 0

    with open(out_path, "w", encoding="utf-8") as wf:
        for zh, fpath in ranked:
            if total >= max_chars:
                break
            try:
                raw = Path(fpath).read_text(encoding="utf-8", errors="replace")
            except Exception:
                continue
            try:
                cleaned = clean_markdown(raw)
            except Exception as exc:
                skipped_bad += 1
                if skipped_bad <= 5:
                    print(f"  [warn] clean failed: {Path(fpath).name}: {exc}", flush=True)
                continue
            chinese_chars = count_chinese(cleaned)
            if chinese_chars < 20:
                continue

            # Per-file cap keeps the corpus diverse
            if max_per_file:
                cleaned = cleaned[:max_per_file]

            remaining = max_chars - total
            piece = cleaned[:remaining]
            if files_used > 0:
                wf.write("\n\n")
            wf.write(piece)
            total += len(piece)
            chinese_total += count_chinese(piece)  # count what was actually written
            files_used += 1
            if files_used % 10 == 0:
                print(f"  {files_used} files, {total/1e6:.0f}M chars", flush=True)

    print(f"Extracted: {files_used} files, "
          f"{total:,} chars total, "
          f"{chinese_total:,} Chinese characters")
    return total, chinese_total, files_used


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Prepare Chinese training data for ESP32-LLM")
    ap.add_argument("--source", default="D:/docs/raw",
                    help="Source directory with .md/.txt files")
    ap.add_argument("--vocab-size", type=int, default=8000,
                    help="Maximum vocabulary size (character-level)")
    ap.add_argument("--min-freq", type=int, default=2,
                    help="Minimum character frequency to include in vocab")
    ap.add_argument("--val-fraction", type=float, default=0.01,
                    help="Fraction of tokens held out for validation")
    ap.add_argument("--max-chars", type=int, default=100_000_000,
                    help="Cap corpus size (default 100M chars; 0 = no cap)")
    ap.add_argument("--max-per-file", type=int, default=2_000_000,
                    help="Per-file char cap so no single file dominates (0 = no cap)")
    ap.add_argument("--exclude-dirs", default="DLP,DLP案件反馈",
                    help="Comma-separated directory names to skip (default excludes DLP data dumps)")
    ap.add_argument("--out-dir", default=None,
                    help="Output directory (default: data_chinese/)")
    args = ap.parse_args()

    out_dir = Path(args.out_dir or (PROJECT_ROOT / "data_chinese"))
    out_dir.mkdir(parents=True, exist_ok=True)

    # Step 1: Extract & clean text (streamed to disk, capped)
    print("=== Step 1: Extract text ===")
    raw_path = out_dir / "corpus.txt"
    exclude = tuple(d.strip() for d in args.exclude_dirs.split(",") if d.strip())
    total_chars, chinese_total, files_used = extract_text(
        args.source, raw_path, max_chars=args.max_chars or 10**12,
        max_per_file=args.max_per_file, exclude_dirs=exclude)

    if total_chars < 1000:
        print(f"Corpus too small ({total_chars} chars). Check source directory.")
        sys.exit(1)

    print(f"Saved raw corpus: {raw_path}  ({total_chars:,} chars)")

    # Step 2: Train character-level tokenizer
    print("\n=== Step 2: Train tokenizer ===")
    corpus = raw_path.read_text(encoding="utf-8")
    tok = CharTokenizer()
    tok.train(corpus, vocab_size=args.vocab_size, min_freq=args.min_freq)

    # Save tokenizer
    tok_path = out_dir / "tokenizer.json"
    tok.save(str(tok_path))
    print(f"Saved tokenizer: {tok_path}  (vocab_size={tok.vocab_size})")

    # Step 3: Encode corpus (chunked, memory-efficient — never hold >1 chunk)
    print("\n=== Step 3: Encode ===")
    CHUNK = 5_000_000  # 5M chars per chunk
    n = len(corpus)
    enc_path = out_dir / "train.bin"
    out = np.memmap(str(enc_path), dtype=np.uint16, mode="w+", shape=(n,))
    pos = 0
    for i in range(0, n, CHUNK):
        chunk_ids = tok.encode(corpus[i:i + CHUNK], bos=False, eos=False)
        out[pos:pos + len(chunk_ids)] = chunk_ids
        pos += len(chunk_ids)
        if (i // CHUNK) % 5 == 0:
            print(f"  {i / 1e6:.0f} / {n / 1e6:.0f} M chars", flush=True)
    out.flush()
    del out  # release memmap (Windows: must close before rewriting the file)
    import gc
    gc.collect()
    print(f"Encoded: {pos:,} tokens")

    # Step 4: Train/val split (load once, write two files)
    n_val = max(1, int(n * args.val_fraction))
    val_path = out_dir / "val.bin"
    train_final = out_dir / "train.bin"

    full = np.fromfile(str(train_final), dtype=np.uint16)  # ~200MB for 100M tokens
    full[-n_val:].tofile(str(val_path))
    full[:-n_val].tofile(str(train_final))  # overwrite with train-only
    keep = len(full) - n_val
    del full

    print(f"\n=== Done ===")
    print(f"Train: {keep:,} tokens → {train_final} ({train_final.stat().st_size / 1e6:.1f} MB)")
    print(f"Val:   {n_val:,} tokens → {val_path} ({val_path.stat().st_size / 1e6:.1f} MB)")
    print(f"Vocab: {tok.vocab_size:,} characters")
    print(f"Corpus tokens/chars: {keep / n:.2f}")
    print(f"\nNext step: uv run python chinese/train.py")


if __name__ == "__main__":
    main()
