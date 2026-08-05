# H1/H2 RAG vs 无 RAG 验证数据汇总 (2026-08-05)

## 1. 模型产物确认 (RAFT 版已烧录)

| 模型 | model_llm.bin | 大小 | 配置 | 词表 |
|---|---|---|---|---|
| H1 | firmware/model_v5/H1/model_llm.bin | 6.31MB | V=6400 D=256 L=6 H=8 F=832 P=96 S=128 | 6400 BPE |
| H2 | firmware/model_v5/H2/model_llm.bin | 14.73MB | V=6400 D=384 L=8 H=8 F=1216 P=128 S=128 | 6400 BPE |

- 均来自 RAFT 训练权重 (full_sft_h1_raft_256_ple.pth / full_sft_h2_raft_384_ple.pth, 08-05)
- C 端 verify_h2 PASS (H1 top=334, H2 top=42, max diff ≤1e-5)

## 2. 生成效果 (GPU, minimind 真实 tokenizer + ChatML)

| 问题 | H1 无RAG | H1 RAG | H2 无RAG | H2 RAG |
|---|---|---|---|---|
| 肺癌早期症状 | 0% 泛泛(发热/乏力/肾损伤混入) | 100% 复述证据 | 0% 重复循环(肺脓肺肝肾×N) | 100% 复述 |
| 高血压诊断标准 | 0% 循环(Agggggg) | 100% 复述 | 0% 泛化(血脂处理) | 100% 复述 |
| 糖尿病临床表现 | 0% 泛化(生理功能) | 100% 复述 | 0% 发病机制循环 | 100% 复述 |
| 病毒性肝炎治疗 | 0% 泛化 | 100% 复述 | 0% 泛化 | 100% 复述 |
| 感染性休克血象 | 0% 泛化 | 92% | 0% 肿瘤标志物错误 | 100% 复述 |

**结论: RAG 决定性有效。** 无 RAG 时两模型都退化/幻觉,有 RAG 时 RAFT 训练的证据复述能力完整发挥。

## 3. GPU 推理速度 (Python, tok/s)

| 模型 | 无RAG (prompt 26tok) | RAG (prompt 163tok) | 差异 |
|---|---|---|---|
| H1 | 127.83 | 141.33 | +10.6% (prefill 摊薄) |
| H2 | 107.47 | 104.52 | -2.7% |

## 4. 设备链路推理速度 (C 端 llm_v5.h, prompt 按固件 max_prompt=100 截断)

| 模型 | 模式 | prefill | decode 28tok | tok/s |
|---|---|---|---|---|
| H1 | 无RAG (42tok) | 0.16s | 0.110s | 254.91 |
| H1 | RAG (100tok) | 0.38s | 0.113s | 248.55 |
| H2 | 无RAG (42tok) | 0.45s | 0.309s | 90.68 |
| H2 | RAG (100tok) | 1.08s | 0.307s | 91.21 |

- decode tok/s: H1 ~250, H2 ~91 (RAG 影响 <2%)
- prefill: RAG prompt 是 2.4x 长度,prefill 时间 2.4x (H1 0.16→0.38s, H2 0.45→1.08s)
- 阶段占比: attn 21-26%, ffn 50-56%
- **注意**: 未截断的 RAG prompt 是 139-182 tok,超出 seq_len=128!固件 max_prompt=100 截断是必须的(证据预算 ~60tok)

## 5. 索引对比 (17 查询实证)

| 维度 | jieba (rag_index.pkl) | 单字 SD (sd_rag) | flash kb (RAG1) |
|---|---|---|---|
| 来源 | kb/format_data.jsonl | V3 KB + 全量指南 | kb 采样 30K |
| docs | 11,000 | 136,877 | ~30K |
| terms | 29,849 (词) | 5,099 (字) | char ids |
| 证据长度 | 60 字 | 40 字 | 50 字 |
| 检索 | jieba 词 IDF | 单字 IDF | 字符 IDF |
| 位置 | PC (pkl) | 设备 SD | 设备 flash |

质量分歧案例:
- 宫外孕: jieba→植发(错), SD→HCG 妇产科(对)
- 肝豆状核: jieba→代谢综合征(错), SD→肝豆状核/帕金森(对)
- 糖尿病临床表现: jieba→【临床表现】指南(对), SD→甲亢(错!)
- 肺癌早期症状: jieba→answer-only 干净(对), SD→患者口语叙述(噪)

## 6. 关键架构发现: 三个索引只有 Index A 在 V5 链路里活着

| 索引 | 位置 | 来源 | docs | V5 是否使用 |
|---|---|---|---|---|
| **A. jieba** | PC (rag_index.pkl / 脚本内构建) | data_v4/kb/format_data.jsonl | 11,000 | ✅ **唯一活链** (PC 端注入) |
| **B. 单字 SD** | 设备 SD (/sdcard/rag/) | V3 KB + 全量指南 | 136,877 | ❌ **死代码** (MM_MINIMIND 禁用) |
| **C. flash kb** | 设备 flash mmap | kb 采样 30K | ~30K | ❌ 仅 v2/v4 字符级固件用 |

- `esp32_llm_zh_v5.ino:28` 定义 `MM_MINIMIND`;`:633` 用 `#ifndef MM_MINIMIND` 包裹 `rag_augment_prompt()` → **设备端检索(含 SD deep 路径)永不执行**
- 设备只做纯推理;证据由 PC 端 jieba 检索 + MiniMind BPE 编码后以 `{"ids":[...]}` 串口注入
- **"注意 rag 索引的位置"的含义**:PC 验证脚本指向 data_v4/kb(11K),设备固件本可读 sd_rag(137K)但实际被禁用——验证链必须以 PC 端 jieba(Index A)为准

## 7. 🔴 新发现的严重 Bug: max_prompt=100 截断静默丢弃问题

`tools/send_prompt_rag.py` `build_chatml_ids()` 的截断逻辑 `ids[:keep] + ids[-4:]`:

```
完整 RAG prompt = 108 tok (系统17 + 证据64 + 问题11 + assistant + 标记)
max_prompt=100 截断 → 保留 证据前96tok + assistant → 问题部分被切掉!
```

**实证**:5 个 RAG prompt 截断后全部 `Q保留=False`(解码验证)——设备收到的是"无问题的证据"。

**修复方案**(已验证):按预算截断**证据**而非整体头部:
- 保留 system 头 + 证据标记 + 证据前缀(整句)
- 问题 + assistant 引导完整保留
- 修复后 102 tok,`Q保留=True`

## 8. Oracle 架构结论摘要

1. **V5 真相源 = Index A**(jieba 11K PC 端)。不要重建 jieba over 137K(会引入指南长文本噪声,且不解决 jieba 分词失败)
2. **jieba 失败根因**:医疗多字术语(宫外孕/肝豆状核)被默认词典切成常见单字 → 假匹配。**修复: `jieba.load_userdict()` 加 ~200 医学词条**(最低成本最高收益)
3. **设备端不放检索**:jieba 词典 ~5MB 不可移植;BPE 编码器无法在 C/ESP32 实现 → PC 端注入是正确终态架构
4. **证据表示**:answer-only 截断(当前 Index A)正确;SD 137K 噪声来自指南 body_text[:40] 截断捕获患者叙述
5. **架构代价**:V5 是"tethered"(依赖 PC),不像 v2/v3/v4 全离线——用生成质量换离线自主性;离线硬需求则保留 v3/v4 字符级固件
6. 待办:修 send_prompt_rag.py 截断 + 加 jieba userdict + 在 ino:28 加架构注释
