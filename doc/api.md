# API Reference — ESP32-S3-Touch-AMOLED-2.06 BSP

Complete reference for the public API of this Board Support Package.

All symbols are declared in the headers under [`include/bsp/`](../include/bsp). Include the umbrella header:

```c
#include "bsp/esp32_s3_touch_amoled_2_06.h"   // or: #include "bsp/esp-bsp.h"
```

`bsp/esp-bsp.h` is a one-line alias for the main header. Display panel/backlight and touch entry points live in `bsp/display.h` and `bsp/touch.h`, both pulled in transitively.

## Contents

- [Conventions](#conventions)
- [Hardware summary](#hardware-summary)
- [Constants & macros](#constants--macros)
- [I²C](#i²c)
- [Power / PMU (AXP2101)](#power--pmu-axp2101)
- [Display — LVGL](#display--lvgl)
- [Display — panel & backlight](#display--panel--backlight)
- [Touch](#touch)
- [Audio](#audio)
- [SPIFFS](#spiffs)
- [microSD](#microsd)
- [Typical init sequence](#typical-init-sequence)
- [Notes on stale header comments](#notes-on-stale-header-comments)

---

## Conventions

- **Return type:** most functions return `esp_err_t` (`ESP_OK` on success). Getters that return `int`/`float`/`bool`/pointers use sentinel values documented per-function (e.g. `-1`, `NULL`).
- **Init order:** `bsp_i2c_init()` underpins the touch, PMU, and audio-codec devices (they share one I²C bus). Most subsystems call it for you, but calling it once up front is harmless and explicit.
- **Thread safety:** LVGL is **not** thread-safe — hold the LVGL mutex (`bsp_display_lock`/`bsp_display_unlock`) around every `lv_*` call. The PMU helpers are safe to call from a single task; `bsp_power_register_event_cb` is mutex-protected.
- **Units:** voltages in millivolts (`_mv`), temperature in °C, brightness/battery in percent.

## Hardware summary

| Subsystem | Device | Bus |
|-----------|--------|-----|
| Display | SH8601 AMOLED, 410×502, RGB565 | QSPI (SPI2_HOST) |
| Touch | FT5x06 | I²C |
| PMU / charger / fuel-gauge | AXP2101 | I²C |
| Audio out | ES8311 (speaker / DAC) | I²S + I²C |
| Audio in | ES7210 (microphone / ADC) | I²S + I²C |
| Storage | microSD (SDMMC 1-bit) + SPIFFS | SDMMC / flash |

---

## Constants & macros

### Capabilities (`bsp/esp-bsp.h`)
`BSP_CAPS_DISPLAY` =1, `BSP_CAPS_TOUCH` =1, `BSP_CAPS_AUDIO` =1, `BSP_CAPS_AUDIO_SPEAKER` =1, `BSP_CAPS_AUDIO_MIC` =1, `BSP_CAPS_SDCARD` =1, `BSP_CAPS_BUTTONS` =0, `BSP_CAPS_IMU` =0.

### Pinout
| Macro | GPIO | | Macro | GPIO |
|-------|------|-|-------|------|
| `BSP_I2C_SCL` | 14 | | `BSP_LCD_CS` | 12 |
| `BSP_I2C_SDA` | 15 | | `BSP_LCD_PCLK` | 11 |
| `BSP_I2S_MCLK` | 16 | | `BSP_LCD_DATA0..3` | 4,5,6,7 |
| `BSP_I2S_SCLK` | 41 | | `BSP_LCD_RST` | 8 |
| `BSP_I2S_LCLK` | 45 | | `BSP_LCD_TOUCH_RST` | 9 |
| `BSP_I2S_DOUT` | 40 | | `BSP_LCD_TOUCH_INT` | 38 |
| `BSP_I2S_DSIN` | 42 | | `BSP_LCD_BACKLIGHT` | `NC` |
| `BSP_POWER_AMP_IO` | 46 | | `BSP_SD_CLK / CMD / D0` | 2 / 1 / 3 |

### Display geometry / format (`bsp/display.h`)
`BSP_LCD_H_RES` =410, `BSP_LCD_V_RES` =502, `BSP_LCD_COLOR_FORMAT` =`ESP_LCD_COLOR_FORMAT_RGB565`, `BSP_LCD_BITS_PER_PIXEL` =16, `BSP_LCD_BIGENDIAN` =0, `BSP_LCD_COLOR_SPACE` =`ESP_LCD_COLOR_SPACE_RGB`.

### Buses, mount points, misc
| Macro | Value | Notes |
|-------|-------|-------|
| `BSP_I2C_NUM` | `CONFIG_BSP_I2C_NUM` | I²C peripheral index |
| `BSP_LCD_SPI_NUM` | `SPI2_HOST` | QSPI host for the panel |
| `I2C_MASTER_TIMEOUT_MS` | 1000 | shared I²C op timeout |
| `BSP_SPIFFS_MOUNT_POINT` | `CONFIG_BSP_SPIFFS_MOUNT_POINT` (`/spiffs`) | |
| `BSP_SD_MOUNT_POINT` | `CONFIG_BSP_SD_MOUNT_POINT` (`/sdcard`) | |
| `BSP_LCD_DRAW_BUFF_SIZE` | `BSP_LCD_H_RES * CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT` | LVGL draw buffer |
| `BSP_CONFIG_NO_GRAPHIC_LIB` | 0 | set to exclude LVGL (`bsp/config.h`) |

---

## I²C

Shared master bus for touch, PMU, and the audio codecs.

```c
esp_err_t                bsp_i2c_init(void);
esp_err_t                bsp_i2c_deinit(void);
i2c_master_bus_handle_t  bsp_i2c_get_handle(void);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_i2c_init` | Install the I²C master driver (idempotent). | `ESP_OK`; `ESP_ERR_INVALID_ARG`; `ESP_FAIL` on install error |
| `bsp_i2c_deinit` | Uninstall the driver and free its resources. | `ESP_OK`; `ESP_ERR_INVALID_ARG` |
| `bsp_i2c_get_handle` | Get the underlying bus handle (e.g. to add your own devices). | bus handle, or `NULL` if not initialized |

---

## Power / PMU (AXP2101)

Battery/charge monitoring, power events, and per-rail power gating. Rail **voltages and charger parameters are intentionally not writable** on this board (fixed by the schematic; changing them risks the SoC).

### Lifecycle

```c
esp_err_t bsp_power_init(void);
esp_err_t bsp_power_deinit(void);
void      bsp_power_isr_handler(void);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_power_init` | Probe the AXP2101, enable the ADC channels (VBAT/VBUS/VSYS/temp), battery detection, fuel gauge, and the PWR-key short-press IRQ. Logs IC type and an initial power summary. | `ESP_OK` or an error code |
| `bsp_power_deinit` | Release the PMU mutex/state and drop the device handle. | `ESP_OK` |
| `bsp_power_isr_handler` | Hook for a PMU IRQ-pin ISR. **Currently a no-op** — this board polls (`bsp_power_poll_pwr_button_short` / `bsp_power_refresh_state`) rather than using a dedicated IRQ GPIO. | — |

### Readouts

```c
int   bsp_power_get_battery_percent(void);
int   bsp_power_get_batt_voltage_mv(void);
int   bsp_power_get_vbus_voltage_mv(void);
int   bsp_power_get_system_voltage_mv(void);
float bsp_power_get_temperature_c(void);
bool  bsp_power_is_battery_connected(void);
bool  bsp_power_is_charging(void);
bool  bsp_power_is_vbus_in(void);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_power_get_battery_percent` | Fuel-gauge state of charge. | 0–100, or `-1` if PMU not ready |
| `bsp_power_get_batt_voltage_mv` | Battery voltage. | mV, or `-1` if not ready |
| `bsp_power_get_vbus_voltage_mv` | VBUS (USB) voltage. | mV, or `-1` if not ready |
| `bsp_power_get_system_voltage_mv` | System (VSYS) voltage. | mV, or `-1` if not ready |
| `bsp_power_get_temperature_c` | AXP2101 die temperature. | °C, or `0.0` if not ready |
| `bsp_power_is_battery_connected` | Battery present (STATUS1 bit, falls back to VBAT>0). | `true`/`false` |
| `bsp_power_is_charging` | Charger in constant-current phase. | `true`/`false` |
| `bsp_power_is_vbus_in` | VBUS present and valid. | `true`/`false` |

> On an I²C read failure a readout logs a warning and returns its not-ready sentinel; a bare `0` from the ADC helpers is similarly logged (so a dead bus is distinguishable from a real reading).

### Per-rail power control

Switch individual AXP2101 rails on/off to power only what you need. Only the rail's enable bit is touched — **voltage is never changed**.

```c
typedef enum {
    BSP_POWER_RAIL_DCDC1, BSP_POWER_RAIL_DCDC2, BSP_POWER_RAIL_DCDC3, BSP_POWER_RAIL_DCDC4,
    BSP_POWER_RAIL_ALDO1, BSP_POWER_RAIL_ALDO2, BSP_POWER_RAIL_ALDO3, BSP_POWER_RAIL_ALDO4,
    BSP_POWER_RAIL_BLDO1, BSP_POWER_RAIL_BLDO2,
    BSP_POWER_RAIL_DLDO1, BSP_POWER_RAIL_DLDO2,
    BSP_POWER_RAIL_CPUSLDO,
    BSP_POWER_RAIL_COUNT
} bsp_power_rail_t;

esp_err_t bsp_power_rail_enable(bsp_power_rail_t rail, bool on);
int       bsp_power_rail_is_enabled(bsp_power_rail_t rail);
bool      bsp_power_rail_is_protected(bsp_power_rail_t rail);
```

**Board rail map** (from the schematic):

| Rail | Net | Role | Protected? |
|------|-----|------|:---------:|
| `DCDC1` | VCC3V3 | main 3.3 V system rail (ESP32-S3 + everything) | **yes** |
| `DCDC2` | 0.9 V | core-class | **yes** |
| `DCDC3` | 1.2 V | core-class | **yes** |
| `DCDC4` | 1.8 V | core-class | **yes** |
| `CPUSLDO` | VCL_1.2V | CPU SLDO, core-class | **yes** |
| `ALDO1` | VL1_3.3V | display / peripheral | no |
| `ALDO2` | VL2_3.3V | display / peripheral | no |
| `ALDO3` | VCC3V | display / peripheral | no |
| `ALDO4` | VL3_1.8V | display / peripheral | no |
| `BLDO1` | (unused) | — | no |
| `BLDO2` | VL_2.8V | peripheral | no |
| `DLDO1` | DC1SW | peripheral / unused | no |
| `DLDO2` | DC4SW | peripheral / unused | no |

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_power_rail_enable` | Enable (`on=true`) or disable (`on=false`) a rail. | `ESP_OK`; `ESP_ERR_INVALID_STATE` (PMU not ready); `ESP_ERR_INVALID_ARG` (bad rail); `ESP_ERR_NOT_ALLOWED` (disabling a protected rail); else I²C error |
| `bsp_power_rail_is_enabled` | Read a rail's current on/off bit. | `1` enabled, `0` disabled, `-1` on error |
| `bsp_power_rail_is_protected` | Whether the rail is in the protected (SoC/core) set. | `true`/`false` |

> **Safety:** disabling a *protected* rail is refused (`ESP_ERR_NOT_ALLOWED`) so you cannot brick the board. Disabling a non-protected rail that still powers something in use (e.g. a display ALDO while the panel is on) is allowed and is the caller's responsibility.

#### Legacy / convenience rail wrappers

Thin wrappers over `bsp_power_rail_enable`, kept for back-compat:

```c
esp_err_t bsp_power_enable_dc1(bool enable);    // -> DCDC1  (disable refused: protected)
esp_err_t bsp_power_enable_aldo1(bool enable);  // -> ALDO1
esp_err_t bsp_power_enable_aldo2(bool enable);  // -> ALDO2
esp_err_t bsp_power_enable_aldo3(bool enable);  // -> ALDO3
esp_err_t bsp_power_enable_aldo4(bool enable);  // -> ALDO4
```

The voltage setters are **not supported** on this board and return `ESP_ERR_NOT_SUPPORTED` (they were previously silent no-ops):

```c
esp_err_t bsp_power_set_dc1_voltage_mv(uint16_t mv);    // ESP_ERR_NOT_SUPPORTED
esp_err_t bsp_power_set_aldo1_voltage_mv(uint16_t mv);  // ESP_ERR_NOT_SUPPORTED
esp_err_t bsp_power_set_aldo2_voltage_mv(uint16_t mv);  // ESP_ERR_NOT_SUPPORTED
```

### Power button & events

```c
bool bsp_power_poll_pwr_button_short(void);
void bsp_power_register_event_cb(bsp_power_event_cb_t cb, void *user_ctx);
void bsp_power_refresh_state(void);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_power_poll_pwr_button_short` | Poll+clear the AXP2101 PWR-key short-press latch. Returns `true` exactly once per press. The latched register/bit are set via Kconfig (`BSP_POWER_PKEY_*`). | `true`/`false` |
| `bsp_power_register_event_cb` | Register a callback for VBUS/charge events (mutex-protected). | — |
| `bsp_power_refresh_state` | One-shot poll of VBUS/charging; emits `BSP_POWER_EVT_*` to the callback **and** posts them on the ESP event base on transitions. Call on whatever cadence you like (e.g. 1 Hz). Replaces the old dedicated monitor task. | — |

```c
typedef enum {
    BSP_POWER_EVT_VBUS_INSERT,
    BSP_POWER_EVT_VBUS_REMOVE,
    BSP_POWER_EVT_CHG_START,
    BSP_POWER_EVT_CHG_DONE,
} bsp_power_event_t;

typedef void (*bsp_power_event_cb_t)(bsp_power_event_t event, void *user_ctx);

ESP_EVENT_DECLARE_BASE(BSP_POWER_EVENT_BASE);

typedef struct {
    int     battery_percent;
    bool    charging;
    bool    vbus_in;
    uint8_t charger_status;   // reserved — currently always 0
} bsp_power_event_payload_t;
```

Events reach you two ways: the registered callback (event id only) and the ESP event loop (`BSP_POWER_EVENT_BASE`, event id = `bsp_power_event_t`, with a `bsp_power_event_payload_t` payload). `charger_status` is currently always `0`.

Example:

```c
static void on_power(bsp_power_event_t evt, void *ctx) {
    if (evt == BSP_POWER_EVT_VBUS_REMOVE) { /* on battery now */ }
}
bsp_power_init();
bsp_power_register_event_cb(on_power, NULL);
/* periodically: */
bsp_power_refresh_state();
```

---

## Display — LVGL

High-level path that brings up the panel and the LVGL port. Available unless `BSP_CONFIG_NO_GRAPHIC_LIB` is set.

```c
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;
    uint32_t        buffer_size;    // screen buffer size in pixels
    uint32_t        trans_size;
    bool            double_buffer;
    struct { unsigned int buff_dma:1; unsigned int buff_spiram:1; } flags;
} bsp_display_cfg_t;

lv_display_t *bsp_display_start(void);
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
lv_indev_t   *bsp_display_get_input_dev(void);
bool          bsp_display_lock(uint32_t timeout_ms);
void          bsp_display_unlock(void);
void          bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_display_start` | Init QSPI + SH8601 + touch and start the LVGL task with defaults. | `lv_display_t*`, or `NULL` on error |
| `bsp_display_start_with_config` | As above, with explicit buffer/PSRAM/DMA config. Backlight/brightness must be set separately. | `lv_display_t*`, or `NULL` |
| `bsp_display_get_input_dev` | The LVGL input (touch) device created during `bsp_display_start`. | `lv_indev_t*`, or `NULL` |
| `bsp_display_lock` | Take the LVGL mutex (`timeout_ms`=0 blocks forever). | `true` if taken |
| `bsp_display_unlock` | Release the LVGL mutex. | — |
| `bsp_display_rotate` | Rotate the display (display must be started). | — |

```c
bsp_display_start();
bsp_display_lock(0);
lv_label_set_text(lv_label_create(lv_screen_active()), "Hi");
bsp_display_unlock();
```

---

## Display — panel & backlight

Lower-level panel control (`bsp/display.h`), usable with or without LVGL.

```c
typedef struct { int max_transfer_sz; } bsp_display_config_t;

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io);
esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
int       bsp_display_brightness_get(void);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);
esp_err_t bsp_display_clear_black(void);
esp_err_t bsp_display_sleep(void);
esp_err_t bsp_display_wake(void);
void      bsp_display_keep_aldo_alive(bool keep);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_display_new` | Create the SH8601 panel + IO handles (reset + init only; does not turn the display on). | `ESP_OK` or esp_lcd error |
| `bsp_display_brightness_init` | Initialize brightness control. | `ESP_OK`; `ESP_ERR_INVALID_ARG` |
| `bsp_display_brightness_set` | Set brightness 0–100 %. (AMOLED: via panel, not a backlight pin.) | `ESP_OK`; `ESP_ERR_INVALID_ARG` |
| `bsp_display_brightness_get` | Last brightness set, in %. | percent |
| `bsp_display_backlight_on` / `_off` | Backlight on/off (maps to panel display on/off; no discrete backlight pin). | `ESP_OK`; `ESP_ERR_INVALID_ARG` |
| `bsp_display_clear_black` | Fill the panel black. | `ESP_OK` or error |
| `bsp_display_sleep` | Display Off (0x28) + Sleep In (0x10); then gates the ALDO display rails unless suppressed (see `bsp_display_keep_aldo_alive`). Safe to call repeatedly. | `ESP_OK`; `ESP_ERR_INVALID_STATE` if panel not initialized |
| `bsp_display_wake` | Re-enables panel rails, Sleep Out (0x11) + Display On (0x29), with the required post-sleep delay. Reinitializes the panel if the rails had been gated. Safe to call repeatedly. | `ESP_OK`; `ESP_ERR_INVALID_STATE` |
| `bsp_display_keep_aldo_alive` | When `keep=true`, the next `bsp_display_sleep()` skips gating the ALDO rails (leaves peripherals on an ALDO rail powered during display sleep, e.g. while audio plays). Not reference-counted — last writer wins; pair every `true` with a `false`. | — |

---

## Touch

```c
typedef struct { void *dummy; } bsp_touch_config_t;   // reserved

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_touch_new` | Create the FT5x06 touch handle (config may be `NULL`). The LVGL path calls this for you; use it directly only for non-LVGL use. | `ESP_OK` or esp_lcd_touch error |

---

## Audio

I²S + codecs, exposed through [`esp_codec_dev`](https://components.espressif.com/components/espressif/esp_codec_dev). Speaker uses **ES8311** (DAC), microphone uses **ES7210** (ADC).

```c
esp_err_t              bsp_audio_init(const i2s_std_config_t *i2s_config);
esp_err_t              bsp_audio_deinit(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_audio_init` | Configure the I²S peripheral and the power-amplifier GPIO. Pass `NULL` for defaults (mono, duplex, 16-bit, 22050 Hz). | `ESP_OK` or I²S error codes |
| `bsp_audio_deinit` | Delete the I²S channels and release resources. | `ESP_OK` |
| `bsp_audio_codec_speaker_init` | Create the ES8311 output codec device (initializes I²C/I²S if needed). | `esp_codec_dev_handle_t`, or `NULL` |
| `bsp_audio_codec_microphone_init` | Create the ES7210 input codec device. | `esp_codec_dev_handle_t`, or `NULL` |

> **Power note:** `bsp_audio_init` initializes the I²S **RX (mic) channel but leaves it disabled** by default, because an enabled RX channel holds an APB power-management lock that blocks light sleep. To record, enable the RX channel (`i2s_channel_enable(i2s_rx_chan)`) before reading.

```c
esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
esp_codec_dev_set_out_vol(spk, 70);
esp_codec_dev_sample_info_t fs = { .sample_rate=22050, .channel=1, .bits_per_sample=16 };
esp_codec_dev_open(spk, &fs);
esp_codec_dev_write(spk, pcm, len);
esp_codec_dev_close(spk);
```

---

## SPIFFS

```c
esp_err_t bsp_spiffs_mount(void);
esp_err_t bsp_spiffs_unmount(void);
```

| Function | Description | Returns |
|----------|-------------|---------|
| `bsp_spiffs_mount` | Mount SPIFFS at `BSP_SPIFFS_MOUNT_POINT`. | `ESP_OK`; `ESP_ERR_INVALID_STATE` (already mounted); `ESP_ERR_NO_MEM`; `ESP_FAIL`; other |
| `bsp_spiffs_unmount` | Unmount SPIFFS. | `ESP_OK`; `ESP_ERR_INVALID_STATE` (already unmounted) |

Access with stdio after mounting, e.g. `fopen(BSP_SPIFFS_MOUNT_POINT "/hello.txt", "w")`.

---

## microSD

```c
extern sdmmc_card_t *bsp_sdcard;          // valid only while mounted (NULL otherwise)

esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);
```

| Symbol | Description | Returns |
|--------|-------------|---------|
| `bsp_sdcard` | Card handle, set by `bsp_sdcard_mount` and reset to `NULL` by `bsp_sdcard_unmount`. Use `bsp_sdcard == NULL` to test "mounted?". | — |
| `bsp_sdcard_mount` | Mount the card (FAT) at `BSP_SD_MOUNT_POINT`. | `ESP_OK`; `ESP_ERR_INVALID_STATE`; `ESP_ERR_NO_MEM`; `ESP_FAIL`; other SDMMC/FATFS errors |
| `bsp_sdcard_unmount` | Unmount and free the card; sets `bsp_sdcard = NULL`. | `ESP_OK`; `ESP_ERR_NOT_FOUND`; `ESP_ERR_INVALID_STATE`; other |

```c
bsp_sdcard_mount();
FILE *f = fopen(BSP_SD_MOUNT_POINT "/log.txt", "a");
fprintf(f, "card %s\n", bsp_sdcard->cid.name);
fclose(f);
bsp_sdcard_unmount();
```

---

## Typical init sequence

```c
#include "bsp/esp32_s3_touch_amoled_2_06.h"

void app_main(void)
{
    bsp_i2c_init();                       // shared bus (touch/PMU/codec)
    bsp_power_init();                     // PMU + fuel gauge

    lv_display_t *disp = bsp_display_start();   // panel + touch + LVGL
    bsp_display_brightness_set(80);

    bsp_display_lock(0);
    /* build UI with lv_* ... */
    bsp_display_unlock();

    // optional: SD, SPIFFS, audio
    bsp_sdcard_mount();
    bsp_spiffs_mount();
    esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
}
```

---

## Notes on stale header comments

A few comments in `include/bsp/esp32_s3_touch_amoled_2_06.h` are leftovers from other Espressif BSPs and do **not** reflect this board — this reference is authoritative where they differ:

- The I²C section mentions "QMA7981 IMU" and "OV2640 camera" — this board has neither (`BSP_CAPS_IMU` = 0). The I²C devices are the FT5x06 touch, AXP2101 PMU, and the ES8311/ES7210 codecs.
- The pinout banner says "ESP-SparkBot-BSP pinout" — pins above are for this board.
- The Power section says "Implemented using XPowersLib" — the AXP2101 driver in this fork is a self-contained register implementation (`axp2101.c` / `axp2101.h`), not XPowersLib.
- The audio section says the ES8311 handles both input and output — output is ES8311, **input is ES7210**.
- `bsp_power_event_payload_t.charger_status` is documented as an `XPOWERS_AXP2101_CHG_*` value but is currently always `0`.
