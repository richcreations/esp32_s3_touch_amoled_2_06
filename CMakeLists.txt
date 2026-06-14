idf_component_register(
    SRCS "esp32_s3_touch_amoled_2_06.c" "axp2101.c" "bsp_lcd_io_psram_spi.c" ${SRC_VER}
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    # esp_driver_spi: spi_master.h + SPI_TRANS_DMA_USE_PSRAM; esp_hw_support:
    # esp_memory_utils.h (esp_ptr_external_ram); hal: gpio_ll.h/gpio_hal.h —
    # all pulled in by the vendored bsp_lcd_io_psram_spi.c.
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_lcd esp_event esp_driver_spi esp_hw_support hal
    PRIV_REQUIRES esp_timer spiffs esp_psram fatfs usb
)
