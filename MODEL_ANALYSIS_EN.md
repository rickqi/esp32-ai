# ESP32 Chinese/English Model — Design & Comparison Report

> Date: 2026-08-01 | Branch: feature/ESP32-S3-4.2inch-RLCD
> Covers: model design & invocation + comprehensive Chinese-vs-English comparison

---

# Part 1: Model Design & Invocation — Full Analysis

## 1. How the Model Is Created

### 1.1 Core Architecture: PLE (Per-Layer Embedding)

The model uses **Google Gemma's Per-Layer Embedding** architecture — the fundamental reason this project runs on a microcontroller.

```
Traditional Transformer:
  Word embedding (Vocab × D) → shared across all layers
  → embedding table must live in fast memory (SRAM)

PLE Architecture:
  Base embedding (Vocab × D)         ← looked up once per token, small
  + PLE table (Vocab × L×P)          ← L rows looked up per token, one per layer, huge

  Each layer: x += RMSNorm(Proj(GELU(Gate(x)) × PLE_input[tok, layer]))
  → only L PLE rows (~450 bytes) read per token; the other ~25M params untouched
```

### 1.2 Creation Pipeline (Training)

```
src/model.py        → Config + TinyLM (PLE Transformer definition)
chinese/train.py    → make_model("ple", target_core=2.5M) binary-search ffn_hidden
                    → train 10k-20k steps → .pt checkpoint
chinese/quantize.py → 4-bit PTQ quantization (group=32, SFT-sensitive)
chinese/export.py   → export flat binary model.bin (PLE1 magic)
```

### 1.3 Three-Tier Parameter Split (Key Design)

```
core   (computed every token) = attention + FFN      → flash read
stream (scanned every token)  = output head (V×D)    → PSRAM
table  (L rows per token)     = PLE table (V×L×P)    → flash mmap
```

## 2. How the Model Fits in 512KB SRAM

### 2.1 Key Insight: the model NEVER lives in SRAM!

```
Flash 16MB (slow, huge)  ← model body (int4 weights)
  │ memory-mapped direct read
PSRAM 8MB (medium)       ← head staging + KV cache + activations
SRAM 512KB (fast, tiny)  ← only activation vectors + small constants
```

### 2.2 Implementation (llm.h + esp32_llm.ino)

```c
// 1. mmap model partition — weights read straight from flash
esp_partition_mmap(model_partition) → base
llm_load(base, &model)  // parse header + bind tensor pointers to flash

// 2. Quant tensor (QT): codes point into flash, dequantized on the fly
typedef struct {
  const uint8_t *codes;   // int4 nibbles, flash addresses
  const uint16_t *scales; // fp16 scales, flash
  int rows, cols, group;
} QT;

// 3. Per-token inference:
matvec_q(&ple_table, x, y)  // read flash int4 → dequant → multiply-add
  → only needed rows accessed
```

### 2.3 What Actually Lives in SRAM (within 512KB)

```c
// Scratch (PSRAM-allocated, not SRAM)
s.x = ps(D*4);          // activations
s.h = ps(F*4);          // hidden
s.kcache = ps(L*S*D*4); // KV cache → PSRAM
s.logits = ps(V*4);     // output logits → PSRAM

// SRAM only:
head_actq[128]   // int8 quantized activations (fixed 128)
static vars       // state, buffers
stack + RTOS task stacks
```

**Zero weight footprint in 512KB SRAM** — weights live in flash/PSRAM.

### 2.4 Why This Works (Memory-Tier Utilization)

| Weight | Access pattern | Storage | Why |
|---|---|---|---|
| PLE table | 6-8 rows/token | flash mmap | sparse random reads, perfect for flash |
| Output head | full scan/token | PSRAM int8 | sequential, bandwidth OK |
| core | full compute/token | flash int4 | compute-bound, flash read is bottleneck |

## 3. How to Train Such a Model

### 3.1 Training Config

```powershell
# Chinese v2 pretrain (GPU RTX 5080)
python3 chinese/train.py --d-model 160 --n-layers 8 --n-heads 8 \
  --ple-dim 192 --target-core 2500000 --steps 20000 --tag zh5
# → ffn=244, core=2.5M, table=10.1M, total=13.7M
```

### 3.2 Data Pipeline

```
ModelScope zjydiary/Medical (encyclopedia 361K + textbooks 8.5K)
  → clean (strip LaTeX/tables) → 100M-char corpus
  → char-level tokenizer (6594 vocab, with SFT markers)
  → train.bin (99M tokens, uint16)
```

### 3.3 Quantization Export

```
fp32 → 4-bit group-wise PTQ (group=32) → model.bin (7.5MB)
  → flash model partition (0x170000)
```

## 4. How SFT Is Done

### 4.1 Why SFT

Pretrained models only continue text; SFT teaches instruction-following.

### 4.2 Data Construction

```
Multi-source instruction mix (57K): zjydiary 30K + BenTsao 7.2K + HuatuoGPT2 20K
Format: [BOS] <user> QUESTION <end> <assistant> ANSWER <end> [EOS]
labels: only answer tokens counted (loss masking, right-shifted)
```

### 4.3 Training

```
Resume from pretrained + 80% instruction + 20% continuation (anti-forgetting) → GPU 2 min
```

## 5. Why RAG Was Added

### 5.1 Core Contradiction

```
13.7M model generates medical-style text but cannot memorize precise facts
Measured: "Does thyroidectomy cause fainting?" → generic medical text, imprecise
```

### 5.2 RAG Solution

```
Question → retrieve Huatuo26M-Lite (93.5K QA) → inject evidence → model answers grounded
Essence: externalize fact memory to KB; model only organizes language
```

### 5.3 Why RAFT Too

```
Small models don't cite evidence without training → RAFT teaches "evidence → paraphrase"
Measured: given "SBP≥140mmHg" evidence → model reproduces precisely ✅
```

### 5.4 Full Loop

```
KB: Huatuo26M-Lite → IDF inverted index (1.83MB) → kb partition
Inference: question → rag.h retrieval (0.6ms) → evidence injection → RAFT model generates
```

---

# Part 2: Chinese vs English Model Comparison

## 6. Architecture Comparison

| Dimension | English cleandeploy | Chinese v2 zh5-multi2 | Why |
|---|---|---|---|
| Vocab | 32,768 (BPE) | 6,594 (char) | Chinese chars are single tokens |
| d_model | 96 | **160** | wider Chinese core |
| Layers | 6 | **8** | deeper |
| FFN | 66 | **244** | core-budget solved |
| ple_dim | 128 | **192** | wider PLE modulation |
| seq_len | 256 | 256 | same |

## 7. Parameter Distribution (Key Difference)

| Tier | English | Share | Chinese v2 | Share |
|---|---|---|---|---|
| core | 558K | 1.9% | **2.5M** | **18%** |
| stream | 3.1M | 10.9% | 1.06M | 8% |
| table | **25.2M** | **87.2%** | 10.1M | 74% |
| total | **28.9M** | — | **13.7M** | — |

```
English: 87% is table (32K vocab → huge table)
Chinese: core 2.5M = 4.5× English 558K → more compute per token
  → stronger conditional/understanding ability (core is the real compute)
```

## 8. Memory & Speed Comparison

| Item | English | Chinese v2 |
|---|---|---|
| model.bin | 14.9MB | **7.5MB** |
| PSRAM | ~5.1MB | ~3.7MB |
| head int8 | 2.53MB | 1.06MB |
| **Speed** | **9.5 tok/s** | ~2 tok/s |
| core flash read/step | 0.28MB | 1.25MB |

```
Speed gap: Chinese core 4.5× → flash-read bound
Tradeoff: Chinese trades speed for quality; English trades quality for speed
```

## 9. Data & Training Comparison

| Dimension | English | Chinese v2 |
|---|---|---|
| Pretrain data | TinyStories 300MB | medical encyclopedia+textbooks 100M chars |
| Training volume | 75M tokens | 99M tokens |
| SFT data | none | **57K instructions + 20K RAFT** |
| val ppl | 11.39 | 12.13 (different distribution, not directly comparable) |

## 10. Capability Comparison

| Capability | English | Chinese v2 |
|---|---|---|
| Text generation | ✅ stories | ✅ medical content |
| Instruction following | ❌ | ✅ 57K SFT |
| Knowledge QA | ❌ | ✅ RAG |
| Evidence citation | ❌ | ✅ RAFT |
| Device-side retrieval | ❌ | ✅ 0.6ms |
| Chinese display | ❌ | ✅ cjk_font.h |

## 11. Summary Matrix

| Dimension | Winner | Reason |
|---|---|---|
| Parameter count | English | larger vocab → larger table |
| Core density | **Chinese** | core 4.5× |
| Model size | **Chinese** | 7.5 vs 14.9MB |
| Speed | English | smaller core |
| Instruction ability | **Chinese** | has SFT |
| QA ability | **Chinese** | has RAG+RAFT |
| Deployment cost | **Chinese** | smaller footprint |

## 12. Core Conclusions

```
【English】large but "puffy" (87% lookup table)
  → fast (9.5 tok/s), tiny core (558K), story-only

【Chinese v2】fewer params but "solid" (18% core, 4.5× density)
  → slow (2 tok/s), strong core (2.5M)
  → full capability chain: generation + SFT + RAG + RAFT

【Essence】same PLE architecture, two tradeoffs:
  English = big vocab + small core → fast but shallow
  Chinese = small vocab + big core → slow but deep
  + Chinese late-mover advantage: full SFT/RAFT/RAG enhancement suite
```

---

## Appendix: Model Evolution

| Version | Model | Params | val ppl | Milestone |
|---|---|---|---|---|
| v1 EN | cleandeploy | 28.9M | 11.39 | TinyStories baseline |
| v1 CN | zh4-ds | 12.5M | 7.64 | OCR corpus + labels bug fix |
| v2 CN | zh5 | 13.7M | 12.13 | clean medical data |
| v2 CN | zh5-multi2 | 13.7M | 7.0 | multi-source SFT |
| v2 CN | ple-raft | 13.7M | 2.6 | RAFT evidence copy |

## Appendix: Key Files

```
src/model.py              PLE model definition
chinese/train.py          training entry (--data-dir/--runs-dir multi-env)
chinese/quantize.py       4-bit PTQ
chinese/export.py         export model.bin (GROUP=32)
chinese_v2/prepare.py     v2 medical data pipeline
chinese_v2/build_sft.py   multi-source SFT build
chinese_v2/build_raft.py  RAFT data build
chinese/kb/build_index.py RAG index build (IDF-weighted)
chinese/kb/test_rag_host.c host-side C verification
firmware/common/llm.h     shared C runtime
firmware/esp32_llm_zh_v2/ v2 firmware (rag.h + kb partition)
firmware/README.md        three-language version docs
chinese/CHANGELOG.md      full change log
```
