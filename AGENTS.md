# AGENTS.md

> 本文件为 OpenCode/Claude 等 AI 代理提供本仓库的关键上下文,避免踩坑。
> 语言:中文(与仓库内 GETTING_STARTED.md / firmware/README.md / chinese*/README.md 一致)

## 仓库定位

在 **ESP32-S3 N16R8**(16MB Flash / 8MB PSRAM)上运行微型 LLM(PLE 架构,Google Gemma Per-Layer Embedding)。包含训练(Python/uv/WSL-GPU)、量化导出、固件(Arduino)、主机验证(C)四部分。活跃开发硬件为 **Waveshare ESP32-S3-RLCD-4.2**(ST7305 反射屏)——feature 分支名来源。

## 分支策略(重要)

- **所有活跃工作在 `feature/ESP32-S3-4.2inch-RLCD`**(比 main 领先 37+ 提交)。
- `main` 已休眠(HEAD 停留在 SD 卡日志时代)。**不要基于 main 开发**。
- 提交信息用 conventional 前缀(`feat:`/`fix:`/`docs:`/`chore:`),中英混合正文。

## 多环境隔离(本仓库最重要的模式)

存在 **4 套相互隔离的训练+固件环境**,任何跨环境混用都会静默破坏数据:

| 环境 | 训练代码 | 数据 | 检查点 | 产物 | 词表 |
|---|---|---|---|---|---|
| 英文 | `src/` | `data/` | `runs/` | `firmware/model/` | 32,768 BPE |
| 中文 v1 | `chinese/` | `data_chinese/` | `runs_chinese/` | `firmware/model_chinese/` | 5,904 字符 |
| 中文 v2 | `chinese_v2/`(复用 chinese/ 脚本) | `data_v2/` | `runs_v2/` | `firmware/model_v2/` | 6,594 字符 |
| 中文 v3 | `chinese_v3/`(复用 chinese/ 脚本) | `data_v3/` | `runs_v3/` | `firmware/model_v3/` | 7,563 字符 |

**规则**:
- 环境切换靠 `--data-dir` / `--runs-dir` / `--out-dir` 参数,绝不手动复制数据。
- 例如 v2 训练:`uv run python chinese/train.py --data-dir data_v2 --runs-dir runs_v2 --tag zh5`
- 导出 v2:`uv run python chinese/export.py ple-zh5-multi2-s42 --runs-dir runs_v2 --out-dir firmware/model_v2`
- `chinese/quantize.py`、`chinese/export.py` 的 GROUP 常量在 `chinese/export.py` 中定义,各环境共用但**必须用 `--runs-dir`/`--out-dir` 隔离**。

## 共享核心(严禁分叉)

- **`src/model.py`**:所有 4 套环境共用的 PLE 模型代码,仅 `vocab_size` 不同。不要复制/修改单个环境的副本。
- **`firmware/common/llm.h`**:4 个固件版本(`esp32_llm` / `esp32_llm_zh` / `esp32_llm_zh_v2` / `esp32_llm_zh_v3`)全部 `#include "../common/llm.h"`,按 model.bin header(V/D/L/H/F/P)动态适配。改动会影响所有版本。

## 构建/验证命令(精确,勿猜错)

### 训练(需 WSL/GPU,torch cu128)
```bash
# 数据 + 分词器 (v2)
python3 chinese_v2/prepare.py --download
python3 chinese_v2/prepare.py
# SFT 数据 + RAFT
python3 chinese_v2/build_sft.py --count 30000 --val-count 3000
python3 chinese_v2/build_raft.py
# 训练 (v2, GPU)
python3 chinese/train.py --data-dir data_v2 --runs-dir runs_v2 --d-model 160 --n-layers 8 --ple-dim 192 --steps 20000 --tag zh5
python3 chinese/sft/sft_train.py --resume runs_v2/ple-zh5-s42.pt --sft-data data_v2/sft/sft_train.json --val-data data_v2/sft/sft_val.json --data-dir data_v2 --runs-dir runs_v2 --steps 3000 --tag zh5-multi2
```
**注意**:`pyproject.toml` 将 torch 固定到 `pytorch-cu128` 源(RTX 5080 Blackwell sm_120 需要 CUDA 12.8+)。普通 CPU 环境 `uv run` 会尝试下载 2.6GB torch,不要在这类环境跑训练。

### 主机验证(C 端,唯一正确性门禁)
```bash
# Windows 无 gcc 时用 WSL:
wsl gcc -O3 -o /tmp/verify firmware/host_verify/verify.c -I firmware/common -lm
wsl /tmp/verify firmware/model_v2/model.bin firmware/model_v2/golden.txt
# 必须输出 PASS, max abs diff ≤ 1e-5
```
- `verify.c`:golden logit 对比;`firmware/host_verify/ppl.c`:perplexity。
- **无 CI、无测试套件、无 linter**——主机 verify 是唯一的正确性验证。修改 llm.h/export 后必须跑。

### 固件编译(Windows,arduino-cli)
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=info --build-path D:\esp32-build-zh-v2 firmware\esp32_llm_zh_v2
```
- **注意**:中文 v2/v3 的 FQBN 可能不包含 `UploadMode=default`(见 ino 内注释);英文版 README 的 FQBN 是参考。

### 烧录
```powershell
# 固件
arduino-cli upload -p COM4 --fqbn <同上> --input-dir <build-path> firmware\esp32_llm_zh_v2
# 模型(分区地址分版本!)
esptool --chip esp32s3 --port COM4 --baud 921600 write_flash 0x170000 firmware\model_v2\model.bin
# v2 RAG 索引(kb 分区,索引由 chinese/kb/build_index.py 构建于 data_v2/kb/)
esptool --chip esp32s3 --port COM4 --baud 921600 write_flash 0xA00000 data_v2\kb\index.bin
```
- 英文模型在 `0x110000`,中文模型在 `0x170000`,kb 索引在 `0xA00000`。
- 固件与模型分区独立,改固件不需重烧模型。
- `data_v2/kb/index.bin`(1.83MB)是 PC 构建产物(gitignored,本地可能不存在),烧录前需先跑 `chinese/kb/build_index.py`。

### 串口测试工具
```bash
# 需先 uv add pyserial 或在有该依赖的 venv 中运行
python tools/send_prompt.py COM4 --prompt "感冒如何治疗"
python tools/capture_screenshot.py COM4
python tools/cjk_*.py          # CJK 显示/截图验证
```
- `tools/send_prompt.py` 用 HF `tokenizers` 库;中文模型是字符级自定义 tokenizer,**需用 v2 词表**。

## 量化陷阱(硬性规则)

- **英文模型:4-bit group=128**。
- **中文 SFT 模型:必须 4-bit group=32**(group=128 会致生成崩溃/退化,如 ">>>>>"、"痞痞痞" 循环)。
- `chinese/export.py` 顶部 `GROUP = 32` 已设定;若改回去会破坏中文模型。
- **group 参数必须显式传给 `quant_pack`**:`quant_pack(t, group=GROUP)`。曾因默认值(128)与文件头(32)不一致导致布局错位、C 端 NaN。
- **模型二进制格式变更 = 必须重烧模型**:`firmware/common/llm.h` 的读取格式(如 `bind_q` 每 tensor 前读 bits 字节)一旦改动,
  `model.bin` 必须同步重新导出 + **重新烧录到设备**。曾踩坑:llm.h 改为新格式(读 1 字节 bits),设备 Flash 里还是旧格式 model.bin,
  固件/模型错位 → 推理静默失败(生成空输出/乱码,但**速度异常快**是信号:profile FFN/PLE 从 ~108/37ms 骤降至 ~41/14ms)。
  判断方法:对比磁盘 model.bin 大小与设备实际烧录版本;`wsl /tmp/verify model.bin golden.txt` 主机 PASS 只能证明磁盘文件正确,
  不证明设备 Flash 版本匹配。

## 已提交二进制(容易误覆盖)

`.gitignore` 声称忽略 `firmware/model*/model.bin`、`golden.*`、`esp32_llm_zh_v2/vocab.h` 等,但**这些文件实际已被 git 跟踪**(忽略规则在提交之后才加)。重新运行 export.py / gen_vocab.py 会覆盖已提交的二进制——改动前先确认是否需要提交新产物。

- 已跟踪:所有 `firmware/model*/{model.bin,golden.npz,golden.txt}`、`firmware/esp32_llm_zh_v2/vocab.h`、`firmware/esp32_llm_zh_v3/vocab.h`
- 未跟踪(需重新生成):`firmware/esp32_llm/vocab.h`(英文)、`firmware/esp32_llm_zh/vocab.h`(v1)
- **例外**:`data_chinese/tokenizer.json` 被强制跟踪(`!data_chinese/tokenizer.json`)——跨机器重新生成 vocab.h 必需,不要删。

## 固件版本矩阵(4 套固件)

| 版本 | 目录 | 模型 | 词表 | RAG | 显示 |
|---|---|---|---|---|---|
| 英文 | `firmware/esp32_llm/` | cleandeploy 28.9M | 32,768 BPE | 无 | TFT/RLCD |
| 中文 v1 | `firmware/esp32_llm_zh/` | zh4-ds 12.5M | 5,904 | 无 | RLCD + CJK |
| 中文 v2 | `firmware/esp32_llm_zh_v2/` | zh5-multi2/raft 13.7M | 6,594 | ✅ TF-IDF | RLCD + CJK |
| 中文 v3 | `firmware/esp32_llm_zh_v3/` | zh6-raft 15.8M | 7,563 | ✅ | RLCD + CJK |

- v3 固件目录目前**只有 vocab.h**,`.ino`/display 尚未构建(蒸馏 WIP)。
- 中文固件含 `cjk_font.h`(14×14 1bpp 字形,GB2312 ~98.7% 覆盖)+ SFT 标记 token 处理。

## 显示层注意(中文固件)

- `display.h` 的 CJK 渲染:UTF-8 解码 → 二分查找 `CJK_CP` → 14×14 字形,缺字显示 □。
- **SFT 标记 token**(`<BOS>`=2、`<user>`/`<assistant>`/`<end>`)在 prompt 显示与生成中都会被屏蔽(vocab 自适应:v1 在 5901/5902/5903,v2/v3 在 6591/6592/6593)。改动此逻辑需同时改 v1 与 v2 两处。
- 采样参数(`SAMPLING_TEMP`/`TOPK`/`REPETITION_PENALTY`)各固件头部定义,repetition penalty 用于抑制小模型重复循环。

## 文档约定

- README.md / RESULTS.md / MODEL_ANALYSIS_EN.md 为英文;GETTING_STARTED.md / firmware/README.md / chinese*/README.md / 所有 PLAN.md 为中文。新写 AGENTS 类/中文文档保持一致。
- 详细入门教程:GETTING_STARTED.md(中文,含工具链安装、烧录、三语言部署对照、故障排查表)。
- RAG 方案细节:chinese/RAG_PLAN.md + firmware/README.md「RAG 索引存放位置」。
- 模型设计分析:MODEL_ANALYSIS.md(中文)/ MODEL_ANALYSIS_EN.md(英文)。
