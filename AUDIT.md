# Code Audit: esp32_s3_touch_amoled_2_06 BSP

**Audited:** 2026-05-23  
**Component version:** 1.0.3  
**IDF target:** ESP32-S3, IDF ≥ 5.3, LVGL 9.3.0

---

## Executive Summary

This is a well-structured, feature-complete Board Support Package for the Waveshare ESP32-S3-Touch-AMOLED-2.06. The public API is clean, the Kconfig system is thorough, and LVGL thread safety is correctly handled via mutex. The main risk areas are in the power management driver (`axp2101.c`): several I2C operations silently discard their return values, and shared state between the monitor task and the main task is unprotected by a mutex. Two additional `ESP_ERROR_CHECK()` calls in the display initialiser will panic the device on failure rather than propagating a graceful error.

| Severity | Count |
|----------|-------|
| Critical | 2 |
| High | 3 |
| Medium | 2 |
| Low | 5 |
| **Total** | **12** |

---

## Critical

### C1 — Silent failure in `pmu_read_u8()` ([axp2101.c:95](axp2101.c#L95))

```c
static uint8_t pmu_read_u8(uint8_t reg)
{
    uint8_t v = 0;
    (void)i2c_master_transmit_receive(s_pmu_dev, &reg, 1, &v, 1, I2C_MASTER_TIMEOUT_MS);
    return v;
}
```

The return value is cast to `(void)`. On I2C failure `v` stays `0`, which is a valid register value. Every caller that relies on `pmu_read_u8()` — battery percent (`AXP2101_REG_BAT_PERCENT`), IC type — will silently return `0` or a stale value with no indication that the bus failed. A dead I2C bus looks identical to a device reporting 0 % battery and unknown IC type.

**Fix:** Return `esp_err_t` from `pmu_read_u8` and propagate to callers, or at minimum log the failure.

---

### C2 — Shared PMU state accessed without synchronisation ([axp2101.c:13–19](axp2101.c#L13), [111–122](axp2101.c#L111), [124–143](axp2101.c#L124), [296–300](axp2101.c#L296))

The following module-level statics are written from the main task (via `bsp_power_init`, `bsp_power_register_event_cb`, `bsp_power_start_monitor`) and read from the monitor task (`pmu_mon_task`) with no mutex:

```c
static bool s_ready = false;          // written: bsp_power_init, read: every getter + monitor task
static bsp_power_event_cb_t s_cb = NULL;      // written: register_event_cb, read: pmu_emit_evt
static void *s_cb_user = NULL;        // same as above
```

`pmu_emit_evt` (line 114) reads `s_cb` and calls it while the monitor task is running. If `bsp_power_register_event_cb` is called concurrently, the callback pointer and its context pointer can be torn — `s_cb` advances to the new pointer while `s_cb_user` still holds the old context.

`s_ready` is written once but read from both tasks without `volatile` or a memory barrier, which is technically undefined behaviour on architectures with weak memory ordering.

**Fix:** Protect `s_cb`/`s_cb_user` writes and `pmu_emit_evt` reads with a FreeRTOS mutex. Mark `s_ready` as `volatile` or use `atomic` access.

---

## High

### H1 — `xTaskCreate()` return value unchecked ([axp2101.c:307](axp2101.c#L307))

```c
xTaskCreate(pmu_mon_task, "pmu_mon", 3072, NULL, 3, &s_mon_task);
```

If heap is exhausted `xTaskCreate` returns `pdFAIL` and `s_mon_task` is set to `NULL`. The function returns `void` so the caller has no way to detect this. Power events will never fire and all `bsp_power_is_*` polling from application code will work, but no proactive event callbacks will be issued — a silent, hard-to-diagnose failure.

**Fix:**
```c
BaseType_t rc = xTaskCreate(pmu_mon_task, "pmu_mon", 3072, NULL, 3, &s_mon_task);
if (rc != pdPASS) {
    ESP_LOGE(TAG, "pmu_mon task creation failed");
}
```

---

### H2 — `ESP_ERROR_CHECK()` in display init panics on failure ([esp32_s3_touch_amoled_2_06.c:594](esp32_s3_touch_amoled_2_06.c#L594), [610](esp32_s3_touch_amoled_2_06.c#L610), [617](esp32_s3_touch_amoled_2_06.c#L617))

```c
ESP_ERROR_CHECK(spi_bus_initialize(...));                     // line 594
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(..., &io_handle));  // line 610
ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(..., &panel_handle)); // line 617
```

The rest of `bsp_display_new` uses `ESP_RETURN_ON_ERROR` which lets the caller handle failures gracefully. These three calls abort the entire device if the SPI bus or LCD panel fails to initialise — there is no recovery path. This is particularly risky if `bsp_display_new` is called more than once (e.g. after a sleep/wake cycle), since `spi_bus_initialize` will return `ESP_ERR_INVALID_STATE` if the bus is already claimed.

**Fix:** Replace with `ESP_RETURN_ON_ERROR(…, TAG, "…")` and add appropriate error messages.

---

### H3 — Display post-init calls discard return values ([esp32_s3_touch_amoled_2_06.c:618–621](esp32_s3_touch_amoled_2_06.c#L618))

```c
esp_lcd_panel_reset(panel_handle);
esp_lcd_panel_init(panel_handle);
esp_lcd_panel_set_gap(panel_handle, 0x16, 0);
esp_lcd_panel_disp_on_off(panel_handle, true);
```

All four functions return `esp_err_t`. None of them are checked. A failed `esp_lcd_panel_init` will produce a blank or corrupted display with no log output to indicate why. Unlike H2, these are not even wrapped in `ESP_ERROR_CHECK` — failures are completely invisible.

**Fix:** Wrap each in `ESP_RETURN_ON_ERROR` or at minimum log the result.

---

## Medium

### M1 — `pmu_read_u8` also used for clear-IRQ and clear-button paths silently ([axp2101.c:88](axp2101.c#L88), [291](axp2101.c#L291))

Two additional `(void)` casts cover I2C writes whose failure has operational consequences:

```c
// line 88 — clears pending IRQ on init; if this fails, the IRQ fires immediately on first press
(void)i2c_master_transmit(s_pmu_dev, clr_buf, sizeof(clr_buf), I2C_MASTER_TIMEOUT_MS);

// line 291 — clears the button latch after detecting a press; if this fails, every poll returns true
(void)i2c_master_transmit(s_pmu_dev, clr_buf, sizeof(clr_buf), I2C_MASTER_TIMEOUT_MS);
```

For line 291 the consequence is a button that fires on every call to `bsp_power_poll_pwr_button_short()` until the next successful I2C transaction.

**Fix:** Log failures. The `(void)` cast should only be used where the failure is genuinely irrelevant.

---

### M2 — Empty error message in touch I/O init ([esp32_s3_touch_amoled_2_06.c:658](esp32_s3_touch_amoled_2_06.c#L658))

```c
ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
```

The third argument is an empty string. The ESP-IDF log will show the error code but no human-readable context. Every other `ESP_RETURN_ON_ERROR` in the file provides a message.

**Fix:**
```c
ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle),
                    TAG, "touch panel IO init failed");
```

---

## Low

### L1 — Inline helpers in `axp2101.h` use literal `1000` instead of `I2C_MASTER_TIMEOUT_MS` ([axp2101.h:53](axp2101.h#L53), [56](axp2101.h#L56), [62](axp2101.h#L62), [65](axp2101.h#L65), [71](axp2101.h#L71), [72](axp2101.h#L72), [79](axp2101.h#L79), [80](axp2101.h#L80))

`I2C_MASTER_TIMEOUT_MS` is defined as `1000` in [include/bsp/esp32_s3_touch_amoled_2_06.h:47](include/bsp/esp32_s3_touch_amoled_2_06.h#L47). The inline helpers in `axp2101.h` cannot include that header without creating a circular dependency, so the literal `1000` is duplicated eight times. If the timeout is ever changed, `axp2101.h` will silently diverge.

**Fix:** Define a local `AXP2101_I2C_TIMEOUT_MS` constant at the top of `axp2101.h` and use it in all inline functions.

---

### L2 — Magic bit values with no named constants ([axp2101.c:63](axp2101.c#L63), [65](axp2101.c#L65), [67](axp2101.c#L67), [69](axp2101.c#L69), [207](axp2101.c#L207), [217](axp2101.c#L217), [225](axp2101.c#L225))

Several bit masks are inlined without symbolic names:

```c
axp2101_set_bits(..., AXP2101_REG_ADC_CH_CTRL, (1<<5)|(1<<4)|(1<<3)|(1<<2)|(1<<0))  // line 63
return ((v >> 5) & 0x07) == 0x01;   // line 217 — charging status field
return (v1 & (1<<5)) != 0;          // line 225 — VBUS good bit
```

The ADC channel bits, charger status field, and VBUS status bit are not documented in `axp2101.h`. Cross-referencing requires the AXP2101 datasheet.

**Fix:** Add named constants to `axp2101.h` (e.g. `AXP2101_ADC_CH_TEMP`, `AXP2101_STATUS2_CHG_MASK`, `AXP2101_STATUS1_VBUS_GOOD`).

---

### L3 — No `bsp_power_deinit()` or `bsp_audio_deinit()` ([axp2101.c](axp2101.c), [esp32_s3_touch_amoled_2_06.c](esp32_s3_touch_amoled_2_06.c))

`bsp_power_init` creates a FreeRTOS task and registers an I2C device but no corresponding teardown function exists. `bsp_audio_init` opens I2S TX/RX channels with no `bsp_audio_deinit`. By contrast, `bsp_i2c_init` has a matching `bsp_i2c_deinit`. Components used in OTA or test harness scenarios that repeatedly init/deinit will leak resources.

---

### L4 — LVGL v8 compatibility block is dead code under LVGL 9.3.0 ([esp32_s3_touch_amoled_2_06.c:733–738](esp32_s3_touch_amoled_2_06.c#L733))

```c
#else  // LVGL_VERSION_MAJOR < 9
    lv_disp_t *disp_v8 = (lv_disp_t *)disp;
    if (disp_v8 && disp_v8->driver) {
        disp_v8->driver->rounder_cb = bsp_lvgl_rounder_cb;
    }
#endif
```

This branch never compiles with the locked LVGL 9.3.0 dependency. It casts `lv_display_t *` to the v8-internal `lv_disp_t *` and accesses the private `driver` member — both of which are removed in LVGL 9. If someone drops the LVGL version constraint or attempts a backport, this code will produce undefined behaviour.

The block can be removed, or at least annotated that it targets LVGL 8 only.

---

### L5 — No tests or usage examples

The repository contains no test files (`test/`, `pytest_*.py`, `*_test.c`) and no example application demonstrating typical usage. This makes regression testing after hardware changes impossible without a physical board and manual verification.

---

## Positive Findings

- **Modular structure.** Display, touch, audio, power management, and filesystems each have clean, well-named public functions. The separation makes it easy to use only what is needed.
- **Kconfig coverage.** Nearly every tuneable parameter (I2C peripheral, LEDC channel, SD mount point, power button register, LVGL buffer strategy) is exposed via `menuconfig`, avoiding hardcoded assumptions.
- **LVGL thread safety.** `bsp_display_lock` / `bsp_display_unlock` correctly wrap LVGL access with a mutex; the pattern is clearly documented.
- **LVGL 8/9 dual support.** The `#if LVGL_VERSION_MAJOR >= 9` guards throughout the display code provide clean version isolation without ifdef sprawl.
- **Error macro consistency.** `BSP_ERROR_CHECK_RETURN_ERR`, `BSP_ERROR_CHECK_RETURN_NULL`, and `BSP_NULL_CHECK` in `bsp_err_check.h` give uniform, configurable error handling across the BSP — the exceptions noted in H2 stand out precisely because the rest of the file is consistent.
- **Power rail control for sleep/wake.** Gating ALDO rails on display sleep and restoring them on wake is correct and saves meaningful current on battery-powered builds.
- **AXP2101 address probing.** Scanning both `0x34` and `0x35` at init time handles board variants without requiring user configuration.

---

## Recommended Actions

Priority order for addressing findings:

1. **[C1]** Fix `pmu_read_u8()` to propagate `esp_err_t` — silent I2C failures are currently indistinguishable from valid zero readings.
2. **[C2]** Add a FreeRTOS mutex around `s_cb`/`s_cb_user` access in `pmu_emit_evt` and `bsp_power_register_event_cb`; mark `s_ready` as `volatile`.
3. **[H2]** Replace the three `ESP_ERROR_CHECK()` calls in `bsp_display_new` with `ESP_RETURN_ON_ERROR`.
4. **[H3]** Add error checking to the four post-init LCD calls (lines 618–621).
5. **[H1]** Check the return value of `xTaskCreate` in `bsp_power_start_monitor`.
6. **[M1]** Log (don't ignore) I2C failures at lines 88 and 291.
7. **[M2]** Add a meaningful error message to the `ESP_RETURN_ON_ERROR` at line 658.
8. **[L1]** Define `AXP2101_I2C_TIMEOUT_MS` in `axp2101.h` and use it in all inline helpers.
9. **[L2]** Add named bit-field constants to `axp2101.h` for ADC channels, charger status, and VBUS status.
10. **[L3]** Add `bsp_power_deinit()` and `bsp_audio_deinit()` to allow clean teardown.
11. **[L4]** Remove or annotate the dead LVGL v8 compatibility block.
12. **[L5]** Add at minimum one example application showing display + touch + power init.
