/*
 * board_rlcd.h — Waveshare S3 RLCD-4.2 board glue (ESP-IDF V5).
 *
 * LCD (display_bsp, already ESP-IDF), SD card, ADC battery, RTC, UART console.
 */
#ifndef BOARD_RLCD_H
#define BOARD_RLCD_H

// Board init: LCD, SD, ADC, RTC, WiFi (if enabled)
void board_init(void);

// Main loop: UART line dispatch (JSON prompt / SHOOT / LOGD / BTSCAN)
void board_loop(void);

#endif
