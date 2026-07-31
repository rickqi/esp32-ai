# 指令数据扩充实现方案（1059 → 3000+）

> 日期: 2026-07-31 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 目标: 解决 SFT 问答事实准确性的数据瓶颈

---

## 一、现状与目标

| 指标 | 当前 | 目标 |
|---|---|---|
| 指令样本 | 1,176（train 1,059 + val 117） | **3,000+** |
| 来源构成 | wsl_train 921 / search_logs 125 / wsl_cot 80 / wsl_val 50 | 多源混合 |
| 领域覆盖 | 医学为主 + 少量制度/消保/法律 | 医学深挖 + 制度/法律扩充 |

## 二、前置条件（已确认 ✅）

| 条件 | 状态 |
|---|---|
| DeepSeek API key | ✅ `D:\docs\doc-search\.env`（WSL 可读） |
| 医学文档源 | ✅ `/home/raw/medica`（928 文件，4 类） |
| WSL QA 生成器 | ✅ `med_qa_generator.py` + `gen_targeted_qa.py` + `gen_cot_data.py` + `gen_qa_docsearch.py` |
| search_logs.db | ✅ SQLite 存在（可深挖） |
| 数据管道 | ✅ `chinese/sft/sft_data.py`（labels bug 已修复） |

---

## 三、三条扩充路径

### 路径 A：WSL QA 生成器扩产（主路径，可产 1500-2500 条）

**A1. 全量生成器重跑**（`med_qa_generator.py`）

```bash
# 在 WSL 执行
cd /home/LLMs-from-scratch/projects/chinese-medical-text-generation
# 修改 CATEGORY_CONFIG 的 sample_count（当前每类 12-18 条 → 40-60 条）
python scripts/med_qa_generator.py generate   # 4 类 × 50 ≈ 200 条/轮
python scripts/med_qa_generator.py all        # 全流程
```

**A2. 定向生成器**（`gen_targeted_qa.py`，补弱覆盖领域）

```bash
# 目标领域已含：TNM分期、影像学对比、术后管理、甲状腺、放化疗
# 扩充 TARGET_TOPICS：增加 甲状腺/乳腺/肺癌/消化系统 等
python scripts/gen_targeted_qa.py             # 每领域 10 问 × 5 领域 × N 轮
```

**A3. CoT 生成器**（`gen_cot_data.py`，增强推理链）

```bash
python scripts/gen_cot_data.py --count 300    # 为 300 条 QA 加 <think> 推理
```

**成本估算**：DeepSeek API，每 QA 对 ~0.5-1k tokens，3000 条约 ¥10-20。

### 路径 B：search_logs 深挖（辅助路径，可产 300-500 条）

**B1. SQLite 历史数据**（`search_logs.db`）

```sql
-- 检查表结构与数据量
SELECT name FROM sqlite_master WHERE type='table';
SELECT COUNT(*) FROM searches;  -- 若含历史搜索记录
```

- 从 db 提取**成功检索 + 有实质回答**的历史 QA
- 与 md 文件去重（按 instruction hash）

**B2. 放宽现有过滤**（`sft_data.py` 参数化）

- 降低 MIN_RESPONSE_CHARS：30 → 15（保留短回答）
- 新增 `--min-filters` 开关：保留"未找到"型回答？❌ 不推荐（教模型拒绝）
- **保留"部分命中"回答**：当前 NEGATIVE_PATTERNS 过滤过严，可细分（区分"完全无信息"vs"有部分信息"）

**B3. 多轮变体扩充**（search_logs 指令改写）

- 对 125 条清洗后 QA，用规则生成同义问法（如"感冒如何治疗" → "感冒怎么治"/"感冒的治疗方法"）
- 保守：每条 1-2 个变体 → 250-375 条

### 路径 C：本地模板构造（补充路径，可产 500-1000 条）

**C1. 基于预训练语料构造**（无 API 成本）

- 从 `data_chinese/corpus.txt` 提取结构化段落（含"诊断标准""治疗方案"等锚点）
- 规则抽取"实体 + 属性"对，套用 QA 模板：

```
模板: "{entity}的{aspect}是什么？" → "{extracted answer}"
实体: 甲状腺/肺癌/心衰/高血压...
方面: 诊断标准/治疗方案/临床表现/禁忌证...
```

**C2. 英文 TinyStories 式自我对话**（低质，不推荐单独使用）

---

## 四、数据管道集成（sft_data.py 扩展）

```
chinese/sft/sft_data.py 新增:
  1. --wsl-qa-dir: 指向 WSL 生成的 ChatML（增量合并，按 question hash 去重）
  2. --search-db: 解析 search_logs.db 增量提取
  3. --augment: 开启 search_logs 同义改写
  4. --local-templates: 本地模板构造数量
  输出: data_chinese/sft/processed/sft_train.json (3000+)
```

**合并规则**：
1. 按 `question` hash 全局去重（WSL + search_logs + 模板）
2. train/val 划分保持 90/10 + 分层（保证各源在 val 有代表）
3. 保留 source 标签（wsl/targeted/cot/search_logs/template），便于质量审计

## 五、质量保障

| 检查 | 方法 |
|---|---|
| 去重 | question hash，重复率 < 5% |
| 污染检测 | TOOL_RESIDUE / NEGATIVE_PATTERNS 全量扫描 |
| 长度分布 | 问题 5-100 字符，回答 30-3000 字符 |
| 抽样人工审查 | 每来源抽 10 条人工评分（1-5） |
| 训练验证 | SFT 后 8 标准问题对比（Phase 4） |

## 六、实施步骤与工作量

| 步骤 | 内容 | 工作量 | 依赖 |
|---|---|---|---|
| 1 | WSL 全量+定向+CoT 生成（改 sample_count + 跑 3 脚本） | 2-3h + API 等待 | DeepSeek key ✅ |
| 2 | search_logs.db 解析脚本（`sft_data.py --search-db`） | 1-2h | — |
| 3 | 同义改写 + 模板构造（`sft_data.py --augment --local-templates`） | 1-2h | — |
| 4 | 数据合并去重 + 质量审计 | 1h | 步骤 1-3 |
| 5 | zh4 SFT 重训（300 步）+ 8 问题评估 | 1h | 数据 ✅ |
| 合计 | | **~8h** | |

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| DeepSeek API 配额/限流 | 分批生成（--sample-count 控制），失败重试 |
| 生成 QA 质量参差 | 抽样人工审查 + 污染规则过滤 |
| WSL 生成器与本地格式差异 | 统一走 ChatML（med_instruction_*.json），sft_data.py 已兼容 |
| 同义改写引入错误 | 仅对 search_logs 保守改写（1-2 变体） |
| 模板构造过于机械 | 只做高置信模板（锚点段落），占比 < 30% |

## 八、验收标准

```
1. sft_train.json ≥ 3000 样本（去重后）
2. 各来源占比: wsl ≥ 50%, search_logs 10-20%, template ≤ 30%
3. 污染率: TOOL_RESIDUE 0 条, NEGATIVE < 5%
4. 抽样 10 条/来源 人工评分 ≥ 3.5/5
5. zh4-sft 重训后 8 标准问题: 至少 6/8 有结构化相关回答
```
