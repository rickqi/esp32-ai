/*
 * board_rlcd.c — Waveshare S3 RLCD-4.2 board glue (ESP-IDF V5).
 *
 * UART console (JSON prompt / BTSCAN) + LCD display + BLE keyboard hotkeys.
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

static char line_buf[LINE_BUF];
static int line_pos = 0;
static DisplayPort *g_display = NULL;

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

    // BLE keyboard hotkeys
    keyboard_ble_on_key(board_key_cb);
    ESP_LOGI(TAG, "board ready");
}

// --- BLE keyboard hotkey mapping (V5) --------------------------------------
void board_key_cb(uint8_t keycode, uint8_t modifier) {
    (void)modifier;
    ESP_LOGI(TAG, "BLE key: 0x%02x", keycode);
    switch (keycode) {
    case 0x28:  // Enter — demo generation trigger
        ESP_LOGI(TAG, "hotkey Enter");
        break;
    case 0x29:  // Esc — clear display
        if (g_display) { g_display->RLCD_ColorClear(ColorWhite); g_display->RLCD_Display(); }
        break;
    case 0x13:  // R — screenshot marker
        ESP_LOGI(TAG, "hotkey R (screenshot)");
        break;
    case 0x19:  // V — version on LCD
        if (g_display) {
            g_display->RLCD_ColorClear(ColorWhite);
            g_display->RLCD_Display();
            ESP_LOGI(TAG, "hotkey V (version)");
        }
        break;
    case 0x2B:  // Tab — model info
        if (g_display && llm_engine_ready()) {
            Model *m = llm_engine_model();
            ESP_LOGI(TAG, "hotkey Tab: model V=%d D=%d L=%d",
                     m->c.vocab, m->c.dim, m->c.n_layers);
        }
        break;
    default:
        ESP_LOGI(TAG, "hotkey 0x%02x (unmapped)", keycode);
        break;
    }
}

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
