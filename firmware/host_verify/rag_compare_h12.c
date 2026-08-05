/*
 * rag_compare_h12.c — H1/H2 RAG vs 无 RAG 生成质量对比验证.
 *
 * Loads model_llm.bin (llm_v5.h) + SD RAG index, generates:
 *   A. 无 RAG:   prompt = <question>
 *   B. 有 RAG:   prompt = <evidence> <question>
 * Compares output quality (evidence reproduction / coherence).
 *
 * Compile (WSL):
 *   gcc -O3 -o /tmp/ragcmp firmware/host_verify/rag_compare_h12.c \
 *       -I firmware/esp32_llm_zh_v5 -lm
 *   /tmp/ragcmp firmware/model_v5/H2/model_llm.bin "1185 3042 777 4807"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int decode_token(int id, char *buf, int cap) {
  if (id < 0 || id >= VOCAB_N) return 0;
  int a = VOCAB_OFF[id], b = VOCAB_OFF[id + 1];
  int n = b - a;
  if (n > cap - 1) n = cap - 1;
  memcpy(buf, VOCAB_BLOB + a, n);
  buf[n] = 0;
  return n;
}

// generate from token ids; returns chars written
static int generate(Model *m, Scratch *s, const int *ids, int n_ids,
                    char *out, int cap, int max_new) {
  int D = m->c.dim, L = m->c.n_layers, P = m->c.ple_dim, F = m->c.ffn;
  int V = m->c.vocab, S = m->c.seq_len;
  int hist[256], hn = 0;
  for (int i = 0; i < n_ids && i < S; i++) {
    llm_forward(m, ids[i], i, s);
    if (hn < 256) hist[hn++] = ids[i];
  }
  int pos = n_ids, ob = 0;
  for (int step = 0; step < max_new && pos < S; step++) {
    int tok = sample_token(s->logits, V, 0.8f, 40, hist, hn, 1.3f);
    if (hn < 256) hist[hn++] = tok;
    char ch[8]; int cl = decode_token(tok, ch, sizeof(ch));
    if (cl > 0 && ob < cap - cl) { memcpy(out + ob, ch, cl); ob += cl; }
    if (tok == 2) break;
    llm_forward(m, tok, pos, s);
    pos++;
  }
  if (ob < cap) out[ob] = 0;
  return ob;
}

// minimal UTF-8 -> token ids (single-char BPE; chars not in vocab skipped)
static int utf8_to_ids(const char *text, int *ids, int max_ids) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)text;
  while (*p && n < max_ids) {
    int clen;
    unsigned char b0 = p[0];
    if (b0 < 0x80) clen = 1;
    else if ((b0 & 0xE0) == 0xC0) clen = 2;
    else if ((b0 & 0xF0) == 0xE0) clen = 3;
    else { p++; continue; }
    // find matching vocab token (exact byte match)
    for (int t = 4; t < VOCAB_N; t++) {
      int tl = VOCAB_OFF[t + 1] - VOCAB_OFF[t];
      if (tl == clen && memcmp(VOCAB_BLOB + VOCAB_OFF[t], p, clen) == 0) {
        ids[n++] = t; break;
      }
    }
    p += clen;
  }
  return n;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <model_llm.bin> \"<prompt token ids or chinese text>\" [max_new]\n", argv[0]);
    return 2;
  }
  const char *bin = argv[1];
  int max_new = argc > 3 ? atoi(argv[3]) : 80;

  size_t n;
  uint8_t *buf = read_file(bin, &n);
  Model m;
  if (llm_load(buf, &m)) { fprintf(stderr, "bad magic\n"); return 1; }
  int D = m.c.dim, L = m.c.n_layers, P = m.c.ple_dim, F = m.c.ffn;
  int V = m.c.vocab, S = m.c.seq_len;
  printf("model: V=%d D=%d L=%d H=%d F=%d P=%d S=%d (%.2fMB)\n",
         V, D, L, m.c.n_heads, F, P, S, n / 1e6);

  Scratch s;
  s.x = malloc(D * 4); s.h = malloc((F > D ? F : D) * 4);
  s.qkv = malloc(3 * D * 4); s.att = malloc(D * 4);
  s.g1 = malloc(F * 4); s.g2 = malloc((P > F ? P : F) * 4);
  s.ple = malloc(L * P * 4); s.tmpP = malloc(L * P * 4); s.trow = malloc(L * P * 4);
  s.logits = malloc(V * 4);
  s.scores = malloc(S * 4);
  s.kcache = malloc((size_t)L * S * D * 4);
  s.vcache = malloc((size_t)L * S * D * 4);

  // prompt: if argv[2] is numeric space-separated, parse as ids; else UTF-8
  int prompt_ids[64], plen = 0;
  char *arg = argv[2];
  int all_digits = 1;
  for (char *p = arg; *p; p++) if (*p != ' ' && (*p < '0' || *p > '9')) { all_digits = 0; break; }
  if (all_digits) {
    char *p = arg;
    while (*p && plen < 64) {
      while (*p == ' ') p++;
      if (!*p) break;
      prompt_ids[plen++] = atoi(p);
      while (*p && *p != ' ') p++;
    }
  } else {
    plen = utf8_to_ids(arg, prompt_ids, 64);
  }
  printf("prompt tokens: %d\n\n", plen);

  // A. 无 RAG (baseline)
  char outA[1024];
  rng_state = 42;
  int obA = generate(&m, &s, prompt_ids, plen, outA, sizeof(outA), max_new);
  printf("=== [无 RAG] baseline ===\n%s\n\n", outA);

  // B. 有 RAG (system 引导 + 参考材料格式, 同 minimind chat template)
  // 参考材料: 真实肺癌证据 (与问题相关)
  const char *evidence =
    "肺癌早期常见的症状有咳嗽、咳痰、咳血、胸痛、气促、声音嘶哑、反复发热等。"
    "这些症状也可能出现在其他呼吸系统疾病中，需要结合影像学检查明确诊断。";
  // system: "你是一名医学助手，根据提供的参考材料准确回答问题。"
  const char *sys_msg = "你是一名医学助手，根据提供的参考材料准确回答问题。";
  char rag_text[512];
  snprintf(rag_text, sizeof(rag_text),
           "参考材料：\n%s\n\n问题：肺癌早期症状是什么", evidence);
  // 组装: system + user (同 minimind apply_chat_template 语义)
  char full_prompt[640];
  snprintf(full_prompt, sizeof(full_prompt), "%s\n%s", sys_msg, rag_text);
  int rag_ids[192], rn = utf8_to_ids(full_prompt, rag_ids, 192);

  char outB[1024];
  rng_state = 42;
  int obB = generate(&m, &s, rag_ids, rn, outB, sizeof(outB), max_new);
  printf("=== [有 RAG] system + 参考材料 ===\n%s\n\n", outB);

  // C. RAG evidence reproduction score: how much of evidence appears in output
  int ev_ids[128];
  int evn = utf8_to_ids(evidence, ev_ids, 128);
  int ev_covered = 0;
  for (int i = 0; i < evn && i < 30; i++) {
    // check if evidence token appears in output as UTF-8
    char ch[8]; int cl = decode_token(ev_ids[i], ch, sizeof(ch));
    if (cl > 0 && strstr(outB, ch)) ev_covered++;
  }
  printf("=== RAG 效果评估 ===\n");
  printf("无 RAG 输出: %d chars\n有 RAG 输出: %d chars\n", obA, obB);
  printf("evidence 复现率(前30 token): %d/%d = %.0f%%\n",
         ev_covered, 30 > evn ? evn : 30,
         100.0 * ev_covered / (30 > evn ? evn : 30));
  return 0;
}
