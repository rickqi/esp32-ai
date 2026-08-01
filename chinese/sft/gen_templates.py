"""
Local QA template construction — Path C of the data-expansion plan.

Extracts "entity + aspect" pairs from the pretraining corpus using anchor
keywords, then builds QA pairs where the answer is the verbatim source
sentence (grounded, no API cost).

Usage:
    uv run python chinese/sft/gen_templates.py --count 1000
    uv run python chinese/sft/gen_templates.py --count 1000 --out data_chinese/sft/processed/templates.json
"""

import argparse
import json
import random
import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
CORPUS = PROJECT_ROOT / "data_chinese" / "corpus.txt"

# Anchor aspects + their keyword patterns
ASPECT_PATTERNS = [
    ("诊断标准", ["诊断标准", "诊断依据", "诊断要点", "确诊标准"]),
    ("治疗方案", ["治疗方案", "治疗原则", "治疗方法", "治疗策略"]),
    ("临床表现", ["临床表现", "临床特征", "症状", "典型表现"]),
    ("并发症", ["并发症", "术后并发症"]),
    ("禁忌证", ["禁忌证", "禁忌症", "绝对禁忌"]),
    ("适应证", ["适应证", "适应症", "适应范围"]),
    ("鉴别诊断", ["鉴别诊断"]),
    ("操作步骤", ["操作步骤", "操作规程", "手术步骤"]),
    ("分期分级", ["分期", "分级", "TNM"]),
    ("预后", ["预后", "生存率"]),
    ("预防措施", ["预防措施", "预防方法", "防控"]),
    ("随访", ["随访", "复查"]),
]

# Stopwords / noisy tokens that shouldn't be the "entity"
STOP = {"的", "与", "和", "或", "及", "等", "中", "后", "前", "内", "外",
        "患者", "病人", "临床", "治疗", "诊断", "进行", "以及", "包括",
        "根据", "按照", "对于", "通过", "采用", "使用"}

# Entity should plausibly end with a domain noun
ENTITY_SUFFIXES = ("癌", "病", "症", "瘤", "综合征", "综合症", "炎", "痛",
                   "肿", "肉瘤", "肿瘤", "血症", "障碍", "损伤", "衰竭",
                   "感染", "出血", "梗死", "坏死", "病变", "骨折", "结核")


def extract_entity_around(sentence: str, anchor_start: int) -> str:
    """Extract a plausible entity immediately before the anchor keyword."""
    before = sentence[:anchor_start]
    # candidate = last 2-8 chars before anchor; must end with a domain noun
    for ln in range(8, 1, -1):
        cand = before[-ln:].strip()
        if not cand:
            continue
        # reject if starts with number/order/pattern or contains punctuation
        if re.match(r"^[\d\s.()（）【】\[\]一二三四五六七八九十]+", cand):
            continue
        if any(p in cand for p in "，。；：？！、（）()《》\"'【】"):
            continue
        if not cand.endswith(ENTITY_SUFFIXES):
            continue
        if any(cand.endswith(w) for w in STOP):
            continue
        return cand
    return None


def build_samples(max_count: int, seed: int = 42) -> list:
    rng = random.Random(seed)
    if not CORPUS.exists():
        print(f"corpus not found: {CORPUS}")
        return []

    text = CORPUS.read_text(encoding="utf-8")
    # Split into sentences on Chinese punctuation
    sentences = re.split(r"[。！？\n]+", text)
    print(f"corpus: {len(text):,} chars, {len(sentences):,} sentences")

    samples = []
    for sentence in sentences:
        s = sentence.strip()
        if len(s) < 20 or len(s) > 200:  # too short/long to be a clean QA
            continue
        for aspect, keywords in ASPECT_PATTERNS:
            for kw in keywords:
                idx = s.find(kw)
                if idx <= 0:
                    continue
                entity = extract_entity_around(s, idx)
                if not entity or len(entity) < 2:
                    continue
                # avoid pure numbers / weird tokens
                if re.fullmatch(r"[\d\sA-Za-z]+", entity):
                    continue
                q = f"{entity}的{aspect}是什么？"
                a = s
                samples.append({
                    "question": q,
                    "answer": a,
                    "entity": entity,
                    "aspect": aspect,
                    "source": "template",
                })
                break  # one aspect per sentence
        if len(samples) >= max_count * 10:  # over-sample for dedup later
            break

    # Dedupe by question, cap
    seen = set()
    deduped = []
    rng.shuffle(samples)
    for s in samples:
        if s["question"] in seen:
            continue
        seen.add(s["question"])
        deduped.append(s)
        if len(deduped) >= max_count:
            break

    print(f"extracted {len(samples)} raw, deduped to {len(deduped)}")
    from collections import Counter
    print(f"aspect dist: {dict(Counter(s['aspect'] for s in deduped))}")
    return deduped


def main():
    ap = argparse.ArgumentParser(description="Build local template QA pairs")
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", default=str(PROJECT_ROOT / "data_chinese" / "sft"
                                          / "processed" / "templates.json"))
    args = ap.parse_args()

    samples = build_samples(args.count, args.seed)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(samples, f, ensure_ascii=False, indent=2)
    print(f"saved {len(samples)} templates -> {out}")

    # Show samples
    for s in samples[:5]:
        print(f"\nQ: {s['question']}")
        print(f"A: {s['answer'][:80]}...")


if __name__ == "__main__":
    main()
