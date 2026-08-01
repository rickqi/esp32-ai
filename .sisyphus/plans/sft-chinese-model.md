# SFT 升级中文模型执行计划

> 计划创建: 2026-07-31 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 关联文档: chinese/SFT_PLAN.md

---

## 目标

将纯预训练中文模型（zh2, ppl 12.7）通过 SFT 升级为能回答领域问题（如"甲状腺切除是否会造成晕倒"）的指令跟随模型，并完成设备部署（完全隔离英文链路）。

## 成功标准

1. SFT 后模型能对 8 个标准问题给出结构化回答（非纯续写）
2. 量化退化 deg ≤ 0.3（4-bit PTQ）
3. model.bin ≤ 2 MB，可烧录到 ESP32-S3
4. 英文链路（src/, firmware/esp32_llm/, tools/）零改动
5. 全部产物可复现（脚本化）

---

## Phase 0: SFT 数据构建（~2h）

### 任务 0.1: 数据拉取归档
- [ ] 创建 `data_chinese/sft/raw/` 目录
- [ ] 从 WSL 复制 3 个文件：
  - `med_instruction_train_chatml.json` (1.9MB)
  - `med_instruction_val_chatml.json` (100KB)
  - `med_instruction_cot_chatml.json` (218KB)
- [ ] 从 D:\docs\search_logs 提取 QA → `search_logs_qa.json`（169 有效样本）
- [ ] 写 `chinese/sft/sft_data.py` 完成上述拉取（可重跑）

### 任务 0.2: search_logs 清洗
- [ ] 过滤规则：
  - 丢弃 "找到 N 个结果:" 纯列表响应（无实质内容）
  - 丢弃空响应 / success=false / 指令 < 5 字符
  - 保留含实质中文内容的响应（≥ 50 字符且非纯列表）
- [ ] 输出清洗后统计（预期 ~100-130 条）

### 任务 0.3: tokenizer 扩展
- [ ] `chinese/tokenizer.py` 增加 3 个特殊 token：
  - `4 = <|user|>`、`5 = <|assistant|>`、`6 = <|end|>`
- [ ] 注意：tokenizer.json 需要版本化（追加特殊 token 后 vocab_size = 5901 + 3 = 5904）
- [ ] 验证旧模型权重兼容性（head 行数需从 5901 → 5904，训练时重新初始化新增行）

### 任务 0.4: 数据转换
- [ ] `sft_data.py` 输出 `data_chinese/sft/processed/sft_train.json`：
  - 格式: `{"text": "...", "loss_mask": [0/1...]}`
  - 样本结构: `BOS <|user|> 问题 <|end|> <|assistant|> 回答 <|end|> EOS`
  - loss_mask: assistant 部分为 1，其余为 0
- [ ] 合并：WSL 923 条 + search_logs ~120 条 + CoT 80 条 = ~1100 条
- [ ] 输出 train/val 划分（90/10）

**验收**: `sft_train.json` 样本数 ≥ 900，loss_mask 正确（assistant 段=1）

---

## Phase 1: SFT 训练（~2h + 训练时间）

### 任务 1.1: 预训练 checkpoint 适配
- [ ] 从 `runs_chinese/ple-zh2-s42.pt` 加载，扩展 head 到 5904 行（新增 3 行随机初始化）

### 任务 1.2: 训练脚本 `chinese/sft/sft_train.py`
- [ ] `InstructionDataset`: 加载 sft_train.json，按 loss_mask 计算
- [ ] `MixedDataset`: SFT 80% + 预训练语料 20%（防遗忘）
- [ ] 从预训练 checkpoint 续训（`--resume`）
- [ ] 超参: lr 5e-4, steps 3000, batch 16, seq 256, warmup 100, cosine
- [ ] 输出到 `runs_chinese/sft/ple-zh2-sft-s42.pt`

### 任务 1.3: 训练执行
- [ ] 训练 3000 步（预计 ~30 min CPU）
- [ ] 记录 val loss 曲线（SFT loss 应 < 预训练最后阶段的 loss）

**验收**: SFT checkpoint 生成，val loss 收敛（最后 500 步下降 < 0.01）

---

## Phase 2: 量化导出（~30 min）

### 任务 2.1: 量化
- [ ] `chinese/quantize.py --tag zh2-sft --seed 42`
- [ ] 验收: 4-bit deg ≤ 0.3（若 > 0.3 考虑 int8 head 或增大 group）

### 任务 2.2: 导出
- [ ] `chinese/export.py ple-zh2-sft-s42`
- [ ] 验收: `firmware/model_chinese/model.bin` ≤ 2 MB，PLE1 magic 正确

---

## Phase 3: 设备适配（~2h，需 arduino-cli 环境）

### 任务 3.1: 中文 vocab.h 生成
- [ ] `chinese/sft/gen_vocab.py`（或 chinese/gen_vocab.py）：
  - 用 CharTokenizer 生成 `VOCAB_N/VOCAB_BLOB/VOCAB_OFF`
  - 格式与 `src/gen_assets.py` 输出一致
- [ ] 输出到 `firmware/esp32_llm_zh/vocab.h`

### 任务 3.2: 中文固件副本
- [ ] 复制 `firmware/esp32_llm/` → `firmware/esp32_llm_zh/`
- [ ] 修改：
  - `#include "vocab.h"`（指向中文版）
  - `DEMO_PROMPT_IDS` → 中文 demo（如"本报告"的字符 ID）
  - 保持串口 JSON 协议不变（PC 端拼好指令模板）
- [ ] 编译验证（需 arduino-cli）

### 任务 3.3: PC 脚本
- [ ] 复制 `tools/send_prompt.py` → `tools/send_prompt_zh.py`
- [ ] 修改 tokenizer 路径 → `data_chinese/tokenizer.json`

### 任务 3.4: 烧录
- [ ] `esptool.py write_flash 0x110000 firmware/model_chinese/model.bin`
- [ ] 烧录固件 + 串口验证

**验收**: 设备上电，串口问"甲状腺切除是否会造成晕倒"→ 有结构化回答

---

## Phase 4: 评估（~1h）

### 任务 4.1: SFT 前后对比
- [ ] 8 个标准问题（内置，无需外部引用）：
  1. 甲状腺切除是否会造成晕倒
  2. 胃癌的典型临床表现有哪些？请列举
  3. 导尿管相关尿路感染的预防措施是什么
  4. 一位56岁男性上腹痛伴体重下降，应考虑哪些鉴别诊断
  5. 请描述气管插管的标准操作步骤
  6. 肺癌的TNM分期标准是什么
  7. 手术后需要观察哪些并发症？请列出
  8. 什么是制度修订流程
- [ ] SFT 前（zh2）vs SFT 后（zh2-sft）输出对比表

### 任务 4.2: 领域保留验证
- [ ] 纯续写测试（"临床表现："）确认未灾难性遗忘
- [ ] 医疗/制度/消保/法律 各测 2 题

### 任务 4.3: 文档更新
- [ ] chinese/README.md 补充 SFT 流程
- [ ] 提交并推送全部变更

---

## 依赖与风险

| 依赖 | 状态 |
|---|---|
| WSL 数据可访问 | ✅ 已验证 |
| search_logs 可用 | ✅ 已验证（169 条） |
| 预训练模型 zh2 | ✅ runs_chinese/ple-zh2-s42.pt (8.3MB) |
| arduino-cli 环境 | ⚠️ 当前机器无，需在有环境的机器执行 Phase 3 |
| GPU 加速 | ❌ 无（CPU 训练 ~30min，可接受） |

| 风险 | 缓解 |
|---|---|
| 1100 条指令偏少 | 混合 20% 预训练；WSL QA 生成器扩展 |
| tokenizer 版本升级破坏旧模型 | head 5901→5904 需显式处理，写迁移测试 |
| SFT 后量化退化大 | int8 head 备选方案 |
| Phase 3 无 arduino-cli | 交付脚本+文档，用户在有环境机器执行 |

## 执行顺序

```
Phase 0 (数据) → Phase 1 (训练) → Phase 2 (量化导出) → [Phase 3 需要外部环境] → Phase 4 (评估)
     ↓
可并行: 0.2 search_logs 清洗 与 0.3 tokenizer 扩展
```
