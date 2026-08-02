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
| 中文 v1 | 固件 + model.bin(0x170000) |
| 中文 v2 | 固件 + model.bin(0x170000) + **kb 索引(0xA00000)** |

## 生成能力对比

| 维度 | 英文 | 中文 v1 | 中文 v2 |
|---|---|---|---|
| 文本质量 | 故事流畅 | 领域文本 | **医学内容真实** |
| 精准问答 | ❌ | ❌ | ⚠️ RAG 辅助（证据引用） |
| 推理速度 | 9.5 tok/s | ~2 tok/s | ~2 tok/s |
| 知识库 | 无 | 无 | **✅ 1.83MB 设备端** |
