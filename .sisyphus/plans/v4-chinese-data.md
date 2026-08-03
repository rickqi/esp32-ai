# V4 中文训练数据方案执行计划（临床指南数据整合）

> 日期: 2026-08-03 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 目标: 将 V1 的临床诊疗指南全集 + medica（共 ~275MB 专业临床知识）
>       按 V2/V3 数据格式生成 V4 训练数据, 补齐 V2/V3 缺失的"临床指南/规范"维度
> 硬件: WSL + RTX 5080 16GB (cu128)

---

## 〇、背景与动机

| 版本 | 语料 | 缺什么 |
|---|---|---|
| V1 | 保险业务文档 (OCR) | 临床知识 |
| V2/V3 | 医学百科+教材 (zjydiary) | **官方指南/临床规范** |
| **V4** | V2/V3 + **临床诊疗指南全集(170MB) + medica(105MB)** | ✅ 补齐指南维度 |

新数据价值: 指南/规范是"实践级"知识 (诊疗流程/用药标准/分级规范),
与百科/教材的"基础级"知识互补 → 模型从"懂原理"升级到"会诊疗"。

---

## 一、V4 环境结构（遵循多环境隔离模式）

```
chinese_v4/               # 新环境目录 (复用 chinese_v2/ 脚本)
  prepare_v4.py           # 新: 临床数据清洗 + 语料合并 + tokenizer 重训
  build_sft_v4.py         # 新: 临床数据 SFT QA 构建 (合并 V2 三池)
  build_raft_v4.py        # 复用 build_raft.py (--top2 格式)
  kb/build_guide_kb.py    # 新: 指南分块 → KB 条目
  README.md / PLAN.md     # 文档
data_v4/                  # 新数据目录 (完全隔离)
  corpus.txt / tokenizer.json / train.bin / val.bin
  sft/ (sft_train/val + raft_train/val)
  kb/ (format_data.jsonl + index.bin)
runs_v4/                  # 模型检查点
firmware/model_v4/        # 导出产物 (如需要)
firmware/esp32_llm_zh_v4/ # 固件 (如需要, 词表变化时)
```

---

## 二、数据构建流程（三阶段）

### 阶段 1: 临床语料清洗 + 预训练数据 (prepare_v4.py)

**输入**: 临床诊疗指南全集 (77 md) + medica (444 md)
**输出**: data_v4/corpus.txt + tokenizer + train.bin/val.bin

```
Step 1.1 扫描与过滤
  - 仅处理 *.md, 跳过 _index.md / *_ocr.md (OCR 重复版) / 伴生 .json
  - 统计: 临床全集 40 有效 md, medica 410 有效 md

Step 1.2 清洗 (增强版 clean_markdown)
  基础清洗 (复用 V1 chinese/prepare.py):
  - 剥 YAML frontmatter (--- 之间, 含 title/headings)
  - 去 HTML 标签 / Markdown 链接图片 / 表格管道
  新增指南特定规则:
  - 丢弃图书噪声行: 出版社/邮编/电话/网址/QQ/定价/CIP/购书热线
  - 丢弃目录页码行: "1. xxx ..... 5" (含连续点号)
  - 丢弃纯数字/ISBN/版权行
  - 保留 ## 章节标题 (作为正文一部分, 保留结构语义)

Step 1.3 语料合并 (两套方案取其一)
  方案 A (推荐): 独立 V4 语料 = 指南 + medica + V2 corpus
    corpus_v4 = guide_clean + medica_clean + v2_corpus
    上限: --max-chars 200M (V2 的 100M 太紧)
  方案 B (轻量): 仅新增指南 + medica, 不合并 V2 (预训练从头)
    (模型架构不变时建议 A, 知识面最全)

Step 1.4 tokenizer 重训
  - CharTokenizer vocab_size=8192 (同 V3), 在 V4 语料上训练
  - 预期词表 ~7.5-8K (指南含更多医学术语 → 可能 >7563)
  - 保存 data_v4/tokenizer.json

Step 1.5 编码 train.bin / val.bin
  - 分块流式编码 (memmap, 复用 V1 逻辑)
  - 99:1 切分, uint16
```

### 阶段 2: SFT + RAFT 数据 (build_sft_v4.py + build_raft_v4.py)

**输出**: data_v4/sft/sft_train.json + raft_train.json

```
Step 2.1 SFT 池构建 (合并 4 源):
  A. V2 三池: zjydiary finetune + BenTsao + HuatuoGPT2 (医学 QA, 53K 池)
  B. 新增【指南 QA 对】:
     - 指南章节 → (question=章节标题转问句, answer=章节正文清洗)
     - 例: "糖尿病酮症酸中毒的诊疗要点" → {章节内容}
     - 规则生成: 标题前加 "的诊疗要点是什么" / "如何诊疗" / "指南要点"
     - 预计产出 3-8K 条 (按章节粒度)
  采样: --zjydiary 30000 --benchao 8000 --huatuogpt2 20000 --guide 5000
  格式: <BOS><user>Q<end><assistant>A<EOS>, shifted labels (复用 encode_instruction)

Step 2.2 RAFT 数据 (复用 build_raft.py --top2):
  - 输入: data_v4/kb/format_data.jsonl (含新指南条目)
  - 输出: raft_train.json (E1\nE2\nQ 格式, P3 对齐)
  - --count 20000 --data-dir data_v4 --top2
```

### 阶段 3: KB 检索索引 (kb/build_guide_kb.py)

**输出**: data_v4/kb/index.bin (RAG 设备端检索)

```
Step 3.1 指南 KB 条目生成:
  - 章节切块 (300-800 字符/块)
  - 条目格式: {"id", "question": 标题, "answer": 块内容, "score", "label": 科室}
  - 合并现有 format_data.jsonl (93.5K 条)

Step 3.2 重建索引 (复用 build_index.py):
  - python3 chinese/kb/build_index.py --kb data_v4/kb/format_data.jsonl \
      --tokenizer data_v4/tokenizer.json --out data_v4/kb/index.bin
  - ⚠️ kb 分区 2MB 上限: 现有 1.83MB → 新增条目需控制
    - 方案 1: 精选高频科室指南 (~10-15K 条)
    - 方案 2: P4 分区扩展 (kb 移 0xC00000, 索引可 1.9MB)
```

---

## 三、训练流程 (WSL, RTX 5080)

```
Step 4.1 预训练 (与 V3 同架构, 更大语料):
  python3 chinese/train.py --data-dir data_v4 --runs-dir runs_v4 \
    --d-model 160 --n-layers 8 --n-heads 8 --ple-dim 192 \
    --target-core 2500000 --steps 20000 --tag zh7

Step 4.2 SFT (含指南 QA):
  python3 chinese/sft/sft_train.py --resume runs_v4/ple-zh7-s42.pt \
    --sft-data data_v4/sft/sft_train.json --val-data data_v4/sft/sft_val.json \
    --data-dir data_v4 --runs-dir runs_v4 \
    --steps 3000 --eval-every 500 --instruction-ratio 0.9 --tag zh7-guide

Step 4.3 RAFT (复述对齐):
  python3 chinese/sft/sft_train.py --resume runs_v4/ple-zh7-guide-s42.pt \
    --sft-data data_v4/sft/raft_train.json --val-data data_v4/sft/raft_val.json \
    --data-dir data_v4 --runs-dir runs_v4 \
    --steps 1500 --instruction-ratio 1.0 --tag zh7-raft

Step 4.4 量化 + 导出 + 验证:
  python3 chinese/quantize.py --tag zh7-raft --seed 42 --data-dir data_v4 --runs-dir runs_v4
  python3 chinese/export.py ple-zh7-raft-s42 --runs-dir runs_v4 --out-dir firmware/model_v4
  wsl gcc -O3 -o /tmp/verify firmware/host_verify/verify.c -I firmware/common -lm
  wsl /tmp/verify firmware/model_v4/model.bin firmware/model_v4/golden.txt  # 必须 PASS
```

---

## 四、质量验证 (每阶段门禁)

| 阶段 | 验证项 | 通过标准 |
|---|---|---|
| 1 语料 | 清洗后字符量 | ≥80M 有效字符 (指南噪声去除率 >60%) |
| 1 语料 | tokenizer 覆盖 | 指南高频医学术语非 UNK |
| 2 SFT | 指南 QA 样本抽查 | 问题↔答案语义匹配 |
| 3 KB | 检索命中率 | "糖尿病诊疗" → 命中指南条目 |
| 4 训练 | val ppl | < V3 的 2.2 (指南知识提升) |
| 4 训练 | 主机 verify | PASS (max abs diff ≤1e-5) |
| 4 训练 | 生成测试 | "糖尿病酮症酸中毒如何急救?" → 指南式回答 |

---

## 五、执行顺序与时间预估

```
Phase 0: 建环境 chinese_v4/ + data_v4/          (~10 min)
Phase 1: prepare_v4.py (清洗+语料+tokenizer)    (~20 min, 含清洗验证)
Phase 2: build_sft_v4.py + build_raft_v4.py     (~15 min)
Phase 3: kb/build_guide_kb.py + 重建索引         (~15 min)
Phase 4: GPU 训练链 (预训练/SFT/RAFT)            (~60-90 min)
Phase 5: 量化导出 + 主机验证 + 生成测试          (~15 min)
Total:                                          ~2.5-3 小时
```

---

## 六、风险与决策点

| 风险 | 影响 | 缓解 |
|---|---|---|
| 指南 OCR 噪声残留 | 语料质量 | 增强清洗规则 + 抽样人工审查 |
| kb 2MB 分区瓶颈 | 检索容量 | 精选科室 或 P4 分区扩展 |
| 新词表 >7563 | 固件需重建 vocab.h | gen_vocab.py 重新生成 |
| 训练时间 | - | GPU 全程 ~2h 可接受 |

**决策点**:
1. 语料方案: A (合并 V2) vs B (仅指南) → **推荐 A**
2. 是否做 SFT 指南 QA: 是 (核心价值) → **做**
3. kb 容量: 精选 vs 扩展分区 → **先精选, 验证后扩展**
