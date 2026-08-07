/*
 * board_rlcd.c — Waveshare S3 RLCD-4.2 board glue (ESP-IDF V5).
 *
 * UART console (JSON prompt / BTSCAN) + LCD display + BLE keyboard.
 *
 * 键盘 UI (V5.3):
 *   模式 A (预设菜单): ↑/↓ 切换 22 个预烘焙中文问题, Enter 推理
 *   模式 B (自由输入): 字母数字键直接输入 ASCII, Backspace 删除, Enter 推理
 *   Esc: 菜单↔输入切换 / 输入中取消
 *   Tab: 切换输入模式 (预设菜单 ↔ 自由输入)
 *   推理时: 提示词显示在顶部输入区, 生成结果显示在下方输出区
 *
 * Display: DisplayPort (ST7305 via esp_lcd, already ESP-IDF).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_rom_uart.h"
#include <time.h>
#include "vocab.h"

#include "board_rlcd.h"
#include "llm_engine.h"
#include "keyboard_ble.h"
#include "display_bsp.h"
#include "ui_render.h"
#include "presets.h"
#include "prompt_encoder.h"
#include "bpe_encoder.h"
#include "rag_retrieval.h"
#include "rag_sd.h"
#include "sd_bsp.h"
#include "driver/usb_serial_jtag.h"
#include "mbedtls/base64.h"

static const char *TAG = "board";

// Firmware version label (header row2 right).  RULE: bump PATCH on every
// user-visible change, MINOR on milestones.  See AGENTS.md.
#define FW_VERSION "v5.3.15"

// 模型语言标识 (标题显示): ZH=中文模型, EN=英文模型.
// 当前 H1/H2 raft_v4 均为中文医学; 未来英文模型部署时改为 "EN".
#define MODEL_LANG "ZH"

#define LINE_BUF 1024

// RLCD-4.2 SPI: mosi=12, scl=11, dc=5, cs=40, rst=41 (matches Arduino v5)
#define LCD_MOSI 12
#define LCD_SCL  11
#define LCD_DC   5
#define LCD_CS   40
#define LCD_RST  41

// ---- 键盘 UI 状态 (须在按键 ISR 处理之前定义) ------------------------------
typedef enum {
    KBD_MODE_PRESET = 0,   // 预设菜单
    KBD_MODE_TEXT   = 1,   // ASCII 自由输入
} KbdMode;

static KbdMode g_kbd_mode = KBD_MODE_PRESET;
static int g_preset_idx = 0;
static char g_input_buf[80];      // 自由输入缓冲 (ASCII)
static int g_input_len = 0;
static bool g_generating = false;

// ---- 自动循环演示状态 (须在按键 ISR 处理之前定义) ---------------------------
#define AUTO_IDLE_MS    5000    // 无输入 5s 后启动自动循环
#define AUTO_NEXT_MS    2000    // 每个预设完成后 2s 执行下一个
static bool g_auto_mode = false;      // 自动循环激活中
static int64_t g_last_activity = 0;   // 最后用户输入时间 (esp_timer us)
static int g_auto_idx = 0;            // 自动循环当前预设

// 板载按键 (Waveshare ESP32-S3-RLCD-4.2, 与 xiaozhi-esp32 一致, Active LOW 上拉):
//   BOOT (GPIO0) = 激活 BT 搜索配对键盘
//   KEY  (GPIO18) = 中断当前推理 + 下翻预设并立即运行 (轮流切换)
#define BTN_BOOT_GPIO GPIO_NUM_0
#define BTN_KEY_GPIO GPIO_NUM_18
#define BTN_DEBOUNCE_US (40 * 1000)   // 消抖
#define BTN_REPEAT_US   (400 * 1000)  // 长按连发间隔

// ISR 置位的事件标志 (推理阻塞 board_loop 期间仍捕获按键)
static volatile bool s_btn_boot_pending = false;
static volatile bool s_btn_key_pending = false;
static int64_t s_btn_boot_last_act = 0, s_btn_key_last_act = 0;
static bool s_bt_was_connected = false;

// ---- 板载按键轮询任务 (独立任务, 推理阻塞 board_loop 期间仍工作) ----------
// 仅读 GPIO0/GPIO18, 不配置其他引脚 (避免干扰显示/SD/console).
static void btn_task(void *arg) {
    (void)arg;
    bool last_boot = true, last_key = true;
    int64_t boot_down_t = 0, key_down_t = 0;
    bool boot_down = false, key_down = false;
    while (1) {
        int64_t now = esp_timer_get_time();
        bool b = gpio_get_level(BTN_BOOT_GPIO);
        bool k = gpio_get_level(BTN_KEY_GPIO);
        // 诊断: 电平变化打印 (确认按键硬件)
        if (b != last_boot || k != last_key) {
            ESP_LOGI(TAG, "BTN lvl: boot=%d key=%d", b, k);
            last_boot = b; last_key = k;
        }
    // BOOT 按下 (Active LOW): 消抖 40ms -> BTSCAN toggle (开/关)
    if (!b && !boot_down) { boot_down = true; boot_down_t = now; }
    else if (b && boot_down) { boot_down = false; }
    if (boot_down && now - boot_down_t >= BTN_DEBOUNCE_US &&
        now - s_btn_boot_last_act >= BTN_REPEAT_US) {
        s_btn_boot_last_act = now;
        s_btn_boot_pending = true;
        llm_engine_request_stop();
        if (keyboard_ble_scanning() || keyboard_ble_pairing()) {
            keyboard_ble_scan_stop();
            ESP_LOGI(TAG, "BOOT btn: BTSCAN OFF");
        } else {
            keyboard_ble_scan();
            ESP_LOGI(TAG, "BOOT btn: BTSCAN ON");
        }
    }
        // KEY 按下: 消抖 40ms -> 预设下翻
        if (!k && !key_down) { key_down = true; key_down_t = now; }
        else if (k && key_down) { key_down = false; }
        if (key_down && now - key_down_t >= BTN_DEBOUNCE_US &&
            now - s_btn_key_last_act >= BTN_REPEAT_US) {
            s_btn_key_last_act = now;
            s_btn_key_pending = true;
            llm_engine_request_stop();
            ESP_LOGI(TAG, "KEY btn: preset next request");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void btns_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO) | (1ULL << BTN_KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,   // Active LOW 下降沿
    };
    gpio_config(&cfg);
    // 按键轮询任务 (独立任务, 推理期间仍工作; 仅读 GPIO0/18)
    xTaskCreate(btn_task, "btn_poll", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "buttons: BOOT=GPIO%d (BTSCAN+stop), KEY=GPIO%d (preset next+run)",
             BTN_BOOT_GPIO, BTN_KEY_GPIO);
}

// 前向声明 (btns_handle 在 UI 函数定义前使用)
static void ui_draw_header(void);
static void ui_render_input(void);

// 处理按键事件 (board_loop 推理间隙执行; 长按连发由 REAPEAT 间隔控制)
static void btns_handle(void) {
    int64_t now = esp_timer_get_time();
    // BOOT: BTSCAN toggle 已在 btn_task 执行; 这里只打断 auto + 刷新 header
    if (s_btn_boot_pending && now - s_btn_boot_last_act >= BTN_REPEAT_US) {
        s_btn_boot_pending = false;
        s_btn_boot_last_act = now;
        ESP_LOGI(TAG, "BOOT btn handled (BTSCAN toggle)");
        g_auto_mode = false;
        g_last_activity = now;
        ui_draw_header();   // BT:SCAN/OFF 状态刷新
    } else if (s_btn_boot_pending) {
        s_btn_boot_pending = false;   // 连发窗口内丢弃
    }
    // KEY: 中断推理已由 ISR 请求; 下翻预设并立即运行
    if (s_btn_key_pending && now - s_btn_key_last_act >= BTN_REPEAT_US) {
        s_btn_key_pending = false;
        s_btn_key_last_act = now;
        ESP_LOGI(TAG, "KEY btn: stop gen, preset %d -> %d", g_preset_idx,
                 (g_preset_idx + 1) % KBD_PRESET_COUNT);
        g_auto_mode = true;
        g_auto_idx = g_preset_idx = (g_preset_idx + 1) % KBD_PRESET_COUNT;
        g_kbd_mode = KBD_MODE_PRESET;
        g_last_activity = now - AUTO_NEXT_MS * 1000;   // 立即触发下一预设
        ui_draw_header();
        ui_render_input();
    } else if (s_btn_key_pending) {
        s_btn_key_pending = false;
    }
    // BLE 键盘连接上升沿 -> 自动切键盘输入模式 (TEXT)
    bool conn = keyboard_ble_connected();
    if (conn && !s_bt_was_connected) {
        ESP_LOGI(TAG, "BLE keyboard connected -> KBD_MODE_TEXT");
        g_kbd_mode = KBD_MODE_TEXT;
        ui_draw_header();
        ui_render_input();
    }
    s_bt_was_connected = conn;
}

// ---- UI layout (400x300, V3 同款 3-zone TUI) -------------------------------
#define TUI_LEFT   1
#define TUI_RIGHT  (399 - UI_BORDER_W)  // 397
#define TUI_TOP    1
#define TUI_BOT    (299 - UI_BORDER_W)  // 297
#define TEXT_LEFT  (TUI_LEFT + UI_BORDER_W + 4)   // 7
#define TEXT_RIGHT (TUI_RIGHT - UI_BORDER_W - 3)  // 392
// Zone rows (V3 布局数值, 400x300 相同)
#define HDR_Y1   3     // header row 1: 2x 标题 (反色)
#define HDR_Y2   16
#define HDR2_Y1  17    // header row 2: 1x 信息条 (反色)
#define HDR2_Y2  26
#define DIV1_Y   27
#define INP_Y1   29    // 键盘输入区 (2 行: 预设菜单 / ASCII 输入)
#define INP_Y2   56
#define DIV2_Y   57
#define OUT_Y    59    // 输出区
#define OUT_BOT  281
#define DIV3_Y   281
#define FTR_Y1   282   // footer (反色)
#define FTR_Y2   293

static char line_buf[LINE_BUF];
static int line_pos = 0;
static DisplayPort *g_display = NULL;

// ---- 自动循环调度 (board_loop) ----------------------------------------------

// 流式渲染上下文 (输出区光标)
static int s_out_x = TEXT_LEFT, s_out_y = OUT_Y;
static int s_out_col = 0;   // 当前行字符数 (换行判断)
static int g_stream_tok = 0;     // 当前推理已生成 token 数 (实时 footer)
static int64_t g_stream_t0 = 0;  // 当前推理起始时间

// ---- TUI 渲染 (V3 风格 3-zone) ---------------------------------------------
// 全屏边框 + 2 条分隔线 (一次绘制后 RLCD_Display, 避免逐元素 flush)
static void ui_draw_frame(void) {
    ui_draw_rect(g_display, TUI_LEFT, TUI_TOP, TUI_RIGHT, TUI_BOT);
    ui_hline(g_display, DIV1_Y, TUI_LEFT + 1, TUI_RIGHT - 1);
    ui_hline(g_display, DIV2_Y, TUI_LEFT + 1, TUI_RIGHT - 1);
    ui_hline(g_display, DIV3_Y, TUI_LEFT + 1, TUI_RIGHT - 1);
    g_display->RLCD_Display();
}

// 反色双行 header: row1 = 标题, row2 = 键盘信息条 (BT/模式/预设位置)
static void ui_draw_header(void) {
    // Row 1: 反色标题居中 (普通字体, 无 X2 缩放)
    ui_fill_rect(g_display, TUI_LEFT + 1, HDR_Y1, TUI_RIGHT - 1, HDR_Y2);
    const char *title = "ESP32-S3 PLE V5:" MODEL_LANG;
    int tw = strlen(title) * UI_CW;
    int tx = TEXT_LEFT + (TEXT_RIGHT - TEXT_LEFT - tw) / 2;
    if (tx < TEXT_LEFT) tx = TEXT_LEFT;
    ui_text_inv(g_display, tx, HDR_Y1 + 2, title);

    // Row 2: 反色信息条 — 左:BT 状态 | 中:模式 | 右:版本号/输入长度
    // (右不再显示 [n/22] — 与输入区预设编号重复; 版本号对齐 V3 header 设计)
    ui_fill_rect(g_display, TUI_LEFT + 1, HDR2_Y1, TUI_RIGHT - 1, HDR2_Y2);
    int y = HDR2_Y1 + 1;
    // 左:BT 状态 (优先级: 连接 > 配对 > 扫描 > 有目标 > 关闭)
    // 配对/扫描中每 500ms 交替闪烁提示 (动态刷新由 board_loop 触发)
    const char *bt;
    if (keyboard_ble_connected())            bt = "BT:ON ";
    else if (keyboard_ble_pairing())         bt = "BT:PAIR";
    else if (keyboard_ble_scanning())        bt = "BT:SCAN";
    else if (keyboard_ble_has_target())      bt = "BT:WAIT";
    else                                     bt = "BT:OFF";
    ui_text_inv(g_display, TEXT_LEFT, y, bt);
    const char *mode = (g_kbd_mode == KBD_MODE_PRESET) ? "PRESET" : "TEXT";
    int mlen = strlen(mode);
    int mx = TEXT_LEFT + (TEXT_RIGHT - TEXT_LEFT - mlen * UI_CW) / 2;
    ui_text_inv(g_display, mx, y, mode);
    char right[24];
    if (g_kbd_mode == KBD_MODE_PRESET)
        snprintf(right, sizeof(right), FW_VERSION);
    else
        snprintf(right, sizeof(right), "%dch", g_input_len);
    ui_text_inv(g_display, TEXT_RIGHT - strlen(right) * UI_CW, y, right);
}

// 反色 footer: 推理统计 (动态宽度布局, 保证不重叠)
// 字段: t/s | ms | V词表 | RAG索引 | N token | 秒
// 每个字段按实际字符数推进, 字段间留 GAP 像素, 无固定宽度步进.
#define FTR_GAP 6   // 字段间像素间隙

static void ui_draw_clock(int y);   // 前向声明 (footer 调用)

static void ui_draw_footer(float tok_s, int ms, int ntok, int secs) {
    ui_fill_rect(g_display, TUI_LEFT, FTR_Y1, TUI_RIGHT, FTR_Y2);
    int y = FTR_Y1 + 2;
    char buf[24];
    int x = TEXT_LEFT;
    int w;   // 当前字段像素宽

    // 模型名 (H1/H2 + 量化位数): 最左
    if (llm_engine_ready()) {
        const Model *mdl = llm_engine_model();
        snprintf(buf, sizeof(buf), "%s-%dB",
                 mdl->c.dim >= 384 ? "H2" : "H1", llm_engine_head_bits());
        ui_text_inv(g_display, x, y, buf);
        w = (int)strlen(buf) * UI_CW;
        x += w + FTR_GAP;
    }

    // t/s (7 字符固定)
    snprintf(buf, sizeof(buf), "%4.1ft/s", tok_s);
    ui_text_inv(g_display, x, y, buf);
    w = (int)strlen(buf) * UI_CW;
    x += w + FTR_GAP;

    // ms (动态 2-4 字符)
    snprintf(buf, sizeof(buf), "%dms", ms);
    ui_text_inv(g_display, x, y, buf);
    w = (int)strlen(buf) * UI_CW;
    x += w + FTR_GAP;

    // V词表 (固定 V6400 = 6 字符)
    snprintf(buf, sizeof(buf), "V%u", (unsigned)VOCAB_N);
    ui_text_inv(g_display, x, y, buf);
    w = (int)strlen(buf) * UI_CW;
    x += w + FTR_GAP;

    // RAG 索引状态 (动态: RAG137K 7字 / noRAG 5字)
    if (rag_retrieval_ready()) {
        snprintf(buf, sizeof(buf), "RAG%uK",
                 (unsigned)(rag_retrieval_doc_count() / 1000));
    } else {
        snprintf(buf, sizeof(buf), "noRAG");
    }
    ui_text_inv(g_display, x, y, buf);
    w = (int)strlen(buf) * UI_CW;
    x += w + FTR_GAP;

    // N token (动态)
    snprintf(buf, sizeof(buf), "N%d", ntok);
    ui_text_inv(g_display, x, y, buf);
    w = (int)strlen(buf) * UI_CW;
    x += w + FTR_GAP;

    // 秒 (动态, 左对齐跟随 — 消除右侧空白, 保证不重叠)
    snprintf(buf, sizeof(buf), "%ds", secs);
    ui_text_inv(g_display, x, y, buf);

    // 日期时间: 右对齐 (与统计字段留间隙, 不重叠)
    ui_draw_clock(y);

    g_display->RLCD_Display();
}

// 时钟: 编译时间基准 + 运行 elapsed, 显示 "MM-DD HH:MM:SS" 右对齐.
// 离线设备无 NTP/RTC, 以编译时间为基准 (重启后从编译时刻重新走时).
static time_t s_clock_base = 0;
static void clock_init(void) {
    if (s_clock_base) return;
    static const char *MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    struct tm t = {0};
    char mon[8]; int d, y, hh, mm, ss;
    if (sscanf(__DATE__, "%7s %d %d", mon, &d, &y) == 3 &&
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) == 3) {
        for (int i = 0; i < 12; i++)
            if (strcmp(MON[i], mon) == 0) { t.tm_mon = i; break; }
        t.tm_mday = d; t.tm_year = y - 1900;
        t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
        s_clock_base = mktime(&t);
    } else {
        s_clock_base = 0;
    }
}

static void ui_draw_clock(int y) {
    clock_init();
    // 时间字段固定右对齐: TUI_RIGHT-1 为右边界, 宽 14 字符 (MM-DD HH:MM:SS)
    int cw = 14 * UI_CW;
    int x0 = TUI_RIGHT - 1 - cw;
    ui_fill_rect(g_display, x0, FTR_Y1, TUI_RIGHT, FTR_Y2);
    char buf[20];
    if (s_clock_base) {
        time_t now = s_clock_base + (time_t)(esp_timer_get_time() / 1000000ULL);
        struct tm *tm = localtime(&now);
        if (tm) strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", tm);
        else snprintf(buf, sizeof(buf), "--:--:--");
    } else {
        snprintf(buf, sizeof(buf), "-- -- --:--");
    }
    ui_text_inv(g_display, x0, y, buf);
    g_display->RLCD_Display();
}

// 渲染输入区 (预设菜单或 ASCII 输入)
static void ui_render_input(void) {
    ui_clear_rect(g_display, TEXT_LEFT, INP_Y1, TEXT_RIGHT, INP_Y2);
    if (g_kbd_mode == KBD_MODE_PRESET) {
        char num[24];
        snprintf(num, sizeof(num), "[%d/%d] ", g_preset_idx + 1, KBD_PRESET_COUNT);
        ui_text(g_display, TEXT_LEFT, INP_Y1, num);
        ui_text(g_display, TEXT_LEFT + 40, INP_Y1, kbd_presets[g_preset_idx].text);
        // 第二行显示提示
        ui_text(g_display, TEXT_LEFT, INP_Y1 + UI_ROW_H,
                "Tab:text | Up/Dn:select | Enter:run | Esc:clear");
    } else {
        if (g_input_len > 0) {
            char buf[88];
            memcpy(buf, g_input_buf, g_input_len);
            buf[g_input_len] = 0;
            ui_text(g_display, TEXT_LEFT, INP_Y1, buf);
        } else {
            ui_text(g_display, TEXT_LEFT, INP_Y1, "type question... (ASCII)");
        }
    }
    g_display->RLCD_Display();
}

// ---- HID keycode → 输入处理 ----------------------------------------------
// HID 键盘 keycode 表 (0x04=a ... 0x1D=z, 0x1E-0x27=1-0, 0x28=Enter ...)
static int hid_keycode_to_ascii(uint8_t keycode, uint8_t modifier) {
    int shift = (modifier & 0x22) ? 1 : 0;   // L/R Shift
    // 字母 a-z
    if (keycode >= 0x04 && keycode <= 0x1D) {
        char c = 'a' + (keycode - 0x04);
        return shift ? (c - 'a' + 'A') : c;
    }
    // 数字 1-0 (0x1E=1 ... 0x26=9, 0x27=0)
    if (keycode >= 0x1E && keycode <= 0x27) {
        char c = (keycode == 0x27) ? '0' : ('1' + (keycode - 0x1E));
        return shift ? "!@#$%^&*()"[c - '1'] : c;
    }
    // 标点 (无 shift)
    switch (keycode) {
    case 0x2D: return '-';
    case 0x2E: return '=';
    case 0x2F: return '[';
    case 0x30: return ']';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x2C: return ' ';   // Space
    default: return -1;
    }
}

// 渲染生成文本到输出区 (UTF-8 自动换行, 保留 TUI)
static void render_output_text(const char *out, int ob) {
    // 过滤 <think>...</think> 思考段 (键盘/JSON 路径不走 stream_token_cb, 需在此统一过滤).
    // 未闭合兜底: 思考段字符超 THINK_MAX_CHARS 强制退出 (模型可能未输出闭合标签).
    #define THINK_MAX_CHARS 120
    static char filtered[2048];
    int fo = 0;
    bool in_think = false;
    int think_chars = 0;
    for (int i = 0; i < ob && fo < (int)sizeof(filtered) - 1; i++) {
        if (!in_think) {
            if (out[i] == '<' && i + 5 < ob && memcmp(out + i, "<think", 6) == 0) {
                in_think = true; think_chars = 0; i += 5; continue;
            }
            filtered[fo++] = out[i];
        } else {
            // 匹配完整 </think> (8字节含 >), 防 > 残留
            if (out[i] == '<' && i + 7 < ob && memcmp(out + i, "</think>", 8) == 0) {
                in_think = false; think_chars = 0; i += 7; continue;
            }
            if (++think_chars > THINK_MAX_CHARS) {
                in_think = false; think_chars = 0;   // 未闭合兜底: 显示后续回答
            }
        }
    }
    filtered[fo] = 0;

    int x = TEXT_LEFT, y = OUT_Y;
    const unsigned char *p = (const unsigned char *)filtered;
    int col = 0;
    char line[80];
    int lp = 0;
    while (*p && y + UI_ROW_H <= OUT_BOT) {
        if (*p == '\n' || col >= 26) {
            line[lp] = 0;
            ui_text(g_display, x, y, line);
            y += UI_ROW_H;
            col = 0; lp = 0;
            if (*p == '\n') { p++; continue; }
        }
        int clen = ((*p & 0xE0) == 0xC0) ? 2 : ((*p & 0xF0) == 0xE0) ? 3 : 1;
        for (int i = 0; i < clen && *p; i++) line[lp++] = *p++;
        col += (clen == 1) ? 1 : 1;
    }
    if (lp > 0 && y + UI_ROW_H <= OUT_BOT) {
        line[lp] = 0;
        ui_text(g_display, x, y, line);
    }
    g_display->RLCD_Display();
}

// ---- 流式渲染: 每生成一个 token 追加显示到输出区 ---------------------------
// 处理 UTF-8 多字节字符: 不足一字的字节先缓存, 完整后画字符.
static char s_pend[4];
static int s_pend_n = 0;

static void stream_draw_char(int cp) {
    // 画单个字符到当前光标, 处理换行
    if (cp == '\n') { s_out_x = TEXT_LEFT; s_out_y += UI_ROW_H; s_out_col = 0; return; }
    if (cp >= 32 && cp < 0x80) {
        if (s_out_col >= 26 || s_out_x + UI_CW > TEXT_RIGHT) { s_out_x = TEXT_LEFT; s_out_y += UI_ROW_H; s_out_col = 0; }
        if (s_out_y + UI_ROW_H > OUT_BOT) { s_out_y = OUT_Y; s_out_x = TEXT_LEFT; s_out_col = 0; }
        ui_draw_char(g_display, s_out_x, s_out_y + (UI_ROW_H - UI_CH) / 2, (unsigned char)cp);
        s_out_x += UI_CW; s_out_col++;
    } else if (cp >= 0x80) {
        if (s_out_col >= 26 || s_out_x + UI_CJK_W > TEXT_RIGHT) { s_out_x = TEXT_LEFT; s_out_y += UI_ROW_H; s_out_col = 0; }
        if (s_out_y + UI_ROW_H > OUT_BOT) { s_out_y = OUT_Y; s_out_x = TEXT_LEFT; s_out_col = 0; }
        ui_draw_cjk(g_display, s_out_x, s_out_y, cp);
        s_out_x += UI_CJK_W; s_out_col++;
    }
}

// llm_token_cb_t: 追加 UTF-8 token 到输出区 (自动循环流式显示)
// 完全隐藏 <think>...</think> 思考段 (标签+内容均不渲染).
// 未闭合兜底: 思考段 token 超 THINK_MAX_TOKS 强制退出 (模型可能未输出闭合标签).
#define THINK_MAX_TOKS 30
static bool s_think_mode = false;
static int  s_think_toks = 0;
static char s_think_buf[10];   // 跨 token 匹配 <think/</think 前缀
static int  s_think_bn = 0;

// 返回 1=进入think段, -1=退出think段, 0=非标签 (匹配 "<think"/"</think" 前缀, 防部分标签泄漏)
static int think_tag(const char *utf8, int len) {
    for (int i = 0; i < len; i++) {
        if (s_think_bn >= (int)sizeof(s_think_buf)) s_think_bn = 0;
        s_think_buf[s_think_bn++] = utf8[i];
        if (s_think_bn >= 8 && memcmp(s_think_buf + s_think_bn - 8, "</think>", 8) == 0) {
            s_think_bn = 0; return -1;
        }
        if (s_think_bn >= 6 && memcmp(s_think_buf + s_think_bn - 6, "<think", 6) == 0) {
            s_think_bn = 0; return 1;
        }
        // 窗口限 9 字节: 保留最近 9 字节 (bn 同步更新, 防大 token 截断标签)
        if (s_think_bn > 9) {
            memmove(s_think_buf, s_think_buf + s_think_bn - 9, 9);
            s_think_bn = 9;
        }
    }
    return 0;
}

static void stream_token_cb(const char *utf8, int len, void *ctx) {
    (void)ctx;
    int tg = think_tag(utf8, len);
    if (tg == 1) { s_think_mode = true; s_think_toks = 0; return; }
    if (tg == -1) { s_think_mode = false; s_think_toks = 0; return; }
    if (s_think_mode) {
        if (++s_think_toks > THINK_MAX_TOKS) {
            s_think_mode = false; s_think_toks = 0;   // 未闭合兜底: 显示后续回答
        } else {
            return;                                    // 隐藏思考内容
        }
    }
    for (int i = 0; i < len; i++) {
        // 防止 s_pend[4] 越界: 若缓冲满但非完整字符, 直接按字节丢弃
        if (s_pend_n >= 4) {
            s_pend_n = 0;
        }
        s_pend[s_pend_n++] = utf8[i];
        // 判断是否凑齐一个完整 UTF-8 字符
        int need;
        unsigned char b0 = (unsigned char)s_pend[0];
        if (b0 < 0x80) need = 1;
        else if ((b0 & 0xE0) == 0xC0) need = 2;
        else if ((b0 & 0xF0) == 0xE0) need = 3;
        else need = 4;
        if (s_pend_n >= need) {
            int clen = s_pend_n;
            int cp = ui_utf8_decode((const unsigned char *)s_pend, clen, &clen);
            if (cp >= 0) stream_draw_char(cp);
            s_pend_n = 0;
        }
    }
    // 实时 footer: 每 token 更新 tok/s (基于当前推理耗时)
    g_stream_tok++;
    int64_t now = esp_timer_get_time();
    int el = (int)((now - g_stream_t0) / 1000000);
    float ts = el > 0 ? (float)g_stream_tok / el : 0.0f;
    int mp = g_stream_tok > 0 ? (int)((now - g_stream_t0) / 1000 / g_stream_tok) : 0;
    ui_draw_footer(ts, mp, g_stream_tok, el);
}

// ---- RAG 证据注入 (设备端 BPE 编码) ----------------------------------------
// 检索证据 → bpe_encode → 组装 ChatML (系统+证据+问题+assistant).
// 返回完整 prompt token ids 数 (<= max_ids). 无 RAG 时退回纯问题.
// 注意: 与 prompt_encoder.c PE_SYSTEM_PROMPT 字节级一致.
static const char RAG_SYSTEM_PROMPT[] = "你是一个医学助手，请根据提供的参考资料准确回答问题。";

static int build_rag_prompt(const char *question_text, int *ids, int max_ids) {
    if (max_ids < 16) return 0;

    // 1. 检索证据 (SD RAG, 若就绪)
    char evidence[RAGSD_DOC_CAP * RAGSD_MAX_DOCS + 8] = "";
    bool has_ev = false;
    if (rag_retrieval_ready() && question_text && *question_text) {
        int elen = rag_retrieval_retrieve(question_text, evidence, sizeof(evidence) - 1);
        if (elen > 0) { evidence[elen] = 0; has_ev = true; }
    }

    // 2. BPE 编码各段 (系统/证据/问题/assistant)
    int n = 0;

    // <im_start>system\n{system}<im_end>\n
    ids[n++] = 1;
    n += bpe_encode("system\n", ids + n, max_ids - n);
    n += bpe_encode(RAG_SYSTEM_PROMPT, ids + n, max_ids - n);
    ids[n++] = 2;
    n += bpe_encode("\n", ids + n, max_ids - n);

    // <im_start>user\n
    ids[n++] = 1;
    n += bpe_encode("user\n", ids + n, max_ids - n);

    // 证据注入: "参考资料：\n{evidence}\n\n"
    if (has_ev) {
        n += bpe_encode("参考资料：\n", ids + n, max_ids - n);
        n += bpe_encode(evidence, ids + n, max_ids - n);
        n += bpe_encode("\n\n", ids + n, max_ids - n);
    }

    // {q}<im_end>\n  (MiniMind SFT 格式: user 消息直接内容, 无"问题："前缀)
    n += bpe_encode(question_text, ids + n, max_ids - n);
    ids[n++] = 2;
    n += bpe_encode("\n", ids + n, max_ids - n);

    // <im_start>assistant\n
    ids[n++] = 1;
    n += bpe_encode("assistant\n", ids + n, max_ids - n);

    ESP_LOGI(TAG, "RAG prompt: %d tok, evidence=%s", n, has_ev ? "YES" : "NO");
    return n;
}

// 自动循环执行一个预设 (流式显示)
static void auto_run_preset(int idx) {
    if (idx >= KBD_PRESET_COUNT) idx = 0;
    g_preset_idx = idx;
    g_kbd_mode = KBD_MODE_PRESET;
    g_generating = true;

    // 重置流式渲染状态 (关键: 防止跨推理残留导致崩溃/错位)
    s_pend_n = 0;
    s_out_x = TEXT_LEFT; s_out_y = OUT_Y; s_out_col = 0;
    g_stream_tok = 0;
    g_stream_t0 = esp_timer_get_time();

    // 更新输入区显示当前预设 + 清输出区
    ui_render_input();
    ui_clear_rect(g_display, TEXT_LEFT, OUT_Y, TEXT_RIGHT, OUT_BOT);
    g_display->RLCD_Display();
    ESP_LOGI(TAG, "auto: preset[%d] %s", idx, kbd_presets[idx].text);

    // 构建 RAG prompt (检索证据 + BPE 编码) 或回退预设 ids
    const KbdPreset *p = &kbd_presets[idx];
    static int ids[PE_MAX_PROMPT + 8];
    int prompt_len;
    if (rag_retrieval_ready()) {
        prompt_len = build_rag_prompt(p->text, ids, PE_MAX_PROMPT + 8);
        if (prompt_len <= 0) {
            for (int i = 0; i < p->len && i < PE_MAX_PROMPT + 8; i++) ids[i] = p->ids[i];
            prompt_len = p->len;
        }
    } else {
        for (int i = 0; i < p->len && i < PE_MAX_PROMPT + 8; i++) ids[i] = p->ids[i];
        prompt_len = p->len;
    }
    int64_t t0 = esp_timer_get_time();
    int n = llm_engine_generate_stream(ids, prompt_len, stream_token_cb, NULL, 120);
    int64_t t1 = esp_timer_get_time();
    int secs = (int)((t1 - t0) / 1000000);
    float tok_s = secs > 0 ? (float)n / secs : 0.0f;
    int ms_per_tok = n > 0 ? (int)((t1 - t0) / 1000 / n) : 0;
    ui_draw_footer(tok_s, ms_per_tok, n, secs);   // 最终统计 (覆盖实时值)
    ESP_LOGI(TAG, "auto: done %d tokens in %ds", n, secs);
    g_generating = false;
    g_last_activity = esp_timer_get_time();  // 完成也算活动, 控制间隔
}

static void kbd_run_inference(const int *ids, int len) {
    g_generating = true;
    // 清空输出区 (保留边框/TUI)
    ui_clear_rect(g_display, TEXT_LEFT, OUT_Y, TEXT_RIGHT, OUT_BOT);
    g_display->RLCD_Display();

    int64_t t0 = esp_timer_get_time();
    static char out[1024];   // 静态: 避免 main task 栈溢出
    int ob = llm_engine_generate(ids, len, out, sizeof(out), 120);
    int64_t t1 = esp_timer_get_time();
    out[ob] = 0;

    render_output_text(out, ob);
    printf("{\"done\":true,\"tokens\":%d}\n", ob);
    ESP_LOGI(TAG, "generated %d chars", ob);
    // footer 统计: tok/s = 生成 token / 总耗时 (含 prefill)
    int secs = (int)((t1 - t0) / 1000000);
    float tok_s = secs > 0 ? (float)ob / secs : 0.0f;
    int ms_per_tok = ob > 0 ? (int)((t1 - t0) / 1000 / ob) : 0;
    ui_draw_footer(tok_s, ms_per_tok, ob, secs);
    g_generating = false;
}

static void kbd_enter_pressed(void) {
    if (g_generating) return;
    int ids[PE_MAX_PROMPT + 8];
    int len = 0;

    if (g_kbd_mode == KBD_MODE_PRESET) {
        // 预设: 用 RAG 证据注入 (若 SD RAG 就绪) 或预烘焙 ids
        const KbdPreset *p = &kbd_presets[g_preset_idx];
        ESP_LOGI(TAG, "preset[%d]: %s", g_preset_idx, p->text);
        if (rag_retrieval_ready()) {
            len = build_rag_prompt(p->text, ids, PE_MAX_PROMPT + 8);
            if (len <= 0) {
                for (int i = 0; i < p->len && i < PE_MAX_PROMPT + 8; i++) ids[i] = p->ids[i];
                len = p->len;
            }
        } else {
            for (int i = 0; i < p->len && i < PE_MAX_PROMPT + 8; i++) ids[i] = p->ids[i];
            len = p->len;
        }
    } else {
        // 自由输入: 编码 + ChatML
        if (g_input_len == 0) { ESP_LOGW(TAG, "empty input"); return; }
        g_input_buf[g_input_len] = 0;
        len = pe_prompt_from_text(g_input_buf, ids, PE_MAX_PROMPT + 8);
        ESP_LOGI(TAG, "free text: %s (%d tok)", g_input_buf, len);
        if (len <= 0) return;
    }
    kbd_run_inference(ids, len);
}

// --- BLE keyboard callback (keycode + modifier) ---------------------------
void board_key_cb(uint8_t keycode, uint8_t modifier) {
    ESP_LOGI(TAG, "BLE key: 0x%02x mod=0x%02x", keycode, modifier);
    if (g_generating) return;

    switch (keycode) {
    case 0x28:  // Enter — run inference
        kbd_enter_pressed();
        break;
    case 0x29:  // Esc — clear / toggle
        if (g_kbd_mode == KBD_MODE_PRESET) {
            g_preset_idx = 0;
        } else {
            g_input_len = 0;
        }
        ui_render_input();
        break;
    case 0x2B:  // Tab — switch mode
        g_kbd_mode = (g_kbd_mode == KBD_MODE_PRESET) ? KBD_MODE_TEXT : KBD_MODE_PRESET;
        ESP_LOGI(TAG, "mode -> %s", g_kbd_mode == KBD_MODE_PRESET ? "PRESET" : "TEXT");
        ui_draw_header();
        ui_render_input();
        break;
    case 0x52:  // Up — prev preset
        if (g_kbd_mode == KBD_MODE_PRESET) {
            g_preset_idx = (g_preset_idx + KBD_PRESET_COUNT - 1) % KBD_PRESET_COUNT;
            ui_render_input();
        }
        break;
    case 0x51:  // Down — next preset
        if (g_kbd_mode == KBD_MODE_PRESET) {
            g_preset_idx = (g_preset_idx + 1) % KBD_PRESET_COUNT;
            ui_render_input();
        }
        break;
    case 0x2A:  // Backspace
        if (g_kbd_mode == KBD_MODE_TEXT && g_input_len > 0) {
            g_input_len--;
            ui_render_input();
        }
        break;
    default: {
        // ASCII 输入 (TEXT 模式)
        if (g_kbd_mode == KBD_MODE_TEXT) {
            int c = hid_keycode_to_ascii(keycode, modifier);
            if (c >= 32 && c < 127 && g_input_len < (int)sizeof(g_input_buf) - 1) {
                g_input_buf[g_input_len++] = (char)c;
                ui_render_input();
            }
        }
        break;
    }
    }
}

// ---- serial screenshot (PBM P4 -> base64) — 与 Arduino v5 协议一致 ---------
//   PC -> MCU: "SHOOT\n"
//   MCU -> PC: "SCREENSHOT_START\n" <base64 PBM 72-char 行> "SCREENSHOT_END\n"
static void take_screenshot(void) {
  if (!g_display) return;
  int w = g_display->GetWidth();
  int h = g_display->GetHeight();
  int row_bytes = (w + 7) / 8;                    // 50 for 400px
  char hdr[24];
  int hdr_len = snprintf(hdr, sizeof(hdr), "P4\n%d %d\n", w, h);
  int pbm_size = hdr_len + row_bytes * h;         // 13 + 15000 = 15013

  uint8_t *pbm = (uint8_t *)malloc(pbm_size);
  if (!pbm) { printf("SCREENSHOT_ERROR: out of memory\n"); return; }
  memcpy(pbm, hdr, hdr_len);

  uint8_t *pdata = pbm + hdr_len;
  for (int y = 0; y < h; y++) {
    for (int bx = 0; bx < row_bytes; bx++) {
      uint8_t byte = 0;
      for (int b = 0; b < 8; b++) {
        int x = bx * 8 + b;
        if (x >= w) break;
        if (g_display->GetPixel(x, y) == ColorBlack) byte |= (0x80 >> b);
      }
      *pdata++ = byte;
    }
  }

  size_t b64_len = 0;
  mbedtls_base64_encode(NULL, 0, &b64_len, pbm, pbm_size);
  uint8_t *b64 = (uint8_t *)malloc(b64_len + 1);
  if (!b64) { free(pbm); printf("SCREENSHOT_ERROR: base64 alloc\n"); return; }
  mbedtls_base64_encode(b64, b64_len, &b64_len, pbm, pbm_size);
  b64[b64_len] = '\0';

  printf("SCREENSHOT_START\n");
  const int chunk = 72;
  for (size_t i = 0; i < b64_len; i += chunk) {
    int remain = (int)b64_len - (int)i;
    int len = (remain < chunk) ? remain : chunk;
    // USB-Serial-JTAG 分块写 (逐字节写 20K 太慢, 会阻塞数分钟)
    usb_serial_jtag_write_bytes(&b64[i], len, pdMS_TO_TICKS(500));
  }
  usb_serial_jtag_write_bytes((const uint8_t *)"\n", 1, pdMS_TO_TICKS(100));
  printf("SCREENSHOT_END\n");
  free(b64);
  free(pbm);
}

// ---- COM → SD 文件传输 (XFER) ----------------------------------------------
// 协议:
//   PC → MCU: "XFER <filename> <size>\n"   (filename 不含路径, 写入 /sdcard/rag/)
//   MCU → PC: "XFER_OK\n"                  (就绪, 开始发数据)
//   PC → MCU: 256B 块 (原始二进制) — 匹配 USB-Serial-JTAG 单次传输上限
//   MCU → PC: "XFER_ACK_<n>\n"    (每块 ACK, 序号递增, 流控)
//   PC → MCU: 剩余字节 (发完全部 size 字节)
//   MCU → PC: "XFER_DONE\n"       (写完关闭)
// 安全: 文件名白名单 (仅 index/docs/meta), 防路径穿越.
#define XFER_CHUNK 256

static void do_xfer(char *args) {
    if (!sd_ready()) { printf("ERR: no SD\n"); return; }
    // 解析 "<filename> <size>"
    char fname[32];
    long fsize = 0;
    int n = sscanf(args, "%31s %ld", fname, &fsize);
    if (n != 2 || fsize <= 0 || fsize > (64L * 1024 * 1024)) {
        printf("ERR: bad xfer args\n");
        return;
    }
    // 文件名白名单 (防路径穿越): 仅允许 rag 索引文件
    if (strcmp(fname, "index.bin") != 0 && strcmp(fname, "docs.bin") != 0 &&
        strcmp(fname, "meta.bin") != 0 && strcmp(fname, "term_overlay.bin") != 0) {
        printf("ERR: filename not allowed\n");
        return;
    }
    // 就绪信号
    printf("XFER_OK\n");

    char path[64];
    snprintf(path, sizeof(path), "/sdcard/rag/%s", fname);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("ERR: open failed\n"); return; }

    // 8KB 内部 SRAM 缓冲 (DMA 友好, 避免 PSRAM 直写)
    uint8_t *buf = (uint8_t *)heap_caps_malloc(XFER_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) { fclose(f); printf("ERR: no buf\n"); return; }

    long remaining = fsize;
    int ok = 1;
    int chunk_n = 0;
    while (remaining > 0) {
        size_t want = remaining < XFER_CHUNK ? remaining : XFER_CHUNK;
        size_t got = 0;
        int idle = 0;
        while (got < want) {
            // 请求剩余全部 — USB-Serial-JTAG 单次最多 ~256B, 循环累积
            int r = usb_serial_jtag_read_bytes(buf + got, want - got,
                                               pdMS_TO_TICKS(200));
            if (r <= 0) {
                if (++idle > 50) {   // 10s 无数据则放弃
                    ESP_LOGE(TAG, "xfer read timeout chunk=%d got=%d want=%d",
                             chunk_n, (int)got, (int)want);
                    ok = 0;
                    break;
                }
                continue;
            }
            got += r;
            idle = 0;
        }
        if (!ok) break;
        if (fwrite(buf, 1, want, f) != want) {
            printf("ERR: write failed\n");
            ok = 0;
            break;
        }
        remaining -= want;
        chunk_n++;
        // 独特 ACK 带序号 — 防二进制数据流中的 "OK" 误判 (PC 按行匹配前缀)
        printf("XFER_ACK_%d\n", chunk_n);
        ESP_LOGI(TAG, "xfer chunk %d: %d bytes (%ld left)", chunk_n,
                 (int)want, remaining);
    }
    fclose(f);
    free(buf);
    if (ok) {
        printf("XFER_DONE\n");
        ESP_LOGI(TAG, "xfer %s: %ld bytes written", fname, fsize);
    }
    // 刷新活动时间 — 防止 XFER 长阻塞后 auto 立即启动吞掉下一条命令
    g_last_activity = esp_timer_get_time();
}

// ---- UART JSON prompt (保留 COM 输入兼容) --------------------------------
static void handle_json_prompt(char *json) {
    ESP_LOGI(TAG, "HJP-ENTER buf=[%.60s]", json);
    const char *p = strstr(json, "\"ids\":[");
    if (!p) return;
    p += 7;
    int ids[512], n = 0;
    while (*p && *p != ']' && n < 512) {
        if (*p == ' ' || *p == ',') { p++; continue; }
        ids[n++] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
    }
    if (n == 0) return;
    int max = 200;
    p = strstr(json, "\"max\":");
    if (p) { p += 6; while (*p == ' ') p++; int m = atoi(p); if (m > 0 && m <= 128) max = m; }

    // 清输出区 (保留 TUI 边框/header/footer)
    if (g_display) {
        ui_clear_rect(g_display, TEXT_LEFT, OUT_Y, TEXT_RIGHT, OUT_BOT);
        g_display->RLCD_Display();
    }

    static char out[2048];   // 静态: 避免 main task 栈溢出
    int64_t t0 = esp_timer_get_time();
    int ob = llm_engine_generate(ids, n, out, sizeof(out), max);
    int64_t t1 = esp_timer_get_time();
    out[ob] = 0;
    render_output_text(out, ob);
    printf("{\"done\":true,\"tokens\":%d}\n", ob);
    ESP_LOGI(TAG, "generated %d chars", ob);
    // footer 统计
    int secs = (int)((t1 - t0) / 1000000);
    float tok_s = secs > 0 ? (float)ob / secs : 0.0f;
    int ms_per_tok = ob > 0 ? (int)((t1 - t0) / 1000 / ob) : 0;
    ui_draw_footer(tok_s, ms_per_tok, ob, secs);
}

void board_init(void) {
    ESP_LOGI(TAG, "board init (RLCD-4.2)");

    // COM 口实际是 Espressif USB-Serial-JTAG (VID_303A PID_1001), 非 UART0 桥.
    // RX 必须从 usb_serial_jtag 读 (uart_read_bytes(UART_NUM_0) 收不到 USB 数据).
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_rc = usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "usb_serial_jtag_driver_install rc=%d (%s)", usj_rc, esp_err_to_name(usj_rc));

    // LCD display (ST7305 reflective panel, 400x300)
    g_display = new DisplayPort(LCD_MOSI, LCD_SCL, LCD_DC, LCD_CS, LCD_RST, 400, 300);
    if (g_display) {
        g_display->RLCD_Init();
        g_display->RLCD_ColorClear(ColorWhite);
        g_display->RLCD_Display();
        ESP_LOGI(TAG, "LCD ready (400x300 ST7305)");
    } else {
        ESP_LOGE(TAG, "LCD init failed");
    }

    // BLE keyboard
    keyboard_ble_on_key(board_key_cb);

    // 初始 UI: TUI 边框 + header + 预设菜单 + footer 占位
    ui_draw_frame();
    ui_draw_header();
    ui_render_input();
    ui_draw_footer(0, 0, 0, 0);
    g_last_activity = esp_timer_get_time();   // 自动循环计时起点
    ESP_LOGI(TAG, "board ready — keyboard UI: [Tab] preset/text, [Up/Dn] nav, [Enter] run");
    ESP_LOGI(TAG, "auto-demo: %ds idle -> loop presets (streaming)", AUTO_IDLE_MS / 1000);
    btns_init();   // 板载按键 (BOOT=BTSCAN, KEY=preset next)
}

void board_loop(void) {
    // RX 走 USB-Serial-JTAG (COM 口是原生 USB 枚举, 非 UART0 GPIO44)
    int loop_count = 0;
    int64_t last_clock = 0;
    while (1) {
        if ((++loop_count % 5000) == 0) ESP_LOGI(TAG, "loop heartbeat %d", loop_count);

        // 板载按键事件处理 (ISR 捕获, 推理间隙执行): BOOT=BTSCAN, KEY=preset next+run
        btns_handle();

        // 时钟: 每秒刷新底部时间 + BT 状态 (扫描/配对中实时更新)
        int64_t now = esp_timer_get_time();
        if (now - last_clock > 1000000) {
            last_clock = now;
            ui_draw_clock(FTR_Y1 + 2);
            ui_draw_header();   // BT:SCAN/PAIR/ON/OFF 实时刷新
        }

        // 自动循环调度: 无输入空闲超时 -> 执行下一个预设
        int64_t now2 = esp_timer_get_time();
        if (!g_generating && !g_auto_mode &&
            now2 - g_last_activity > AUTO_IDLE_MS * 1000) {
            g_auto_mode = true;
            g_auto_idx = 0;
            ESP_LOGI(TAG, "auto mode start (idle %ds)", AUTO_IDLE_MS / 1000);
        }
        if (g_auto_mode && !g_generating) {
            if (now - g_last_activity > AUTO_NEXT_MS * 1000) {
                auto_run_preset(g_auto_idx);
                g_auto_idx = (g_auto_idx + 1) % KBD_PRESET_COUNT;
            }
        }

        uint8_t c;
        int r = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
        if (r == 1) {
            if (c == '\n') {
                line_buf[line_pos] = 0;
                line_pos = 0;
                if (line_buf[0] == '{') {
                    g_auto_mode = false;   // 用户输入打断自动循环
                    g_last_activity = esp_timer_get_time();
                    handle_json_prompt(line_buf);
                } else if (strcmp(line_buf, "BTSCAN") == 0) {
                    g_last_activity = esp_timer_get_time();
                    keyboard_ble_scan();
                } else if (strcmp(line_buf, "SHOOT") == 0) {
                    take_screenshot();
                } else if (strncmp(line_buf, "KEY ", 4) == 0) {
                    // KEY <hex> — 模拟键盘按键 (0x51=Down, 0x52=Up, 0x28=Enter, 0x2B=Tab)
                    uint8_t kc = (uint8_t)strtol(line_buf + 4, NULL, 16);
                    ESP_LOGI(TAG, "KEY sim 0x%02x", kc);
                    g_auto_mode = false;   // 用户按键打断自动循环
                    g_last_activity = esp_timer_get_time();
                    board_key_cb(kc, 0);
                } else if (strncmp(line_buf, "XFER ", 5) == 0) {
                    // XFER <filename> <size> — COM → SD 文件传输
                    g_auto_mode = false;
                    g_last_activity = esp_timer_get_time();
                    do_xfer(line_buf + 5);
                } else if (strncmp(line_buf, "RAGQ ", 5) == 0) {
                    // RAGQ <问题> — 调试: 打印设备端检索证据 (验证索引质量)
                    g_auto_mode = false;
                    g_last_activity = esp_timer_get_time();
                    char ev[256];
                    int rn = rag_retrieval_retrieve(line_buf + 5, ev, sizeof(ev));
                    ESP_LOGI(TAG, "RAGQ [%s] -> %d docs: %.120s", line_buf + 5, rn, ev);
                }
            } else if (c != '\r' && line_pos < LINE_BUF - 1) {
                line_buf[line_pos++] = (char)c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
