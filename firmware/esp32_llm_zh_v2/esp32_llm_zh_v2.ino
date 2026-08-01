// PLE TinyLM inference on the ESP32-S3 鈥?涓枃鐗?(zh4-ds).
// The 12.5M-param Chinese model (6.49MB, 4-bit) lives in a flash 'model'
// partition, memory-mapped so the PLE table is read a row at a time from
// flash; the tied head plus scratch and KV cache sit in PSRAM. Same llm.h
// that was verified against PyTorch on the host.

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <mbedtls/base64.h>
#include <Wire.h>
#include <WiFi.h>

// ADC (battery)
#include "driver/adc.h"

#define LLM_PROFILE 1
#define LLM_PROFILE_NOW() esp_timer_get_time()
#include "../common/llm.h"
#include "vocab.h"

// ---- SD card logging (Waveshare RLCD-4.2: SDMMC, CLK=38 CMD=21 D0=39) ------
// Writes every generation (prompt + output) as UTF-8 to /sdcard/logs/llm.log,
// so Chinese output is readable from the SD card even when the serial terminal
// shows mojibake. Uses ESP-IDF sdmmc+fatfs (matches the reference wifi_sta BSP).
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <sys/stat.h>

static sdmmc_card_t *sd_card = NULL;
static bool sd_ok = false;
static FILE *sd_logfp = NULL;

static void sd_init() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = GPIO_NUM_38; slot.cmd = GPIO_NUM_21; slot.d0 = GPIO_NUM_39;
  esp_vfs_fat_sdmmc_mount_config_t mount = {};
  mount.format_if_mount_failed = false;
  mount.max_files = 5;
  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &sd_card);
  sd_ok = (err == ESP_OK);
  if (sd_ok) {
    struct stat st;
    if (stat("/sdcard/logs", &st) != 0) mkdir("/sdcard/logs", 0777);
    Serial.println("SD: mounted /sdcard/logs");
  } else {
    Serial.println("SD: mount failed (no card?)");
  }
}

static void sd_log_open(const char *meta) {
  if (!sd_ok) return;
  sd_logfp = fopen("/sdcard/logs/llm.log", "a");
  if (sd_logfp) { fprintf(sd_logfp, "\n=== %s ===\n", meta); }
}

static void sd_log_close() {
  if (sd_logfp) { fclose(sd_logfp); sd_logfp = NULL; }
}

// Dump the SD log over serial (UTF-8) — lets the user read Chinese output
// without removing the SD card. Triggered by serial command "LOGD".
static void sd_log_dump() {
  if (!sd_ok) { Serial.println("LOGD_ERROR: no SD"); return; }
  FILE *f = fopen("/sdcard/logs/llm.log", "r");
  if (!f) { Serial.println("LOGD_ERROR: no log"); return; }
  char c;
  while (fread(&c, 1, 1, f) == 1) Serial.write((uint8_t)c);
  fclose(f);
  Serial.println("\n--- LOG END ---");
}

// Set to 1 once a display panel is wired up -- see display.h.
// Leave 0 to run serial-only (no panel needed).
#define USE_DISPLAY 1
#if USE_DISPLAY
// ST7305 RLCD (Waveshare ESP32-S3-RLCD-4.2): 4.2" 400x300 monochrome reflective LCD.
#define DISPLAY_KIND DISPLAY_RLCD_ST7305
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

// ---- WiFi STA + Battery ADC globals ----------------------------------------
static char wifi_status[20] = "No WiFi";
static float battery_voltage = 3.7f;
static float shtc3_temp = 25.0f, shtc3_humi = 50.0f;
static unsigned long last_wifi_check = 0;
#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
static bool display_suppress = false;  // suppress 1x display during 2x prompt priming
#endif

// Refresh WiFi status (called from run_generation / header draw).
static void update_wifi_status() {
  unsigned long now = millis();
  if (now - last_wifi_check < 5000) return;  // throttle 5s
  last_wifi_check = now;
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(wifi_status, sizeof(wifi_status), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else {
    strcpy(wifi_status, "No WiFi");
  }
}

// ---- SHTC3 temperature/humidity sensor (I2C 0x70, same bus as RTC) ---------
static bool read_shtc3(float *t, float *h) {
  Wire.beginTransmission(0x70);
  Wire.write(0x35); Wire.write(0x17);  // wake-up
  if (Wire.endTransmission() != 0) return false;
  delay(20);
  Wire.beginTransmission(0x70);
  Wire.write(0x7C); Wire.write(0xA2);  // measure with clock stretch
  if (Wire.endTransmission() != 0) return false;
  delay(20);
  Wire.requestFrom(0x70, 6);
  if (Wire.available() < 6) return false;
  uint16_t tr = (Wire.read() << 8) | Wire.read();
  Wire.read();  // CRC (ignored for simplicity)
  uint16_t hr = (Wire.read() << 8) | Wire.read();
  Wire.read();  // CRC
  *t = -45.0f + 175.0f * tr / 65535.0f;
  *h = 100.0f * hr / 65535.0f;
  return true;
}

// Initialize WiFi station 鈥?connects asynchronously to rickqi11.
// Uses Arduino WiFi library (compatible with Arduino-ESP32 core).
static void init_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin("rickqi11", "18620907850");
  Serial.println("WiFi: connecting to rickqi11... (async)");
}

// ---- Battery ADC (GPIO4, 3x voltage divider for 18650) --------------------
static void init_adc() {
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_12);  // GPIO4
}

static float read_battery() {
  int raw = adc1_get_raw(ADC1_CHANNEL_3);
  float mv = (float)raw * 3300.0f / 4095.0f;   // mV @ ADC pin
  return mv * 3.0f / 1000.0f;                   // battery voltage (3x divider)
}

static int read_battery_pct() {
  float v = read_battery();
  if (v < 3.0f) return 0;
  if (v > 4.12f) return 100;
  return (int)((v - 3.0f) / 1.12f * 100.0f);
}

// ---- serial prompt ----------------------------------------------------------
// Default demo prompt used when PROMPT_TIMEOUT_MS expires.
static const int DEMO_PROMPT_IDS[] = {269, 88, 11, 358, 204};   // "糖尿病二型" (v2 vocab)
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
  if (!display_suppress) display_puts(bytes, len);
#endif
  if (sd_logfp) fwrite(bytes, 1, len, sd_logfp);  // UTF-8 log to SD card
}

#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
// ---- serial screenshot (PBM P4 -> base64, 72-char lines) --------------------
// Protocol (identical to the reference wifi_sta project so PC tooling works):
//   PC -> MCU:  "SHOOT\n"
//   MCU -> PC:  "SCREENSHOT_START\n"
//               <base64 of PBM, 72 chars per line>
//               "SCREENSHOT_END\n"
// PBM format (P4 binary): header "P4\n400 300\n", then 1-bit MSB-first,
// top-to-bottom, 1=black 0=white.  Size: 50 bytes/row x 300 rows = 15000.
static void take_screenshot() {
  if (!rlcd) return;
  int w = rlcd->GetWidth();
  int h = rlcd->GetHeight();
  int row_bytes = (w + 7) / 8;                    // 50 for 400px
  char hdr[24];
  int hdr_len = snprintf(hdr, sizeof(hdr), "P4\n%d %d\n", w, h);
  int pbm_size = hdr_len + row_bytes * h;         // 13 + 15000 = 15013

  uint8_t *pbm = (uint8_t *)malloc(pbm_size);
  if (!pbm) { Serial.println("SCREENSHOT_ERROR: out of memory"); return; }
  memcpy(pbm, hdr, hdr_len);

  // Convert framebuffer -> PBM pixel data (row-major, MSB first, 1=black)
  uint8_t *pdata = pbm + hdr_len;
  for (int y = 0; y < h; y++) {
    for (int bx = 0; bx < row_bytes; bx++) {
      uint8_t byte = 0;
      for (int b = 0; b < 8; b++) {
        int x = bx * 8 + b;
        if (x >= w) break;
        if (rlcd->GetPixel(x, y) == ColorBlack)
          byte |= (0x80 >> b);
      }
      *pdata++ = byte;
    }
  }

  // Base64 encode
  size_t b64_len = 0;
  mbedtls_base64_encode(NULL, 0, &b64_len, pbm, pbm_size);
  uint8_t *b64 = (uint8_t *)malloc(b64_len + 1);
  if (!b64) { free(pbm); Serial.println("SCREENSHOT_ERROR: base64 alloc"); return; }
  mbedtls_base64_encode(b64, b64_len, &b64_len, pbm, pbm_size);
  b64[b64_len] = '\0';

  Serial.println("SCREENSHOT_START");
  const int chunk = 72;
  for (size_t i = 0; i < b64_len; i += chunk) {
    int remain = (int)b64_len - (int)i;
    int len = (remain < chunk) ? remain : chunk;
    Serial.write((const char *)b64 + i, len);
    Serial.println();
  }
  Serial.println("SCREENSHOT_END");

  free(b64);
  free(pbm);
}
#endif

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

static int8_t head_actq[256];  // was 128 - Chinese D=160 overflow       // quantized activation, shared by both cores
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
// Sampling: temperature + top-k + repetition penalty. Greedy argmax makes the
// small Chinese model loop on EOS; sampling escapes the attractor and produces
// varied text.  Repetition penalty (like sft_generate.py) suppresses degenerate
// loops such as "痞痞痞..." on the quantized zh5-med model.
#define SAMPLING_TEMP  1.0f
#define SAMPLING_TOPK  40
#define REPETITION_PENALTY 1.3f
#define HIST_WINDOW    50      // penalize tokens seen in the last 50 positions
static uint32_t rng_state = 42;
static uint32_t xrng() {
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
  return rng_state;
}
static int sample_token(float *logits, int n, float temp, int topk, uint32_t *rng,
                        const int *hist, int hist_n) {
  if (n <= 1) return 0;
  // temperature scaling FIRST (matches sft_generate.py: logits/temp, then penalty)
  if (temp > 0) for (int i = 0; i < n; i++) logits[i] /= temp;
  // repetition penalty: divide logits of recently-generated tokens
  if (hist_n > 0 && hist) {
    for (int h = 0; h < hist_n; h++) {
      int t = hist[h];
      if (t >= 0 && t < n) logits[t] /= REPETITION_PENALTY;
    }
  }
  if (topk > 0 && topk < n) {
    static float topk_buf[64];
    int cnt = 0;
    for (int i = 0; i < n; i++) {
      if (cnt < topk) { topk_buf[cnt++] = logits[i]; continue; }
      int mi = 0; for (int j = 1; j < topk; j++) if (topk_buf[j] < topk_buf[mi]) mi = j;
      if (logits[i] > topk_buf[mi]) topk_buf[mi] = logits[i];
    }
    float thr = 1e30f;
    for (int j = 0; j < topk; j++) if (topk_buf[j] < thr) thr = topk_buf[j];
    for (int i = 0; i < n; i++) if (logits[i] < thr) logits[i] = -1e30f;
  }
  float mx = -1e30f, sum = 0;
  for (int i = 0; i < n; i++) if (logits[i] > mx) mx = logits[i];
  for (int i = 0; i < n; i++) { logits[i] = expf(logits[i] - mx); sum += logits[i]; }
  if (!(sum > 0)) { for (int i = 0; i < n; i++) if (logits[i] >= mx) return i; return 0; }
  float r = (float)(xrng() % 100000) / 100000.0f * sum;
  float c = 0;
  for (int i = 0; i < n; i++) { c += logits[i]; if (c > r) return i; }
  for (int i = n - 1; i >= 0; i--) if (logits[i] >= mx) return i;
  return 0;
}
// Run the full generate loop using the last received prompt (recv_ids/recv_n).
// Writes tokens to serial (raw text) and display, then emits a JSON done signal.
static void run_generation() {
  sd_log_open("generation");   // open UTF-8 log on SD (prompt+output via emit)
#if USE_DISPLAY
  display_home();
#endif
#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
  // TUI: border frame + two-row header + 2x prompt prefix
  display_draw_frame();
  update_wifi_status();               // refresh WiFi
  int bat_pct = read_battery_pct();   // battery %
  read_shtc3(&shtc3_temp, &shtc3_humi);  // refresh SHTC3
  display_draw_header(wifi_status, bat_pct, shtc3_temp, shtc3_humi);
  display_draw_hline(DIV1_Y);
  // Prompt in 2x: decode recv_ids to bytes, render "> " + prompt.
  // Skip SFT structural tokens (BOS/<user>/<end>/<assistant>) so the display
  // shows only the real user question, not the marker text.  Token ids are
  // vocab-dependent: v1 (5904) markers at 2/5901/5902/5903, v2 (6594) at
  // 2/6591/6592/6593.
  char prompt_buf[160];
  int plen = 0;
  for (int i = 0; i < recv_n && plen < (int)sizeof(prompt_buf) - 1; i++) {
    int t = recv_ids[i];
    if (t < 0 || t >= VOCAB_N) continue;
    if (t == 2) continue;                                       // <BOS>
    if (t == (VOCAB_N == 6594 ? 6591 : 5901)) continue;         // <user>
    if (t == (VOCAB_N == 6594 ? 6592 : 5902)) continue;         // <assistant>
    if (t == (VOCAB_N == 6594 ? 6593 : 5903)) continue;         // <end>
    int tlen = VOCAB_OFF[t + 1] - VOCAB_OFF[t];
    for (int j = 0; j < tlen && plen < (int)sizeof(prompt_buf) - 1; j++)
      prompt_buf[plen++] = (char)VOCAB_BLOB[VOCAB_OFF[t] + j];
  }
  prompt_buf[plen] = 0;
  display_draw_prompt_2x("> ", (const unsigned char *)prompt_buf, plen);
  display_suppress = true;  // priming emit() must not 1x-draw the prompt again
#endif
  int pos = 0, tok = 0;
  int64_t decode_us = 0;
  int decoded = 0;

  for (int i = 0; i < recv_n; i++) {               // prime with the prompt
    tok = recv_ids[i];
    emit(tok);
    llm_forward(&model, tok, pos++, &s);
  }

#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
  display_suppress = false;  // generation tokens draw normally (1x output area)
  // Fixed divider + output area (between prompt and footer)
  display_draw_hline(DIV2_Y);
  display_set_output_area(OUT_Y, DIV3_Y - 1);
  display_set_cursor(5, OUT_Y);
#endif

  llm_profile_reset(&s);
  int64_t t_start = esp_timer_get_time();

  // Recent-token history for repetition penalty (ring buffer).
  static int hist[HIST_WINDOW];
  int hist_n = 0;

  for (int step = 0; step < recv_max && pos < model.c.seq_len; step++) {
    // Block SFT structural tokens for small (Chinese) models so they don't
    // appear mid-generation (only EOS/<end> may terminate).
    // Token ids are vocab-dependent: v1 vocab (5904) has <user>/<assistant>/<end>
    // at 5901/5902/5903; v2 vocab (6594) puts them at 6591/6592/6593.
    // <BOS> (2) is also blocked: the model must never re-emit the start marker.
    s.logits[2] = -1e30f;                              // <BOS>
    if (VOCAB_N == 5904) { s.logits[5901] = -1e30f; s.logits[5902] = -1e30f; }
    else if (VOCAB_N == 6594) { s.logits[6591] = -1e30f; s.logits[6592] = -1e30f; }
    // temperature + top-k sampling with repetition penalty
    tok = sample_token(s.logits, VOCAB_N, SAMPLING_TEMP, SAMPLING_TOPK, &rng_state,
                       hist, hist_n);
    // Stop on end markers: 0=English EOT, 3=Chinese EOS, <end> (vocab-dependent)
    if (tok == 0 || tok == 3 || tok == (VOCAB_N == 6594 ? 6593 : 5903)) break;
    // record token in history ring buffer
    if (hist_n < HIST_WINDOW) hist[hist_n++] = tok;
    else { for (int h = 0; h < HIST_WINDOW - 1; h++) hist[h] = hist[h + 1]; hist[HIST_WINDOW - 1] = tok; }
    emit(tok);
#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
    display_draw_cursor();   // show live generation position
#endif
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
  // Keep the generated story (prompt + output) visible on screen instead of
  // overwriting with the stats card.  Stats are still printed to serial above.
  // To restore the stats card, uncomment the next two lines:
  // display_stats(decoded * 1e6f / decode_us, decode_us / 1000.0f / decoded);
#endif
#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
  // Erase the live generation cursor so no stray vertical bar remains next to
  // the last character, then draw the footer bar (does NOT clear the story).
  display_clear_cursor();
  display_draw_footer(decoded * 1e6f / decode_us, decode_us / 1000.0f / decoded);
#endif

  // JSON completion marker: the PC script recognises this as end-of-stream.
  Serial.printf("{\"done\":true,\"tok/s\":%.2f}\n", decoded * 1e6f / total_us);
  blink(0);
  sd_log_close();   // flush generation to SD card log
}

// ---- PCF85063 RTC (Waveshare RLCD-4.2 onboard, I2C: SDA=13, SCL=14) --------
// Read the hardware RTC and set the ESP32 system clock via settimeofday().
// If RTC is unavailable or returns implausible time, falls back to the
// compile-time date (so the footer always shows a reasonable timestamp).
static void init_pcf85063() {
  int yy = 0, mo = -1, dd = 0, hh = 0, mm = 0, ss = 0;
  bool have_rtc = false;
  Wire.begin(13, 14);
  Wire.setClock(100000);
  delay(10);
  // Read 7 RTC registers starting at 0x04 (sec/min/hour/day/wday/month/year)
  Wire.beginTransmission(0x51);
  Wire.write(0x04);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom(0x51, 7);
    if (Wire.available() >= 7) {
      ss = Wire.read() & 0x7F;  mm = Wire.read() & 0x7F;
      hh = Wire.read() & 0x3F;  dd = Wire.read() & 0x3F;
      Wire.read();  // skip weekday
      mo = (Wire.read() & 0x1F) - 1;  // tm_mon 0-11
      yy = ((Wire.read() >> 4) * 10 + (Wire.read() & 0x0F)) + 2000;
      // Validate BCD ranges and year plausibility (within 2 years of compile year)
      int cyr = 2026; sscanf(__DATE__ + 7, "%d", &cyr);  // extract year from __DATE__
      have_rtc = (ss < 60 && mm < 60 && hh < 24 && dd > 0 && dd < 32
                  && mo >= 0 && mo < 12 && abs(yy - cyr) <= 2);
    }
  }
  struct tm tm = {0};
  if (have_rtc) {
    tm.tm_sec = ss; tm.tm_min = mm; tm.tm_hour = hh;
    tm.tm_mday = dd; tm.tm_mon = mo; tm.tm_year = yy - 1900;
    Serial.printf("RTC: time set from PCF85063  %04d-%02d-%02d %02d:%02d\n",
                  yy, mo+1, dd, hh, mm);
  } else {
    // Fallback: use the compile timestamp (__DATE__ "Mmm DD YYYY", __TIME__ "HH:MM:SS")
    static const char *const MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
    char mon_s[4] = ""; int dd_i = 0, yy_i = 0, hh_i = 0, mm_i = 0;
    sscanf(__DATE__, "%3s %d %d", mon_s, &dd_i, &yy_i);
    sscanf(__TIME__, "%d:%d", &hh_i, &mm_i);
    for (int i = 0; i < 12; i++) if (strcmp(mon_s, MON[i]) == 0) { mo = i; break; }
    tm.tm_mday = dd_i; tm.tm_mon = mo; tm.tm_year = yy_i - 1900;
    tm.tm_hour = hh_i; tm.tm_min = mm_i; tm.tm_sec = 0;
    Serial.println("RTC: not available, using compile-time date");
  }
  time_t t = mktime(&tm);
  struct timeval tv = { .tv_sec = t };
  settimeofday(&tv, NULL);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-S3 PLE TinyLM ===");
  init_pcf85063();  // try PCF85063 RTC 鈫?settimeofday() for real date/time
  init_adc();       // battery ADC (GPIO4, 3x divider)
  init_wifi();      // WiFi STA (rickqi11)
  sd_init();        // SD card → /sdcard/logs/ for UTF-8 generation logs 鈥?async connect
  // Read battery once; WiFi will connect in background

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
      if (strcmp(line_buf, "SHOOT") == 0) {
#if USE_DISPLAY && DISPLAY_KIND == DISPLAY_RLCD_ST7305
        take_screenshot();
#else
        Serial.println("SCREENSHOT_ERROR: no RLCD display");
#endif
      }
      else if (strcmp(line_buf, "LOGD") == 0) {
        sd_log_dump();   // dump SD log (UTF-8) over serial
      }
      else if (line_buf[0] == '{' && parse_json_prompt(line_buf, recv_ids, &recv_n, &recv_max) == 0) {
        run_generation();
        Serial.println("{\"ready\":true}");           // signal ready for next prompt
      }
      break;
    }
    if (c != '\r') line_buf[line_pos++] = c;          // strip CR, keep everything else
  }
  delay(1);                                            // yield to idle task
}

