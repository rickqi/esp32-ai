#!/usr/bin/env python3
"""
export_bpe_tables.py — 从 MiniMind tokenizer.json 导出设备端 BPE 编码表.

输出: firmware/esp32_llm_v5_idf/main/core/bpe_tables.h
  1. BPE_VOCAB_KEYS: vocab 键的 UTF-8 字节 (查词表用)
  2. BPE_VOCAB_OFF:  键偏移 (同 VOCAB_OFF 模式)
  3. BPE_MERGES:     (left_id:16, right_id:16) 按 rank 排序, rank = 数组下标
  4. BPE_B2U:        byte_to_unicode 映射 (byte → vocab 键的 unicode 字符)

设备端编码:
  text 字节 → b2u 映射为 unicode 字符序列
  → 每个字符查 vocab (unicode 键) 得初始 token id
  → 贪心: 找最低 rank 的相邻 (left_id, right_id) merge
  → 合并后查 vocab 得新 id
"""
import json, struct, sys, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

TOK_PATH = '/mnt/d/codes/minimind/model/tokenizer.json'
OUT_PATH = '/mnt/d/codes/esp32-ai/firmware/esp32_llm_v5_idf/main/core/bpe_tables.h'

tok = json.load(open(TOK_PATH))
m = tok['model']
vocab = m['vocab']
merges = m['merges']

# 1. vocab: unicode 键 → id
#    需要: 键的 UTF-8 字节 (BPE_VOCAB_KEYS) + 偏移 (BPE_VOCAB_OFF)
key_bytes = b''
offs = []
for i in range(len(vocab)):
    # vocab 键顺序: dict 插入序 = id 顺序? 验证
    pass

# 按 id 排序重建 (vocab dict 可能不按 id 序)
id_to_key = {}
for k, v in vocab.items():
    id_to_key[v] = k

blob = bytearray()
offs = [0]
max_id = max(id_to_key.keys())
for i in range(max_id + 1):
    k = id_to_key.get(i, '')
    kb = k.encode('utf-8')
    blob += kb
    offs.append(offs[-1] + len(kb))

print(f'vocab: {max_id+1} ids, blob {len(blob)} bytes')

# 2. byte_to_unicode (GPT-2 标准)
def bytes_to_unicode():
    bs = list(range(ord('!'), ord('~')+1)) + list(range(ord('\xa1'), ord('\xac')+1)) + list(range(ord('\xae'), ord('\xff')+1))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256+n)
            n += 1
    return dict(zip(bs, [chr(c) for c in cs]))

b2u = bytes_to_unicode()

# 3. merges: 按 rank (下标) 排序, 每项 (left_id, right_id)
#    注意: merge 符号是 unicode 键, 查 vocab id
#    merge 结果 id: left符号+right符号 拼接后查 vocab (BPE 保证存在)
merge_pairs = []
merge_result_ids = []
for rank, pair in enumerate(merges):
    l, r = pair
    if l not in vocab or r not in vocab:
        print(f'  [warn] merge {rank}: {l!r}/{r!r} 不在 vocab, 跳过')
        continue
    merged = l + r
    mid = vocab.get(merged, -1)
    if mid < 0:
        print(f'  [warn] merge {rank}: 结果 {merged!r} 不在 vocab, 跳过')
        continue
    merge_pairs.append((vocab[l], vocab[r]))
    merge_result_ids.append(mid)
print(f'merges: {len(merge_pairs)} valid pairs')

# 验证: merge 的 left/right 是否都作为独立 token 存在于 vocab
# (BPE 训练保证: 每个 merge 的输入是之前的 merge 结果或初始字符, 都在 vocab)
missing = 0
for pair in merges:
    for s in pair:
        if s not in vocab:
            missing += 1
print(f'merge 输入符号不在 vocab 的: {missing}')
# 验证 merge 结果 id 是否唯一 (一个 id 只能由一个 merge 产生?)
from collections import Counter
cnt = Counter(merge_result_ids)
dups = [x for x, c in cnt.items() if c > 1]
print(f'merge 结果 id 重复: {len(dups)} 个 (应 0)')

# 4. 生成 C 头文件
lines = []
lines.append('/*')
lines.append(' * bpe_tables.h — MiniMind ByteLevel BPE 编码表 (由 tools/export_bpe_tables.py 生成, 勿手改)')
lines.append(' *')
lines.append(f' * vocab: {max_id+1} tokens | merges: {len(merge_pairs)} | 生成时间见 git')
lines.append(' * 结构: BPE_VOCAB_KEYS/OFF (查词表) + BPE_MERGES (rank 排序) + BPE_B2U (字节映射)')
lines.append(' */')
lines.append('#ifndef BPE_TABLES_H')
lines.append('#define BPE_TABLES_H')
lines.append('#include <stdint.h>')
lines.append('')
lines.append(f'#define BPE_VOCAB_N {max_id+1}')
lines.append(f'#define BPE_MERGE_N {len(merge_pairs)}')
lines.append('')
lines.append(f'static const uint8_t BPE_VOCAB_BLOB[{len(blob)}] = {{')
for i in range(0, len(blob), 16):
    chunk = blob[i:i+16]
    lines.append('  ' + ','.join(str(b) for b in chunk) + ',')
lines.append('};')
lines.append('')
lines.append(f'static const uint32_t BPE_VOCAB_OFF[{len(offs)}] = {{')
for i in range(0, len(offs), 8):
    chunk = offs[i:i+8]
    lines.append('  ' + ','.join(str(o) for o in chunk) + ',')
lines.append('};')
lines.append('')
lines.append(f'static const uint32_t BPE_MERGES[{len(merge_pairs)}] = {{')
for i in range(0, len(merge_pairs), 8):
    chunk = merge_pairs[i:i+8]
    lines.append('  ' + ','.join(f'(({l}<<16)|{r})' for l, r in chunk) + ',')
lines.append('};')
lines.append('')
lines.append(f'static const uint16_t BPE_MERGE_RESULT[{len(merge_result_ids)}] = {{')
for i in range(0, len(merge_result_ids), 16):
    chunk = merge_result_ids[i:i+16]
    lines.append('  ' + ','.join(str(x) for x in chunk) + ',')
lines.append('};')
lines.append('')
lines.append('static const uint16_t BPE_B2U[256] = {')
for i in range(0, 256, 8):
    chunk = [ord(b2u[b]) for b in range(i, min(i+8, 256))]
    lines.append('  ' + ','.join(str(c) for c in chunk) + ',')
lines.append('};')
lines.append('')
lines.append('#endif')
lines.append('')

os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
with open(OUT_PATH, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))
print(f'写入 {OUT_PATH}')

# 5. 自检: 表大小
import os
sz = os.path.getsize(OUT_PATH)
print(f'头文件大小: {sz/1024:.1f} KB')
print(f'  BPE_VOCAB_BLOB: {len(blob)/1024:.1f} KB')
print(f'  BPE_MERGES: {len(merge_pairs)*4/1024:.1f} KB')
print(f'  BPE_B2U: {256*2/1024:.1f} KB')
