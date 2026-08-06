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

// ---- int8 output head staging (Arduino esp32_llm_zh_v5 同款优化) ----------
// tok_emb 头 [V,D] 每 token 全量 matvec, 从 flash mmap 读 14MB/次 (dio 80MHz
// ~40MB/s → 72 tokens ≈ 25-50s, 50x 慢于 host). 启动时把 int4 nibbles 解包为
// int8 存 PSRAM (一次), 每 token 只做 int8×int8 点积, 不再读 flash.
// v2 (2026-08-06): 修复 per-group scale 折叠 bug — 原实现只用 group0 的 scale
// 缩放整行, 但 n_groups>1 (H2: D=384/32=12), 导致输出头 logits 分布破坏。
// 现改为逐 group 累加 (与 llm_v5.h matvec_q8_range 一致)。
static int8_t *s_head_w8 = NULL;       // [rows*cols] 解包 int8 权重 (-7..7)
static uint16_t *s_head_scales = NULL; // [rows*n_groups] 每行每组 fp16 scale
static int s_head_rows = 0, s_head_cols = 0, s_head_groups = 0;

// head_matvec 覆盖: 用 staged int8 权重, 每 token 只读 PSRAM.
// v3 (2026-08-06): 改用 fp32 激活 + 逐组反量化 — 消除 int8 激活量化的大误差
// (实测 int8 激活对 head 引入 max_diff ~4.2, 会扰乱 logits 排序; verify 的
// matvec_q 路径用 fp32 激活故 PASS, 设备此路径是唯一偏离点)。
static void head_matvec_int8(const QT *t, const float *x, float *y) {
  (void)t;
  int gsize = s_head_cols / s_head_groups;  // group=32 (末组可能短)
  for (int r = 0; r < s_head_rows; r++) {
    const int8_t *wrow = s_head_w8 + (size_t)r * s_head_cols;
    const uint16_t *sc = s_head_scales + (size_t)r * s_head_groups;
    float acc = 0.f;
    for (int gi = 0; gi < s_head_groups; gi++) {
      int begin = gi * gsize, end = begin + gsize;
      if (end > s_head_cols) end = s_head_cols;
      float g = 0.f;   // fp32 group dot (权重 int8 反量化 * fp32 激活)
      for (int j = begin; j < end; j++) g += (float)wrow[j] * x[j];
      acc += g * half2float(sc[gi]);
    }
    y[r] = acc;
  }
}

// 启动时把 tok_emb 头解包到 PSRAM (int4 nibbles -> int8, 一次)
static int stage_head_int8(QT *t) {
  s_head_rows = t->rows;
  s_head_cols = t->cols;
  s_head_groups = t->n_groups;
  if (s_head_cols <= 0 || s_head_groups <= 0) {
    ESP_LOGE(TAG, "head invalid rows/cols/groups %d/%d/%d", s_head_rows, s_head_cols, s_head_groups);
    return -1;
  }
  s_head_w8 = heap_caps_malloc((size_t)s_head_rows * s_head_cols, MALLOC_CAP_SPIRAM);
  s_head_scales = heap_caps_malloc((size_t)s_head_rows * s_head_groups * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!s_head_w8 || !s_head_scales) {
    ESP_LOGE(TAG, "head staging alloc failed"); return -2;
  }
  for (int r = 0; r < s_head_rows; r++) {
    const uint8_t *row = t->codes + (size_t)r * t->row_bytes;
    int8_t *dst = s_head_w8 + (size_t)r * s_head_cols;
    for (int j = 0; j < s_head_cols; j++) {
      uint8_t byte = row[j >> 1];
      int code = (j & 1) ? (byte >> 4) : (byte & 0xF);
      dst[j] = (int8_t)(code - 8);
    }
    // 保存每行全部 n_groups 个 fp16 scale (修复: 原只存 group0 的)
    memcpy(s_head_scales + (size_t)r * s_head_groups,
           t->scales + (size_t)r * t->n_groups,
           (size_t)s_head_groups * sizeof(uint16_t));
  }
  ESP_LOGI(TAG, "head staged int8: %.2f MB (%d x %d, %d groups)",
           ((size_t)s_head_rows * s_head_cols + (size_t)s_head_rows * s_head_groups * 2) / 1e6,
           s_head_rows, s_head_cols, s_head_groups);
  return 0;
}

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

    // 输出头 int8 staging: 每 token 不再从 flash 读 14MB (性能关键)
    if (stage_head_int8(&g_model.tok_emb) == 0)
        g_model.head_matvec = head_matvec_int8;
    else
        ESP_LOGW(TAG, "head staging failed — 用 flash mmap matvec (慢)");

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

// 收集回调: 将流式 token 追加到 out 缓冲 (供 llm_engine_generate 复用)
struct llm_collect_ctx { char *out; int cap; int *ob; };
static void llm_engine_collect_token(const char *utf8, int len, void *ctx) {
    struct llm_collect_ctx *c = (struct llm_collect_ctx *)ctx;
    if (c && c->out && len > 0 && *c->ob + len < c->cap) {
        memcpy(c->out + *c->ob, utf8, len);
        *c->ob += len;
    }
}

int llm_engine_generate_stream(const int *prompt_ids, int prompt_len,
                               llm_token_cb_t on_token, void *ctx,
                               int max_new) {
    if (!g_ready) return -1;
    int D = g_model.c.dim, L = g_model.c.n_layers, P = g_model.c.ple_dim;
    int F = g_model.c.ffn, V = g_model.c.vocab, S = g_model.c.seq_len;

    static int hist[256];   // 静态: 避免 main task 栈溢出 (栈仅 3.5KB)
    int hist_n = 0;
    for (int i = 0; i < prompt_len && i < S; i++) {
        llm_forward(&g_model, prompt_ids[i], i, &g_scratch);
        if (hist_n < 256) hist[hist_n++] = prompt_ids[i];
    }
    int pos = prompt_len;
    int n_gen = 0;
    for (int step = 0; step < max_new && pos < S; step++) {
        int tok = sample_token(g_scratch.logits, V, g_sampling_temp,
                               g_sampling_topk, hist, hist_n,
                               g_repetition_penalty);
        if (hist_n < 256) hist[hist_n++] = tok;
        if (on_token) {
            char ch[8];
            int cl = decode_token(tok, ch, sizeof(ch));
            if (cl > 0) on_token(ch, cl, ctx);
        }
        n_gen++;
        if (tok == 2) break;  // <|endoftext|>
        llm_forward(&g_model, tok, pos, &g_scratch);
        pos++;
    }
    return n_gen;
}

int llm_engine_generate(const int *prompt_ids, int prompt_len,
                        char *out, int out_cap, int max_new) {
    if (!g_ready || !out || out_cap <= 0) return -1;
    int ob = 0;
    struct llm_collect_ctx ctx = { out, out_cap, &ob };
    int n = llm_engine_generate_stream(prompt_ids, prompt_len,
        (llm_token_cb_t)llm_engine_collect_token, &ctx, max_new);
    if (ob < out_cap) out[ob] = 0;
    return n;
}
