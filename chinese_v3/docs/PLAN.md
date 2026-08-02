# chinese_v3 蒸馏方案（V3 独立版本）

> 日期: 2026-08-02 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 目标: 从单台 24M 蒸馏开始，生成 V3 版本（与 V2 完全隔离）
> 蒸馏源: Qwen3-0.6B（WSL 已有环境）

---

## 一、版本隔离概览

| 项 | v2 (chinese_v2/) | **v3 (chinese_v3/)** |
|---|---|---|
| 模型来源 | 从零训练（医学语料） | **Qwen3-0.6B 蒸馏** |
| 目标参数 | 13.7M | **24M** |
| 词表 | 6,594 | **8,192**（更全） |
| 数据目录 | data_v2/ | **data_v3/** |
| 模型目录 | runs_v2/ | **runs_v3/** |
| 产物目录 | firmware/model_v2/ | **firmware/model_v3/** |
| 文档 | chinese_v2/README.md | **chinese_v3/docs/** |

**隔离保证**: 目录全独立 + 训练脚本 `--data-dir/--runs-dir` 参数切换。

## 二、为什么蒸馏（vs 从零训练）

| 对比 | v2 从零训练 | v3 蒸馏 |
|---|---|---|
| 数据需求 | 100M 字符语料 | **50K 高质量问答（Qwen 生成）** |
| 训练时间 | GPU 12 分钟（预训练） | GPU 2-3 天（蒸馏） |
| 模型质量 | 医学领域尚可 | **继承 Qwen3-0.6B 能力** |
| 泛化 | 领域内 | **更通用（学教师分布）** |

**核心**: 蒸馏让小模型继承大模型"软知识"（token 分布），比从零训练学得更深。

## 三、蒸馏方案

### 3.1 蒸馏源：Qwen3-0.6B（WSL 已有）

```
路径: /home/LLMs-from-scratch/projects/chinese-medical-text-generation/
已有: train_qwen_lora.py（加载 Qwen/Qwen3-0.6B）
      数据生成脚本（med_qa_generator.py 等）
```

### 3.2 学生模型（24M PLE）

```
架构: d160 / l8 / p192 / 词表 8192
  core   3M   (attention + FFN, 每层 ~375K)
  table  18M  (8192 × 8 × 288 = 18.9M)
  stream 2M   (8192 × 160 = 1.3M)
  total  24M → int4 ~12MB (单台 14MB ✅)
```

### 3.3 蒸馏方法（两级）

```
① 数据蒸馏（主导）
   Qwen3-0.6B 生成 50K 医学问答
   → 复用 chinese_v2/build_sft.py + build_raft.py 管线
   → 输出 data_v3/sft/

② Logits 蒸馏（增强）
   训练损失 = 0.7 × CE(真实标签) + 0.3 × KL(学生, 教师)
   教师 = Qwen3-0.6B（冻结，WSL 加载）
   学生 = 24M PLE（本项目架构）
```

### 3.4 训练流程

```
P0: 数据准备
    Qwen3-0.6B 生成 50K QA + 20K RAFT 对 → data_v3/
    训练 8192 字符级 tokenizer → data_v3/tokenizer.json

P1: 预训练（可选，医学语料继续）
    chinese/train.py --data-dir data_v3 --runs-dir runs_v3 \
      --d-model 160 --n-layers 8 --ple-dim 288 --vocab-size 8192 \
      --steps 15000 --tag zh6

P2: 蒸馏 SFT
    chinese_v3/distill_train.py（新增）
      --teacher Qwen/Qwen3-0.6B（WSL）
      --student runs_v3/ple-zh6-s42.pt
      --loss 0.7*CE + 0.3*KL
      --steps 3000

P3: RAFT 微调
    chinese_v2/build_raft.py（复用，data_v3 路径）

P4: 量化导出
    quantize.py --tag zh6-distill --runs-dir runs_v3
    export.py → firmware/model_v3/model.bin (~12MB)
    gen_vocab.py --tokenizer data_v3/tokenizer.json
```

## 四、实施步骤

| 步骤 | 内容 | 工作量 |
|---|---|---|
| P0.1 | Qwen3-0.6B 生成 50K QA（WSL） | 3-5h API/GPU |
| P0.2 | 训练 8192 词表 + 编码 | 30min |
| P1 | zh6 预训练（GPU） | 20min |
| P2 | distill_train.py + 蒸馏 SFT | 1-2h 写 + 3h 训 |
| P3 | RAFT | 30min |
| P4 | 量化导出 + 验证 | 1h |
| 合计 | | **~1 天** |

## 五、验收标准

```
1. 24M PLE 学生（int4 ~12MB，单台 ESP32 可装）
2. 蒸馏后 val ppl < v2 对应水平
3. 8 标准问题: 生成质量 ≥ v2（医学内容更真实）
4. RAG 证据利用: 复述准确率 ≥ v2
5. 推理速度 ≥ 1.5 tok/s
```

## 六、风险

| 风险 | 缓解 |
|---|---|
| Qwen3-0.6B 权重下载慢 | WSL 已有项目缓存；HF 镜像 |
| 蒸馏训练内存（教师+学生同载） | 教师 int8/fp16 + 梯度不更新教师 |
| 8192 词表与 v2 不同 | 独立 tokenizer + 独立 vocab.h（已隔离） |
| 24M 蒸馏质量不足 | 增加蒸馏数据至 100K；调 KL 权重 |

## 七、文件清单

```
chinese_v3/
  docs/PLAN.md        # 本文档（方案）
  docs/ANALYSIS.md    # 设计分析（待生成）
  distill_train.py    # 蒸馏训练（待实现）
  prepare.py          # 数据准备（复用 v2 + Qwen 生成）
data_v3/              # (gitignored) 词表/数据/SFT/RAFT
runs_v3/              # (gitignored) 模型
firmware/model_v3/    # (gitignored) 量化产物
```
