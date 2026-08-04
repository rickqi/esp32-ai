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

// Load model.bin from 'model' partition (mmap'd). Returns 0 ok.
int llm_engine_load(void);

// Is model loaded?
bool llm_engine_ready(void);

// Generate text from prompt token ids.  Fills out[] with decoded UTF-8.
// Returns number of generated tokens.
int llm_engine_generate(const int *prompt_ids, int prompt_len,
                        char *out, int out_cap, int max_new);

// Low-level: forward one token (for incremental use / RAG).
void llm_engine_forward(int token, int pos);

// Sampling params.
extern float g_sampling_temp;
extern int   g_sampling_topk;
extern float g_repetition_penalty;

// Model accessor (for RAG / direct logits).
Model *llm_engine_model(void);
Scratch *llm_engine_scratch(void);

#endif
