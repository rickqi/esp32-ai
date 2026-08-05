@echo off
REM ============================================================
REM V5 IDF H2 烧录脚本 (Windows PowerShell)
REM 分区: bootloader 0x0 | partition 0x8000 | firmware 0x10000 | model 0x170000
REM 前置: COM 口设备, IDF build 产物已存在
REM 用法: powershell -ExecutionPolicy Bypass -File flash_v5.ps1 [-Port COM4]
REM ============================================================
param([string]$Port = "COM3")

$ESP  = "C:\Users\szk220009\.local\bin\esptool.exe"
$BAUD = 921600
$BUILD = "D:\codes\esp32-ai\firmware\esp32_llm_v5_idf\build"
$MODEL = "D:\codes\esp32-ai\firmware\model_v5\H2\model_llm.bin"

# ---- 前置校验: 所有产物必须存在 -----------------------------------------
$files = @(
    @{ Path = "$BUILD\bootloader\bootloader.bin";            Addr = "0x0";      Desc = "bootloader" },
    @{ Path = "$BUILD\partition_table\partition-table.bin";   Addr = "0x8000";   Desc = "partition-table" },
    @{ Path = "$BUILD\esp32_llm_v5.bin";                     Addr = "0x10000";  Desc = "firmware" },
    @{ Path = $MODEL;                                        Addr = "0x170000"; Desc = "model H2" }
)
$missing = $false
foreach ($f in $files) {
    if (-not (Test-Path $f.Path)) {
        Write-Host "[MISSING] $($f.Desc): $($f.Path)" -ForegroundColor Red
        $missing = $true
    } else {
        $sz = (Get-Item $f.Path).Length
        Write-Host "[OK] $($f.Desc): $([math]::Round($sz/1KB)) KB -> $($f.Addr)" -ForegroundColor Green
    }
}
if ($missing) { Write-Host "=== 前置校验失败, 请先构建 ===" -ForegroundColor Red; exit 1 }

Write-Host "=== V5 IDF H2 烧录 (port $Port) ===" -ForegroundColor Cyan
foreach ($f in $files) {
    Write-Host "write $($f.Desc) -> $($f.Addr)"
    & $ESP --chip esp32s3 --port $Port --baud $BAUD write_flash $f.Addr $f.Path
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $($f.Desc)" -ForegroundColor Red; exit 1 }
}
Write-Host "=== 烧录完成 ===" -ForegroundColor Green
