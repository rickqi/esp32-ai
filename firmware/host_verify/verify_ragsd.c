/* verify_ragsd.c — host-side check of rag_sd.h retrieval against Python ref.
 *
 * Compile & run (WSL):
 *   gcc -O2 -o /tmp/verify_ragsd firmware/host_verify/verify_ragsd.c
 *   /tmp/verify_ragsd data_v4/sd_rag
 *
 * Loads the SD index (index.bin/docs.bin/meta.bin), runs queries, prints
 * top-1 doc text.  Cross-check with build_sd_index.py --verify-only output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* stub heap_caps_malloc for host build */
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }

/* stub esp_heap_caps.h (host build) */
#ifndef ESP_HEAP_CAPS_H
#define ESP_HEAP_CAPS_H
#include <stdlib.h>
#endif

/* rag_sd.h uses open/read/lseek from unistd — fine on Linux host. */
#include "rag_sd.h"

static void run_query(const char *q) {
  uint32_t best[RAGSD_TOP_K];
  int scores[RAGSD_TOP_K];
  int n = ragsd_retrieve(q, best, scores, RAGSD_TOP_K);
  printf("Q: %s\n", q);
  for (int i = 0; i < n; i++) {
    char doc[RAGSD_DOC_CAP + 1];
    int clen = ragsd_read_doc(best[i], doc, sizeof(doc));
    printf("  [%3d] doc=%u: %.*s\n", scores[i], best[i], clen, doc);
  }
  if (n == 0) printf("  (no match)\n");
  printf("\n");
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <sd_rag_dir>\n", argv[0]); return 2; }
  /* rag_sd.h hardcodes /sdcard/rag/* — for the host test we create that
     path as a symlink farm to the real dir. */
  char cmd[512];
  snprintf(cmd, sizeof(cmd),
      "mkdir -p /sdcard/rag && "
      "ln -sf %s/index.bin /sdcard/rag/index.bin && "
      "ln -sf %s/docs.bin /sdcard/rag/docs.bin && "
      "ln -sf %s/meta.bin /sdcard/rag/meta.bin",
      argv[1], argv[1], argv[1]);
  int rc2 = system(cmd);
  if (rc2 != 0) { fprintf(stderr, "symlink setup failed\n"); return 3; }

  int rc = ragsd_init();
  if (rc != 0) {
    fprintf(stderr, "ragsd_init failed: %d\n", rc);
    return 1;
  }
  printf("ragsd ready: %u docs, %u terms\n\n", g_rag_sd.n_docs, g_rag_sd.n_terms);

  run_query("感冒发烧");
  run_query("肺癌早期症状");
  run_query("急性重型肝炎");
  run_query("糖尿病酮症酸中毒");
  run_query("白疕皮损特点");
  run_query("宫外孕如何治疗");
  return 0;
}
