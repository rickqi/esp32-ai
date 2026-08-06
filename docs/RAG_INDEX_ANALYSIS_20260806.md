# RAG 索引方案分析 (2026-08-06, 含 08-06 重建实况)

> 供 esp32-ai 项目决策。基于 V5_RAG_verification_20260805.md (权威验证) + minimind 侧 2026-08-06 KB/索引修复实况。
> 目标: 明确三条索引链的现状、质量、代价, 给出实际选择建议与部署方式。

---

## 1. 三条索引链现状总览

| 维度 | **Index A: PC jieba** | Index B: 单字 SD | Index C: flash kb (RAG1) |
|---|---|---|---|
| 位置 | `minimind/out/rag_index.pkl` (PC) | `data_v4/sd_rag/{index,docs,meta}.bin` | `data_v4/kb/index.bin` |
| 数据源 | `data_v4/kb/format_data.jsonl` (11000 医学) | format_data.jsonl 医学成品 (10,999) | kb 采样 ~30K |
| 检索 | jieba 词 IDF | 单字 IDF | 字符 IDF |
| docs/terms | 11,000 / 29,849 (词) | 10,999 / 3,053 (字) | ~30K / char ids |
| 证据长度 | 60 字 | 40 字 | 50 字 |
| 大小 | 3.6MB | 3.9MB (SD 卡) | 2.05MB |
| V5 使用 | ✅ **PC 注入活链** | ⚠️ **IDF 变体键盘/预设路径活**; Arduino 死代码 | ❌ 仅 v2/v4 |

**架构事实**: `esp32_llm_zh_v5.ino:28` 定义 `MM_MINIMIND`; `:633` `#ifndef` 包裹 `rag_augment_prompt()` → Arduino 变体设备端检索(含 SD deep)永不执行。**IDF 变体 (`esp32_llm_v5_idf`) SD RAG 已活**(有设备端 BPE 编码器), 键盘/预设路径走检索, UART ids 路径纯推理。详见 §9 支持矩阵。

---

## 2. 检索质量实证 (17 查询对比, 来自权威验证文档)

| 查询 | Index A (jieba) | Index B (单字 SD) |
|---|---|---|
| 宫外孕 | ❌ 植发 (错) | ✅ HCG 妇产科 (对) |
| 肝豆状核 | ❌ 代谢综合征 (错) | ✅ 肝豆状核/帕金森 (对) |
| 糖尿病临床表现 | ✅ 指南 (对) | ❌ 甲亢 (错!) |
| 肺癌早期症状 | ✅ answer-only 干净 | ❌ 患者口语 (噪) |

**结论: 无单一索引全胜**。jieba 词级在"整词匹配"场景优, 单字在"术语切碎"场景优, 但都各有关键失败。

> ⚠️ **重要修正 (2026-08-06 实测)**: 上表对比的是**修复前无医学词典的旧 jieba**。修复后 (加载 368 词医学词典 + KB 100% 医学), 结论已反转 — 见 §2.1。

### 2.1 实测对比 (2026-08-06, 统一 10 查询, Top-1 相关性人工判定)

对修复后的 Index A (jieba + 医学词典) 与 Index B (单字 SD, 抽样 20000/136877 docs) 实测:

| 查询 | Index A (jieba 修复后) | Index B (单字 SD) |
|---|---|---|
| 肺癌早期症状 | ✅ 早期诊断肺癌 | ✅ 小细胞肺癌咳嗽咯血 |
| 高血压诊断标准 | ⚠️ 促红细胞生成素 (偏) | ✅ 动态血压 24h 平均 |
| 糖尿病临床表现 | ✅ 临床表现章节 | ✅ 糖尿病不能根治 |
| 肝硬化腹水 | ✅ 肝硬化征象腹水 | ✅ 肝硬化腹水治疗 |
| 急性心肌梗死 | ✅ 急性心肌梗死 | ✅ 心肌梗死心功能 |
| 宫外孕 | ✅ 宫外孕治疗方式 | ✅ HCG 药物治疗 |
| **肝豆状核变性** | ✅ **精准命中** | ❌ **梅核气/慢性咽炎 (错)** |
| 上消化道出血 | ✅ 上消化道呕血黑便 | ⚠️ 下消化道出血 (近似) |
| 不孕不育 | ✅ 宫颈糜烂不孕 | ✅ 继发性不孕 |
| **白疕** | ✅ **精准命中** | ❌ **白癜风 (近似非目标)** |
| **Top-1 相关率** | **90% (9/10)** | **80% (8/10)** |

**结论 (修正)**: 修复后 Index A (jieba) **90% > SD 单字 80%**, 且在多字术语场景 (肝豆状核/白疕) 明显更准。SD 单字倒排的失败根因: "豆/核" 等单字 IDF 饱和 255 → 含"核"的无关文档 (梅核气) 抢 Top-1, 字符级索引无法识别多字术语边界。

**补充**: V3 OPTIMIZATION.md 的"Top-5 全命中肺癌QA"是 **FLASH kb 索引** (10000 docs) 的窄验证 (仅 1 查询), 常被误认为 SD 索引结论; 且 SD 索引停在 08-03, 未随 08-06"KB 100% 医学"修复更新。

> **更新 (2026-08-06)**: SD 索引已按医学过滤重建 (见 §3.4)。重建后 113,609 docs (过滤 23K 非医学), 肺癌/宫外孕/酮症酸中毒 命中更准, 但**肝豆状核/白疕 仍误配** (梅核气/白癜风) — 这是单字倒排的结构性局限, 数据纯度无法根治字符级切分问题。

---

## 3. 2026-08-06 修复实况 (本轮已完成)

### 3.1 minimind 侧 (PC 活链 Index A)
- **RAG 索引修复**: `cmd_build` 加 `med_only=True` (排除健康管理/理赔/销售); `cmd_query`/`cmd_chat` 补 `load_medical_dict()`; 重建 `rag_index.pkl`
- **修复效果**: 上消化道出血/不孕不育 由"无匹配"→精准命中; 词典覆盖率缺失 57%→36%
- **KB 病种覆盖修复** (esp32-ai 侧驱动): `build_guide_kb.py` 正则加 `变性/肝豆状核/黄斑变性` + 长度下限 80→40 + `is_medical_label` 过滤
  - 肝豆状核变性 0→4 条, 戊型肝炎 0→1 条, 肱骨外上髁 0→2 条
  - **KB 现 100% 医学** (11000 条, 非医学 6837→0)

### 3.2 Index B 现状 (SD 索引: 11K 医学成品 v3, 已提交)
- **2026-08-06 v3 重建**: `build_sd_index.py` 改为优先读 `format_data.jsonl` (build_guide_kb 产出的 11K 100% 医学成品), 回退 V3+guides 重建
- **对比实测 (统一 10 查询)**: v3 (11K) **90% > v2 (113K) 80%**
  - 肝豆状核: 113K 命中"梅核气"(错) → 11K 命中"肝豆状核"(对)
  - 白疕: 113K 命中"白癜风"(近似错) → 11K 命中"白疕"(精准)
  - Oracle 建议成立: 精度>召回, format_data.jsonl 病种覆盖修复后是高精度医学数据
- 结果: **10,999 docs** (11K 医学成品), index.bin 2.46MB + docs.bin 1.40MB + meta 14.7KB = **3.9MB** (SD 卡容量充足, 且省 PSRAM)
- **已提交推送**: esp32-ai commit (v3 脚本 + 11K 索引)
- **部署**: 三文件拷入 SD 卡 `/sdcard/rag/` (FAT32) → `esp32_llm_v5_idf` 启动自动加载 (`rag_sd.h` 路径已硬编码)
- 固件侧: `esp32_llm_v5_idf` 可直接消费 (格式未变); `esp32_llm_zh_v5` 维持死代码
- **⚠️ 维护注意**: 此产物已在 git 追踪; 若本地重跑 `build_sd_index.py` 会覆盖磁盘文件, 如需还原用 `git checkout <commit> -- data_v4/sd_rag/`

### 3.3 Index C 现状 (flash RAG1)
- `data_v4/kb/index.bin` 随 build_guide_kb.py 重建 (2.05MB, 08-06), 但仅 v2/v4 字符级固件消费

---

## 4. 已知缺陷与根因

| 缺陷 | 根因 | 修复状态 |
|---|---|---|
| jieba 术语切碎 (宫外孕/肝豆状核假匹配) | 医疗多字术语被默认词典切碎 | `medical_jieba.txt` 368 词条已加载; Index A 修复后肝豆状核命中 |
| Index A 别名不可命中 (网球肘) | answer 60字截断 + 别名不匹配 | 已知限制 (设计权衡), 不改 |
| send_prompt_rag.py 截断静默丢问题 | `ids[:keep]+ids[-4:]` 逻辑 | 已修复 (按预算截断证据, 保问题+assistant) |
| IDF 98.3% 饱和 | ESP32 uint8 硬约束 (255 cap) | **不建议移除** — 加法打分下对排序无实质影响 |

---

## 5. 实际选择建议 (供决策)

### 场景 A: 现状 V5 (tethered, PC 注入) — **推荐维持**
- 活链 Index A (jieba) 已修复到最佳状态, KB 100% 医学
- 生成质量: RAG 92-100% vs 无RAG 0% (决定性有效)
- 速度: decode 影响 <2%, prefill 2.4x (可接受)
- **无需任何进一步索引工作**

### 场景 B: 需要离线 RAG (无 PC) — SD 索引已就绪
- **SD 索引 (113,609 docs, 医学过滤) 已重建并提交** (`0b4c432`), 直接拷入 `/sdcard/rag/` 即可
- 固件: `esp32_llm_v5_idf` 原生消费 SD 索引 (格式匹配); `esp32_llm_zh_v5` 需移除 MM_MINIMIND 守卫
- **代价**: 检索质量 80% < PC jieba 90%, 且肝豆状核/白疕 字符级误配是结构性局限
- **结论**: 离线 RAG 现可直接部署 (SD 索引 + esp32_llm_v5_idf); PC 联动场景仍推荐 jieba (Index A)

### 场景 C: 追求检索质量上限 (实验性)
- 第四种索引 (BPE 子词级) 可填补 jieba 与单字盲区, 但需 C 端 BPE 实现 (工作量大)
- **不建议现在投入**

---

## 6. 决策树 (简版)

```
需要 RAG 吗?
├─ 否 → 直接部署 H1/H2 (无 RAG, 泛化弱, 不推荐)
└─ 是 → 允许 PC 联动?
    ├─ 是 → V5 + Index A (jieba) ✅ 推荐 (当前已就绪)
    └─ 否 (纯离线) → v3/v4 字符级固件 + Index C (flash kb)
        └─ 需接受字符级检索局限 + 重新构建/烧录
```

---

## 7. 结论

1. **当前 V5 架构下, Index A (PC jieba) 是唯一正确选择, 且已修复到最佳状态** — 无需切换到 Index B/C
2. **实测确认 (10 查询): 修复后 Index A 90% > SD 单字 80%** — "SD 优于 jieba"的旧结论已过时 (基于修复前无词典 jieba + 单一查询窄验证)
3. **SD 索引 (Index B) 已医学过滤重建 (113,609 docs, 47.5MB, commit 0b4c432)**: 数据已纯净, 供 `esp32_llm_v5_idf` 离线 RAG 直接使用; 肝豆状核/白疕 字符级误配是结构性局限
4. Index C (flash RAG1) 仅 v2/v4 旧固件消费, 已随 KB 重建
5. 真正值得的后续投入: **jieba 词典持续扩充** (医学词条 368→更多) + **KB 病种覆盖补充** (已部分完成) + **V5_TIERED_RAG_PLAN 分级架构** (flash 优先 → SD 兜底, 前瞻设计)

---

## 8. 索引生成与部署速查

### 生成命令
```bash
# PC jieba 索引 (minimind)
cd /mnt/d/codes/minimind && python3 scripts/rag_medical.py build

# SD 全量索引 (esp32-ai, 医学过滤)
cd /mnt/d/codes/esp32-ai && python3 chinese_v4/kb/build_sd_index.py
python3 chinese_v4/kb/build_sd_index.py --verify-only   # 验证

# flash RAG1 索引 (esp32-ai)
python3 chinese_v4/kb/build_guide_kb.py
```

### 部署到 esp32
| 索引 | 文件 | 目标位置 |
|---|---|---|
| SD 离线 RAG | `data_v4/sd_rag/{index,docs,meta}.bin` | SD 卡 `/sdcard/rag/` (FAT32) |
| flash KB (v2/v3) | `data_v4/kb/index.bin` | flash 分区 0xA00000 (v2) / 0xA60000 (v3) |

### 索引文件清单 (最终确认)
| 文件 | 大小 | 用途 |
|---|---|---|
| `esp32-ai/data_v4/sd_rag/index.bin` | 2.46MB | SD 离线 RAG (10,999 docs, 11K 医学成品) |
| `esp32-ai/data_v4/sd_rag/docs.bin` | 1.40MB | 证据文本 |
| `esp32-ai/data_v4/sd_rag/meta.bin` | 14.7KB | 单字 IDF 表 |
| `esp32-ai/data_v4/kb/index.bin` | 2.05MB | flash RAG1 (v2/v3) |
| `minimind/out/rag_index.pkl` | 3.6MB | PC jieba (评估) |

---

## 9. V5 固件索引部署模式支持矩阵 (2026-08-06 代码实证)

### 9.1 支持矩阵

| 部署模式 | esp32_llm_zh_v5 (Arduino) | esp32_llm_v5_idf (ESP-IDF) |
|---|---|---|
| **(1) Flash RAG1 单文件** | ❌ 不支持 | ❌ 不支持 |
| **(2) SD 三文件** | ❌ 死代码 (永不执行) | ✅ **键盘/预设路径已活** |
| **(3) 分级 Flash+SD** | ❌ 无级联代码 | ❌ 无级联代码 (计划未落地) |

### 9.2 关键代码证据

| 事实 | 位置 |
|---|---|
| Arduino: `#define MM_MINIMIND` 无条件定义 | `esp32_llm_zh_v5.ino:28` |
| Arduino: `rag_augment_prompt()` 被 `#ifndef MM_MINIMIND` 包裹 → 死代码 | `esp32_llm_zh_v5.ino:633-638` |
| Arduino: `rag_init()`(flash mmap)+`ragsd_init()`(SD) 在 setup 调用但结果永不注入 | `esp32_llm_zh_v5.ino:869,871` |
| Arduino: **无设备端 BPE 编码器** (证据注入靠 PC) | 无 bpe_encoder.h |
| IDF: `rag_retrieval.c` 是 53 行 SD-only 封装, 只调 `ragsd_retrieve` | `rag_retrieval.c:34-52` |
| IDF: **有设备端 BPE 编码器** (ByteLevel, 8/8 HF 匹配) | `bpe_encoder.h` |
| IDF: SD RAG 活用于键盘/预设路径 | `board_rlcd.cpp:407,461` |
| IDF: UART `{"ids":[]}` 路径纯推理 (PC 已编码) | `board_rlcd.cpp:677-696` |
| 两个变体分区表均无 kb 分区 (model 占 14.5/16MB) | `partitions.csv` |
| V5_TIERED_RAG_PLAN 分级架构为纯设计稿 (步骤 0/3 未实现) | `V5_TIERED_RAG_PLAN_20260806.md` |

### 9.3 各模式部署结论

**模式 (1) Flash RAG1**: ❌ 当前不可行。16MB flash 被 model (14.5MB) 占满, 剩余 ~24KB; RAG1 索引 2.05MB 无处放置。需缩 model (H1 版 ~8MB) 或换 ≥32MB flash 芯片。

**模式 (2) SD 三文件**:
- **IDF 变体已可用**: `data_v4/sd_rag/` 三文件 (11K v3 医学成品, 3.9MB) 拷入 SD 卡 `/sdcard/rag/`, 走键盘/预设输入路径 — **零代码改动**
- **Arduino 变体需移植**: ① 拷 IDF 的 bpe_encoder.h/bpe_tables.h ② 写 build_rag_prompt 等价物 ③ 解除 MM_MINIMIND 守卫 ④ (可选) rag_sd.h 哈希表改动态

**模式 (3) 分级**: ❌ 当前无级联代码。需先实现模式 (1) 的 flash tier (含 kb 分区), 再在 `rag_retrieval.c` 加 `#ifdef CONFIG_KB_PARTITION` flash→SD fallback 级联。

### 9.4 部署路线建议

```
当前立即可用 (零改动): IDF 固件 + SD 卡 11K v3 索引 + 键盘/预设路径
        ↓ (如需 Arduino 支持)
中等工程量: 移植 BPE 编码器到 Arduino + 解除 MM_MINIMIND 守卫
        ↓ (如需 flash/分级)
需换硬件: 缩 model 或 32MB flash → 加 kb 分区 → flash tier + 分级级联
```
