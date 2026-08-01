# chinese_v2 训练环境说明（独立版本）

> 版本: v2 | 数据源: 医学百科+教材（魔搭 zjydiary/Medical）
> 与 v1（OCR 业务文档）完全隔离，独立保存

---

## 一、版本隔离概览

| 项 | v1 (chinese/) | **v2 (chinese_v2/)** |
|---|---|---|
| 预训练数据源 | D:\docs\raw OCR 业务文档 | **zjydiary/Medical 医学百科+教材** |
| SFT 数据源 | DeepSeek 生成 + search_logs | **zjydiary finetune 医学指令 (1.95M)** |
| 数据目录 | data_chinese/ | **data_v2/** |
| 模型目录 | runs_chinese/ | **runs_v2/** |
| 产物目录 | firmware/model_chinese/ | **firmware/model_v2/** |
| 词表 | 5,904 字符 | **6,594 字符**（独立） |
| 模型 | zh4-ds (12.5M) | **zh5-med (13.68M)** |

**隔离保证**：目录全独立 + 训练脚本 `--data-dir/--runs-dir` 参数切换，互不覆盖。

## 二、完整训练流程（P0→P2 已执行）

### P0: 数据下载 + 构建

```powershell
uv run python chinese_v2/prepare.py --download   # zjydiary/Medical 1.97GB
uv run python chinese_v2/prepare.py              # 清洗 → corpus → tokenizer → bin
```

| 产物 | 规模 |
|---|---|
| corpus.txt | 100M 字符（百科 361K + 教材 8.5K） |
| tokenizer.json | **6,594 词表** |
| train.bin | 99.3M tokens |
| val.bin | 1M tokens |

### P1: 预训练 zh5（GPU）

```bash
# WSL (RTX 5080)
cd /mnt/d/codes/esp32-ai
python3 chinese/train.py --data-dir data_v2 --runs-dir runs_v2 \
  --d-model 160 --n-layers 8 --n-heads 8 --ple-dim 192 \
  --target-core 2500000 --steps 20000 --tag zh5
```

结果: `runs_v2/ple-zh5-s42.pt` | val ppl **12.13** | GPU 12.6 分钟

### P2: SFT 微调 zh5-med

```bash
python3 chinese_v2/build_sft.py --count 30000 --val-count 3000
python3 chinese/sft/sft_train.py --resume runs_v2/ple-zh5-s42.pt \
  --sft-data data_v2/sft/sft_train.json --val-data data_v2/sft/sft_val.json \
  --data-dir data_v2 --runs-dir runs_v2 \
  --steps 3000 --eval-every 500 --instruction-ratio 0.9 --tag zh5-med
```

结果: `runs_v2/ple-zh5-med-s42.pt` | val ppl **9.7** | GPU 112 秒

## 三、量化导出评估（结论: 需要）

| 检查项 | 值 | 结论 |
|---|---|---|
| 模型格式 | fp32 PyTorch 权重 | **必须 4-bit PTQ 量化** |
| PSRAM 需求 | 3.73M / 8M | ✅ 设备可容纳 |
| Flash 需求 | ~6.84M / 14.5M | ✅ 设备可容纳 |
| 词表差异 | 6,594 ≠ v1 5,904 | **需独立 vocab.h + model.bin** |

### 量化导出步骤

```bash
# 1. 4-bit 量化验证 (v2 路径)
python3 chinese/quantize.py --tag zh5-med --seed 42 --runs-dir runs_v2 --data-dir data_v2

# 2. 导出 model.bin (需支持 --runs-dir/--out-dir)
python3 chinese/export.py ple-zh5-med-s42 --runs-dir runs_v2 --out-dir firmware/model_v2

# 3. 独立 vocab.h (v2 词表)
uv run python chinese/gen_vocab.py --tokenizer data_v2/tokenizer.json \
  --out firmware/esp32_llm_zh_v2/vocab.h

# 4. 固件副本 (v2)
# 复制 esp32_llm_zh/ → esp32_llm_zh_v2/, vocab.h 用 v2, DEMO_PROMPT_IDS 用 v2 词表 ID
```

## 四、文件清单

```
chinese_v2/
  env.py          # 环境配置（路径/模型参数/数据源）
  prepare.py      # 魔搭下载 + 清洗 + 字符级 tokenizer
  build_sft.py    # finetune 采样 → SFT 格式（shifted labels）
  PLAN.md         # v2 方案文档
  README.md       # 本文档（训练说明）
data_v2/          # (gitignored) corpus/tokenizer/train.bin/val.bin/sft/
runs_v2/          # (gitignored) ple-zh5-s42.pt / ple-zh5-med-s42.pt
firmware/model_v2/  # (gitignored) 量化导出产物
```

## 五、质量对比（v1 vs v2）

| 问题 | v1 (zh4-ds) | v2 (zh5-med) |
|---|---|---|
| 胃癌的临床表现 | 模板文本答非所问 | 真实症状（上腹不适/食欲不振/恶心）✅ |
| 甲状腺切除 | 无关联文本 | 关联喉返神经/手术 ✅ |
| 文本自然度 | 低（LaTeX 噪声） | **高**（干净医学语料） |

## 六、已知限制与后续

- 12.5-13.7M 小模型**精准问答**仍受限 → 需方案 B（RAG）
- 预训练 20K 步可继续增加（val 仍在下行）
- SFT 30K/1.95M 可扩大采样
