"""
DeepSeek high-quality QA generator — Path v2 of the data-expansion plan.

Turns document segments from the pretraining corpus into grounded QA pairs:
for each segment, DeepSeek generates 3-5 question/answer pairs whose answers
are based on the segment text. This yields high-quality instruction data that
small models can actually learn "question -> precise answer" from.

Usage:
    uv run python chinese/sft/gen_qa_deepseek.py --n-segments 200 --qa-per-segment 4
    uv run python chinese/sft/gen_qa_deepseek.py --dry-run
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
OUT_DIR = PROJECT_ROOT / "data_chinese" / "sft" / "processed"

SEGMENT_MIN = 400
SEGMENT_MAX = 900

PROMPT_TMPL = """你是医学/保险领域专家。请根据以下文档段落，生成 {n} 个高质量问答对。

要求：
1. 问题必须能从该段落直接回答（有原文依据）
2. 问题要多样化：事实型、诊断型、流程型、对比型等
3. 答案要准确、完整，可直接引用段落内容
4. 输出 JSON 数组，格式: [{{"question": "...", "answer": "..."}}]

文档段落：
{segment}

请只输出 JSON 数组，不要其他内容。"""


def load_api_key():
    env_path = Path(r"D:\docs\doc-search\.env")
    if env_path.exists():
        for line in env_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "DEEPSEEK_API_KEY" in line:
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    return ""


def call_deepseek(prompt, key, model="deepseek-chat", max_tokens=1200, retries=3):
    import urllib.request
    import urllib.error
    payload = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0.7,
    }).encode()
    for attempt in range(retries):
        try:
            req = urllib.request.Request(
                "https://api.deepseek.com/chat/completions",
                data=payload,
                headers={"Content-Type": "application/json",
                         "Authorization": f"Bearer {key}"},
            )
            resp = json.load(urllib.request.urlopen(req, timeout=120))
            return resp["choices"][0]["message"]["content"]
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(5 * (attempt + 1))
            else:
                print(f"  API fail: {e}")
    return None


def parse_qa_json(text):
    """Extract JSON array from model output (tolerate markdown fences)."""
    if not text:
        return []
    text = text.strip()
    # strip ```json ... ``` fences
    m = re.search(r"```(?:json)?\s*(\[.*?\])\s*```", text, re.DOTALL)
    if m:
        text = m.group(1)
    try:
        data = json.loads(text)
        if isinstance(data, list):
            return [d for d in data if isinstance(d, dict)
                    and d.get("question") and d.get("answer")]
    except json.JSONDecodeError:
        # try to find array start
        start = text.find("[")
        end = text.rfind("]")
        if start >= 0 and end > start:
            try:
                data = json.loads(text[start:end + 1])
                if isinstance(data, list):
                    return [d for d in data if isinstance(d, dict)
                            and d.get("question") and d.get("answer")]
            except json.JSONDecodeError:
                pass
    return []


def split_segments(corpus_text, n_segments, min_len=SEGMENT_MIN, max_len=SEGMENT_MAX):
    """Split corpus into ~500-900 char segments, skipping junk."""
    segments = []
    # split on double newlines (paragraph breaks)
    blocks = re.split(r"\n\s*\n", corpus_text)
    current = ""
    for b in blocks:
        b = b.strip()
        if len(b) < 30:
            continue
        if len(current) + len(b) < max_len:
            current += b + "\n"
        else:
            if len(current) >= min_len:
                segments.append(current.strip())
            current = b + "\n"
    if len(current) >= min_len:
        segments.append(current.strip())
    # filter: need meaningful Chinese
    def has_chinese(s):
        return sum(1 for c in s if "\u4e00" <= c <= "\u9fff") > 50
    segments = [s for s in segments if has_chinese(s)]
    return segments[:n_segments]


def main():
    ap = argparse.ArgumentParser(description="DeepSeek QA generator (paragraph -> QA)")
    ap.add_argument("--corpus", default=str(PROJECT_ROOT / "data_chinese" / "corpus.txt"))
    ap.add_argument("--n-segments", type=int, default=1500)
    ap.add_argument("--qa-per-segment", type=int, default=4)
    ap.add_argument("--start", type=int, default=0, help="resume from segment index")
    ap.add_argument("--out", default=str(OUT_DIR / "deepseek_qa.jsonl"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--sleep", type=float, default=0.5, help="delay between calls")
    args = ap.parse_args()

    key = load_api_key()
    if not key:
        print("DeepSeek key not found in D:\\docs\\doc-search\\.env")
        sys.exit(1)
    print(f"API key: {key[:8]}...")

    corpus = Path(args.corpus).read_text(encoding="utf-8")
    segments = split_segments(corpus, args.n_segments)
    print(f"segments: {len(segments)} (from {args.start})")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    # load existing checkpoint
    done_questions = set()
    if out.exists():
        for line in out.open(encoding="utf-8"):
            try:
                d = json.loads(line)
                done_questions.add(d["question"])
            except Exception:
                pass
        print(f"checkpoint: {len(done_questions)} questions already generated")

    if args.dry_run:
        print("=== DRY RUN ===")
        print(f"segment 0 preview: {segments[0][:120]}...")
        print("prompt:")
        print(PROMPT_TMPL.format(n=args.qa_per_segment, segment=segments[0][:300]))
        return

    total_new = 0
    f = open(out, "a", encoding="utf-8")
    try:
        for i in range(args.start, len(segments)):
            seg = segments[i]
            prompt = PROMPT_TMPL.format(n=args.qa_per_segment, segment=seg)
            text = call_deepseek(prompt, key)
            qas = parse_qa_json(text) if text else []
            new = 0
            for qa in qas:
                q = qa["question"].strip()
                a = qa["answer"].strip()
                if not q or not a or q in done_questions:
                    continue
                if len(q) < 5 or len(a) < 30:
                    continue
                rec = {"question": q, "answer": a, "segment_idx": i}
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
                done_questions.add(q)
                total_new += 1
                new += 1
            if (i - args.start) % 10 == 0 or new:
                print(f"[{i}/{len(segments)}] +{new} (total {total_new})", flush=True)
            time.sleep(args.sleep)
    finally:
        f.close()

    print(f"\nDone: {total_new} new QA -> {out}")
    print(f"Total in file: {len(done_questions)}")


if __name__ == "__main__":
    main()
