/*
 * rag_retrieval.c — device-side RAG retrieval (ESP-IDF V5).
 *
 * V5 has no flash kb partition (model uses all space).  RAG is SD-only
 * deep search via rag_sd.h.  This file wraps rag_sd.h for the engine.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "rag_retrieval.h"
#include "rag_sd.h"

static const char *TAG = "rag";

static bool g_rag_sd_ready = false;

void rag_retrieval_init(void) {
    if (ragsd_init() == 0) {
        g_rag_sd_ready = true;
        ESP_LOGI(TAG, "SD RAG index ready: %u docs, %u terms",
                 g_rag_sd.n_docs, g_rag_sd.n_terms);
    } else {
        ESP_LOGW(TAG, "SD RAG index not available (no /sdcard/rag/*.bin)");
    }
}

bool rag_retrieval_ready(void) { return g_rag_sd_ready; }

int rag_retrieval_retrieve(const char *question, char *out_ev, int out_cap) {
    if (!g_rag_sd_ready || !question || !out_ev) return 0;
    uint32_t best[RAGSD_TOP_K];
    int scores[RAGSD_TOP_K];
    int n = ragsd_retrieve(question, best, scores, 2);
    if (n <= 0) return 0;
    // concatenate top evidence docs
    int ob = 0;
    for (int k = 0; k < n && ob < out_cap - 1; k++) {
        char doc[RAGSD_DOC_CAP + 1];
        int clen = ragsd_read_doc(best[k], doc, sizeof(doc));
        if (clen > 0) {
            if (ob > 0 && ob < out_cap - 1) out_ev[ob++] = '\n';
            for (int i = 0; i < clen && ob < out_cap - 1; i++)
                out_ev[ob++] = doc[i];
        }
    }
    out_ev[ob] = 0;
    return ob;
}
