# ESP32-S3-Touch-AMOLED-2.06 — Board Support Package

An ESP-IDF Board Support Package (BSP) for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**, a 2.06″ 410×502 capacitive-touch AMOLED development board built around the ESP32-S3.

This is a fork that extends the upstream BSP with first-class **AXP2101 power management**, **display sleep/wake**, and a set of power-saving and robustness fixes (see [What this fork adds](#what-this-fork-adds)).

> **Lineage:** [waveshare](http://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06) → [joaquimorg/esp32_s3_touch_amoled_2_06](https://github.com/joaquimorg/esp32_s3_touch_amoled_2_06) → this fork (`richcreations`). The board has diverged substantially from the original BSP; this README documents the current API, not Waveshare's.

---

## Hardware

| Subsystem | Part | Interface |
|-----------|------|-----------|
| MCU | ESP32-S3, 8 MB PSRAM, 32 MB flash | — |
| Display | 2.06″ AMOLED, 410×502, **SH8601** controller | QSPI (SPI2_HOST), RGB565 |
| Touch | **FT5x06** capacitive | I²C |
| PMU / charger | **AXP2101** | I²C |
| Audio out | **ES8311** codec (speaker / DAC) | I²S + I²C |
| Audio in | **ES7210** codec (microphone / ADC) | I²S + I²C |
| Storage | microSD (SDMMC) + SPIFFS | SDMMC / flash |

The display has **no separate backlight pin** — it is an AMOLED, so "brightness" is driven through the panel itself.

---

## What this fork adds

Relative to a stock Waveshare/joaquimorg BSP:

- **AXP2101 power management** — a complete `bsp_power_*` API (no XPowersLib C++ dependency): battery %, battery/VBUS/system voltage, die temperature, charge/VBUS state, rail control (DC1, ALDO1–4), and PWR-key short-press detection.
- **Power events** — register a callback (`bsp_power_register_event_cb`) and/or consume an ESP-IDF event base (`BSP_POWER_EVENT_BASE`) for VBUS insert/remove and charge start/done. State is refreshed on demand via `bsp_power_refresh_state()` (no dedicated polling task).
- **Display sleep/wake** — `bsp_display_sleep()` / `bsp_display_wake()` issue the panel's Sleep-In/Display-Off and Sleep-Out/Display-On sequences; plus `bsp_display_clear_black()` and `bsp_display_brightness_get()`.
- **PSRAM-direct LCD flush (custom QSPI panel IO)** — the LVGL draw buffer stays in PSRAM (`buff_spiram = true`), and the BSP ships its own SPI/QSPI panel IO ([`bsp_lcd_io_psram_spi.c`](bsp_lcd_io_psram_spi.c)) — a vendored copy of ESP-IDF's `esp_lcd_panel_io_spi.c` with one change: it sets `SPI_TRANS_DMA_USE_PSRAM` on the color transaction. Stock esp_lcd never sets that flag, so a PSRAM buffer is treated as non-DMA and spi_master `malloc`s a fresh ~13 KB **internal** bounce buffer on **every** flush — which fails under radio load (BLE + WiFi + SignalK) with `setup_dma_priv_buffer: Failed to allocate priv TX buffer` and a blank/garbled screen. With the flag, GDMA reads straight from PSRAM (or bounces through a **PSRAM**-allocated copy), so a flush uses **zero internal RAM** and the image is clean. Display init calls `bsp_lcd_new_panel_io_spi()` instead of `esp_lcd_new_panel_io_spi()`. The vendored file is coupled to IDF 5.5.4 — re-sync it on an IDF upgrade.
- **Light-sleep friendly audio** — the I²S RX (mic) channel is initialised but left disabled by default so it doesn't hold an APB power-management lock that would block light sleep.
- **Robustness** — I²C helpers log on failure instead of silently returning `0`; SD-card mount/unmount handle reset correctly. A code audit ([AUDIT.md](AUDIT.md)) and a per-file integrity manifest ([CHECKSUMS.json](CHECKSUMS.json)) are tracked in-tree.

## AXP2101 power rail mapping

This board uses an AXP2101 PMU. The BSP exposes per-rail enable control for the display/peripheral rails while protecting the core system rails.

| PMU output | Board net | Role |
|------------|-----------|------|
| `DCDC1` | `VCC3V3` | Main 3.3V system rail: ESP32-S3 VDD pins, flash, touch, RTC, display logic, and most board logic |
| `ALDO3` | `A3V3` | Analog 3.3V rail for audio codecs and microphone bias |
| `RTCLDO` | `VCC-RTC` | RTC backup supply |
| `ALDO1` | `VL1_3.3V` | Reserved / display-related; no confirmed consumers in the available schematic |
| `ALDO2` | `VL2_3.3V` | Reserved / display-related; no confirmed consumers in the available schematic |
| `ALDO4` | `VL3_1.8V` | Display/touch related supply |
| `BLDO2` | `VL_2.8V` | Display-related supply |
| `DCDC2` | `CORE_0V9` | Internal low-voltage core domain |
| `DCDC3` | `VL_1.2V` | Display-related 1.2V rail |
| `DCDC4` | `1V8_MAIN` | Display-related 1.8V rail |
| `CPUSLDO` | `VCL_1.2V` | ESP32-S3 internal CPU core rail |

> The display subsystem is partly fed from rails that terminate in the AMOLED connector. The exact downstream consumers on `ALDO1`/`ALDO2` are not visible in the board-level schematic.

---

## Requirements

- ESP-IDF **≥ 5.3**
- Target: **esp32s3** (8 MB PSRAM, 32 MB flash)
- LVGL **≥ 8, < 10** (the BSP ships with LVGL enabled; build the `noglib` flow if you want it excluded)
- PSRAM (holds the LVGL draw buffer, flushed via the custom PSRAM-direct panel IO — see [What this fork adds](#what-this-fork-adds))

Pulled-in dependencies (see [idf_component.yml](idf_component.yml)): `waveshare/esp_lcd_sh8601`, `esp_lcd_touch_ft5x06`, `esp_lcd_panel_io_additions`, `espressif/esp_lvgl_port`, `esp_codec_dev`, `lvgl/lvgl`.

---

## Installation

This fork is consumed as a **git dependency** (it is not published to the ESP Component Registry). Add it to your project's `idf_component.yml`:

```yaml
dependencies:
  esp32_s3_touch_amoled_2_06:
    git: https://github.com/richcreations/esp32_s3_touch_amoled_2_06.git
    # pin to a branch, tag, or commit:
    version: "main"
```

Then resolve and build:

```sh
idf.py update-dependencies   # re-resolve to pick up new commits
idf.py build
```

> If pinned to a branch, the exact commit is frozen in `dependencies.lock`; run `idf.py update-dependencies` (or delete the lock) to advance it.

---

## Quick start

```c
#include "bsp/esp32_s3_touch_amoled_2_06.h"

void app_main(void)
{
    // Shared I²C bus (touch, PMU, codec all live here)
    bsp_i2c_init();

    // Display + LVGL
    lv_display_t *disp = bsp_display_start();
    bsp_display_brightness_set(80);

    bsp_display_lock(0);                       // LVGL is not thread-safe
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello AMOLED");
    lv_obj_center(label);
    bsp_display_unlock();

    // Power
    bsp_power_init();
    printf("Battery %d%%, Vbat %d mV, charging=%d\n",
           bsp_power_get_battery_percent(),
           bsp_power_get_batt_voltage_mv(),
           bsp_power_is_charging());
}
```

### Power events

```c
static void on_power(bsp_power_event_t evt, void *ctx)
{
    if (evt == BSP_POWER_EVT_VBUS_INSERT) { /* charger plugged in */ }
}

bsp_power_register_event_cb(on_power, NULL);

// Call periodically (e.g. from a 1 Hz timer/scheduler) to emit transitions:
bsp_power_refresh_state();
```

### microSD

```c
bsp_sdcard_mount();
FILE *f = fopen(BSP_SD_MOUNT_POINT "/hello.txt", "w");
fprintf(f, "card: %s\n", bsp_sdcard->cid.name);
fclose(f);
bsp_sdcard_unmount();   // resets bsp_sdcard to NULL on success
```

### Audio playback (ES8311 speaker)

```c
esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
esp_codec_dev_set_out_vol(spk, 70);

esp_codec_dev_sample_info_t fs = {
    .sample_rate = 22050, .channel = 1, .bits_per_sample = 16,
};
esp_codec_dev_open(spk, &fs);
esp_codec_dev_write(spk, pcm_bytes, pcm_len);
esp_codec_dev_close(spk);
```

### Audio capture (ES7210 microphone)

```c
esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
esp_codec_dev_set_in_gain(mic, 30.0);   // dB

esp_codec_dev_sample_info_t fs = {
    .sample_rate = 16000, .channel = 1, .bits_per_sample = 16,
};
esp_codec_dev_open(mic, &fs);
esp_codec_dev_read(mic, pcm_buf, pcm_len);
esp_codec_dev_close(mic);
```

> **Note:** to save power the BSP initialises but does **not enable** the I²S RX channel by default (an enabled RX channel holds an APB power-management lock that blocks light sleep). Recording requires `i2s_channel_enable(i2s_rx_chan)` — enable it in your build of `bsp_audio_init()` / `bsp_audio_codec_microphone_init()` if you need the mic.

### Power saving

```c
bsp_display_sleep();   // panel off + sleep-in
// ... light sleep / idle ...
bsp_display_wake();    // sleep-out + display on
```

---

## Configuration (`idf.py menuconfig` → *Board Support Package* / *Power*)

| Option | Default | Notes |
|--------|---------|-------|
| `BSP_I2C_NUM` | 1 | I²C peripheral index |
| `BSP_I2C_FAST_MODE` | y | 400 kHz vs 100 kHz |
| `BSP_SPIFFS_MOUNT_POINT` | `/spiffs` | + format-on-fail, partition label, max files |
| `BSP_SD_MOUNT_POINT` | `/sdcard` | + format-on-fail |
| `BSP_LCD_RGB_BUFFER_NUMS` | 1 | >1 enables tear-avoidance modes |
| `BSP_DISPLAY_LVGL_BUF_HEIGHT` | 100 | LVGL buffer height (lines) |
| `BSP_DISPLAY_BRIGHTNESS_LEDC_CH` | 1 | LEDC channel for brightness PWM |
| `BSP_POWER_PKEY_IRQ_STATUS_REG_CH` | 2 | Which AXP2101 INTSTS register holds the PWR-key bit |
| `BSP_POWER_PKEY_SHORT_BIT` | 1 | Bit index of the short-press latch |

---

## API overview

📖 **Full per-function reference: [doc/api.md](doc/api.md).** The summary below is a quick index.

All public APIs are in [`include/bsp/esp32_s3_touch_amoled_2_06.h`](include/bsp/esp32_s3_touch_amoled_2_06.h), [`include/bsp/display.h`](include/bsp/display.h), and [`include/bsp/touch.h`](include/bsp/touch.h).

- **I²C:** `bsp_i2c_init`, `bsp_i2c_deinit`, `bsp_i2c_get_handle`
- **Display (LVGL):** `bsp_display_start`, `bsp_display_start_with_config`, `bsp_display_lock` / `bsp_display_unlock`, `bsp_display_rotate`, `bsp_display_get_input_dev`
- **Display (panel):** `bsp_display_new`, `bsp_display_brightness_set` / `_get`, `bsp_display_backlight_on` / `_off`, `bsp_display_sleep` / `_wake`, `bsp_display_clear_black`
- **Touch:** `bsp_touch_new`
- **Power (AXP2101):** `bsp_power_init` / `_deinit`, readouts (`bsp_power_get_*`, `bsp_power_is_*`), per-rail control (`bsp_power_rail_enable` / `_is_enabled` / `_is_protected`, plus the `bsp_power_enable_aldo*` / `_dc1` wrappers), `bsp_power_poll_pwr_button_short`, `bsp_power_register_event_cb`, `bsp_power_refresh_state`
- **Audio:** `bsp_audio_init` / `_deinit`, `bsp_audio_codec_speaker_init`, `bsp_audio_codec_microphone_init`
- **Storage:** `bsp_sdcard_mount` / `_unmount` (+ `bsp_sdcard`), `bsp_spiffs_mount` / `_unmount`

> **LVGL is not thread-safe** — wrap all `lv_*` calls between `bsp_display_lock()` and `bsp_display_unlock()`.

> **Rail power gating** — `bsp_power_rail_enable()` switches individual AXP2101 rails on/off to save power (voltages are never changed). The SoC/core rails (DCDC1=VCC3V3, DCDC2–4, CPUSLDO) are *protected*: disabling one returns `ESP_ERR_NOT_ALLOWED` so you can't accidentally kill the board. The display runs off the ALDO rails, which are freely switchable.

---

## Pinout

| Function | GPIO | | Function | GPIO |
|----------|------|-|----------|------|
| I²C SCL | 14 | | LCD CS | 12 |
| I²C SDA | 15 | | LCD PCLK | 11 |
| I²S MCLK | 16 | | LCD DATA0–3 | 4, 5, 6, 7 |
| I²S SCLK | 41 | | LCD RST | 8 |
| I²S LCLK | 45 | | Touch RST | 9 |
| I²S DOUT | 40 | | Touch INT | 38 |
| I²S DSIN | 42 | | SD CLK / CMD / D0 | 2 / 1 / 3 |
| Audio PA enable | 46 | | | |

---

## License

**Apache License 2.0** — see [LICENSE](LICENSE). Original BSP © Espressif/Waveshare; fork modifications retain the same license.
