/*
 * prompt_encoder.h — 设备端 UTF-8 文本 → MiniMind BPE token ids.
 *
 * MiniMind 是 ByteLevel BPE: vocab 含全部 256 单字节 token (已验证),
 * 因此任意 UTF-8 文本可无损编码。策略:
 *   1. 最长匹配: 在 VOCAB_BLOB 中找当前位置最长的 token (多字 token 优先)
 *   2. 字节回退: 无多字节匹配时用单字节 token (保证无损)
 *
 * 预设菜单走 presets.h (PC 端真实 BPE 烘焙), 本编码器用于 ASCII 自由输入.
 * ChatML 包装与 tools/send_prompt_rag.py 一致.
 */
#ifndef PROMPT_ENCODER_H
#define PROMPT_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PE_IM_START 1
#define PE_IM_END   2
#define PE_MAX_PROMPT 100   // 与 send_prompt_rag.py max_prompt 一致

// 最长匹配编码: UTF-8 文本 → token ids. 返回 token 数 (<=max_ids), 空输入返回 -1.
int pe_encode_utf8(const char *text, int *ids, int max_ids);

// ChatML 包装: system(带证据) + user(问题) + assistant 引导.
// 与 send_prompt_rag.py build_chatml_ids 一致 (无证据版本).
// 返回 token 数 (<=max_ids).
int pe_build_chatml(const char *question, int *ids, int max_ids);

// 便捷: 编码 + ChatML 包装一步完成 (键盘输入主入口).
// 返回 token 数, 或 -1.
int pe_prompt_from_text(const char *question, int *ids, int max_ids);

#ifdef __cplusplus
}
#endif

#endif
