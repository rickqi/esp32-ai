/*
 * prompt_encoder.c — 设备端 UTF-8 → MiniMind BPE token ids 实现.
 *
 * 编码策略 (与 HF tokenizer 100% 一致):
 *   - 完整 ByteLevel BPE: 字节映射 → vocab 查初始 id → 贪心 merge
 *   - 表由 tools/export_bpe_tables.py 生成 (bpe_tables.h)
 *   - 替代旧版缺陷 enc_seg (限单字符最长匹配, 无法产生多字 BPE token)
 * ChatML 包装与 tools/send_prompt_rag.py build_chatml_ids 字节级一致.
 */
#include <string.h>
#include "prompt_encoder.h"
#include "bpe_encoder.h"

// 设备端 system prompt (与 PC 端字节级一致)
static const char PE_SYSTEM_PROMPT[] = "你是一个医学助手，请根据提供的参考资料准确回答问题。";

// 将一段文本编码为 ids (pe_encode 内部使用, 无 ChatML). 用真实 BPE.
static int enc_seg(const char *text, int *ids, int max_ids) {
  return bpe_encode(text, ids, max_ids);
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
