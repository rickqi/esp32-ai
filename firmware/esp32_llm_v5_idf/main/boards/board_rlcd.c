/*
 * board_rlcd.c — Waveshare S3 RLCD-4.2 board glue (ESP-IDF V5).
 *
 * UART console: read JSON prompt lines ({"ids":[...],"max":N}), dispatch
 * SHOOT/LOGD/BTSCAN.  LCD via display_bsp (already ESP-IDF).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board_rlcd.h"
#include "llm_engine.h"
#include "keyboard_ble.h"

static const char *TAG = "board";

#define UART_PORT UART_NUM_0
#define UART_BUF_SIZE 1024
#define LINE_BUF 1024

static char line_buf[LINE_BUF];
static int line_pos = 0;

void board_init(void) {
    ESP_LOGI(TAG, "board init (RLCD-4.2)");
    // UART console already configured by ESP-IDF console driver (UART0)
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "board ready");
}

static void handle_json_prompt(char *json) {
    // parse {"ids":[...],"max":N}
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

    char out[2048];
    int ob = llm_engine_generate(ids, n, out, sizeof(out), max);
    printf("{\"done\":true,\"tokens\":%d}\n", ob);
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
