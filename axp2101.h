#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// AXP2101 basic register map (subset used)
#define AXP2101_I2C_ADDR_PRIMARY     0x34
#define AXP2101_I2C_ADDR_ALTERNATE   0x35

#define AXP2101_REG_STATUS1          0x00
#define AXP2101_REG_STATUS2          0x01
#define AXP2101_REG_IC_TYPE          0x03

#define AXP2101_REG_ADC_CH_CTRL      0x30
#define AXP2101_REG_ADC_RES0         0x34
#define AXP2101_REG_ADC_RES1         0x35
#define AXP2101_REG_ADC_RES2         0x36
#define AXP2101_REG_ADC_RES3         0x37
#define AXP2101_REG_ADC_RES4         0x38
#define AXP2101_REG_ADC_RES5         0x39
#define AXP2101_REG_ADC_RES6         0x3A
#define AXP2101_REG_ADC_RES7         0x3B
#define AXP2101_REG_ADC_RES8         0x3C
#define AXP2101_REG_ADC_RES9         0x3D

#define AXP2101_REG_INTEN1           0x40
#define AXP2101_REG_INTEN2           0x41
#define AXP2101_REG_INTEN3           0x42
#define AXP2101_REG_INTSTS1          0x48
#define AXP2101_REG_INTSTS2          0x49
#define AXP2101_REG_INTSTS3          0x4A

#define AXP2101_REG_DCDC_ONOFF_CTRL  0x80  // DCDC1-4 enable bits 0..3 (bit4=DCDC5, RO)
#define AXP2101_REG_LDO_ONOFF_CTRL0  0x90  // ALDO1-4 b0-3, BLDO1-2 b4-5, CPUSLDO b6, DLDO1 b7
#define AXP2101_REG_LDO_ONOFF_CTRL1  0x91  // DLDO2 b0

#define AXP2101_REG_BAT_DET_CTRL     0x68
#define AXP2101_REG_BAT_PERCENT      0xA4
#define AXP2101_REG_CHG_GAUGE_WDT    0x18

// ADC temperature conversion (from XPowersLib)
#define AXP2101_TEMP_C_FROM_RAW(raw)  (22.0f + ((7274.0f - (float)(raw)) / 20.0f))

// I2C timeout used by all inline helpers (matches I2C_MASTER_TIMEOUT_MS in the main header)
#define AXP2101_I2C_TIMEOUT_MS  1000

// AXP2101_REG_ADC_CH_CTRL bits
#define AXP2101_ADC_GATE_EN     (1u << 5)  // ADC main gate enable
#define AXP2101_ADC_CH_TEMP     (1u << 4)  // Die temperature channel
#define AXP2101_ADC_CH_SYS      (1u << 3)  // System voltage channel
#define AXP2101_ADC_CH_VBUS     (1u << 2)  // VBUS voltage channel
#define AXP2101_ADC_CH_TS       (1u << 1)  // TS pin channel (disabled in init)
#define AXP2101_ADC_CH_VBAT     (1u << 0)  // Battery voltage channel

// AXP2101_REG_STATUS1 bits
#define AXP2101_STATUS1_VBUS_GOOD  (1u << 5)  // VBUS present and valid
#define AXP2101_STATUS1_BAT_CONN   (1u << 3)  // Battery physically connected

// AXP2101_REG_STATUS2 charger state field
#define AXP2101_STATUS2_CHG_SHIFT  5
#define AXP2101_STATUS2_CHG_MASK   0x07u
#define AXP2101_STATUS2_CHG_CC     0x01u  // Constant-current phase = actively charging

// AXP2101_REG_BAT_DET_CTRL bits
#define AXP2101_BAT_DET_EN      (1u << 0)

// AXP2101_REG_CHG_GAUGE_WDT bits
#define AXP2101_GAUGE_EN        (1u << 3)

// Bit helpers
static inline esp_err_t axp2101_set_bits(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask)
{
    uint8_t v;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &v, 1, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    uint8_t buf[2] = { reg, (uint8_t)(v | mask) };
    return i2c_master_transmit(dev, buf, sizeof(buf), AXP2101_I2C_TIMEOUT_MS);
}

static inline esp_err_t axp2101_clear_bits(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask)
{
    uint8_t v;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &v, 1, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    uint8_t buf[2] = { reg, (uint8_t)(v & (uint8_t)(~mask)) };
    return i2c_master_transmit(dev, buf, sizeof(buf), AXP2101_I2C_TIMEOUT_MS);
}

static inline uint16_t axp2101_read_h5_l8(i2c_master_dev_handle_t dev, uint8_t reg_h, uint8_t reg_l)
{
    uint8_t rh = reg_h, rl = reg_l, vh = 0, vl = 0;
    // Log on failure rather than returning a bare 0 — a dead I2C bus would
    // otherwise be indistinguishable from a real 0 reading (cf. AUDIT.md C1,
    // fixed the same way in pmu_read_u8). "AXP2101" literal is used because the
    // .c file's TAG is not in scope inside this header.
    esp_err_t err = i2c_master_transmit_receive(dev, &rh, 1, &vh, 1, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW("AXP2101", "axp2101_read_h5_l8(0x%02X) failed: %s", reg_h, esp_err_to_name(err));
        return 0;
    }
    err = i2c_master_transmit_receive(dev, &rl, 1, &vl, 1, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW("AXP2101", "axp2101_read_h5_l8(0x%02X) failed: %s", reg_l, esp_err_to_name(err));
        return 0;
    }
    return (uint16_t)(((vh & 0x1F) << 8) | vl);
}

static inline uint16_t axp2101_read_h6_l8(i2c_master_dev_handle_t dev, uint8_t reg_h, uint8_t reg_l)
{
    uint8_t rh = reg_h, rl = reg_l, vh = 0, vl = 0;
    // Log on failure rather than returning a bare 0 — for temperature in
    // particular, AXP2101_TEMP_C_FROM_RAW(0) is ~386 C and looks like real
    // sensor data (cf. AUDIT.md C1). "AXP2101" literal is used because the .c
    // file's TAG is not in scope inside this header.
    esp_err_t err = i2c_master_transmit_receive(dev, &rh, 1, &vh, 1, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW("AXP2101", "axp2101_read_h6_l8(0x%02X) failed: %s", reg_h, esp_err_to_name(err));
        return 0;
    }
    err = i2c_master_transmit_receive(dev, &rl, 1, &vl, 1, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW("AXP2101", "axp2101_read_h6_l8(0x%02X) failed: %s", reg_l, esp_err_to_name(err));
        return 0;
    }
    return (uint16_t)(((vh & 0x3F) << 8) | vl);
}

#ifdef __cplusplus
}
#endif

