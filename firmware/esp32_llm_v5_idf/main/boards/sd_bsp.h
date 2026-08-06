/*
 * sd_bsp.h — SD 卡挂载 (V3 同款: SDMMC CLK=38 CMD=21 D0=39, 1-bit).
 */
#ifndef SD_BSP_H
#define SD_BSP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount SD card to /sdcard. Safe to call once. Returns true if mounted.
bool sd_mount(void);

// Is SD card mounted?
bool sd_ready(void);

#ifdef __cplusplus
}
#endif

#endif
