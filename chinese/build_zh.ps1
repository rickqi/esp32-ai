# build_zh.ps1 — Build & flash the Chinese (zh4) firmware for ESP32-S3
#
# Requires:
#   - data_chinese/tokenizer.json   (gitignored — copy from the CUDA machine
#     that ran chinese/prepare.py; it lives at D:\codes\esp32-ai\data_chinese\tokenizer.json there)
#
# Steps:
#   1. Generate Chinese vocab.h from the CharTokenizer
#   2. Compile firmware/esp32_llm_zh
#   3. Print flash commands (firmware + Chinese model.bin @ 0x170000)
#
# Usage:  powershell -ExecutionPolicy Bypass -File chinese/build_zh.ps1 [-Port COM4]

param([string]$Port = "COM4")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tok  = Join-Path $root "data_chinese\tokenizer.json"
$fqbn = 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=info'

# ---- 1. tokenizer check + vocab.h -------------------------------------------
if (-not (Test-Path $tok)) {
    Write-Host "`n[ERROR] data_chinese/tokenizer.json not found." -ForegroundColor Red
    Write-Host "It is gitignored. Copy it from the CUDA training machine:"
    Write-Host "  D:\codes\esp32-ai\data_chinese\tokenizer.json"
    Write-Host "then re-run this script.`n"
    exit 1
}
Write-Host "[1/4] Generating Chinese vocab.h..."
uv run python chinese/gen_vocab.py --tokenizer $tok --out firmware/esp32_llm_zh/vocab.h
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 2. compile -------------------------------------------------------------
Write-Host "[2/4] Compiling zh firmware..."
& "D:\arduino-cli\arduino-cli.exe" compile --fqbn $fqbn `
    --build-property compiler.optimization_flags=-O3 `
    --build-path D:\esp32-build-zh `
    firmware\esp32_llm_zh
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 3. flash firmware ------------------------------------------------------
Write-Host "[3/4] Flashing zh firmware to $Port ..."
& "D:\arduino-cli\arduino-cli.exe" upload -p $Port --fqbn $fqbn `
    --input-dir D:\esp32-build-zh `
    firmware\esp32_llm_zh

# ---- 4. flash Chinese model -------------------------------------------------
Write-Host "[4/4] Flashing Chinese model (model_chinese/model.bin @ 0x170000)..."
esptool --chip esp32s3 --port $Port --baud 921600 write_flash 0x170000 firmware\model_chinese\model.bin

Write-Host "`n[OK] Chinese zh4 firmware flashed. Monitor with:"
Write-Host "  arduino-cli monitor -p $Port --config baudrate=115200"
