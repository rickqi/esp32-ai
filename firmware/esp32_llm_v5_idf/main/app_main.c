/*
 * app_main.c — ESP-IDF V5 PLE LLM firmware entry.
 *
 * V5 (esp32_llm_v5_idf): ESP-IDF port of esp32_llm_zh_v5 (Arduino) for the
 * external MiniMind H1/H2 PLE models.  Pure-C core (llm_v5.h/vocab/rag) is
 * framework-independent; board glue (display_bsp) already uses ESP-IDF APIs.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "llm_engine.h"
#include "board_rlcd.h"
#include "keyboard_ble.h"

static const char *TAG = "v5_main";

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-LLM V5 (ESP-IDF) ===");

    // NVS (BLE bonding persistence + general)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load model from flash partition
    if (llm_engine_load() != 0) {
        ESP_LOGE(TAG, "model load failed — check model partition (0x170000)");
    }

    // Board init (LCD, SD, ADC, WiFi, RTC)
    board_init();

    // BLE keyboard (HID Host)
    keyboard_ble_init();

    ESP_LOGI(TAG, "ready. Send JSON prompt {\"ids\":[...],\"max\":N} over UART");

    // Main loop: read UART lines, dispatch
    board_loop();
}
