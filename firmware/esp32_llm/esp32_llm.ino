// PLE TinyLM inference on the ESP32-S3.
// The 28.9M-param model (14.9MB, 4-bit) lives in a flash 'model' partition,
// memory-mapped so the 25M table is read a row at a time from flash; the hot
// tied head plus scratch and KV cache sit in PSRAM. Same llm.h that was verified
// against PyTorch on the host -- only the platform hooks differ here.

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define LLM_PROFILE 1
#define LLM_PROFILE_NOW() esp_timer_get_time()
#include "../common/llm.h"
#include "vocab.h"

// Set to 1 once a GMT020-02-7P (2.0" 240x320 ST7789) is wired up — see display.h.
// Leave 0 to run serial-only (no panel needed).
#define USE_DISPLAY 1
#if USE_DISPLAY
#include "display.h"
#endif

// ---- serial prompt input buffer (replaces hardcoded PROMPT_IDS) ---------------
#define MAX_PROMPT_IDS 512
#define LINE_BUF_SIZE 4096
// Timeout before running the default demo prompt when no serial input arrives.
// Set to 0 to wait forever (serial-only mode).
#define PROMPT_TIMEOUT_MS 10000

static int recv_ids[MAX_PROMPT_IDS];  // received token IDs
static int recv_n = 0;                // number of valid IDs in recv_ids
static int recv_max = 200;            // tokens to generate (from "max" field)
static char line_buf[LINE_BUF_SIZE];  // line accumulation buffer
static int line_pos = 0;              // current position in line_buf
// Default demo prompt used when PROMPT_TIMEOUT_MS expires.
static const int DEMO_PROMPT_IDS[] = {433, 447, 259, 405};  // "Once upon a time"
static const int DEMO_N_GENERATE = 200;

// Emit one token to every active output (serial always; TFT when enabled).
static void emit(int tok) {
  if (tok >= VOCAB_N) return;
  const unsigned char *bytes = VOCAB_BLOB + VOCAB_OFF[tok];
  int len = VOCAB_OFF[tok + 1] - VOCAB_OFF[tok];
  // Non-blocking: when no host is draining the USB-CDC buffer (running as a
  // standalone gadget on the display), skip the write instead of stalling the
  // whole generation once the TX buffer fills.
  if ((int)Serial.availableForWrite() >= len) Serial.write(bytes, len);
#if USE_DISPLAY
  display_puts(bytes, len);
#endif
}

Model model;
Scratch s;

// ---- int8 output head (SIMD-friendly) --------------------------------------
// The head is scanned in full every token and dominates runtime. We stage it as
// int8 in PSRAM at boot (int4 nibbles unpacked ONCE), so per token there is no
// nibble unpacking and no float conversion of weights -- just int8 x int8 ->
// int32 dot per row. Its input dim (D=96) is a single group, so one scale per
// row. int8-activation quality was validated on host (val perplexity delta ~0,
// see firmware/host_verify/ppl.c). Output rows split across both LX7 cores.
static int8_t *head_w8 = NULL;      // [rows * cols] unpacked int8 weights (-7..7)
static float  *head_scale8 = NULL;  // [rows] per-row dequant scale
static int head_rows, head_cols;

static int8_t head_actq[128];       // quantized activation, shared by both cores
static float  head_acts;            // its scale

// int8 dot -> int32. Tight and branch-free so the S3 int SIMD / -O3 unrolls it.
static inline int32_t dot_i8(const int8_t *a, const int8_t *b, int n) {
  int32_t acc = 0;
  for (int i = 0; i < n; i++) acc += (int32_t)a[i] * (int32_t)b[i];
  return acc;
}

static void head_rows_range(float *y, int r0, int r1) {
  for (int r = r0; r < r1; r++)
    y[r] = (float)dot_i8(head_actq, head_w8 + (size_t)r * head_cols, head_cols)
           * head_scale8[r] * head_acts;
}

// dual-core plumbing (worker does the first half of the rows on core 0)
static TaskHandle_t head_worker;
static TaskHandle_t inference_task;
static float *volatile head_job_y;
static volatile int head_job_split;

static void head_worker_main(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    head_rows_range(head_job_y, 0, head_job_split);
    xTaskNotifyGive(inference_task);
  }
}

// Matches Model.head_matvec (QT*, float*, float*); QT unused (weights staged).
static void head_matvec_int8(const QT *t, const float *x, float *y) {
  (void)t;
  quantize_act(x, head_cols, head_actq, &head_acts);  // once; both cores read it
  head_job_y = y;
  head_job_split = head_rows / 2;
  xTaskNotifyGive(head_worker);
  head_rows_range(y, head_job_split, head_rows);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void *ps(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) { Serial.printf("PSRAM alloc failed (%u bytes)\n", (unsigned)n); while (1) delay(1000); }
  return p;
}

// Unpack the (row-capped) head from int4 to int8 in PSRAM, once at boot.
static void stage_head_int8(QT *t) {
  head_rows = t->rows; head_cols = t->cols;
  head_w8 = (int8_t *)ps((size_t)head_rows * head_cols);
  head_scale8 = (float *)ps((size_t)head_rows * sizeof(float));
  for (int r = 0; r < head_rows; r++) {
    const uint8_t *row = t->codes + (size_t)r * t->row_bytes;
    int8_t *dst = head_w8 + (size_t)r * head_cols;
    for (int j = 0; j < head_cols; j++) {
      uint8_t byte = row[j >> 1];
      int code = (j & 1) ? (byte >> 4) : (byte & 0xF);
      dst[j] = (int8_t)(code - 8);
    }
    head_scale8[r] = half2float(t->scales[(size_t)r * t->n_groups]);  // n_groups==1
  }
  Serial.printf("head staged int8: %.2f MB\n",
                ((size_t)head_rows * head_cols + (size_t)head_rows * 4) / 1e6);
}

static void blink(uint8_t g) {
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, 0, g, g / 3);
#endif
}

// ---- serial prompt JSON parser ------------------------------------------------
// Parse a JSON line of the form:  {"ids": [433, 447, 259, 405], "max": 200}
// Fills ids[] and sets *n to the count, *max to the requested generation length.
// Returns 0 on success, -1 on parse failure.
static int parse_json_prompt(const char *json, int *ids, int *n, int *max) {
  *n = 0;
  *max = model.c.seq_len;                          // default: generate to context limit
  const char *p = strstr(json, "\"ids\":[");
  if (!p) return -1;
  p += 7;                                          // skip past "ids":[
  while (*p && *p != ']' && *n < MAX_PROMPT_IDS) {
    if (*p == ' ' || *p == ',') { p++; continue; }
    ids[(*n)++] = atoi(p);
    while (*p && *p != ',' && *p != ']') p++;
  }
  if (*n == 0) return -1;
  p = strstr(json, "\"max\":");
  if (p) {
    p += 6;                                        // skip past "max":
    while (*p == ' ') p++;
    int m = atoi(p);
    if (m > 0 && m <= model.c.seq_len) *max = m;
  }
  return 0;
}

// ---- prompt-driven generation ------------------------------------------------
// Run the full generate loop using the last received prompt (recv_ids/recv_n).
// Writes tokens to serial (raw text) and display, then emits a JSON done signal.
static void run_generation() {
  int pos = 0, tok = 0;
  int64_t decode_us = 0;
  int decoded = 0;

  for (int i = 0; i < recv_n; i++) {               // prime with the prompt
    tok = recv_ids[i];
    emit(tok);
    llm_forward(&model, tok, pos++, &s);
  }

  llm_profile_reset(&s);
  int64_t t_start = esp_timer_get_time();

  for (int step = 0; step < recv_max && pos < model.c.seq_len; step++) {
    // greedy: argmax over the trained vocab
    int best = 0; float bv = -1e30f;
    for (int v = 0; v < VOCAB_N; v++)
      if (s.logits[v] > bv) { bv = s.logits[v]; best = v; }
    tok = best;
    emit(tok);
    blink((step & 1) ? 40 : 8);

    int64_t d0 = esp_timer_get_time();
    llm_forward(&model, tok, pos++, &s);
    decode_us += esp_timer_get_time() - d0;
    decoded++;
    if ((step & 7) == 0) delay(0);                  // feed the task WDT
  }
  int64_t total_us = esp_timer_get_time() - t_start;

  Serial.printf("\n\n--- %d tokens in %.2f s ---\n", decoded, total_us / 1e6);
  Serial.printf("throughput: %.2f tok/s   (%.1f ms/token)\n",
                decoded * 1e6 / total_us, decode_us / 1000.0 / decoded);
  if (s.profile.calls) {
    float n = (float)s.profile.calls * 1000.f;
    Serial.printf("profile ms/token: input %.1f | attn %.1f | ffn %.1f | ple %.1f | head %.1f\n",
                  s.profile.input_us / n, s.profile.attn_us / n,
                  s.profile.ffn_us / n, s.profile.ple_us / n,
                  s.profile.head_us / n);
  }
#if USE_DISPLAY
  display_stats(decoded * 1e6f / decode_us, decode_us / 1000.0f / decoded);
#endif

  // JSON completion marker: the PC script recognises this as end-of-stream.
  Serial.printf("{\"done\":true,\"tok/s\":%.2f}\n", decoded * 1e6f / total_us);
  blink(0);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-S3 PLE TinyLM ===");

  // Map the model partition.
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");
  if (!part) { Serial.println("model partition not found"); return; }
  const void *base;
  esp_partition_mmap_handle_t h;
  esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                     ESP_PARTITION_MMAP_DATA, &base, &h);
  if (err != ESP_OK) { Serial.printf("mmap failed: %d\n", err); return; }

  if (llm_load((const uint8_t *)base, &model)) { Serial.println("bad model magic"); return; }
  Cfg *c = &model.c;
  Serial.printf("model: V=%d D=%d L=%d H=%d F=%d P=%d  (mapped %.1f MB)\n",
                c->vocab, c->dim, c->n_layers, c->n_heads, c->ffn, c->ple_dim,
                part->size / 1e6);

#if USE_DISPLAY
  display_begin();
#endif

  // Cap head rows to the trained vocab BEFORE staging: the tokenizer learned
  // 25,353 entries; the padded rows above that can never be emitted (and have no
  // decode entry), so we neither stage nor score them.
  model.tok_emb.rows = VOCAB_N;
  stage_head_int8(&model.tok_emb);  // int8-staged head; input embedding still uses mmap
  inference_task = xTaskGetCurrentTaskHandle();
  if (xTaskCreatePinnedToCore(head_worker_main, "head", 4096, NULL, 2,
                             &head_worker, 0) != pdPASS) {
    Serial.println("head worker creation failed");
    return;
  }
  model.head_matvec = head_matvec_int8;

  int D = c->dim, L = c->n_layers, P = c->ple_dim, F = c->ffn, V = c->vocab, S = c->seq_len;
  s.x = (float *)ps(D * 4);
  s.h = (float *)ps((F > D ? F : D) * 4);
  s.qkv = (float *)ps(3 * D * 4);
  s.att = (float *)ps(D * 4);
  s.g1 = (float *)ps(F * 4);
  s.g2 = (float *)ps((P > F ? P : F) * 4);
  s.ple = (float *)ps(L * P * 4);
  s.tmpP = (float *)ps(L * P * 4);
  s.trow = (float *)ps(L * P * 4);
  s.logits = (float *)ps(V * 4);
  s.scores = (float *)ps(S * 4);
  s.kcache = (float *)ps((size_t)L * S * D * 4);
  s.vcache = (float *)ps((size_t)L * S * D * 4);
  Serial.printf("PSRAM free after alloc: %u KB\n\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

  // ---- ready for serial prompt input ----
  Serial.println("{\"ready\":true}");
}

void loop() {
#if PROMPT_TIMEOUT_MS > 0
  // Fallback: if no prompt arrives within the timeout, use the default demo.
  static unsigned long boot_ms = 0;
  if (!boot_ms) boot_ms = millis();
  if (!recv_n && line_pos == 0 && (millis() - boot_ms > PROMPT_TIMEOUT_MS)) {
    Serial.print("\n[timeout] using demo prompt");
    memcpy(recv_ids, DEMO_PROMPT_IDS, sizeof(DEMO_PROMPT_IDS));
    recv_n = sizeof(DEMO_PROMPT_IDS) / sizeof(int);
    recv_max = DEMO_N_GENERATE;
    run_generation();
    boot_ms = millis();   // reset timer for next cycle
    recv_n = 0;           // go back to waiting after generation
    line_pos = 0;
    Serial.println("{\"ready\":true}");
  }
#endif

  // Accumulate one line from Serial, then parse and run generation.
  while (Serial.available() && line_pos < LINE_BUF_SIZE - 1) {
    char c = (char)Serial.read();
    if (c == '\n') {
      line_buf[line_pos] = '\0';                     // null-terminate
      line_pos = 0;
      if (line_buf[0] == '{' && parse_json_prompt(line_buf, recv_ids, &recv_n, &recv_max) == 0) {
        run_generation();
        Serial.println("{\"ready\":true}");           // signal ready for next prompt
      }
      break;
    }
    if (c != '\r') line_buf[line_pos++] = c;          // strip CR, keep everything else
  }
  delay(1);                                            // yield to idle task
}
