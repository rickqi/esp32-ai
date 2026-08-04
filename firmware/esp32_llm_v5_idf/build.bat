@echo off
REM Build esp32_llm_v5_idf (ESP-IDF v5.5) on Windows.
REM Usage: build.bat
REM   or: powershell -ExecutionPolicy Bypass -File build.ps1
REM
REM Requires: D:\esp\esp-idf (IDF v5.5), C:\Users\szk220009\.espressif (tools)

set IDF_PATH=D:\esp\esp-idf
set IDF_PYTHON_ENV_PATH=C:\Users\szk220009\.espressif\python_env\idf5.5_py3.11_env
set IDF_TOOLS_PATH=C:\Users\szk220009\.espressif
set PATH=C:\Users\szk220009\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;%IDF_PYTHON_ENV_PATH%\Scripts;C:\Users\szk220009\.espressif\tools\cmake\3.30.2\bin;C:\Users\szk220009\.espressif\tools\ninja\1.12.1;%PATH%

REM First build: set-target + build. Incremental: build only.
if not exist build\CMakeCache.txt (
    python %IDF_PATH%\tools\idf.py set-target esp32s3 || exit /b 1
)
python %IDF_PATH%\tools\idf.py build
