# V5 固件设计文档（esp32_llm_zh_v5 — 外部 MiniMind PLE 模型）

> 日期: 2026-08-04 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 状态: 实施完成（H1/H2 verify PASS + 固件编译成功）

---

## 一、目标与约束

```
目标: 将外部 MiniMind PLE 模型 (H1/H2) 部署到 ESP32-S3 独立固件
约束:
  - 不动共享 llm.h (V4 固件已恢复, 需保持稳定)
  - V5 完全独立目录, 与 V4 互不干扰
  - 数值正确性以 verify (C vs PyTorch golden) 为唯一门禁
```

## 二、模型来源与架构差异

### 2.1 来源
- MiniMind 项目 (rickqi/minimind), Per-Layer Embedding 架构
- 训练链路: 预训练 → SFT → DPO 偏好优化 → int4 group=32 PTQ

| 模型 | 架构 | 参数量 | model.bin |
|---|---|---|---|
| **H1** | d256/l6/p96, V=6400 | 10.79M | 重导出 6.31MB |
| **H2** | d384/l8/p128, V=6400 | 24.95M | 重导出 14.05MB |

### 2.2 与 esp32-ai 的 3 处架构差异（诊断确认）

```
1. Q tensor 格式: MiniMind 无 bits 字节
   MiniMind: [i32 group][packed int4 codes][fp16 scales]
   llm.h:    [1B bits=4][i32 group][codes][scales]

2. q_norm/k_norm: MiniMind 有 per-head RMSNorm, llm.h 无
   forward: q_proj → q_norm(head_dim) → RoPE → attn
            k_proj → k_norm(head_dim) → RoPE → attn
   (state_dict 验证: q_norm.weight = [head_dim], H2=48, H1=32)

3. q/k/v 存储: MiniMind 分开 3 个 [D,D], llm.h 合并 qkv [3D,D]
```

### 2.3 已兼容项（无需修改）
- rope_theta=1e6: llm.h 动态从 header 读 ✅
- RMSNorm eps=1e-6: 与 llm.h RMS_EPS 一致 ✅
- GQA: export_ple1.py 已 repeat_interleave 转 MHA ✅
- RoPE 公式: MiniMind `1/rope^(2i/dim)` == llm.h `powf(rope_theta, -2i/Dh)` ✅

## 三、核心修改

### 3.1 `firmware/esp32_llm_zh_v5/llm_v5.h`（fork 共享 llm.h）

**设计决策**: 独立 fork 而非改共享 llm.h —— 共享 llm.h 被 4 个固件依赖, 直接改风险高。

```c
// Model 结构新增 (per-head q/k RMSNorm 权重)
const float *q_norm[32];  // [head_dim] per-head RMSNorm on q
const float *k_norm[32];  // [head_dim] per-head RMSNorm on k

// llm_load 每层绑定顺序 (匹配转换后文件布局)
attn_norm(F[D]) q_norm(F[Dh]) k_norm(F[Dh]) qkv(Q[3D,D])
attn_proj(Q[D,D]) ffn_norm(F[D]) gate/up/down ple_gate/ple_proj ple_norm(F[D])

// forward 注意力: qkv matvec 后, RoPE 前, per-head RMSNorm
for (int hh = 0; hh < H; hh++) {
  rmsnorm(q + hh * Dh, m->q_norm[l], Dh, q + hh * Dh);
  rmsnorm(k + hh * Dh, m->k_norm[l], Dh, k + hh * Dh);
}
```

### 3.2 `chinese_v5/convert_h2.py`（MiniMind PLE1 → llm_v5.h 格式）

```
1. 解析 MiniMind plan (89/117 tensor, 从 export_ple1.py 推导)
2. q/k/v 三个 [D,D] 按行拼接 → qkv [3D,D]
3. 每 Q tensor 前插 bits=4 字节
4. q_norm/k_norm 权重保留 (F[Dh])
5. tensor 重排为 llm_v5.h 布局
```

### 3.3 `firmware/esp32_llm_zh_v5/`（独立固件目录）

- esp32_llm_zh_v5.ino: `#include "llm_v5.h"`（非 `../common/llm.h`）
- vocab.h: MiniMind BPE 6400（id → 原始 UTF-8 字节）
- rag.h/rag_sd.h/display.h/cjk_font.h: 复用 V4

### 3.4 验证工具链

```
verify_h2.c   数值正确性门禁 (C 推理 vs PyTorch golden)
generate_h2.c 生成能力验证 (采样循环 + BPE decode)
```

## 四、验证结果

| 模型 | verify | max diff | 中文生成 |
|---|---|---|---|
| **H1** (d256/l6) | ✅ PASS top=334 | 1e-5 | ✅ 中文输出 |
| **H2** (d384/l8) | ✅ PASS top=42 | 1e-5 | ✅ 中文输出 |

生成质量说明: 中文输出但重复 —— 10-25M 小模型固有表现（无 RLHF 针对性 prompt 工程），非移植错误（数值已严格验证）。

## 五、关键发现与修复

### 5.1 git 里的 H2 model.bin 不完整
```
git H2: 13.42MB, 解析后差 296KB+
→ 用 MiniMind dpo_h2_384_ple.pth 重导出 14.05MB → 解析完全匹配
H1 同理: 5.8MB → 重导出 6.31MB
```

### 5.2 V4 vocab.h 被误覆盖
```
6b60c6e 提交将 V4 vocab.h (8196) 覆盖为 V5 (6400)
→ 用 data_v4/tokenizer.json 重新生成 8196 → 恢复 V4
→ V4 固件 1385KB 重编 + verify PASS
```

### 5.3 vocab.h 编码损坏
```
git 恢复时 PowerShell 重定向 → UTF-16 LE
→ 转 UTF-8 (generate_h2.c 编译时发现 stray '\377')
```

## 六、部署要求

```
分区: model 需扩到 ≥14.5MB (H2 model_llm 14.05MB)
  当前 V5 partitions.csv: model 0x8F0000 (8.94MB) — 放不下 H2
  方案 (英文版布局, 无 kb): model 0x170000 size 0xE80000 = 14.50MB
    - H2 14.05MB FITS (+0.45MB)
    - 固件 1385KB FITS (factory 1.375MB)
    - 无 kb 分区 → RAG 仅 SD 深搜 (rag_sd.h, flash rag.h 不可用)

烧录 (esptool, 需先改 partitions.csv + gen_esp32part 生成 partitions.bin):
  esptool --chip esp32s3 --port COM3 --baud 921600 write_flash 0x0 bootloader.bin
  esptool --chip esp32s3 --port COM3 --baud 921600 write_flash 0x10000 esp32_llm_zh_v5.ino.bin
  esptool --chip esp32s3 --port COM3 --baud 921600 write_flash 0x170000 firmware/model_v5/H2/model_llm.bin
  esptool --chip esp32s3 --port COM3 --baud 921600 write_flash 0x8000 partitions.bin

注意: COM3 设备当前跑 XiaoZhi 固件, 烧录 V5 将覆盖。
```

## 七、文件清单

```
firmware/esp32_llm_zh_v5/
  esp32_llm_zh_v5.ino    # 主固件 (include llm_v5.h)
  llm_v5.h               # 独立推理核心 (fork + q_norm/k_norm)
  vocab.h                # MiniMind BPE 6400
  rag.h / rag_sd.h       # RAG 检索 (复用 V4)
  display.h / cjk_font.h # 显示 (复用 V4)
  partitions.csv
chinese_v5/
  convert_h2.py          # MiniMind PLE1 → llm_v5.h 格式
  docs/DESIGN.md         # 本文档
firmware/host_verify/
  verify_h2.c            # 数值验证
  generate_h2.c          # 生成验证
firmware/model_v5/
  H1/model_llm.bin       # 转换后 H1 (6.01MB)
  H2/model_llm.bin       # 转换后 H2 (14.05MB)
```

## 八、设计决策记录

| 决策 | 理由 |
|---|---|
| 独立 llm_v5.h (fork) 非改共享 llm.h | V4 需稳定; 共享 llm.h 被 4 固件依赖 |
| 转换脚本非改 C 读取逻辑 | 源头格式转换一次; 改 C 影响所有模型 |
| 保留 q_norm/k_norm (新增) 非丢弃 | 丢弃 → 推理错误 (verify FAIL) |
| 用 MiniMind checkpoint 重导出 | git 内 model.bin 不完整 |
