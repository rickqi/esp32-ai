"""
chinese_v4 SD 扩展索引构建 — 全量 KB → index.bin/docs.bin/meta.bin (V2)

格式定型 (基于 IDF 84% 饱和 + PSRAM 预算分析):
  index.bin:
    [u16 n_terms]
    term table (常驻 PSRAM ~18KB):  每 term: u8 len + UTF-8 + u32 doclist_offset + u16 doc_count
    doclists (流式, SD 卡):          每 term: u16 doc_id[]
  docs.bin:
    [u16 n_docs] 每 doc: u16 len + UTF-8 (evidence)
  meta.bin:
    [u32 N] [u16 n_terms] 每 term: u8 len + UTF-8 + u8 idf

单字 term (医学字符集有限 ~4-5K), 避免 2-gram 导致 term 表爆炸.
PC 端 Python 检索参考实现 + 触发信号验证.

触发机制结论 (2026-08-03 实测 12+ 问题):
  - 绝对分数/覆盖率/top-ratio 均不可作自动触发 (分数带交叠, 覆盖率全 1.00,
    ratio good/bad 重叠) -> 采用【显式 "deep":true】为唯一触发方式.
  - 检索精度: 单字倒排显著优于旧 RAG1 全扫描 (肺癌/白疕/带状疱疹命中修正),
    但罕见病仍有误配 (宫外孕->植发), 属字符级检索固有局限.

Usage:
    python3 chinese_v4/kb/build_sd_index.py                 # 全量构建
    python3 chinese_v4/kb/build_sd_index.py --max-docs 5000 # 受限构建(测试)
    python3 chinese_v4/kb/build_sd_index.py --verify-only   # 仅检索验证
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

DATA_V4 = Path("/mnt/d/codes/esp32-ai/data_v4")
KB = DATA_V4 / "kb" / "format_data.jsonl"
OUT_DIR = DATA_V4 / "sd_rag"

DOC_CHARS = 40          # evidence block length (prompt budget)
IDF_SCALE = 64.0


def build_ngrams(text):
    """Unigram terms only (medical char set is bounded)."""
    return set(text)


def load_entries(max_docs):
    entries = []
    with open(KB, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("question", "") or "").strip()
            a = (d.get("answer", "") or "").strip()
            label = d.get("label", "")
            if len(q) >= 3 and len(a) >= 20:
                entries.append((q, a, label))
            if max_docs and len(entries) >= max_docs:
                break
    return entries


def build_index(entries):
    """term -> doc list (doclist per term) + doc blocks."""
    docs = [(a[:DOC_CHARS], label) for q, a, label in entries]
    inverted = {}
    for di, (q, a, label) in enumerate(entries):
        for ch in set(q + a[:DOC_CHARS]):
            if ch.strip() and not ch.isspace():
                inverted.setdefault(ch, []).append(di)

    N = len(docs)
    idf = {}
    for t, dlist in inverted.items():
        df = len(dlist)
        idf[t] = min(int(IDF_SCALE * math.log(1.0 + N / max(df, 1))), 255)
    return docs, inverted, idf


def serialize(docs, inverted, idf, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    terms = sorted(inverted.keys())

    # ---- index.bin: term table + doclists ----
    table_bytes = 2 + sum(1 + len(t.encode()) + 4 + 2 for t in terms)
    offsets = {}
    off = table_bytes
    for t in terms:
        offsets[t] = off
        off += len(inverted[t]) * 2

    with open(out_dir / "index.bin", "wb") as f:
        f.write(struct.pack("<H", len(terms)))
        for t in terms:
            tb = t.encode("utf-8")
            f.write(struct.pack("<B", len(tb)))
            f.write(tb)
            f.write(struct.pack("<I", offsets[t]))
            f.write(struct.pack("<H", len(inverted[t])))
        for t in terms:
            f.write(struct.pack(f"<{len(inverted[t])}H", *inverted[t]))

    # ---- docs.bin ----
    with open(out_dir / "docs.bin", "wb") as f:
        f.write(struct.pack("<H", len(docs)))
        for doc, label in docs:
            tb = doc.encode("utf-8")
            lb = label.encode("utf-8")[:20]
            f.write(struct.pack("<HH", len(tb), len(lb)))
            f.write(tb)
            f.write(lb)

    # ---- meta.bin: term -> idf ----
    with open(out_dir / "meta.bin", "wb") as f:
        f.write(struct.pack("<I", len(docs)))
        f.write(struct.pack("<H", len(terms)))
        for t in terms:
            tb = t.encode("utf-8")
            f.write(struct.pack("<B", len(tb)))
            f.write(tb)
            f.write(struct.pack("<B", idf[t]))

    idx_sz = (out_dir / "index.bin").stat().st_size
    table_sz = table_bytes
    print(f"index.bin: {idx_sz/2**10:.1f}KB ({len(terms)} terms)")
    print(f"  term table (常驻): {table_sz/2**10:.1f}KB (预算 64KB: {'OK' if table_sz <= 64*1024 else 'OVER'})")
    print(f"  doclists (流式):  {(idx_sz-table_sz)/2**10:.1f}KB")
    print(f"docs.bin:  {(out_dir/'docs.bin').stat().st_size/2**20:.2f}MB ({len(docs)} docs)")
    print(f"meta.bin:  {(out_dir/'meta.bin').stat().st_size/2**10:.1f}KB")


def load_index(out_dir):
    """PC-side reference rag_sd_load."""
    terms = []      # (term, offset, count)
    with open(out_dir / "index.bin", "rb") as f:
        n_terms = struct.unpack("<H", f.read(2))[0]
        for _ in range(n_terms):
            tl = struct.unpack("<B", f.read(1))[0]
            t = f.read(tl).decode("utf-8")
            off, cnt = struct.unpack("<IH", f.read(6))
            terms.append((t, off, cnt))

    doclists = {}
    with open(out_dir / "index.bin", "rb") as f:
        for t, off, cnt in terms:
            f.seek(off)
            doclists[t] = struct.unpack(f"<{cnt}H", f.read(cnt * 2))

    docs = []
    with open(out_dir / "docs.bin", "rb") as f:
        n_docs = struct.unpack("<H", f.read(2))[0]
        for _ in range(n_docs):
            tl, ll = struct.unpack("<HH", f.read(4))
            t = f.read(tl).decode("utf-8")
            label = f.read(ll).decode("utf-8")
            docs.append((t, label))

    idf = {}
    with open(out_dir / "meta.bin", "rb") as f:
        N = struct.unpack("<I", f.read(4))[0]
        n_terms = struct.unpack("<H", f.read(2))[0]
        for _ in range(n_terms):
            tl = struct.unpack("<B", f.read(1))[0]
            t = f.read(tl).decode("utf-8")
            idf[t] = struct.unpack("<B", f.read(1))[0]

    return terms, doclists, docs, idf


def retrieve(query, doclists, docs, idf, k=3):
    """Inverted lookup: query chars -> candidate docs -> score."""
    q_chars = set(query)
    scores = {}
    matched_terms = 0
    for ch in q_chars:
        if ch not in idf:
            continue
        matched_terms += 1
        w = idf[ch]
        for d in doclists.get(ch, ()):
            scores[d] = scores.get(d, 0) + w
    top = sorted(scores, key=scores.get, reverse=True)[:k]
    coverage = matched_terms / max(len(q_chars), 1)
    return top, scores, coverage


def verify():
    terms, doclists, docs, idf = load_index(OUT_DIR)
    print(f"loaded: {len(terms)} terms, {len(docs)} docs, {len(idf)} idf\n")

    tests = [
        "感冒发烧", "肺癌早期症状", "急性重型肝炎", "糖尿病酮症酸中毒",
        "白疕皮损特点", "宫外孕如何治疗", "高血压用药", "带状疱疹后遗神经痛",
        "肝硬化腹水治疗", "儿童肺炎支原体感染", "肝豆状核变性", "心肌梗死急救",
    ]
    print(f"{'问题':<12} {'top1':>5} {'top3':>5} {'ratio':>6} {'cov':>5}  命中")
    for q in tests:
        top, scores, cov = retrieve(q, doclists, docs, idf)
        if not top:
            print(f"{q:<12}  ----  (no match, cov={cov:.2f})")
            continue
        t1 = scores[top[0]]
        t3 = scores[top[2]] if len(top) >= 3 else scores[top[-1]]
        ratio = t1 / max(t3, 1)
        doc, label = docs[top[0]]
        print(f"{q:<12} {t1:>5} {t3:>5} {ratio:>6.2f} {cov:>5.2f}  ({label}) {doc[:24]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-docs", type=int, default=0)
    ap.add_argument("--verify-only", action="store_true")
    args = ap.parse_args()

    if args.verify_only:
        verify()
        return

    print("[1/3] loading KB entries ...")
    entries = load_entries(args.max_docs)
    print(f"  {len(entries)} entries")

    print("[2/3] building inverted index ...")
    docs, inverted, idf = build_index(entries)

    print("[3/3] serializing ...")
    serialize(docs, inverted, idf, OUT_DIR)

    print("\n=== verification ===")
    verify()


if __name__ == "__main__":
    main()
