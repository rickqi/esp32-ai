/*
 * rag_sd.h — device-side inverted-index retrieval over SD-card RAG index.
 *
 * Companion to rag.h (flash mmap).  This one reads a FULL-KB inverted index
 * from the SD card (index.bin term table resident in PSRAM + doclists
 * streamed; docs.bin doc blocks via fseek).  Triggered by "deep":true.
 *
 * Index format (built by chinese_v4/kb/build_sd_index.py):
 *   index.bin: [u16 n_terms]
 *              term table: per term: u8 len + UTF-8 + u32 doclist_off + u32 doc_count
 *              doclists:   per term: u32 doc_id[]  (streamed, fseek)
 *   docs.bin:  [u32 n_docs][u32 doc_off[n_docs+1]]
 *              per doc: u16 doc_len + u16 label_len + UTF-8 + label
 *   meta.bin:  [u32 N][u16 n_terms] per term: u8 len + UTF-8 + u8 idf
 *
 * I/O notes (from ESP-IDF perf guidance):
 *   - Use POSIX open/read/lseek, keep fds open across queries.
 *   - I/O buffer in internal SRAM (DMA-aligned), NOT PSRAM.
 *   - Term table lives in PSRAM (MALLOC_CAP_SPIRAM).
 */

#ifndef RAG_SD_H
#define RAG_SD_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifndef RAGSD_TAG
#define RAGSD_TAG "rag"
#endif

#define RAGSD_INDEX_PATH "/sdcard/rag/index.bin"
#define RAGSD_DOCS_PATH  "/sdcard/rag/docs.bin"
#define RAGSD_META_PATH  "/sdcard/rag/meta.bin"
#define RAGSD_OVERLAY_PATH "/sdcard/rag/term_overlay.bin"
#define RAGSD_TOP_K      3
#define RAGSD_MAX_Q      128
#define RAGSD_MAX_DOCS   3          // evidence docs injected (2 used + slack)
#define RAGSD_DOC_CAP    40         // max chars per evidence doc (与 PC 端 DOC_CHARS=40 一致)
#define RAGSD_TERM_BONUS 600        // 术语命中加分 (压过纯单字噪声, 见 RAG_INDEX_ANALYSIS)
#define RAGSD_OVERLAY_MAX_TERMS 512 // 词表上限 (当前 287)

typedef struct {
  // resident (PSRAM)
  char     **terms;        // n_terms UTF-8 strings (single chars)
  uint32_t  *term_off;     // doclist offset in index.bin
  uint32_t  *term_cnt;     // doclist length (doc ids)
  uint8_t   *idf;          // per-term idf weight
  uint32_t   n_terms;
  uint32_t   n_docs;
  // file handles (kept open)
  int        fd_index;
  int        fd_docs;
  int        fd_meta;
  // I/O buffer (internal SRAM, DMA-safe)
  uint8_t   *io_buf;
  size_t     io_buf_sz;
  bool       ready;
  // ---- term overlay (方案 B: 术语叠加层, 修复单字 IDF 边界误配) ----
  char      *ov_terms[RAGSD_OVERLAY_MAX_TERMS];  // 术语 UTF-8 (PSRAM, 按长度降序)
  uint16_t  *ov_doclist[RAGSD_OVERLAY_MAX_TERMS]; // doc id 数组 (PSRAM, u16)
  uint16_t   ov_cnt[RAGSD_OVERLAY_MAX_TERMS];     // doclist 长度
  uint32_t   ov_n_terms;
  bool       ov_ready;
} RagSD;

static RagSD g_rag_sd;   // single instance (firmware is single-threaded)

// --- internal helpers ------------------------------------------------------

// Load one UTF-8 term (len-prefixed) from fd at current position into dst.
// Returns bytes consumed (len+data), or -1.
static int ragsd_read_term_str(int fd, char *dst, int cap, uint8_t *io) {
  uint8_t hdr[4];
  if (read(fd, hdr, 1) != 1) return -1;
  int len = hdr[0];
  if (len <= 0 || len >= cap) return -1;
  if (read(fd, dst, len) != len) return -1;
  dst[len] = 0;
  return 1 + len;
}

// Linear scan term table for a single-char UTF-8 query char (n_terms ~5K,
// fine at query time; could binary-search if terms were sorted).
static int ragsd_find_term(const char *qchar) {
  for (uint32_t i = 0; i < g_rag_sd.n_terms; i++) {
    if (strcmp(g_rag_sd.terms[i], qchar) == 0) return (int)i;
  }
  return -1;
}

// Stream one term's doclist from index.bin into out[] (up to max_docs).
// Returns count.
static int ragsd_read_doclist(int term_idx, uint32_t *out, int max_docs) {
  uint32_t off = g_rag_sd.term_off[term_idx];
  uint32_t cnt = g_rag_sd.term_cnt[term_idx];
  if (cnt > (uint32_t)max_docs) cnt = max_docs;
  if (lseek(g_rag_sd.fd_index, (off_t)off, SEEK_SET) < 0) return 0;
  // read doc ids in chunks through io_buf (internal SRAM)
  uint32_t got = 0;
  while (got < cnt) {
    uint32_t want = (cnt - got) * 4;
    if (want > g_rag_sd.io_buf_sz) want = g_rag_sd.io_buf_sz;
    ssize_t n = read(g_rag_sd.fd_index, g_rag_sd.io_buf, want);
    if (n <= 0) break;
    uint32_t n_ids = (uint32_t)n / 4;
    for (uint32_t i = 0; i < n_ids; i++) {
      uint32_t id;
      memcpy(&id, g_rag_sd.io_buf + i * 4, 4);
      out[got + i] = id;
    }
    got += n_ids;
  }
  return (int)got;
}

// Read one doc block from docs.bin by doc id (O(1) via offset table).
// Fills utf8 out (NUL-terminated). Returns char count.
static int ragsd_read_doc(uint32_t doc_id, char *out, int out_cap) {
  if (doc_id >= g_rag_sd.n_docs) return 0;
  // doc_off table: read the two offsets (u32 each) at 4 + doc_id*4
  uint8_t hdr[8];
  uint32_t table_pos = 4 + doc_id * 4;
  if (lseek(g_rag_sd.fd_docs, (off_t)table_pos, SEEK_SET) < 0) return 0;
  if (read(g_rag_sd.fd_docs, hdr, 8) != 8) return 0;
  uint32_t off_a, off_b;
  memcpy(&off_a, hdr, 4); memcpy(&off_b, hdr + 4, 4);
  // read doc header (u16 len + u16 label_len)
  uint8_t dh[4];
  if (lseek(g_rag_sd.fd_docs, (off_t)off_a, SEEK_SET) < 0) return 0;
  if (read(g_rag_sd.fd_docs, dh, 4) != 4) return 0;
  uint16_t doc_len, label_len;
  memcpy(&doc_len, dh, 2); memcpy(&label_len, dh + 2, 2);
  if (doc_len > out_cap - 1) doc_len = out_cap - 1;
  ssize_t n = read(g_rag_sd.fd_docs, out, doc_len);
  if (n < 0) return 0;
  out[n] = 0;
  (void)off_b; (void)label_len;
  return (int)n;
}

// --- term overlay (方案 B) ---------------------------------------------------
// term_overlay.bin (built by chinese_v4/kb/build_term_overlay.py):
//   Header: magic "RAG2"(4) + n_terms(u16) + n_docs(u32)
//   Term table: per term: u8 len + UTF-8 + u32 doclist_off + u16 doc_count
//   Doclists:  u16 doc_id[]  (36KB total, 全量常驻 PSRAM)
static int ragsd_load_overlay(void) {
  g_rag_sd.ov_ready = false;
  int fd = open(RAGSD_OVERLAY_PATH, O_RDONLY);
  if (fd < 0) { ESP_LOGW(RAGSD_TAG, "overlay: open fail"); return -1; }   // 无 overlay — 单字检索兜底
  // 读全文件到 PSRAM (36KB)
  off_t fsz = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (fsz <= 0 || fsz > 1024 * 1024) { close(fd); ESP_LOGW(RAGSD_TAG, "overlay: bad size %d", (int)fsz); return -1; }
  uint8_t *blob = (uint8_t *)heap_caps_malloc(fsz, MALLOC_CAP_SPIRAM);
  if (!blob) { close(fd); return -2; }
  ssize_t rd = 0, r;
  while (rd < fsz) { r = read(fd, blob + rd, fsz - rd); if (r <= 0) break; rd += r; }
  close(fd);
  if (rd != fsz) { free(blob); ESP_LOGW(RAGSD_TAG, "overlay: read %d/%d", (int)rd, (int)fsz); return -3; }

  // parse header
  if (fsz < 10 || memcmp(blob, "RAG2", 4) != 0) { free(blob); ESP_LOGW(RAGSD_TAG, "overlay: bad magic"); return -4; }
  uint16_t n_terms;
  memcpy(&n_terms, blob + 4, 2);
  uint32_t n_docs;
  memcpy(&n_docs, blob + 6, 4);
  if (n_terms == 0 || n_terms > RAGSD_OVERLAY_MAX_TERMS || n_docs != g_rag_sd.n_docs) {
    free(blob); ESP_LOGW(RAGSD_TAG, "overlay: hdr mismatch terms=%u docs=%u (want %u)", n_terms, n_docs, g_rag_sd.n_docs); return -5;
  }
  g_rag_sd.ov_n_terms = 0;
  // term table: u8 len + utf8 + u32 off + u16 cnt
  uint8_t *p = blob + 10;
  for (uint32_t i = 0; i < n_terms; i++) {
    uint8_t tl = *p++;
    if (tl == 0 || tl > 32) { free(blob); return -6; }
    char *t = (char *)heap_caps_malloc(tl + 1, MALLOC_CAP_SPIRAM);
    if (!t) { free(blob); return -7; }
    memcpy(t, p, tl); t[tl] = 0;
    p += tl;
    uint32_t off; uint16_t cnt;
    memcpy(&off, p, 4); p += 4;
    memcpy(&cnt, p, 2); p += 2;
    // doclist 数据区在 blob 中: base = blob + off (off 是相对 blob 起点的偏移)
    // blob 数据区从 10 + table_bytes 开始; off 由 PC 端按 header 后偏移计算
    g_rag_sd.ov_terms[g_rag_sd.ov_n_terms] = t;
    g_rag_sd.ov_doclist[g_rag_sd.ov_n_terms] = (uint16_t *)(blob + off);
    g_rag_sd.ov_cnt[g_rag_sd.ov_n_terms] = cnt;
    g_rag_sd.ov_n_terms++;
  }
  g_rag_sd.ov_ready = true;
  ESP_LOGI(RAGSD_TAG, "RAG overlay: %u terms (%dKB)", g_rag_sd.ov_n_terms, (int)(fsz / 1024));
  return 0;
}

// FMM: 正向最大匹配 — 扫描问题文本, 对每个位置找最长匹配术语 (terms 已按长度降序).
// 命中术语的 doclist 加分 (RAGSD_TERM_BONUS * 术语命中数).
// hash_key/hash_score 由调用方提供 (已 memset 0).
static void ragsd_apply_overlay(const char *question, uint32_t *hash_key,
                                uint16_t *hash_score, uint32_t hash_size,
                                uint32_t hash_shift) {
  if (!g_rag_sd.ov_ready) return;
  for (const char *p = question; *p;) {
    // 跳过单字节 (ASCII/标点) — 术语均为中文
    unsigned char b0 = (unsigned char)p[0];
    int clen;
    if (b0 < 0x80) { p++; continue; }
    else if ((b0 & 0xE0) == 0xC0) clen = 2;
    else if ((b0 & 0xF0) == 0xE0) clen = 3;
    else { p++; continue; }

    // 对当前 UTF-8 字符位置, 从第一个字符起找最长术语前缀
    const char *best = NULL; uint16_t best_len = 0; uint16_t *best_dl = NULL; uint16_t best_cnt = 0;
    for (uint32_t i = 0; i < g_rag_sd.ov_n_terms; i++) {
      const char *t = g_rag_sd.ov_terms[i];
      if (strncmp(t, p, strlen(t)) == 0) {  // 前缀匹配
        if (strlen(t) > best_len) {         // 最长优先
          best = t; best_len = (uint16_t)strlen(t);
          best_dl = g_rag_sd.ov_doclist[i]; best_cnt = g_rag_sd.ov_cnt[i];
        }
      }
    }
    if (best) {
      // 术语命中 → doclist 全部加分
      for (uint16_t k = 0; k < best_cnt; k++) {
        uint32_t d = best_dl[k];
        uint32_t h = (d * 2654435761u) >> (32 - hash_shift);
        while (hash_key[h] && hash_key[h] != d + 1) h = (h + 1) & (hash_size - 1);
        if (!hash_key[h]) { hash_key[h] = d + 1; hash_score[h] = RAGSD_TERM_BONUS; }
        else hash_score[h] += RAGSD_TERM_BONUS;
      }
      p += best_len;
    } else {
      p += clen;
    }
  }
}

// --- public API ------------------------------------------------------------

// Load index.bin term table + meta into PSRAM; open docs.bin. Returns 0 ok.
static int ragsd_init(void) {
  memset(&g_rag_sd, 0, sizeof(g_rag_sd));
  g_rag_sd.ready = false;

  g_rag_sd.fd_index = open(RAGSD_INDEX_PATH, O_RDONLY);
  g_rag_sd.fd_docs  = open(RAGSD_DOCS_PATH, O_RDONLY);
  g_rag_sd.fd_meta  = open(RAGSD_META_PATH, O_RDONLY);
  if (g_rag_sd.fd_index < 0 || g_rag_sd.fd_docs < 0 || g_rag_sd.fd_meta < 0) {
    if (g_rag_sd.fd_index >= 0) close(g_rag_sd.fd_index);
    if (g_rag_sd.fd_docs >= 0) close(g_rag_sd.fd_docs);
    if (g_rag_sd.fd_meta >= 0) close(g_rag_sd.fd_meta);
    return -1;  // no SD index — keep flash RAG only
  }

  // I/O buffer in internal SRAM (DMA-safe), 8KB
  g_rag_sd.io_buf_sz = 8192;
  g_rag_sd.io_buf = (uint8_t *)heap_caps_malloc(
      g_rag_sd.io_buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!g_rag_sd.io_buf) { close(g_rag_sd.fd_index); close(g_rag_sd.fd_docs);
                          close(g_rag_sd.fd_meta); return -2; }

  // meta.bin: N u32 + n_terms u16 + per term (u8 len + utf8 + u8 idf)
  uint8_t mh[6];
  if (read(g_rag_sd.fd_meta, mh, 6) != 6) return -3;
  memcpy(&g_rag_sd.n_docs, mh, 4);
  memcpy(&g_rag_sd.n_terms, mh + 4, 2);
  if (g_rag_sd.n_terms > 20000) return -3;   // sanity

  // index.bin: u16 n_terms + term table (u8 len + utf8 + u32 off + u32 cnt)
  uint8_t nh[2];
  if (read(g_rag_sd.fd_index, nh, 2) != 2) return -3;
  uint16_t n_idx_terms;
  memcpy(&n_idx_terms, nh, 2);
  if (n_idx_terms != g_rag_sd.n_terms) return -3;  // must match meta

  g_rag_sd.terms    = (char **)heap_caps_malloc(g_rag_sd.n_terms * sizeof(char *),
                                                MALLOC_CAP_SPIRAM);
  g_rag_sd.term_off = (uint32_t *)heap_caps_malloc(g_rag_sd.n_terms * 4,
                                                   MALLOC_CAP_SPIRAM);
  g_rag_sd.term_cnt = (uint32_t *)heap_caps_malloc(g_rag_sd.n_terms * 4,
                                                   MALLOC_CAP_SPIRAM);
  g_rag_sd.idf      = (uint8_t *)heap_caps_malloc(g_rag_sd.n_terms,
                                                  MALLOC_CAP_SPIRAM);
  if (!g_rag_sd.terms || !g_rag_sd.term_off || !g_rag_sd.term_cnt || !g_rag_sd.idf)
    return -4;

  // read term table + doclist offsets from index.bin
  for (uint32_t i = 0; i < g_rag_sd.n_terms; i++) {
    char tbuf[8];
    int consumed = ragsd_read_term_str(g_rag_sd.fd_index, tbuf, sizeof(tbuf),
                                       g_rag_sd.io_buf);
    if (consumed < 0) return -5;
    g_rag_sd.terms[i] = (char *)heap_caps_malloc(strlen(tbuf) + 1, MALLOC_CAP_SPIRAM);
    if (!g_rag_sd.terms[i]) return -6;
    strcpy(g_rag_sd.terms[i], tbuf);
    uint8_t oa[8];
    if (read(g_rag_sd.fd_index, oa, 8) != 8) return -7;
    memcpy(&g_rag_sd.term_off[i], oa, 4);
    memcpy(&g_rag_sd.term_cnt[i], oa + 4, 4);
  }

  // read idf from meta.bin (terms in same order)
  for (uint32_t i = 0; i < g_rag_sd.n_terms; i++) {
    uint8_t mhdr[1];
    if (read(g_rag_sd.fd_meta, mhdr, 1) != 1) return -8;
    int len = mhdr[0];
    uint8_t tmp[8];
    if (read(g_rag_sd.fd_meta, tmp, len) != len) return -9;
    if (read(g_rag_sd.fd_meta, &g_rag_sd.idf[i], 1) != 1) return -10;
  }

  g_rag_sd.ready = true;
  // 方案 B: 术语叠加层 (修复单字边界误配; 无 overlay 时单字兜底)
  ragsd_load_overlay();
  return 0;
}

// Retrieve top-K docs for a UTF-8 question.  Fills best_docs[] (doc ids) and
// best_scores[].  Returns count.
static int ragsd_retrieve(const char *question, uint32_t *best_docs,
                          int *best_scores, int k) {
  if (!g_rag_sd.ready) return 0;
  // candidate scoring via open-addressing hash table sized to n_docs.
  // Dynamic: next power of 2 above n_docs*1.5 (load <0.67).
  // 137K docs → 2^18 (1.5MB); 11K docs → 2^15 (192KB) — 省 1.3MB PSRAM.
  uint32_t hash_size = 1024;
  while (hash_size < g_rag_sd.n_docs * 3 / 2) hash_size <<= 1;
  if (hash_size < 1024) hash_size = 1024;
  uint32_t hash_shift = 0;
  { uint32_t v = hash_size; while (v >>= 1) hash_shift++; }
  static uint32_t *hash_key = NULL;      // doc id + 1, 0 = empty
  static uint16_t *hash_score = NULL;
  static uint32_t s_hash_size = 0;
  if (!hash_key || s_hash_size != hash_size) {
    if (hash_key) { free(hash_key); free(hash_score); hash_key = NULL; hash_score = NULL; }
    hash_key = (uint32_t *)heap_caps_malloc(hash_size * 4, MALLOC_CAP_SPIRAM);
    hash_score = (uint16_t *)heap_caps_malloc(hash_size * 2, MALLOC_CAP_SPIRAM);
    if (!hash_key || !hash_score) return 0;
    s_hash_size = hash_size;
  }
  memset(hash_key, 0, hash_size * 4);

  // iterate UTF-8 chars of question
  for (const char *p = question; *p;) {
    char qc[8];
    int clen;
    unsigned char b0 = (unsigned char)p[0];
    if (b0 < 0x80) { clen = 1; qc[0] = p[0]; qc[1] = 0; }
    else if ((b0 & 0xE0) == 0xC0) { clen = 2; qc[0] = p[0]; qc[1] = p[1]; qc[2] = 0; }
    else if ((b0 & 0xF0) == 0xE0) { clen = 3; qc[0] = p[0]; qc[1] = p[1]; qc[2] = p[2]; qc[3] = 0; }
    else { p++; continue; }

    int ti = ragsd_find_term(qc);
    if (ti >= 0) {
      int w = g_rag_sd.idf[ti];
      uint32_t off = g_rag_sd.term_off[ti];
      uint32_t cnt = g_rag_sd.term_cnt[ti];
      if (lseek(g_rag_sd.fd_index, (off_t)off, SEEK_SET) < 0) { p += clen; continue; }
      uint32_t remaining = cnt;
      while (remaining > 0) {
        uint32_t want = remaining * 4;
        if (want > g_rag_sd.io_buf_sz) want = g_rag_sd.io_buf_sz;
        ssize_t n = read(g_rag_sd.fd_index, g_rag_sd.io_buf, want);
        if (n <= 0) break;
        uint32_t n_ids = (uint32_t)n / 4;
        for (uint32_t i = 0; i < n_ids; i++) {
          uint32_t d;
          memcpy(&d, g_rag_sd.io_buf + i * 4, 4);
          // hash insert/accumulate
          uint32_t h = (d * 2654435761u) >> (32 - hash_shift);
          while (hash_key[h] && hash_key[h] != d + 1) h = (h + 1) & (hash_size - 1);
          if (!hash_key[h]) { hash_key[h] = d + 1; hash_score[h] = (uint16_t)w; }
          else hash_score[h] += (uint16_t)w;
        }
        remaining -= n_ids;
      }
    }
    p += clen;
  }

  // 方案 B: 术语叠加加分 (FMM 命中术语 → doclist 加权)
  ragsd_apply_overlay(question, hash_key, hash_score, hash_size, hash_shift);

  // scan hash table for top-K
  int scores[RAGSD_TOP_K] = {0, 0, 0};
  uint32_t best[RAGSD_TOP_K] = {0, 0, 0};
  for (uint32_t i = 0; i < hash_size; i++) {
    if (!hash_key[i]) continue;
    int sc = hash_score[i];
    uint32_t d = hash_key[i] - 1;
    for (int kk = 0; kk < k; kk++) {
      if (sc > scores[kk]) {
        for (int j = k - 1; j > kk; j--) { scores[j] = scores[j-1]; best[j] = best[j-1]; }
        scores[kk] = sc; best[kk] = d;
        break;
      }
    }
  }
  int n = 0;
  for (int kk = 0; kk < k; kk++) {
    if (scores[kk] > 0) { best_docs[n] = best[kk]; best_scores[n] = scores[kk]; n++; }
  }
  return n;
}

#endif
