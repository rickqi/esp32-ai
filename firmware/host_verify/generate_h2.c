/* generate_h2.c — V5 (llm_v5.h) local generation test on H2.
 *
 * Loads model_llm.bin (llm_v5.h format) + V5 vocab.h, runs sampling loop,
 * decodes token ids to UTF-8 via VOCAB_BLOB/VOCAB_OFF.
 *
 * Compile (WSL):
 *   gcc -O3 -o /tmp/gen_h2 firmware/host_verify/generate_h2.c \
 *       -I firmware/esp32_llm_zh_v5 -I firmware/esp32_llm_zh_v5 -lm
 *   /tmp/gen_h2 firmware/model_v5/H2/model_llm.bin "提示词文本"
 *
 * NOTE: H2 uses MiniMind BPE vocab (6400) — Chinese text must be tokenized
 * with MiniMind tokenizer first.  For the demo we accept raw prompt token ids
 * via argv:  e.g. /tmp/gen_h2 model.bin "1 500 1000 200 42 777 13 99"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
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

// decode token id -> UTF-8 bytes into buf, return length
static int decode_token(int id, char *buf, int cap) {
  if (id < 0 || id >= VOCAB_N) return 0;
  int a = VOCAB_OFF[id], b = VOCAB_OFF[id + 1];
  int n = b - a;
  if (n > cap - 1) n = cap - 1;
  memcpy(buf, VOCAB_BLOB + a, n);
  buf[n] = 0;
  return n;
}

// simple sampling: temperature + top-k + repetition penalty
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
    static float tb[64];
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

int main(int argc, char **argv) {
  const char *bin = argc > 1 ? argv[1] : "firmware/model_v5/H2/model_llm.bin";
  int max_new = argc > 3 ? atoi(argv[3]) : 100;
  size_t n;
  uint8_t *buf = read_file(bin, &n);
  Model m;
  if (llm_load(buf, &m)) { fprintf(stderr, "bad magic\n"); return 1; }
  printf("loaded: V=%d D=%d L=%d H=%d F=%d P=%d S=%d (%.2fMB)\n",
         m.c.vocab, m.c.dim, m.c.n_layers, m.c.n_heads, m.c.ffn, m.c.ple_dim,
         m.c.seq_len, n / 1e6);

  int D = m.c.dim, L = m.c.n_layers, P = m.c.ple_dim, F = m.c.ffn, V = m.c.vocab, S = m.c.seq_len;
  Scratch s;
  s.x = malloc(D * 4); s.h = malloc((F > D ? F : D) * 4);
  s.qkv = malloc(3 * D * 4); s.att = malloc(D * 4);
  s.g1 = malloc(F * 4); s.g2 = malloc((P > F ? P : F) * 4);
  s.ple = malloc(L * P * 4); s.tmpP = malloc(L * P * 4); s.trow = malloc(L * P * 4);
  s.logits = malloc(V * 4);
  s.scores = malloc(S * 4);
  s.kcache = malloc((size_t)L * S * D * 4);
  s.vcache = malloc((size_t)L * S * D * 4);

  // parse prompt token ids from argv[2] ("1 500 1000 200 42 777 13 99")
  int prompt[64], plen = 0;
  if (argc > 2) {
    char *p = argv[2];
    while (*p && plen < 64) {
      while (*p == ' ') p++;
      if (!*p) break;
      prompt[plen++] = atoi(p);
      while (*p && *p != ' ') p++;
    }
  }
  if (plen == 0) { prompt[plen++] = 1; }
  printf("prompt: ");
  for (int i = 0; i < plen; i++) {
    char ch[8]; decode_token(prompt[i], ch, sizeof(ch));
    printf("%d(%s) ", prompt[i], ch);
  }
  printf("\n\ngenerating %d tokens...\n", max_new);

  int hist[256], hist_n = 0;
  for (int i = 0; i < plen; i++) {
    llm_forward(&m, prompt[i], i, &s);
    if (hist_n < 256) hist[hist_n++] = prompt[i];
  }
  // generation
  int pos = plen;
  char out_buf[4096]; int ob = 0;
  for (int step = 0; step < max_new && pos < S; step++) {
    float *logits = s.logits;
    // read head logits: llm_forward leaves logits at final step? verify.c pattern:
    // we need head matvec — llm_forward should fill s.logits for the token.
    int tok = sample_token(logits, V, 0.8f, 40, hist, hist_n, 1.3f);
    if (hist_n < 256) hist[hist_n++] = tok;
    char ch[8]; int cl = decode_token(tok, ch, sizeof(ch));
    if (cl > 0 && ob < sizeof(out_buf) - cl) { memcpy(out_buf + ob, ch, cl); ob += cl; }
    if (tok == 2) break;  // <|endoftext|> stop
    llm_forward(&m, tok, pos, &s);
    pos++;
  }
  out_buf[ob] = 0;
  printf("\n=== generated ===\n%s\n", out_buf);
  return 0;
}
