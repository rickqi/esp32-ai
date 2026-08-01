# 中文训练模型方案 v2（独立环境）

> 日期: 2026-07-31 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 依据: PLAN_AB_RETRAIN_RAG.md 方案 A（数据源升级）
> 目标: 用干净医学数据（魔搭镜像）训练**另一套**独立中文模型，与 v1（OCR 语料）完全隔离

---

## 一、为什么另建环境（与 v1 隔离）

| 维度 | v1 (chinese/) | v2 (chinese_v2/) |
|---|---|---|
| 预训练语料 | D:\docs\raw OCR 文档（含 LaTeX/表格噪声） | **zjydiary/Medical 医学百科+教材**（干净） |
| SFT 数据 | DeepSeek 生成 + search_logs（7387 条） | **本草 8K + HuatuoGPT2 + Firefly**（12K+） |
| 数据目录 | data_chinese/ | **data_v2/** |
| 模型目录 | runs_chinese/ | **runs_v2/** |
| 产物 | firmware/model_chinese/ | **firmware/model_v2/** |
| 分词器 | data_chinese/tokenizer.json（已提交） | **data_v2/tokenizer.json**（新词表） |

**隔离保证**：数据/模型/产物全部分目录，训练脚本通过 `--data-dir/--runs-dir` 参数切换环境，互不覆盖。

## 二、数据源（魔搭镜像，已验证可访问 ✅）

| 用途 | 数据集 | 规模 | 状态 |
|---|---|---|---|
| **预训练** | `zjydiary/Medical`（shibing624/medical 镜像） | 百科 360K + 教材 8.5K | ✅ API 验证可访问 |
| **SFT 主数据** | BenTsao/本草（GitHub SCIR-HI） | 8K 医学指令 | ⏳ 待下载 |
| **SFT 补充** | HuatuoGPT2-SFT-GPT4-140K（采样 3K） | 142K | ⏳ 待采样 |
| **通用指令** | `AI-ModelScope/firefly-train-1.1M`（采样 1-2K） | 1.1M | ✅ API 验证可访问 |

## 三、实施步骤

### P0: 数据下载 + 构建（30-60 分钟）

```powershell
uv run python chinese_v2/prepare.py --download   # 拉取 zjydiary/Medical
uv run python chinese_v2/prepare.py              # 清洗 + 构建语料 + 字符级 tokenizer
```

产物: `data_v2/{corpus.txt, tokenizer.json, train.bin, val.bin}`

### P1: 预训练 zh5（1-2 小时）

```powershell
uv run python chinese/train.py --data-dir data_v2 --runs-dir runs_v2 `
  --d-model 160 --n-layers 8 --n-heads 8 --ple-dim 192 --target-core 2500000 `
  --steps 10000 --tag zh5
```

架构与 zh4 相同（PLE, 12.5M），仅数据源不同 → 干净医学文本质量。

### P2: SFT 重训（30-60 分钟）

```powershell
# 本草 8K + HuatuoGPT2 3K + Firefly 1K → 12K 指令样本
uv run python chinese/sft/sft_train.py --resume runs_v2/ple-zh5-s42.pt `
  --sft-data <本草转换后数据> --steps 600 --instruction-ratio 0.9 --tag zh5-med
```

### P3: 导出 v2 模型

```powershell
uv run python chinese/quantize.py --tag zh5-med --seed 42 --runs-dir runs_v2
uv run python chinese/export.py ple-zh5-med-s42 --runs-dir runs_v2 --out-dir firmware/model_v2
uv run python chinese/gen_vocab.py --tokenizer data_v2/tokenizer.json --out firmware/esp32_llm_zh_v2/vocab.h
```

## 四、与方案 B（RAG）衔接

```
v2 模型（文本质量↑）+ RAG 知识库（Huatuo26M-Lite, 事实知识）
  = 可用的设备端医学问答
```

- RAG 知识库构建（P3 in AB plan）与 v2 训练并行
- 设备端: esp32_llm_zh_v2 固件 + RAG 检索器

## 五、验收标准

| 项 | v1 (zh4-ds) | v2 (zh5-med) 目标 |
|---|---|---|
| 预训练 val ppl | 7.64 | **< 7.0**（干净数据） |
| SFT val ppl | 8.6 | **< 8.0** |
| 文本噪声 | 含 LaTeX/表格残留 | **显著减少** |
| 医学术语准确 | 中 | **高**（百科+教材） |

## 六、风险

| 风险 | 缓解 |
|---|---|
| 本草/Huatuo 下载受限 | 用魔搭镜像或 HF 镜像 |
| 12K SFT 对小模型仍可能过拟合 | 混合 20% 预训练 + early stopping |
| v2 词表与 v1 不同 | 独立 tokenizer.json + 独立 vocab.h（已隔离） |
| 医学百科缺保险领域 | 8:2 混合保留 v1 保险语料（可选） |
