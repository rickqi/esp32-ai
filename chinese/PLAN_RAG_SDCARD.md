# RAG 方案:SD 卡知识库 + 设备端检索

> 目标:让 zh4-ds(12.5M)模型能"引用知识回答"医学问题。
> 核心思路:知识库放 **SD 卡**(4GB+ 天然容量),ESP32 端做轻量检索,检索内容拼入 prompt 让模型续写。
> 状态:方案定稿(2026-08-01),R0 尚未实施。

---

## 1. 为什么是 SD 卡(而非 PSRAM)

原 PLAN_AB_RETRAIN_RAG.md 方案 B2 建议知识库压缩到 1-2MB 存 PSRAM。实测后修正为 **SD 卡方案**:

| 对比项 | PSRAM 方案(原) | **SD 卡方案(推荐)** |
|---|---|---|
| 知识库容量 | 1-2MB(几百条 QA) | **SD 卡全容量(4GB+)** |
| 检索索引 | 全量常驻 PSRAM | 索引头常驻(≤64KB)+ 文档按需读 |
| 更新知识库 | 重新编译烧录 | **替换 SD 卡文件即可** |
| 启动 PSRAM 占用 | +1-2MB | +≤64KB(几乎零) |
| 检索延迟 | 内存访问(µs) | FATFS 顺序读 1-2ms(远低于 232ms/token) |

设备现状:
- SD 卡已挂载:`/sdcard`(SDMMC CLK=38, CMD=21, D0=39),`sd_ok` 检测 + 日志写入已验证
- PSRAM 8MB,启动后余量约 4-5MB,但被模型(head int8 + KV cache 1.88MB + scratch)占用
- 知识库不挤占 PSRAM 是 SD 卡方案的**决定性优势**

---

## 2. 核心约束(实测)

| 参数 | 值 | 对 RAG 的影响 |
|---|---|---|
| seq_len | **256 tokens** | 检索文本必须 ≤ ~120 tokens(最硬约束) |
| 中文 token | ≈1 token/字符 | "甲状腺切除是否会造成晕倒" = 12 tokens |
| 词表 | 字符级 5904 | 检索可天然按字符/词匹配 |
| 推理速度 | 3.8 tok/s | 检索 1-2ms 可忽略;生成 1 句 ≈ 0.5s |
| prompt 模板 | `<user>Q<end><assistant>` | SFT 已学格式,检索内容插入 Q 之前 |

**prompt 预算**:
```
seq_len 256
  - 问题(用户输入,平均 10-30 tokens)
  - 检索内容(1-2 块,20-60 字符/块)
  - 模板标记 <user>/<end>/<assistant> ≈ 3 tokens
  - 生成预留 50-80 tokens
────────────────────────────────
检索内容预算: 最多 ~120 tokens(但小模型利用差,20-60 足够)
```

---

## 3. 架构

```
用户问题 (串口 JSON {"ids":[...]})
   ↓
[ESP32 检索器] rag_retrieve(question)
   │   ① 字符级 TF-IDF 打分(问题 token → 倒排索引)
   │   ② 取 Top-2 文档块
   │   ③ 从 SD 卡 fseek+fread 读块文本(1-2ms)
   ↓
[prompt 组装] <user>[参考] 检索块1 检索块2 问题<end><assistant>
   ↓
[12.5M 模型] 现有生成循环(seq_len=256 内)
   ↓
医学风格回答(引用检索内容)
```

---

## 4. 知识库构建(R0,PC 端)

### 4.1 数据源

| 数据源 | 规模 | 用途 | 许可 |
|---|---|---|---|
| `Huatuo26M-Lite` | ~160K 精选 QA | 主知识库 | 研究用途 |
| `shibing624/medical`(魔搭 `zjydiary/Medical`) | 360K 百科 + 8.5K 教材 | 补充百科条目 | Apache 2.0 |

### 4.2 分块规则

- **每块 20-60 字符**(1-2 句),小模型上下文利用差必须短
- 按句号/问号/换行切分,保留完整句子
- 过滤:LaTeX、表格、URL、重复段(复用 `prepare.py` 的 `clean_markdown`)

### 4.3 索引格式(ESP32 可解析,低内存)

```
/sdcard/rag/index.bin            # 倒排索引
  uint16 num_terms
  per term:
    uint16 term_len + UTF-8 bytes(单字或 2-gram)
    uint16 doc_count
    uint16[doc_count] doc_ids

/sdcard/rag/docs.bin             # 文档块库
  uint16 num_docs
  per doc:
    uint16 len + UTF-8 bytes(20-60 字符文本)

/sdcard/rag/meta.bin             # (可选)词频/DF 统计用于 TF-IDF 权重
```

**内存占用**:5000 词条 × ~20B ≈ 100KB 索引头常驻 PSRAM(可接受);
文档块**不常驻**,按 doc_id 偏移 fseek 读取。

### 4.4 检索算法:字符级 TF-IDF

```
1. 问题分词:直接用词表 token(字符级天然分词),或用 2-gram 滑窗
2. 每 token 查倒排索引 → 候选文档集合
3. TF-IDF 打分:score(doc) = Σ tf(t,doc) * idf(t)
   (tf = 词频, idf = log(N/df) 可从 meta.bin 读)
4. 取 Top-2 文档块
```

复杂度:问题 12 token × 每词条平均 10 文档 = ~120 次打分,ESP32 微秒级。

---

## 5. 固件实现(R1/R2)

### 5.1 R1:检索器(`firmware/esp32_llm_zh/rag.h`)

```c
// 新文件 rag.h,接口:
bool   rag_init();                        // 读 index.bin 头 + 词表入 PSRAM,失败返回 false
int    rag_retrieve(const int *qids, int qn,
                    char *out1, int cap1,   // Top-1 块 UTF-8
                    char *out2, int cap2);  // Top-2 块(可空)
```

- `rag_init()`:sd 挂载后调用;索引头 malloc 到 PSRAM(`heap_caps_malloc SPIRAM`)
- `rag_retrieve()`:问题 token → 查索引 → TF-IDF → `fopen`/`fseek`/`fread` 读块
- 无 SD 卡/索引文件 → 返回 0,降级为纯生成(现状)

### 5.2 R2:推理接入(`esp32_llm_zh.ino`)

```c
// parse_json_prompt 成功后,生成前:
char ctx1[128], ctx2[128];
int nctx = rag_retrieve(recv_ids, recv_n, ctx1, sizeof ctx1, ctx2, sizeof ctx2);

// 重组 prompt token 序列(在现有 recv_ids 前插入):
//   <user> [参考] ctx1 ctx2 问题 <end> <assistant>
// 重新 tokenize(ctx 用词表 stoi 逐字符映射)
// 严格裁剪:检索部分 ≤ 120 tokens,超限截断
```

- 检索只做**一次**(每个问题),不做多轮
- token 组装用现有 `VOCAB_OFF/VOCAB_BLOB`(字符级,直接查 `stoi` 等价映射)

### 5.3 显示

- 检索到的参考块可显示在 prompt 2x 区下方(或输出区顶部,标记 `[参考]`)
- 或保持简洁:只显示问题 + 生成结果

---

## 6. RAFT 微调(R3,关键)

**12.5M 模型不会主动利用检索内容** —— 需 RAFT(Retrieval-Augmented Fine-Tuning)式微调:

### 训练格式

```
输入: <user>[参考] 检索块内容 问题<end><assistant>
目标: 答案(训练模型"抄写/改写"参考内容)
```

### 数据构造

1. 用 PC 端检索器对每个问题检索 Top-1 块
2. 正样本:块确实包含答案的问题-答案对(标注过滤)
3. 负样本(RAFT 关键):部分训练样本**故意给无关块**,教模型"无参考时明说不知道"

### 训练

```powershell
uv run python chinese/sft/sft_train.py --resume runs_chinese/ple-zh4-s42.pt `
  --steps 300 --eval-every 50 --tag zh4-raft
```

### 预期效果分级

| 级别 | 模型行为 | 是否需要 RAFT |
|---|---|---|
| 引用式 | 直接续写检索块开头(答案已在其中) | 可不做,但回答生硬 |
| **问答式** | "甲状腺切除术后晕倒多为体位性低血压…" | ✅ 需 RAFT |
| 拒答式 | "根据现有资料无法确定" | ✅ 需 RAFT(负样本) |

---

## 7. 分阶段路线图

| 阶段 | 内容 | 产出 | 验证方式 |
|---|---|---|---|
| **R0** | PC 建库脚本:下载 → 清洗 → 分块 → 倒排索引 | `index.bin`+`docs.bin` | PC 端检索 demo(查准率抽查) |
| **R1** | 固件 `rag.h`:init + retrieve | 设备端检索函数 | 串口命令 `RAGRET <问题>` 打印 Top-2 |
| **R2** | 推理接入:检索 → prompt 重组 | 端侧 RAG 问答 | 烧录实测"甲状腺切除是否会造成晕倒" |
| **R3** | RAFT 微调 | `ple-zh4-raft.pt` | 对比微调前后回答质量 |
| **R4** | 端侧验证 + 展示优化 | 医学问答演示 | 截图 + 串口 |

**依赖关系**:R0 → R1 → R2 串行;R3 可与 R2 并行(独立训练),R4 收尾。

---

## 8. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| **12.5M 模型仍不用检索内容** | 回答退化(现状) | **R3 RAFT 必做**;R2 用"引用式"先验证管线 |
| 检索块与问题不匹配 | 回答无关 | 索引加同义词表(医疗缩写/别称);Top-2 而非 Top-1 |
| prompt 超 seq_len | 生成截断/崩溃 | 严格裁剪:检索 ≤120 tokens;问题超 100 截断 |
| SD 无卡/损坏 | RAG 不可用 | `sd_ok` 检测,降级纯生成(现状兜底) |
| SD 读延迟 | 检索慢 | FATFS 顺序读 1-2ms,远低于推理 232ms/token,可忽略 |
| 数据许可 | 商用受限 | 个人研究可接受;商用换 Apache-2.0 数据源 |

---

## 9. 下一步(建议立即执行)

**R0(PC 端知识库构建脚本)** —— 所有后续步骤的依赖,可立即验证索引格式与检索质量:

1. 下载 `zjydiary/Medical`(魔搭镜像,国内快)或 Huatuo26M-Lite 子集
2. 写 `chinese/build_rag_index.py`:清洗 → 分块 → 倒排索引 → 输出 `index.bin`+`docs.bin`
3. PC 端写 `rag_retrieve` 的 Python 参考实现,抽查 20 个医学问题的查准率
4. 产出物拷贝到 SD 卡 `/sdcard/rag/`,供 R1/R2 使用
