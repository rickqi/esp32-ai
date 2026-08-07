# 中文字体显示系统：跨项目移植性分析与方案 B 实施

> 日期: 2026-08-06
> 分支: feature/ESP32-S3-4.2inch-RLCD
> 参考: xiaozhi-esp32 `docs/font-system-design.md`

---

## 1. 背景

本项目（esp32-ai）在 Waveshare ESP32-S3-RLCD-4.2（ST7305 反射屏）上运行微型 LLM。
中文固件（v1/v2/v3/v5）依赖 `cjk_font.h` 渲染 CJK 字符。
本文档对比当前方案与 xiaozhi-esp32 的字体系统，评估移植可行性，并实施选定方案。

---

## 2. 当前系统分析（esp32-ai）

### 2.1 数据格式

```
cjk_font.h（~814KB 源码 / ~276KB 二进制数据）
├── CJK_CP[7854]     uint32_t  排序的 Unicode 码点表（~30.7KB）
├── CJK_OFF[7854]    uint32_t  CJK_BLOB 中的字节偏移（~30.7KB）
└── CJK_BLOB[~220KB] uint8_t   14×14 1bpp 字形位图（每字 28 字节）
```

- **字形布局**: 固定 14×14 单元格，MSB-first，像素 `(row,col)` = `BLOB[off + row*2 + col/8]` bit `(0x80 >> (col%8))`
- **无 PROGMEM 属性**, 但 `static const` 在 ESP32 上默认链接到 Flash `.rodata`
- **无压缩、无 RLE**

### 2.2 渲染管线（display.h）

```
UTF-8 文本 → utf8_decode() → Unicode 码点
           → cjk_find()    → 二分搜索 CJK_CP[7854]，O(log₂7854) ≈ 13 次比较
           → rlcd_draw_cjk() → 逐像素 RLCD_SetPixel()（每字 196 次调用）
```

- ASCII 字符使用内嵌 5×7 位图字体，垂直居中于 14px 行
- 缺字渲染为空心方框 □
- 无 LVGL 依赖，纯 C 自定义渲染

### 2.3 显示驱动

| 项 | 值 |
|---|---|
| 面板 | ST7305 反射式 LCD，400×300，1bpp 单色 |
| 帧缓冲 | PSRAM，15000 字节（400×300/8） |
| 像素寻址 | LUT 加速（PixelIndexLUT + PixelBitLUT，~351KB PSRAM） |
| SPI | 10MHz，全屏刷新一次 |
| 框架 | Arduino + 自定义 display_bsp |

### 2.4 字体生成

```
font_noto_qwen_14_1.bin (xiaozhi cbin)
       ↓
gen_cjk_font_cbin.py
  · 解析 LVGL cbin 格式（cmaps/glyph_dsc/glyph_bitmap）
  · 目标字符集 = GB2312 全量 (7445) ∪ 模型词表字符
  · 逐字提取 → 居中到 14×14 单元格 → 输出 C 数组
       ↓
cjk_font.h (7854 字形)
```

- **已有旧脚本** `gen_cjk_font.py`：从 LVGL SimSun C 源码提取 16×16（已弃用）

### 2.5 局限性

| 局限 | 影响 |
|---|---|
| 固定 14×14 单元格 | 无变宽/字距（所有 CJK 等宽，ASCII 居中） |
| 仅 1bpp | 无抗锯齿/灰度 |
| 编译时 #include | 换字库需重编固件 |
| uint32_t 码点表 | 浪费（所有码点 < 0xFFFF，可用 uint16_t 省 15KB） |
| 逐像素 SetPixel | 性能低（但 ~2 tok/s 下无瓶颈） |
| GB2312 ~98.7% 覆盖 | 87 字渲染为 □ |

---

## 3. 参考系统分析（xiaozhi-esp32）

### 3.1 架构：双层字体 + 零拷贝 mmap

```
层 1（编译内置）: font_puhui_basic_14_1.c → 固件 .rodata
  · 基础子集 ~314 字（界面字符串 + 拉丁字符）
  · const 指针直接访问，零 RAM

层 2（运行时覆盖）: font_puhui_common_14_1.bin → assets 分区 (mmap)
  · 全量子集 ~18000+ 字（LLM tokenizer 语料提取）
  · esp_partition_mmap() 映射 → 字形指针直指 Flash → 零拷贝
  · OTA 可更新

fallback 链 (v3.7.0): cbin(堆分配) → .fallback = &BUILTIN_FONT
```

### 3.2 格式：LVGL 原生 lv_font_fmt_txt

```c
// 内置 C 源码和运行时 cbin 共用同一格式
static const uint8_t glyph_bitmap[];           // 打包位图（1/4bpp）
static const lv_font_glyph_dsc_t glyph_dsc[];   // 每字形指标（box_w/box_h/ofs_x/ofs_y/adv_w）
static const lv_font_cmap_t cmaps[];            // Unicode → glyph_id 映射（多段，含稀疏）
static const lv_font_fmt_txt_dsc_t font_dsc;    // 顶层描述符
const lv_font_t font_xxx = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,   // LVGL 原生函数
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .dsc = &font_dsc,
};
```

- **cmap 分段**: ASCII/Latin 用 FORMAT0 连续段 O(1)；CJK 用 SPARSE_TINY 稀疏段（二分搜索 `unicode_list`）
- **cbin** = `lv_font_fmt_txt` 的二进制序列化，指针换相对偏移；`cbin_font_create()` 反序列化（~83 行 C）

### 3.3 渲染：LVGL 9.3.0 全栈

```
UTF-8 文本 → LVGL lv_text 解码 → Unicode 码点
           → lv_font_get_glyph_dsc_fmt_txt() 查 cmaps[]
           → lv_font_get_bitmap_fmt_txt() 取位图（Flash 直读）
           → LVGL blitter 绘制到 draw buffer（PSRAM）
```

- LVGL 9 无字形缓存（v8 的 per-font cache 已移除）
- 每字形 RAM ≈ `stride_A8(box_w) × round_up(box_h, 32)` ≈ 1KB（复用暂存）

### 3.4 内存模型

| 项 | 内置字体 | 运行时 cbin |
|---|---|---|
| 字形存储 | Flash `.rodata` | assets 分区（mmap） |
| 字形 RAM | **0** | **0**（mmap 直读） |
| 元数据 RAM | 0（const） | ~几百字节（lv_malloc） |
| 全量字库代价 | 固件 +1.7MB | 占 assets 分区，固件零成本 |

### 3.5 工具链

```
TTF 源（8 个 Noto 字体合并）
  → fontTools.subset（字符集子集化）
  → lv_font_conv（78 fork）
    --format lvgl  → .c 源码（内置）
    --format cbin  → .bin 二进制（运行时）
    --no-compress --no-prefilter --force-fast-kern-format
```

- 字符集来源: DeepSeek-R1 + Qwen3 tokenizer 语料 → ~18000 常用字
- 可用像素: 14/16/20/30 px；bpp: 1（单色）/ 4（抗锯齿）

### 3.6 同款硬件

`waveshare-s3-rlcd-4.2` = ESP32-S3 N16R8 + ST7305 400×300 1-bit — **与本项目的 feature 分支硬件完全一致**。

---

## 4. 方案对比

### 4.1 总览

| 方案 | 描述 | 工程量 | 风险 | 收益 |
|---|---|---|---|---|
| **A. 全 LVGL** | 引入 LVGL 全栈，替换整个显示层 | 极大 | 高（Arduino libsdetect 死锁恶化；v5_idf 尚未完成） | AA 抗锯齿、变宽字体、OTA |
| **B. 增强字库** ★ | 用 xiaozhi 全量 cbin 重新生成 cjk_font.h | 小（改脚本 + 重生成） | 极低 | 覆盖率 98.7% → ~99.9%，消除 □ 缺字 |
| **C. mmap cbin lite** | 移植 cbin 反序列化器 + font 分区 | 中 | 中（cbin 格式随 LVGL 版本变） | OTA 可更新字体 |
| **D. 优化现有格式** | uint16 码点 + 行级 blit | 小 | 低 | 省 Flash、提速渲染 |

### 4.2 决策：方案 B

**理由**:

1. **零代码风险**: 仅改字体生成脚本 + 重生成 cjk_font.h，固件代码零改动
2. **即时收益**: 当前 87 个 GB2312 缺字渲染为 □ → 全量提取后基本消除
3. **成本可控**: Flash +362KB（276KB → 638KB），16MB Flash 宽裕
4. **数据已就绪**: `font_noto_qwen_14_1.bin`（626KB，18129 字形）已在 xiaozhi-esp32 仓库
5. **现有脚本可复用**: `gen_cjk_font_cbin.py` 已实现 cbin 解析 + 字形提取，仅需扩大字符集

---

## 5. 方案 B 实施细节

### 5.1 字符集变化

| 项 | 当前 | 方案 B 后 |
|---|---|---|
| 来源 | GB2312 (7445) ∪ 模型词表 | **cbin 全量** (18129) |
| 字形数 | 7854 | ~18129 |
| BLOB 大小 | ~215KB | ~496KB |
| CP + OFF 表 | ~61KB (uint32×2×7854) | ~141KB (uint32×2×18129) |
| **总二进制** | **~276KB** | **~638KB** |
| .h 源码 | 814KB | ~2.8MB |

### 5.2 脚本修改

`gen_cjk_font_cbin.py` 新增 `--full` 模式:
- `--full`: 提取 cbin 中所有字形（不再限制 GB2312 子集）
- 默认: 保持原有行为（GB2312 ∪ 模型词表）
- 输出路径参数化: `--out` 指定目标 cjk_font.h

### 5.3 分发策略（Flash 分区约束）

**关键约束**: factory 分区仅 0x160000 (1408KB)，当前固件已 1385KB，仅剩 23KB 空间。
全量字体 637KB 需扩展 factory 分区。各变体方案:

| 固件 | 字体 | glyphs | factory 分区 | model 分区 | 说明 |
|---|---|---|---|---|---|
| **v1** (esp32_llm_zh) | **FULL** | 18129 | 0x1C0000 (1792KB) | 0xE20000 (14.13MB) | 分区扩展，model 6.3MB 宽裕 |
| **v2** (esp32_llm_zh_v2) | **FULL** | 18129 | 0x1C0000 (1792KB) | 0x830000 (8.19MB) | 分区扩展，model 7.71MB < 8.19MB ✓ |
| v3 (esp32_llm_zh_v3) | GB2312+vocab | 7854 | 0x160000 (不变) | 不变 | model 8.81MB 太大，无法扩展 |
| v5 (esp32_llm_zh_v5) | GB2312+vocab | 7854 | 0x160000 (不变) | 不变 | model 14.05MB 太大 |
| **v5_idf** | **FULL** | 18129 | 0x160000 (不变) | 不变 | IDF 固件仅 264KB，天然放得下 |

**分区扩展详情** (v1/v2):
- factory: `0x160000` → `0x1C0000` (+400KB，容纳 637KB 字体)
- model 起始: `0x170000` → `0x1D0000`
- model 大小: 相应缩小，但仍容纳现有 model.bin
- **注意**: 分区表变更后需重新烧录 partitions.bin + model.bin 到新地址

**v3/v5 替代方案** (如需全量字体):
1. 等待 v5_idf 成熟后切换（IDF 固件体积仅 1/5）
2. 使用 `--max-glyphs 13000` 限制字符数（牺牲 ~5000 罕用字）
3. 将字体移至独立分区（Plan C，mmap 零拷贝读取）

### 5.4 验证

1. **字符数核对**: 新 cjk_font.h 的 `CJK_N` 应 ≈ 18129
2. **覆盖率统计**: GB2312 覆盖率应 > 99.5%
3. **格式一致性**: CJK_CP/CJK_OFF/CJK_BLOB 结构不变，display.h 零改动
4. **固件编译**: 增量编译验证（复用 D:\esp32-build-zh-v3-test 缓存）

---

## 6. 后续可选优化（方案 D，非本轮）

| 优化 | 效果 | 改动 |
|---|---|---|
| `CJK_CP` uint32→uint16 | 省 ~35KB Flash（18129×2） | 脚本 + display.h 类型 |
| 行级 blit 替代逐像素 | 渲染提速 ~10× | display.h 重写渲染 |
| RLE 压缩 BLOB | BLOB 压缩 ~40-60% | 脚本 + display.h 解码 |

---

## 7. 附录：数据来源

| 文件 | 位置 | 说明 |
|---|---|---|
| xiaozhi cbin (全量) | `D:\codes\xiaozhi-esp32\managed_components\78__xiaozhi-fonts\cbin\font_noto_qwen_14_1.bin` | 626KB, 18129 字形, 1bpp |
| xiaozhi cbin (common) | 同目录 `font_puhui_common_14_1.bin` | 266KB, 6813 字形（Noto Sans SC 子集） |
| 提取脚本 | `chinese/gen_cjk_font_cbin.py` | 本项目 |
| 当前字库 | `firmware/esp32_llm_zh_v2/cjk_font.h` 等 5 份 | 7854 字形，5 份完全一致 |
| 设计文档 | `D:\codes\xiaozhi-esp32\docs\font-system-design.md` | xiaozhi 字体系统设计（422 行） |
