"""
chinese_v4 SFT data builder — guide QA + V2 medical pools.

Builds the V4 SFT pool from four sources:
  A. 临床诊疗指南全集 + medica 章节 → 指南 QA 对 (NEW, V4 核心价值)
  B. zjydiary finetune   (1.95M medical QA)
  C. BenTsao 本草         (8.6K structured QA)
  D. HuatuoGPT2-SFT-GPT4  (142K multi-turn)

Guide QA generation: 每个指南 md 按 `##` 章节切块, 章节标题 → 问句
  ("{标题}的诊疗要点是什么" / "根据指南,{标题}如何处理" / "{标题}指南要点")
  answer = 章节正文 (清洗后).

Usage:
    python3 chinese_v4/build_sft.py --count 50000
    python3 chinese_v4/build_sft.py --guide-only --count 8000   # guide QA only
"""

import argparse
import json
import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from chinese.tokenizer import CharTokenizer  # noqa: E402
from chinese_v4.prepare import clean_guide_md  # noqa: E402

DATA_V4 = Path("/mnt/d/codes/esp32-ai/data_v4")
V2_RAW = Path("/mnt/d/codes/esp32-ai/data_v2/raw")
OUT = DATA_V4 / "sft"
GUIDE_DIR = Path("/mnt/d/docs/raw/临床诊疗指南全集")
MEDICA_DIR = Path("/mnt/d/docs/raw/medica")
PAD, UNK, BOS, EOS = 0, 1, 2, 3

RE_HEADING = re.compile(r"^#{1,6}\s+(.+)$")


def encode_instruction(tok, question, answer):
    tok_user, tok_assist, tok_end = tok.USER, tok.ASSIST, tok.END

    def ids_of(text):
        ids = []
        i = 0
        while i < len(text):
            if text.startswith("<user>", i):
                ids.append(tok_user); i += 5
            elif text.startswith("<assistant>", i):
                ids.append(tok_assist); i += 10
            elif text.startswith("<end>", i):
                ids.append(tok_end); i += 4
            else:
                ids.append(tok.stoi.get(text[i], UNK)); i += 1
        return ids

    prefix = ids_of(f"<user>{question}<end><assistant>")
    answer_ids = ids_of(answer)
    input_ids = [BOS] + prefix + answer_ids + [EOS]
    n_pref = len(prefix) + 1
    labels = [-100] * (n_pref - 1) + answer_ids + [EOS] + [-100]
    assert len(labels) == len(input_ids)
    return input_ids, labels


# ---------------------------------------------------------------------------
# Guide QA extraction
# ---------------------------------------------------------------------------
def split_by_headings(text):
    """Split cleaned md into (heading, body) sections."""
    lines = text.split("\n")
    sections = []            # [(level, heading, [body lines])]
    cur_level, cur_head, cur_body = 0, "", []
    for ln in lines:
        m = RE_HEADING.match(ln)
        if m:
            if cur_head:
                sections.append((cur_level, cur_head, cur_body))
            cur_level = len(m.group(1)) if m.group(1) else 0
            cur_head = m.group(1).strip()
            cur_body = []
        else:
            cur_body.append(ln)
    if cur_head:
        sections.append((cur_level, cur_head, cur_body))
    return sections


def heading_to_questions(heading):
    """Convert a chapter heading into plausible medical questions."""
    h = re.sub(r"^[\d\s.．、-]+", "", heading).strip()
    if len(h) < 2 or len(h) > 40:
        return []
    qs = []
    if any(k in h for k in ("诊断", "治疗", "预防", "指南", "规范", "用药", "管理", "处理")):
        qs.append(f"根据临床指南，{h}的内容要点有哪些")
        qs.append(f"{h}，具体应如何实施")
    else:
        qs.append(f"{h}的临床诊疗要点是什么")
        qs.append(f"临床实践中，{h}应如何处理")
    return qs


def load_guide_qa(max_sections=12000):
    """Extract QA pairs from guidebook + medica sections."""
    out = []
    for d in (GUIDE_DIR, MEDICA_DIR):
        if not d.exists():
            continue
        mds = [m for m in d.rglob("*.md")
               if "_index" not in m.name and "_ocr" not in m.name]
        for m in mds:
            raw = m.read_text(encoding="utf-8", errors="replace")
            cleaned = clean_guide_md(raw)
            for level, head, body in split_by_headings(cleaned):
                body_text = "\n".join(body).strip()
                if len(body_text) < 30 or len(body_text) > 2000:
                    continue
                for q in heading_to_questions(head):
                    out.append((q, body_text[:1500]))
                    if len(out) >= max_sections:
                        return out
    return out


# ---------------------------------------------------------------------------
# V2 pools
# ---------------------------------------------------------------------------
def load_pool(path, text_keys, sample_n, rng, is_huatuo=False):
    out = []
    if not path.exists():
        print(f"  missing: {path}")
        return []
    if is_huatuo:
        d = json.load(open(path, encoding="utf-8"))
        for item in d:
            convs = item.get("conversations", [])
            turns = []
            for c in convs:
                v = c.get("value", "")
                if isinstance(v, list):
                    v = "\n".join(str(x) for x in v)
                turns.append((c.get("from", ""), str(v or "").strip()))
            human = next((v for f, v in turns if f == "human"), "")
            gpt = next((v for f, v in turns if f in ("gpt", "assistant")), "")
            if len(human) >= 5 and len(gpt) >= 30:
                out.append((human, gpt))
    else:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue
                q = next((str(d.get(k, "") or "").strip() for k in text_keys
                          if d.get(k)), "")
                a = next((str(d.get(k, "") or "").strip() for k in
                          ("output", "answer", "text") if d.get(k)), "")
                if len(q) >= 5 and len(a) >= 30:
                    out.append((q, a))
    rng.shuffle(out)
    print(f"  {path.name}: collected {len(out)}, sample {sample_n}")
    return out[:sample_n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=50000)
    ap.add_argument("--guide", type=int, default=5000,
                    help="max guide QA pairs")
    ap.add_argument("--zjydiary", type=int, default=30000)
    ap.add_argument("--benchao", type=int, default=8000)
    ap.add_argument("--huatuogpt2", type=int, default=20000)
    ap.add_argument("--val-count", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--guide-only", action="store_true")
    args = ap.parse_args()

    tok = CharTokenizer.load(str(DATA_V4 / "tokenizer.json"))
    print(f"tokenizer: {tok.vocab_size}\n")
    rng = random.Random(args.seed)
    OUT.mkdir(parents=True, exist_ok=True)

    all_samples = []

    # A. guide QA (V4 核心)
    print("[A] guide QA ...")
    guide_qa = load_guide_qa(args.guide)
    rng.shuffle(guide_qa)
    guide_pick = guide_qa[:args.guide]
    all_samples += guide_pick
    print(f"  guide QA: {len(guide_pick)}")

    if not args.guide_only:
        # B/C/D. V2 pools
        print("[B] zjydiary ...")
        all_samples += load_pool(
            V2_RAW / "zjydiary_Medical" / "finetune" / "train_zh_0.json",
            ("instruction",), args.zjydiary, rng)
        print("[C] BenTsao ...")
        all_samples += load_pool(
            V2_RAW / "benchao" / "llama_data.json", ("instruction",),
            args.benchao, rng)
        print("[D] HuatuoGPT2 ...")
        all_samples += load_pool(
            V2_RAW / "huatuogpt2" / "HuatuoGPT2-GPT4-SFT-140K.json",
            (), args.huatuogpt2, rng, is_huatuo=True)

    rng.shuffle(all_samples)
    all_samples = all_samples[:args.count]
    val = all_samples[:args.val_count]
    train = all_samples[args.val_count:]
    print(f"\ntotal: {len(all_samples)} (train {len(train)}, val {len(val)})")

    def encode_list(items):
        return [{"input_ids": i, "labels": l}
                for q, a in items
                for i, l in [encode_instruction(tok, q, a)]]

    with open(OUT / "sft_train.json", "w", encoding="utf-8") as f:
        json.dump({"data": encode_list(train), "format": "char-level",
                   "special_tokens": {"user": tok.USER, "assistant": tok.ASSIST,
                                      "end": tok.END}}, f, ensure_ascii=False)
    with open(OUT / "sft_val.json", "w", encoding="utf-8") as f:
        json.dump({"data": encode_list(val), "format": "char-level"},
                  f, ensure_ascii=False)
    print(f"wrote {OUT}/sft_train.json + sft_val.json")


if __name__ == "__main__":
    main()
