# Chinese PLE Model Training for ESP32-S3

Train a tiny Transformer model on Chinese insurance / medical / legal documents,
using the same PLE (Per-Layer Embedding) architecture that powers the English
TinyStories model on the ESP32-S3.

## Design

| Aspect | English (original) | Chinese (this) |
|---|---|---|
| Tokenizer | BPE subword (vocab=32768) | **Character-level** (vocab≈5900) |
| Data | TinyStories 300MB (HuggingFace) | D:\docs\raw (13,250 .md, 2.35GB) |
| Data dir | `data/` | `data_chinese/` |
| Runs dir | `runs/` | `runs_chinese/` |
| Training code | `src/train.py` | `chinese/train.py` (wrapper) |
| Model | Same `src/model.py` | Same (changes only `vocab_size`) |
| Quant/Export | `src/quantize.py` + `src/export.py` | `chinese/quantize.py` + `chinese/export.py` |
| Artifact | `firmware/model/` | `firmware/model_chinese/` |

## 完整训练过程（实测记录）

### Step 1 — 语料提取 `chinese/prepare.py`

```
uv run python chinese/prepare.py --source "D:/docs/raw" --vocab-size 8000 --max-chars 100000000
```

流程：
1. **扫描排序**：遍历 D:\docs\raw 全部 13,250 个 .md/.txt，按中文含量降序排列（`_scan_files`）
2. **排除低质量目录**：默认跳过 `DLP` / `DLP案件反馈`（占原始语料 74.7% 的表格数据转储）
3. **行级清洗**（`clean_markdown`）：
   - 剥离 YAML frontmatter、HTML 标签、Markdown 图片/链接/标题
   - 丢弃：OCR 日志、文件路径、时间戳、emoji 行
   - 丢弃：数字密集行（>35%）、管道表格行（≥3 个 `|`）、纯数字/编码序列
4. **流式写盘**：每文件上限 2M 字符（`--max-per-file`），防止单文件垄断；总量上限 100M（`--max-chars`）
5. **训练字符级 tokenizer**（`CharTokenizer`，vocab≈5900，覆盖高频汉字+标点）
6. **分块编码**：5M 字符/块 → memmap 写 `train.bin`（uint16），避免 1.4B 字符全量进内存

**实测结果（排除 DLP 后）**：

| 指标 | 值 |
|---|---|
| 扫描文件 | 10,988 个含中文 |
| 实际使用 | 6,106 个文件 |
| 总字符 | 100M（70.1M 汉字） |
| 词表大小 | 5,901 字符 |
| train.bin | 99M tokens（198 MB） |
| val.bin | 1M tokens（2 MB） |

### Step 2 — 训练 `chinese/train.py`

```
uv run python chinese/train.py --steps 10000 --tag zh2
```

配置（复用 src/model.py 的 PLE 架构）：

| 参数 | 值 |
|---|---|
| 架构 | PLE (Per-Layer Embedding) |
| d_model / layers / heads | 64 / 4 / 4 |
| ffn_hidden（自动求解） | 214 |
| ple_dim | 64 |
| 词表 | 5,901 |
| 参数量 | core 280K + table 1.51M + stream 378K = **2.17M** |
| batch 16 × seq 256 | 4,096 tokens/step |
| 优化器 | AdamW (lr 1e-3, wd 0.1, warmup 100, cosine) |

**实测训练曲线**：

```
Step   Train   Val     PPL
    0   8.68   8.64   5650   随机初始化
  500   4.11   3.58     36   快速收敛
 1000   3.56   3.12     23
 2000   3.26   2.88     18
 5000   3.20   2.66     14
 7500   2.93   2.58     13
10000   2.88   2.54     13   ← 健康收敛，无过拟合
```

- 耗时：10,000 步 ≈ **1,208s（~20 分钟）**，CPU
- 最终 val loss 2.5406 / **ppl 12.69**

### Step 3 — 量化 `chinese/quantize.py`

```
uv run python chinese/quantize.py --tag zh2 --seed 42
```

```
ple fp32 val 2.5890 (ppl 13.32)
ple 4-bit val 2.7089 (ppl 15.01) | deg +0.1199 | quantized 2.2M params
```

4-bit PTQ 退化 +0.12 nats，几乎无损。

### Step 4 — 导出 `chinese/export.py`

```
uv run python chinese/export.py ple-zh2-s42
```

```
wrote firmware/model_chinese/model.bin (1.13 MB)  45 tensors
```

- 格式与英文版完全一致（PLE1 magic, group=128, int4 + fp16 scales）
- 比英文版 14.9MB 小 13 倍（词表 5901 vs 32768）

### Step 5 — 推理验证 `chinese/generate.py`

```
uv run python chinese/generate.py "检查结果" --tag zh2
```

## 版本演进记录

| 版本 | 语料 | 步数 | Val PPL | 说明 |
|---|---|---|---|---|
| v1 (`zh`) | 137 文件 / 15K 汉字（D:\docs\source 误用） | 5,000 | 43.1 | 含 DLP 表格垃圾，过拟合边缘 |
| **v2 (`zh2`)** | **6,106 文件 / 70M 汉字（D:\docs\raw 纯净）** | **10,000** | **12.7** | 排除 DLP + 行级过滤，质量飞跃 |

## Usage

```powershell
# 1. Extract + clean text from D:\docs\raw
uv run python chinese/prepare.py

# 2. Train the model
uv run python chinese/train.py --steps 10000 --tag zh2

# 3. Speed test first (recommended)
uv run python chinese/train.py --steps 200 --eval-every 50 --tag speedtest

# 4. Quantize + export
uv run python chinese/quantize.py --tag zh2 --seed 42
uv run python chinese/export.py ple-zh2-s42

# 5. Generate
uv run python chinese/generate.py "甲状腺切除" --tag zh2
```

## Overrides

```powershell
# Larger model (if data is plentiful)
uv run python chinese/train.py --d-model 96 --n-layers 6 --target-core 560000 --steps 20000

# Custom vocab size
uv run python chinese/prepare.py --vocab-size 6000

# Custom source directory / include DLP
uv run python chinese/prepare.py --source "D:/other_docs" --exclude-dirs ""
```

## File structure

```
chinese/
  prepare.py     # Extract, clean, tokenize → data_chinese/
  train.py       # Training wrapper → runs_chinese/
  tokenizer.py   # Character-level tokenizer
  quantize.py    # 4-bit PTQ check
  export.py      # Export → firmware/model_chinese/
  generate.py    # Inference demo
  README.md      # This file
data_chinese/    # (gitignored) corpus.txt, tokenizer.json, train.bin, val.bin
runs_chinese/    # (gitignored) ple-zh2-s42.pt / .json
firmware/model_chinese/  # (gitignored) model.bin, golden.npz, golden.txt
```

## 已知限制

- **纯预训练模型**：能生成领域风格文本（保险条款、临床指南），但不能问答/指令跟随（需 SFT 或 RAG）
- 词表 5,901 字符基于当前语料；新增文档后应重新执行 prepare.py
- D:\docs\raw 是 OCR 转换产物，含一定噪声；行级过滤已缓解但非完美
