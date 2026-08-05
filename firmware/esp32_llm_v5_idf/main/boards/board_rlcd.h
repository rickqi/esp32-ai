/*
 * board_rlcd.h — Waveshare S3 RLCD-4.2 board glue (ESP-IDF V5).
 *
 * LCD (display_bsp, already ESP-IDF), SD card, ADC battery, RTC, UART console.
 */
#ifndef BOARD_RLCD_H
#define BOARD_RLCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Board init: LCD, SD, ADC, RTC, WiFi (if enabled)
void board_init(void);

// Main loop: UART line dispatch (JSON prompt / SHOOT / LOGD / BTSCAN)
void board_loop(void);

// BLE keyboard hotkey handler (registered to keyboard_ble)
void board_key_cb(uint8_t keycode, uint8_t modifier);

#ifdef __cplusplus
}
#endif

#endif
