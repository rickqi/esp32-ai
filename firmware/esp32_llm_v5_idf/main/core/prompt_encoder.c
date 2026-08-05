/*
 * prompt_encoder.c — 设备端 UTF-8 → MiniMind BPE token ids 实现.
 *
 * 编码策略:
 *   - 最长匹配: 当前位置在 VOCAB_BLOB 中找最长匹配 token (UTF-8 长度内)
 *   - 字节回退: 无匹配时用单字节 token (ByteLevel BPE 保证 256 全覆盖)
 * ChatML 包装与 tools/send_prompt_rag.py build_chatml_ids 字节级一致.
 */
#include <string.h>
#include "prompt_encoder.h"
#include "vocab.h"

// 设备端 system prompt (与 PC 端字节级一致)
static const char PE_SYSTEM_PROMPT[] = "你是一个医学助手，请根据提供的参考资料准确回答问题。";

// 将一段文本编码为 ids (pe_encode 内部使用, 无 ChatML).
static int enc_seg(const char *text, int *ids, int max_ids) {
  const unsigned char *p = (const unsigned char *)text;
  int n = 0;
  while (*p && n < max_ids) {
    unsigned char b0 = p[0];
    // 当前 UTF-8 字符的字节长度
    int clen = (b0 < 0x80) ? 1 : ((b0 & 0xE0) == 0xC0) ? 2 :
               ((b0 & 0xF0) == 0xE0) ? 3 : 1;
    // 1. 最长匹配: 扫描 vocab 找最长 token (长度 <= clen, 字节相等)
    int best = -1, best_len = 0;
    for (int t = 4; t < VOCAB_N; t++) {   // 跳过特殊 token 0-3
      int tl = VOCAB_OFF[t + 1] - VOCAB_OFF[t];
      if (tl > best_len && tl <= clen &&
          memcmp(VOCAB_BLOB + VOCAB_OFF[t], p, tl) == 0) {
        best = t;
        best_len = tl;
      }
    }
    if (best >= 0 && best_len > 0) {
      ids[n++] = best;
      p += best_len;
    } else {
      // 2. 字节回退: 单字节 token (ByteLevel 保证存在)
      for (int t = 4; t < VOCAB_N; t++) {
        int tl = VOCAB_OFF[t + 1] - VOCAB_OFF[t];
        if (tl == 1 && VOCAB_BLOB[VOCAB_OFF[t]] == b0) {
          ids[n++] = t;
          break;
        }
      }
      p += 1;   // 即使没找到也推进 (避免死循环)
    }
  }
  return n;
}

int pe_encode_utf8(const char *text, int *ids, int max_ids) {
  if (!text || !*text || max_ids <= 0) return -1;
  return enc_seg(text, ids, max_ids);
}

int pe_build_chatml(const char *question, int *ids, int max_ids) {
  if (!question || !*question || max_ids < 8) return -1;

  int n = 0;
  // <im_start>system\n{system}<im_end>\n
  ids[n++] = PE_IM_START;
  n += enc_seg("system\n", ids + n, max_ids - n);
  n += enc_seg(PE_SYSTEM_PROMPT, ids + n, max_ids - n);
  ids[n++] = PE_IM_END;
  n += enc_seg("\n", ids + n, max_ids - n);

  // <im_start>user\n问题：{q}<im_end>\n
  ids[n++] = PE_IM_START;
  n += enc_seg("user\n", ids + n, max_ids - n);
  n += enc_seg("问题：", ids + n, max_ids - n);
  n += enc_seg(question, ids + n, max_ids - n);
  ids[n++] = PE_IM_END;
  n += enc_seg("\n", ids + n, max_ids - n);

  // <im_start>assistant\n
  ids[n++] = PE_IM_START;
  n += enc_seg("assistant\n", ids + n, max_ids - n);

  // 超长截断: 保留 assistant 引导 (与 PC 一致)
  if (n > PE_MAX_PROMPT && PE_MAX_PROMPT >= 4) {
    int keep = PE_MAX_PROMPT - 4;
    memmove(ids + keep, ids + n - 4, 4 * sizeof(int));
    n = PE_MAX_PROMPT;
  }
  return n;
}

int pe_prompt_from_text(const char *question, int *ids, int max_ids) {
  return pe_build_chatml(question, ids, max_ids);
}
