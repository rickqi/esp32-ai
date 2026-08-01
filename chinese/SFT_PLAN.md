# 中文模型 SFT 训练方案（独立隔离）

> 日期: 2026-07-31 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 关联: WSL LLMs-from-scratch 评估、D:\docs\search_logs 评估

---

## 〇、隔离原则（核心）

```
英文链路（零改动）:                   中文 SFT 链路（完全独立）:
├─ src/train.py                      ├─ chinese/sft/            ← 独立目录
├─ src/quantize.py                   │   ├─ sft_train.py         ← SFT 训练
├─ src/export.py                     │   ├─ sft_data.py          ← 数据构建
├─ src/gen_assets.py                 │   ├─ gen_vocab.py         ← 中文 vocab.h
├─ firmware/esp32_llm/               │   └─ sft_eval.py          ← SFT 评估
├─ tools/send_prompt.py              ├─ data_chinese/sft/        ← SFT 数据独立归档
└─ firmware/model/                   │   ├─ raw/                 ← 原始来源（WSL/search_logs）
                                     │   ├─ processed/           ← 转换后指令数据
                                     │   └─ split/               ← train/val 划分
                                     ├─ runs_chinese/sft/        ← SFT 模型独立保存
                                     ├─ firmware/esp32_llm_zh/   ← 中文固件副本
                                     └─ firmware/model_chinese/  ← 中文模型产物
```

---

## 一、SFT 数据来源评估

### 1.1 来源 A：WSL 医学指令数据（✅ 高质量，主来源）

| 数据集 | 规模 | 质量 |
|---|---|---|
| `med_instruction_train_chatml.json` | **923 条** (avg 466 chars) | ✅ 4 领域医学 QA，结构化 |
| `med_instruction_val_chatml.json` | 50 条 | ✅ 独立验证集 |
| `med_instruction_cot_chatml.json` | 80 条 (avg 867 chars) | ✅ CoT 推理链 |

**路径**: `\\wsl.localhost\Ubuntu-22.04\home\LLMs-from-scratch\projects\chinese-medical-text-generation\docs\`

### 1.2 来源 B：D:\docs\search_logs（⚠️ 部分可用，需清洗）

**评估结果**：

| 指标 | 值 |
|---|---|
| 总文件数 | 184 个 md |
| 有效 QA（Response ≥ 20 字符） | **169 条** |
| 带检索来源（RAG 证据） | 161 条 |
| 总响应字符 | 75,607（avg 447/条） |
| 失败/空响应 | 15 条 |

**领域分布**：

| 领域 | 数量 | 可用性 |
|---|---|---|
| 医疗/临床 | 44 | ✅ 高（如"感冒如何治疗"→结构化指南回答） |
| 制度 | 41 | ✅ 中高（公司制度类） |
| 消保 | 36 | ✅ 中（消费者保护） |
| 法律 | 27 | ✅ 中（法规查询） |
| 再保 | 6 | ⚠️ 少但高价值 |
| DLP | 2 | ⚠️ |
| 其他/未知 | 13 | ❌ 需过滤 |

**需过滤的低质量样本**：
1. 纯列表响应（"找到 10 个结果: 文档1, 文档2..."）—— 无实质内容
2. 空列表响应（"北京" → "找到 10 个结果: , , ,"）
3. 指令过短（< 5 字符，如"北京"）
4. 失败搜索（success: false）

**预估清洗后**：~**100-130 条**高质量样本（医疗 44 + 制度/消保/法律 ~80）

### 1.3 数据合并策略

| 来源 | 采用量 | 占比 | 用途 |
|---|---|---|---|
| WSL ChatML 医学 | 923 train + 50 val + 80 cot | ~80% | 主指令数据 |
| search_logs 清洗后 | ~120 | ~10% | 制度/消保/法律补充 |
| 纯续写语料 (data_chinese) | 20% 混合 | ~10% | 防灾难性遗忘 |

---

## 二、SFT 数据格式与独立归档

### 2.1 数据归档结构（data_chinese/sft/）

```
data_chinese/sft/                      ← 独立归档（gitignore，本地生成）
├── raw/                               ← 原始来源（只读，不修改）
│   ├── wsl_med_train_chatml.json      ← 从 WSL 复制
│   ├── wsl_med_val_chatml.json
│   ├── wsl_med_cot_chatml.json
│   └── search_logs_qa.json            ← 从 D:\docs\search_logs 提取清洗
├── processed/                         ← 转换后（字符级 tokenizer 格式）
│   ├── sft_train.json                 ← [{"text": "BOS <|user|>...<|assistant|>...EOS", "loss_mask": [...]}]
│   ├── sft_val.json
│   └── sft_cot.json
└── split/
    ├── train.txt                      ← 纯字符序列（预训练风格，防遗忘用）
    └── val.txt
```

### 2.2 特殊 token 设计（追加到 CharTokenizer）

```python
# tokenizer 扩展：3 个新特殊 token（在 PAD/UNK/BOS/EOS 之后）
# 4 = <|user|>      指令开始
# 5 = <|assistant|> 回答开始（loss 从此标记后计算）
# 6 = <|end|>       消息结束
```

### 2.3 样本格式

```
输入（全序列）:
  BOS <|user|> 甲状腺切除是否会造成晕倒 <|end|> <|assistant|> 甲状腺切除... <|end|> EOS

loss mask:
  [0, 0, 0, ..., 0,  0,  0, 1, 1, ..., 1,  0, 0]
       ↑user部分不计算loss↑  ↑assistant部分计算loss↑
```

---

## 三、SFT 训练方案

### 3.1 训练脚本 `chinese/sft/sft_train.py`

```
复用: src/model.py (TinyLM PLE), chinese/tokenizer.py (CharTokenizer + 3 新token)
新增:
  - InstructionDataset: 加载 sft_train.json，按 loss_mask 计算 loss
  - MixedDataset: SFT 80% + 预训练语料 20%
  - 从预训练 checkpoint 续训（resume runs_chinese/ple-zh2-s42.pt）
  - 超参: lr 5e-4（预训练 1e-3 一半）、steps 3000、全量微调（2.17M 参数无需 LoRA）
```

### 3.2 训练命令

```powershell
# 1. 构建 SFT 数据（从 WSL + search_logs）
uv run python chinese/sft/sft_data.py

# 2. 训练（从预训练模型续训）
uv run python chinese/sft/sft_train.py \
    --resume runs_chinese/ple-zh2-s42.pt \
    --sft-data data_chinese/sft/processed/sft_train.json \
    --steps 3000 --lr 5e-4 --tag zh2-sft

# 3. 评估（SFT 前后对比）
uv run python chinese/sft/sft_eval.py --tag zh2-sft
```

### 3.3 SFT 后必须重新量化导出

```
SFT 改变了模型权重 → 必须重新走量化+导出链路（复用现有工具）:

uv run python chinese/quantize.py --tag zh2-sft --seed 42
uv run python chinese/export.py ple-zh2-sft-s42
→ firmware/model_chinese/model.bin (更新)
```

---

## 四、量化评估（问题 2 回答）

### 4.1 当前状态：✅ 已量化

| 模型 | 量化 | 退化 | model.bin |
|---|---|---|---|
| zh2 (预训练) | 4-bit PTQ 已完成 | deg +0.12 | 1.13 MB ✅ |
| 英文 cleandeploy | 4-bit PTQ 已完成 | deg +0.057 | 14.9 MB |

**当前模型不需要再量化** —— 已经量化完毕且质量良好。

### 4.2 何时需要重新量化

| 场景 | 是否需要量化 | 原因 |
|---|---|---|
| 当前 zh2 直接部署 | ❌ 不需要 | 已量化 |
| **SFT 训练之后** | **✅ 必须** | 权重已改变，旧量化失效 |
| 增加预训练步数 | ✅ 需要 | 权重改变 |
| 只改词表/数据 | ✅ 需要 | tokenizer 变化影响 head |

**结论**：SFT 是下一步，所以**先训练 SFT → 再量化导出 → 最后烧录**。量化工具链（chinese/quantize.py + export.py）已就绪，SFT 后直接复用。

---

## 五、实施路线

```
Phase 0: 数据构建（~2h）
  ├─ sft_data.py: 拉取 WSL ChatML → 字符级格式
  ├─ 解析 search_logs → 清洗 → 合并
  └─ 数据归档到 data_chinese/sft/

Phase 1: SFT 训练（~2h + 训练时间）
  ├─ tokenizer.py 扩展 3 特殊 token
  ├─ sft_train.py: InstructionDataset + MixedDataset
  └─ 训练 3000 步（预计 ~30 min CPU）

Phase 2: 量化导出（~30 min）
  ├─ chinese/quantize.py --tag zh2-sft
  └─ chinese/export.py → model.bin 更新

Phase 3: 设备适配（~2h，需 arduino-cli）
  ├─ gen_vocab.py → 中文 vocab.h
  ├─ firmware/esp32_llm_zh/ 固件副本
  └─ 烧录 + 验证 "甲状腺切除是否会造成晕倒"

Phase 4: 评估（~1h）
  └─ sft_eval.py: SFT 前后 8 个标准问题对比
```

---

## 六、风险与缓解

| 风险 | 缓解 |
|---|---|
| search_logs 部分响应是"找到N个结果"列表 | 清洗时按响应特征过滤（无实质内容） |
| 923+120 条指令对 2.17M 模型偏少 | 混合 20% 预训练数据；用 WSL QA 生成器扩展 |
| 字符级模型回答短问题（用户问题~30字符） | 问题会 pad 到 seq 长度，正常 |
| SFT 后领域语言退化 | instruction_ratio=0.8 + 20% 续写混合 |
| SFT 后量化退化增大 | 量化时验证 deg，如 >0.3 考虑 int8 头部 |
