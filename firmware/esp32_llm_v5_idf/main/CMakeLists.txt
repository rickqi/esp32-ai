# main component: V5 PLE LLM firmware (ESP-IDF port)
idf_component_register(
    SRCS
        "app_main.c"
        "core/llm_engine.c"
        "core/rag_retrieval.c"
        "core/prompt_encoder.c"
        "boards/board_rlcd.cpp"
        "boards/display_bsp.cpp"
        "boards/keyboard_ble.c"
    INCLUDE_DIRS
        "."
        "core"
        "boards"
    REQUIRES
        esp_partition
        esp_timer
        driver
        fatfs
        sdmmc
        mbedtls
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        bt
        esp_mm
        heap
        soc
        esp_hid
        esp_lcd
        esp_driver_usb_serial_jtag
)

# LLM 推理热路径 (matvec/dequant): 单独 -O2, 避免 -Os 低效内循环.
# (全局 -Os 因 -Og ICE 于 esp_lcd; 仅本文件用 O2 不影响其它代码)
target_compile_options(__idf_main PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-O2>"
    "$<$<COMPILE_LANGUAGE:CXX>:-O2>"
)
