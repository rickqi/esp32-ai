# 中文模型项目变更说明 (CHANGELOG)

> 分支: feature/ESP32-S3-4.2inch-RLCD
> 覆盖: chinese/ (v1) + chinese_v2/ (v2) + chinese_v3/ (v3) 全部演进

---

## V5 环境（外部 MiniMind PLE 模型, model_v5）

### 2026-08-04: V5 固件编译完成 + manual_compile.py 参数化
- 🏗️ **V5 固件编译成功**: esp32_llm_zh_v5.ino include llm_v5.h → bin 1385KB
  (含 rag_sd.h + deep 逻辑, 197 符号含 llm_forward)
- ⚙️ **manual_compile.py 参数化**: 加 `--sketch` 参数 (esp32_llm_zh_v3/v5 通用)
  - gen_inocpp #line 用 ino.name (自动适配 sketch 名)
  - link_elf/make_bin 输出名用 sketch_name
- 📚 **firmware/README.md**: 版本矩阵加 v4/v5 行
- ✅ **验证**: V4 verify PASS + V5 H2 verify PASS (均 max diff 1e-5)

### 2026-08-04: V5 固件核心实施 — llm_v5.h + H2 转换 + verify PASS
- 🧬 **llm_v5.h**: fork 共享 llm.h, 加 per-head q_norm/k_norm (MiniMind H2 架构)
  - Model 结构加 `q_norm[32]/k_norm[32]` (head_dim=48)
  - llm_load 每层读 attn_norm → q_norm → k_norm → qkv → attn_proj
  - forward: qkv matvec 后 per-head RMSNorm(q/k), 再 RoPE
- 🔄 **chinese_v5/convert_h2.py**: MiniMind PLE1 → llm_v5.h 格式
  - q/k/v 三个 [D,D] 拼接 qkv [3D,D]
  - Q tensor 前插 bits=4 字节 (MiniMind 无 bits 字节)
  - q_norm/k_norm 权重保留
- ✅ **verify_h2.c PASS**: C top=42 = PyTorch top=42, max diff 1e-5
- 🐛 **关键发现**: git 里的 H2 model.bin (13.42MB) 不完整
  → 用 MiniMind `dpo_h2_384_ple.pth` 重新导出 (14.05MB)
- 🏗️ **固件编译**: esp32_llm_zh_v5.ino include llm_v5.h → bin 1385KB
- ⚠️ **V4 修复**: 6b60c6e 误覆盖 vocab.h (8196→6400), 用 data_v4 重新生成恢复

### 2026-08-04: V5 部署产物 — 外部 PLE 模型接入 (H1/H2)
- 🧠 **来源**: [MiniMind](https://github.com/rickqi/minimind) 项目的 PLE 模型
  (Per-Layer Embedding, 对齐 esp32-ai PLE 架构), 完整链路:
  预训练 → SFT → DPO 偏好优化 → int4 group=32 PTQ → PLE1 扁平二进制导出
- 📦 **产物结构** (按 model_v4 格式约定, `firmware/model_v5/`):
  ```
  model_v5/
  ├── H1/                      ← MiniMind H1 (d256/l6/p96, 10.79M)
  │   ├── model.bin    5.8 MB  ← PLE1 (vocab 6400, rope 1e6, group 32)
  │   ├── golden.npz           ← golden 参考 (固定 prompt logits)
  │   └── golden.txt
  └── H2/                      ← MiniMind H2 (d384/l8/p128, 24.95M)
      ├── model.bin   13.42 MB ← PLE1
      ├── golden.npz
      └── golden.txt
  ```
- 🏋️ **训练链**: H1 loss 预训练 2.27 → SFT 2.04 → DPO 0.51; H2 预训练 2.07 → SFT 1.77 → DPO 0.52
- 🎯 **量化**: int4 group=32 PTQ, deg H1 +0.124 / H2 +0.041 (DPO 后量化鲁棒性与 SFT 后一致)
- ⚠️ **注意**: H1/H2 与 V4 词表不同 (6400 vs 8196)、rope_theta 不同 (1e6 vs 1e4)、
  seq_len 不同 (128 vs 256) — 固件侧需对应适配 tokenizer/vocab.h, 不能直接复用 V4 固件
- 🔍 **验证**: model.bin 与 golden 哈希校验 MATCH (源在 MiniMind `models/dpo_h{1,2}_*`)

### 2026-08-04: V5 MiniMind BPE 词表 vocab.h (VOCAB_N=6400)
- 📖 **词表交付** (commit `6b60c6e`): `firmware/esp32_llm_zh_v3/vocab.h`
  - 由 [MiniMind](https://github.com/rickqi/minimind) `scripts/gen_vocab_minimind.py` 生成
  - MiniMind BPE+ByteLevel token id → raw UTF-8 字节 (逆映射还原, 无 U+FFFD 污染)
  - VOCAB_N 7563 (V4) → **6400** (V5), blob 29.5KB
  - 已验证: 特殊 token (`<|endoftext|>`/`<|im_start|>`/`<|im_end|>`) 与 BPE token ("你好"/"什么"/"是") 字节正确
- ⚠️ **范围**: 本次仅交付词表, **未做设备端推理适配** (MiniMind H2 为 Qwen3 风格架构:
  独立 q/k/v/o 投影 + q_norm/k_norm, 与 esp32-ai llm.h 的融合 qkv 变体不兼容;
  且固件为字符级 tokenizer, BPE 需 PC 端预分词。如需推理需新增推理核, 另行评估)

---

## V4 环境（临床指南整合，独立）

### 2026-08-03: V4 中文环境建立 — 临床指南数据整合
- 🧬 **新环境 `chinese_v4/` + `data_v4/`**: 完全隔离 (遵循多环境模式)
- 📚 **语料扩充**: 临床诊疗指南全集(40 md) + medica(363 md) + V2 语料
  → corpus 130.35M tokens (V2 的 99.3M +31%), 词表 8196 (V3 的 7563 +633)
- 🎯 **SFT 数据**: 50K = 6K 指南 QA + 30K zjydiary + 8K BenTsao + 20K HuatuoGPT2
  (指南 QA: 章节标题→问句 + 章节正文, 新增 RE_NOISE_HEAD/RE_CLINICAL_HEAD 过滤)
- 🗂️ **KB**: 11K docs (8K 指南 + 3K V3), index.bin 1.97MB (2MB 分区内)
- 🏋️ **训练链**: zh7 预训练 ppl **11.59** (V3: 22.35) → SFT ppl **7.1** (9.2) → RAFT ppl **1.0** (2.7)
- 📦 **产物**: model.bin 8.81MB (16.4M params), 主机 verify **PASS**
- ⚠️ **发现**: model.bin 8.81MB > model 分区 8.56MB → 需扩分区 (0x170000 size 0x8F0000)

### 2026-08-03: SD 扩展索引（全量 KB, 双索引 RAG）
- 🔍 **触发机制分析**: 实测 12+ 问题证明 绝对分数/覆盖率/top-ratio **均不可靠**
  (分数带交叠 1401-2670 vs 1414-2090, IDF 84% 饱和, 覆盖率全 1.00)
  → **采用显式 `"deep":true` 为唯一触发方式**
- 🗄️ **SD 索引构建** (`chinese_v4/kb/build_sd_index.py`): 单字倒排
  index.bin(term表 59KB 常驻 + doclists 流式) + docs.bin(偏移表 O(1) 定位) + meta.bin
- 📊 **全量 136,877 docs** (V3 93,502 + 指南 43,375 突破 8K 上限)
  index 34.8MB + docs 17.4MB (~52MB SD 卡存储)
- ✅ **检索质量提升**: 肺癌/白疕/带状疱疹命中修正, 宫外孕→妇产科

### 2026-08-03: 固件 deep 检索集成 (rag_sd.h)
- 🛠️ **`firmware/esp32_llm_zh_v3/rag_sd.h`**: SD 倒排检索器
  - term 表(59KB) 常驻 PSRAM, doclists 流式 fread, 哈希表(2^18桶)打分
  - 遵循 ESP-IDF 性能指南: POSIX read/lseek, 内部 SRAM I/O 缓冲
  - 主机验证 `verify_ragsd.c`: 分数与 Python 参考完全一致 (PASS)
- 🔌 **`.ino` 集成**: `rag_deep` 全局标志, JSON `"deep":true` 触发,
  `utf8_to_token_id()`/`append_sd_evidence()` 证据注入, setup() 调 ragsd_init()
- 🏗️ **编译**: esp32_llm_zh_v3.ino.bin 1381KB ✅
- 🐛 **修复**: 编译 libsdetect 死锁 (arduino-cli 1.5.1 Windows bug) → 复用旧 build 目录增量编译

---

## v3 环境（蒸馏版本，独立）

### 2026-08-02: P3 RAFT 格式对齐落地（zh6-raft3）
- 🎯 **根因定位**: raft1 训练格式 `BOS <user> E <end> <assistant>` 无 question,
  固件推理格式 `BOS <user> E1 \n E2 \n QUESTION <end> <assistant>` 有 question+双证据
  → 训练/推理分布偏移 = 复述保真度有限的真根因(Top-1 注入退化也由此解释,941f21b)
- 🛠️ **落地**: `build_raft.py --top2` 对齐固件注入格式(E1/E2 同条目 answer 两段 +
  真实 question),重训 `zh6-raft3`(resume zh6-distill, 1500 步, 60s)
- 📊 **验证**: val ppl 1.0(vs raft1 2.7);"肺癌早期症状"逐字忠实复述证据
  (raft1 有"气胸/呼吸困难"自由发挥),双 seed 一致
- 📦 **产物**: `firmware/model_v3/model.bin` 8.5MB, 主机 verify **PASS** (max abs diff 1e-5)
- 🗑️ **废弃**: `[证据]` 前缀方案(139d590)因固件模板无前缀而回归,`--prefix-marker` 保留参考

### 2026-08-02: 版本号标签 + RAG Top-2 回滚
- 🏷️ **固件版本号 `v3.2.1`**: 语义化版本(3=v3 线, 2=显示+采样里程碑, 1=RAG Top-1 回滚修复),
  定义于 `display.h` `#define FW_VERSION "v3.2.1"`,注释说明 bump 规则
- 🎨 **顶部通知栏版本号标签**: header 第二行 WiFi 图标+SSID+版本号**整体居中**显示
  (组中心 x≈205, 屏幕中心 200),版本号在 WiFi 名后 6px 间隙;
  截图逐像素验证: [📶]IP v3.2.1 居中, 右侧温度/电池不受影响无重叠
- 🔧 **RAG 注入回滚 Top-1→Top-2**: 设备实测 Top-1 全面退化(癌症肿瘤 12 tok 早停 /
  感冒如何**空输出** / 肺癌早期症状 9 tok),Top-2 提供冗余证据供 raft 复述
  恢复后: 癌症肿瘤 21 tok / 感冒如何 37 tok 连贯文本
  教训: PC 检索检查(文档含完整答案)≠ 设备生成质量,单证据不足时 raft 早停
- 提交: `941f21b`(Top-2 回滚)

### 2026-08-02: 设备侧打磨 — footer 间距 + P1 采样调优 + 默认提示词修正
- 🎨 **footer 字段间距重排**: RAG/Ntok/Time 字段 x+126/168/204(原 +120/156/192),
  每字段保持 ≥6px 可见间隙;截图逐像素验证落位 7/55/91/133/175/211/326,
  无重叠(此前 V→RAG 间隙 0px 重叠),日期右对齐 x=390<392 无溢出
- ✅ **P1 采样调优落地**: `SAMPLING_TEMP 0.7→0.6` + `REPETITION_PENALTY 1.4→1.5`
  (此前 CHANGELOG 记录"5 组参数无显著差异"基于 PC 端;设备端实测改善明显)
  实测对比:
  ```
  调优前: 感冒发烧要在床上擦拭清洗，每天用热水澡20浸泡身头后...
  调优后: 感冒发烧超过38号表示感冒发热引起炎症引起的咳嗽。建议您到正够在医院进
  ```
  噪音开头(NSV-80岁 类)消失,内容连贯度显著提升;3.11 tok/s 性能不变
- 🔧 **默认提示词修正**: `DEMO_PROMPT_IDS` 原 `{269,88,11,358,204}` 实际解码
  **"糖尿病二型"**(269糖/88尿/11病/358二/204型),注释却误标"感冒发烧";
  改为 `{66,1109,16,838}` = **"感冒发烧"**(感66/冒1109/发16/烧838,词表解码验证),
  设备串口确认 `[timeout] using demo prompt感冒发烧...`
- 提交: `891d192`(footer spacing + sampling tuning + demo prompt fix)

### 2026-08-02: RAG 优化验证（P1-P3）
- ✅ **P2 证据精化（真根因）**: answer-only 索引 → 噪声彻底消除
  ```
  根因: 证据含患者问句部分（"我现在是肺癌晚期了，每天反反复"）污染生成
  修复: build_index.py 只存 answer 段
  效果: "肺癌丸肝癌...痰痰胸骨颈" → "低热、咳嗽、咯痰、胸痛、气闷"（零噪声）
  验证: Top-1 注入优于 Top-2（避免错误证据稀释）
  ```
- ❌ **P1 采样调优**: 5 组参数（temp 0.5-0.7/rep 1.4-1.6）无显著差异 → 保持基线
- ❌ **P0 混合量化**: 8-bit core 不降噪 + 超分区（11.09MB > 8.98MB）→ 放弃
- 📌 **P3 分区/速度**: model 余量 61KB（紧）、kb 余 166KB、~2.9 tok/s（可接受）
- 提交: `3ff61b8`（answer-only 索引落地）

### 📌 V3 数据生成来源与具体方式（补充）

| 数据 | 来源 | 具体方式 |
|---|---|---|
| **预训练语料** | 魔搭 `zjydiary/Medical`（shibing624/medical 镜像） | 医学百科 361,420 条 + 教材 8,475 条 → 清洗去 LaTeX/表格 → 100M 字符 → v3 词表（min_freq=1）编码 99.3M tokens |
| **SFT 指令数据** | 三个高质量池（均 Qwen/DeepSeek 等大模型生成） | ① `zjydiary finetune`（1.95M QA）采样 30K ② `BenTsao 本草`（GitHub SCIR-HI 8.6K）全量 7.2K ③ `HuatuoGPT2-GPT4-140K`（hf-mirror 142K）采样 20K → **57K 多源混合** → shifted-labels 编码 |
| **RAFT 证据数据** | `Huatuo26M-Lite`（93.5K 医学 QA） | answer[:60] 为自引用证据 → answer 为目标，20K 对（教"抄写证据"） |
| **词表** | 医学语料统计 | min_freq=1 保留低频字符 → **7,563 词表**（v2 6,594） |

**数据蒸馏本质**：SFT 数据全部来自 Qwen3-0.6B/DeepSeek/HuatuoGPT2 大模型生成的高质量医学问答——学生（15.8M PLE）从"教师产出"学习，即数据蒸馏（logits KL 因词表不匹配不可行）。

### 2026-08-02: V3 固件构建 + 部署文档修正
- ✅ **v3 固件完成**: `esp32_llm_zh_v3/` 由 v2 结构构建(ino/display/cjk_font/rag.h),
  vocab.h 7563 词表, 特殊 token 用通用公式 `N-3/N-2/N-1`(词表末尾),
  DEMO_PROMPT_IDS "糖尿病二型" [269,88,11,358,204], 编译 1347KB ✅
- 🔧 **分区复用**: v3 model.bin 8.92MB 复用 v2 分区表(model 0x170000 8.98MB 余 0.06MB,
  kb 0xA00000 2MB), 固件可独立烧录
- 🔧 **GETTING_STARTED §12 修正**: 过时英文分区 0x110000/0xEE0000/1MB-factory
  → 实际 0x170000/0xE80000/1.375MB(与 partitions.csv 一致)
- ✅ **端侧实测**: v3 固件+模型烧录成功(V=7563 D=160 L=8 F=374 P=192, verify PASS)。
  **zh6-raft 是 RAG 依赖模型**——无证据 prompt 输出空(不编造),带证据(RAG 注入)
  时基于证据复述生成(实测"感冒如何治疗"+证据 → "引起感冒是由病毒感染引起的症状...建议就医检查")
- ✅ **V3 RAG 索引烧录**: `data_v3/kb/index.bin`(1.83MB, RAG1 magic, 10000 docs,
  2566 terms, vocab 7563)烧录到 kb 分区 0xA00000, 设备日志确认 `RAG ready: 10000 docs, 2566 terms`
- ✅ **端到端 RAG 问答**: 输入"肺癌早期症状"→ 固件自动检索 kb → 证据注入 →
  zh6-raft 生成"肺癌早期...咳嗽反应逐渐加重（因胸膜癌破坏了声音嘶哑）"——证据复述完整链路打通
- 🎨 **显示优化**: 顶部标题"ESP32-S3 PLE LLM"**居中**显示(实测偏移 -2px);
  footer **Model 字段精确三档**(v3=15.8M/v2=13.7M/v1=12.5M,按 VOCAB_N);
  footer **RAG 状态字段**(RAG10K/noRAG,显示 kb 索引文档数)替换硬编码 S3-N16R8
- 🎨 **footer 完善**: Model 字段改显 **V<词表>**(V7563 更精确);
  RAG 后补充 **N<生成token数>** 和 **<耗时>s**(如 N104/42s);
  实测渲染 V7563|RAG10K|N104|42s|08/02 22:26,无溢出(最右 x=390<392)

### 2026-08-02: V3 蒸馏完成（P0-P4）
- ✅ 数据: 7,563 词表（min_freq=1）+ 50K SFT + 99M tokens
- ✅ zh6 预训练 15.8M（val ppl 22.35）
- ✅ zh6-distill 数据蒸馏 SFT（val ppl 9.2）——Qwen3-0.6B 数据蒸馏
- ✅ zh6-raft（val ppl 2.7，证据复述：咳嗽/咳痰/胸痛）
- ✅ model.bin 8.92MB + vocab.h 7,563（单台 ESP32 ✅）
- 🔧 关键决策: logits KL 蒸馏不可行（Qwen 151K BPE vs 学生 7.5K 字符词表不匹配）→ 改数据蒸馏
- 🔧 修复: val.bin 编码 0 尾部 bug
- 提交: `dc804a2` `15eb35d` `cb7ff6f` `f041de5`

## v2 环境（医学数据，独立版本）

### 2026-08-01: P3.5 RAFT 微调（证据复述能力）
- ✅ build_raft.py: 20K 自引用证据对（answer[:60] → answer），GPU 50s 训练
- ✅ val ppl 2.6；验证: 给"收缩压≥140mmHg"证据 → 模型精确复述
- 📌 **RAG 完整生成与生效机制**（端到端）:
  ```
  【知识库生成】(PC 一次性)
  Huatuo26M-Lite(93.5K QA) → build_index.py
    → 字符级倒排索引 1.83MB（doc_off + doc_ids + inverted）
    → 烧录到 ESP32 "kb" 分区 (0xA00000, 2MB)

  【设备端生效】(每次提问)
  ① 串口收到问题(PC 已 token 化) → decode_question 还原文本
  ② rag.h 检索: 问题字符 → 倒排/线性扫描打分 → top-2 医学QA片段
  ③ rag_augment_prompt: BOS <user> [KB1][KB2] 问题 <end> <assistant>
  ④ RAFT 微调模型(ple-raft) 基于证据续写 → 输出有依据回答

  【为什么 RAFT 关键】
  小模型(13.7M)无事实记忆 → 单纯 RAG 不引用证据
  RAFT 训练"证据→答案"复述 → 模型学会提取证据内容
  本地验证: 无 RAG 时严重退化(重复), 有 RAG+RAFT 引用咳嗽/胸痛 ✅
  ```
- ⚠️ **当前瓶颈**: 检索噪声(肠癌/肝癌误命中)被 RAFT 忠实复述 → 需 IDF 加权
- 提交: `b13ce8b`

### 2026-08-01: P3 RAG 设备端检索（B3 混合方案）
- ✅ Huatuo26M-Lite 93.5K 知识库 + 1.83MB 倒排索引
- ✅ rag.h 检索器（线性扫描 TF-IDF，本地验证 23-53ms/10K）
- ✅ 固件集成（kb 分区 + 检索片段注入 prompt）
- 📌 **RAG 生效原理**（解释）:
  ```
  用户问题 → 字符级检索(Huatuo26M-Lite 知识库) → top-K 医学QA片段
    → 拼入 prompt: [KB证据] + [问题] → 模型基于证据续写回答
  本质: 用外部知识库弥补 12.5M 小模型"无事实记忆"的缺陷
        检索提供"答案锚点"，模型负责组织语言
  ```
- 📌 **配合模型使用方式**:
  - 设备端: 串口收问题 → RAG 检索 → 模型生成（完全离线）
  - PC 端: `sft_generate.py` 可手动拼 KB 片段验证
- 📌 **本地验证结果**（未烧录）:
  - 检索: 23-53ms ✅ 达标
  - 胃癌问题: RAG 后输出结构化"1. 胃部不适" ✅ 提升
  - 检索噪声: "肺癌"命中"肠癌/外阴炎"（共享高频词）⚠️
  - 模型证据利用: 有限（未 RAFT）⚠️
- 📌 **优化空间分析**:
  | 优化 | 说明 | 优先级 |
  |---|---|---|
  | IDF 加权检索 | 稀有词权重更高，过滤"临床表现"等高频词 | ⭐⭐⭐ |
  | RAFT 微调 | DeepSeek 生成"检索+回答"对，教模型抄写证据 | ⭐⭐⭐ |
  | 片段精化 | 只取 answer 段（跳过 question），提升证据质量 | ⭐⭐ |
  | 扩大知识库 | 10K → 50K 文档（PSRAM 允许 ~3MB） | ⭐ |
- 提交: `c0e7bfc` `eb950d8` `276e506`

### 2026-08-01: SFT 数据源扩充（本草 + HuatuoGPT2）
- ✅ 本草 BenTsao: 8,658 条高质量中文医学指令（GitHub SCIR-HI，2.9MB）
- ✅ HuatuoGPT2-SFT-GPT4: 142,248 条（hf-mirror，245MB，分 4 次断点续传）
- 🔧 未下载 Firefly（通用指令，可选）
- **为什么要扩充**（补充说明）:
  - 当前 v2 SFT 30K 全部来自 zjydiary finetune **单一数据源**——同源采样导致指令风格/分布同质化，模型学到的是"该数据源的问答模式"而非通用医学问答能力
  - 本草（结构化 QA）+ HuatuoGPT2（多轮对话）来自**不同生成方**，指令分布互补，可提升模型对未见问题的泛化
  - 实验依据：v1 曾因数据同质化（模板/单源）导致问答能力弱，多源混合是已验证的改进方向
- 数据路径: `data_v2/raw/benchao/` + `data_v2/raw/huatuogpt2/`
### 2026-08-01: 设备端 CJK 显示 + v2 推理根因修复
- ✅ **RLCD 中文显示**: 从 XiaoZhi `font_noto_qwen_14_1.bin` 提取 7854 个 14×14 1bpp 字形
  （GB2312 汉字覆盖 98.71%，含全部医疗生僻字 腺/癌/龋/颧/癃 等），生成 `cjk_font.h`
- ✅ **display.h CJK 渲染**: UTF-8 3 字节解码 → 二分查找 CJK_CP → 14×14 字形渲染，
  缺字显示 □；prompt 2x 区支持中文；生成光标(2px 竖条) + 生成后清除
- ✅ **v2 固件完整化**: `esp32_llm_zh_v2/` 补齐 display.h/display_bsp/cjk_font/ino，
  vocab.h 用 v2(6594)，DEMO_PROMPT_IDS 适配 v2 词表(285/995/1186)
- ✅ **repetition penalty**: 固件采样加入 rep-penalty(1.3)+ 历史窗口(50)，
  抑制小模型重复循环（v1/v2 同步）
- 🔧 **v2 推理 NaN 根因修复**: `chinese/export.py` 调用 `quant_pack(t)` 未传 group，
  默认用了 src 的 GROUP=128 但文件头写 32 → 布局错位 → C 端 NaN → 生成退化
  （"痞痞痞..."块循环）。修复为 `quant_pack(t, group=GROUP)`
- ✅ **C 端验证**: WSL 重导出 model.bin 7.71MB，verify.c PASS（max abs diff 0.00001）
- ✅ **端侧实测**: 医学 prompt 生成有意义中文（"喑是很正常人"），无 NaN 块循环
- 🔧 **footer 模型参数修复**: 底部状态栏"28.9M"(英文模型)改为按 VOCAB_N 显示实际
  模型(v1=12.5M, v2=13.7M)，不再误导
- ✅ **防御性修复**: export.py/quantize.py 增加 NaN/Inf 清零；verify.c probe 越界修复
- 提交: (本批次) (feat: RLCD CJK display (7854 glyphs) + v2 firmware + fix v2 NaN root cause (group param))

### 2026-08-01: 设备端显示打磨 + 默认提示词医疗化
- 🔧 **prompt 区去除 SFT 标记**: display_draw_prompt_2x 不再渲染 `<BOS>`/`<user>`/
  `<end>`/`<assistant>` 字面文本，只显示真实用户问题（v1/v2 词表自适应）
- 🔧 **屏蔽生成期 BOS token**: 模型不再在生成中输出 `<BOS>`(id 2)标记文本，
  与 `<user>`/`<assistant>` 同样屏蔽（v1/v2）
- 🔧 **footer 显示实际模型**: 底部状态栏按 VOCAB_N 显示 v1=12.5M / v2=13.7M，
  替换原英文模型硬编码 "28.9M"
- 🔧 **默认提示词医疗化**: DEMO_PROMPT_IDS "本报告" → **"糖尿病二型"**
  （v2: [269,88,11,358,204]，v1: [716,407,31,132,267]）
- ✅ 端侧实测: "感冒如何治疗"/"甲状腺结节切除"生成通顺中文，无标记泄漏无光标残留
- 提交: `19d1bb6` `453786d` `335eff7` `cedc2b2`

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
