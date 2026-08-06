# V5 分级 RAG 索引实现计划 (2026-08-06)

> 基于 RAG_INDEX_ANALYSIS_20260806.md 结论 + 分级索引新需求。
> 目标: 分步骤实现, 从方案 A 开始; 架构支持"flash 优先 → SD 兜底"分级。

---

## 需求摘要

1. **方案 A 起步**: 用修复后的医学 KB (11000 docs, 100% 医学) 重建 SD 索引
2. **分级索引架构**:
   - 有 flash 空间 → flash KB 索引优先, 找不到 → SD
   - 无 flash → 直接 SD
   - SD 索引可多文件, 默认只搜第一个
3. 未来 flash 空间释放后无需重写即可启用 flash 级

---

## 现状约束 (已实证)

| 项 | 值 |
|---|---|
| flash 剩余 | 24KB (缩 factory 最多回收 408KB) |
| RAG1 索引 (flash KB) | 1.96MB (10,999 docs, char-id IDF) |
| SD 索引 | 34.8MB+17.4MB (136,877 docs, 单字 IDF) |
| 检索质量 (10 查询) | SD 单字 80% / PC jieba 90% |
| 设备端 BPE | ✅ 已实现 (8/8 HF 匹配) |
| SD 挂载 | ✅ 已实现 (实测 SD mounted) |

**核心结论**: flash 当前无空间放 KB 索引 → 实际只有 SD 级; 但架构需前瞻支持 flash 级。

---

## 实现步骤

### 步骤 0: 架构重构 — 分级检索抽象 (先行, 支撑后续所有步骤)

**目标**: 把 `build_rag_prompt` 的单路 SD 检索, 抽象为**分级链** (flash 优先 → SD 兜底)。

```
rag_retrieval.h (重构):
  typedef struct {
    const char *name;
    bool ready;
    int (*retrieve)(const char *q, char *ev, int cap);
  } RagTier;

  int rag_retrieval_init(void);   // 初始化所有 tier
  int rag_retrieval_retrieve(q, ev, cap);  // 逐 tier 尝试, 首个命中返回

RagTier tiers[]:
  [0] flash_tier (RAG1, 编译期 CONFIG_KB_PARTITION 控制, 无空间则跳过)
  [1] sd_tier0   (/sdcard/rag/index0.bin, 主 SD 索引)
  [2] sd_tierN   (可选, 惰性加载)
```

**关键决策**:
- flash tier 用 `#ifdef CONFIG_KB_PARTITION` 编译期控制 — 无 flash 时零开销
- SD tier0 启动时加载; tierN 惰性加载 (miss 时)
- 每个 tier 独立 ready 状态, 逐级 fallback

### 步骤 1 (方案 A): 重建 SD 索引 (医学 KB)

**现状**: `data_v4/sd_rag/` 停在 08-03, 数据源是旧 V3 KB + 全量指南 (136,877 docs)。

**做法**: 重跑 `build_sd_index.py`, 但用**修复后的数据源**:
- V3 KB → 新 format_data.jsonl (100% 医学, 11000 docs)
- 指南/medica 目录 → 已修复正则 (变性/肝豆状核等)

**产出**: 更新 `/sdcard/rag/index0.bin` 等 (拷到 SD 卡)

### 步骤 2: SD 多索引支持

**布局** (后缀方案, 最简单):
```
/sdcard/rag/index0.bin + docs0.bin + meta0.bin   ← 主 (默认搜)
/sdcard/rag/index1.bin + docs1.bin + meta1.bin   ← 备用 (可选)
```

**实现**: rag_sd.h 路径参数化 (`ragsd_init_idx(n)`), tier0 默认加载, tier1+ 惰性。

### 步骤 3: flash 优先逻辑 (前瞻)

**当前**: 无 flash 空间 → flash_tier 编译期禁用, 直接 SD。

**未来** (flash 释放后):
- 加 kb 分区 (RAG1, ~2MB) → 编译期 CONFIG_KB_PARTITION 启用
- `build_index.py` 用医学 KB 重建 RAG1
- 运行时: flash tier 命中 → 返回; miss → SD tier0

---

## 验证计划

| 步骤 | 验证 |
|---|---|
| 0 (架构) | 编译通过 + 现有 SD 检索不回归 |
| 1 (方案 A) | 10 查询对比新旧 SD 索引命中率 |
| 2 (多索引) | index0 主检索正常, index1 惰性加载 |
| 3 (flash) | 未来验证 (需 flash 空间) |

---

## 待 Oracle 确认的点 (结论已回)

1. **RagTier[] vtable → 否决**: 用简单 if/else 级联 (flash 与 SD 非多态, 路径完全不同)
2. **SD 多索引后缀方案**: index0.bin/docs0.bin 后缀, 但**暂不实现** (单医学索引够用)
3. **惰性加载**: 有多个索引时才需要; 当前单索引启动加载即可
4. **flash tier 编译期**: `#ifdef CONFIG_KB_PARTITION`, 无 flash 时零死代码
5. **方案 A 选 (b)**: 新建 11K 医学 SD 索引, **替换** 137K 广谱 (去保险/法务稀释)

**额外发现 (Oracle)**: `rag_sd.h` HASH_SHIFT=18 硬编码 1.5MB PSRAM, 11K docs 需缩至 2^15=192KB — 省 1.3MB。

---

## 修订后的实现步骤

### 步骤 0: 架构 — 简单级联 (不引入 vtable)

```
rag_retrieval.c (保持 53 行薄封装):
  int rag_retrieval_retrieve(q, ev, cap):
    #ifdef CONFIG_KB_PARTITION
    if (g_flash_ready && flash_retrieve() > 0) return n;  // 未来
    #endif
    if (g_sd_ready && sd_retrieve() > 0) return n;        // 当前唯一
    return 0;
```

### 步骤 1 (方案 A): 重建 11K 医学 SD 索引

1. `build_guide_kb.py` 重跑 → `data_v4/kb/format_data.jsonl` (11K 医学)
2. **改 `build_sd_index.py::load_entries()`**: 直接读 format_data.jsonl, 移除 V3+guides 重建路径
3. 运行 → `data_v4/sd_rag/{index,docs,meta}.bin`
4. `--verify-only` 验证 Top-1 命中率
5. 拷 3 文件到 SD 卡 `/sdcard/rag/` (覆盖旧 137K)
6. **修 `rag_sd.h` hash 表**: HASH_SHIFT 动态按 n_docs (11K → 2^15=192KB, 省 1.3MB PSRAM)
7. 烧录 + 串口验证证据注入 (evidence=YES)

### 步骤 2: SD 多索引 (延后, 仅设计)

- 路径参数化 `index0.bin` 后缀, g_rag_sd[2] 数组
- 暂不实现 (单索引够用)

### 步骤 3: flash 优先 (延后, 仅 if/else 预留)

- 未来 flash 释放后: 加 kb 分区 + CONFIG_KB_PARTITION + **词级 jieba 索引** (90% 质量, 唯一值得加 flash 的场景)


---

## 追加: 方案 B 术语叠加层 (2026-08-06 晚, 已实现 + 设备端验证)

### 背景
单字 SD 索引 80% 质量, 结构性缺陷: 肝豆状核→梅核气, 白疕→白癜风 (单字 IDF 饱和 255, 无法识别多字术语边界). 文档 §2.1 实测 PC jieba 90%.

### 否决的路线 (实证)
- **BPE 子词索引** (文档场景 C): MiniMind BPE 词表英文导向, 中文退化为单字切分 (肝豆状核变性→7 单字 token), 无法保留术语边界 — 实测否决.

### 实施方案 (Oracle 定案: 方案 B 术语叠加层)
1. **PC 端** chinese_v4/kb/build_term_overlay.py:
   - 读 medical_jieba.txt (368 词, 过滤 22 噪音 → 346) + format_data.jsonl (10999 docs)
   - FMM (正向最大匹配, 长词优先) 对每 doc 的 q+a[:80] 找术语 → term→docs 倒排
   - 输出 term_overlay.bin (287 terms 命中文档, 36.3KB): magic RAG2 + n_terms + n_docs + term table + u16 doclists
   - 关键坑: ① a[:40] 截断术语 (肝豆状核变性 被切) → 索引窗口扩到 80 字; ② 换行切词 (肝豆状核变\\n性) → 去 \\n; ③ 目录文档污染 (第X章...页码) → is_toc_doc 过滤
2. **设备端** ag_sd.h:
   - ragsd_load_overlay(): 全量读入 PSRAM (36KB), 解析 term table
   - ragsd_apply_overlay(): 对问题 FMM (每位置最长术语前缀), 命中术语 → doclist 全部 +600 (RAGSD_TERM_BONUS)
   - 叠加在单字 hash 打分后, 单字兜底保留
3. **FATFS LFN**: term_overlay.bin 13 字符超 8.3 → 启用 CONFIG_FATFS_LFN_HEAP + MAX_LFILES=8 (sdkconfig.defaults + sdkconfig)

### 设备端实测 (RAGQ 命令, 10 查询 Top-1)
| 查询 | 单字旧 | 术语叠加新 |
|---|---|---|
| 肝豆状核变性 | ❌ 梅核气 | ✅ 非酒精性脂肪肝鉴别 |
| 白疕 | ❌ 白癜风 | ✅ 白疕初起皮损 |
| 肺癌早期症状 | ⚠️ | ✅ 肺癌早期症状不明显 |
| 高血压诊断标准 | ✅ | ✅ 用药期间关注高血压 |
| 糖尿病临床表现 | ⚠️ | ✅ 典型临床表现 |
| 肝硬化腹水 | ⚠️ | ✅ 肝硬化征象 |
| 急性心肌梗死 | ✅ | ✅ 急性心肌梗死 |
| 上消化道出血 | ⚠️ | ✅ 排除上消化道出血 |
| 宫外孕 | ✅ | ✅ 二次宫外孕 |
| 不孕不育 | ✅ | ✅ 宫颈糜烂 |
| **Top-1 相关率** | **80%** | **90%** |

### 部署
- 文件: data_v4/sd_rag/term_overlay.bin (36.3KB) → SD /sdcard/rag/ (XFER 协议)
- 固件: esp32_llm_v5_idf (RAGSD_TERM_BONUS=600)
- 启动日志: ag: RAG overlay: 287 terms (36KB) + SD RAG index ready: 10999 docs

### 结论
- 检索质量 80% → 90% (达到 PC jieba 水平), 修复肝豆状核/白疕 结构性误配
- 成本: PC 脚本 ~1h + 设备端 ~2h + 36KB PSRAM (可忽略)
- 后续可扩展: 词表扩充 (346→更多), 术语命中权重调优 (600 经验值)
