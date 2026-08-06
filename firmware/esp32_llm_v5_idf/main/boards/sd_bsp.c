/*
 * sd_bsp.c — SD 卡挂载 (移植 V3: SDMMC 1-bit, Waveshare RLCD-4.2 引脚).
 *
 * 引脚: CLK=GPIO38 CMD=GPIO21 D0=GPIO39 (V3 验证可用).
 * 挂载后 /sdcard 可访问, RAG 深搜索引由 rag_sd.h 读取 /sdcard/rag 下的索引.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "sd_bsp.h"

static const char *TAG = "sd_bsp";

static sdmmc_card_t *s_sd_card = NULL;
static bool s_sd_ok = false;

bool sd_mount(void) {
    if (s_sd_ok) return true;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_38;
    slot.cmd = GPIO_NUM_21;
    slot.d0  = GPIO_NUM_39;

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 5;

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &s_sd_card);
    s_sd_ok = (err == ESP_OK);
    if (s_sd_ok) {
        ESP_LOGI(TAG, "SD mounted /sdcard");
        // 建 rag 目录 (索引文件若在则跳过)
        struct stat st;
        if (stat("/sdcard/rag", &st) != 0) mkdir("/sdcard/rag", 0777);
    } else {
        ESP_LOGW(TAG, "SD mount failed (no card?): %s", esp_err_to_name(err));
    }
    return s_sd_ok;
}

bool sd_ready(void) { return s_sd_ok; }
