/*
 * keyboard_ble.c — BLE HID keyboard (esp_hid / NimBLE) for V5.
 *
 * NOTE: Full esp_hidh integration (two-stage scan + GATT) is ported from
 * xiaozhi-esp32 bluetooth_keyboard.cc.  This file provides the skeleton;
 * the event loop + HID report parsing follow esp_hidh.h (IDF v5.5).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_hidh.h"
#include "nvs_flash.h"

#include "keyboard_ble.h"

static const char *TAG = "keyboard_ble";

static bool g_connected = false;
static key_cb_t g_key_cb = NULL;

void keyboard_ble_on_key(key_cb_t cb) { g_key_cb = cb; }
bool keyboard_ble_connected(void) { return g_connected; }

// esp_hidh event handler (registered via esp_event_handler_register)
static void hidh_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *data) {
    esp_hidh_event_data_t *ev = (esp_hidh_event_data_t *)data;
    if (!ev) return;
    switch (event_id) {
    case ESP_HIDH_OPEN_EVENT:
        if (ev->open.status == ESP_OK) {
            g_connected = true;
            ESP_LOGI(TAG, "keyboard connected");
        } else {
            ESP_LOGW(TAG, "open failed status=%d", ev->open.status);
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        g_connected = false;
        ESP_LOGI(TAG, "keyboard disconnected");
        break;
    case ESP_HIDH_INPUT_EVENT: {
        const uint8_t *d = ev->input.data;
        if (ev->input.length >= 3 && g_key_cb) {
            uint8_t modifier = d[0];
            uint8_t keycode = d[2];
            if (keycode) g_key_cb(keycode, modifier);
        }
        break;
    }
    default:
        break;
    }
}

void keyboard_ble_init(void) {
    ESP_LOGI(TAG, "BLE keyboard init (esp_hidh + NimBLE)");
    esp_hidh_config_t config = {
        .callback = hidh_event_handler,   // IDF v5.5 uses .callback
    };
    esp_err_t err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "BLE keyboard ready — send BTSCAN to pair");
}

void keyboard_ble_scan(void) {
    ESP_LOGI(TAG, "BTSCAN: scanning for BLE keyboard (10s)...");
    // NOTE: full two-stage scan (esp_ble_gap_start_scanning + appearance
    // 0x03C1 filter + NimBLE central connect) ported from xiaozhi
    // bluetooth_keyboard.cc — enable when GAP integration lands.
    ESP_LOGW(TAG, "scan skeleton — full NimBLE GAP in xiaozhi bluetooth_keyboard.cc");
}
