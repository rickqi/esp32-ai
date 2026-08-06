/* verify_v5.c — 用 llm_v5.h (设备同款) 真实验证: 对比 Python golden, nan 敏感 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../esp32_llm_zh_v5/llm_v5.h"

static uint8_t *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*n);
  if (fread(b, 1, *n, f) != *n) exit(1);
  fclose(f); return b;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: verify_v5 model.bin golden.txt\n"); return 1; }
  size_t n; uint8_t *buf = read_file(argv[1], &n);
  Model m; if (llm_load(buf, &m)) { fprintf(stderr, "load fail\n"); return 1; }
  int D = m.c.dim, L = m.c.n_layers, P = m.c.ple_dim, F = m.c.ffn, V = m.c.vocab, S = m.c.seq_len;
  Scratch s;
  s.x=malloc(D*4); s.h=malloc((F>D?F:D)*4); s.qkv=malloc(3*D*4); s.att=malloc(D*4);
  s.g1=malloc(F*4); s.g2=malloc((P>F?P:F)*4); s.ple=malloc(L*P*4); s.tmpP=malloc(L*P*4); s.trow=malloc(L*P*4);
  s.logits=malloc(V*4); s.scores=malloc(S*4);
  s.kcache=malloc((size_t)L*S*D*4); s.vcache=malloc((size_t)L*S*D*4);

  FILE *gf = fopen(argv[2], "r"); if (!gf) { perror(argv[2]); return 1; }
  int plen; fscanf(gf, "%d", &plen);
  int *prompt = malloc(plen*4);
  for (int i=0;i<plen;i++) fscanf(gf, "%d", &prompt[i]);
  float *ref = malloc(V*4);
  for (int i=0;i<V;i++) fscanf(gf, "%f", &ref[i]);
  fclose(gf);

  for (int pos=0; pos<plen; pos++) llm_forward(&m, prompt[pos], pos, &s);

  // nan 敏感对比
  int nan_cnt = 0;
  double maxabs = 0, sum2 = 0;
  int c_top=0, r_top=0, c_nan=0;
  for (int i=0;i<V;i++) {
    float c = s.logits[i];
    if (isnan(c)) { nan_cnt++; continue; }
    double d = (double)c - ref[i];
    double ad = fabs(d);
    if (ad > maxabs) maxabs = ad;
    sum2 += d*d;
    if (i==0 || c > s.logits[c_top]) c_top = i;
    if (ref[i] > ref[r_top]) r_top = i;
  }
  printf("plen=%d nan_logits=%d maxabs=%.4f rms=%.4f C_top=%d Py_top=%d\n",
         plen, nan_cnt, maxabs, sqrt(sum2/V), c_top, r_top);
  // 关键: C_top 是否 == Py_top (top-1 一致性才决定生成质量)
  printf("top1_match=%s\n", (c_top==r_top && nan_cnt==0) ? "YES" : "NO");
  return 0;
}
