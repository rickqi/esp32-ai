# esp32_llm_v5_idf — ESP-IDF port of the V5 PLE LLM firmware
# (external MiniMind H1/H2 models, per-head q_norm/k_norm via llm_v5.h)
#
# Build (Windows, ESP-IDF v5.5):
#   cd firmware/esp32_llm_v5_idf
#   D:\esp\esp-idf\export.bat          # or export.ps1 in PowerShell
#   idf.py set-target esp32s3
#   idf.py build
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.0")

# disable noisy missing-field warnings from nimble/esp_hid
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_llm_v5)
