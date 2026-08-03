"""
chinese_v4 data preparation — clinical guideline corpus + tokenizer + train.bin.

V4 merges the V1 clinical guideline collections (临床诊疗指南全集 + medica)
into the V2/V3 medical corpus, training a fresh 8192-char tokenizer on the
combined data.  This adds the "guideline/practice-level" knowledge that
V2/V3 (encyclopedia + textbooks) lack.

Inputs:
    D:\\docs\\raw\\临床诊疗指南全集   (specialty manuals, ~77 md)
    D:\\docs\\raw\\medica            (official L1-L4 guidelines, ~444 md)
    data_v2/corpus.txt              (existing encyclopedia+textbook corpus)

Outputs (data_v4/):
    corpus.txt        merged + cleaned corpus
    tokenizer.json    8192-char tokenizer trained on the merged corpus
    train.bin/val.bin 99:1 uint16 token split

Usage:
    python3 chinese_v4/prepare.py            # clean + merge + tokenizer + encode
    python3 chinese_v4/prepare.py --scan     # report file counts only
    python3 chinese_v4/prepare.py --max-chars 200000000
"""

import argparse
import re
import sys
from pathlib import Path

import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

from chinese.tokenizer import CharTokenizer  # noqa: E402

DATA_V4 = PROJECT_ROOT / "data_v4"
V2_CORPUS = PROJECT_ROOT / "data_v2" / "corpus.txt"

GUIDE_DIR = Path("/mnt/d/docs/raw/临床诊疗指南全集")
MEDICA_DIR = Path("/mnt/d/docs/raw/medica")

# ---- noise patterns specific to PDF-converted guidebooks -------------------
# Publisher / contact noise lines (封面页眉、版权页)
RE_PUBLISHER = re.compile(
    r"^(出版社|出版发行|发行|主编|编著|编委会|责任编辑|封面设计|印刷|版次|印次|"
    r"开本|印张|字数|书号|ISBN|国际标准书号|定价|邮编|邮政编码|地址|电话|传真|"
    r"网址|E-?mail|QQ|购书热线|热线|版社|有限公司印刷|各地新华书店|版权所有|"
    r"侵权必究|图书在版编目|CIP|社长热线|邮箱|电子邮箱)[:：\s]",
    re.IGNORECASE)

# TOC page-number lines: "1. 标题 .......... 5" or "## 5�1 xxx ......... 57"
RE_TOC_PAGE = re.compile(r"^.{0,60}[.．·]{5,}\s*\d{1,4}\s*$")
RE_TOC_LEAD = re.compile(r"^\d{1,2}[.．、]\s*\S")

# OCR artifacts from PDFs
RE_OCR_IMG = re.compile(r"!\[\]\(page=[^)]+\)")
RE_MD_IMG = re.compile(r"!\[.*?\]\([^)]+\)")
RE_MD_LINK = re.compile(r"\[([^\]]*)\]\([^)]+\)")
RE_HTML = re.compile(r"<[^>]+>")
RE_MULTI_NL = re.compile(r"\n{3,}")
RE_MULTI_SPACE = re.compile(r"[ \t]{2,}")
RE_HR = re.compile(r"^---+\s*$", re.MULTILINE)
RE_TABLE_SEP = re.compile(r"\|[\s\-:]+\|")

# English/CIP noise (library catalog / copyright blocks)
RE_EN_LINE = re.compile(r"^[A-Za-z][A-Za-z0-9\s\.,:'()\-/]{15,}$")
RE_ISBN = re.compile(r"^isbn[\s:]+", re.IGNORECASE)


def count_chinese(text: str) -> int:
    return sum(1 for ch in text if "\u4e00" <= ch <= "\u9fff")


def clean_guide_md(text: str) -> str:
    """Clean PDF-converted clinical guideline markdown."""
    # YAML frontmatter (contains headings list / title)
    text = re.sub(r"^---\n.*?\n---\n", "", text, flags=re.DOTALL)
    text = RE_OCR_IMG.sub("", text)
    text = RE_MD_IMG.sub("", text)
    text = RE_HR.sub("", text)
    text = RE_TABLE_SEP.sub("", text)
    text = RE_HTML.sub("", text)
    text = RE_MD_LINK.sub(r"\1", text)
    # keep heading markers (##) so build_sft can split sections;
    # strip only the '#' decoration but retain a marker line for structure
    text = re.sub(r"^#{1,6}\s+", "## ", text, flags=re.MULTILINE)
    text = re.sub(r"^[\*\-\+]\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"^>\s?", "", text, flags=re.MULTILINE)

    lines = text.split("\n")
    clean = []
    for line in lines:
        s = line.strip()
        if not s:
            continue
        # book-noise lines
        if RE_PUBLISHER.match(s):
            continue
        if RE_TOC_PAGE.match(s):
            continue
        if RE_ISBN.match(s):
            continue
        if re.search(r"(出版社|印刷有限公司|版权所有|侵权必究|图书在版编目|定价[:：]|"
                     r"ISBN|购书热线|新华书店)", s) and len(s) < 60:
            continue
        if RE_EN_LINE.match(s):
            # keep only if it has Chinese too (mixed titles)
            if count_chinese(s) == 0:
                continue
        if re.match(r"^[\d\s\.\,\%]+$", s):
            continue
        if re.match(r"^={10,}$", s) or re.match(r"^[\-]{10,}$", s):
            continue
        if re.match(r"^[\u4e00-\u9fff]{0,2}目\s*录$", s):
            continue
        # digit-heavy rows (ICD codes etc)
        n = len(s)
        digits = 0
        for ch2 in s:
            if ch2.isdigit():
                digits += 1
        if n > 0 and digits / n > 0.35:
            continue
        if s.count("|") >= 3:
            continue
        clean.append(s)

    text = "\n".join(clean)
    # strip LaTeX/math residue
    text = re.sub(r"\\[a-zA-Z]+", " ", text)
    text = re.sub(r"\$[^$]*\$", " ", text)
    text = RE_MULTI_NL.sub("\n\n", text)
    text = RE_MULTI_SPACE.sub(" ", text)
    return text.strip()


def scan_dir(d: Path):
    """Return (md_files, non_index_non_ocr)."""
    if not d.exists():
        return 0, 0
    mds = list(d.rglob("*.md"))
    good = [m for m in mds
            if "_index" not in m.name and "_ocr" not in m.name]
    return len(mds), len(good)


def collect_clean(d: Path, out_lines, stats):
    """Walk d, clean each md, append to out_lines."""
    if not d.exists():
        return
    mds = [m for m in d.rglob("*.md")
           if "_index" not in m.name and "_ocr" not in m.name]
    for m in mds:
        raw = m.read_text(encoding="utf-8", errors="replace")
        if len(raw) < 500:
            continue
        c = clean_guide_md(raw)
        zh = count_chinese(c)
        if zh < 200:            # skip near-empty after cleaning
            stats["skipped_small"] += 1
            continue
        out_lines.append(c)
        stats["files"] += 1
        stats["chars"] += zh


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", action="store_true", help="report files only")
    ap.add_argument("--max-chars", type=int, default=200_000_000)
    ap.add_argument("--vocab-size", type=int, default=8192)
    ap.add_argument("--skip-encode", action="store_true", help="stop after corpus+tokenizer")
    args = ap.parse_args()

    # ---- scan report ----
    g_md, g_good = scan_dir(GUIDE_DIR)
    m_md, m_good = scan_dir(MEDICA_DIR)
    print(f"临床诊疗指南全集: {g_md} md ({g_good} usable)")
    print(f"medica: {m_md} md ({m_good} usable)")
    if args.scan:
        return

    DATA_V4.mkdir(parents=True, exist_ok=True)

    # ---- clean guidebooks + medica ----
    stats = {"files": 0, "chars": 0, "skipped_small": 0}
    corpus = []
    print("\n[1/4] cleaning clinical guides ...")
    collect_clean(GUIDE_DIR, corpus, stats)
    print(f"  guides: {stats['files']} files, {stats['chars']//10000}w chars")
    g_files, g_chars = stats["files"], stats["chars"]
    collect_clean(MEDICA_DIR, corpus, stats)
    print(f"  medica: +{stats['files']-g_files} files, +{(stats['chars']-g_chars)//10000}w chars")
    print(f"  total guide corpus: {stats['files']} files, {stats['chars']//10000}w chars")

    # ---- merge V2 corpus (encyclopedia + textbooks) ----
    print("\n[2/4] merging V2 corpus ...")
    if V2_CORPUS.exists():
        v2 = V2_CORPUS.read_text(encoding="utf-8", errors="replace")
        corpus.append(v2)
        print(f"  V2 corpus: {count_chinese(v2)//10000}w chars")

    full = "\n\n".join(corpus)
    zh_total = count_chinese(full)
    print(f"  merged corpus: {len(full)//10000}w chars, {zh_total//10000}w Chinese")
    if len(full) > args.max_chars:
        full = full[:args.max_chars]
        print(f"  capped at {args.max_chars//10000}w chars")

    # ---- write corpus ----
    corpus_path = DATA_V4 / "corpus.txt"
    corpus_path.write_text(full, encoding="utf-8")
    print(f"\n[3/4] wrote {corpus_path} ({corpus_path.stat().st_size/2**20:.1f}MB)")

    # ---- train tokenizer ----
    print(f"[4/4] training {args.vocab_size}-char tokenizer ...")
    tok = CharTokenizer()
    tok.train(full, vocab_size=args.vocab_size, min_freq=1)
    tok.save(str(DATA_V4 / "tokenizer.json"))
    print(f"  tokenizer: {tok.vocab_size} chars -> data_v4/tokenizer.json")

    if args.skip_encode:
        return

    # ---- encode train/val bin (streaming, 2-pass) ----
    print("\nencoding train.bin / val.bin (streaming memmap) ...")
    CHUNK = 5_000_000
    import gc
    del corpus                      # free merged list
    del full                        # free corpus string before 260MB memmap
    gc.collect()
    full_text = (DATA_V4 / "corpus.txt").read_text(encoding="utf-8")
    n = len(full_text)
    enc_path = DATA_V4 / "train.bin"

    # pass 1: count tokens (need exact length for memmap)
    print("  pass 1: counting tokens ...")
    total_tok = 0
    for i in range(0, n, CHUNK):
        total_tok += len(tok.encode(full_text[i:i + CHUNK], bos=False, eos=False))
    print(f"  total tokens: {total_tok:,}")

    # pass 2: write
    out = np.memmap(str(enc_path), dtype=np.uint16, mode="w+", shape=(total_tok,))
    pos = 0
    for i in range(0, n, CHUNK):
        chunk_ids = tok.encode(full_text[i:i + CHUNK], bos=False, eos=False)
        out[pos:pos + len(chunk_ids)] = chunk_ids
        pos += len(chunk_ids)
        if (i // CHUNK) % 5 == 0:
            print(f"  {i / 1e6:.0f} / {n / 1e6:.0f} M chars", flush=True)
    out.flush()
    del out
    gc.collect()
    print(f"  encoded: {pos:,} tokens")
    import gc
    gc.collect()
    print(f"  encoded: {pos:,} tokens")

    # split: last 1% to val
    n_val = max(1, int(pos * 0.01))
    full_ids = np.fromfile(str(enc_path), dtype=np.uint16)
    full_ids[-n_val:].tofile(str(DATA_V4 / "val.bin"))
    full_ids[:-n_val].tofile(str(enc_path))
    print(f"  train.bin: {len(full_ids)-n_val:,} tokens, val.bin: {n_val:,} tokens")
    print("\nDONE")


if __name__ == "__main__":
    main()
