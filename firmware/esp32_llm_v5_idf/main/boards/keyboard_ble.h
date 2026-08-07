/*
 * keyboard_ble.h — BLE HID keyboard (esp_hid / NimBLE) for V5.
 *
 * Port of xiaozhi-esp32 bluetooth_keyboard, upgraded with breezy_bt features:
 * NVS persistence, boot auto-reconnect, Protocol Mode workaround.
 */
#ifndef KEYBOARD_BLE_H
#define KEYBOARD_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Init BLE controller + esp_hidh + NimBLE host (safe to call once).
// Loads saved keyboard from NVS and arms boot auto-reconnect.
void keyboard_ble_init(void);

// Scan: with a saved keyboard -> background scan + auto-connect;
// without -> 15s general scan, saves the first HID keyboard found to NVS.
void keyboard_ble_scan(void);

// Stop scanning / cancel in-progress connection (BOOT key toggle off).
void keyboard_ble_scan_stop(void);

// Is a keyboard connected?
bool keyboard_ble_connected(void);

// Key callback: (HID keycode, modifier bits) — fired once per newly-pressed key.
typedef void (*key_cb_t)(uint8_t keycode, uint8_t modifier);
void keyboard_ble_on_key(key_cb_t cb);

// Forget the saved keyboard + clear NimBLE bonds (NVS erased).
esp_err_t keyboard_ble_clear_bonds(void);

// BLE 状态 (UI header 显示 BT:SCAN/BT:PAIR/BT:ON/BT:OFF)
bool keyboard_ble_scanning(void);     // 扫描中 (s_is_scanning)
bool keyboard_ble_pairing(void);      // 连接建立中 (s_connect_in_progress || s_connect_pending)
bool keyboard_ble_has_target(void);   // 已保存键盘地址 (g_have_target)

#ifdef __cplusplus
}
#endif

#endif
