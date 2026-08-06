"""
chinese_v4 KB builder — clinical guideline sections → KB entries.

Converts guidebook/medica sections into format_data.jsonl entries
({id, question, answer, score, label}) merged with the existing V2/V3 KB,
then rebuilds the index.bin via the shared build_index.py.

Usage:
    python3 chinese_v4/kb/build_guide_kb.py --max-entries 15000
    python3 chinese_v4/kb/build_guide_kb.py --rebuild-only   # skip extraction
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from chinese_v4.prepare import clean_guide_md  # noqa: E402
from chinese_v4.build_sft import split_by_headings, heading_to_questions  # noqa: E402

DATA_V4 = Path("/mnt/d/codes/esp32-ai/data_v4")
V3_KB = Path("/mnt/d/codes/esp32-ai/data_v3/kb/format_data.jsonl")
OUT_KB = DATA_V4 / "kb" / "format_data.jsonl"
GUIDE_DIR = Path("/mnt/d/docs/raw/临床诊疗指南全集")
MEDICA_DIR = Path("/mnt/d/docs/raw/medica")

RE_TITLE = re.compile(r"^title:\s*(.+)$", re.IGNORECASE)

# noise headings that must never become QA pairs
RE_NOISE_HEAD = re.compile(
    r"(电子书|PDF|代找|QQ|微信|购书|出版社|版权|版权所有|侵权|CIP|在版编目|"
    r"定价|新华书店|印刷|联系电话|联系|邮箱|热线|说明|目录|封面|前言|序言|"
    r"保留部分版权|许可协议|知识产权|组织西太平洋|图书在版|引言|序\s*$|"
    r"ZYYXH|ISO|GB\s*[0-9]|中华中医药学会|中国中医药出版社|WHO|ISBN)", re.IGNORECASE)

# require the heading to look like a clinical topic (disease/symptom/therapy)
RE_CLINICAL_HEAD = re.compile(
    r"(诊断|治疗|预防|用药|管理|规范|指南|护理|康复|预后|症状|综合征|病|炎|"
    r"癌|瘤|感染|损伤|评估|筛查|标准|原则|适应证|禁忌|随访|监测|分期|分级|"
    r"急救|中毒|出血|休克|发热|疼痛|方案|流程|操作|检查|检验|影像|病理|"
    r"变性|肝豆状核|视网膜色素变性|黄斑变性)")


def doc_label(path):
    """Infer clinical department/label from path or frontmatter."""
    parts = path.parts
    for p in parts:
        if "L1_" in p:
            return "卫健委规范"
        if "L2_" in p:
            return "行业标准"
        if "L3_" in p:
            return "学会指南"
        if "L4_" in p:
            return "诊疗规范"
    # try frontmatter type/tags
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
        fm = re.search(r"^---\n(.*?)\n---", raw, re.DOTALL)
        if fm:
            m = re.search(r"tags:\s*\[([^\]]*)\]", fm.group(1))
            if m:
                return m.group(1).strip()[:20]
            m = re.search(r"type:\s*(\S+)", fm.group(1))
            if m:
                return m.group(1)
    except Exception:
        pass
    return "临床指南"


NON_MEDICAL_LABEL = ['健康管理', '理赔', '产品条款', '销售', '消保']


def is_medical_label(label):
    """排除保险/健康管理域 (与 minimind rag_medical.py med_only 对齐)."""
    return not any(k in label for k in NON_MEDICAL_LABEL)


def extract_entries(max_entries):
    entries = []
    eid = 10_000_000
    for d in (GUIDE_DIR, MEDICA_DIR):
        if not d.exists():
            continue
        mds = [m for m in d.rglob("*.md")
               if "_index" not in m.name and "_ocr" not in m.name]
        for m in mds:
            raw = m.read_text(encoding="utf-8", errors="replace")
            cleaned = clean_guide_md(raw)
            label = doc_label(m)
            if not is_medical_label(label):
                continue  # 健康管理/理赔/销售等非医学域整文件跳过
            for level, head, body in split_by_headings(cleaned):
                body_text = "\n".join(body).strip()
                if len(body_text) < 40 or len(body_text) > 1500:
                    continue
                if RE_NOISE_HEAD.search(head):
                    continue
                if not RE_CLINICAL_HEAD.search(head):
                    continue
                qs = heading_to_questions(head)
                if not qs:
                    continue
                # one entry per section (first question as retrieval key)
                entries.append({
                    "id": eid, "question": qs[0],
                    "answer": body_text[:1200], "score": 5, "label": label,
                })
                eid += 1
                if len(entries) >= max_entries:
                    return entries
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-entries", type=int, default=15000)
    ap.add_argument("--rebuild-only", action="store_true")
    args = ap.parse_args()

    DATA_V4.mkdir(parents=True, exist_ok=True)
    (DATA_V4 / "kb").mkdir(parents=True, exist_ok=True)

    entries = []
    if not args.rebuild_only:
        print("[1/3] extracting guide KB entries ...")
        entries = extract_entries(args.max_entries)
        print(f"  guide entries: {len(entries)}")

        # merge existing V3 KB (93.5K) — but the index format stores doc
        # lists as uint16 (max 65535 docs per term) AND the kb flash
        # partition is only 2MB (~11.4K docs at 5695 docs/MB).
        # Sample so the final index fits: guides priority, then V3.
        print("[2/3] merging V3 KB (sampled to fit 2MB partition) ...")
        v3_entries = []
        with open(V3_KB, encoding="utf-8", errors="replace") as f:
            for line in f:
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue
                v3_entries.append(d)
        print(f"  v3 entries: {len(v3_entries)}")
        # V3 只保留医学条目 (与 guide 医学过滤一致)
        v3_entries = [e for e in v3_entries if is_medical_label(e.get("label", ""))]
        print(f"  v3 medical entries: {len(v3_entries)}")
        import random
        rng = random.Random(42)
        rng.shuffle(v3_entries)
        # guides priority (cover disease chapters incl. 肝豆状核变性), V3 fill to 2MB budget
        guide_n = min(len(entries), 10500)
        budget = 11000 - guide_n
        merged = entries[:guide_n] + v3_entries[:max(budget, 0)]
        entries = merged
        print(f"  merged total: {len(entries)} (guide {guide_n} + v3 {len(entries)-guide_n})")

        with open(OUT_KB, "w", encoding="utf-8") as f:
            for e in entries:
                f.write(json.dumps(e, ensure_ascii=False) + "\n")
        print(f"  wrote {OUT_KB}")

    # rebuild index
    print("[3/3] rebuilding index.bin ...")
    import subprocess
    r = subprocess.run([
        sys.executable, "chinese/kb/build_index.py",
        "--kb", str(OUT_KB),
        "--tokenizer", str(DATA_V4 / "tokenizer.json"),
        "--out", str(DATA_V4 / "kb" / "index.bin"),
        "--sample", "200000",
    ], cwd=str(Path(__file__).resolve().parent.parent.parent))
    print(f"  build_index exit: {r.returncode}")


if __name__ == "__main__":
    main()
