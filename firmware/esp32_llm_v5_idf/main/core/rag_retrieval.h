/*
 * rag_retrieval.h — device-side RAG retrieval (ESP-IDF V5).
 * V5: SD-only deep search (no flash kb partition).
 */
#ifndef RAG_RETRIEVAL_H
#define RAG_RETRIEVAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Init SD RAG index (reads /sdcard/rag/*.bin). Safe to call once.
void rag_retrieval_init(void);
bool rag_retrieval_ready(void);

// Number of docs in loaded SD RAG index (0 if not ready).
uint32_t rag_retrieval_doc_count(void);

// Retrieve top evidence for a question.  Fills out_ev (UTF-8, newline-joined).
// Returns chars written (0 = none).
int rag_retrieval_retrieve(const char *question, char *out_ev, int out_cap);

#ifdef __cplusplus
}
#endif

#endif
