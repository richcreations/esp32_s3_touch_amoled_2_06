#pragma once
#include "esp_lcd_panel_io.h"   // esp_lcd_spi_bus_handle_t, esp_lcd_panel_io_spi_config_t, handle

#ifdef __cplusplus
extern "C" {
#endif

// Drop-in replacement for esp_lcd_new_panel_io_spi() that sets
// SPI_TRANS_DMA_USE_PSRAM on color transactions, so a PSRAM LVGL framebuffer is
// flushed with no per-flush INTERNAL bounce buffer (aligned -> direct PSRAM DMA;
// unaligned -> PSRAM-allocated bounce). Same signature/semantics as the IDF
// function. Implemented in bsp_lcd_io_psram_spi.c (a vendored copy of IDF 5.5.4
// esp_lcd_panel_io_spi.c with that one change).
esp_err_t bsp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io);

#ifdef __cplusplus
}
#endif
