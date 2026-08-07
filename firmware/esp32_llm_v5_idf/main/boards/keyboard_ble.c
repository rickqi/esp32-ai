/*
 * keyboard_ble.c — BLE HID keyboard (esp_hid / NimBLE) for V5 (ESP-IDF).
 *
 * Port of xiaozhi-esp32 bluetooth_keyboard (C++ -> C), upgraded with
 * production features from valdanylchuk/breezy_bt (MIT):
 *   - NVS persistence of keyboard address + boot auto-reconnect
 *   - Protocol Mode workaround: fall back to BLE_GAP_EVENT_NOTIFY_RX when
 *     the keyboard lacks the Protocol Mode characteristic (esp_hidh would
 *     silently drop notifications otherwise)
 *   - Dual filter: appearance 0x03C1 OR UUID 0x1812 (HID Service)
 *   - Security: Just Works + Secure Connections + bonding (key dist both ways)
 *   - Connection parameter tuning (7.5-15ms interval, 1s supervision)
 *   - REPEAT_PAIRING: clear stale bond + retry
 *   - Key repeat de-duplication (report only newly-pressed keys)
 *
 * Public API unchanged: keyboard_ble_init/scan/connected/on_key.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_hidh.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_private/esp_hidh_private.h"  // esp_hidh_dev_t.protocol_mode

#include "keyboard_ble.h"

// forward decl: NimBLE store config init (defined in nimble, not in headers)
void ble_store_config_init(void);

static const char *TAG = "keyboard_ble";

#define ESP_HID_APPEARANCE_KEYBOARD 0x03C1
#define CONNECT_TASK_STACK (6 * 1024)
#define CONNECT_TASK_PRIO 3
#define MIN_FREE_INTERNAL 15000

// NVS keys (breezy_bt layout)
#define NVS_NAMESPACE  "bt_kbd"
#define NVS_KEY_ADDR   "addr"
#define NVS_KEY_TYPE   "type"

// --- state ---------------------------------------------------------------
static key_cb_t g_key_cb = NULL;
static bool g_connected = false;
static bool g_encrypted = false;
static bool g_have_target = false;
static uint8_t g_target_bda[6];
static uint8_t g_target_type;

static uint8_t s_own_addr_type;
static int s_is_scanning = 0;
static int s_reconnect_mode = 0;
static volatile int s_connect_pending = 0;
static volatile int s_connect_in_progress = 0;
static volatile int s_scan_requested = 0;

static TaskHandle_t s_connect_task = NULL;
static TimerHandle_t s_boot_timer = NULL;

// Protocol Mode workaround state
static uint16_t s_kbd_notify_handle = 0;
static bool s_use_input_event = true;

// Key de-dup state (report only newly-pressed keys)
static uint8_t s_last_keys[6] = {0};

// --- forward decls --------------------------------------------------------
static void start_background_scan(void);
static void start_general_scan(void);
static void request_connection(void);
static void handle_key_report(const uint8_t *data, size_t len);

// ========================= NVS persistence ===============================

static void save_target_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_ADDR, g_target_bda, 6);
        nvs_set_u8(h, NVS_KEY_TYPE, g_target_type);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved keyboard address to NVS");
    }
}

static void load_target_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = 6;
        esp_err_t e1 = nvs_get_blob(h, NVS_KEY_ADDR, g_target_bda, &len);
        esp_err_t e2 = nvs_get_u8(h, NVS_KEY_TYPE, &g_target_type);
        if (e1 == ESP_OK && e2 == ESP_OK) {
            g_have_target = true;
            ESP_LOGI(TAG, "Loaded saved keyboard: %02x:%02x:%02x:%02x:%02x:%02x type=%d",
                     g_target_bda[5], g_target_bda[4], g_target_bda[3],
                     g_target_bda[2], g_target_bda[1], g_target_bda[0], g_target_type);
        }
        nvs_close(h);
    }
}

// ========================= key handling ==================================

static void handle_key_report(const uint8_t *d, size_t len) {
    if (len < 3 || !g_key_cb) return;
    uint8_t modifier = d[0];
    int key_start = (len >= 8) ? 2 : 1;   // 8-byte boot report: 2 header bytes
    for (int i = key_start; i < len && i < key_start + 6; i++) {
        uint8_t key = d[i];
        if (key == 0 || key >= 232) continue;
        // de-dup: report only newly-pressed keys
        int already = 0;
        for (int j = 0; j < 6; j++) if (s_last_keys[j] == key) already = 1;
        if (!already) g_key_cb(key, modifier);
    }
    for (int i = 0; i < 6; i++)
        s_last_keys[i] = (key_start + i < len) ? d[key_start + i] : 0;
}

// ========================= connection task ===============================

static void connect_task(void *arg) {
    ESP_LOGI(TAG, "connect task started");
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // scan requested (BTSCAN with saved target / boot timer)
        if (s_scan_requested) {
            s_scan_requested = 0;
            s_connect_pending = 0;
            if (!g_connected && g_have_target && !s_is_scanning) {
                ESP_LOGI(TAG, "Starting scan to find saved keyboard...");
                start_background_scan();
            }
            continue;
        }

        if (!s_connect_pending || g_connected || !g_have_target) {
            s_connect_pending = 0;
            continue;
        }
        if (s_is_scanning) {
            ble_gap_disc_cancel();
            s_is_scanning = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // memory pre-check (crash prevention)
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_internal < MIN_FREE_INTERNAL) {
            ESP_LOGE(TAG, "Insufficient internal RAM (%u < %d), aborting connect",
                     (unsigned)free_internal, MIN_FREE_INTERNAL);
            continue;
        }

        ESP_LOGI(TAG, ">>> esp_hidh_dev_open %02x:%02x:%02x:%02x:%02x:%02x type=%d (blocking) <<<",
                 g_target_bda[5], g_target_bda[4], g_target_bda[3],
                 g_target_bda[2], g_target_bda[1], g_target_bda[0], g_target_type);
        s_connect_pending = 0;
        g_encrypted = false;
        s_connect_in_progress = 1;
        esp_hidh_dev_t *dev = esp_hidh_dev_open(g_target_bda, ESP_HID_TRANSPORT_BLE,
                                                g_target_type);
        s_connect_in_progress = 0;

        if (dev != NULL) {
            // Protocol Mode workaround: esp_hidh silently drops notifications
            // when the Protocol Mode characteristic is absent (protocol_mode[0]
            // uninitialized != REPORT). Fall back to direct NOTIFY_RX.
            s_use_input_event = (dev->protocol_mode[0] == ESP_HID_PROTOCOL_MODE_REPORT);
            ESP_LOGI(TAG, "connected (protocol_mode=%d, path=%s)",
                     dev->protocol_mode[0],
                     s_use_input_event ? "INPUT_EVENT" : "NOTIFY_RX");
        } else {
            ESP_LOGW(TAG, "esp_hidh_dev_open returned NULL");
            // clear stale bond (prevents memory leak across reboots)
            ble_addr_t peer;
            memcpy(peer.val, g_target_bda, 6);
            peer.type = g_target_type;
            int rc = ble_store_util_delete_peer(&peer);
            if (rc == 0) ESP_LOGI(TAG, "Cleared stale NimBLE bond");
            if (!g_connected) start_background_scan();
        }
    }
}

static void request_connection(void) {
    if (!s_connect_task) return;
    if (g_connected || !g_have_target || s_connect_pending) return;
    s_connect_pending = 1;
    xTaskNotifyGive(s_connect_task);
}

// ========================= NimBLE GAP event listener =====================

static struct ble_gap_event_listener s_gap_listener;

static int gap_event_listener(struct ble_gap_event *event, void *arg) {
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected");
            // connection parameter tuning
            struct ble_gap_upd_params params = {0};
            params.itvl_min = 6;               // 7.5ms
            params.itvl_max = 12;              // 15ms
            params.latency = 0;
            params.supervision_timeout = 100;  // 1s
            ble_gap_update_params(event->connect.conn_handle, &params);
            ble_gap_security_initiate(event->connect.conn_handle);
            // save the identity address for NVS persistence
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                memcpy(g_target_bda, desc.peer_id_addr.val, 6);
                g_target_type = desc.peer_id_addr.type;
                g_have_target = true;
                save_target_to_nvs();
            }
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        g_encrypted = (event->enc_change.status == 0);
        ESP_LOGI(TAG, "encryption %s", g_encrypted ? "OK (bonded)" : "FAILED");
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        g_encrypted = false;
        s_kbd_notify_handle = 0;
        s_use_input_event = true;
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "repeat pairing — clearing old bond");
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX:
        // Protocol Mode workaround: keyboards without the Protocol Mode
        // characteristic report key data via direct notify
        if (s_use_input_event) break;
        {
            uint16_t attr = event->notify_rx.attr_handle;
            int len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (s_kbd_notify_handle == 0 && len >= 3) {
                s_kbd_notify_handle = attr;
                ESP_LOGI(TAG, "NOTIFY_RX handle %d (protocol_mode workaround)", attr);
            }
            if (attr == s_kbd_notify_handle && s_kbd_notify_handle != 0) {
                uint8_t buf[20] = {0};
                ble_hs_mbuf_to_flat(event->notify_rx.om, buf,
                                    len < (int)sizeof(buf) ? len : (int)sizeof(buf), NULL);
                handle_key_report(buf, (size_t)len);
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

// ========================= esp_hidh event handler ========================

static void hidh_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *data) {
    esp_hidh_event_data_t *ev = (esp_hidh_event_data_t *)data;
    if (!ev) return;
    switch (event_id) {
    case ESP_HIDH_OPEN_EVENT:
        if (ev->open.status == ESP_OK) {
            g_connected = true;
            ESP_LOGI(TAG, "HID keyboard connected");
        } else {
            g_connected = false;
            ESP_LOGW(TAG, "HID open failed status=%d", ev->open.status);
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        g_connected = false;
        ESP_LOGI(TAG, "HID keyboard disconnected");
        // auto-reconnect to saved keyboard
        if (g_have_target) start_background_scan();
        break;
    case ESP_HIDH_INPUT_EVENT:
        if (s_use_input_event && ev->input.usage == ESP_HID_USAGE_KEYBOARD) {
            handle_key_report(ev->input.data, ev->input.length);
        }
        break;
    default:
        break;
    }
}

// ========================= scanning ======================================

static int scan_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) return 0;
        // dual filter: appearance 0x03C1 OR HID Service UUID 0x1812
        int is_hid = 0;
        if (fields.appearance_is_present && fields.appearance == ESP_HID_APPEARANCE_KEYBOARD)
            is_hid = 1;
        for (int i = 0; i < fields.num_uuids16; i++) {
            if (ble_uuid_u16(&fields.uuids16[i].u) == 0x1812) { is_hid = 1; break; }
        }
        int addr_match = 0;
        if (g_have_target && memcmp(event->disc.addr.val, g_target_bda, 6) == 0)
            addr_match = 1;

        ESP_LOGI(TAG, "BLE: %02x:%02x:%02x:%02x:%02x:%02x hid=%d match=%d%s",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0],
                 is_hid, addr_match, is_hid ? " [KBD]" : "");

        // reconnect mode: only connect to the saved address
        if (s_reconnect_mode && !addr_match) return 0;
        if (!is_hid) return 0;

        ESP_LOGI(TAG, ">>> Found %s HID keyboard <<<", addr_match ? "MATCHING" : "NEW");
        ble_gap_disc_cancel();
        s_is_scanning = 0;
        s_reconnect_mode = 0;

        memcpy(g_target_bda, event->disc.addr.val, 6);
        g_target_type = event->disc.addr.type;
        g_have_target = true;

        request_connection();

    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_is_scanning = 0;
        s_reconnect_mode = 0;
        ESP_LOGI(TAG, "scan complete (target=%d)", g_have_target ? 1 : 0);
        if (!g_connected && g_have_target) {
            start_background_scan();   // keep trying for the saved keyboard
        }
    }
    return 0;
}

static void start_background_scan(void) {
    if (g_connected || s_is_scanning || !g_have_target) return;
    if (s_connect_in_progress) {
        ESP_LOGW(TAG, "Cancelling stuck connection before scanning...");
        ble_gap_conn_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "Background scan for saved keyboard...");
    s_is_scanning = 1;
    s_reconnect_mode = 1;
    struct ble_gap_disc_params p = {
        .passive = 0,
        .filter_duplicates = 0,
        .itvl = 0x100,
        .window = 0x50,
    };
    if (ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, scan_event_cb, NULL) != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed");
        s_is_scanning = 0;
        s_reconnect_mode = 0;
    }
}

static void start_general_scan(void) {
    if (g_connected) return;
    if (s_is_scanning) { ble_gap_disc_cancel(); s_is_scanning = 0; }
    ESP_LOGI(TAG, "General scan for keyboards (15s)...");
    s_is_scanning = 1;
    s_reconnect_mode = 0;
    struct ble_gap_disc_params p = {
        .passive = 0,
        .filter_duplicates = 1,
    };
    if (ble_gap_disc(s_own_addr_type, 15000, &p, scan_event_cb, NULL) != 0) {
        s_is_scanning = 0;
    }
}

// ========================= public API ====================================

void keyboard_ble_on_key(key_cb_t cb) { g_key_cb = cb; }

bool keyboard_ble_connected(void) { return g_connected; }

bool keyboard_ble_scanning(void) { return s_is_scanning != 0; }
bool keyboard_ble_pairing(void)  { return s_connect_in_progress != 0 || s_connect_pending != 0; }
bool keyboard_ble_has_target(void) { return g_have_target; }

void keyboard_ble_scan(void) {
    if (g_connected) { ESP_LOGI(TAG, "BTSCAN: already connected"); return; }
    if (s_connect_in_progress) {
        ESP_LOGW(TAG, "BTSCAN: cancelling in-progress connection...");
        ble_gap_conn_cancel();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (s_is_scanning) { ble_gap_disc_cancel(); s_is_scanning = 0; }

    if (g_have_target) {
        ESP_LOGI(TAG, "BTSCAN: scanning for saved keyboard...");
        s_scan_requested = 1;
        if (s_connect_task) xTaskNotifyGive(s_connect_task);
        return;
    }
    start_general_scan();
}

esp_err_t keyboard_ble_clear_bonds(void) {
    ESP_LOGI(TAG, "Clearing bonds...");
    ble_store_clear();
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    g_have_target = false;
    return ESP_OK;
}

// ========================= boot auto-reconnect ===========================

static void boot_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (s_connect_task && !g_connected && g_have_target) {
        s_scan_requested = 1;
        xTaskNotifyGive(s_connect_task);
    }
}

static void on_sync(void) {
    ESP_LOGI(TAG, "BLE synced");
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (g_have_target && s_boot_timer) {
        ESP_LOGI(TAG, "Saved keyboard found — boot timer (100ms) armed");
        xTimerStart(s_boot_timer, 0);
    }
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "BLE reset: %d", reason);
}

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ========================= init ==========================================

void keyboard_ble_init(void) {
    ESP_LOGI(TAG, "BLE keyboard init (esp_hidh + NimBLE)");

    // NVS (idempotent with app_main)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }

    load_target_from_nvs();

    xTaskCreate(connect_task, "kbd_connect", CONNECT_TASK_STACK, NULL,
                CONNECT_TASK_PRIO, &s_connect_task);
    s_boot_timer = xTimerCreate("kbd_boot", pdMS_TO_TICKS(100), pdFALSE, NULL,
                                boot_timer_cb);

    // classic BT not needed — release its memory for BLE
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    nimble_port_init();
    ble_store_config_init();
    ble_svc_gap_device_name_set("ESP32-Console");

    esp_hidh_config_t config = {
        .callback = hidh_event_handler,
        .event_stack_size = 4096,
    };
    esp_err_t err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        return;
    }

    // security: Just Works + Secure Connections + bonding
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_gap_event_listener_register(&s_gap_listener, gap_event_listener, NULL);

    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "BLE keyboard ready — BTSCAN to pair (auto-reconnect ON)");
}
