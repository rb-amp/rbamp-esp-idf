/**
 * @file    rbamp.h
 * @brief   ESP-IDF client component for the rbAmp I²C sensor / dimmer module —
 *          public API.
 * @author  rbAmp team
 * @date    2026
 * @version 1.0.0
 *
 * @details
 * @par Overview
 * This component wraps the rbAmp wire-level I2C protocol defined in
 * @c libs/spec/SPEC.md (the cross-platform "single source of truth"). It
 * uses ESP-IDF's @b new @c i2c_master driver (IDF v5.2+). Older IDF
 * users should target the Arduino-as-component path with the rbAmp
 * Arduino library instead.
 *
 * @par Quick start
 * @code
 * i2c_master_bus_config_t bus_cfg = {
 *     .i2c_port  = I2C_NUM_0,
 *     .sda_io_num = GPIO_NUM_21,
 *     .scl_io_num = GPIO_NUM_22,
 *     .clk_source = I2C_CLK_SRC_DEFAULT,
 *     .glitch_ignore_cnt = 7,
 *     .flags.enable_internal_pullup = true,
 * };
 * i2c_master_bus_handle_t bus;
 * ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
 *
 * rbamp_handle_t dev;
 * ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));
 * ESP_ERROR_CHECK(rbamp_begin(dev));
 *
 * float p;
 * ESP_ERROR_CHECK(rbamp_read_power(dev, 0, &p));
 * ESP_LOGI("app", "ch0 power = %.1f W", p);
 *
 * rbamp_period_snapshot_t snap;
 * if (rbamp_read_period_snapshot(dev, &snap, 50) == ESP_OK) {
 *     ESP_LOGI("app", "Wh ch0 = %.4f", rbamp_energy_wh(dev, 0));
 * }
 *
 * rbamp_del(dev);
 * @endcode
 *
 * @par Protocol invariants enforced
 * - One I2C address phase per byte — no auto-increment (SPEC §6).
 * - 50 ms settle after @c CMD_LATCH_PERIOD before reading the period block.
 * - 700 ms settle after @c CMD_SAVE_GAINS.
 * - @c REG_V03_PERIOD_VALID checked before consuming the snapshot.
 * - Two-step API for I2C address changes (prepare + commit within 5 s).
 *
 * @see libs/spec/SPEC.md
 * @see libs/spec/registers.yaml
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#include "rbamp_registers.h"  /* auto-generated; do not edit */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Topology enumeration
 * ------------------------------------------------------------------------- */

/**
 * @brief Variant topology — populated by ::rbamp_begin via NACK probe.
 *
 * Reflects how many independent current channels the device exposes.
 * Voltage hardware presence is a separate flag — see ::rbamp_has_voltage_hw.
 */
typedef enum {
    RBAMP_TOPOLOGY_SINGLE      = 1, /**< 1 current channel (UI1 / I1). */
    RBAMP_TOPOLOGY_SPLIT_PHASE = 2, /**< 2 current channels (UI2 / I2). */
    RBAMP_TOPOLOGY_THREE_PHASE = 3, /**< 3 current channels (UI3 / I3). */
} rbamp_topology_t;

/**
 * @brief Sensor class — wire-byte values for @c REG_SENSOR_CLASS (0x25).
 *
 * Set via ::rbamp_set_sensor_class before the first ::rbamp_set_ct_model* call.
 * Underlying type is @c uint8_t (cast yields the wire encoding directly).
 *
 * On v1.2+ firmware @c RBAMP_SENSOR_UNSET makes ::rbamp_set_ct_model and
 * ::rbamp_set_ct_model_ch refuse with @c ESP_ERR_INVALID_STATE. On v1.0 / v1.1
 * firmware the register has no functional effect and the guard is skipped.
 *
 * @c RBAMP_SENSOR_WIRED_CT and @c RBAMP_SENSOR_BUILTIN_CT are reserved for
 * future sensor-class SKUs (STANDARD / PRO tiers) and currently behave the
 * same as @c RBAMP_SENSOR_SCT013 device-side.
 */
typedef enum {
    RBAMP_SENSOR_UNSET       = 0, /**< Default after factory reset. */
    RBAMP_SENSOR_SCT013      = 1, /**< SCT-013 current transformer (shipping default). */
    RBAMP_SENSOR_WIRED_CT    = 2, /**< Reserved — wired CT class (STANDARD tier). */
    RBAMP_SENSOR_BUILTIN_CT  = 3, /**< Reserved — built-in CT class (PRO tier). */
} rbamp_sensor_class_t;

/* -------------------------------------------------------------------------
 * Snapshot structures
 * ------------------------------------------------------------------------- */

/**
 * @brief Real-time metering snapshot — output of ::rbamp_read_all.
 *
 * All fields are SI units. Refreshed every 200 ms by the device firmware.
 * Unused channels (per ::rbamp_channels) are zeroed.
 */
typedef struct {
    float voltage;          /**< REG_V03_U_RMS, V. */
    float voltage_peak;     /**< REG_V03_U_PEAK, V. */
    float current[3];       /**< RMS current per channel, A. */
    float current_peak[3];  /**< Peak current per channel, A. */
    float power[3];         /**< Real power per channel, W (signed). */
    float power_factor[3];  /**< Power factor per channel (-1..+1). */
    float frequency;        /**< REG_AC_FREQ, Hz. */
    rbamp_topology_t topology; /**< Variant. */
    uint8_t channels;       /**< 1..3. */
    bool has_voltage_hw;
} rbamp_snapshot_t;

/**
 * @brief Period-metering snapshot — output of ::rbamp_read_period_snapshot.
 *
 * Master tracks wall-clock @c master_dt_ms between successful latches and
 * integrates Wh from @c avg_p (see SPEC §7).
 */
typedef struct {
    float avg_p[3];      /**< Average real power per channel over the latched period, W. */
    float max_p;         /**< Peak real power on channel 0 during the period, W. */
    uint32_t latch_ms;   /**< Device-reported period duration (ms, diagnostic only). */
    uint32_t master_dt_ms; /**< Master's wall-clock dt since previous successful latch, ms. */
    bool valid;          /**< true if REG_V03_PERIOD_VALID bit0 == 1 at read time. */
} rbamp_period_snapshot_t;

/* -------------------------------------------------------------------------
 * Opaque handle
 * ------------------------------------------------------------------------- */

/** @brief Opaque handle for one rbAmp slave device. Created by ::rbamp_new. */
typedef struct rbamp_obj_t *rbamp_handle_t;

/**
 * @defgroup rbamp_settle_ms Settle-time constants (ms)
 * @brief Canonical command-settle durations from SPEC.
 *
 * These constants encode the wait-after-write time required by the device
 * firmware after each blocking command. The library's public wrappers
 * (::rbamp_save_gains, ::rbamp_factory_reset, ::rbamp_reset,
 * ::rbamp_latch_period, ::rbamp_set_ct_model_ch) already include the right
 * settle delay internally — most users never need these constants directly.
 *
 * Use them when writing raw-API code that bypasses the library wrappers
 * (e.g. issuing a `CMD_*` opcode manually via your own I²C transport) so
 * the timing constants stay in sync with the SPEC across firmware revisions.
 * They are defined in @c rbamp_registers.h (auto-generated from SPEC) and
 * promoted to public surface here.
 * @{
 */

/** @brief Settle ms after @c CMD_LATCH_PERIOD before reading the period block. */
/* RBAMP_SETTLE_MS_LATCH_PERIOD — defined in rbamp_registers.h, value 50. */

/** @brief Settle ms after @c CMD_RESET. */
/* RBAMP_SETTLE_MS_RESET — defined in rbamp_registers.h, value 100. */

/** @brief Settle ms after @c CMD_SAVE_GAINS (flash erase + write). */
/* RBAMP_SETTLE_MS_SAVE_GAINS — defined in rbamp_registers.h, value 700. */

/** @brief Settle ms after @c CMD_FACTORY_RESET. Bus unavailable during reset. */
/* RBAMP_SETTLE_MS_FACTORY_RESET — defined in rbamp_registers.h, value 1500. */

/** @brief Settle ms after @c CMD_SET_CT_MODEL_CH0 (in-RAM preset-table lookup). */
/* RBAMP_SETTLE_MS_SET_CT_MODEL_CH0 — defined in rbamp_registers.h, value 5. */

/** @brief Settle ms after @c CMD_SET_CT_MODEL_CH1. */
/* RBAMP_SETTLE_MS_SET_CT_MODEL_CH1 — defined in rbamp_registers.h, value 5. */

/** @brief Settle ms after @c CMD_SET_CT_MODEL_CH2. */
/* RBAMP_SETTLE_MS_SET_CT_MODEL_CH2 — defined in rbamp_registers.h, value 5. */

/** @} */ /* end rbamp_settle_ms */

/* -------------------------------------------------------------------------
 * Lifecycle (SPEC §12)
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate and bind a new rbAmp device handle to a bus + address.
 *
 * Topology defaults to ::RBAMP_TOPOLOGY_THREE_PHASE (see SPEC §8 for why
 * over-detection is harmless on v1 firmware). Use ::rbamp_new_with_topology
 * to pin a specific topology.
 *
 * @param[in]  bus       I2C master bus from @c i2c_new_master_bus.
 * @param[in]  addr      7-bit slave address (0x08..0x77).
 * @param[out] out       Receives the new handle on success.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on bad @c addr or null @c out
 *   - @c ESP_ERR_NO_MEM on allocation failure
 */
esp_err_t rbamp_new(i2c_master_bus_handle_t bus, uint8_t addr, rbamp_handle_t *out);

/**
 * @brief Allocate and bind a new rbAmp device handle with a user-supplied topology.
 *
 * Use this when you know the device variant ahead of time (e.g. UI1, I3) and
 * want to suppress the THREE_PHASE default. See SPEC §8 — v1 firmware does
 * not expose a reliable in-band channel-count signal, so the topology is
 * pinned by the caller until v1.1.
 *
 * @param[in]  bus      I2C master bus from @c i2c_new_master_bus.
 * @param[in]  addr     7-bit slave address (0x08..0x77).
 * @param[in]  topology One of ::RBAMP_TOPOLOGY_SINGLE / SPLIT_PHASE / THREE_PHASE.
 * @param[out] out      Receives the new handle on success.
 */
esp_err_t rbamp_new_with_topology(i2c_master_bus_handle_t bus, uint8_t addr,
                                  rbamp_topology_t topology, rbamp_handle_t *out);

/**
 * @brief Free a handle previously returned by ::rbamp_new.
 *
 * @param[in] dev Handle to release. Passing NULL is a no-op.
 */
void rbamp_del(rbamp_handle_t dev);

/**
 * @brief Probe the device, detect voltage hardware, run primer LATCH.
 *
 * Performs:
 * 1. @c REG_VERSION read — fails if device does not ACK or reports 0x00/0xFF.
 * 2. Voltage-hardware probe via U_rms threshold (>1.0 V → present). Topology
 *    is NOT auto-detected on v1 firmware; it stays at the value set in
 *    ::rbamp_new / ::rbamp_new_with_topology (SPEC §8).
 * 3. @c CMD_LATCH_PERIOD primer write + 50 ms settle.
 * 4. Records master_t_last for subsequent energy integration.
 *
 * Idempotent (but NOT free) — each call issues one primer LATCH.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_RESPONSE if firmware version is 0 or 0xFF
 *   - @c ESP_ERR_TIMEOUT or @c ESP_FAIL on I2C transport failure
 */
esp_err_t rbamp_begin(rbamp_handle_t dev);

/**
 * @brief Lightweight alive check — one byte read of REG_VERSION.
 *
 * @return @c ESP_OK if the slave ACKs and reports a supported firmware version.
 */
esp_err_t rbamp_probe(rbamp_handle_t dev);

/**
 * @brief Poll @c REG_STATUS (0x00) bit 0 until the device reports READY.
 *
 * Sticky / non-destructive — safe to poll. Do NOT use @c REG_V03_STATUS
 * (0xCE) for this purpose — that register is "cleared on read" and races
 * the firmware's commit thread.
 *
 * @param[in] dev         Handle.
 * @param[in] timeout_ms  Maximum wait in ms.
 * @return @c ESP_OK or @c ESP_ERR_TIMEOUT.
 */
esp_err_t rbamp_wait_ready(rbamp_handle_t dev, uint32_t timeout_ms);

/** @return Firmware version byte (REG_VERSION), or 0 on failure. */
uint8_t rbamp_firmware_version(rbamp_handle_t dev);

/** @return Topology — caller-pinned via ::rbamp_new_with_topology, or default THREE_PHASE. */
rbamp_topology_t rbamp_topology(rbamp_handle_t dev);

/** @return Number of valid current channels (1..3). */
uint8_t rbamp_channels(rbamp_handle_t dev);

/** @return true if voltage sensing hardware was detected. */
bool rbamp_has_voltage_hw(rbamp_handle_t dev);

/** @return Current 7-bit slave address (updates after ::rbamp_commit_address_change). */
uint8_t rbamp_address(rbamp_handle_t dev);

/* -------------------------------------------------------------------------
 * Real-time reads (SPEC §12 — RT block, 200 ms refresh on device)
 * ------------------------------------------------------------------------- */

/** @brief Read RMS voltage (REG_V03_U_RMS, 0x86) in V. */
esp_err_t rbamp_read_voltage(rbamp_handle_t dev, uint8_t phase, float *out);

/** @brief Read peak voltage (REG_V03_U_PEAK, 0x8A) in V. */
esp_err_t rbamp_read_voltage_peak(rbamp_handle_t dev, uint8_t phase, float *out);

/** @brief Read RMS current for one channel (REG_V03_I*_RMS) in A. */
esp_err_t rbamp_read_current(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read peak current for one channel (REG_V03_I*_PEAK) in A. */
esp_err_t rbamp_read_current_peak(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read real power for one channel (REG_V03_P*_REAL) in W (signed). */
esp_err_t rbamp_read_power(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read power factor for one channel (REG_V03_PF*) in -1..+1. */
esp_err_t rbamp_read_power_factor(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read mains frequency (REG_AC_FREQ, 0x20) in Hz. */
esp_err_t rbamp_read_frequency(rbamp_handle_t dev, float *out);

/**
 * @brief One-shot read of the full RT block into a snapshot struct.
 *
 * Unused channels (per ::rbamp_channels) are filled with 0.
 *
 * @param[in]  dev Handle.
 * @param[out] out Snapshot to populate.
 * @return @c ESP_OK on success, or the first underlying error.
 */
esp_err_t rbamp_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out);

/* -------------------------------------------------------------------------
 * Period metering (SPEC §7)
 * ------------------------------------------------------------------------- */

/** @brief Issue @c CMD_LATCH_PERIOD (write 0x27 to REG_COMMAND). Does not wait. */
esp_err_t rbamp_latch_period(rbamp_handle_t dev);

/** @brief Read @c REG_V03_PERIOD_VALID (0x07) bit 0. */
esp_err_t rbamp_is_period_valid(rbamp_handle_t dev, bool *out);

/** @brief Read @c REG_V03_PERIOD_AVG_P for one channel (W; non-contiguous registers). */
esp_err_t rbamp_read_period_avg_power(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read @c REG_V03_PERIOD_MAX_P_F0 (0xE0) — ch0 peak power this period (W). */
esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out);

/** @brief Read @c REG_V03_PERIOD_LATCH_MS (0xEC) — device-reported period duration (diagnostic). */
esp_err_t rbamp_read_period_latch_ms(rbamp_handle_t dev, uint32_t *out);

/**
 * @brief One-shot period snapshot: latch, settle, valid-check, read, integrate.
 *
 * Recommended entry point for period metering. Sequence:
 * 1. Write @c CMD_LATCH_PERIOD (skipped if @c skip_latch is true).
 * 2. @c vTaskDelay(settle_ms) — default 50 ms per SPEC.
 * 3. Read @c REG_V03_PERIOD_VALID; return @c ESP_ERR_INVALID_RESPONSE if 0.
 * 4. Read @c avg_p for each populated channel + @c max_p + @c latch_ms.
 * 5. Compute @c master_dt_ms since previous successful snapshot.
 * 6. Integrate into per-channel Wh totals (if energy is enabled in Kconfig).
 *
 * @param[in]  dev         Handle.
 * @param[out] out         Snapshot to populate.
 * @param[in]  settle_ms   Wait after latch before reading (default 50).
 *                         Pass 0 if @c skip_latch is true (no settle needed).
 * @param[in]  skip_latch  Skip the LATCH write — use after ::rbamp_broadcast_latch.
 *
 * @return
 *   - @c ESP_OK on success (snapshot is fresh)
 *   - @c ESP_ERR_INVALID_RESPONSE if @c REG_V03_PERIOD_VALID == 0 (stale)
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on transport failure
 */
esp_err_t rbamp_read_period_snapshot(rbamp_handle_t dev,
                                     rbamp_period_snapshot_t *out,
                                     uint16_t settle_ms,
                                     bool skip_latch);

/**
 * @brief Convenience helper — drop-in for ::rbamp_read_period_snapshot with
 *        @c settle_ms=50 and @c skip_latch=false.
 */
static inline esp_err_t rbamp_read_period_snapshot_simple(
        rbamp_handle_t dev, rbamp_period_snapshot_t *out)
{
    return rbamp_read_period_snapshot(dev, out, 50, false);
}

/* -------------------------------------------------------------------------
 * Energy (master-side, library-owned)
 * ------------------------------------------------------------------------- */

#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
/**
 * @brief Read the running total Wh for one channel.
 *
 * Updated automatically by ::rbamp_read_period_snapshot. To opt out per-handle
 * call ::rbamp_energy_disable. To remove the accumulator from the build
 * entirely, enable @c CONFIG_RBAMP_DISABLE_ENERGY in menuconfig — this also
 * removes the ::rbamp_energy_* prototypes from this header (linker would
 * otherwise emit unresolved-symbol errors).
 *
 * @param[in] dev Handle.
 * @param[in] ch  Channel index 0..2.
 * @return Running total in Wh (signed; negative = net export), or 0
 *         if @c ch is out of range or energy is disabled.
 */
double rbamp_energy_wh(rbamp_handle_t dev, uint8_t ch);

/** @brief Reset one channel's accumulator to zero. */
void rbamp_energy_reset(rbamp_handle_t dev, uint8_t ch);

/** @brief Reset all channels' accumulators to zero. */
void rbamp_energy_reset_all(rbamp_handle_t dev);

/** @brief Disable automatic integration. ::rbamp_energy_wh keeps returning the frozen value. */
void rbamp_energy_disable(rbamp_handle_t dev);

/** @brief Re-enable automatic integration after ::rbamp_energy_disable. */
void rbamp_energy_enable(rbamp_handle_t dev);
#endif /* !CONFIG_RBAMP_DISABLE_ENERGY */

/* -------------------------------------------------------------------------
 * Configuration (SPEC §10, §11)
 * ------------------------------------------------------------------------- */

/**
 * @brief Pin the sensor class and persist to flash.
 *
 * Writes @c REG_SENSOR_CLASS (0x25), issues @c CMD_SAVE_GAINS, blocks 700 ms
 * for the flash erase + write cycle.
 *
 * On v1.2+ firmware (@c rbamp_firmware_version >= 0x03) this is a precondition
 * for ::rbamp_set_ct_model and ::rbamp_set_ct_model_ch — calling either with
 * @c REG_SENSOR_CLASS still @c RBAMP_SENSOR_UNSET returns @c ESP_ERR_INVALID_STATE.
 * Pinning the class also resets @c REG_CT_MODEL to 0 device-side, preventing
 * stale class/model bleed across a two-step provisioning sequence.
 *
 * On v1.0 / v1.1 firmware the register exists in the firmware table but has no
 * functional effect — the write is harmless and ignored.
 *
 * Side effect: resets the per-channel batch tracker used by
 * ::rbamp_set_ct_model_ch — see that function's @c ESP_ERR_INVALID_STATE
 * description. Call this before reconfiguring CT models from scratch.
 *
 * @param[in] dev Handle.
 * @param[in] cls Sensor class. Only ::RBAMP_SENSOR_SCT013 ships as of v1.2;
 *                ::RBAMP_SENSOR_WIRED_CT and ::RBAMP_SENSOR_BUILTIN_CT are
 *                reserved.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev or unrecognised @c cls
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev, rbamp_sensor_class_t cls);

/**
 * @brief Set the SCT-013 CT model on channel 0 (legacy single-arg form).
 *
 * Writes @c REG_CT_MODEL (0x05), issues @c CMD_SAVE_GAINS, blocks 700 ms.
 *
 * v1.2+ firmware precondition: ::rbamp_set_sensor_class MUST be called first.
 * Otherwise this returns @c ESP_ERR_INVALID_STATE without writing. On v1.0 /
 * v1.1 firmware the precondition is skipped (backward compatibility — the
 * device-side callback has no guard).
 *
 * For multi-channel modules (UI2 / UI3 / I2 / I3) use ::rbamp_set_ct_model_ch
 * instead — this single-arg form configures only channel 0 via the device's
 * legacy direct-write path.
 *
 * @param[in] dev  Handle.
 * @param[in] code 1=SCT_013_005, 2=-010, 3=-030, 4=-050, 5=-100.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev or @c code outside 1..5
 *   - @c ESP_ERR_INVALID_STATE if v1.2+ firmware reports @c REG_SENSOR_CLASS == UNSET
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code);

/**
 * @brief Set the SCT-013 CT model on a specific channel (v1.2+ firmware).
 *
 * Sequence: writes @c REG_CT_MODEL, issues @c CMD_SET_CT_MODEL_CH0 /
 * @c CMD_SET_CT_MODEL_CH1 / @c CMD_SET_CT_MODEL_CH2 (0x28 / 0x29 / 0x2A) per
 * @p channel, waits 5 ms settle for the in-RAM preset-table lookup, then
 * issues @c CMD_SAVE_GAINS for flash persistence (700 ms erase). Blocks
 * ~705 ms per call.
 *
 * @warning Multi-channel call order matters. Writing @c REG_CT_MODEL also
 *          triggers the device-side legacy direct-write callback which applies
 *          the preset to channel 0 unconditionally. So
 *          @c rbamp_set_ct_model_ch(dev, 1, code) writes @c code's preset to
 *          channel 1 AS INTENDED, but also clobbers channel 0 to the same
 *          preset as a side-effect. To configure all channels with different
 *          models, **call the higher channel indices FIRST**:
 * @code
 * rbamp_set_ct_model_ch(dev, 2, 5);  // ch2 = SCT-013-100 (clobbers ch0 → 5)
 * rbamp_set_ct_model_ch(dev, 1, 3);  // ch1 = SCT-013-030 (clobbers ch0 → 3)
 * rbamp_set_ct_model_ch(dev, 0, 1);  // ch0 = SCT-013-005 (final ch0 preset)
 * @endcode
 *          Final state: ch0=preset 1, ch1=preset 3, ch2=preset 5.
 *
 * Requires @c rbamp_firmware_version() >= 0x03 (v1.2). Returns
 * @c ESP_ERR_NOT_SUPPORTED on older firmware. The same v1.2+ sensor-class
 * precondition as ::rbamp_set_ct_model applies.
 *
 * @par Batch tracker — ascending-order hard-fail
 * The library tracks the most-recent @p channel passed to this function
 * across a configuration batch and refuses any subsequent call where
 * @p channel is greater-than-or-equal-to the previous one (ascending or
 * repeat). The boundary of a "batch" is reset by ::rbamp_set_sensor_class
 * (which also resets @c REG_CT_MODEL device-side, matching the master-side
 * tracker). To reconfigure all channels with new presets, call
 * ::rbamp_set_sensor_class first, then call ::rbamp_set_ct_model_ch in
 * strict descending channel order.
 *
 * @param[in] dev     Handle.
 * @param[in] channel 0..2.
 * @param[in] code    1=SCT_013_005, 2=-010, 3=-030, 4=-050, 5=-100.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev, @c channel > 2, or @c code outside 1..5
 *   - @c ESP_ERR_NOT_SUPPORTED on firmware older than v1.2
 *   - @c ESP_ERR_INVALID_STATE in two cases (disambiguate via @c ESP_LOGW message):
 *     - @c REG_SENSOR_CLASS == @c UNSET (call ::rbamp_set_sensor_class first), OR
 *     - @p channel is greater-than-or-equal-to the previous batch channel
 *       (call ::rbamp_set_sensor_class to reset the batch, then call in
 *       descending order)
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code);

/**
 * @brief Bare @c CMD_SAVE_GAINS — flush gains/noise registers to flash (700 ms block).
 *
 * Normally invoked internally by ::rbamp_set_sensor_class, ::rbamp_set_ct_model,
 * ::rbamp_set_ct_model_ch, and ::rbamp_commit_address_change — most users
 * never call this directly.
 *
 * @warning Normally invoked internally. Bare @c rbamp_save_gains is relevant
 *          ONLY if the caller has manually written to non-public calibration
 *          registers via raw register access — that is an out-of-warranty
 *          operation and bypasses the SKU-matched preset table. Incorrect
 *          values produce wrong current / power readings with no obvious
 *          warning. Each call performs a flash erase + write cycle (~700 ms);
 *          flash endurance is finite (~10 000 cycles per page) — do not call
 *          in a loop.
 */
esp_err_t rbamp_save_gains(rbamp_handle_t dev);

/**
 * @brief Arm an I2C address change (step 1 of 2).
 *
 * Validates the new address range, verifies @c REG_MODE == develop, and
 * records the arm timestamp. Caller must call ::rbamp_commit_address_change
 * within 5 seconds.
 *
 * @warning Develop-mode-only operation. Address change requires the module to
 *          be in develop mode (factory-controlled — @c REG_MODE == 1). On a
 *          standard production module @c REG_MODE == 0 and this method
 *          returns @c ESP_ERR_INVALID_STATE — the device WILL NOT accept the
 *          address change. This pair of methods is intended for factory
 *          provisioning and integrator-side bench operations, not end-user
 *          code. If a deployed module needs a different I²C address, the
 *          documented path is via the factory bench (out of scope for the
 *          library).
 *
 * @param[in] dev      Handle.
 * @param[in] new_addr New 7-bit slave address (0x08..0x77, != current).
 *
 * @return
 *   - @c ESP_OK if armed
 *   - @c ESP_ERR_INVALID_ARG on bad @c new_addr
 *   - @c ESP_ERR_INVALID_STATE if device is not in develop mode
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_prepare_address_change(rbamp_handle_t dev, uint8_t new_addr);

/**
 * @brief Commit the previously prepared address change (step 2 of 2).
 *
 * Re-creates the internal @c i2c_master_dev_handle_t at the new address
 * after @c CMD_RESET completes; the caller's handle remains valid.
 *
 * @warning Develop-mode-only operation. Address change requires the module to
 *          be in develop mode (factory-controlled — @c REG_MODE == 1). On a
 *          standard production module @c REG_MODE == 0 and this method
 *          returns @c ESP_ERR_INVALID_STATE — the device WILL NOT accept the
 *          address change. This pair of methods is intended for factory
 *          provisioning and integrator-side bench operations, not end-user
 *          code. If a deployed module needs a different I²C address, the
 *          documented path is via the factory bench (out of scope for the
 *          library).
 *
 * @warning Re-enumeration after commit. After a successful commit the device
 *          resets and re-enumerates at the NEW address. Subsequent calls on
 *          this handle target the new address transparently — but any other
 *          master on the bus (Python script, ESP-IDF component instance on a
 *          different MCU, debug probe) still believes the device is at the
 *          old address until its own state is updated.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_STATE if no prior ::rbamp_prepare_address_change
 *   - @c ESP_ERR_TIMEOUT if the 5 s arm window expired
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_commit_address_change(rbamp_handle_t dev);

/**
 * @brief Issue @c CMD_FACTORY_RESET (0xAA) and wait 1500 ms.
 *
 * Erases ALL flash params (CT model, sensor class, calibration gains, I²C
 * address) and reboots the device. The bus is unavailable during reset.
 *
 * @warning Destructive operation. Erases ALL flash params (CT model, sensor
 *          class, calibration gains, I²C address). The module returns to
 *          factory defaults — @c rbamp_sensor_class_t becomes
 *          @c RBAMP_SENSOR_UNSET, @c REG_CT_MODEL becomes 0. Any tuning
 *          previously applied via ::rbamp_set_sensor_class /
 *          ::rbamp_set_ct_model is gone. The next user MUST re-apply both
 *          before metering is usable again. This is NOT a routine soft
 *          restart — use ::rbamp_reset for that. Reserve
 *          ::rbamp_factory_reset for known-bad-state recovery or for handing
 *          the module to another user / installation.
 */
esp_err_t rbamp_factory_reset(rbamp_handle_t dev);

/** @brief Issue @c CMD_RESET (0x01) and wait 100 ms. */
esp_err_t rbamp_reset(rbamp_handle_t dev);

/* -------------------------------------------------------------------------
 * Multi-module synchronisation
 * ------------------------------------------------------------------------- */

/**
 * @brief I2C General-Call broadcast LATCH — sync multiple modules at once.
 *
 * Writes @c [REG_COMMAND, CMD_LATCH_PERIOD] to general-call address 0x00.
 * All rbAmp modules on the bus latch within microseconds. Master should
 * then time its own wall-clock dt and call ::rbamp_read_period_snapshot
 * with @c skip_latch=true on each device handle.
 *
 * Internally adds a temporary device at 0x00, transmits, and removes it.
 *
 * @param[in] bus        I2C master bus handle.
 * @param[in] timeout_ms Bus transaction timeout.
 * @return @c ESP_OK if the general-call write succeeded.
 */
esp_err_t rbamp_broadcast_latch(i2c_master_bus_handle_t bus, uint32_t timeout_ms);

/* -------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

/** @brief Return a human-readable string for an rbamp / esp_err_t code. */
const char *rbamp_err_to_str(esp_err_t err);

/**
 * @brief Return the running retry-exhaustion counter.
 *
 * Incremented by 1 each time the I²C read-with-retry helper exhausts all
 * @c CONFIG_RBAMP_NACK_RETRY_ATTEMPTS attempts on a single byte read
 * (SPEC §B.5 transient NACK budget). Useful for monitoring bus health in
 * long-running deployments.
 *
 * Counter is monotonic — no reset API. Take snapshot + diff for
 * "exhaustions over interval" semantics.
 *
 * @return Counter value, or 0 on null @c dev.
 */
uint32_t rbamp_retry_exhaustion_count(rbamp_handle_t dev);

/**
 * @brief Return the running sanity-reject counter.
 *
 * Incremented by 1 each time a float-typed read passes I²C transport but
 * fails the in-library plausibility check (NaN / Inf / |value| > 10 000).
 * The read returns @c ESP_ERR_INVALID_RESPONSE; the caller normally retries
 * one period later. High counts suggest firmware-side calibration drift or
 * bus-noise corruption.
 *
 * Counter is monotonic — no reset API.
 *
 * @return Counter value, or 0 on null @c dev.
 */
uint32_t rbamp_sanity_reject_count(rbamp_handle_t dev);

#ifdef __cplusplus
}
#endif
