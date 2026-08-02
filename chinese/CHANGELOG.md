# 中文模型项目变更说明 (CHANGELOG)

> 分支: feature/ESP32-S3-4.2inch-RLCD
> 覆盖: chinese/ (v1) + chinese_v2/ (v2) 全部演进

---

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
