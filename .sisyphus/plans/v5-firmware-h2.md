# V5 固件实施计划（esp32_llm_zh_v5 — H2 独立推理）

> 日期: 2026-08-04 | 分支: feature/ESP32-S3-4.2inch-RLCD
> 前置: V4 已恢复 (vocab.h 8196, 提交 1f5666d), 推理+RAG 验证通过

---

## 〇、目标与约束

```
目标: H2 model.bin (MiniMind PLE, V=6400 D=384 L=8 F=1216 P=128)
      在独立固件 esp32_llm_zh_v5 上跑通 (verify PASS + 可生成)
约束: 不动共享 llm.h (V4 固件已恢复, 需保持稳定)
      V5 完全独立目录, 与 V4 互不干扰
```

## 一、已确认的格式差异（诊断结论）

```
1. H2 model.bin 的 Q tensor 缺 bits 字节:
   MiniMind: [i32 group][packed codes][fp16 scales]
   llm.h:    [1B bits][i32 group][codes][scales]     ← 缺 1 字节

2. MiniMind 有 q_norm/k_norm (per-head RMSNorm), llm.h 无:
   forward: q_proj → q_norm → RoPE → attn
            k_proj → k_norm → RoPE → attn

3. MiniMind q/k/v 分开存, llm.h qkv 合并 [3D,D]

4. GQA 已解决: export_ple1.py 已 repeat_interleave 转 MHA (无需运行时处理)

5. rope_theta=1e6 在 header (llm.h 动态读) ✅
```

## 二、实施步骤

### Step 1: fork llm_v5.h（独立推理核心）
```
1. 复制 llm.h → firmware/esp32_llm_zh_v5/llm_v5.h
2. Model 结构加: const float *q_norm[32], *k_norm[32];
3. llm_load 每层绑定顺序 (转换后的文件布局):
   attn_norm(F) q_norm(F) k_norm(F) qkv(Q[3D,D]) attn_proj(Q[D,D])
   ffn_norm(F) gate(Q) up(Q) down(Q) ple_gate(Q) ple_proj(Q) ple_norm(F)
4. forward 注意力: qkv matvec 后, per-head rmsnorm:
   for h in heads: rmsnorm(q[h*hd:(h+1)*hd], q_norm, hd)
                   rmsnorm(k[h*hd:(h+1)*hd], k_norm, hd)
   (head_dim = D/H = 384/8 = 48)
5. RMSNorm eps 对齐 MiniMind config
```

### Step 2: 模型转换脚本（convert_h2.py → tools/）
```
1. 读取 MiniMind 格式 H2 model.bin
2. tensor 重排: q/k/v → qkv 拼接 [3D,D]
3. 每 Q tensor 前插 bits=4 字节
4. 输出 model_v5/H2/model_llm.bin (llm_v5.h 格式)
5. 保留 q_norm/k_norm 权重
```

### Step 3: verify_h2.c（fork verify.c）
```
1. 复制 verify.c → firmware/host_verify/verify_h2.c
2. include llm_v5.h (改路径)
3. 跑: wsl gcc ... && /tmp/verify_h2 H2/model_llm.bin H2/golden.txt
4. 期望 PASS (golden 是 MiniMind 反量化 forward 生成)
```

### Step 4: 固件编译
```
1. esp32_llm_zh_v5.ino: #include "../common/llm.h" → #include "llm_v5.h"
2. 词表: vocab.h 6400 (已就位)
3. 分区: model 需扩到 13.75MB (H2 13.42MB)
4. tools/manual_compile.py 适配 v5 (新 build dir)
```

### Step 5: 验证 + 文档
```
1. verify H2 PASS
2. 生成测试 (如可行: 需 H2 vocab id→char 表, MiniMind BPE)
3. 更新 CHANGELOG / firmware/README.md 版本矩阵
4. 提交
```

## 三、风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| q_norm/k_norm per-head RMSNorm 错误 | verify FAIL | 逐层对比 MiniMind forward 定位 |
| RMSNorm eps 差异 | 数值偏移 | 读 MiniMind config 对齐 |
| D=384 xq[1024] 溢出 | 仅 LLM_INT8_ACT 模式 | V5 默认 fp32, 不启用 int8 |
| 转换布局错误 | verify FAIL | 转换脚本打印 tensor 校验 |
| verify 不过 | 调试周期长 | 先验证单层 forward |

## 四、验收标准

```
✅ verify_h2 PASS (max abs diff < 0.02)
✅ 固件编译成功
✅ V4 固件不受影响 (llm.h 未动)
✅ 文档更新
```
