# 中文模型项目变更说明 (CHANGELOG)

> 分支: feature/ESP32-S3-4.2inch-RLCD
> 覆盖: chinese/ (v1) + chinese_v2/ (v2) 全部演进

---

## v2 环境（医学数据，独立版本）

### 2026-08-01: 部署验证 + NaN 防护
- ✅ model.bin 7.10MB / PSRAM 3.73MB / vocab 6,594 全部通过部署检查
- ✅ 权重与导出产物 NaN/Inf 清零验证（0/86 tensors）
- ✅ export.py 增加 NaN/Inf 防御性清理
- 🔧 4-bit 量化 group 128→32（SFT 模型敏感，128 生成崩溃）
- 提交: `81e1464` `be5ddb0` `3ba6087`

### 2026-08-01: P2 SFT zh5-med
- 数据: zjydiary finetune 采样 30K（1.95M 池）
- GPU 3000 步 112 秒，val ppl 9.7
- 生成真实医学内容（胃癌症状/甲状腺手术）
- 提交: `3eb3912`

### 2026-08-01: P1 预训练 zh5 + GPU 环境
- 检测 RTX 5080（Blackwell sm_120）→ WSL torch 2.9.1+cu128
- pyproject.toml: torch≥2.11 + pytorch-cu128 源
- zjydiary/Medical 1.97GB 下载（百科 361K + 教材 8.5K）
- zh5 预训练 20000 步 GPU 12.6 分钟，val ppl 12.13
- 提交: `890345d` `1686113`

## v1 环境（OCR 业务文档）

### 2026-07-31: 部署交付
- zh4-ds 12.5M 部署验证 17/17 通过
- firmware/esp32_llm_zh/ 独立固件副本 + 中文 vocab.h
- tokenizer.json 入库（解锁构建）
- 提交: `3a6ac49` `f507478`

### 2026-07-31: 数据扩充 + 纯生成
- DeepSeek 生成 5,394 条高质量 QA（语料段落→QA）
- SFT 数据 1059→7387（deepseek 73% 主源）
- zh4-ds val ppl 8.6
- 提交: `34ca249` `44cef32` `f342a95`

### 2026-07-31: 模型扩增 zh2→zh3→zh4
- 12.5M 参数（core 2.5M），val ppl 7.64
- SFT labels 错位 bug 修复（核心突破）
- 提交: `20652f5` `426ae41`

### 2026-07-30: 中文管线搭建
- 字符级 tokenizer + prepare/train/quantize/export 全链路
- v1 数据: D:\docs\raw OCR 文档
- 提交: `9f0ac1a` `6af4ac3`

---

## 关键里程碑

| 日期 | 事件 | 影响 |
|---|---|---|
| 07-30 | 中文训练管线 v1 搭建 | 首次可训练中文模型 |
| 07-31 | SFT labels 错位修复 | 从"复制崩溃"到真实学习 |
| 07-31 | DeepSeek 纯生成 5K QA | 数据量 7 倍提升 |
| 08-01 | GPU 环境（RTX 5080） | 训练提速 10 倍 |
| 08-01 | v2 医学数据环境 | 文本质量质变（真实医学内容） |
| 08-01 | 4-bit group=32 + NaN 防护 | 部署产物干净可用 |

## 版本对照

| 版本 | 环境 | 数据源 | 参数 | 词表 | 状态 |
|---|---|---|---|---|---|
| v1 zh4-ds | chinese/ | OCR 业务文档 | 12.5M | 5,904 | 部署就绪 |
| v2 zh5-med | chinese_v2/ | 医学百科+教材 | 13.68M | 6,594 | 部署就绪 ✅ |
