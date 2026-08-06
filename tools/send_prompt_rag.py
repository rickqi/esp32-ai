#!/usr/bin/env python3
"""
RAG 串口发送器 (MiniMind H1/H2 + esp32-ai V5 固件)

PC 端完成: 证据检索 (jieba IDF) -> ChatML prompt 构建 (含证据) -> BPE 分词
         -> 串口发送 {"ids":[...],"max":N} -> 读取生成文本

固件侧 (esp32_llm_zh_v5): 直接推理 PC 发来的 ids, 回传生成文本.
设备端 RAG 已禁用 (MM_MINIMIND): 证据注入完全在 PC 端.

用法:
  python tools/send_prompt_rag.py COM3 "肺癌的早期症状有哪些"
  python tools/send_prompt_rag.py COM3 "高血压的诊断标准是什么" --max-tokens 80
  python tools/send_prompt_rag.py COM3 --interactive
"""

import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

import jieba
from tokenizers import Tokenizer

# 串口
try:
    import serial
except ImportError:
    sys.exit("pyserial not installed.\n  pip install pyserial")

PROJECT_ROOT = Path(__file__).resolve().parent.parent
MINIMIND_TOKENIZER = r"D:\codes\minimind\model\tokenizer.json"
KB_PATH = PROJECT_ROOT / "data_v4" / "kb" / "format_data.jsonl"
DOC_CHARS = 60
IDF_SCALE = 64.0
STOP_WORDS = set('的了是在和有就不都而及与或一个中其') | set('，。、；：！？""\'\'（）【】\n \t')


# ---------------------------------------------------------------------------
# 检索 (jieba IDF 倒排, 与 minimind rag_medical.py 一致)
# ---------------------------------------------------------------------------
def terms_of(text):
    return set(t for t in jieba.cut(text) if t.strip() and t not in STOP_WORDS and not t.isspace())


def load_kb(med_only=False):
    entries = []
    if not KB_PATH.exists():
        print(f"[warn] KB not found: {KB_PATH} — RAG 禁用")
        return []
    with open(KB_PATH, "r", encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            entries.append((d.get("question", ""), d.get("answer", ""), d.get("label", "")))
    if med_only:
        entries = [e for e in entries if is_medical_label(e[2])]
    return entries


def is_medical_label(label):
    """排除保险/健康管理域 (与 minimind rag_medical.py 对齐)."""
    NON_MEDICAL_KW = ['健康管理', '理赔', '产品条款', '销售', '消保']
    return not any(k in label for k in NON_MEDICAL_KW)


def build_index(entries):
    docs = [(a[:DOC_CHARS], label) for q, a, label in entries]
    inv = {}
    for di, (q, a, label) in enumerate(entries):
        for term in terms_of((q or "") + (a or "")[:DOC_CHARS]):
            inv.setdefault(term, []).append(di)
    N = len(docs)
    idf = {}
    for t, dl in inv.items():
        idf[t] = min(int(IDF_SCALE * math.log(1.0 + N / max(len(dl), 1))), 255)
    return docs, inv, idf


def retrieve(query, inv, idf, k=2):
    sc = {}
    for term in terms_of(query):
        if term not in idf:
            continue
        for did in inv.get(term, ()):
            sc[did] = sc.get(did, 0) + idf[term]
    return sorted(sc, key=sc.get, reverse=True)[:k]


# ---------------------------------------------------------------------------
# ChatML prompt 构建 (MiniMind BPE + 证据注入)
# ---------------------------------------------------------------------------
def build_chatml_ids(tok, question, evidence, max_prompt=100):
    """构建 ChatML ids: system(带证据) + user(问题) + assistant 引导.
    约束: 总长度 <= max_prompt (seq_len=128, 留空间给生成).
    截断策略: 证据优先被截断(保留整句前缀), 问题与 assistant 引导始终完整保留.
    修复(2026-08-05): 旧逻辑 ids[:keep]+ids[-4:] 会静默丢弃问题部分."""
    im_start, im_end = 1, 2

    def enc(text):
        return tok.encode(text, add_special_tokens=False).ids

    # 固定结构 (不含证据): system 头 + user 标记
    head = [im_start] + enc("system\n你是一个医学助手，请根据提供的参考资料准确回答问题。") + [im_end]
    head += enc("\n")
    head += [im_start] + enc("user\n")
    # 问题 + assistant 引导 (必须完整)
    q_part = enc("问题：" + question) + [im_end] + enc("\n")
    tail = [im_start] + enc("assistant\n")

    ids = head + q_part + tail
    if evidence:
        ev_prefix = enc("参考资料：\n")
        sep = enc("\n\n")  # 证据与问题之间的分隔
        # 预算 = max_prompt - 固定结构 - 证据标记 - 分隔符; 若预算不足则放弃证据保问题
        budget = max_prompt - len(head) - len(q_part) - len(tail) - len(ev_prefix) - len(sep)
        if budget > 0:
            ev_ids = enc(evidence)
            # 截断到 UTF-8 边界 (避免多字节字符被拦腰切断)
            while budget > 0 and budget < len(ev_ids):
                cut = tok.decode(ev_ids[:budget], skip_special_tokens=False)
                if "\ufffd" not in cut:
                    break
                budget -= 1
            ev_trunc = ev_ids[:budget]
            ids = head + ev_prefix + ev_trunc + sep + q_part + tail
    return ids


# ---------------------------------------------------------------------------
# 串口收发
# ---------------------------------------------------------------------------
def send_and_read(ser, ids, max_tokens, quiet=False):
    payload = {"ids": ids}
    if max_tokens:
        payload["max"] = max_tokens
    ser.write((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))
    ser.flush()

    out_lines = []
    t0 = time.time()
    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace")
        ls = line.strip()
        if ls.startswith("{"):
            try:
                msg = json.loads(ls)
            except json.JSONDecodeError:
                sys.stdout.write(line)
                sys.stdout.flush()
                continue
            if msg.get("done"):
                out_lines.append(f"\n[完成] {msg.get('tok/s', 0):.2f} tok/s ({time.time()-t0:.1f}s)")
                break
            out_lines.append(ls)
            continue
        if ls.startswith("--- ") or ls.startswith("throughput"):
            continue  # 统计行
        out_lines.append(line)
    return "\n".join(out_lines)


def main():
    ap = argparse.ArgumentParser(description="RAG 串口发送器 (MiniMind H1/H2 + V5 固件)")
    ap.add_argument("port", help="串口 (COM3 / /dev/ttyUSB0)")
    ap.add_argument("question", nargs="?", default=None, help="问题 (单次模式)")
    ap.add_argument("--tokenizer", default=MINIMIND_TOKENIZER)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--max-tokens", type=int, default=60, help="生成 token 数 (seq_len=128, 留空间)")
    ap.add_argument("--no-rag", action="store_true", help="禁用 RAG (不注入证据)")
    ap.add_argument("--interactive", action="store_true", help="交互模式")
    args = ap.parse_args()

    # tokenizer
    tok = Tokenizer.from_file(args.tokenizer)
    print(f"Tokenizer: {args.tokenizer} (vocab {tok.get_vocab_size()})")

    # 检索索引 (惰性) — 医学词典 + med_only 过滤 (与 minimind rag_medical.py 对齐)
    med_dict = r"D:\codes\minimind\out\medical_jieba.txt"
    if os.path.exists(med_dict):
        try:
            jieba.load_userdict(med_dict)
            print(f"[jieba] 加载医学词典: {med_dict}")
        except Exception as exc:
            print(f"[jieba] 词典加载失败: {exc}")
    entries = load_kb(med_only=True)
    if entries and not args.no_rag:
        docs, inv, idf = build_index(entries)
        print(f"KB: {len(docs)} docs, {len(idf)} terms (RAG 启用, 医学过滤)")
    else:
        docs, inv, idf = None, None, None

    # 串口
    try:
        ser = serial.Serial(args.port, args.baud, timeout=10)
    except serial.SerialException as exc:
        sys.exit(f"Serial error: {exc}")
    ser.reset_input_buffer()
    print(f"Serial: {args.port} @ {args.baud}")

    # 等待设备就绪
    print("等待设备启动...")
    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if '"ready"' in line:
            break
    print("设备就绪.\n")

    questions = [args.question] if args.question else []
    if args.interactive or not questions:
        print("输入问题 (空行退出):")
        while True:
            try:
                q = input("RAG> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not q:
                break
            questions.append(q)

    for q in questions:
        evidence = ""
        if inv is not None:
            top = retrieve(q, inv, idf, k=2)
            if top:
                evidence = "\n".join(docs[d][0] for d in top)
                print(f"[RAG] 检索到 {len(top)} 条证据")
        ids = build_chatml_ids(tok, q, evidence)
        print(f"[提示词] {len(ids)} tokens (证据 {len(evidence)} 字符)")
        result = send_and_read(ser, ids, args.max_tokens)
        print(f"\n[回答]\n{result}\n{'='*50}")

    ser.close()


if __name__ == "__main__":
    main()
