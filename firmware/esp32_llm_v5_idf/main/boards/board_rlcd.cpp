/*
 * board_rlcd.c — Waveshare S3 RLCD-4.2 board glue (ESP-IDF V5).
 *
 * UART console (JSON prompt / BTSCAN) + LCD display + BLE keyboard.
 *
 * 键盘 UI (V5.3):
 *   模式 A (预设菜单): ↑/↓ 切换 22 个预烘焙中文问题, Enter 推理
 *   模式 B (自由输入): 字母数字键直接输入 ASCII, Backspace 删除, Enter 推理
 *   Esc: 菜单↔输入切换 / 输入中取消
 *   Tab: 切换输入模式 (预设菜单 ↔ 自由输入)
 *   推理时: 提示词显示在顶部输入区, 生成结果显示在下方输出区
 *
 * Display: DisplayPort (ST7305 via esp_lcd, already ESP-IDF).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "board_rlcd.h"
#include "llm_engine.h"
#include "keyboard_ble.h"
#include "display_bsp.h"
#include "ui_render.h"
#include "presets.h"
#include "prompt_encoder.h"

static const char *TAG = "board";

#define UART_PORT UART_NUM_0
#define UART_BUF_SIZE 1024
#define LINE_BUF 1024

// RLCD-4.2 SPI: mosi=12, scl=11, dc=5, cs=40, rst=41 (matches Arduino v5)
#define LCD_MOSI 12
#define LCD_SCL  11
#define LCD_DC   5
#define LCD_CS   40
#define LCD_RST  41

// ---- UI layout (400x300) ------------------------------------------------
#define UI_TEXT_LEFT   4
#define UI_TEXT_RIGHT  396
#define UI_HEADER_H    14        // 1 行标题 (14px)
#define UI_INPUT_H     28        // 2 行输入区 (14px x 2)
#define UI_INPUT_TOP   (UI_HEADER_H + 4)
#define UI_OUT_TOP     (UI_INPUT_TOP + UI_INPUT_H)
#define UI_OUT_BOTTOM  296

static char line_buf[LINE_BUF];
static int line_pos = 0;
static DisplayPort *g_display = NULL;

// ---- 键盘 UI 状态 -------------------------------------------------------
typedef enum {
    KBD_MODE_PRESET = 0,   // 预设菜单
    KBD_MODE_TEXT   = 1,   // ASCII 自由输入
} KbdMode;

static KbdMode g_kbd_mode = KBD_MODE_PRESET;
static int g_preset_idx = 0;
static char g_input_buf[80];      // 自由输入缓冲 (ASCII)
static int g_input_len = 0;
static bool g_generating = false;

// ---- 渲染辅助 ------------------------------------------------------------
static void ui_render_header(void) {
    // 标题行: 模式 + 提示
    ui_clear_rect(g_display, 0, 0, 399, UI_HEADER_H - 1);
    const char *mode = (g_kbd_mode == KBD_MODE_PRESET) ? "PRESET" : "TEXT";
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "KB: %s  [Tab]switch  [Esc]clear", mode);
    ui_text(g_display, UI_TEXT_LEFT, 0, hdr);
}

static void ui_render_input(void) {
    // 输入区: 2 行
    ui_clear_rect(g_display, 0, UI_INPUT_TOP - 1, 399, UI_INPUT_TOP + UI_INPUT_H - 1);
    if (g_kbd_mode == KBD_MODE_PRESET) {
        // 显示当前预设 + 位置
        char num[24];
        snprintf(num, sizeof(num), "[%d/%d] ", g_preset_idx + 1, KBD_PRESET_COUNT);
        ui_text(g_display, UI_TEXT_LEFT, UI_INPUT_TOP, num);
        ui_text(g_display, UI_TEXT_LEFT + 40, UI_INPUT_TOP, kbd_presets[g_preset_idx].text);
    } else {
        // ASCII 输入: 显示输入缓冲
        if (g_input_len > 0) {
            char buf[88];
            memcpy(buf, g_input_buf, g_input_len);
            buf[g_input_len] = 0;
            ui_text(g_display, UI_TEXT_LEFT, UI_INPUT_TOP, buf);
        } else {
            ui_text(g_display, UI_TEXT_LEFT, UI_INPUT_TOP, "type question... (ASCII)");
        }
    }
    g_display->RLCD_Display();
}

// ---- HID keycode → 输入处理 ----------------------------------------------
// HID 键盘 keycode 表 (0x04=a ... 0x1D=z, 0x1E-0x27=1-0, 0x28=Enter ...)
static int hid_keycode_to_ascii(uint8_t keycode, uint8_t modifier) {
    int shift = (modifier & 0x22) ? 1 : 0;   // L/R Shift
    // 字母 a-z
    if (keycode >= 0x04 && keycode <= 0x1D) {
        char c = 'a' + (keycode - 0x04);
        return shift ? (c - 'a' + 'A') : c;
    }
    // 数字 1-0 (0x1E=1 ... 0x26=9, 0x27=0)
    if (keycode >= 0x1E && keycode <= 0x27) {
        char c = (keycode == 0x27) ? '0' : ('1' + (keycode - 0x1E));
        return shift ? "!@#$%^&*()"[c - '1'] : c;
    }
    // 标点 (无 shift)
    switch (keycode) {
    case 0x2D: return '-';
    case 0x2E: return '=';
    case 0x2F: return '[';
    case 0x30: return ']';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x2C: return ' ';   // Space
    default: return -1;
    }
}

static void kbd_run_inference(const int *ids, int len) {
    g_generating = true;
    // 清空输出区
    ui_clear_rect(g_display, 0, UI_OUT_TOP, 399, UI_OUT_BOTTOM);
    g_display->RLCD_Display();

    char out[1024];
    int ob = llm_engine_generate(ids, len, out, sizeof(out), 60);
    out[ob] = 0;

    // 显示生成结果 (输出区, 自动换行: 简单按 20 字/行)
    int x = UI_TEXT_LEFT, y = UI_OUT_TOP;
    const unsigned char *p = (const unsigned char *)out;
    int col = 0;
    char line[80];
    int lp = 0;
    while (*p && y + UI_ROW_H <= UI_OUT_BOTTOM) {
        if (*p == '\n' || col >= 26) {
            line[lp] = 0;
            ui_text(g_display, x, y, line);
            y += UI_ROW_H;
            col = 0; lp = 0;
            if (*p == '\n') { p++; continue; }
        }
        // UTF-8 复制一个字符
        int clen = ((*p & 0xE0) == 0xC0) ? 2 : ((*p & 0xF0) == 0xE0) ? 3 : 1;
        for (int i = 0; i < clen && *p; i++) line[lp++] = *p++;
        col += (clen == 1) ? 1 : 1;
    }
    if (lp > 0 && y + UI_ROW_H <= UI_OUT_BOTTOM) {
        line[lp] = 0;
        ui_text(g_display, x, y, line);
    }
    g_display->RLCD_Display();
    printf("{\"done\":true,\"tokens\":%d}\n", ob);
    ESP_LOGI(TAG, "generated %d chars", ob);
    g_generating = false;
}

static void kbd_enter_pressed(void) {
    if (g_generating) return;
    int ids[PE_MAX_PROMPT + 8];
    int len = 0;

    if (g_kbd_mode == KBD_MODE_PRESET) {
        // 预设: 直接用预烘焙 ids
        const KbdPreset *p = &kbd_presets[g_preset_idx];
        len = p->len;
        for (int i = 0; i < len && i < PE_MAX_PROMPT + 8; i++) ids[i] = p->ids[i];
        ESP_LOGI(TAG, "preset[%d]: %s", g_preset_idx, p->text);
    } else {
        // 自由输入: 编码 + ChatML
        if (g_input_len == 0) { ESP_LOGW(TAG, "empty input"); return; }
        g_input_buf[g_input_len] = 0;
        len = pe_prompt_from_text(g_input_buf, ids, PE_MAX_PROMPT + 8);
        ESP_LOGI(TAG, "free text: %s (%d tok)", g_input_buf, len);
        if (len <= 0) return;
    }
    kbd_run_inference(ids, len);
}

// --- BLE keyboard callback (keycode + modifier) ---------------------------
void board_key_cb(uint8_t keycode, uint8_t modifier) {
    ESP_LOGI(TAG, "BLE key: 0x%02x mod=0x%02x", keycode, modifier);
    if (g_generating) return;

    switch (keycode) {
    case 0x28:  // Enter — run inference
        kbd_enter_pressed();
        break;
    case 0x29:  // Esc — clear / toggle
        if (g_kbd_mode == KBD_MODE_PRESET) {
            g_preset_idx = 0;
        } else {
            g_input_len = 0;
        }
        ui_render_input();
        break;
    case 0x2B:  // Tab — switch mode
        g_kbd_mode = (g_kbd_mode == KBD_MODE_PRESET) ? KBD_MODE_TEXT : KBD_MODE_PRESET;
        ESP_LOGI(TAG, "mode -> %s", g_kbd_mode == KBD_MODE_PRESET ? "PRESET" : "TEXT");
        ui_render_header();
        ui_render_input();
        break;
    case 0x52:  // Up — prev preset
        if (g_kbd_mode == KBD_MODE_PRESET) {
            g_preset_idx = (g_preset_idx + KBD_PRESET_COUNT - 1) % KBD_PRESET_COUNT;
            ui_render_input();
        }
        break;
    case 0x51:  // Down — next preset
        if (g_kbd_mode == KBD_MODE_PRESET) {
            g_preset_idx = (g_preset_idx + 1) % KBD_PRESET_COUNT;
            ui_render_input();
        }
        break;
    case 0x2A:  // Backspace
        if (g_kbd_mode == KBD_MODE_TEXT && g_input_len > 0) {
            g_input_len--;
            ui_render_input();
        }
        break;
    default: {
        // ASCII 输入 (TEXT 模式)
        if (g_kbd_mode == KBD_MODE_TEXT) {
            int c = hid_keycode_to_ascii(keycode, modifier);
            if (c >= 32 && c < 127 && g_input_len < (int)sizeof(g_input_buf) - 1) {
                g_input_buf[g_input_len++] = (char)c;
                ui_render_input();
            }
        }
        break;
    }
    }
}

// ---- UART JSON prompt (保留 COM 输入兼容) --------------------------------
static void handle_json_prompt(char *json) {
    const char *p = strstr(json, "\"ids\":[");
    if (!p) return;
    p += 7;
    int ids[512], n = 0;
    while (*p && *p != ']' && n < 512) {
        if (*p == ' ' || *p == ',') { p++; continue; }
        ids[n++] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
    }
    if (n == 0) return;
    int max = 200;
    p = strstr(json, "\"max\":");
    if (p) { p += 6; while (*p == ' ') p++; int m = atoi(p); if (m > 0 && m <= 128) max = m; }

    // clear display before generation
    if (g_display) {
        g_display->RLCD_ColorClear(ColorWhite);
        g_display->RLCD_Display();
    }

    char out[2048];
    int ob = llm_engine_generate(ids, n, out, sizeof(out), max);
    printf("{\"done\":true,\"tokens\":%d}\n", ob);
    ESP_LOGI(TAG, "generated %d chars", ob);
}

void board_init(void) {
    ESP_LOGI(TAG, "board init (RLCD-4.2)");
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0);

    // LCD display (ST7305 reflective panel, 400x300)
    g_display = new DisplayPort(LCD_MOSI, LCD_SCL, LCD_DC, LCD_CS, LCD_RST, 400, 300);
    if (g_display) {
        g_display->RLCD_Init();
        g_display->RLCD_ColorClear(ColorWhite);
        g_display->RLCD_Display();
        ESP_LOGI(TAG, "LCD ready (400x300 ST7305)");
    } else {
        ESP_LOGE(TAG, "LCD init failed");
    }

    // BLE keyboard
    keyboard_ble_on_key(board_key_cb);

    // 初始 UI: 标题 + 预设菜单
    ui_render_header();
    ui_render_input();
    ESP_LOGI(TAG, "board ready — keyboard UI: [Tab] preset/text, [Up/Dn] nav, [Enter] run");
}

void board_loop(void) {
    while (1) {
        uint8_t c;
        int r = uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(10));
        if (r == 1) {
            if (c == '\n') {
                line_buf[line_pos] = 0;
                line_pos = 0;
                if (line_buf[0] == '{') {
                    handle_json_prompt(line_buf);
                } else if (strcmp(line_buf, "BTSCAN") == 0) {
                    keyboard_ble_scan();
                }
            } else if (c != '\r' && line_pos < LINE_BUF - 1) {
                line_buf[line_pos++] = (char)c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
