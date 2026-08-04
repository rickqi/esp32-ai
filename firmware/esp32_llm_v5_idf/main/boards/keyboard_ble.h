/*
 * keyboard_ble.h — BLE HID keyboard (esp_hid / NimBLE) for V5.
 *
 * Port of xiaozhi-esp32 bluetooth_keyboard (ESP-IDF native).  MVP: hotkeys.
 */
#ifndef KEYBOARD_BLE_H
#define KEYBOARD_BLE_H

#include <stdint.h>
#include <stdbool.h>

// Init BLE controller + esp_hidh + NimBLE host (safe to call once).
void keyboard_ble_init(void);

// Two-stage scan: 1st saves pending keyboard, 2nd connects.
void keyboard_ble_scan(void);

// Is a keyboard connected?
bool keyboard_ble_connected(void);

// Key callback: (HID keycode, modifier bits)
typedef void (*key_cb_t)(uint8_t keycode, uint8_t modifier);
void keyboard_ble_on_key(key_cb_t cb);

#endif
