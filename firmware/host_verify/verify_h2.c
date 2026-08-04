/* verify_h2.c — host check of V5 (llm_v5.h) on converted H2 model.
 *
 * Compile (WSL):
 *   gcc -O3 -o /tmp/verify_h2 firmware/host_verify/verify_h2.c -I firmware/esp32_llm_zh_v5 -lm
 *   /tmp/verify_h2 firmware/model_v5/H2/model_llm.bin firmware/model_v5/H2/golden.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../esp32_llm_zh_v5/llm_v5.h"

static uint8_t *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*n);
  if (fread(b, 1, *n, f) != *n) { fprintf(stderr, "short read\n"); exit(1); }
  fclose(f); return b;
}

int main(int argc, char **argv) {
  const char *bin = argc > 1 ? argv[1] : "firmware/model_v5/H2/model_llm.bin";
  const char *gold = argc > 2 ? argv[2] : "firmware/model_v5/H2/golden.txt";
  size_t n;
  uint8_t *buf = read_file(bin, &n);
  Model m;
  if (llm_load(buf, &m)) { fprintf(stderr, "bad magic\n"); return 1; }
  printf("loaded: V=%d D=%d L=%d H=%d F=%d P=%d group=%d  (%.2f MB)\n",
         m.c.vocab, m.c.dim, m.c.n_layers, m.c.n_heads, m.c.ffn, m.c.ple_dim,
         m.c.group, n / 1e6);

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

  FILE *gf = fopen(gold, "r");
  if (!gf) { perror(gold); return 1; }
  int plen; fscanf(gf, "%d", &plen);
  int *prompt = malloc(plen * sizeof(int));
  for (int i = 0; i < plen; i++) fscanf(gf, "%d", &prompt[i]);
  float *ref = malloc(V * sizeof(float));
  for (int i = 0; i < V; i++) fscanf(gf, "%f", &ref[i]);
  fclose(gf);

  for (int t = 0; t < plen; t++) {
    llm_forward(&m, prompt[t], t, &s);
  }
  // last position logits: forward at plen-1 filled s.logits (if final step reads head)
  float *logits = s.logits;
  float maxabs = 0, rms = 0;
  int ctop = 0, ptop = 0;
  for (int i = 0; i < V; i++) {
    float diff = logits[i] - ref[i];
    if (diff < 0) diff = -diff;
    if (diff > maxabs) maxabs = diff;
    rms += (logits[i] - ref[i]) * (logits[i] - ref[i]);
    if (logits[i] > logits[ctop]) ctop = i;
    if (ref[i] > ref[ptop]) ptop = i;
  }
  rms = sqrtf(rms / V);
  printf("logits: C top=%d PyTorch top=%d\n", ctop, ptop);
  printf("max abs diff = %.5f   rms diff = %.6f\n", maxabs, rms);
  if (maxabs < 0.02f) { printf("PASS: C matches PyTorch golden\n"); return 0; }
  printf("FAIL: numerics diverge\n");
  return 2;
}
