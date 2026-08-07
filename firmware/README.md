# 固件版本说明（三语言模型）

> 分支: feature/ESP32-S3-4.2inch-RLCD
> 三个版本相互隔离，可独立编译烧录

---

## 版本矩阵

| 版本 | 目录 | 模型 | 语言 | 词表 | 模型文件 | RAG | 显示 |
|---|---|---|---|---|---|---|---|
| **英文版** | `firmware/esp32_llm/` | cleandeploy 28.9M | 英文故事 | 32,768 BPE | `firmware/model/model.bin` (14.9MB) | ❌ | ✅ TFT/RLCD |
| **中文 v1** | `firmware/esp32_llm_zh/` | zh4-ds 12.5M | 中文 | 5,904 字符 | `firmware/model_chinese/model.bin` (6.3MB) | ❌ | ✅ + CJK |
| **中文 v2** | `firmware/esp32_llm_zh_v2/` | zh5-multi2/raft 13.7M | 中文医学 | 6,594 字符 | `firmware/model_v2/model.bin` (7.5MB) | ✅ | ✅ + CJK |
| **中文 v3** | `firmware/esp32_llm_zh_v3/` | zh6-raft 15.8M | 中文医学 | 7,563 字符 | `firmware/model_v3/model.bin` (8.92MB) | ✅ | ✅ + CJK |
| **中文 v4** | `firmware/esp32_llm_zh_v3/` | zh7-raft 16.4M | 中文医学+指南 | **8,196 字符** | `firmware/model_v4/model.bin` (8.81MB) | ✅ deep | ✅ + CJK |
| **中文 v5** | `firmware/esp32_llm_zh_v5/` | MiniMind H2 24.95M | 外部 PLE | **6,400 BPE** | `firmware/model_v5/H2/model_llm.bin` (14.05MB) | ✅ deep | ✅ + CJK |

> v3 = 蒸馏版本（Qwen3-0.6B 数据蒸馏 + RAFT 证据复述），详见 `chinese_v3/docs/PLAN.md`
> v3 固件复用 v2 分区表（model 0x170000 8.98MB 放 8.92MB model.bin，kb 0xA00000 2MB），特殊 token 在词表末尾（<user>=N-3, <assistant>=N-2, <end>=N-1）
> **v4** = 在 v3 目录上新增指南语料（vocab 8196），词表由 `data_v4/tokenizer.json` 生成（`gen_vocab.py`）
> **v5** = 独立目录，fork `llm_v5.h`（加 per-head q_norm/k_norm）适配外部 MiniMind H2，模型用 `chinese_v5/convert_h2.py` 从 MiniMind PLE1 转换

---

## 各版本实现

### 1. 英文版（`esp32_llm`）— 原始基线

```
firmware/esp32_llm/
  esp32_llm.ino      # 推理主逻辑（串口 prompt + 显示）
  vocab.h            # 英文 BPE 32768 → 25353 实际 token
  display.h          # TFT/RLCD 显示
  display_bsp.cpp/h  # 显示 BSP
  partitions.csv     # factory + model + coredump
```

- 模型: cleandeploy 28.9M（core 558K + table 25.2M）
- 生成: 英文 TinyStories 故事，9.5 tok/s
- 显示: 英文/ASCII + 图形

### 2. 中文 v1（`esp32_llm_zh`）— 中文 OCR 语料

```
firmware/esp32_llm_zh/
  esp32_llm_zh.ino   # 中文推理 + 串口 prompt
  vocab.h            # 字符级 5904（含 SFT 标记）
  cjk_font.h         # 中文点阵字体（显示）
  display.h          # RLCD 显示（ST7305）
  display_bsp.cpp/h
  partitions.csv     # 3 分区（无 kb）
```

- 模型: zh4-ds 12.5M（core 1.5M + table 4.5M），词表 5904
- 生成: 中文领域文本（OCR 业务文档训练）
- 数据: D:\docs\raw（保险/法规/医疗 OCR）
- 显示: 中文点阵（2x 放大）

### 3. 中文 v2（`esp32_llm_zh_v2`）— 中文医学 + RAG ★

```
firmware/esp32_llm_zh_v2/
  esp32_llm_zh_v2.ino   # 中文医学推理 + RAG 增强
  vocab.h               # 字符级 6594（含 SFT 标记）
  rag.h                 # 设备端 TF-IDF 检索器（0.6ms）
  cjk_font.h            # 中文点阵字体
  display.h / display_bsp
  partitions.csv        # 4 分区：factory + model + kb + coredump
```

- 模型: zh5-multi2 13.7M → **raft 微调版**（证据复述），词表 6594
- 生成: 中文医学内容（百科+教材+30K 指令），引用知识库证据
- 知识库: Huatuo26M-Lite 93.5K → 1.83MB 倒排索引（IDF 加权）
- 数据: 魔搭 zjydiary/Medical（干净医学百科+教材）
- RAG 流程: 问题 → 检索 → 证据注入 prompt → RAFT 模型续写

---

## 共享核心

```
firmware/common/llm.h    # PLE 推理核心（版本无关，读 header 动态配置）
```

三个版本均 `#include "../common/llm.h"`——按 model.bin 的 header（vocab/dim/layers/ple_dim）动态适配，无版本冲突。

---

## 模型产物对照

| 版本 | 模型文件 | 大小 | 词表 | 分区 |
|---|---|---|---|---|
| 英文 | `firmware/model/model.bin` | 14.9MB | 32768 | model 0x170000 |
| 中文 v1 | `firmware/model_chinese/model.bin` | 6.3MB | 5904 | model 0x170000 |
| 中文 v2 | `firmware/model_v2/model.bin` | 7.5MB | 6594 | model 0x170000 + kb 0xA00000 |

## 烧录差异

| 版本 | 烧录内容 |
|---|---|
| 英文 | 固件 + model.bin(0x170000) |
| 中文 v1 | 固件 + model.bin(**0x1D0000**) + **partitions.bin(0x8000)** ← 分区变更 |
| 中文 v2 | 固件 + model.bin(**0x1D0000**) + **partitions.bin(0x8000)** + kb 索引(0xA00000) ← 分区变更 |
| 中文 v3 | 固件 + model.bin(0x170000) + kb 索引(0xA60000) ← 分区不变 |
| 中文 v5 | 固件 + model.bin(0x170000) ← 分区不变 |
| v5_idf | 固件(0x10000) + model_llm.bin(0x170000) ← 分区不变 |

> ⚠️ **v1/v2 分区变更（2026-08-06）**: factory 从 0x160000 扩至 0x1C0000（+400KB）以容纳全量 CJK 字库（18129 字形 / 637KB）。model 起始地址从 0x170000 变为 **0x1D0000**。首次烧录必须先刷 `partitions.bin`，否则分区表与固件不匹配。

## RAG 索引存放位置（v2）

### 📁 源文件（PC 构建产物）

```
D:\codes\esp32-ai\data_v2\kb\index.bin         ← 1.83MB 倒排索引（构建产物）
D:\codes\esp32-ai\data_v2\kb\format_data.jsonl ← Huatuo26M-Lite 原始数据 72.8MB
D:\codes\esp32-ai\data_v2\kb\                  ← 知识库目录
```

### 🎯 烧录位置（设备端）

```
ESP32-S3 Flash 分区布局 (16MB) — v2（2026-08-06 更新）:
┌──────────────────────────────────────────┐
│ nvs       0x9000    (20KB)              │
│ factory   0x10000   (1.75MB 固件+全量字库)│ ← 扩展 +400KB
│ model     0x1D0000  (8.19MB 模型)        │ ← 地址变更!
│ kb        0xA00000  (2MB 索引) ★         │
│ coredump  0xFF0000  (64KB)              │
└──────────────────────────────────────────┘

v1/v3/v5 布局不同，参见各自 partitions.csv
```

### 🔄 设备端使用流程

```powershell
# 烧录时（有 arduino-cli 环境）
esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash 0xA00000 data_v2/kb/index.bin
```

```c
// 运行时 (esp32_llm_zh_v2.ino)
rag_init()
  → esp_partition_find_first("kb", subtype 0x41)
  → esp_partition_mmap(kb 分区)       // flash 内存映射
  → rag.h 从 mmap 地址检索 (0.6ms/10K 文档)
```

### 关键点

| 项 | 位置 |
|---|---|
| 索引源文件 | `data_v2/kb/index.bin`（1.83MB，PC 构建） |
| 烧录分区 | Flash `0xA00000`（kb 分区，2MB） |
| 运行时访问 | **flash mmap**（ESP_PARTITION_MMAP_DATA，不占 PSRAM） |
| 构建脚本 | `chinese/kb/build_index.py` |

> 💡 索引通过 flash mmap 直接读取（像 model.bin 一样），**不占用 PSRAM**——PSRAM 8MB 余量全部留给模型运行。

## 生成能力对比

| 维度 | 英文 | 中文 v1 | 中文 v2 |
|---|---|---|---|
| 文本质量 | 故事流畅 | 领域文本 | **医学内容真实** |
| 精准问答 | ❌ | ❌ | ⚠️ RAG 辅助（证据引用） |
| 推理速度 | 9.5 tok/s | ~2 tok/s | ~2 tok/s |
| 知识库 | 无 | 无 | **✅ 1.83MB 设备端** |

---

## CJK 字体系统

### 字体格式

所有中文固件使用 `cjk_font.h`（14×14 1bpp 点阵字库），格式:
```c
#define CJK_N 18129           // 字形数（FULL 模式）或 7854（GB2312 模式）
static const uint32_t CJK_CP[N];    // Unicode 码点（排序，二分搜索）
static const uint32_t CJK_OFF[N];   // CJK_BLOB 偏移
static const uint8_t  CJK_BLOB[];   // 字形位图（每字 28 字节）
```
渲染: `display.h` 中 `cjk_find()` 二分搜索 + `rlcd_draw_cjk()` 逐像素绘制。零 RAM，全 Flash 直读。

### 两种模式

| 模式 | 字形数 | 大小 | 覆盖 | 适用 |
|---|---|---|---|---|
| **FULL** (`--full`) | 18,129 | 637KB | CJK 全量（含繁体/Ext-A） | v1, v2, v5_idf |
| GB2312+vocab（默认） | 7,854 | 276KB | GB2312 98.7% | v3, v5（分区受限） |

### 重新生成

```bash
# 全量模式（Plan B，18129 字形）
python chinese/gen_cjk_font_cbin.py --full --out firmware/esp32_llm_zh_v2/cjk_font.h

# 默认模式（GB2312 + 模型词表，7854 字形）
python chinese/gen_cjk_font_cbin.py --out firmware/esp32_llm_zh_v3/cjk_font.h

# 限制字形数（适应小分区）
python chinese/gen_cjk_font_cbin.py --full --max-glyphs 13000 --out path/cjk_font.h
```

> 数据来源: xiaozhi-esp32 `font_noto_qwen_14_1.bin`（626KB，18129 字形）
> 详细分析: `docs/FONT_SYSTEM_ANALYSIS.md`
