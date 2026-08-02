# ESP32 中文/英文模型 设计与对比分析报告

> 日期: 2026-08-01 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 覆盖: 模型设计调用方式 + 中文 vs 英文全面对比

---

# 第一部分：模型设计调用方式全面分析

## 1. 使用什么方式创建模型

### 1.1 核心架构：PLE（Per-Layer Embedding，逐层嵌入）

模型采用 **Google Gemma 的 Per-Layer Embedding** 架构——这是整个项目能跑在 MCU 上的根本原因。

```
传统 Transformer:
  词嵌入表 (Vocab × D) ──→ 每层共享同一个嵌入
  → 嵌入表必须常驻快速内存（SRAM）

PLE 架构:
  基础嵌入 (Vocab × D)        ← 每 token 查一次，小表
  + PLE 表 (Vocab × L×P)      ← 每 token 查 L 行，每层一行，超大表

  每层: x += RMSNorm(Proj(GELU(Gate(x)) × PLE_input[tok, layer]))
  → 每 token 只读 L 行 PLE 表（~450 字节），其余 2500 万参数不用动
```

### 1.2 创建流程（训练管线）

```
src/model.py        → Config + TinyLM（PLE Transformer 定义）
chinese/train.py    → make_model("ple", target_core=2.5M) 二分搜索 ffn_hidden
                    → 训练 10000-20000 步 → .pt checkpoint
chinese/quantize.py → 4-bit PTQ 量化（group=32，SFT 模型敏感）
chinese/export.py   → 导出扁平二进制 model.bin（PLE1 magic）
```

### 1.3 参数三层划分（核心设计）

```
core   (每 token 全算)   = attention + FFN       → flash 读取
stream (每 token 扫描)   = 输出头 (Vocab×D)      → PSRAM
table  (每 token 查 L 行) = PLE 表 (Vocab×L×P)   → flash mmap
```

## 2. 模型如何放进 ESP32 的 512KB SRAM

### 2.1 核心洞察：模型根本不放进 SRAM！

```
Flash 16MB（慢但巨大）  ← 模型主体（int4 权重）
  │ mmap 内存映射直接读
PSRAM 8MB（中等）       ← 输出头 staging + KV cache + 激活
SRAM 512KB（快极小）    ← 只有激活向量 + 小常量
```

### 2.2 具体实现（llm.h + esp32_llm.ino）

```c
// 1. 模型分区 mmap —— 模型直接从 flash 读，不加载到内存
esp_partition_mmap(model_partition) → base
llm_load(base, &model)  // 解析 header + 绑定 tensor 指针到 flash 地址

// 2. 大 tensor 结构（QT）：codes 指向 flash，直接解量化
typedef struct {
  const uint8_t *codes;   // int4 nibbles，指向 flash 地址
  const uint16_t *scales; // fp16 scale，指向 flash
  int rows, cols, group;
} QT;

// 3. 每 token 推理：
matvec_q(&ple_table, x, y)  // 读 flash int4 → 解量化 → 乘加
  → 只访问需要的行，其余 flash 不动
```

### 2.3 SRAM 里到底有什么（512KB 内）

```c
// Scratch（PSRAM 分配，非 SRAM）
s.x = ps(D*4);          // 激活
s.h = ps(F*4);          // 中间层
s.kcache = ps(L*S*D*4); // KV cache → PSRAM
s.logits = ps(V*4);     // 输出 logits → PSRAM

// SRAM 只有:
head_actq[128]   // int8 量化激活（固定 128 元素）
static 变量       // 状态、缓冲区
栈 + RTOS 任务栈
```

**512KB SRAM 零权重占用**——模型全在 flash/PSRAM，SRAM 只放少量标量。

### 2.4 为什么能这样（内存分层利用）

| 权重 | 访问模式 | 存储 | 为什么 |
|---|---|---|---|
| PLE 表 | 每 token 只读 6-8 行 | flash mmap | 稀疏随机读，flash 完美匹配 |
| 输出头 | 每 token 全扫描 | PSRAM int8 | 顺序读，带宽够 |
| core | 每 token 全算 | flash int4 | 计算密集，flash 读取是瓶颈 |

## 3. 如何训练一个这样的模型

### 3.1 训练配置

```powershell
# 中文 v2 预训练（GPU RTX 5080）
python3 chinese/train.py --d-model 160 --n-layers 8 --n-heads 8 \
  --ple-dim 192 --target-core 2500000 --steps 20000 --tag zh5
# → ffn=244, core=2.5M, table=10.1M, total=13.7M
```

### 3.2 数据管线

```
魔搭 zjydiary/Medical（百科 361K + 教材 8.5K）
  → 清洗（去 LaTeX/表格）→ 100M 字符语料
  → 字符级 tokenizer（6594 词表，含 SFT 标记）
  → train.bin（99M tokens，uint16）
```

### 3.3 量化导出

```
fp32 → 4-bit group-wise PTQ（group=32）→ model.bin（7.5MB）
  → 烧录 model 分区（0x170000）
```

## 4. 如何进行 SFT

### 4.1 为什么 SFT

预训练只会"续写"，SFT 教会"问答"。

### 4.2 数据构造

```
多源指令混合（57K）：zjydiary 30K + 本草 7.2K + HuatuoGPT2 20K
格式: [BOS] <user> 问题 <end> <assistant> 答案 <end> [EOS]
labels: 只计算答案部分（loss masking，右移一位）
```

### 4.3 训练

```
从预训练续训 + 80% 指令 + 20% 续写（防遗忘）→ GPU 2 分钟
```

## 5. 为什么要增加 RAG

### 5.1 核心矛盾

```
13.7M 模型能生成医学风格文本，但无法记忆精确医学知识
实测: "甲状腺切除是否会造成晕倒" → 泛泛医学文本，不精准
```

### 5.2 RAG 解决方案

```
问题 → 检索 Huatuo26M-Lite(93.5K QA) → 证据注入 prompt → 模型基于证据回答
本质: 事实记忆外置知识库，模型负责语言组织
```

### 5.3 为什么还要 RAFT

```
小模型看到证据也不会引用（未学过）→ RAFT 微调"证据→复述"
实测: 给"收缩压≥140mmHg"证据 → 模型精确复述 ✅
```

### 5.4 完整闭环

```
知识库: Huatuo26M-Lite → IDF 倒排索引(1.83MB) → kb 分区
推理: 问题 → rag.h 检索(0.6ms) → 证据注入 → RAFT 模型续写
```

---

# 第二部分：中文 vs 英文模型对比分析

## 6. 架构参数对比

| 维度 | 英文 cleandeploy | 中文 v2 zh5-multi2 | 差异原因 |
|---|---|---|---|
| 词表 | 32,768（BPE） | 6,594（字符） | 中文汉字独立 token |
| d_model | 96 | **160** | 中文 core 更宽 |
| 层数 | 6 | **8** | 中文更深 |
| FFN | 66 | **244** | core 预算求解 |
| ple_dim | 128 | **192** | PLE 调制更宽 |
| seq_len | 256 | 256 | 相同 |

## 7. 参数分布对比（核心差异）

| 层 | 英文 | 占比 | 中文 v2 | 占比 |
|---|---|---|---|---|
| core | 558K | 1.9% | **2.5M** | **18%** |
| stream | 3.1M | 10.9% | 1.06M | 8% |
| table | **25.2M** | **87.2%** | 10.1M | 74% |
| total | **28.9M** | — | **13.7M** | — |

```
英文 87% 是 table（词表 32768 大导致）
中文 core 2.5M = 英文 558K 的 4.5 倍 → 每 token 计算量更大
  → 中文表达能力更强（core 是真正计算核心）
```

## 8. 内存与速度对比

| 项 | 英文 | 中文 v2 |
|---|---|---|
| model.bin | 14.9MB | **7.5MB** |
| PSRAM | ~5.1MB | ~3.7MB |
| head int8 | 2.53MB | 1.06MB |
| **推理速度** | **9.5 tok/s** | ~2 tok/s |
| core flash 读取/步 | 0.28MB | 1.25MB |

```
速度差异: 中文 core 4.5× → flash 读取瓶颈
权衡: 中文用速度换质量，英文用质量换速度
```

## 9. 数据与训练对比

| 维度 | 英文 | 中文 v2 |
|---|---|---|
| 预训练数据 | TinyStories 300MB | 医学百科+教材 100M 字符 |
| 训练量 | 75M tokens | 99M tokens |
| SFT 数据 | 无 | **57K 指令 + 20K RAFT** |
| val ppl | 11.39 | 12.13（不同分布不可直接比） |

## 10. 能力对比

| 能力 | 英文 | 中文 v2 |
|---|---|---|
| 文本生成 | ✅ 故事 | ✅ 医学内容 |
| 指令跟随 | ❌ | ✅ 57K SFT |
| 知识问答 | ❌ | ✅ RAG |
| 证据引用 | ❌ | ✅ RAFT |
| 设备端检索 | ❌ | ✅ 0.6ms |
| 中文显示 | ❌ | ✅ cjk_font.h |

## 11. 综合矩阵

| 维度 | 胜者 | 原因 |
|---|---|---|
| 参数量 | 英文 | 词表大 → table 大 |
| 核心密度 | **中文** | core 4.5× |
| 模型大小 | **中文** | 7.5 vs 14.9MB |
| 推理速度 | 英文 | core 小 |
| 指令能力 | **中文** | 有 SFT |
| 问答能力 | **中文** | 有 RAG+RAFT |
| 部署成本 | **中文** | 占用更小 |

## 12. 核心结论

```
【英文模型】参数量大但"虚胖"（87% 查表）
  → 快（9.5 tok/s）、小核心（558K）、仅故事生成

【中文 v2】参数少但"实心"（18% 核心，4.5× 密度）
  → 慢（2 tok/s）、强核心（2.5M）
  → 完整能力链：生成 + SFT + RAG + RAFT

【本质】同一 PLE 架构，两种权衡：
  英文 = 大词表 + 小核心 → 快而浅
  中文 = 小词表 + 大核心 → 慢而深
  + 中文后发优势：SFT/RAFT/RAG 全套增强
```

---

## 附：模型演进历程

| 版本 | 模型 | 参数量 | val ppl | 里程碑 |
|---|---|---|---|---|
| v1 英文 | cleandeploy | 28.9M | 11.39 | TinyStories 基线 |
| v1 中文 | zh4-ds | 12.5M | 7.64 | OCR 语料 + labels bug 修复 |
| v2 中文 | zh5 | 13.7M | 12.13 | 干净医学数据 |
| v2 中文 | zh5-multi2 | 13.7M | 7.0 | 多源 SFT |
| v2 中文 | ple-raft | 13.7M | 2.6 | RAFT 证据复述 |
| **v3 中文（计划）** | **zh6-distill** | **24M** | — | **Qwen3-0.6B 蒸馏**（详见 chinese_v3/docs/PLAN.md） |

### V3 蒸馏计划（2026-08-02）

```
目标: 24M PLE 学生（int4 ~12MB，单台 ESP32）
蒸馏源: Qwen3-0.6B（WSL 已有环境）
方法: 数据蒸馏（50K QA）+ Logits 蒸馏（CE + KL）
独立目录: chinese_v3/ + data_v3/ + runs_v3/（与 v2 完全隔离）
参考: chinese_v3/docs/PLAN.md

演进: 单台 24M（能力 1.5×）→ 后续 2 台 48M 流水线（能力 3×）
```

## 附：关键文件索引

```
src/model.py              PLE 模型定义
chinese/train.py          训练入口（--data-dir/--runs-dir 多环境）
chinese/quantize.py       4-bit PTQ
chinese/export.py         导出 model.bin（GROUP=32）
chinese_v2/prepare.py     v2 医学数据管线
chinese_v2/build_sft.py   多源 SFT 构建
chinese_v2/build_raft.py  RAFT 数据构建
chinese/kb/build_index.py RAG 索引构建（IDF 加权）
chinese/kb/test_rag_host.c 主机端 C 验证
firmware/common/llm.h     共享 C runtime
firmware/esp32_llm_zh_v2/ v2 固件（rag.h + kb 分区）
firmware/README.md        三语言版本说明
chinese/CHANGELOG.md      完整变更记录
```
