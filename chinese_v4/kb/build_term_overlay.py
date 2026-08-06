"""
build_term_overlay.py — 术语叠加层索引 (方案 B, 修复单字 IDF 结构性误配)

问题 (RAG_INDEX_ANALYSIS_20260806.md §2.1):
  单字 IDF 饱和 (84% term = 255), 无法识别多字术语边界
  -> 肝豆状核 → 梅核气 (错), 白疕 → 白癜风 (近似错)

方案 B (Oracle 定案):
  保留单字索引 (80% 基线), 另建 368 医学词条 -> docs 映射叠加层.
  设备端 FMM (正向最大匹配) 切问题 -> 术语命中加权 -> 单字兜底.

输出 term_overlay.bin (全量 ~15KB, PSRAM 常驻, 无需 SD 流式):
  Header:  magic "RAG2" (4) + n_terms (u16) + n_docs (u32)
  Term table:  每 term: u8 len + UTF-8 + u32 doclist_off + u16 doc_count
  Doclists:    u16 doc_id[]  (n_docs < 65536, 用 u16 省空间)

用法:
  python3 chinese_v4/kb/build_term_overlay.py              # 构建
  python3 chinese_v4/kb/build_term_overlay.py --verify     # PC 检索验证
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

DATA_V4 = Path("/mnt/d/codes/esp32-ai/data_v4")
KB = DATA_V4 / "kb" / "format_data.jsonl"
OUT = DATA_V4 / "sd_rag" / "term_overlay.bin"
MED_JIEBA = Path("/mnt/d/codes/minimind/out/medical_jieba.txt")
DOC_CHARS = 40          # 与 build_sd_index.py 一致 (证据截取)
IDX_CHARS = 80          # 索引 FMM 窗口: 需覆盖多字术语 (40 字会截断"肝豆状核变性")

MAGIC = b"RAG2"

# 明显非医学短语 (KB 提取时混入的指南头尾), 过滤防稀释
NOISE = ("严防", "商业", "疫情监测", "自我监测", "人员", "原则", "病例",
         "法规", "报告", "管理", "登记", "流程", "审核")


def load_terms():
    """读 medical_jieba.txt (jieba userdict: 词 词频 词性) -> 过滤后词表."""
    if not MED_JIEBA.exists():
        print(f"[ERR] 词表不存在: {MED_JIEBA}")
        sys.exit(1)
    terms = []
    for line in MED_JIEBA.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 1:
            continue
        t = parts[0].strip()
        if len(t) < 2:               # 单字无意义 (单字索引已有)
            continue
        if any(n in t for n in NOISE):   # 非医学短语
            continue
        terms.append(t)
    terms = sorted(set(terms), key=len, reverse=True)   # FMM: 长词优先
    print(f"词表: {len(terms)} 词条 (过滤 {len(set(l.split()[0] for l in MED_JIEBA.read_text(encoding='utf-8').splitlines() if l.strip())) - len(terms)} 噪音)")
    return terms


def load_docs():
    """读 format_data.jsonl -> [(q, a)] (与 build_sd_index.py load_entries 同源)."""
    docs = []
    if not KB.exists():
        print(f"[ERR] KB 不存在: {KB}")
        sys.exit(1)
    with open(KB, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            q = (d.get("question", "") or "").strip()
            a = (d.get("answer", "") or "").strip()
            if len(q) >= 3 and len(a) >= 20:
                docs.append((q, a))
    print(f"docs: {len(docs)}")
    return docs


def fmm(text, terms):
    """正向最大匹配: 返回 text 中命中的词表术语集合."""
    terms_by_char = {}
    for t in terms:
        terms_by_char.setdefault(t[0], []).append(t)
    hits = set()
    i = 0
    n = len(text)
    while i < n:
        cands = terms_by_char.get(text[i], ())
        matched = None
        for t in cands:             # 已按长度降序, 最长匹配
            if text.startswith(t, i):
                matched = t
                break
        if matched:
            hits.add(matched)
            i += len(matched)
        else:
            i += 1
    return hits


def is_toc_doc(q, a):
    """目录文档判别: q 以'第X章'开头 且 a 为目录行 (第X章 ... 页码)."""
    if not re.match(r'^第[一二三四五六七八九十百0-9]+章', q or ""):
        return False
    toc_lines = 0
    for ln in (a or "").splitlines():
        if re.match(r'^第[一二三四五六七八九十百0-9]+章.*\.\.\..*?\d+$', ln):
            toc_lines += 1
    return toc_lines >= 2


def build(terms, docs):
    """term -> doc_id 倒排 (u16). 去换行 (多字术语跨行断裂) + 跳过目录文档."""
    inv = {}
    skipped_toc = 0
    for di, (q, a) in enumerate(docs):
        if is_toc_doc(q, a):
            skipped_toc += 1
            continue
        text = (q or "").replace("\n", "") + (a or "").replace("\n", "")[:IDX_CHARS]
        for t in fmm(text, terms):
            inv.setdefault(t, []).append(di)
    print(f"跳过目录文档: {skipped_toc}")
    covered = sum(1 for t in terms if t in inv)
    print(f"覆盖: {covered}/{len(terms)} 词条命中文档")
    return inv


def serialize(terms, inv, out):
    # 只写有 doclist 的词条
    used = [t for t in terms if t in inv]
    n_terms = len(used)
    # term table bytes = sum(1 + len(t) + 4 + 2)
    table_bytes = sum(1 + len(t.encode()) + 4 + 2 for t in used)
    offsets = {}
    off = 4 + 2 + 4 + table_bytes        # magic + n_terms + n_docs + table
    for t in used:
        offsets[t] = off
        off += len(inv[t]) * 2
    with open(out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<H", n_terms))
        f.write(struct.pack("<I", len(docs_global)))
        for t in used:
            tb = t.encode("utf-8")
            f.write(struct.pack("<B", len(tb)))
            f.write(tb)
            f.write(struct.pack("<I", offsets[t]))
            f.write(struct.pack("<H", len(inv[t])))
        for t in used:
            f.write(struct.pack(f"<{len(inv[t])}H", *inv[t]))
    sz = out.stat().st_size
    print(f"term_overlay.bin: {sz/1024:.1f}KB ({n_terms} terms)")


docs_global = []     # 供 serialize 用


def verify(terms, inv, docs):
    """PC 参考检索: FMM 术语加权 + 单字兜底."""
    tests = [
        "肺癌早期症状", "高血压诊断标准", "糖尿病临床表现", "肝硬化腹水",
        "急性心肌梗死", "宫外孕", "肝豆状核变性", "上消化道出血",
        "不孕不育", "白疕",
    ]
    print(f"\n{'查询':<10} {'术语':<12} 命中")
    for q in tests:
        qhits = fmm(q, terms)
        hittxt = "|".join(qhits) if qhits else "-"
        # 术语命中的 doc 集合
        term_docs = {}
        for t in qhits:
            for d in inv.get(t, ()):
                term_docs[d] = term_docs.get(d, 0) + 1
        if term_docs:
            best = sorted(term_docs, key=term_docs.get, reverse=True)[0]
            doc = docs[best][1][:24]
            print(f"{q:<10} {hittxt:<12} doc{best}: {doc}")
        else:
            print(f"{q:<10} {hittxt:<12} (无术语命中, 单字兜底)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    terms = load_terms()
    global docs_global
    docs_global = load_docs()

    if args.verify:
        # 需重建 inv
        inv = build(terms, docs_global)
        verify(terms, inv, docs_global)
        return

    print("[1/3] 构建术语倒排 ...")
    inv = build(terms, docs_global)
    print("[2/3] 序列化 ...")
    serialize(terms, inv, OUT)
    print("[3/3] 验证 ...")
    verify(terms, inv, docs_global)


if __name__ == "__main__":
    main()
