# ESP32-S3 PLE TinyLM — Getting Started (English)

> Run a **28.9M parameter language model** on a **$8 ESP32-S3 microcontroller**.
> Generates short stories locally on the device at ~9.5 tok/s with a TUI display on the 4.2" RLCD screen.
> Hardware: Waveshare ESP32-S3-RLCD-4.2 (or generic ESP32-S3 N16R8 + optional display).

---

## Hardware Requirements

| Component | Requirement | Price |
|---|---|---|
| ESP32-S3 board | **Must be N16R8** (16MB Flash + 8MB PSRAM) | $8–15 |
| Waveshare ESP32-S3-RLCD-4.2 | Has built-in 4.2" RLCD + audio + SD card + sensors | $25–30 |
| USB data cable | Must support data transfer | — |
| (Optional) I2C OLED / SPI TFT | Alternative display options | $3–10 |

---

## 1. Windows Toolchain Installation

### 1.1 Python 3.12+

```powershell
# Download Python 3.12+ from python.org, check "Add to PATH"
python --version   # should show Python 3.12.x
```

### 1.2 uv (Python package manager)

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
uv --version
```

### 1.3 Arduino CLI + ESP32 support

```powershell
# Download arduino-cli from https://github.com/arduino/arduino-cli/releases
# Extract to D:\arduino-cli\, add to PATH
arduino-cli version
arduino-cli config init
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

> ⚠️ If slow, try a phone hotspot or retry later.

### 1.4 Arduino Libraries

```powershell
arduino-cli lib install "Adafruit GFX Library"
# Optional display libraries:
arduino-cli lib install "Adafruit SH110X"      # 1.3" OLED
arduino-cli lib install "Adafruit SSD1306"     # 0.96" OLED
arduino-cli lib install "Adafruit ST7735 and ST7789 Library"  # TFT
```

### 1.5 esptool (flashing tool)

```powershell
uv tool install esptool
esptool version
```

> **Note**: esptool 5.x renamed `esptool.py` to `esptool`.

### 1.6 GCC (for host verification)

**Option A**: WSL (recommended — gcc already present in Ubuntu 22.04)
**Option B**: MinGW-w64 (from niXman/mingw-builds-binaries)
**Option C**: Git Bash (includes gcc)

---

## 2. Project Dependencies

```powershell
cd D:\codes\esp32-ai

# China users — use Tsinghua mirror (otherwise PyTorch download times out):
$env:UV_DEFAULT_INDEX = "https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple"

uv sync
```

This installs torch, numpy, tokenizers, requests, tqdm (28 packages).

---

## 3. Data Preparation

```powershell
# China users — use ModelScope mirror first (huggingface.co is blocked):
# Create and run this helper script, OR use the built-in --mirror if available:
uv run python data/prepare.py --vocab 32768
```

This downloads the first 300MB of TinyStories, trains a BPE tokenizer (vocab=32768), and generates:
- `data/train_v32768.bin` — 74.9M tokens
- `data/val_v32768.bin` — 376K tokens
- `data/bpe32768.json` — BPE tokenizer

> ⚠️ `prepare.py` uses a hardcoded `huggingface.co` URL with raw `requests`, so `HF_ENDPOINT` has **no effect**. Pre-download the 300MB slice from ModelScope (`https://www.modelscope.cn/datasets/AI-ModelScope/TinyStories/resolve/master/TinyStories-train.txt`) to `data/tinystories_slice.txt` — prepare.py checks for this file and skips download if present (≥297MB).

---

## 4. Model Training

> **You MUST train or obtain a checkpoint.** The "download pretrained model" mentioned in earlier versions does not exist — see [Issue #5](https://github.com/slvDev/esp32-ai/issues/5).

```powershell
# PLE model (28.9M params — recommended)
uv run python src/train.py --arm ple --vocab 32768 --d-model 96 --n-layers 6 `
  --ple-dim 128 --target-core 560000 --batch-size 16 --seq-len 256 `
  --steps 5000 --seed 42 --tag cleandeploy
```

- Produces `runs/ple-cleandeploy-s42.pt`
- CPU training: ~3.3 s/step × 5000 ≈ **4.5 hours** (not "days" as the tutorial pessimistically states)
- `get_device()` auto-falls back to CPU on non-CUDA (AMD) machines
- Or train on a free cloud GPU (Colab T4) in ~30 minutes

---

## 5. Quantization & Export

```powershell
uv run python src/quantize.py --tag cleandeploy --seed 42
uv run python src/export.py ple-cleandeploy-s42
```

Outputs:
- `firmware/model/model.bin` — 14.9MB 4-bit quantized model
- `firmware/model/golden.npz` + `golden.txt` — reference logits
- `firmware/model/model.bin`: 14,912,332 bytes

---

## 6. Generate vocab.h

```powershell
uv run python src/gen_assets.py
```

Generates `firmware/esp32_llm/vocab.h` (25,353 tokens, 161KB blob).
Prints prompt token IDs for "Once upon a time" → `[433, 447, 259, 405]`.

---

## 7. Host Verification

```powershell
# Using WSL (recommended — gcc 11.4+):
wsl -e bash -c "cd /mnt/d/codes/esp32-ai && gcc -O3 -o /tmp/v firmware/host_verify/verify.c -lm && /tmp/v firmware/model/model.bin firmware/model/golden.txt"
```

Expected output:
```
max abs diff = 0.00001   rms diff = 0.000001
PASS: C matches PyTorch golden
```

---

## 8. Compile Firmware

```powershell
arduino-cli compile `
  --fqbn 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=info' `
  --build-property compiler.optimization_flags=-O3 `
  --build-path D:\esp32-build `
  firmware\esp32_llm
```

Build output: ~1.31MB firmware (7% of 16MB flash), ~60KB SRAM (18%).

> ⚠️ With WiFi + ADC + SHTC3 enabled, the factory partition was enlarged from 1MB to 1.375MB (`partitions.csv`). The model partition was shifted from 0x110000 to 0x170000.

---

## 9. Flashing

### 9.1 Find the COM port

```powershell
arduino-cli board list
# Look for "ESP32 Family Device" — note the COM port (e.g., COM4)
```

### 9.2 Flash firmware

```powershell
arduino-cli upload `
  -p COM4 `
  --fqbn 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=info' `
  --input-dir D:\esp32-build `
  firmware\esp32_llm
```

### 9.3 Flash model data

```powershell
esptool --chip esp32s3 --port COM4 --baud 921600 write_flash 0x170000 firmware\model\model.bin
```

> **Important**: model offset is **0x170000** (not 0x110000 — the partition was shifted for the enlarged factory partition).

---

## 10. Running & Debugging

### 10.1 Start serial monitor

```powershell
arduino-cli monitor -p COM4 --config baudrate=115200
```

### 10.2 Expected boot output

```
=== ESP32-S3 PLE TinyLM ===
RTC: time set from PCF85063  YYYY-MM-DD HH:MM
WiFi: connecting to rickqi11... (async)
model: V=32768 D=96 L=6 H=4 F=66 P=128  (mapped 15.2 MB)
head staged int8: 2.54 MB
PSRAM free after alloc: 3953 KB

{"ready":true}
[timeout] using demo promptOnce upon a time...
```

### 10.3 Generation throughput

```
--- 200 tokens in 21.00 s ---
throughput: 9.52 tok/s   (102.9 ms/token)
profile ms/token: input 4.4 | attn 25.6 | ffn 6.9 | ple 8.5 | head 57.6
{"done":true,"tok/s":9.52}
```

### 10.4 Interactive prompt (send custom prompt from PC)

```powershell
uv run --with pyserial python tools/send_prompt.py COM4 --prompt "The brave robot" --max-tokens 100
```

---

## 11. TUI Display Layout (v2)

The firmware uses a **three-zone TUI** layout on the 400×300 monochrome RLCD:

```
┌──────────────────────────────────────────────────────────┐  ← 2px border
│ rickqi11   ESP32-S3 PLE LLM             34C 53% 65%    █  ← Header
├──────────────────────────────────────────────────────────┤  ← Divider
  > rick say something                                    ← Prompt
├──────────────────────────────────────────────────────────┤  ← Divider
  was very sad and started to cry...                        ← Output (scrollable)
  ...
├──────────────────────────────────────────────────────────┤  ← Divider
│ 10.7t/s  93ms  28.9M  S3-N16R8  07/30 19:00           █  ← Footer
└──────────────────────────────────────────────────────────┘  ← 2px border
```

### Header fields (fixed position, fixed width)

| Field | Format | Example | x pos | Width |
|---|---|---|---|---|
| WiFi | SSID (8 chars) | `rickqi11` | x=7 | 48px |
| Title | fixed | `ESP32-S3 PLE LLM` | x=62 | 96px |
| Temperature | `%.0fC` | `34C` | right | auto |
| Humidity | `%.0f%%` | `53%` | right | auto |
| Battery | `%d%%` | `65%` | right | 3ch |

### Footer fields

| Field | Format | Example | x pos | Width |
|---|---|---|---|---|
| Throughput | `%5.1ft/s` | ` 9.5t/s` | x=7 | 42px |
| Latency | `%3.0fms` | `098ms` | x=55 | 30px |
| Model | fixed | `28.9M` | x=91 | 30px |
| Hardware | fixed | `S3-N16R8` | x=127 | 48px |
| Date/Time | `%m/%d %H:%M` | `07/30 19:00` | right | 66px |

### Screenshot capture

Send `SHOOT\n` over serial to capture the screen as a PBM → base64 → PC decodes to PNG.

```powershell
uv run --with pyserial,Pillow python tools/capture_screenshot.py COM4 --prompt "The dragon" --max-tokens 80
# Saves to output/screenshot_YYYYMMDD_HHMMSS.png
```

![TUI Display](media/tui-display.png)

### Hardware features

| Feature | Source | Header display |
|---|---|---|
| **WiFi** | Arduino `WiFi.begin("rickqi11", ...)` | SSID or "No WiFi" (left) |
| **Temperature** | SHTC3 sensor (I²C 0x70) | `34C` (right) |
| **Humidity** | SHTC3 sensor (I²C 0x70) | `53%` (right) |
| **Battery** | GPIO4 ADC (3× divider for 18650) | `65%` (right, 3.0V=0% 4.12V=100%) |
| **RTC** | PCF85063 (I²C 0x51) | Footer time (compile-time fallback if invalid) |

---

## 12. Performance

| Metric | Value |
|---|---|
| Throughput | ~9.5 tok/s (measured: 9.28–10.36 tok/s) |
| Time/token | ~103 ms |
| Model size | 14.9 MB (4-bit quantized) |
| Param count | 28.9M (25M in flash lookup table) |
| Flash usage | 7% (1.24MB / 16MB) |
| SRAM usage | 18% (60KB / 327KB) |
| PSRAM free | ~3953 KB |

Profile breakdown: input 4.4ms | attn 25.6ms | ffn 6.9ms | ple 8.5ms | head 57.6ms

---

## 13. Command Quick Reference

| Action | Command |
|---|---|
| Prepare data | `uv run python data/prepare.py --vocab 32768` |
| Train PLE model | `uv run python src/train.py --arm ple --vocab 32768 --steps 5000 --seed 42 --tag cleandeploy ...` |
| Quantize | `uv run python src/quantize.py --tag cleandeploy --seed 42` |
| Export .bin | `uv run python src/export.py ple-cleandeploy-s42` |
| Generate vocab.h | `uv run python src/gen_assets.py` |
| Host verify | `wsl -e bash -c "cd /mnt/d/... && gcc ... && /tmp/v firmware/model/model.bin firmware/model/golden.txt"` |
| Compile | `arduino-cli compile --fqbn '...' --build-path D:\esp32-build firmware\esp32_llm` |
| Flash firmware | `arduino-cli upload -p COM4 --fqbn '...' --input-dir D:\esp32-build firmware\esp32_llm` |
| Flash model | `esptool --chip esp32s3 --port COM4 --baud 921600 write_flash 0x170000 firmware\model\model.bin` |
| Serial monitor | `arduino-cli monitor -p COM4 --config baudrate=115200` |
| Interactive prompt | `uv run --with pyserial python tools/send_prompt.py COM4 --prompt "Hello"` |
| Screenshot | `uv run --with pyserial,Pillow python tools/capture_screenshot.py COM4 --prompt "The sea"` |

---

## 14. Troubleshooting

| Issue | Cause | Fix |
|---|---|---|
| `model partition not found` | Model not flashed or wrong offset | Reflash: `esptool ... write_flash 0x170000 firmware/model/model.bin` |
| `bad model magic` | Corrupted model.bin | Re-run `export.py` |
| No serial output | Wrong port or baud rate | Check `arduino-cli board list` and monitor with `baudrate=115200` |
| `i2cWrite failed` spam | Display type mismatch | Already fixed in current firmware (RLCD uses SPI not I2C) |
| WiFi doesn't connect | Wrong SSID/password or router | Edit `WIFI_SSID`/`WIFI_PASS` in `esp32_llm.ino`, recompile |
| App doesn't fit in partition | Firmware too large | Enlarge factory in `partitions.csv`, reflash model at new offset |

---

## 15. Files Reference

| File | Purpose |
|---|---|
| `firmware/esp32_llm/esp32_llm.ino` | Main sketch (model inference, serial prompt, WiFi/ADC/SHTC3) |
| `firmware/esp32_llm/display.h` | Display driver with ST7305 RLCD branch + TUI layout |
| `firmware/esp32_llm/display_bsp.h/cpp` | ST7305 low-level driver (ported from ESP-IDF wifi_sta project) |
| `firmware/esp32_llm/partitions.csv` | Custom partition table (factory 1.375MB, model 14.5MB) |
| `tools/send_prompt.py` | Send custom prompt via serial |
| `tools/capture_screenshot.py` | Prompt + screenshot capture to PNG |
| `docs/examples/` (in reference project) | Official Waveshare hardware test examples (WiFi, SD, ADC, RTC, SHTC3, Audio) |

---

> This guide corresponds to [esp32-ai](https://github.com/rickqi/esp32-ai) (fork of slvDev/esp32-ai).
> For the original design, see [slvDev/esp32-ai](https://github.com/slvdev/esp32-ai).
