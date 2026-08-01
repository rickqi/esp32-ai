"""
SFT data builder for the Chinese ESP32 model — Phase 0 of the SFT plan.

Fully isolated from the English pipeline. Builds the instruction-following
dataset from two sources:

  A. WSL medical ChatML data (923 train + 50 val + 80 CoT)
     \\wsl.localhost\\Ubuntu-22.04\\home\\LLMs-from-scratch\\projects\\
       chinese-medical-text-generation\\docs\\med_instruction_*_chatml.json
  B. D:\\docs\\search_logs QA logs (184 md files, ~169 valid, cleaned to ~100+)

Output archive (gitignored, local):
  data_chinese/sft/raw/        — verbatim copies of source data
  data_chinese/sft/processed/  — tokenised samples with loss masks
  data_chinese/sft/split/      — plain-text train/val for anti-forgetting mix

Special tokens (must match chinese/tokenizer.py after extension):
  4 = <user>      5 = <assistant>      6 = <end>
  (0=PAD 1=UNK 2=BOS 3=EOS already defined)

Usage:
    uv run python chinese/sft/sft_data.py
    uv run python chinese/sft/sft_data.py --skip-search-logs
"""

import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

# ---- special token ids: PAD/UNK/BOS/EOS fixed; SFT markers appended after
# chars (dynamic ids read from the tokenizer) ------------------------------
PAD, UNK, BOS, EOS = 0, 1, 2, 3

RAW_DIR = PROJECT_ROOT / "data_chinese" / "sft" / "raw"
PROC_DIR = PROJECT_ROOT / "data_chinese" / "sft" / "processed"
SPLIT_DIR = PROJECT_ROOT / "data_chinese" / "sft" / "split"

WSL_DOCS = Path(r"\\wsl.localhost\Ubuntu-22.04\home\LLMs-from-scratch\projects"
                r"\chinese-medical-text-generation\docs")
SEARCH_LOGS = Path(r"D:\docs\search_logs")

MIN_INSTRUCTION_CHARS = 5      # drop junk prompts like "北京"
MIN_RESPONSE_CHARS = 30        # drop empty / trivial responses
MAX_RESPONSE_CHARS = 3000      # drop absurdly long dumps


# ---------------------------------------------------------------------------
#  Source A: WSL ChatML
# ---------------------------------------------------------------------------

def pull_wsl_chatml():
    """Copy the WSL ChatML files into the raw archive.

    Primary source is the merged med_instruction_chatml.json (includes
    targeted-generation additions); val and cot stay separate.
    """
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    copied = []
    for name in ["med_instruction_chatml.json",
                 "med_instruction_val_chatml.json",
                 "med_instruction_cot_chatml.json"]:
        src = WSL_DOCS / name
        dst = RAW_DIR / name
        if src.exists():
            shutil.copy2(src, dst)
            copied.append(name)
        else:
            print(f"  [warn] WSL file missing: {src}")
    print(f"  copied {len(copied)} WSL files: {copied}")
    return copied


def load_wsl_chatml(name):
    p = RAW_DIR / name
    if not p.exists():
        return []
    d = json.load(open(p, encoding="utf-8"))
    return d.get("data", []) if isinstance(d, dict) else d


# ---------------------------------------------------------------------------
#  Source B: search_logs
# ---------------------------------------------------------------------------

def parse_search_logs():
    """Extract (instruction, response) pairs from all srch_*.md files."""
    pairs = []
    if not SEARCH_LOGS.is_dir():
        print(f"  [warn] search_logs dir missing: {SEARCH_LOGS}")
        return pairs
    for fp in sorted(SEARCH_LOGS.glob("srch_*.md")):
        try:
            content = fp.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        # Strip YAML frontmatter
        m = re.match(r"^---\n.*?\n---\n", content, re.DOTALL)
        if m:
            content = content[m.end():]
        # Instruction
        im = re.search(r"# Instruction\n+(.*?)(?=\n# |\Z)", content, re.DOTALL)
        rm = re.search(r"# Response\n+(.*?)(\Z)", content, re.DOTALL)
        if not im or not rm:
            continue
        inst = im.group(1).strip()
        resp = rm.group(1).strip()
        pairs.append({"instruction": inst, "response": resp,
                      "source": fp.name})
    print(f"  parsed {len(pairs)} search_log pairs")
    return pairs


def is_list_only_response(text):
    """True if the response is just a 'found N results: doc1, doc2...' listing."""
    s = text.strip()
    if s.startswith("找到") and ("结果" in s or "个结果" in s):
        # contains a colon then mostly document names / separators
        body = s.split(":", 1)[-1] if ":" in s else s
        # if the body has almost no Chinese content beyond separators
        zh = sum(1 for c in body if "\u4e00" <= c <= "\u9fff")
        if len(body) > 20 and zh / len(body) < 0.2:
            return True
    return False


# Tool-call / retrieval-routing residue from search_logs (not real answers)
TOOL_RESIDUE = [
    "<tool_call>", "<arg_key>", "<arg_value>", "doc_id",
    "我需要读取", "让我先读取", "让我读取", "让我先搜索", "让我进行搜索",
    "让我查看", "让我查一下", "正在读取文档", "为了回答该问题，我需要",
    "为了回答这个问题，我需要", "为了获取相关文档",
]
# Negative / non-answer responses (RAG found nothing — teaches model to refuse)
NEGATIVE_PATTERNS = [
    "我没有找到", "无法找到", "未找到任何", "没有找到与",
    "没有提及", "不包含任何", "没有包含", "不存在相关",
    "无法回答", "无法提供", "没有检索到", "未检索到",
    "我没有检索到", "没有在文档中", "未在文档中",
]


def is_low_quality_search_log(p):
    """True if a search-log pair should be dropped (tool residue / negation)."""
    resp = p["response"]
    if any(patt in resp for patt in TOOL_RESIDUE):
        return True
    if any(patt in resp for patt in NEGATIVE_PATTERNS):
        return True
    return False


def clean_search_logs(pairs):
    """Filter low-quality search-log QA pairs."""
    kept = []
    dropped_tool = 0
    dropped_neg = 0
    for p in pairs:
        inst = p["instruction"].strip()
        resp = p["response"].strip()
        if len(inst) < MIN_INSTRUCTION_CHARS:
            continue
        if len(resp) < MIN_RESPONSE_CHARS:
            continue
        if len(resp) > MAX_RESPONSE_CHARS:
            continue
        if is_list_only_response(resp):
            continue
        # must contain real Chinese in both
        if not re.search(r"[\u4e00-\u9fff]", inst):
            continue
        if not re.search(r"[\u4e00-\u9fff]", resp):
            continue
        if is_low_quality_search_log(p):
            if any(t in resp for t in TOOL_RESIDUE):
                dropped_tool += 1
            else:
                dropped_neg += 1
            continue
        kept.append(p)
    print(f"  cleaned: {len(pairs)} -> {len(kept)} "
          f"(dropped tool-residue={dropped_tool}, negative={dropped_neg})")
    return kept


# ---------------------------------------------------------------------------
#  Build samples
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
#  Synonym augmentation (Path B3)
# ---------------------------------------------------------------------------

SYNONYM_RULES = [
    (r"^(.*?)如何治疗$", r"\1怎么治"),
    (r"^(.*?)如何治疗$", r"\1的治疗方法"),
    (r"^(.*?)是什么$", r"什么是\1"),
    (r"^(.*?)有哪些$", r"\1包括哪些"),
    (r"^(.*?)有哪些$", r"列举\1"),
    (r"^(.*?)的临床表现$", r"\1有什么症状"),
    (r"^(.*?)的临床表现$", r"\1的典型表现"),
    (r"^(.*?)的预防措施$", r"如何预防\1"),
    (r"^(.*?)的禁忌证$", r"\1的禁忌情况"),
    (r"^(.*?)的适应证$", r"哪些情况适合\1"),
    (r"^(.*?)怎么(.*)$", r"\1如何\2"),
    (r"^(.*?)如何处理$", r"\1的处理原则"),
    (r"^(.*?)的标准$", r"\1的诊断标准"),
    (r"^(.*?)的流程$", r"\1的流程是什么"),
    (r"^(.*?)制度$", r"\1制度的内容"),
    (r"^(.*?)报告$", r"\1报告的主要内容"),
    (r"^(.*?)要求$", r"\1的要求有哪些"),
    (r"^(.*?)规定$", r"\1的规定是什么"),
    (r"^(.*?)区别$", r"\1的区别是什么"),
    (r"^(.*?)如何(.*)$", r"\1怎么\2"),
]


def augment_synonyms(question, max_variants=3):
    """Generate paraphrase variants of a question (conservative rules)."""
    variants = []
    for pat, repl in SYNONYM_RULES:
        m = re.match(pat, question)
        if m:
            v = re.sub(pat, repl, question)
            if v != question and 5 <= len(v) <= 100:
                variants.append(v)
            if len(variants) >= max_variants:
                break
    return variants


def build_samples_from_messages(messages):
    """Convert a ChatML message list to (text, assistant_start) sample.

    text = "<user> QUESTION <end><assistant> ANSWER <end>"
    Returns None if no user/assistant pair.
    """
    user_parts = []
    assist_parts = []
    for m in messages:
        role = m.get("role", "")
        content = str(m.get("content", "")).strip()
        if role == "user" and content:
            user_parts.append(content)
        elif role == "assistant" and content:
            assist_parts.append(content)
    if not user_parts or not assist_parts:
        return None
    question = "\n".join(user_parts).strip()
    answer = "\n".join(assist_parts).strip()
    if len(question) < MIN_INSTRUCTION_CHARS or len(answer) < MIN_RESPONSE_CHARS:
        return None
    return {
        "text": f"<user>{question}<end><assistant>{answer}<end>",
        "question": question,
        "answer": answer,
        "question_len": len(question),
        "answer_len": len(answer),
    }


def encode_sample(sample, tok=None):
    """Tokenize a sample text into ids + loss mask using the CharTokenizer."""
    if tok is None:
        from chinese.tokenizer import CharTokenizer
        tok = CharTokenizer.load(str(PROJECT_ROOT / "data_chinese" / "tokenizer.json"))
    tok_user, tok_assist, tok_end = tok.USER, tok.ASSIST, tok.END
    if tok_user < 0:
        print("  [error] tokenizer.json has no SFT markers; run migrate step")
        return None
    text = sample["text"]
    # Split at <assistant> to find the loss region
    marker = "<assistant>"
    marker_idx = text.find(marker)
    if marker_idx < 0:
        return None
    prefix = text[:marker_idx + len(marker)]
    answer_part = text[marker_idx + len(marker):]

    # Build id list manually to control special tokens
    def ids_of(segment):
        ids = []
        i = 0
        while i < len(segment):
            if segment.startswith("<user>", i):
                ids.append(tok_user); i += len("<user>")
            elif segment.startswith("<assistant>", i):
                ids.append(tok_assist); i += len("<assistant>")
            elif segment.startswith("<end>", i):
                ids.append(tok_end); i += len("<end>")
            else:
                ch = segment[i]
                ids.append(tok.stoi.get(ch, UNK))
                i += 1
        return ids

    prefix_ids = ids_of(prefix)
    answer_ids = ids_of(answer_part)

    input_ids = [BOS] + prefix_ids + answer_ids + [EOS]
    n_pref = len(prefix_ids) + 1  # + BOS
    # label[i] must equal input_ids[i+1] (predict the NEXT token).
    # Loss starts at n_pref-1 (the <assistant> marker predicts answer[0]).
    labels = [-100] * (n_pref - 1) + answer_ids + [EOS] + [-100]
    assert len(labels) == len(input_ids), f"{len(labels)} != {len(input_ids)}"
    return {"input_ids": input_ids, "labels": labels}


def ensure_tokenizer_sft():
    """Migrate tokenizer.json: ensure the 3 SFT markers are present.

    Old builds used '<|user|>' etc. which embed '|' and cause self-reinforcing
    pipe loops at inference. If legacy markers are found they are replaced by
    '<user>' / '<assistant>' / '<end>'. Because markers live at the END of the
    vocab, char ids / train.bin / val.bin stay unchanged — only the model head
    needs 3 extra rows at SFT time (handled in sft_train.py).
    """
    from chinese.tokenizer import CharTokenizer
    tok_path = PROJECT_ROOT / "data_chinese" / "tokenizer.json"
    if not tok_path.exists():
        print(f"  [error] tokenizer.json missing: {tok_path}")
        sys.exit(1)
    tok = CharTokenizer.load(str(tok_path))

    # Drop legacy pipe-markers if present
    legacy = ["<|user|>", "<|assistant|>", "<|end|>"]
    for m in legacy:
        if m in tok.stoi:
            del tok.stoi[m]
            print(f"  removed legacy marker: {m}")

    if all(tok.stoi.get(s, -1) < 0 for s in CharTokenizer.EXTRA_SPECIAL):
        for s in CharTokenizer.EXTRA_SPECIAL:
            tok.stoi[s] = len(tok.stoi)
        tok.itos = {v: k for k, v in tok.stoi.items()}
        tok.vocab_size = len(tok.stoi)
        tok._set_extra_ids()
        tok.save(str(tok_path))
        print(f"  migrated tokenizer: vocab {tok.vocab_size - 3} -> {tok.vocab_size} "
              f"(user={tok.USER}, assist={tok.ASSIST}, end={tok.END})")
    else:
        print(f"  tokenizer already has SFT markers (user={tok.USER}, "
              f"assist={tok.ASSIST}, end={tok.END}), vocab={tok.vocab_size}")
    return tok


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Build SFT dataset for Chinese ESP32 model")
    ap.add_argument("--skip-search-logs", action="store_true",
                    help="Only use WSL ChatML data, skip D:/docs/search_logs")
    ap.add_argument("--augment", action="store_true",
                    help="Add synonym-paraphrase variants of search_log questions")
    ap.add_argument("--no-templates", action="store_true",
                    help="Skip local template samples")
    ap.add_argument("--template-cap", type=int, default=300,
                    help="Max template samples to include (default 300)")
    ap.add_argument("--no-deepseek", action="store_true",
                    help="Skip DeepSeek-generated QA")
    ap.add_argument("--val-fraction", type=float, default=0.10)
    args = ap.parse_args()

    for d in (RAW_DIR, PROC_DIR, SPLIT_DIR):
        d.mkdir(parents=True, exist_ok=True)

    print("=== Phase 0.0: tokenizer SFT migration ===")
    ensure_tokenizer_sft()

    print("\n=== Phase 0.1: pull WSL ChatML ===")
    pull_wsl_chatml()

    print("\n=== Phase 0.2: search_logs ===")
    samples = []
    if not args.skip_search_logs:
        pairs = parse_search_logs()
        kept = clean_search_logs(pairs)
        for p in kept:
            s = build_samples_from_messages([
                {"role": "user", "content": p["instruction"]},
                {"role": "assistant", "content": p["response"]},
            ])
            if s:
                s["source"] = "search_logs"
                samples.append(s)
            # Synonym variants
            if args.augment:
                for v in augment_synonyms(p["instruction"]):
                    sv = build_samples_from_messages([
                        {"role": "user", "content": v},
                        {"role": "assistant", "content": p["response"]},
                    ])
                    if sv:
                        sv["source"] = "search_logs_aug"
                        samples.append(sv)
        # archive raw pairs
        with open(RAW_DIR / "search_logs_qa.json", "w", encoding="utf-8") as f:
            json.dump(kept, f, ensure_ascii=False, indent=2)
        print(f"  archived {len(kept)} cleaned search_log QA to raw/")

    print("\n=== Phase 0.3: WSL ChatML samples ===")
    for name, tag in [("med_instruction_chatml.json", "wsl_train"),
                      ("med_instruction_cot_chatml.json", "wsl_cot"),
                      ("med_instruction_val_chatml.json", "wsl_val")]:
        items = load_wsl_chatml(name)
        n = 0
        for item in items:
            s = build_samples_from_messages(item.get("messages", []))
            if s:
                s["source"] = tag
                samples.append(s)
                n += 1
        print(f"  {name}: {len(items)} items -> {n} samples")

    print("\n=== Phase 0.3b: local templates (Path C, down-weighted) ===")
    templates_path = PROC_DIR / "templates.json"
    if templates_path.exists() and not args.no_templates:
        tpl = json.load(open(templates_path, encoding="utf-8"))
        # Cap templates at 300 (deepseek QA is now the primary high-quality source)
        tpl = tpl[:args.template_cap]
        n = 0
        for t in tpl:
            s = build_samples_from_messages([
                {"role": "user", "content": t["question"]},
                {"role": "assistant", "content": t["answer"]},
            ])
            if s:
                s["source"] = "template"
                samples.append(s)
                n += 1
        print(f"  templates: {len(tpl)} -> {n} samples (capped {args.template_cap})")

    print("\n=== Phase 0.3c: DeepSeek generated QA (Path v2, primary) ===")
    deepseek_path = PROC_DIR / "deepseek_qa.jsonl"
    if deepseek_path.exists() and not args.no_deepseek:
        n = 0
        for line in deepseek_path.open(encoding="utf-8"):
            try:
                d = json.loads(line)
            except Exception:
                continue
            s = build_samples_from_messages([
                {"role": "user", "content": d["question"]},
                {"role": "assistant", "content": d["answer"]},
            ])
            if s:
                s["source"] = "deepseek"
                samples.append(s)
                n += 1
        print(f"  deepseek QA: {n} samples")

    print(f"\nTotal samples: {len(samples)}")
    print(f"  by source: ", end="")
    from collections import Counter
    for src, cnt in Counter(s["source"] for s in samples).most_common():
        print(f"{src}={cnt} ", end="")
    print()

    # Save text-level samples
    samples_path = PROC_DIR / "sft_samples.json"
    with open(samples_path, "w", encoding="utf-8") as f:
        json.dump(samples, f, ensure_ascii=False, indent=2)
    print(f"Saved text samples: {samples_path}")

    # Tokenize with loss masks
    print("\n=== Phase 0.4: tokenize + split ===")
    from chinese.tokenizer import CharTokenizer
    tok = CharTokenizer.load(str(PROJECT_ROOT / "data_chinese" / "tokenizer.json"))
    encoded = []
    failed = 0
    for s in samples:
        e = encode_sample(s, tok=tok)
        if e:
            e["source"] = s.get("source", "")
            encoded.append(e)
        else:
            failed += 1
    print(f"  encoded: {len(encoded)}, failed: {failed}")

    # Deterministic split
    import random
    rng = random.Random(42)
    rng.shuffle(encoded)
    n_val = max(1, int(len(encoded) * args.val_fraction))
    val = encoded[:n_val]
    train = encoded[n_val:]

    with open(PROC_DIR / "sft_train.json", "w", encoding="utf-8") as f:
        json.dump({"data": train, "format": "char-level",
                   "special_tokens": {"user": tok.USER, "assistant": tok.ASSIST,
                                      "end": tok.END}},
                  f, ensure_ascii=False)
    with open(PROC_DIR / "sft_val.json", "w", encoding="utf-8") as f:
        json.dump({"data": val, "format": "char-level"}, f, ensure_ascii=False)

    print(f"  train: {len(train)}, val: {len(val)}")

    # Anti-forgetting plain-text split (subset of pretraining corpus)
    corpus_path = PROJECT_ROOT / "data_chinese" / "corpus.txt"
    if corpus_path.exists():
        corpus = corpus_path.read_text(encoding="utf-8")
        n_chars = min(len(corpus), 5_000_000)  # 5M chars for mixed training
        with open(SPLIT_DIR / "train.txt", "w", encoding="utf-8") as f:
            f.write(corpus[:n_chars])
        print(f"  anti-forgetting train.txt: {n_chars:,} chars")

    # Sample verification
    print("\n=== Sample check (train[0]) ===")
    if train:
        s = train[0]
        print(f"  input_ids: {s['input_ids'][:20]}... (len {len(s['input_ids'])})")
        print(f"  labels:    {s['labels'][:20]}...")
        loss_pos = [i for i, v in enumerate(s["labels"]) if v != -100]
        print(f"  loss positions: {len(loss_pos)} (from {loss_pos[0] if loss_pos else '?'})")

    print("\n=== Phase 0 DONE ===")
    print(f"Archive: {RAW_DIR}")
    print(f"Processed: {PROC_DIR}")


if __name__ == "__main__":
    sys.exit(main())
