

#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"

/* LCD color formats */
#define ESP_LCD_COLOR_FORMAT_RGB565    (1)
#define ESP_LCD_COLOR_FORMAT_RGB888    (2)

/* LCD display color format */
#define BSP_LCD_COLOR_FORMAT        (ESP_LCD_COLOR_FORMAT_RGB565)
/* LCD display color bytes endianess */
#define BSP_LCD_BIGENDIAN           (0)
/* LCD display color bits */
#define BSP_LCD_BITS_PER_PIXEL      (16)
/* LCD display color space */
#define BSP_LCD_COLOR_SPACE         (ESP_LCD_COLOR_SPACE_RGB)
/* LCD display definition */
#define BSP_LCD_H_RES              (410)
#define BSP_LCD_V_RES              (502)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP display configuration structure
 *
 */
typedef struct {
    int max_transfer_sz;    /*!< Maximum transfer size, in bytes. */
} bsp_display_config_t;

/**
 * @brief Create new display panel
 *
 * For maximum flexibility, this function performs only reset and initialization of the display.
 * You must turn on the display explicitly by calling esp_lcd_panel_disp_on_off().
 * The display's backlight is not turned on either. You can use bsp_display_backlight_on/off(),
 * bsp_display_brightness_set() (on supported boards) or implement your own backlight control.
 *
 * If you want to free resources allocated by this function, you can use esp_lcd API, ie.:
 *
 * \code{.c}
 * esp_lcd_panel_del(panel);
 * esp_lcd_panel_io_del(io);
 * spi_bus_free(spi_num_from_configuration);
 * \endcode
 *
 * @param[in]  config    display configuration
 * @param[out] ret_panel esp_lcd panel handle
 * @param[out] ret_io    esp_lcd IO handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd failure
 */
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io);

/**
 * @brief Initialize display's brightness
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_brightness_init(void);

/**
 * @brief Set display's brightness
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new()
 *
 * @param[in] brightness_percent Brightness in [%]
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_brightness_set(int brightness_percent);

int bsp_display_brightness_get(void);

/**
 * @brief Turn on display backlight
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new()
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief Turn off display backlight
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new()
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_backlight_off(void);
esp_err_t bsp_display_clear_black(void);

/**
 * @brief Put display panel into sleep and turn display off
 *
 * Sends Display Off (0x28) and Sleep In (0x10) to the panel.
 * Safe to call repeatedly.
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE If panel is not initialized
 */
esp_err_t bsp_display_sleep(void);

/**
 * @brief Wake display panel from sleep and turn display on
 *
 * Sends Sleep Out (0x11) followed by Display On (0x29) to the panel.
 * Includes the mandatory delay after Sleep Out.
 * Safe to call repeatedly.
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE If panel is not initialized
 */
esp_err_t bsp_display_wake(void);

/**
 * @brief Reinitialise the panel after its ALDO rails were cut and restored
 *
 * power_manager now owns the ALDO rails (DISPLAY lock = ALDO1/2/4) and cuts them
 * during display-off, so on wake the panel IC is power-cycled — DCS Sleep-Out is
 * not enough. Call this once the rails are back on: full reset + init + display-on
 * + black clear. Does NOT touch any ALDO rail (the caller owns rail power).
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE If panel is not initialized
 */
esp_err_t bsp_display_wake_from_gated(void);

/**
 * @brief Prevent bsp_display_sleep() from cutting the ALDO power rails
 *
 * When @p keep is true, the next call(s) to bsp_display_sleep() will skip the
 * ALDO rail gate-off step, leaving those rails powered even while the display
 * panel is in its software sleep state.  Use this when a peripheral on an ALDO
 * rail (e.g. I2C pull-ups, touch IC, audio path) must stay powered during
 * display sleep — typically while audio is actively playing.
 *
 * Callers must pair every keep=true with a keep=false when the need lapses.
 * The flag is not reference-counted; the last writer wins.
 *
 * @param keep true  -> suppress ALDO gate during next sleep(s)
 *             false -> allow normal ALDO gating (default)
 */
void bsp_display_keep_aldo_alive(bool keep);

#ifdef __cplusplus
}
#endif
