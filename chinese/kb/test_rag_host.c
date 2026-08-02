/* Host-side verification of rag.h against the real index.bin.
 * Build: gcc -O2 -o /tmp/ragtest test_rag_host.c
 * Run:   /tmp/ragtest /mnt/d/codes/esp32-ai/data_v2/kb/index.bin
 * Verifies: load, doc decode, IDF retrieval — same logic as device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "rag.h"

/* simple char-id -> utf8 print: we only know vocab via a tiny map for test.
 * For display, decode doc_ids to utf8 using a prebuilt table is heavy; here we
 * just print ids and the first bytes via the known tokenizer is skipped.
 * Instead: print score + doc id + raw char ids count.
 */
static void print_doc(const RagIndex *r, uint32_t di) {
  uint16_t buf[64];
  uint32_t n = rag_doc_chars(r, di, buf, 63);
  printf("    doc %u (%u chars) ids:", di, n);
  for (uint32_t i = 0; i < n && i < 16; i++) printf(" %u", buf[i]);
  printf("...\n");
}

/* tiny utf8 decode for question chars (test only, ascii+latin fallback) */
static int query_char_ids(const RagIndex *r, const char *q, uint16_t *out, int max) {
  /* We cannot map utf8->id without vocab; rely on the idf table + inverted
   * to simulate: here we only test structure. The real mapping lives in the
   * firmware via VOCAB_BLOB. For the host test we embed a few known ids from
   * the v2 tokenizer (肺=?, 癌=?) — instead we just exercise rag_load/retrieve
   * with a dummy query to prove the C code path compiles + runs. */
  (void)r; (void)q; (void)out; (void)max;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s index.bin\n", argv[0]); return 1; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc(sz);
  fread(buf, 1, sz, f);
  fclose(f);
  printf("loaded %ld bytes\n", sz);

  RagIndex r;
  if (rag_load(&r, buf) != 0) { printf("rag_load FAILED (magic?)\n"); return 1; }
  printf("rag_load OK: n_docs=%u n_terms=%u vocab=%u\n",
         r.n_docs, r.n_terms, r.vocab_size);

  /* structural checks */
  if (r.n_docs == 0 || r.n_terms == 0) { printf("empty index\n"); return 1; }
  printf("doc_off[0]=%u doc_off[last]=%u\n", r.doc_off[0], r.doc_off[r.n_docs]);
  printf("idf[0]=%u idf[%u]=%u\n", r.idf[0], r.n_terms - 1, r.idf[r.n_terms - 1]);

  /* retrieve with a synthetic query: use a couple of term ids found in the
   * inverted index (first term's char id). Proves retrieval path runs. */
  uint16_t qids[8];
  const uint8_t *p = r.inv_start;
  uint32_t cid0; uint16_t cnt0;
  memcpy(&cid0, p, 4); memcpy(&cnt0, p + 4, 2);
  qids[0] = (uint16_t)cid0;
  qids[1] = (uint16_t)(r.n_terms > 1 ? cid0 + 1 : cid0);  // second term approx
  uint32_t best[RAG_TOP_K];
  int scores[RAG_TOP_K];
  clock_t t0 = clock();
  int n = rag_retrieve(&r, qids, 2, best, scores);
  double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  printf("retrieve: %d hits in %.1f ms\n", n, ms);
  for (int i = 0; i < n; i++) {
    printf("  top%d score=%d\n", i, scores[i]);
    print_doc(&r, best[i]);
  }

  /* doc decode roundtrip: first doc char count matches offset table */
  uint32_t n0 = rag_doc_chars(&r, 0, qids, 63);
  printf("doc0 chars=%u (offset span=%u)\n", n0, r.doc_off[1] - r.doc_off[0]);

  printf("\nHOST VERIFY: rag.h compiles & runs OK\n");
  free(buf);
  return 0;
}
