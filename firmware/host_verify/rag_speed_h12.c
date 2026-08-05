/* rag_speed_h12.c — H1/H2 有/无 RAG 推理速度对比 (llm_v5.h 忠实固件链路)
 *
 * Loads model_llm.bin + prompt id 序列 (JSON, 由 export_prompt_ids.py 生成),
 * 测 prefill 与 decode 两阶段耗时, 输出 tok/s 与各阶段 LLM_PROFILE 耗时.
 *
 * Compile (WSL):
 *   gcc -O3 -o /tmp/ragspeed firmware/host_verify/rag_speed_h12.c \
 *       -I firmware/esp32_llm_zh_v5 -I firmware/esp32_llm_zh_v5 -lm -DLLM_PROFILE
 *   /tmp/ragspeed firmware/model_v5/H2/model_llm.bin /tmp/prompt_ids.json 80
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// MUST be defined before llm_v5.h (its inline llm_forward uses it)
#define LLM_PROFILE_NOW() ({ struct timeval tv; gettimeofday(&tv, 0); \
  (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec; })
#include "../esp32_llm_zh_v5/llm_v5.h"
#include "../esp32_llm_zh_v5/vocab.h"

static uint8_t *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*n);
  if (fread(b, 1, *n, f) != *n) { fprintf(stderr, "short read\n"); exit(1); }
  fclose(f); return b;
}

static uint32_t rng_state = 42;
static uint32_t xrng() {
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
  return rng_state;
}
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
    float tb[64]; int cnt = 0;
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

// --- minimal JSON array of arrays parser: {"q": {"norag":[...],"rag":[...]}, ...}
// We parse all arrays into a flat list: {qname, mode, ids[], n}
#define MAX_PROMPTS 32
#define MAX_IDS 256
typedef struct { char q[64]; char mode[8]; int ids[MAX_IDS]; int n; } Prompt;
static Prompt g_prompts[MAX_PROMPTS];
static int g_np = 0;

// find "key": [ ... ] and collect ints
static void parse_arrays(const char *json, int len) {
  // scan for string keys "norag"/"rag"
  const char *p = json;
  while (p < json + len) {
    // find next quote
    const char *q1 = strchr(p, '"');
    if (!q1) break;
    // key = norag or rag
    if (strncmp(q1, "\"norag\"", 7) == 0 || strncmp(q1, "\"rag\"", 5) == 0) {
      int is_rag = (q1[1] == 'r');
      // find [ ... ]
      const char *br = strchr(q1, '[');
      if (!br) break;
      const char *end = strchr(br, ']');
      if (!end) break;
      Prompt *pr = &g_prompts[g_np];
      // q name: search backwards for the nearest "..." before this key at depth-1
      memset(pr, 0, sizeof(*pr));
      strcpy(pr->mode, is_rag ? "rag" : "norag");
      // find question: scan forward for the first string before this array at outer level
      const char *cq = q1;
      // question key appears before mode key in JSON: find "肺癌..." patterns by
      // walking back to previous top-level "key": {  -- simpler: extract from file
      const char *s = br;
      int n = 0;
      while (s < end && n < MAX_IDS) {
        while (s < end && (*s < '0' || *s > '9')) s++;
        if (s >= end) break;
        pr->ids[n++] = atoi(s);
        while (s < end && *s >= '0' && *s <= '9') s++;
      }
      pr->n = n;
      g_np++;
      p = end + 1;
    } else {
      p = q1 + 1;
    }
  }
}

// fill question names: walk top-level keys (the strings before "norag")
static void fill_qnames(const char *json, int len) {
  const char *p = json;
  int qi = 0;
  // top-level: "question": {"norag":..., "rag":...}
  while (p < json + len) {
    const char *q1 = strchr(p, '"');
    if (!q1) break;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) break;
    // check this key is followed by : { then "norag" or "rag"
    const char *colon = strchr(q2, ':');
    if (!colon) break;
    const char *after = colon + 1;
    while (*after == ' ' || *after == '\t' || *after == '\n') after++;
    if (*after == '{' && qi < MAX_PROMPTS) {
      // question key — but "norag"/"rag" are also keys; check the next string
      const char *nq = strchr(after, '"');
      int is_mode_key = nq && (strncmp(nq, "\"norag\"", 7) == 0 || strncmp(nq, "\"rag\"", 5) == 0);
      if (!is_mode_key) {
        // copy question name (may contain escaped unicode — fine as-is)
        int klen = (int)(q2 - q1 - 1);
        if (klen > 63) klen = 63;
        memcpy(g_prompts[qi].q, q1 + 1, klen);
        g_prompts[qi].q[klen] = 0;
        qi++;
        // note: this key opens an object with 2 arrays; next key at same level
        p = nq ? nq : q2 + 1;
        continue;
      }
    }
    p = q1 + 1;
  }
}

static double now_s() {
  struct timeval tv; gettimeofday(&tv, 0);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <model_llm.bin> <prompt_ids.json> [max_new]\n", argv[0]);
    return 2;
  }
  const char *bin = argv[1];
  int max_new = argc > 3 ? atoi(argv[3]) : 80;

  size_t nb;
  uint8_t *buf = read_file(bin, &nb);
  Model m;
  if (llm_load(buf, &m)) { fprintf(stderr, "bad magic\n"); return 1; }
  int D = m.c.dim, L = m.c.n_layers, P = m.c.ple_dim, F = m.c.ffn, V = m.c.vocab, S = m.c.seq_len;
  printf("model: V=%d D=%d L=%d H=%d F=%d P=%d S=%d (%.2fMB)\n",
         V, D, L, m.c.n_heads, F, P, S, nb / 1e6);

  // load JSON
  size_t jn;
  uint8_t *jb = read_file(argv[2], &jn);
  parse_arrays((const char *)jb, (int)jn);
  fill_qnames((const char *)jb, (int)jn);
  printf("parsed %d prompts\n\n", g_np);

  Scratch s;
  s.x = malloc(D * 4); s.h = malloc((F > D ? F : D) * 4);
  s.qkv = malloc(3 * D * 4); s.att = malloc(D * 4);
  s.g1 = malloc(F * 4); s.g2 = malloc((P > F ? P : F) * 4);
  s.ple = malloc(L * P * 4); s.tmpP = malloc(L * P * 4); s.trow = malloc(L * P * 4);
  s.logits = malloc(V * 4);
  s.scores = malloc(S * 4);
  s.kcache = malloc((size_t)L * S * D * 4);
  s.vcache = malloc((size_t)L * S * D * 4);

  // results table
  printf("%-24s %-6s %6s %6s %9s %9s %9s %9s %9s\n",
         "question", "mode", "n_pfx", "n_gen", "prefill_s", "decode_s", "tok/s", "attn%", "ffn%");
  for (int pi = 0; pi < g_np; pi++) {
    Prompt *pr = &g_prompts[pi];
    int n_ids = pr->n;
    if (n_ids == 0 || n_ids > S - max_new) { printf("skip %s/%s (n=%d)\n", pr->q, pr->mode, n_ids); continue; }

    // ---- prefill (per-token forward, 不采样) ----
    llm_profile_reset(&s);
    memset(s.kcache, 0, (size_t)L * S * D * 4);
    memset(s.vcache, 0, (size_t)L * S * D * 4);
    double t0 = now_s();
    for (int i = 0; i < n_ids; i++) llm_forward(&m, pr->ids[i], i, &s);
    double t_pre = now_s() - t0;

    // ---- decode (采样生成) ----
    rng_state = 42;
    int hist[256], hn = 0;
    for (int i = 0; i < n_ids && i < 256; i++) hist[hn++] = pr->ids[i];
    int pos = n_ids, n_gen = 0;
    double t1 = now_s();
    for (int step = 0; step < max_new && pos < S; step++) {
      int tok = sample_token(s.logits, V, 0.8f, 40, hist, hn, 1.3f);
      if (hn < 256) hist[hn++] = tok;
      n_gen++;
      if (tok == 2) break;
      llm_forward(&m, tok, pos, &s);
      pos++;
    }
    double t_dec = now_s() - t1;

    double tok_s = n_gen / t_dec;
    uint64_t tot = s.profile.attn_us + s.profile.ffn_us + s.profile.input_us
                 + s.profile.ple_us + s.profile.head_us;
    double attn_pct = tot ? 100.0 * s.profile.attn_us / tot : 0;
    double ffn_pct  = tot ? 100.0 * s.profile.ffn_us / tot : 0;
    printf("%-24s %-6s %6d %6d %9.3f %9.3f %9.2f %8.1f%% %8.1f%%\n",
           pr->q, pr->mode, n_ids, n_gen, t_pre, t_dec, tok_s, attn_pct, ffn_pct);
  }
  return 0;
}
