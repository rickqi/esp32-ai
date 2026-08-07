/*
 * llm_engine.h — V5 LLM inference engine (ESP-IDF).
 *
 * Wraps llm_v5.h (pure-C, framework-independent) + model loading from the
 * 'model' flash partition.  Exposes generation with sampling.
 */
#ifndef LLM_ENGINE_H
#define LLM_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "llm_v5.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load model.bin from 'model' partition (mmap'd). Returns 0 ok.
int llm_engine_load(void);

// Is model loaded?
bool llm_engine_ready(void);

// Generate text from prompt token ids.  Fills out[] with decoded UTF-8.
// Returns number of generated tokens.
int llm_engine_generate(const int *prompt_ids, int prompt_len,
                        char *out, int out_cap, int max_new);

// 流式生成回调: 每生成一个 token 调用 on_token(utf8 字节, len, ctx).
typedef void (*llm_token_cb_t)(const char *utf8, int len, void *ctx);

// 流式生成: prefill 后逐 token 生成并回调 (供 UI 实时显示).
// 返回生成 token 数. on_token 为 NULL 时退化为 llm_engine_generate 行为.
int llm_engine_generate_stream(const int *prompt_ids, int prompt_len,
                               llm_token_cb_t on_token, void *ctx,
                               int max_new);

// Low-level: forward one token (for incremental use / RAG).
void llm_engine_forward(int token, int pos);

// Sampling params.
extern float g_sampling_temp;
extern int   g_sampling_topk;
extern float g_repetition_penalty;

// Model accessor (for RAG / direct logits).
Model *llm_engine_model(void);
Scratch *llm_engine_scratch(void);
// 输出头量化位数 (4/8) — footer 模型名显示
int llm_engine_head_bits(void);

#ifdef __cplusplus
}
#endif

#endif
