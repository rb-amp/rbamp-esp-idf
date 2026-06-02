/**
 * @file    rbamp.c
 * @brief   Implementation of the ESP-IDF rbAmp client component.
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * All wire-level protocol invariants from @c libs/spec/SPEC.md are enforced
 * here:
 *  - One I2C address phase per byte (SPEC §6) — every multi-byte read is
 *    a loop of single-byte @c i2c_master_transmit_receive calls.
 *  - 50 ms settle after @c CMD_LATCH_PERIOD (SPEC §7).
 *  - 700 ms settle after @c CMD_SAVE_GAINS (SPEC §11).
 *  - @c REG_V03_PERIOD_VALID checked before consuming a snapshot.
 *  - Two-step address change with 5 s arm window (SPEC §10).
 *
 * v1.0 firmware-driven design notes:
 *  - Variant detection is best-effort (SPEC §8). v1 firmware does not NACK
 *    unmapped registers and there is no dedicated topology byte, so the
 *    library defaults to THREE_PHASE and lets the caller pin a specific
 *    topology via ::rbamp_new_with_topology. Channels above the actual
 *    device count read 0.0 — harmless for Wh integration.
 *  - I2C General-Call is disabled in v1 firmware. ::rbamp_broadcast_latch
 *    returns @c ESP_ERR_NOT_SUPPORTED. Reserved for v2.
 *
 * Targets the new IDF v5.2+ @c i2c_master driver. Legacy @c i2c.h is not
 * supported.
 *
 * @see rbamp.h for the public API.
 */
#include "sdkconfig.h"

/* Gate compile-time log strings by CONFIG_RBAMP_LOG_LEVEL (Kconfig).
 * Must precede esp_log.h, which is pulled in transitively via rbamp.h. */
#if defined(CONFIG_RBAMP_LOG_LEVEL)
#  define LOG_LOCAL_LEVEL CONFIG_RBAMP_LOG_LEVEL
#endif

#include "rbamp.h"
#include "rbamp_energy.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "rbamp";

/* Default I2C transaction timeout — long enough to cover clock stretching
 * during the device's pre-read callback, short enough to fail fast on bus
 * trouble. */
#ifndef CONFIG_RBAMP_I2C_TIMEOUT_MS
#  define CONFIG_RBAMP_I2C_TIMEOUT_MS 100
#endif

#ifndef CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ
#  define CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ 100000
#endif

/* -------------------------------------------------------------------------
 * Handle definition
 * ------------------------------------------------------------------------- */

struct rbamp_obj_t {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t                 addr;

    rbamp_topology_t        topology;
    uint8_t                 channels;
    bool                    has_voltage_hw;
    bool                    topology_user_pinned; /**< true if caller passed an explicit
                                                   *   topology — disable any future
                                                   *   over-write in begin(). */

    /* Period-metering state — master-tracked wall-clock for energy. */
    int64_t                 last_latch_us;
    bool                    have_last_latch;

    /* Address-change two-step state. */
    uint8_t                 pending_addr;
    int64_t                 pending_armed_us;
    bool                    addr_change_armed;

    /* Per-channel CT-model batch tracker (Ask 3 ascending-order hard-fail).
     * Reset by rbamp_set_sensor_class — see SPEC: setting sensor class also
     * resets REG_CT_MODEL device-side so the master-side tracker matches. */
    uint8_t                 ct_model_seq_last_ch;  /**< Last channel passed to set_ct_model_ch. */
    bool                    ct_model_seq_active;   /**< false = batch reset; true = batch in progress. */

    /* Diagnostic counters (Ask 2). Monotonic; no reset API. */
    uint32_t                retry_exhaustion_count;
    uint32_t                sanity_reject_count;

#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_t          energy;
#endif
};

/* -------------------------------------------------------------------------
 * Internal helpers — single-byte I/O honouring the SPEC §6 convention
 * ------------------------------------------------------------------------- */

/** @internal Add a device handle for @c addr on @c bus. */
static esp_err_t _add_device(i2c_master_bus_handle_t bus, uint8_t addr,
                             i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, out);
}

/** @internal Single-byte read at @c reg with NACK-retry per SPEC §B.5.
 *
 * The IDF v5 @c i2c_master driver reports NACK as @c ESP_FAIL and may also
 * leak read-buffer state on partial failures. Slave NACKs ~20 % of reads
 * at 100 kHz; configurable retry (default 3 attempts × 5 ms gap) drops the
 * residual error rate to <0.8 %. v1.1 firmware aims to fix the NACK source
 * at the slave, at which point retry can be set to 1. */
static esp_err_t _read_u8(rbamp_handle_t dev, uint8_t reg, uint8_t *out)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        err = i2c_master_transmit_receive(
            dev->dev, &reg, 1, out, 1,
            CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt + 1 < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_RBAMP_NACK_RETRY_GAP_MS));
        }
    }
    dev->retry_exhaustion_count++;
    return err;
}

/** @internal Single-byte write to @c reg. */
static esp_err_t _write_u8(rbamp_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev->dev, buf, 2, CONFIG_RBAMP_I2C_TIMEOUT_MS);
}

/** @internal Helper: write a single byte to REG_COMMAND. */
static inline esp_err_t _write_cmd(rbamp_handle_t dev, uint8_t opcode)
{
    return _write_u8(dev, RBAMP_REG_COMMAND, opcode);
}

/** @internal uint32 LE read — 4 separate single-byte reads per SPEC §6. */
static esp_err_t _read_u32_le(rbamp_handle_t dev, uint8_t reg, uint32_t *out)
{
    uint8_t b[4];
    for (uint8_t i = 0; i < 4; ++i) {
        esp_err_t err = _read_u8(dev, (uint8_t)(reg + i), &b[i]);
        if (err != ESP_OK) return err;
    }
    *out =  (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return ESP_OK;
}

/** @internal float32 LE read — 4 separate single-byte reads per SPEC §6.
 *
 * Per SPEC §B.5, the assembled float is sanity-checked against numerically
 * exotic patterns (NaN / Inf / out-of-range) that survive the per-byte retry
 * loop. The filter intentionally has **no** physical lower bounds, so
 * legitimate brownout (U ≈ 80 V), mains disconnect (U = 0 V), or off-grid
 * test scenarios pass through unmodified. */
static esp_err_t _read_float_le(rbamp_handle_t dev, uint8_t reg, float *out)
{
    uint8_t b[4];
    for (uint8_t i = 0; i < 4; ++i) {
        esp_err_t err = _read_u8(dev, (uint8_t)(reg + i), &b[i]);
        if (err != ESP_OK) return err;
    }
    memcpy(out, b, 4);  /* portable bit-reinterpret; no strict-aliasing UB */
    if (!isfinite(*out) || fabsf(*out) > 10000.0f) {
        dev->sanity_reject_count++;
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/** @internal Sleep @c ms milliseconds via FreeRTOS. */
static inline void _sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_new(i2c_master_bus_handle_t bus, uint8_t addr, rbamp_handle_t *out)
{
    /* Default topology: THREE_PHASE. Over-detection is harmless because
     * unused current channels read 0.0 (see SPEC §8). Pass an explicit
     * topology via rbamp_new_with_topology() to suppress the default. */
    return rbamp_new_with_topology(bus, addr, RBAMP_TOPOLOGY_THREE_PHASE, out);
}

esp_err_t rbamp_new_with_topology(i2c_master_bus_handle_t bus, uint8_t addr,
                                  rbamp_topology_t topology, rbamp_handle_t *out)
{
    if (!out || !bus || addr < 0x08 || addr > 0x77) {
        return ESP_ERR_INVALID_ARG;
    }
    if (topology < RBAMP_TOPOLOGY_SINGLE || topology > RBAMP_TOPOLOGY_THREE_PHASE) {
        return ESP_ERR_INVALID_ARG;
    }
    rbamp_handle_t dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }
    dev->bus = bus;
    dev->addr = addr;
    dev->topology = topology;
    dev->channels = (uint8_t)topology;
    dev->topology_user_pinned = true;
    dev->has_voltage_hw = false;
    dev->have_last_latch = false;
    dev->addr_change_armed = false;
#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_init(&dev->energy);
#endif

    esp_err_t err = _add_device(bus, addr, &dev->dev);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }
    *out = dev;
    return ESP_OK;
}

void rbamp_del(rbamp_handle_t dev)
{
    if (!dev) return;
    if (dev->dev) {
        (void)i2c_master_bus_rm_device(dev->dev);
    }
    free(dev);
}

/**
 * @internal
 * Variant probe (revised v1 — SPEC §8 best-effort).
 *
 * v1 firmware ACKs every read and returns 0x00 for unmapped registers, so the
 * original NACK-probe approach silently misclassified single-variant devices.
 * The library keeps the constructor-supplied topology (or the THREE_PHASE
 * default from rbamp_new) and only probes voltage hardware via the U_rms
 * threshold — which still works because no-voltage variants return 0x00*4 = 0.0f.
 */
static void _detect_variant(rbamp_handle_t dev)
{
    /* Topology is already set by the constructor — don't override it.
     * Channels above the actual device count read as 0.0 and contribute 0.0
     * to all downstream metrics. */

    float u_rms = 0.0f;
    if (_read_float_le(dev, RBAMP_REG_V03_U_RMS, &u_rms) == ESP_OK
            && !isnan(u_rms) && !isinf(u_rms) && u_rms > 1.0f) {
        dev->has_voltage_hw = true;
    } else {
        dev->has_voltage_hw = false;
    }

    ESP_LOGI(TAG, "variant: channels=%u%s has_voltage_hw=%s",
             dev->channels,
             dev->topology_user_pinned ? " (user-pinned)" : " (default)",
             dev->has_voltage_hw ? "yes" : "no");
}

esp_err_t rbamp_begin(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;

    /* 1. Probe REG_VERSION */
    uint8_t version = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe failed at 0x%02X: %s", dev->addr, esp_err_to_name(err));
        return err;
    }
    if (version == 0 || version == 0xFF) {
        ESP_LOGE(TAG, "unsupported firmware version 0x%02X", version);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 2. Variant probe (voltage HW only — topology stays as constructed). */
    _detect_variant(dev);

    /* 3. Primer LATCH — discard first accumulator window. Record
     *    last_latch_us RIGHT AFTER the bus write (before settle) so the
     *    first user-visible read_period_snapshot reports a master_dt_ms
     *    consistent with subsequent cycles. */
    err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
    if (err != ESP_OK) return err;
    dev->last_latch_us = esp_timer_get_time();
    dev->have_last_latch = true;
    _sleep_ms(RBAMP_SETTLE_MS_LATCH_PERIOD);

    return ESP_OK;
}

esp_err_t rbamp_probe(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    uint8_t v = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &v);
    if (err != ESP_OK) return err;
    if (v == 0 || v == 0xFF) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

esp_err_t rbamp_wait_ready(rbamp_handle_t dev, uint32_t timeout_ms)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* Poll REG_STATUS (0x00) bit0=READY — sticky/non-destructive.
     * NOT REG_V03_STATUS (0xCE) which is "cleared on read" and races with
     * the firmware's commit thread. */
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t status = 0;
        if (_read_u8(dev, RBAMP_REG_STATUS, &status) == ESP_OK
                && (status & 0x01)) {
            return ESP_OK;
        }
        _sleep_ms(10);
    }
    return ESP_ERR_TIMEOUT;
}

uint8_t rbamp_firmware_version(rbamp_handle_t dev)
{
    if (!dev) return 0;
    uint8_t v = 0;
    return (_read_u8(dev, RBAMP_REG_VERSION, &v) == ESP_OK) ? v : 0;
}

rbamp_topology_t rbamp_topology(rbamp_handle_t dev)
{
    return dev ? dev->topology : RBAMP_TOPOLOGY_SINGLE;
}

uint8_t rbamp_channels(rbamp_handle_t dev)
{
    return dev ? dev->channels : 0;
}

bool rbamp_has_voltage_hw(rbamp_handle_t dev)
{
    return dev ? dev->has_voltage_hw : false;
}

uint8_t rbamp_address(rbamp_handle_t dev)
{
    return dev ? dev->addr : 0;
}

/* -------------------------------------------------------------------------
 * Real-time reads
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_read_voltage(rbamp_handle_t dev, uint8_t phase, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (phase != 0) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_U_RMS, out);
}

esp_err_t rbamp_read_voltage_peak(rbamp_handle_t dev, uint8_t phase, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (phase != 0) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_U_PEAK, out);
}

esp_err_t rbamp_read_current(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_I0_RMS + ch * 4), out);
}

esp_err_t rbamp_read_current_peak(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_I0_PEAK + ch * 4), out);
}

esp_err_t rbamp_read_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_P0_REAL + ch * 4), out);
}

esp_err_t rbamp_read_power_factor(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_PF0 + ch * 4), out);
}

esp_err_t rbamp_read_frequency(rbamp_handle_t dev, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t f = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_AC_FREQ, &f);
    if (err != ESP_OK) return err;
    *out = (float)f;
    return ESP_OK;
}

esp_err_t rbamp_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->topology = dev->topology;
    out->channels = dev->channels;
    out->has_voltage_hw = dev->has_voltage_hw;

    esp_err_t err;
    err = rbamp_read_voltage(dev, 0, &out->voltage);              if (err != ESP_OK) return err;
    err = rbamp_read_voltage_peak(dev, 0, &out->voltage_peak);    if (err != ESP_OK) return err;
    for (uint8_t ch = 0; ch < dev->channels; ++ch) {
        err = rbamp_read_current(dev, ch, &out->current[ch]);             if (err != ESP_OK) return err;
        err = rbamp_read_current_peak(dev, ch, &out->current_peak[ch]);   if (err != ESP_OK) return err;
        err = rbamp_read_power(dev, ch, &out->power[ch]);                 if (err != ESP_OK) return err;
        err = rbamp_read_power_factor(dev, ch, &out->power_factor[ch]);   if (err != ESP_OK) return err;
    }
    return rbamp_read_frequency(dev, &out->frequency);
}

/* -------------------------------------------------------------------------
 * Period metering
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_latch_period(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
    if (err == ESP_OK) {
        /* Stamp master wall-clock so a subsequent rbamp_read_period_snapshot
         * with skip_latch=true reports an accurate master_dt_ms. */
        dev->last_latch_us = esp_timer_get_time();
        dev->have_last_latch = true;
    }
    return err;
}

esp_err_t rbamp_is_period_valid(rbamp_handle_t dev, bool *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t v = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_V03_PERIOD_VALID, &v);
    if (err != ESP_OK) return err;
    *out = (v & 0x01) != 0;
    return ESP_OK;
}

esp_err_t rbamp_read_period_avg_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    /* Non-contiguous register addresses per SPEC §7: ch0=0xDC, ch1=0xC2, ch2=0xC6. */
    uint8_t reg;
    switch (ch) {
        case 0: reg = RBAMP_REG_V03_PERIOD_AVG_P_F0; break;
        case 1: reg = RBAMP_REG_V03_PERIOD_AVG_P_F1; break;
        case 2: reg = RBAMP_REG_V03_PERIOD_AVG_P_F2; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    return _read_float_le(dev, reg, out);
}

esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_PERIOD_MAX_P_F0, out);
}

esp_err_t rbamp_read_period_latch_ms(rbamp_handle_t dev, uint32_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u32_le(dev, RBAMP_REG_V03_PERIOD_LATCH_MS, out);
}

esp_err_t rbamp_read_period_snapshot(rbamp_handle_t dev,
                                     rbamp_period_snapshot_t *out,
                                     uint16_t settle_ms,
                                     bool skip_latch)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t err;

    /* 1. Latch (skip if caller used broadcast_latch or manual rbamp_latch_period). */
    if (!skip_latch) {
        err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
        if (err != ESP_OK) return err;
    }

    /* 2. Capture master wall-clock for dt computation. */
    const int64_t now_us = esp_timer_get_time();
    if (dev->have_last_latch) {
        out->master_dt_ms = (uint32_t)((now_us - dev->last_latch_us) / 1000);
    }

    /* 3. Settle. */
    _sleep_ms(settle_ms);

    /* 4. Verify the snapshot is fresh. */
    bool valid = false;
    err = rbamp_is_period_valid(dev, &valid);
    if (err != ESP_OK) return err;
    if (!valid) {
        /* Stamp last_latch_us even on stale so the NEXT successful snapshot
         * integrates over a single period instead of 2× period — otherwise Wh
         * doubles on the recovery cycle. SPEC §7 / audit cross-cutting HIGH #3. */
        dev->last_latch_us = now_us;
        dev->have_last_latch = true;
        ESP_LOGW(TAG, "period STALE — discarded");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 5. Read avg_p per channel + max_p + latch_ms. */
    for (uint8_t ch = 0; ch < dev->channels; ++ch) {
        err = rbamp_read_period_avg_power(dev, ch, &out->avg_p[ch]);
        if (err != ESP_OK) return err;
    }
    err = rbamp_read_period_max_power(dev, &out->max_p);
    if (err != ESP_OK) return err;
    err = rbamp_read_period_latch_ms(dev, &out->latch_ms);
    if (err != ESP_OK) return err;
    out->valid = true;

    /* 6. Commit master timestamp & integrate into Wh accumulator. */
    dev->last_latch_us = now_us;
    dev->have_last_latch = true;
#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_tick(&dev->energy, out->avg_p, dev->channels,
                      out->master_dt_ms, out->valid);
#endif

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Energy
 * ------------------------------------------------------------------------- */

#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
double rbamp_energy_wh(rbamp_handle_t dev, uint8_t ch)
{
    if (!dev) return 0.0;
    return rbamp_energy_wh_get(&dev->energy, ch);
}

void rbamp_energy_reset(rbamp_handle_t dev, uint8_t ch)
{
    if (dev) rbamp_energy_reset_ch(&dev->energy, ch);
}

void rbamp_energy_reset_all(rbamp_handle_t dev)
{
    if (dev) rbamp_energy_reset_all_ch(&dev->energy);
}

void rbamp_energy_disable(rbamp_handle_t dev)
{
    if (dev) rbamp_energy_set_enabled(&dev->energy, false);
}

void rbamp_energy_enable(rbamp_handle_t dev)
{
    if (dev) rbamp_energy_set_enabled(&dev->energy, true);
}
#endif

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

/**
 * @internal
 * Check the v1.2+ sensor-class precondition shared by ::rbamp_set_ct_model
 * and ::rbamp_set_ct_model_ch.
 *
 * On v1.0 / v1.1 firmware (@c REG_VERSION < 0x03) the guard is skipped — the
 * device-side callback has no precondition and we preserve backward
 * compatibility. On v1.2+ the guard reads @c REG_SENSOR_CLASS and returns
 * @c ESP_ERR_INVALID_STATE if it is @c RBAMP_SENSOR_UNSET.
 *
 * Returns @c ESP_OK if the caller may proceed with the CT-model write.
 */
static esp_err_t _check_sensor_class_set(rbamp_handle_t dev)
{
    uint8_t version = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &version);
    if (err != ESP_OK) return err;
    if (version < 0x03) return ESP_OK;  /* v1.0 / v1.1 — no guard */
    uint8_t cls = 0;
    err = _read_u8(dev, RBAMP_REG_SENSOR_CLASS, &cls);
    if (err != ESP_OK) return err;
    if (cls == (uint8_t)RBAMP_SENSOR_UNSET) {
        ESP_LOGW(TAG, "set_ct_model refused: sensor class is UNSET "
                      "(call rbamp_set_sensor_class first)");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev, rbamp_sensor_class_t cls)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if ((uint8_t)cls > (uint8_t)RBAMP_SENSOR_BUILTIN_CT) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_u8(dev, RBAMP_REG_SENSOR_CLASS, (uint8_t)cls);
    if (err != ESP_OK) return err;
    err = rbamp_save_gains(dev);
    if (err != ESP_OK) return err;
    /* Reset the per-channel CT-model batch tracker — REG_CT_MODEL is also
     * reset device-side (SPEC §11), so the master-side tracker matches. */
    dev->ct_model_seq_active = false;
    return ESP_OK;
}

esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (code < 1 || code > 5) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _check_sensor_class_set(dev);
    if (err != ESP_OK) return err;
    err = _write_u8(dev, RBAMP_REG_CT_MODEL, code);
    if (err != ESP_OK) return err;
    return rbamp_save_gains(dev);
}

esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (channel > 2) return ESP_ERR_INVALID_ARG;
    if (code < 1 || code > 5) return ESP_ERR_INVALID_ARG;

    /* v1.2+ requirement — CMD_SET_CT_MODEL_CH* does not exist on older fw. */
    uint8_t version = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &version);
    if (err != ESP_OK) return err;
    if (version < 0x03) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: firmware 0x%02X < v1.2", version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Same sensor-class precondition as the single-arg form. */
    uint8_t cls = 0;
    err = _read_u8(dev, RBAMP_REG_SENSOR_CLASS, &cls);
    if (err != ESP_OK) return err;
    if (cls == (uint8_t)RBAMP_SENSOR_UNSET) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: sensor class is UNSET "
                      "(call rbamp_set_sensor_class first)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ascending-order hard-fail (Ask 3 / SPEC descending-channel batch rule).
     * Tracker is reset by rbamp_set_sensor_class — see that function's docs. */
    if (dev->ct_model_seq_active && channel >= dev->ct_model_seq_last_ch) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: channel %u after %u "
                      "(descending order required; call rbamp_set_sensor_class "
                      "first to reset batch)",
                 channel, dev->ct_model_seq_last_ch);
        return ESP_ERR_INVALID_STATE;
    }

    /* Per SPEC: write REG_CT_MODEL with the preset, then issue the
     * per-channel CMD which routes the in-RAM preset to the requested
     * channel's gain registers. Direct-write side-effect: ch0 is always
     * clobbered to the same preset. Caller must invoke higher channels
     * first if configuring different presets per channel — see the
     * @warning block in rbamp.h. */
    err = _write_u8(dev, RBAMP_REG_CT_MODEL, code);
    if (err != ESP_OK) return err;

    static const uint8_t kCmdPerCh[3] = {
        RBAMP_CMD_SET_CT_MODEL_CH0,
        RBAMP_CMD_SET_CT_MODEL_CH1,
        RBAMP_CMD_SET_CT_MODEL_CH2,
    };
    static const uint32_t kSettlePerCh[3] = {
        RBAMP_SETTLE_MS_SET_CT_MODEL_CH0,
        RBAMP_SETTLE_MS_SET_CT_MODEL_CH1,
        RBAMP_SETTLE_MS_SET_CT_MODEL_CH2,
    };
    err = _write_cmd(dev, kCmdPerCh[channel]);
    if (err != ESP_OK) return err;
    _sleep_ms(kSettlePerCh[channel]);

    err = rbamp_save_gains(dev);
    if (err != ESP_OK) return err;

    /* Mark this channel as the new batch frontier — next call must be
     * channel < this. Reset via rbamp_set_sensor_class. */
    dev->ct_model_seq_last_ch = channel;
    dev->ct_model_seq_active = true;
    return ESP_OK;
}

esp_err_t rbamp_save_gains(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_cmd(dev, RBAMP_CMD_SAVE_GAINS);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_SETTLE_MS_SAVE_GAINS);  /* 700 ms flash erase + write */
    return ESP_OK;
}

esp_err_t rbamp_prepare_address_change(rbamp_handle_t dev, uint8_t new_addr)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (new_addr < 0x08 || new_addr > 0x77 || new_addr == dev->addr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mode = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_MODE, &mode);
    if (err != ESP_OK) return err;
    if (mode != 1) {
        ESP_LOGW(TAG, "address change refused: REG_MODE=%u (production)", mode);
        return ESP_ERR_INVALID_STATE;
    }
    dev->pending_addr = new_addr;
    dev->pending_armed_us = esp_timer_get_time();
    dev->addr_change_armed = true;
    return ESP_OK;
}

esp_err_t rbamp_commit_address_change(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->addr_change_armed) return ESP_ERR_INVALID_STATE;
    const int64_t elapsed_ms = (esp_timer_get_time() - dev->pending_armed_us) / 1000;
    if (elapsed_ms > 5000) {
        dev->addr_change_armed = false;
        ESP_LOGW(TAG, "address change arm expired (%lld ms)", elapsed_ms);
        return ESP_ERR_TIMEOUT;
    }

    /* Clear the arm flag eagerly so a partial-failure path never re-enters
     * commit with stale state. Re-arming requires a fresh prepare. */
    dev->addr_change_armed = false;
    const uint8_t new_addr = dev->pending_addr;

    esp_err_t err;
    err = _write_u8(dev, RBAMP_REG_I2C_ADDRESS, new_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "address change: REG_I2C_ADDRESS write failed (%s); device unchanged",
                 esp_err_to_name(err));
        return err;
    }
    err = _write_cmd(dev, RBAMP_CMD_SAVE_GAINS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "address change: SAVE_GAINS write failed (%s); device may be in inconsistent state",
                 esp_err_to_name(err));
        return err;
    }
    _sleep_ms(RBAMP_SETTLE_MS_SAVE_GAINS);
    err = _write_cmd(dev, RBAMP_CMD_RESET);
    if (err != ESP_OK) {
        /* SAVE_GAINS persisted the new address but RESET failed. The device
         * will adopt the new address on its next power cycle. Try to re-bind
         * at the new address pre-emptively. */
        ESP_LOGW(TAG, "address change: RESET write failed (%s) — re-binding at new address",
                 esp_err_to_name(err));
    } else {
        _sleep_ms(RBAMP_SETTLE_MS_RESET);
    }

    /* Re-bind the dev handle at the new address. */
    i2c_master_dev_handle_t old = dev->dev;
    i2c_master_dev_handle_t newdev = NULL;
    err = _add_device(dev->bus, new_addr, &newdev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "post-change add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    (void)i2c_master_bus_rm_device(old);
    dev->dev = newdev;
    dev->addr = new_addr;
    return ESP_OK;
}

esp_err_t rbamp_factory_reset(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_cmd(dev, RBAMP_CMD_FACTORY_RESET);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_SETTLE_MS_FACTORY_RESET);
    return ESP_OK;
}

esp_err_t rbamp_reset(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_cmd(dev, RBAMP_CMD_RESET);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_SETTLE_MS_RESET);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Broadcast LATCH (multi-module sync) — RESERVED FOR v2 FIRMWARE
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_broadcast_latch(i2c_master_bus_handle_t bus, uint32_t timeout_ms)
{
    (void)bus;
    (void)timeout_ms;
    /* I2C General-Call is DISABLED in v1 slave firmware (SPEC §9). The slave
     * does not respond to address 0x00 at all, so any general-call frame is
     * silently discarded. Reserved for v2 firmware.
     *
     * Multi-module workaround: issue per-device rbamp_latch_period() in a
     * tight loop, then one shared settle, then per-device
     * rbamp_read_period_snapshot with skip_latch=true. */
    ESP_LOGW(TAG, "broadcast_latch: feature reserved for v2 firmware (GC disabled)");
    return ESP_ERR_NOT_SUPPORTED;
}

/* -------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

const char *rbamp_err_to_str(esp_err_t err)
{
    /* Map common esp_err_t values onto rbamp-flavoured descriptions; for
     * codes we don't override, fall back to esp_err_to_name. */
    switch (err) {
        case ESP_OK:                    return "OK";
        case ESP_ERR_INVALID_ARG:       return "Bad parameter";
        case ESP_ERR_INVALID_RESPONSE:  return "Stale or unsupported response";
        case ESP_ERR_INVALID_STATE:     return "Wrong call sequence (check log: develop mode / sensor class UNSET / CT model ascending order)";
        case ESP_ERR_TIMEOUT:           return "Bus or arm-window timeout";
        case ESP_ERR_NO_MEM:            return "Out of memory";
        case ESP_ERR_NOT_SUPPORTED:     return "Feature reserved for v2 firmware";
        default:                        return esp_err_to_name(err);
    }
}

uint32_t rbamp_retry_exhaustion_count(rbamp_handle_t dev)
{
    return dev ? dev->retry_exhaustion_count : 0;
}

uint32_t rbamp_sanity_reject_count(rbamp_handle_t dev)
{
    return dev ? dev->sanity_reject_count : 0;
}
