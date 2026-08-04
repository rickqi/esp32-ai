@echo off
REM V5 H2 烧录脚本 (Windows PowerShell 执行)
REM 前置: COM3 设备, 覆盖 XiaoZhi 固件
REM 用法: powershell -ExecutionPolicy Bypass -File flash_v5.ps1

$ESP = "C:\Users\szk220009\.local\bin\esptool.exe"
$PORT = "COM3"
$BAUD = 921600

Write-Host "=== V5 H2 烧录 ==="
Write-Host "1/5 bootloader -> 0x0"
& $ESP --chip esp32s3 --port $PORT --baud $BAUD write_flash 0x0 "D:\esp32-build-zh-v3-test\esp32_llm_zh_v3.ino.bootloader.bin"
Write-Host "2/5 boot_app0  -> 0xE000"
& $ESP --chip esp32s3 --port $PORT --baud $BAUD write_flash 0xE000 "D:\esp32-build-zh-v3-test\boot_app0.bin"
Write-Host "3/5 partitions -> 0x8000"
& $ESP --chip esp32s3 --port $PORT --baud $BAUD write_flash 0x8000 "D:\esp32-build-zh-v3-test\esp32_llm_zh_v5.ino.partitions.bin"
Write-Host "4/5 firmware   -> 0x10000"
& $ESP --chip esp32s3 --port $PORT --baud $BAUD write_flash 0x10000 "D:\esp32-build-zh-v3-test\esp32_llm_zh_v5.ino.bin"
Write-Host "5/5 model H2   -> 0x170000"
& $ESP --chip esp32s3 --port $PORT --baud $BAUD write_flash 0x170000 "D:\codes\esp32-ai\firmware\model_v5\H2\model_llm.bin"
Write-Host "=== 烧录完成 ==="
