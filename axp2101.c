#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "axp2101.h"

static const char *TAG = "AXP2101";

static i2c_master_dev_handle_t s_pmu_dev = NULL;
static uint8_t s_pmu_addr = AXP2101_I2C_ADDR_PRIMARY;
static volatile bool s_ready = false;
// Persistent state for bsp_power_refresh_state() — initialized in bsp_power_init().
static bool s_prev_vbus = false;
static bool s_prev_chg = false;
static bsp_power_event_cb_t s_cb = NULL;
static void *s_cb_user = NULL;
static SemaphoreHandle_t s_mutex = NULL;

static esp_err_t pmu_add_on_bus(void)
{
    if (s_pmu_dev) return ESP_OK;
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_FAIL;

    const uint8_t candidates[] = { AXP2101_I2C_ADDR_PRIMARY, AXP2101_I2C_ADDR_ALTERNATE };
    esp_err_t perr = ESP_FAIL;
    for (size_t i = 0; i < sizeof(candidates); ++i) {
        perr = i2c_master_probe(bus, candidates[i], I2C_MASTER_TIMEOUT_MS);
        if (perr == ESP_OK) {
            s_pmu_addr = candidates[i];
            break;
        }
    }
    ESP_RETURN_ON_ERROR(perr, TAG, "probe failed");

    // Resolve I2C speed
#ifdef CONFIG_BSP_I2C_CLK_SPEED_HZ
    const uint32_t i2c_speed = CONFIG_BSP_I2C_CLK_SPEED_HZ;
#elif defined(CONFIG_I2C_MASTER_FREQUENCY)
    const uint32_t i2c_speed = CONFIG_I2C_MASTER_FREQUENCY;
#else
    const uint32_t i2c_speed = 400000;
#endif

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_pmu_addr,
        .scl_speed_hz = i2c_speed,
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 },
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_pmu_dev), TAG, "add dev");
    ESP_LOGI(TAG, "AXP210x detected at 0x%02X, I2C %lu Hz", s_pmu_addr, (unsigned long)i2c_speed);
    return ESP_OK;
}

static esp_err_t pmu_config_adc_and_gauge(void)
{
    // Enable ADC main gate + Temp/Sys/VBUS/VBAT channels
    ESP_RETURN_ON_ERROR(axp2101_set_bits(s_pmu_dev, AXP2101_REG_ADC_CH_CTRL,
                                         AXP2101_ADC_GATE_EN | AXP2101_ADC_CH_TEMP |
                                         AXP2101_ADC_CH_SYS  | AXP2101_ADC_CH_VBUS |
                                         AXP2101_ADC_CH_VBAT), TAG, "adc ch");
    // Disable TS pin measurement
    ESP_RETURN_ON_ERROR(axp2101_clear_bits(s_pmu_dev, AXP2101_REG_ADC_CH_CTRL, AXP2101_ADC_CH_TS), TAG, "ts off");
    // Enable battery detection
    ESP_RETURN_ON_ERROR(axp2101_set_bits(s_pmu_dev, AXP2101_REG_BAT_DET_CTRL, AXP2101_BAT_DET_EN), TAG, "bat det");
    // Enable fuel gauge
    ESP_RETURN_ON_ERROR(axp2101_set_bits(s_pmu_dev, AXP2101_REG_CHG_GAUGE_WDT, AXP2101_GAUGE_EN), TAG, "gauge");

    // Enable PWR key short-press IRQ based on Kconfig selection
#ifdef CONFIG_BSP_POWER_PKEY_IRQ_STATUS_REG_CH
    uint8_t irq_reg_ch = CONFIG_BSP_POWER_PKEY_IRQ_STATUS_REG_CH; // 1..3
#else
    uint8_t irq_reg_ch = 2; // default INTSTS2/INTEN2
#endif
#ifdef CONFIG_BSP_POWER_PKEY_SHORT_BIT
    uint8_t pkey_bit = CONFIG_BSP_POWER_PKEY_SHORT_BIT; // 0..7
#else
    uint8_t pkey_bit = 1; // sensible default
#endif
    uint8_t inten_addr = (uint8_t)(AXP2101_REG_INTEN1 + (irq_reg_ch - 1));
    ESP_RETURN_ON_ERROR(axp2101_set_bits(s_pmu_dev, inten_addr, (1u << pkey_bit)), TAG, "pkey inten");

    // Clear any pending IRQ status for that bit
    uint8_t intsts_addr = (uint8_t)(AXP2101_REG_INTSTS1 + (irq_reg_ch - 1));
    uint8_t clr_buf[2] = { intsts_addr, (uint8_t)(1u << pkey_bit) };
    esp_err_t clr_err = i2c_master_transmit(s_pmu_dev, clr_buf, sizeof(clr_buf), I2C_MASTER_TIMEOUT_MS);
    if (clr_err != ESP_OK) {
        ESP_LOGW(TAG, "IRQ status clear failed: %s", esp_err_to_name(clr_err));
    }
    return ESP_OK;
}

static uint8_t pmu_read_u8(uint8_t reg)
{
    uint8_t v = 0;
    esp_err_t err = i2c_master_transmit_receive(s_pmu_dev, &reg, 1, &v, 1, I2C_MASTER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pmu_read_u8(0x%02X) failed: %s", reg, esp_err_to_name(err));
    }
    return v;
}

static void pmu_emit_evt(bsp_power_event_t evt)
{
    if (!s_ready) return;

    bsp_power_event_cb_t cb = NULL;
    void *cb_user = NULL;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        cb = s_cb;
        cb_user = s_cb_user;
        xSemaphoreGive(s_mutex);
    }
    if (cb) cb(evt, cb_user);

    bsp_power_event_payload_t pl = {
        .battery_percent = bsp_power_get_battery_percent(),
        .charging = bsp_power_is_charging(),
        .vbus_in = bsp_power_is_vbus_in(),
        .charger_status = 0,
    };
    esp_err_t post_err = esp_event_post(BSP_POWER_EVENT_BASE, evt, &pl, sizeof(pl), 0);
    if (post_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_event_post failed: %s", esp_err_to_name(post_err));
    }
}

// One-shot AXP2101 state refresh: reads VBUS/charging and posts events on
// transitions. Replaces the previous dedicated pmu_mon_task — callers (e.g.
// task_coordinator subscriber) invoke this on whatever cadence they want.
void bsp_power_refresh_state(void)
{
    if (!s_ready) return;
    bool vbus = bsp_power_is_vbus_in();
    if (vbus != s_prev_vbus) {
        pmu_emit_evt(vbus ? BSP_POWER_EVT_VBUS_INSERT : BSP_POWER_EVT_VBUS_REMOVE);
        s_prev_vbus = vbus;
    }
    bool chg = bsp_power_is_charging();
    if (!s_prev_chg && chg) {
        pmu_emit_evt(BSP_POWER_EVT_CHG_START);
    } else if (s_prev_chg && !chg) {
        pmu_emit_evt(BSP_POWER_EVT_CHG_DONE);
    }
    s_prev_chg = chg;
}

esp_err_t bsp_power_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "mutex alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(pmu_add_on_bus(), TAG, "dev");
    // Read IC type for info
    uint8_t ic = pmu_read_u8(AXP2101_REG_IC_TYPE);
    ESP_LOGI(TAG, "AXP210x IC_TYPE(0x03)=0x%02X", (int)ic);
    ESP_RETURN_ON_ERROR(pmu_config_adc_and_gauge(), TAG, "cfg");
    s_ready = true;
    s_prev_vbus = bsp_power_is_vbus_in();
    s_prev_chg  = bsp_power_is_charging();
    ESP_LOGI(TAG, "AXP2101 initialized: Batt %d%%, Vbat %dmV, Vbus %dmV, Vsys %dmV",
             bsp_power_get_battery_percent(), bsp_power_get_batt_voltage_mv(),
             bsp_power_get_vbus_voltage_mv(), bsp_power_get_system_voltage_mv());
    return ESP_OK;
}

esp_err_t bsp_power_deinit(void)
{
    s_ready = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_cb = NULL;
        s_cb_user = NULL;
        xSemaphoreGive(s_mutex);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_pmu_dev = NULL;
    return ESP_OK;
}

void bsp_power_isr_handler(void)
{
    // No dedicated IRQ pin; polling task handles events.
}

int bsp_power_get_battery_percent(void)
{
    if (!s_ready) return -1;
    return (int)pmu_read_u8(AXP2101_REG_BAT_PERCENT);
}

int bsp_power_get_batt_voltage_mv(void)
{
    if (!s_ready) return -1;
    uint16_t raw = axp2101_read_h5_l8(s_pmu_dev, AXP2101_REG_ADC_RES0, AXP2101_REG_ADC_RES1);
    return (int)raw;
}

int bsp_power_get_vbus_voltage_mv(void)
{
    if (!s_ready) return -1;
    uint16_t raw = axp2101_read_h6_l8(s_pmu_dev, AXP2101_REG_ADC_RES4, AXP2101_REG_ADC_RES5);
    return (int)raw;
}

int bsp_power_get_system_voltage_mv(void)
{
    if (!s_ready) return -1;
    uint16_t raw = axp2101_read_h6_l8(s_pmu_dev, AXP2101_REG_ADC_RES6, AXP2101_REG_ADC_RES7);
    return (int)raw;
}

float bsp_power_get_temperature_c(void)
{
    if (!s_ready) return 0.0f;
    uint16_t raw = axp2101_read_h6_l8(s_pmu_dev, AXP2101_REG_ADC_RES8, AXP2101_REG_ADC_RES9);
    return AXP2101_TEMP_C_FROM_RAW(raw);
}

bool bsp_power_is_battery_connected(void)
{
    if (!s_ready) return false;
    // Fallback: VBAT ADC non-zero considered connected
    uint8_t reg = AXP2101_REG_STATUS1, v = 0;
    if (i2c_master_transmit_receive(s_pmu_dev, &reg, 1, &v, 1, I2C_MASTER_TIMEOUT_MS) == ESP_OK) {
        if (v & AXP2101_STATUS1_BAT_CONN) return true;
    }
    return bsp_power_get_batt_voltage_mv() > 0;
}

bool bsp_power_is_charging(void)
{
    if (!s_ready) return false;
    uint8_t reg = AXP2101_REG_STATUS2, v = 0;
    if (i2c_master_transmit_receive(s_pmu_dev, &reg, 1, &v, 1, I2C_MASTER_TIMEOUT_MS) != ESP_OK) return false;
    return ((v >> AXP2101_STATUS2_CHG_SHIFT) & AXP2101_STATUS2_CHG_MASK) == AXP2101_STATUS2_CHG_CC;
}

bool bsp_power_is_vbus_in(void)
{
    if (!s_ready) return false;
    uint8_t r1 = AXP2101_REG_STATUS1, v1 = 0;
    if (i2c_master_transmit_receive(s_pmu_dev, &r1, 1, &v1, 1, I2C_MASTER_TIMEOUT_MS) != ESP_OK) return false;
    return (v1 & AXP2101_STATUS1_VBUS_GOOD) != 0;
}

// ---- Generic per-rail enable/disable -----------------------------------
// Maps each bsp_power_rail_t to its on/off register and bit. Output voltage is
// never touched here. Register/bit assignments verified against AXP2101
// datasheet section 6.13.2 (REG 0x80 / 0x90 / 0x91).
static const struct { uint8_t reg; uint8_t bit; } s_rail_map[BSP_POWER_RAIL_COUNT] = {
    [BSP_POWER_RAIL_DCDC1]   = { AXP2101_REG_DCDC_ONOFF_CTRL, 0 },
    [BSP_POWER_RAIL_DCDC2]   = { AXP2101_REG_DCDC_ONOFF_CTRL, 1 },
    [BSP_POWER_RAIL_DCDC3]   = { AXP2101_REG_DCDC_ONOFF_CTRL, 2 },
    [BSP_POWER_RAIL_DCDC4]   = { AXP2101_REG_DCDC_ONOFF_CTRL, 3 },
    [BSP_POWER_RAIL_ALDO1]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 0 },
    [BSP_POWER_RAIL_ALDO2]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 1 },
    [BSP_POWER_RAIL_ALDO3]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 2 },
    [BSP_POWER_RAIL_ALDO4]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 3 },
    [BSP_POWER_RAIL_BLDO1]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 4 },
    [BSP_POWER_RAIL_BLDO2]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 5 },
    [BSP_POWER_RAIL_CPUSLDO] = { AXP2101_REG_LDO_ONOFF_CTRL0, 6 },
    [BSP_POWER_RAIL_DLDO1]   = { AXP2101_REG_LDO_ONOFF_CTRL0, 7 },
    [BSP_POWER_RAIL_DLDO2]   = { AXP2101_REG_LDO_ONOFF_CTRL1, 0 },
};

// Rails that power the SoC/core and must never be switched off. Confirmed from
// the board schematic: DCDC1=VCC3V3 (system), DCDC2/3/4 core-class, CPUSLDO core.
#define BSP_POWER_PROTECTED_RAILS                                    \
    ((1u << BSP_POWER_RAIL_DCDC1) | (1u << BSP_POWER_RAIL_DCDC2) |   \
     (1u << BSP_POWER_RAIL_DCDC3) | (1u << BSP_POWER_RAIL_DCDC4) |   \
     (1u << BSP_POWER_RAIL_CPUSLDO))

bool bsp_power_rail_is_protected(bsp_power_rail_t rail)
{
    if ((unsigned)rail >= BSP_POWER_RAIL_COUNT) return false;
    return (BSP_POWER_PROTECTED_RAILS & (1u << rail)) != 0;
}

esp_err_t bsp_power_rail_enable(bsp_power_rail_t rail, bool on)
{
    if (!s_ready || !s_pmu_dev) return ESP_ERR_INVALID_STATE;
    if ((unsigned)rail >= BSP_POWER_RAIL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!on && bsp_power_rail_is_protected(rail)) {
        ESP_LOGW(TAG, "refusing to disable protected rail %d", (int)rail);
        return ESP_ERR_NOT_ALLOWED;
    }
    uint8_t mask = (uint8_t)(1u << s_rail_map[rail].bit);
    // Serialize the read-modify-write: ALDO1-4 (and BLDO/CPUSLDO/DLDO1) all
    // share REG_LDO_ONOFF_CTRL0, and rail toggles come from different tasks
    // (display sleep/wake on the task_coordinator/UI tasks, audio open/close
    // on the audio tasks). set_bits/clear_bits is two I2C transactions —
    // without the mutex, interleaved RMWs lose updates: a display rail left
    // ON after sleep (power regression) or ALDO3 dropped mid-playback.
    // s_mutex is guaranteed non-NULL whenever s_ready is true (bsp_power_init
    // creates it before setting s_ready); held only across two short I2C ops.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = on ? axp2101_set_bits(s_pmu_dev, s_rail_map[rail].reg, mask)
                       : axp2101_clear_bits(s_pmu_dev, s_rail_map[rail].reg, mask);
    xSemaphoreGive(s_mutex);
    return err;
}

int bsp_power_rail_is_enabled(bsp_power_rail_t rail)
{
    if (!s_ready || !s_pmu_dev || (unsigned)rail >= BSP_POWER_RAIL_COUNT) return -1;
    uint8_t reg = s_rail_map[rail].reg, v = 0;
    if (i2c_master_transmit_receive(s_pmu_dev, &reg, 1, &v, 1, I2C_MASTER_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "rail %d state read failed", (int)rail);
        return -1;
    }
    return (v & (1u << s_rail_map[rail].bit)) ? 1 : 0;
}

// Voltage control is intentionally not supported on this board: rail voltages
// are fixed by the schematic and changing them risks the SoC. Report honestly
// rather than silently pretending success (these were previously no-op stubs).
esp_err_t bsp_power_set_dc1_voltage_mv(uint16_t mv)   { (void)mv; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_power_set_aldo1_voltage_mv(uint16_t mv) { (void)mv; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_power_set_aldo2_voltage_mv(uint16_t mv) { (void)mv; return ESP_ERR_NOT_SUPPORTED; }

// Back-compat wrappers — single code path through the generic rail API.
esp_err_t bsp_power_enable_dc1(bool enable)   { return bsp_power_rail_enable(BSP_POWER_RAIL_DCDC1, enable); }
esp_err_t bsp_power_enable_aldo1(bool enable) { return bsp_power_rail_enable(BSP_POWER_RAIL_ALDO1, enable); }
esp_err_t bsp_power_enable_aldo2(bool enable) { return bsp_power_rail_enable(BSP_POWER_RAIL_ALDO2, enable); }
esp_err_t bsp_power_enable_aldo3(bool enable) { return bsp_power_rail_enable(BSP_POWER_RAIL_ALDO3, enable); }
esp_err_t bsp_power_enable_aldo4(bool enable) { return bsp_power_rail_enable(BSP_POWER_RAIL_ALDO4, enable); }

bool bsp_power_poll_pwr_button_short(void)
{
    if (!s_ready) return false;
#ifdef CONFIG_BSP_POWER_PKEY_IRQ_STATUS_REG_CH
    uint8_t irq_reg_ch = CONFIG_BSP_POWER_PKEY_IRQ_STATUS_REG_CH; // 1..3
#else
    uint8_t irq_reg_ch = 2; // default INTSTS2
#endif
#ifdef CONFIG_BSP_POWER_PKEY_SHORT_BIT
    uint8_t pkey_bit = CONFIG_BSP_POWER_PKEY_SHORT_BIT; // 0..7
#else
    uint8_t pkey_bit = 1; // default bit
#endif
    uint8_t reg = (uint8_t)(AXP2101_REG_INTSTS1 + (irq_reg_ch - 1));
    uint8_t v = 0;
    if (i2c_master_transmit_receive(s_pmu_dev, &reg, 1, &v, 1, I2C_MASTER_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    uint8_t mask = (uint8_t)(1u << pkey_bit);
    bool pressed = (v & mask) != 0;
    if (pressed) {
        uint8_t clr_buf[2] = { reg, mask };
        esp_err_t clr_err = i2c_master_transmit(s_pmu_dev, clr_buf, sizeof(clr_buf), I2C_MASTER_TIMEOUT_MS);
        if (clr_err != ESP_OK) {
            ESP_LOGW(TAG, "button latch clear failed: %s", esp_err_to_name(clr_err));
        }
    }
    return pressed;
}

void bsp_power_register_event_cb(bsp_power_event_cb_t cb, void *user)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_cb = cb;
        s_cb_user = user;
        xSemaphoreGive(s_mutex);
    } else {
        s_cb = cb;
        s_cb_user = user;
    }
}

