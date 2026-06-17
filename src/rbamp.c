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
 *  - I2C General-Call broadcast LATCH is opt-in per-module via
 *    @c REG_FLEET_CONFIG bit 0 + @c CMD_SAVE_USER_CONFIG + @c CMD_RESET.
 *    Once enabled, ::rbamp_broadcast_latch transmits a 5-byte GC frame
 *    that latches all consenting modules atomically (truth-doc §5).
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

    /* v1.3 cached identity — populated by rbamp_begin via HW_VARIANT (0x55)
     * + CAPABILITY (0x57). variant==UNKNOWN until begin() runs. */
    rbamp_variant_t         variant;
    uint16_t                capability;

    /* Address-change two-step state. */
    uint8_t                 pending_addr;
    int64_t                 pending_armed_us;
    bool                    addr_change_armed;

    /* Energy dt anchor — MASTER wall-clock (esp_timer) of the previous
     * consumed period read. The chip's REG_V03_PERIOD_LATCH_MS (0xEC)
     * under-counts ~25-30% under SysTick starvation and is diagnostic-only
     * (E.6/F10), so billing energy integrates over the master's own reliable
     * clock interval between successive period reads, not the chip's dt. */
    int64_t                 last_latch_us;
    bool                    have_last_latch;

    /* Diagnostic counters. Cleared by rbamp_reset_counters. */
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
    /* Last-ditch: the IDF i2c_master peripheral can wedge on a bus error
     * (BERR / arbitration loss) that per-attempt retries don't clear — seen on
     * longer reads under multi-module bus load (standfw finding #3). Reset the
     * bus once and try a final time, mirroring the bench's retry+bus_reset. */
    if (i2c_master_bus_reset(dev->bus) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RBAMP_NACK_RETRY_GAP_MS));
        err = i2c_master_transmit_receive(dev->dev, &reg, 1, out, 1,
                                          CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
    }
    dev->retry_exhaustion_count++;
    return err;
}

/** @internal Single-byte write to @c reg, with NACK retry + bus recovery.
 *
 * Writes need the same SPEC §B.5 NACK discipline as reads: under multi-module
 * bus load a NACKed CMD/register write is otherwise silently lost (the device
 * never sees it, and a subsequent REG_ERROR read still shows the PRIOR op's
 * outcome — so the loss is invisible). This caused configure_channels to bind
 * only one of two channels on a 2-module bus (standfw finding #2). */
static esp_err_t _write_u8(rbamp_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        err = i2c_master_transmit(dev->dev, buf, 2, CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt + 1 < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_RBAMP_NACK_RETRY_GAP_MS));
        }
    }
    if (i2c_master_bus_reset(dev->bus) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RBAMP_NACK_RETRY_GAP_MS));
        err = i2c_master_transmit(dev->dev, buf, 2, CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
    }
    dev->retry_exhaustion_count++;
    return err;
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

/** @internal uint16 LE read — 2 separate single-byte reads per SPEC §6. */
static esp_err_t _read_u16_le(rbamp_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    for (uint8_t i = 0; i < 2; ++i) {
        esp_err_t err = _read_u8(dev, (uint8_t)(reg + i), &b[i]);
        if (err != ESP_OK) return err;
    }
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return ESP_OK;
}

/** @internal A/B double-read of a u8 — accept only on agreement.
 *
 * For correctness-critical values that the caller *decides* on (address,
 * GC_TICK, CT mirror) rather than *displays* (RMS telemetry): a torn read
 * here causes a false missed-frame / false bind-fail. Read twice; if they
 * agree return it; otherwise read a third time and accept a 2-of-3 majority;
 * if all three disagree, flag the sanity counter and fail. Single-byte +
 * per-byte NACK retry already runs underneath via _read_u8. */
static esp_err_t _read_u8_ab(rbamp_handle_t dev, uint8_t reg, uint8_t *out)
{
    uint8_t a, b;
    esp_err_t err = _read_u8(dev, reg, &a);
    if (err != ESP_OK) return err;
    err = _read_u8(dev, reg, &b);
    if (err != ESP_OK) return err;
    if (a == b) { *out = a; return ESP_OK; }
    uint8_t c;
    err = _read_u8(dev, reg, &c);
    if (err != ESP_OK) return err;
    if (c == a || c == b) { *out = c; return ESP_OK; }
    dev->sanity_reject_count++;
    return ESP_ERR_INVALID_RESPONSE;
}

/** @internal A/B double-read of a u16 LE — see _read_u8_ab. */
static esp_err_t _read_u16_ab(rbamp_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint16_t a, b;
    esp_err_t err = _read_u16_le(dev, reg, &a);
    if (err != ESP_OK) return err;
    err = _read_u16_le(dev, reg, &b);
    if (err != ESP_OK) return err;
    if (a == b) { *out = a; return ESP_OK; }
    uint16_t c;
    err = _read_u16_le(dev, reg, &c);
    if (err != ESP_OK) return err;
    if (c == a || c == b) { *out = c; return ESP_OK; }
    dev->sanity_reject_count++;
    return ESP_ERR_INVALID_RESPONSE;
}

/** @internal float32 LE read — 4 separate single-byte reads per SPEC §6.
 *
 * Per SPEC §B.5, the assembled float is sanity-checked against numerically
 * exotic patterns (NaN / Inf) and a per-quantity upper bound passed by the
 * caller (RBAMP_SANITY_LIMIT_V / _I / _P / _PF). The filter intentionally has
 * no physical lower bound — legitimate brownout (U≈80 V), mains disconnect
 * (U=0 V), and off-grid test scenarios pass through unmodified. */
static esp_err_t _read_float_le(rbamp_handle_t dev, uint8_t reg,
                                float *out, float max_abs)
{
    uint8_t b[4];
    for (uint8_t i = 0; i < 4; ++i) {
        esp_err_t err = _read_u8(dev, (uint8_t)(reg + i), &b[i]);
        if (err != ESP_OK) return err;
    }
    memcpy(out, b, 4);  /* portable bit-reinterpret; no strict-aliasing UB */
    if (!isfinite(*out) || fabsf(*out) > max_abs) {
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
    dev->variant = RBAMP_VARIANT_UNKNOWN;
    dev->capability = 0;
    dev->addr_change_armed = false;
    dev->have_last_latch = false;
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

/** @internal Map a HW_VARIANT wire byte to (channels, has_voltage). */
static void _apply_variant(rbamp_handle_t dev, uint8_t hw)
{
    switch (hw) {
        case RBAMP_VARIANT_UI1: dev->channels = 1; dev->has_voltage_hw = true;  break;
        case RBAMP_VARIANT_UI2: dev->channels = 2; dev->has_voltage_hw = true;  break;
        case RBAMP_VARIANT_UI3: dev->channels = 3; dev->has_voltage_hw = true;  break;
        case RBAMP_VARIANT_I1:  dev->channels = 1; dev->has_voltage_hw = false; break;
        case RBAMP_VARIANT_I2:  dev->channels = 2; dev->has_voltage_hw = false; break;
        case RBAMP_VARIANT_I3:  dev->channels = 3; dev->has_voltage_hw = false; break;
        default: return;  /* leave caller's fallback values */
    }
    dev->variant = (rbamp_variant_t)hw;
    dev->topology = (rbamp_topology_t)dev->channels;
}

/**
 * @internal
 * Variant detection (v1.3 B1) — authoritative via HW_VARIANT (0x55).
 *
 * v1.3 firmware exposes the build variant in 0x55 and a CAPABILITY bitmap in
 * 0x57. We read both: HW_VARIANT gives channel count + voltage presence
 * (I-variants are current-only), CAPABILITY drives feature gating. If 0x55
 * reads 0 (pre-v1.3 firmware that doesn't map it), we fall back to the
 * constructor-supplied topology plus a U_rms threshold voltage probe.
 */
static void _detect_variant(rbamp_handle_t dev)
{
    uint8_t hw = 0;
    if (_read_u8(dev, RBAMP_V2_REG_HW_VARIANT, &hw) == ESP_OK
            && hw >= RBAMP_VARIANT_UI1 && hw <= RBAMP_VARIANT_I3) {
        _apply_variant(dev, hw);
    } else {
        /* Fallback: keep constructor topology, probe voltage via U_rms. */
        float u_rms = 0.0f;
        dev->has_voltage_hw =
            (_read_float_le(dev, RBAMP_REG_V03_U_RMS, &u_rms, RBAMP_SANITY_LIMIT_V) == ESP_OK
             && u_rms > 1.0f);
    }

    uint16_t cap = 0;
    if (_read_u16_le(dev, RBAMP_V2_REG_CAPABILITY, &cap) == ESP_OK) {
        dev->capability = cap;
    }

    ESP_LOGI(TAG, "variant=%u channels=%u voltage=%s cap=0x%04X",
             (unsigned)dev->variant, dev->channels,
             dev->has_voltage_hw ? "yes" : "no", dev->capability);
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

    /* 1b. v1.3 B2 — a freshly flashed module boots with ERR_FLASH_PARAMS_BAD
     *     (0xFB) + EVENT bit3 because the params page is uninitialised. This
     *     is NOT fatal: factory defaults are loaded and one
     *     CMD_SAVE_USER_CONFIG (rbamp_save_user_config) repairs it. Surface a
     *     warning so provisioning flows know to SAVE once. */
    uint8_t boot_err = 0;
    if (_read_u8(dev, RBAMP_V2_REG_ERROR, &boot_err) == ESP_OK
            && boot_err == RBAMP_V2_DEV_ERR_FLASH_PARAMS_BAD) {
        ESP_LOGW(TAG, "fresh-flash: ERR_FLASH_PARAMS_BAD (factory defaults) — "
                      "call rbamp_save_user_config() once after first config");
    }

    /* 2. Variant detection — HW_VARIANT + CAPABILITY (B1). */
    _detect_variant(dev);

    /* 3. Primer LATCH — discard the first (partial) accumulator window and
     *    seed the master-clock energy anchor, so the first user
     *    rbamp_read_period_snapshot integrates over a full window measured by
     *    the master's reliable clock (not the chip's under-counting 0xEC). */
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
    return _read_float_le(dev, RBAMP_REG_V03_U_RMS, out, RBAMP_SANITY_LIMIT_V);
}

esp_err_t rbamp_read_voltage_peak(rbamp_handle_t dev, uint8_t phase, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (phase != 0) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_U_PEAK, out, RBAMP_SANITY_LIMIT_V);
}

esp_err_t rbamp_read_current(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_I0_RMS + ch * 4),
                          out, RBAMP_SANITY_LIMIT_I);
}

esp_err_t rbamp_read_current_peak(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_I0_PEAK + ch * 4),
                          out, RBAMP_SANITY_LIMIT_I);
}

esp_err_t rbamp_read_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_P0_REAL + ch * 4),
                          out, RBAMP_SANITY_LIMIT_P);
}

esp_err_t rbamp_read_power_factor(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_PF0 + ch * 4),
                          out, RBAMP_SANITY_LIMIT_PF);
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

/** @internal Fold one field read into the snapshot.
 *
 * A sanity-reject (@c ESP_ERR_INVALID_RESPONSE — the value was read but is
 * implausible, e.g. an uncalibrated voltage > the bound) sets the field to
 * NaN, flags @p bit in @p mask, and is NON-fatal so the rest of the snapshot
 * stays usable (standfw finding #3). A genuine transport failure (NACK after
 * retry + bus reset) IS fatal — it aborts read_all so a dead module surfaces
 * as ok=false in the fleet poll rather than as an all-NaN "healthy" snapshot. */
static esp_err_t _fold_field(esp_err_t err, float *field, uint8_t *mask, uint8_t bit)
{
    if (err == ESP_OK) return ESP_OK;
    if (err == ESP_ERR_INVALID_RESPONSE) {
        *field = NAN;
        *mask |= bit;
        return ESP_OK;
    }
    return err;  /* transport failure — propagate */
}

esp_err_t rbamp_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->topology = dev->topology;
    out->channels = dev->channels;
    out->has_voltage_hw = dev->has_voltage_hw;

    esp_err_t err;
    err = _fold_field(rbamp_read_voltage(dev, 0, &out->voltage),
                      &out->voltage, &out->implausible, RBAMP_FIELD_VOLTAGE);
    if (err != ESP_OK) return err;
    err = _fold_field(rbamp_read_voltage_peak(dev, 0, &out->voltage_peak),
                      &out->voltage_peak, &out->implausible, RBAMP_FIELD_VOLTAGE);
    if (err != ESP_OK) return err;
    for (uint8_t ch = 0; ch < dev->channels; ++ch) {
        err = _fold_field(rbamp_read_current(dev, ch, &out->current[ch]),
                          &out->current[ch], &out->implausible, RBAMP_FIELD_CURRENT);
        if (err != ESP_OK) return err;
        err = _fold_field(rbamp_read_current_peak(dev, ch, &out->current_peak[ch]),
                          &out->current_peak[ch], &out->implausible, RBAMP_FIELD_CURRENT);
        if (err != ESP_OK) return err;
        err = _fold_field(rbamp_read_power(dev, ch, &out->power[ch]),
                          &out->power[ch], &out->implausible, RBAMP_FIELD_POWER);
        if (err != ESP_OK) return err;
        err = _fold_field(rbamp_read_power_factor(dev, ch, &out->power_factor[ch]),
                          &out->power_factor[ch], &out->implausible, RBAMP_FIELD_PF);
        if (err != ESP_OK) return err;
    }
    return _fold_field(rbamp_read_frequency(dev, &out->frequency),
                       &out->frequency, &out->implausible, RBAMP_FIELD_FREQUENCY);
}

/* -------------------------------------------------------------------------
 * Period metering
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_latch_period(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    return _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
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
    return _read_float_le(dev, reg, out, RBAMP_SANITY_LIMIT_P);
}

esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_PERIOD_MAX_P_F0, out, RBAMP_SANITY_LIMIT_P);
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

    /* 1. Latch (skip if caller already issued rbamp_latch_period or
     *    rbamp_broadcast_latch in the batched multi-module pattern). */
    if (!skip_latch) {
        err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
        if (err != ESP_OK) return err;
    }

    /* 2. Capture the master-clock reference for this read's energy window and
     *    compute dt against the previous consumed read. The ESP32's esp_timer
     *    is not subject to the chip's SysTick starvation, so this is the
     *    billing-accurate window length (E.6/F10: the chip's 0xEC under-counts
     *    ~25-30% and is diagnostic-only). */
    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = 0;
    if (dev->have_last_latch && now_us > dev->last_latch_us) {
        dt_ms = (uint32_t)((now_us - dev->last_latch_us) / 1000);
    }

    /* 3. Settle. */
    _sleep_ms(settle_ms);

    /* 4. Verify the snapshot is fresh. On stale, do NOT integrate and do NOT
     *    advance the anchor — the chip preserves its accumulator across the
     *    stale window, so the next valid avg_p covers the full master interval
     *    since the last CONSUMED read. */
    bool valid = false;
    err = rbamp_is_period_valid(dev, &valid);
    if (err != ESP_OK) return err;
    if (!valid) {
        ESP_LOGW(TAG, "period STALE — skipping integration "
                      "(accumulator rolls into the next cycle)");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 5. Read avg_p per channel + max_p. latch_ms (0xEC) is read as a
     *    DIAGNOSTIC value only (chip's view of dt); it is NOT used for energy. */
    for (uint8_t ch = 0; ch < dev->channels; ++ch) {
        err = rbamp_read_period_avg_power(dev, ch, &out->avg_p[ch]);
        if (err != ESP_OK) return err;
    }
    err = rbamp_read_period_max_power(dev, &out->max_p);
    if (err != ESP_OK) return err;
    err = rbamp_read_period_latch_ms(dev, &out->latch_ms);
    if (err != ESP_OK) return err;
    out->master_dt_ms = dt_ms;   /* master wall-clock window — used for energy */
    out->valid = true;

    /* 6. Integrate Wh using the MASTER dt, then advance the anchor. */
#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_tick(&dev->energy, out->avg_p, dev->channels,
                      dt_ms, out->valid);
#endif
    dev->last_latch_us = now_us;
    dev->have_last_latch = true;

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
 * Synchronous write-verify (standfw harness finding #1).
 *
 * Read REG_ERROR (0x02) IMMEDIATELY after our own validating write/command,
 * before any other data transaction. In that narrow window REG_ERROR is the
 * synchronous truth (last-write-outcome): an invalid enum / rejected command
 * shows @c ERR_PARAM right away, whereas the durable EVENT bit3 lags ~200 ms
 * for register-write rejections (it only settles on the device's periodic
 * event eval). Setters call this so the caller gets immediate config feedback;
 * @c rbamp_has_error / bit3 remain the durable async-fault channel.
 *
 * Must be the FIRST transaction after the validating write — a SAVE_GAINS or
 * any other data write in between would overwrite REG_ERROR with its own
 * outcome. (Setting the read pointer for this read does not clobber it.)
 */
static esp_err_t _check_write_error(rbamp_handle_t dev)
{
    uint8_t e = 0;
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_ERROR, &e);
    if (err != ESP_OK) return err;
    if (e == RBAMP_V2_DEV_ERR_OK) return ESP_OK;
    if (e == RBAMP_V2_DEV_ERR_PARAM) {
        ESP_LOGW(TAG, "device rejected config write: ERR_PARAM (0x%02X)", e);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "device rejected config write: REG_ERROR=0x%02X", e);
    return ESP_ERR_INVALID_STATE;
}

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

/** @internal Is @p code a characterised CT preset for sensor class @p cls?
 *
 * Per-class accepted set (SPEC registers_v2.yaml 0x05 / firmware
 * CT_PRESET_TABLE — NOT a contiguous range):
 *   SCT_013     {1,2,3,4,6}  (5=-100, 7=-060 reserved-uncharacterised)
 *   WIRED_CT    {1,2,3}
 *   BUILTIN_CT  {}           (per-unit factory cal — no presets)
 * Client-side fast-fail; the firmware also rejects an unaccepted code with
 * ERR_PARAM (caught by _check_write_error). */
static bool _ct_model_valid(uint8_t cls, uint8_t code)
{
    switch (cls) {
        case RBAMP_SENSOR_SCT013:
            return code == 1 || code == 2 || code == 3 || code == 4 || code == 6;
        case RBAMP_SENSOR_WIRED_CT:
            return code == 1 || code == 2 || code == 3;
        case RBAMP_SENSOR_BUILTIN_CT:
        default:
            return false;
    }
}

esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev, rbamp_sensor_class_t cls)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if ((uint8_t)cls > (uint8_t)RBAMP_SENSOR_BUILTIN_CT) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_u8(dev, RBAMP_REG_SENSOR_CLASS, (uint8_t)cls);
    if (err != ESP_OK) return err;
    /* Synchronous verify before SAVE_GAINS — a rejected enum is caught here
     * (immediate), not via the ~200ms-lagged EVENT bit3, and we skip the
     * flash write entirely on rejection. */
    err = _check_write_error(dev);
    if (err != ESP_OK) return err;
    return rbamp_save_gains(dev);
}

esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (code < 1 || code > 7) return ESP_ERR_INVALID_ARG;  /* SKU namespace; per-class set checked below / device-side */
    esp_err_t err = _check_sensor_class_set(dev);
    if (err != ESP_OK) return err;
    /* v1.3 A1: REG_CT_MODEL (0x05) is PURE STAGING — a bare write no longer
     * applies the preset. Binding happens exclusively via CMD_SET_CT_MODEL_CHn.
     * The single-arg form targets channel 0 (the only channel on UI1/I1). */
    err = _write_u8(dev, RBAMP_REG_CT_MODEL, code);
    if (err != ESP_OK) return err;
    err = _write_cmd(dev, RBAMP_V2_CMD_SET_CT_MODEL_CH0);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_V2_SETTLE_MS_SET_CT_MODEL_CH0);
    /* The CMD validates the (class, model) preset — verify before SAVE. */
    err = _check_write_error(dev);
    if (err != ESP_OK) return err;
    return rbamp_save_gains(dev);
}

/**
 * @internal
 * Stage + bind one channel's CT model WITHOUT the flash SAVE_GAINS. Used by
 * the public single-channel setter (which appends one save) and by
 * rbamp_configure_channels (which batches binds behind a single terminal save
 * to spare flash endurance). The per-channel CMD is synchronously verified via
 * _check_write_error before returning, so a rejected (class, model) preset is
 * caught here without persisting anything.
 */
static esp_err_t _bind_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    /* v1.2+ requirement — CMD_SET_CT_MODEL_CH* does not exist on older fw. */
    uint8_t version = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &version);
    if (err != ESP_OK) return err;
    if (version < 0x03) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: firmware 0x%02X < v1.2", version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Sensor-class precondition (same as the single-arg form). */
    uint8_t cls = 0;
    err = _read_u8(dev, RBAMP_REG_SENSOR_CLASS, &cls);
    if (err != ESP_OK) return err;
    if (cls == (uint8_t)RBAMP_SENSOR_UNSET) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: sensor class is UNSET "
                      "(call rbamp_set_sensor_class first)");
        return ESP_ERR_INVALID_STATE;
    }
    /* Per-class accepted-set validation (fast-fail; firmware also rejects). */
    if (!_ct_model_valid(cls, code)) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: code %u not a preset for class %u",
                 code, cls);
        return ESP_ERR_INVALID_ARG;
    }

    /* v1.3 A1: REG_CT_MODEL (0x05) is pure staging — write the preset, then
     * issue the per-channel CMD which validates the (class, model) pair and
     * binds it to the requested channel only (no ch0 clobber → order-
     * independent). */
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
    /* The CMD validates the (class, model) preset — verify before SAVE. */
    return _check_write_error(dev);
}

esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (channel > 2) return ESP_ERR_INVALID_ARG;
    if (code < 1 || code > 7) return ESP_ERR_INVALID_ARG;  /* SKU namespace; per-class set checked below / device-side */
    esp_err_t err = _bind_ct_model_ch(dev, channel, code);
    if (err != ESP_OK) return err;
    return rbamp_save_gains(dev);
}

esp_err_t rbamp_read_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (channel > 2) return ESP_ERR_INVALID_ARG;
    /* Applied-model mirrors are contiguous: CH0=0x51, CH1=0x52, CH2=0x53.
     * A/B double-read — a torn mirror would read as a false bind-fail. */
    return _read_u8_ab(dev, (uint8_t)(RBAMP_V2_REG_CT_MODEL_CH0 + channel), out);
}

/** @internal Post-bind verify of one channel's applied-model mirror.
 *
 * Distinguishes the three outcomes a verify can have under a busy multi-module
 * bus (standfw finding at 7283e13 — a torn mirror read must NOT read as a
 * bind-fail):
 *   - @c ESP_OK                    mirror == expected (confirmed)
 *   - @c ESP_ERR_INVALID_STATE     mirror is a STABLE WRONG value (genuine miss)
 *   - @c ESP_ERR_INVALID_RESPONSE  mirror could not be read cleanly (A/B tore)
 * A torn read retries the READ (cheap) rather than concluding failure. */
static esp_err_t _verify_ct_bind(rbamp_handle_t dev, uint8_t ch, uint8_t expected)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t applied = 0;
        esp_err_t err = rbamp_read_ct_model_ch(dev, ch, &applied);
        if (err == ESP_OK) {
            return (applied == expected) ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        /* A/B torn — retry the read, do not conclude the bind failed. */
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t rbamp_configure_channels(rbamp_handle_t dev, rbamp_sensor_class_t cls,
                                   const uint8_t *models, uint8_t n)
{
    if (!dev || !models) return ESP_ERR_INVALID_ARG;
    esp_err_t err = rbamp_set_sensor_class(dev, cls);
    if (err != ESP_OK) return err;
    uint8_t nch = dev->channels;
    if (n > nch) n = nch;  /* variant-aware clamp */
    bool any_bound = false;
    /* Order-independent (v1.3 A1 / Fix A): REG_CT_MODEL is pure staging and the
     * per-channel CMD binds only its own channel, so binding in natural order
     * is correct. (The pre-Fix-A ch0 auto-apply that made order matter is gone;
     * HW-confirmed ascending {1,3,6} → [1,3,6] on Fix-A firmware.) */
    for (uint8_t ch = 0; ch < n; ++ch) {
        if (models[ch] == 0) continue;  /* skip unconfigured channel */
        /* Bind WITHOUT a per-channel SAVE — batched behind one terminal save
         * below to spare flash endurance (was 1 + n flash cycles per call). */
        err = _bind_ct_model_ch(dev, ch, models[ch]);
        if (err != ESP_OK) return err;
        any_bound = true;

        /* Post-bind verify (standfw finding #2) — the CT_MODEL_CHn mirror
         * reflects the RAM-applied preset, so this works before the save. Only
         * a STABLE WRONG mirror value is a genuine half-apply → re-bind once,
         * then hard-fail. A mirror that merely won't read cleanly (torn under
         * bus load) is NOT a failure: trust the accepted+verified CMD. */
        esp_err_t v = _verify_ct_bind(dev, ch, models[ch]);
        if (v == ESP_ERR_INVALID_STATE) {
            err = _bind_ct_model_ch(dev, ch, models[ch]);  /* re-bind once */
            if (err != ESP_OK) return err;
            v = _verify_ct_bind(dev, ch, models[ch]);
            if (v == ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "configure_channels: ch%u did not bind (want %u)",
                         ch, models[ch]);
                return ESP_ERR_INVALID_STATE;
            }
        }
        if (v == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "configure_channels: ch%u bind not mirror-confirmed "
                          "(transient read) — trusting accepted CMD", ch);
        }
    }
    /* Single terminal flash persist for all channels bound this call. */
    return any_bound ? rbamp_save_gains(dev) : ESP_OK;
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
    /* v1.3 A2 / truth-doc §6.1: the two-phase address commit (magic-armed
     * CMD_COMMIT_ADDR) is production-OK — field-swap of a production spare is
     * supported, no develop-mode gate. (Was develop-gated under the v1.2
     * assumption; corrected for v1.3.) */
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

    /* v1.3 A2 two-phase commit (truth-doc §6.1):
     *   1. write candidate → REG_I2C_ADDRESS (0x30)   [RAM-staged]
     *   2. write 0xA5 → REG_ADDR_COMMIT_MAGIC (0x31)   [arm]
     *   3. CMD_COMMIT_ADDR (0x30)                       [persist to flash]
     *   4. CMD_RESET                                    [new addr active]
     * This replaces the old SAVE_GAINS path (the address is excluded from the
     * SAVE_GAINS namespace in v1.3). CMD_COMMIT_ADDR is production-OK. */
    esp_err_t err;
    err = _write_u8(dev, RBAMP_REG_I2C_ADDRESS, new_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "address change: REG_I2C_ADDRESS write failed (%s); device unchanged",
                 esp_err_to_name(err));
        return err;
    }
    err = _write_u8(dev, RBAMP_V2_REG_ADDR_COMMIT_MAGIC, 0xA5);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "address change: ADDR_COMMIT_MAGIC arm failed (%s); device unchanged",
                 esp_err_to_name(err));
        return err;
    }
    err = _write_cmd(dev, RBAMP_V2_CMD_COMMIT_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "address change: CMD_COMMIT_ADDR write failed (%s); device may be inconsistent",
                 esp_err_to_name(err));
        return err;
    }
    _sleep_ms(RBAMP_V2_SETTLE_MS_COMMIT_ADDR);
    err = _write_cmd(dev, RBAMP_CMD_RESET);
    if (err != ESP_OK) {
        /* CMD_COMMIT_ADDR persisted the new address but RESET failed. The
         * device will adopt the new address on its next power cycle. Try to
         * re-bind at the new address pre-emptively. */
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
 * Broadcast LATCH (multi-module sync)
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_broadcast_latch_group(i2c_master_bus_handle_t bus,
                                      uint8_t group, uint16_t tick,
                                      uint32_t timeout_ms)
{
    if (!bus) return ESP_ERR_INVALID_ARG;
    /* 5-byte GC frame per truth-doc §5.4: [A5][27][group][tick_lo][tick_hi].
     * On the wire this is preceded by the GC address byte 0x00, giving the
     * 6-byte sequence `00 A5 27 grp tl th`. Only GC-enabled modules whose
     * GROUP_ID matches group (or group==0) latch; each stores tick in
     * REG_GC_TICK (0x59). Modules without FLEET_CONFIG.bit0 silently ignore. */
    const uint8_t frame[5] = {
        0xA5, 0x27, group,
        (uint8_t)(tick & 0xFF), (uint8_t)(tick >> 8),
    };

    i2c_master_dev_handle_t gc_dev = NULL;
    const i2c_device_config_t gc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x00,
        .scl_speed_hz    = CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &gc_cfg, &gc_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "broadcast_latch: add temp GC device failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = i2c_master_transmit(gc_dev, frame, sizeof(frame), (int)timeout_ms);
    /* Always remove the temp device, even on transmit failure. */
    (void)i2c_master_bus_rm_device(gc_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "broadcast_latch: transmit failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t rbamp_broadcast_latch(i2c_master_bus_handle_t bus, uint32_t timeout_ms)
{
    return rbamp_broadcast_latch_group(bus, 0x00, 0x0000, timeout_ms);
}

/* -------------------------------------------------------------------------
 * v1.3 identity / capability / error / fleet helpers
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_read_variant(rbamp_handle_t dev, rbamp_variant_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t hw = 0;
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_HW_VARIANT, &hw);
    if (err != ESP_OK) return err;
    *out = (hw >= RBAMP_VARIANT_UI1 && hw <= RBAMP_VARIANT_I3)
         ? (rbamp_variant_t)hw : RBAMP_VARIANT_UNKNOWN;
    return ESP_OK;
}

esp_err_t rbamp_read_capability(rbamp_handle_t dev, uint16_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u16_le(dev, RBAMP_V2_REG_CAPABILITY, out);
}

esp_err_t rbamp_read_product_id(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u8(dev, RBAMP_V2_REG_PRODUCT_ID, out);
}

esp_err_t rbamp_read_uid(rbamp_handle_t dev, uint8_t out[12])
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < 12; ++i) {
        esp_err_t err = _read_u8(dev, (uint8_t)(RBAMP_V2_REG_UID + i), &out[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

bool rbamp_has_voltage(rbamp_handle_t dev)
{
    return dev ? dev->has_voltage_hw : false;
}

esp_err_t rbamp_read_last_error(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u8(dev, RBAMP_V2_REG_ERROR, out);
}

esp_err_t rbamp_read_event_flags(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, out);
}

esp_err_t rbamp_clear_event_flags(rbamp_handle_t dev, uint8_t mask)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* Write-1-to-clear: writing the mask back clears exactly those bits. */
    return _write_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, mask);
}

esp_err_t rbamp_has_error(rbamp_handle_t dev, bool *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t flags = 0;
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, &flags);
    if (err != ESP_OK) return err;
    *out = (flags & RBAMP_V2_EVENT_ERROR) != 0;
    return ESP_OK;
}

esp_err_t rbamp_clear_error(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_cmd(dev, RBAMP_V2_CMD_CLEAR_ERROR);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_V2_SETTLE_MS_CLEAR_ERROR);
    return ESP_OK;
}

esp_err_t rbamp_save_user_config(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _write_cmd(dev, RBAMP_V2_CMD_SAVE_USER_CONFIG);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_V2_SETTLE_MS_SAVE_USER_CONFIG);  /* 700 ms flash */
    return ESP_OK;
}

esp_err_t rbamp_is_provisioned(rbamp_handle_t dev, bool *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t e = 0;
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_ERROR, &e);
    if (err != ESP_OK) return err;
    *out = (e != RBAMP_V2_DEV_ERR_FLASH_PARAMS_BAD);
    return ESP_OK;
}

esp_err_t rbamp_read_active_address(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    /* A/B double-read: a torn address read would cause a false provision-fail. */
    return _read_u8_ab(dev, RBAMP_REG_I2C_ADDRESS, out);
}

esp_err_t rbamp_enable_gc(rbamp_handle_t dev, bool enable)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    uint8_t cfg = 0;
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_FLEET_CONFIG, &cfg);
    if (err != ESP_OK) return err;
    cfg = enable ? (uint8_t)(cfg | 0x01) : (uint8_t)(cfg & ~0x01);
    err = _write_u8(dev, RBAMP_V2_REG_FLEET_CONFIG, cfg);
    if (err != ESP_OK) return err;
    /* GC ISR is wired at boot — persist + reset for the change to take effect. */
    err = rbamp_save_user_config(dev);
    if (err != ESP_OK) return err;
    return rbamp_reset(dev);
}

esp_err_t rbamp_read_fleet_config(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u8(dev, RBAMP_V2_REG_FLEET_CONFIG, out);
}

esp_err_t rbamp_set_group_id(rbamp_handle_t dev, uint8_t group)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    return _write_u8(dev, RBAMP_V2_REG_GROUP_ID, group);
}

esp_err_t rbamp_read_group_id(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    return _read_u8(dev, RBAMP_V2_REG_GROUP_ID, out);
}

esp_err_t rbamp_read_gc_tick(rbamp_handle_t dev, uint16_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    /* A/B double-read: a torn tick reads as a false missed-frame in fleet-sync. */
    return _read_u16_ab(dev, RBAMP_V2_REG_GC_TICK, out);
}

esp_err_t rbamp_read_label(rbamp_handle_t dev, char out[9])
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < 8; ++i) {
        esp_err_t err = _read_u8(dev, (uint8_t)(RBAMP_V2_REG_LABEL + i),
                                 (uint8_t *)&out[i]);
        if (err != ESP_OK) return err;
    }
    out[8] = '\0';
    return ESP_OK;
}

esp_err_t rbamp_write_label(rbamp_handle_t dev, const char *label)
{
    if (!dev || !label) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < 8; ++i) {
        /* ASCII zero-pad past the NUL terminator. */
        char c = label[i];
        esp_err_t err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_LABEL + i),
                                  (uint8_t)c);
        if (err != ESP_OK) return err;
        if (c == '\0') {
            /* zero-fill the remaining bytes */
            for (uint8_t j = (uint8_t)(i + 1); j < 8; ++j) {
                err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_LABEL + j), 0x00);
                if (err != ESP_OK) return err;
            }
            break;
        }
    }
    return ESP_OK;
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
        case ESP_ERR_INVALID_RESPONSE:  return "Stale or out-of-range response (period not ready / sanity reject)";
        case ESP_ERR_INVALID_STATE:     return "Wrong call sequence (sensor class UNSET / address-commit not armed / bus conflict)";
        case ESP_ERR_TIMEOUT:           return "Bus or arm-window timeout";
        case ESP_ERR_NO_MEM:            return "Out of memory";
        case ESP_ERR_NOT_SUPPORTED:     return "Feature not available on current firmware version";
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

void rbamp_reset_counters(rbamp_handle_t dev)
{
    if (!dev) return;
    dev->retry_exhaustion_count = 0;
    dev->sanity_reject_count = 0;
}
