# esp32_llm_v5_idf — V5 PLE LLM 固件（ESP-IDF 移植版）

> 状态: 编译通过 (esp32_llm_v5.bin 264KB) | IDF v5.5 | 2026-08-04
> 目标: 将 V5 (MiniMind H1/H2) 从 Arduino 框架移植到 ESP-IDF

## 为什么移植

| 维度 | Arduino (esp32_llm_zh_v5) | **ESP-IDF (本目录)** |
|---|---|---|
| 编译 | arduino-cli libsdetect 死锁 | idf.py CMake, 无死锁 |
| 固件大小 | 1385KB | **264KB** (-5x, 无 Arduino 框架) |
| BLE 键盘 | 需 NimBLE 手动集成 | esp_hid 原生组件 |
| 内存控制 | 受限 | heap_caps 精细控制 |
| 增量构建 | manual_compile.py 手动 | cmake 自动 |

## 目录结构

```
firmware/esp32_llm_v5_idf/
  CMakeLists.txt          # 工程级 (PROJECT_VER)
  sdkconfig.defaults      # esp32s3 + BLE/NimBLE + -Os
  build.bat               # 一键编译
  partitions/partitions.csv  # model 14.50MB (H2), 无 kb
  main/
    app_main.c            # 入口: NVS + model load + board + BLE
    CMakeLists.txt        # 组件定义 + REQUIRES
    core/
      llm_engine.c/h      # 推理引擎 (llm_v5.h 封装 + 采样)
      rag_retrieval.c/h   # RAG (SD-only deep search)
      llm_v5.h            # 纯 C 推理核心 (零改动, 从 esp32_llm_zh_v5 复制)
      vocab.h             # MiniMind BPE 6400 (零改动)
      rag.h / rag_sd.h    # RAG 检索 (零改动)
      cjk_font.h          # 显示字库 (零改动)
    boards/
      board_rlcd.c/h      # UART 命令分发 + 板初始化
      display_bsp.cpp/h   # LCD 驱动 (已是 ESP-IDF API)
      keyboard_ble.c/h    # BLE 键盘 (esp_hidh 骨架)
```

## 构建

```powershell
# 方式 1: 一键
firmware\esp32_llm_v5_idf\build.bat

# 方式 2: 手动 (需 IDF 环境)
$env:IDF_PATH = "D:\esp\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\szk220009\.espressif\python_env\idf5.5_py3.11_env"
$env:PATH = "...xtensa-esp-elf\bin;...python_env\Scripts;...cmake\bin;...ninja;" + $env:PATH
idf.py set-target esp32s3   # 首次
idf.py build                # 增量
```

## 关键坑（已解决）

1. **`-Og` ICE**: xtensa gcc 14.2.0 编译 esp_lcd 段错误 → 用 `-Os` (CONFIG_COMPILER_OPTIMIZATION_SIZE)
2. **组件名**: IDF v5.5 用 `fatfs` (非 esp_vfs_fat), `esp_mm` (非 esp_heap_caps)
3. **esp_hidh 类型**: 事件数据是 `esp_hidh_event_data_t` (union), 非 `esp_hidh_event_t` (enum)

## BLE 键盘 (已移植, 编译通过)

`keyboard_ble.c` 移植 xiaozhi-esp32 bluetooth_keyboard (C++→C), 升级自 breezy_bt (MIT):
- esp_hidh 初始化 + HID 报告事件回调
- 两段式 BTSCAN: 第 1 次扫描保存 pending 键盘, 第 2 次连接
- **NVS 持久化**: 键盘地址存 NVS (`bt_kbd/addr`), 重启后 boot timer 自动重连
- **后台扫描重连**: 断开后自动后台扫描已存键盘 (地址匹配模式)
- **Protocol Mode 兜底**: 检测键盘缺 Protocol Mode 特征时回退 BLE_GAP_EVENT_NOTIFY_RX (esp_hidh 否则静默丢通知)
- **双条件过滤**: appearance 0x03C1 **或** HID Service UUID 0x1812
- **安全配置**: Just Works + Secure Connections + bonding (key dist 双向)
- **连接参数优化**: 7.5-15ms interval, 1s supervision timeout
- **REPEAT_PAIRING**: 清旧 bond + 重试
- 内存预检 (internal < 15000B 中止) + stale bond 清理 (防泄漏)
- 按键去重: 仅上报新按下的键 (长按不重复触发)

快捷键映射 (接入 board 层): Enter/Esc/Space/↑↓/R/T/M/V/Tab/数字键
→ 需在 board_rlcd.c 的 key_cb 中实现 (待接)

用法:
```
BTSCAN         # 扫描键盘 (有已存键盘则后台重连, 无则 15s 通用扫描)
BTSCAN         # 第 2 次: 连接 (旧两段式流程, 已兼容)
BTFORGET       # 清除已存键盘 + NimBLE bonds (新增, 需在 board 层接命令)
```

## 烧录

```powershell
# 固件 (0x10000) + 模型 (0x1D0000) + bootloader/partitions
# 注意: factory 扩至 0x1C0000 (FULL 18129 字体), model 后移至 0x1D0000
# 推荐用 tools/flash_v5.ps1 (自动前置校验 + 顺序烧录)
esptool --chip esp32s3 --port COM3 write_flash 0x10000 build/esp32_llm_v5.bin
esptool --chip esp32s3 --port COM3 write_flash 0x1D0000 ..\model_v5\H2\model_llm.bin
```
