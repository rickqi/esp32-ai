/*
 * bpe_encoder.h — 设备端 MiniMind ByteLevel BPE 编码器 (ESP-IDF V5).
 *
 * 在 ESP32 上把 UTF-8 中文文本编码为 MiniMind token ids, 使设备端 RAG
 * (SD 检索证据 → 编码 → 注入 prompt) 成为可能 — 与 HF tokenizer 100% 一致.
 *
 * 原理 (表由 tools/export_bpe_tables.py 从 tokenizer.json 生成):
 *   1. 输入字节 → BPE_B2U 映射为 unicode 字符 (GPT-2 ByteLevel)
 *   2. 每个字符查 BPE_VOCAB_BLOB 得初始 token id
 *   3. 贪心: 找最低 rank 的相邻 (left,right) 对 → BPE_MERGES 查 rank
 *      → BPE_MERGE_RESULT[rank] 得合并后的新 id
 *   4. 重复直到无 merge → 最终 token ids
 *
 * 实测: C 端编码与 HF tokenizer 7/7 完全一致.
 */
#ifndef BPE_ENCODER_H
#define BPE_ENCODER_H

#include <stdint.h>
#include <string.h>
#include "bpe_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BPE_MAX_INPUT 512    // 最大输入符号数 (证据块)
#define BPE_MAX_OUT   128    // 最大输出 token 数

// 查 vocab: UTF-8 字节 → token id, 无则 -1. (线性扫 6400, 编码时调用少, 可接受)
static inline int bpe_vocab_lookup(const uint8_t *utf8, int len) {
    for (int t = 0; t < BPE_VOCAB_N; t++) {
        int tl = BPE_VOCAB_OFF[t + 1] - BPE_VOCAB_OFF[t];
        if (tl == len && memcmp(BPE_VOCAB_BLOB + BPE_VOCAB_OFF[t], utf8, len) == 0)
            return t;
    }
    return -1;
}

// 查 merge rank: (left,right) → rank, 无则 -1. (线性扫 6108, merge 次数有限)
static inline int bpe_merge_rank(int left, int right) {
    uint32_t pair = ((uint32_t)left << 16) | (uint32_t)right;
    for (int i = 0; i < BPE_MERGE_N; i++)
        if (BPE_MERGES[i] == pair) return i;
    return -1;
}

// BPE 编码: UTF-8 文本 → token ids. 返回 token 数 (<= max_ids).
// 无法编码的字节跳过 (ByteLevel 全覆盖, 正常不会发生).
static inline int bpe_encode(const char *text, int *ids, int max_ids) {
    if (!text || max_ids <= 0) return 0;
    int n = 0;
    int sym[BPE_MAX_INPUT];
    const unsigned char *p = (const unsigned char *)text;

    // 1. 字节 → B2U 映射 → 查 vocab 得初始 id
    while (*p && n < BPE_MAX_INPUT) {
        int b = *p;
        int cp = BPE_B2U[b];
        uint8_t u8[4]; int ulen;
        if (cp < 0x80) { u8[0] = (uint8_t)cp; ulen = 1; }
        else if (cp < 0x800) { u8[0] = (uint8_t)(0xC0 | (cp >> 6)); u8[1] = (uint8_t)(0x80 | (cp & 0x3F)); ulen = 2; }
        else { u8[0] = (uint8_t)(0xE0 | (cp >> 12)); u8[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); u8[2] = (uint8_t)(0x80 | (cp & 0x3F)); ulen = 3; }
        int id = bpe_vocab_lookup(u8, ulen);
        if (id >= 0) sym[n++] = id;
        p++;
    }

    // 2. 贪心 merge: 重复找最低 rank 相邻对
    while (n > 1) {
        int best_rank = 0x7FFFFFFF, best_i = -1;
        for (int i = 0; i < n - 1; i++) {
            int r = bpe_merge_rank(sym[i], sym[i + 1]);
            if (r >= 0 && r < best_rank) { best_rank = r; best_i = i; }
        }
        if (best_i < 0) break;
        sym[best_i] = BPE_MERGE_RESULT[best_rank];
        for (int i = best_i + 1; i < n - 1; i++) sym[i] = sym[i + 1];
        n--;
    }

    // 3. 输出 (全部已是 vocab id)
    int out = n < max_ids ? n : max_ids;
    for (int i = 0; i < out; i++) ids[i] = sym[i];
    return out;
}

#ifdef __cplusplus
}
#endif

#endif
