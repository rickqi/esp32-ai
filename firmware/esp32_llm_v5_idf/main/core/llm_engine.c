/*
 * llm_engine.c — V5 LLM inference engine (ESP-IDF).
 *
 * Loads model.bin from the 'model' flash partition (mmap), runs llm_v5.h
 * forward, samples tokens (temp/top-k/repetition penalty), decodes via
 * VOCAB_BLOB to UTF-8.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "llm_engine.h"
#include "vocab.h"

static const char *TAG = "llm_engine";

static Model g_model;
static Scratch g_scratch;
static bool g_ready = false;
static const uint8_t *g_model_base = NULL;

float g_sampling_temp = 0.8f;
int   g_sampling_topk = 40;
float g_repetition_penalty = 1.3f;

Model *llm_engine_model(void) { return &g_model; }
Scratch *llm_engine_scratch(void) { return &g_scratch; }
bool llm_engine_ready(void) { return g_ready; }

// simple PRNG (matches Arduino xrng)
static uint32_t rng_state = 42;
static uint32_t xrng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// temperature + top-k + repetition penalty sampling
static int sample_token(float *logits, int n, float temp, int topk,
                        const int *hist, int hist_n, float rep) {
    for (int i = 0; i < n; i++) logits[i] /= temp;
    if (hist_n > 0 && hist) {
        for (int h = 0; h < hist_n; h++) {
            int t = hist[h];
            if (t >= 0 && t < n) logits[t] /= rep;
        }
    }
    if (topk > 0 && topk < n) {
        float tb[64];
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            if (cnt < topk) { tb[cnt++] = logits[i]; continue; }
            int mi = 0; for (int j = 1; j < topk; j++) if (tb[j] < tb[mi]) mi = j;
            if (logits[i] > tb[mi]) tb[mi] = logits[i];
        }
        float thr = 1e30f;
        for (int j = 0; j < topk; j++) if (tb[j] < thr) thr = tb[j];
        for (int i = 0; i < n; i++) if (logits[i] < thr) logits[i] = -1e30f;
    }
    float mx = -1e30f, sum = 0;
    for (int i = 0; i < n; i++) if (logits[i] > mx) mx = logits[i];
    for (int i = 0; i < n; i++) { logits[i] = expf(logits[i] - mx); sum += logits[i]; }
    float r = (float)(xrng() % 100000) / 100000.0f * sum;
    float c = 0;
    for (int i = 0; i < n; i++) { c += logits[i]; if (c > r) return i; }
    for (int i = n - 1; i >= 0; i--) if (logits[i] > -1e29f) return i;
    return 0;
}

// decode token id -> UTF-8 bytes via VOCAB_BLOB
static int decode_token(int id, char *buf, int cap) {
    if (id < 0 || id >= VOCAB_N) return 0;
    int a = VOCAB_OFF[id], b = VOCAB_OFF[id + 1];
    int n = b - a;
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, VOCAB_BLOB + a, n);
    buf[n] = 0;
    return n;
}

int llm_engine_load(void) {
    const esp_partition_t *model = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");
    if (!model) { ESP_LOGE(TAG, "model partition not found"); return -1; }
    const void *base;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(model, 0, model->size, ESP_PARTITION_MMAP_DATA,
                           &base, &h) != ESP_OK) {
        ESP_LOGE(TAG, "model mmap failed"); return -2;
    }
    if (llm_load((const uint8_t *)base, &g_model) != 0) {
        ESP_LOGE(TAG, "bad model magic"); return -3;
    }
    g_model_base = (const uint8_t *)base;

    // allocate scratch in PSRAM
    int D = g_model.c.dim, L = g_model.c.n_layers, P = g_model.c.ple_dim;
    int F = g_model.c.ffn, V = g_model.c.vocab, S = g_model.c.seq_len;
    g_scratch.x     = heap_caps_malloc(D * 4, MALLOC_CAP_SPIRAM);
    g_scratch.h     = heap_caps_malloc((F > D ? F : D) * 4, MALLOC_CAP_SPIRAM);
    g_scratch.qkv   = heap_caps_malloc(3 * D * 4, MALLOC_CAP_SPIRAM);
    g_scratch.att   = heap_caps_malloc(D * 4, MALLOC_CAP_SPIRAM);
    g_scratch.g1    = heap_caps_malloc(F * 4, MALLOC_CAP_SPIRAM);
    g_scratch.g2    = heap_caps_malloc((P > F ? P : F) * 4, MALLOC_CAP_SPIRAM);
    g_scratch.ple   = heap_caps_malloc(L * P * 4, MALLOC_CAP_SPIRAM);
    g_scratch.tmpP  = heap_caps_malloc(L * P * 4, MALLOC_CAP_SPIRAM);
    g_scratch.trow  = heap_caps_malloc(L * P * 4, MALLOC_CAP_SPIRAM);
    g_scratch.logits= heap_caps_malloc(V * 4, MALLOC_CAP_SPIRAM);
    g_scratch.scores= heap_caps_malloc(S * 4, MALLOC_CAP_SPIRAM);
    g_scratch.kcache= heap_caps_malloc((size_t)L * S * D * 4, MALLOC_CAP_SPIRAM);
    g_scratch.vcache= heap_caps_malloc((size_t)L * S * D * 4, MALLOC_CAP_SPIRAM);
    if (!g_scratch.x || !g_scratch.kcache) {
        ESP_LOGE(TAG, "PSRAM alloc failed"); return -4;
    }

    ESP_LOGI(TAG, "model loaded: V=%d D=%d L=%d H=%d F=%d P=%d S=%d",
             V, D, L, g_model.c.n_heads, F, P, S);
    g_ready = true;
    return 0;
}

void llm_engine_forward(int token, int pos) {
    llm_forward(&g_model, token, pos, &g_scratch);
}

int llm_engine_generate(const int *prompt_ids, int prompt_len,
                        char *out, int out_cap, int max_new) {
    if (!g_ready) return -1;
    int D = g_model.c.dim, L = g_model.c.n_layers, P = g_model.c.ple_dim;
    int F = g_model.c.ffn, V = g_model.c.vocab, S = g_model.c.seq_len;

    int hist[256], hist_n = 0;
    for (int i = 0; i < prompt_len && i < S; i++) {
        llm_forward(&g_model, prompt_ids[i], i, &g_scratch);
        if (hist_n < 256) hist[hist_n++] = prompt_ids[i];
    }
    int pos = prompt_len;
    int ob = 0;
    for (int step = 0; step < max_new && pos < S; step++) {
        int tok = sample_token(g_scratch.logits, V, g_sampling_temp,
                               g_sampling_topk, hist, hist_n,
                               g_repetition_penalty);
        if (hist_n < 256) hist[hist_n++] = tok;
        char ch[8];
        int cl = decode_token(tok, ch, sizeof(ch));
        if (cl > 0 && ob < out_cap - cl) { memcpy(out + ob, ch, cl); ob += cl; }
        if (tok == 2) break;  // <|endoftext|>
        llm_forward(&g_model, tok, pos, &g_scratch);
        pos++;
    }
    if (ob < out_cap) out[ob] = 0;
    return ob;
}
