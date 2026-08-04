/*
 * keyboard_ble.c — BLE HID keyboard (esp_hid / NimBLE) for V5 (ESP-IDF).
 *
 * Full port of xiaozhi-esp32 bluetooth_keyboard (C++ -> C):
 *   - esp_hidh init + event loop (HID reports)
 *   - two-stage BTSCAN: 1st scan saves pending keyboard, 2nd connects
 *   - ConnectAsync on dedicated task (blocking esp_hidh_dev_open)
 *   - memory pre-check + stale bond cleanup (crash prevention)
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_hidh.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "host/ble_store.h"

#include "keyboard_ble.h"

static const char *TAG = "keyboard_ble";

#define ESP_HID_APPEARANCE_KEYBOARD 0x03C1
#define CONNECT_TASK_STACK (6 * 1024)
#define CONNECT_TASK_PRIO 3
#define MIN_FREE_INTERNAL 15000

typedef struct {
    bool keyboard_addr_found;
    uint8_t keyboard_addr[6];
    uint8_t keyboard_addr_type;
} BleScanCtx;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
} ConnectArgs;

static bool g_connected = false;
static key_cb_t g_key_cb = NULL;
static uint8_t g_pending_addr[6];
static uint8_t g_pending_addr_type;
static bool g_has_pending = false;
static esp_hidh_dev_t *g_dev = NULL;

void keyboard_ble_on_key(key_cb_t cb) { g_key_cb = cb; }
bool keyboard_ble_connected(void) { return g_connected; }

// --- connect (blocking, run on dedicated task) -----------------------------
static void connect_task(void *arg) {
    ConnectArgs *args = (ConnectArgs *)arg;
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_internal < MIN_FREE_INTERNAL) {
        ESP_LOGE(TAG, "Insufficient internal RAM (%u < %d), aborting connect",
                 (unsigned)free_internal, MIN_FREE_INTERNAL);
        free(args);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Opening HID device %02x:%02x:%02x:%02x:%02x:%02x",
             args->addr[0], args->addr[1], args->addr[2],
             args->addr[3], args->addr[4], args->addr[5]);
    esp_hidh_dev_t *dev = esp_hidh_dev_open(args->addr, ESP_HID_TRANSPORT_BLE,
                                            args->addr_type);
    if (dev) {
        g_dev = dev;
        g_connected = true;
        ESP_LOGI(TAG, "HID device opened: %s", esp_hidh_dev_name_get(dev));
    } else {
        ESP_LOGE(TAG, "esp_hidh_dev_open returned NULL");
        // clear stale bond (prevents memory leak across reboots)
        ble_addr_t peer;
        memcpy(peer.val, args->addr, 6);
        peer.type = args->addr_type;
        int rc = ble_store_util_delete_peer(&peer);
        if (rc == 0) ESP_LOGI(TAG, "Cleared stale NimBLE bond");
    }
    free(args);
    vTaskDelete(NULL);
}

static void connect_async(const uint8_t *bda, uint8_t addr_type) {
    if (g_dev) { ESP_LOGW(TAG, "Already connected; disconnect first"); return; }
    ConnectArgs *args = malloc(sizeof(ConnectArgs));
    if (!args) return;
    memcpy(args->addr, bda, 6);
    args->addr_type = addr_type;
    BaseType_t rc = xTaskCreate(connect_task, "kbd_connect",
                                CONNECT_TASK_STACK, args, CONNECT_TASK_PRIO, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "connect task creation failed");
        free(args);
    }
}

// --- NimBLE GAP scan callback (two-stage) ----------------------------------
static int gap_event_callback(struct ble_gap_event *event, void *arg) {
    BleScanCtx *ctx = (BleScanCtx *)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) return 0;
        char name[64] = "?";
        if (fields.name != NULL) {
            size_t n = fields.name_len < 63 ? fields.name_len : 63;
            memcpy(name, fields.name, n);
            name[n] = '\0';
        }
        uint16_t appearance = fields.appearance_is_present ? fields.appearance : 0;
        bool is_keyboard = (appearance == ESP_HID_APPEARANCE_KEYBOARD);
        ESP_LOGI(TAG, "BLE: %02x:%02x:%02x:%02x:%02x:%02x name='%s' app=0x%04x%s",
                 event->disc.addr.val[0], event->disc.addr.val[1],
                 event->disc.addr.val[2], event->disc.addr.val[3],
                 event->disc.addr.val[4], event->disc.addr.val[5],
                 name, appearance, is_keyboard ? " [KBD]" : "");
        if (is_keyboard && !ctx->keyboard_addr_found) {
            memcpy(ctx->keyboard_addr, event->disc.addr.val, 6);
            ctx->keyboard_addr_type = event->disc.addr.type;
            ctx->keyboard_addr_found = 1;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        ESP_LOGI(TAG, "DISC_COMPLETE: pending=%d found=%d",
                 g_has_pending ? 1 : 0, ctx->keyboard_addr_found);
        if (g_has_pending) {
            ESP_LOGI(TAG, "Scan complete, connecting to pending keyboard...");
            connect_async(g_pending_addr, g_pending_addr_type);
            g_has_pending = false;
        } else if (ctx->keyboard_addr_found) {
            ESP_LOGI(TAG, "Keyboard found. Send BTSCAN again to connect...");
            memcpy(g_pending_addr, ctx->keyboard_addr, 6);
            g_pending_addr_type = ctx->keyboard_addr_type;
            g_has_pending = true;
        } else {
            ESP_LOGI(TAG, "Scan complete, no keyboard found");
        }
        free(ctx);
        return 0;
    }
    default:
        return 0;
    }
}

void keyboard_ble_scan(void) {
    ESP_LOGI(TAG, "BTSCAN: scanning (pending=%d)", g_has_pending ? 1 : 0);
    struct ble_gap_disc_params params = {};
    params.filter_duplicates = 1;
    params.passive = 0;
    params.itvl = 0x50;
    params.window = 0x30;
    params.filter_policy = 0;
    BleScanCtx *ctx = malloc(sizeof(BleScanCtx));
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    int rc = ble_gap_disc(0, 10 * 1000, &params, gap_event_callback, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        free(ctx);
    } else {
        ESP_LOGI(TAG, "Scanning for BLE keyboards (10s)...");
    }
}

// --- esp_hidh event handler ------------------------------------------------
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
        g_dev = NULL;
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
        .callback = hidh_event_handler,
    };
    esp_err_t err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "BLE keyboard ready — send BTSCAN to pair");
}
