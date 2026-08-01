/*
 * rag.h — device-side TF-IDF retrieval over the compressed RAG index.
 *
 * Index format (built by chinese/kb/build_index.py):
 *   [4B magic "RAG1"] [n_docs u32] [n_terms u32] [vocab u32]
 *   [doc_off: (n_docs+1) u32]
 *   [doc_ids: flat uint16 char-ids (len = doc_off[n_docs])]
 *   [inverted: per term: char_id u32 | count u16 | doc_ids u16...]
 *
 * The whole index lives in PSRAM (mmap'd from a data flash partition or
 * loaded at boot). Retrieval: query chars -> inverted lookup -> score docs.
 */

#ifndef RAG_H
#define RAG_H

#include <stdint.h>
#include <string.h>

typedef struct {
  const uint8_t *base;
  uint32_t n_docs;
  uint32_t n_terms;
  uint32_t vocab_size;
  const uint32_t *doc_off;     // n_docs+1 offsets
  const uint16_t *doc_ids;     // flat char ids
  // inverted parsed lazily on load
} RagIndex;

#define RAG_MAGIC 0x31474152u  // "RAG1"
#define RAG_TOP_K 3
#define RAG_MAX_Q 128

// Load index from a byte buffer (PSRAM). Returns 0 on success.
static int rag_load(RagIndex *r, const uint8_t *buf) {
  uint32_t magic;
  memcpy(&magic, buf, 4);
  if (magic != RAG_MAGIC) return -1;
  memcpy(&r->n_docs, buf + 4, 4);
  memcpy(&r->n_terms, buf + 8, 4);
  memcpy(&r->vocab_size, buf + 12, 4);
  r->base = buf;
  r->doc_off = (const uint32_t *)(buf + 16);
  const uint8_t *p = buf + 16 + (r->n_docs + 1) * 4;
  r->doc_ids = (const uint16_t *)p;
  return 0;
}

// Decode one doc into a caller buffer (char ids -> utf8 via caller's table).
// Returns char count (not bytes).
static uint32_t rag_doc_chars(const RagIndex *r, uint32_t di,
                              uint16_t *out, uint32_t max_out) {
  uint32_t a = r->doc_off[di], b = r->doc_off[di + 1];
  uint32_t n = b - a;
  if (n > max_out) n = max_out;
  for (uint32_t i = 0; i < n; i++) out[i] = r->doc_ids[a + i];
  return n;
}

// Retrieve top-K docs for a query (char ids). Scores = matched unique chars.
// Returns number of results; fills best_docs[] and best_scores[].
static int rag_retrieve(const RagIndex *r, const uint16_t *q_ids, int q_len,
                        uint32_t *best_docs, int *best_scores) {
  // Linear scan scoring is fine for ~10K docs on ESP32 (few ms).
  int scores[RAG_TOP_K] = {0, 0, 0};
  uint32_t best[RAG_TOP_K] = {0, 0, 0};
  uint8_t seen[4096] = {0};  // dedupe chars per doc (vocab <= 4096 typical)

  for (uint32_t di = 0; di < r->n_docs; di++) {
    uint32_t a = r->doc_off[di], b = r->doc_off[di + 1];
    int score = 0;
    for (uint32_t i = a; i < b; i++) {
      uint16_t cid = r->doc_ids[i];
      // count query chars present in doc (unique)
      for (int q = 0; q < q_len; q++) {
        if (q_ids[q] == cid) { score++; break; }
      }
    }
    if (score == 0) continue;
    // insert into top-K
    for (int k = 0; k < RAG_TOP_K; k++) {
      if (score > scores[k]) {
        for (int j = RAG_TOP_K - 1; j > k; j--) {
          scores[j] = scores[j - 1]; best[j] = best[j - 1];
        }
        scores[k] = score; best[k] = di;
        break;
      }
    }
  }
  int n = 0;
  for (int k = 0; k < RAG_TOP_K; k++) {
    if (scores[k] > 0) { best_docs[n] = best[k]; best_scores[n] = scores[k]; n++; }
  }
  return n;
}

#endif
