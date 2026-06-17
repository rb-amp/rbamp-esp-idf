/**
 * @file    rbamp.h
 * @brief   ESP-IDF client component for the rbAmp I²C sensor / dimmer module —
 *          public API.
 * @author  rbAmp team
 * @date    2026
 * @version 1.3.0
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

#include "rbamp_registers.h"     /* auto-generated; do not edit */
#include "rbamp_registers_v2.h"  /* auto-generated; v1.3 wire contract — do not edit */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Topology enumeration
 * ------------------------------------------------------------------------- */

/**
 * @brief Variant topology — set by the constructor (::rbamp_new defaults to
 *        THREE_PHASE; ::rbamp_new_with_topology pins it explicitly).
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

/**
 * @brief Hardware build variant — wire values for @c REG_HW_VARIANT (0x55).
 *
 * Source of truth for channel count and whether voltage / power are present
 * (v1.3 B1). UI-variants measure voltage + power; I-variants are current-only
 * (no @c U_rms, no @c P_real). UI3 is not buildable (the package has 3 ADC
 * channels + U; a 3-current + voltage variant would need 4). Branch on this
 * (and ::rbamp_read_capability) — never on @c REG_VERSION heuristics.
 */
typedef enum {
    RBAMP_VARIANT_UNKNOWN = 0, /**< Not yet read / unsupported firmware. */
    RBAMP_VARIANT_UI1     = 1, /**< 1 current + voltage + power. */
    RBAMP_VARIANT_UI2     = 2, /**< 2 current + voltage + power. */
    RBAMP_VARIANT_UI3     = 3, /**< Reserved — not buildable on the 3+U package. */
    RBAMP_VARIANT_I1      = 4, /**< 1 current, no voltage / power. */
    RBAMP_VARIANT_I2      = 5, /**< 2 current, no voltage / power. */
    RBAMP_VARIANT_I3      = 6, /**< 3 current, no voltage / power. */
} rbamp_variant_t;

/**
 * @brief CAPABILITY bitmap (0x57) — public aliases of @c RBAMP_V2_CAP_*.
 *
 * Libraries branch on these feature bits, not on firmware-version guesses.
 */
typedef enum {
    RBAMP_CAP_EXT_ADDRESSING  = RBAMP_V2_CAP_EXT_ADDRESSING,  /**< 16-bit extended register space. */
    RBAMP_CAP_GC_LATCH        = RBAMP_V2_CAP_GC_LATCH,        /**< General-Call broadcast latch. */
    RBAMP_CAP_GC_GROUP_FILTER = RBAMP_V2_CAP_GC_GROUP_FILTER, /**< GROUP_ID filtering of GC latch. */
    RBAMP_CAP_DIGEST          = RBAMP_V2_CAP_DIGEST,          /**< Compact poll DIGEST window. */
    RBAMP_CAP_EVENTS          = RBAMP_V2_CAP_EVENTS,          /**< EVENT_FLAGS + DRDY alarm line. */
    RBAMP_CAP_UID_ARBITRATION = RBAMP_V2_CAP_UID_ARBITRATION, /**< UID-based address arbitration. */
    RBAMP_CAP_SEAL            = RBAMP_V2_CAP_SEAL,            /**< Tamper seal. */
    RBAMP_CAP_TWO_PHASE_ADDR  = RBAMP_V2_CAP_TWO_PHASE_ADDR,  /**< Two-phase address commit (magic-armed). */
    RBAMP_CAP_ZC_PHASE_OFFSET = RBAMP_V2_CAP_ZC_PHASE_OFFSET, /**< Zero-cross phase offset register. */
    RBAMP_CAP_SAVE_USER_CONFIG = RBAMP_V2_CAP_SAVE_USER_CONFIG, /**< CMD_SAVE_USER_CONFIG persistence. */
    RBAMP_CAP_CLEAR_ERROR     = RBAMP_V2_CAP_CLEAR_ERROR,     /**< CMD_CLEAR_ERROR. */
} rbamp_capability_bit_t;

/**
 * @brief EVENT_FLAGS bits (0x2A) — public aliases of @c RBAMP_V2_EVENT_*.
 *
 * Sticky, write-1-to-clear. @c RBAMP_EVENT_ERROR (bit3) is the durable error
 * indicator (v1.3 A3): it asserts on any rejected write/command and survives
 * subsequent unrelated writes, unlike @c REG_ERROR which is last-write-outcome.
 */
typedef enum {
    RBAMP_EVENT_PERIOD_READY   = RBAMP_V2_EVENT_PERIOD_READY,   /**< A period snapshot latched. */
    RBAMP_EVENT_THRESH_I       = RBAMP_V2_EVENT_THRESH_I,       /**< Current threshold crossed. */
    RBAMP_EVENT_THRESH_P       = RBAMP_V2_EVENT_THRESH_P,       /**< Power threshold crossed. */
    RBAMP_EVENT_ERROR          = RBAMP_V2_EVENT_ERROR,          /**< Durable error indicator (REG_ERROR != 0). */
    RBAMP_EVENT_CONFIG_CHANGED = RBAMP_V2_EVENT_CONFIG_CHANGED, /**< Persisted config changed. */
    RBAMP_EVENT_RESET_OCCURRED = RBAMP_V2_EVENT_RESET_OCCURRED, /**< Device reset since last clear. */
} rbamp_event_bit_t;

/* -------------------------------------------------------------------------
 * Snapshot structures
 * ------------------------------------------------------------------------- */

/**
 * @defgroup rbamp_field_bits Snapshot implausible-field bits
 * @brief Quantity classes flagged in @c rbamp_snapshot_t.implausible.
 * @{
 */
#define RBAMP_FIELD_VOLTAGE   (1u << 0) /**< voltage / voltage_peak rejected. */
#define RBAMP_FIELD_CURRENT   (1u << 1) /**< a current / current_peak rejected. */
#define RBAMP_FIELD_POWER     (1u << 2) /**< a power rejected. */
#define RBAMP_FIELD_PF        (1u << 3) /**< a power_factor rejected. */
#define RBAMP_FIELD_FREQUENCY (1u << 4) /**< frequency rejected. */
/** @} */

/**
 * @brief Real-time metering snapshot — output of ::rbamp_read_all.
 *
 * All fields are SI units. Refreshed every 200 ms by the device firmware.
 * Unused channels (per ::rbamp_channels) are zeroed.
 *
 * A single out-of-bound field no longer fails the whole snapshot (standfw
 * finding #3): a field that trips the per-quantity sanity bound (e.g. an
 * uncalibrated @c voltage > @c RBAMP_SANITY_LIMIT_V) is set to @c NaN and its
 * class bit is set in @c implausible, while the rest of the snapshot stays
 * usable and ::rbamp_read_all still returns @c ESP_OK. A genuine transport
 * failure (NACK after retry + bus reset) still aborts the read. Consumers
 * should check @c implausible (or @c isnan per field) before using a value.
 */
typedef struct {
    float voltage;          /**< REG_V03_U_RMS, V. NaN if implausible. */
    float voltage_peak;     /**< REG_V03_U_PEAK, V. NaN if implausible. */
    float current[3];       /**< RMS current per channel, A. NaN if implausible. */
    float current_peak[3];  /**< Peak current per channel, A. NaN if implausible. */
    float power[3];         /**< Real power per channel, W (signed). NaN if implausible. */
    float power_factor[3];  /**< Power factor per channel (-1..+1). NaN if implausible. */
    float frequency;        /**< REG_AC_FREQ, Hz. NaN if implausible. */
    rbamp_topology_t topology; /**< Variant. */
    uint8_t channels;       /**< 1..3. */
    bool has_voltage_hw;
    uint8_t implausible;    /**< Bitmask of @c RBAMP_FIELD_* classes set to NaN. 0 = all OK. */
} rbamp_snapshot_t;

/**
 * @brief Period-metering snapshot — output of ::rbamp_read_period_snapshot.
 *
 * Energy integrates over @c master_dt_ms — the master's own wall-clock window
 * between successive consumed reads (esp_timer). @c latch_ms is the chip's
 * view of that window and is DIAGNOSTIC ONLY: it under-counts ~25-30% under
 * SysTick starvation (E.6/F10), so it must NOT be used for billing.
 */
typedef struct {
    float avg_p[3];      /**< Average real power per channel over the latched period, W. */
    float max_p;         /**< Peak real power on channel 0 during the period, W. */
    uint32_t latch_ms;   /**< Chip-reported period duration, ms — DIAGNOSTIC only (under-counts). */
    uint32_t master_dt_ms; /**< Master wall-clock window since the previous consumed read, ms — the energy dt. */
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

/**
 * @defgroup rbamp_sanity_limits Sanity-check upper bounds
 * @brief Per-quantity plausibility limits used by float-typed reads.
 *
 * Float-typed reads that survive I²C transport but assemble to NaN, Inf,
 * or |value| above the per-quantity limit are rejected as
 * @c ESP_ERR_INVALID_RESPONSE and the sanity counter advances
 * (::rbamp_sanity_reject_count). Limits are intentionally generous: they
 * catch bit-corrupted floats while passing brown-out (U≈80 V), mains
 * disconnect (U=0 V), and industrial 3-phase loads up to ~23 kW.
 * @{
 */
#define RBAMP_SANITY_LIMIT_V   500.0f   /**< Voltage RMS / peak — covers 480 V-class systems. */
#define RBAMP_SANITY_LIMIT_I   150.0f   /**< Current RMS / peak — covers SCT-013-100 + headroom. */
#define RBAMP_SANITY_LIMIT_P   30000.0f /**< Real power — covers industrial 3-phase. */
#define RBAMP_SANITY_LIMIT_PF  1.5f     /**< Power factor — physically bounded by [-1, +1] with tolerance. */
/** @} */

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
 *    is set by ::rbamp_new / ::rbamp_new_with_topology and not changed here.
 * 3. @c CMD_LATCH_PERIOD primer write + 50 ms settle — primes the FW period
 *    accumulator so the first user-visible snapshot integrates a full window.
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
 * Unused channels (per ::rbamp_channels) are filled with 0. A field that trips
 * the per-quantity sanity bound is set to @c NaN with its class bit raised in
 * @c out->implausible (the read still returns @c ESP_OK — see the
 * @c rbamp_snapshot_t docs). Only a genuine transport failure returns non-OK.
 *
 * @param[in]  dev Handle.
 * @param[out] out Snapshot to populate. On a non-OK return its contents are
 *                 partial / undefined — do not consume them (the fleet poll
 *                 zeroes the snapshot for a module that returned non-OK).
 * @return @c ESP_OK (snapshot usable, check @c implausible), or the first
 *         underlying transport error.
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
 * 2. Capture the master wall-clock and compute @c master_dt_ms since the
 *    previous consumed read (the billing energy window — esp_timer, not the
 *    chip's under-counting 0xEC).
 * 3. @c vTaskDelay(settle_ms) — default 50 ms per SPEC.
 * 4. Read @c REG_V03_PERIOD_VALID; return @c ESP_ERR_INVALID_RESPONSE if 0
 *    (on stale, energy is NOT integrated and the anchor is held).
 * 5. Read @c avg_p for each populated channel + @c max_p (+ @c latch_ms as a
 *    diagnostic), then integrate Wh over @c master_dt_ms and advance the
 *    anchor (if energy is enabled in Kconfig).
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
 * Call this before reconfiguring CT models from scratch (a class change also
 * resets @c REG_CT_MODEL device-side).
 *
 * Synchronously validated (standfw finding #1): after the write the device
 * @c REG_ERROR is checked in its reliable window, so a rejected class returns
 * @c ESP_ERR_INVALID_ARG immediately (no ~200 ms EVENT-bit3 wait), and the
 * flash @c SAVE is skipped on rejection.
 *
 * @param[in] dev Handle.
 * @param[in] cls Sensor class. Only ::RBAMP_SENSOR_SCT013 ships as of v1.2;
 *                ::RBAMP_SENSOR_WIRED_CT and ::RBAMP_SENSOR_BUILTIN_CT are
 *                reserved.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev, unrecognised @c cls, or device
 *     ERR_PARAM rejection of the write
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
 * @param[in] code CT preset for the active sensor class. SCT-013: 1=-005,
 *                 2=-010, 3=-030, 4=-050, 6=-020 (5=-100, 7=-060 reserved →
 *                 ERR_PARAM). WIRED_CT: 1..3. The accepted set is per-class,
 *                 not a contiguous range.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev, @c code outside 1..7, or @c code
 *     not in the active class's accepted set
 *   - @c ESP_ERR_INVALID_STATE if v1.2+ firmware reports @c REG_SENSOR_CLASS == UNSET
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code);

/**
 * @brief Set the SCT-013 CT model on a specific channel (v1.2+ firmware).
 *
 * Sequence: stages the preset by writing @c REG_CT_MODEL, issues
 * @c CMD_SET_CT_MODEL_CH0 / @c CMD_SET_CT_MODEL_CH1 / @c CMD_SET_CT_MODEL_CH2
 * (0x28 / 0x29 / 0x2A) per @p channel, waits 5 ms settle for the in-RAM
 * preset-table lookup, then issues @c CMD_SAVE_GAINS for flash persistence
 * (700 ms erase). Blocks ~705 ms per call.
 *
 * @note Order-independent (v1.3 A1 / Fix A). @c REG_CT_MODEL is PURE STAGING —
 *       writing it no longer applies the preset to channel 0; the per-channel
 *       command binds the staged preset to the requested channel ONLY. So
 *       mixed-CT configuration can be done in any channel order, and a bind to
 *       one channel never clobbers another:
 * @code
 * rbamp_set_ct_model_ch(dev, 0, 1);  // ch0 = SCT-013-005
 * rbamp_set_ct_model_ch(dev, 1, 3);  // ch1 = SCT-013-030
 * rbamp_set_ct_model_ch(dev, 2, 6);  // ch2 = SCT-013-020
 * @endcode
 *       (The pre-Fix-A ch0 auto-apply that once forced "bind ch0 last" is gone.)
 *       For a one-call bulk variant see ::rbamp_configure_channels.
 *
 * Requires @c rbamp_firmware_version() >= 0x03 (v1.2). Returns
 * @c ESP_ERR_NOT_SUPPORTED on older firmware. The same v1.2+ sensor-class
 * precondition as ::rbamp_set_ct_model applies.
 *
 * @param[in] dev     Handle.
 * @param[in] channel 0..2.
 * @param[in] code    1=SCT_013_005, 2=-010, 3=-030, 4=-050, 5=-100.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev, @c channel > 2, @c code outside
 *     1..7, or @c code not in the active class's accepted set
 *   - @c ESP_ERR_NOT_SUPPORTED on firmware older than v1.2
 *   - @c ESP_ERR_INVALID_STATE if @c REG_SENSOR_CLASS == @c UNSET on v1.2+
 *     (call ::rbamp_set_sensor_class first)
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code);

/**
 * @brief Read the CT model ACTUALLY APPLIED to @p channel (0x51/0x52/0x53).
 *
 * v1.3 bind-verify (B10): after ::rbamp_set_ct_model_ch, read this mirror to
 * confirm the preset took. @c 0 = unset. Distinct from @c REG_CT_MODEL (0x05)
 * which only holds the staged value.
 *
 * @param[in]  channel 0..2.
 * @param[out] out     Receives the applied model code.
 */
esp_err_t rbamp_read_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t *out);

/**
 * @brief Bulk configure: sensor class + per-channel CT models in one call.
 *
 * Sets @p cls via ::rbamp_set_sensor_class, then binds @p models[ch] for each
 * channel (variant-aware — clamps @p n to ::rbamp_channels). Mixed-CT is
 * order-independent (v1.3 A1 / Fix A — see ::rbamp_set_ct_model_ch). A @c 0
 * entry in @p models skips that channel.
 *
 * @note Flash-friendly: the per-channel binds are batched behind a SINGLE
 *       terminal @c CMD_SAVE_GAINS (plus the one in @c set_sensor_class), so a
 *       3-channel configure costs 2 flash cycles, not 1+N. Each bind is still
 *       synchronously verified and read-back-confirmed before the save.
 *
 * @param[in] dev    Handle.
 * @param[in] cls    Sensor class to apply first.
 * @param[in] models Array of CT model codes (per-class accepted set — see
 *                   ::rbamp_set_ct_model_ch), one per channel.
 * @param[in] n      Length of @p models.
 *
 * @return @c ESP_OK; @c ESP_ERR_INVALID_STATE if a channel could not be bound;
 *         propagated transport / validation errors otherwise.
 */
esp_err_t rbamp_configure_channels(rbamp_handle_t dev, rbamp_sensor_class_t cls,
                                   const uint8_t *models, uint8_t n);

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
 * Validates the new address range and records the arm timestamp. Caller must
 * call ::rbamp_commit_address_change within 5 seconds.
 *
 * @note v1.3: this is **production-OK** (truth-doc §6.1) — the two-phase
 *       magic-armed @c CMD_COMMIT_ADDR is no longer develop-gated, so
 *       field-swapping a production spare is supported. For provisioning a
 *       fresh module from the factory @c 0x50, prefer ::rbamp_provision (it
 *       wraps the solo-on-bus + conflict + verify + SAVE flow).
 *
 * @param[in] dev      Handle.
 * @param[in] new_addr New 7-bit slave address (0x08..0x77, != current).
 *
 * @return
 *   - @c ESP_OK if armed
 *   - @c ESP_ERR_INVALID_ARG on bad @c new_addr
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 */
esp_err_t rbamp_prepare_address_change(rbamp_handle_t dev, uint8_t new_addr);

/**
 * @brief Commit the previously prepared address change (step 2 of 2).
 *
 * Stages the candidate (write 0x30), arms @c REG_ADDR_COMMIT_MAGIC (0x31) =
 * 0xA5, issues @c CMD_COMMIT_ADDR (persists to flash), resets, then re-creates
 * the internal @c i2c_master_dev_handle_t at the new address; the caller's
 * handle remains valid.
 *
 * @note v1.3: production-OK (see ::rbamp_prepare_address_change).
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
 * Transmits a 5-byte general-call frame @c [0xA5, 0x27, group, tick_lo, tick_hi]
 * to address @c 0x00 (group = @c 0x00 → all-call). Modules with GC enabled
 * latch atomically. Master should then sleep one settle interval and call
 * ::rbamp_read_period_snapshot with @c skip_latch=true on each device handle
 * (witness pattern: read @c REG_V03_PERIOD_VALID first, skip read on
 * @c valid=false).
 *
 * @par Enable preconditions (one-time, per module)
 * Each participating module must have GC opted in via
 * @c REG_FLEET_CONFIG (0x27) bit 0 = 1, then persisted with
 * @c CMD_SAVE_USER_CONFIG, then re-booted via @c CMD_RESET. The setting
 * survives power cycles. Modules without the opt-in silently ignore the
 * frame.
 *
 * @par Group filtering
 * @c REG_GROUP_ID (0x28) lets multiple GC domains coexist on one bus.
 * This helper hard-wires @c group=0x00 (all-call) and @c tick=0x0000 —
 * suitable for the common "sync all modules" case. Bespoke group + tick
 * use cases should assemble the 5-byte frame directly.
 *
 * @param[in] bus        I2C master bus handle.
 * @param[in] timeout_ms Bus transaction timeout (also used as post-latch settle).
 *
 * @return
 *   - @c ESP_OK if the general-call frame was transmitted (note: GC is
 *     write-only — there is no ACK from individual modules; success here
 *     means the host driver did not error).
 *   - @c ESP_ERR_INVALID_ARG on null @c bus.
 *   - Bus-transport errors propagated from @c i2c_master_transmit.
 *
 * @see SPEC §B.5 (NACK discipline), truth-doc §5 (GC enable sequence).
 */
esp_err_t rbamp_broadcast_latch(i2c_master_bus_handle_t bus, uint32_t timeout_ms);

/**
 * @brief I2C General-Call broadcast LATCH with explicit group + tick.
 *
 * Transmits @c [0xA5, 0x27, group, tick_lo, tick_hi] to address 0x00. Only
 * modules whose @c REG_GROUP_ID matches @p group (or @p group == 0x00
 * all-call) and that have GC enabled latch. @p tick is the master's
 * fleet-wide window number — each accepted module stores it in
 * @c REG_GC_TICK (0x59) so the master can detect a module that missed a
 * frame (its tick lags, or reads 0xFFFF = never received).
 *
 * @param[in] bus        I2C master bus handle.
 * @param[in] group      GROUP_ID filter, 0x00 = all-call.
 * @param[in] tick       Fleet window number (monotonic on the master side).
 * @param[in] timeout_ms Bus transaction timeout.
 * @return @c ESP_OK if the frame was transmitted; @c ESP_ERR_INVALID_ARG on
 *         null @c bus; bus-transport errors otherwise.
 */
esp_err_t rbamp_broadcast_latch_group(i2c_master_bus_handle_t bus,
                                      uint8_t group, uint16_t tick,
                                      uint32_t timeout_ms);

/* -------------------------------------------------------------------------
 * v1.3 device identity & capability (B1) — branch on these, not on VERSION
 * ------------------------------------------------------------------------- */

/** @brief Read @c REG_HW_VARIANT (0x55) and map to ::rbamp_variant_t. */
esp_err_t rbamp_read_variant(rbamp_handle_t dev, rbamp_variant_t *out);

/** @brief Read the 16-bit CAPABILITY bitmap (0x57). Test bits with @c RBAMP_CAP_*. */
esp_err_t rbamp_read_capability(rbamp_handle_t dev, uint16_t *out);

/** @brief Read @c REG_PRODUCT_ID (0x54): 0x01=rbAmp sensor, 0x02=rbDimmer (own map). */
esp_err_t rbamp_read_product_id(rbamp_handle_t dev, uint8_t *out);

/** @brief Read the 96-bit chip UID (0x5C, 12 bytes, one burst). @p out must hold 12 bytes. */
esp_err_t rbamp_read_uid(rbamp_handle_t dev, uint8_t out[12]);

/**
 * @brief Does this variant measure voltage / power? (false for I1/I2/I3).
 *
 * Convenience over ::rbamp_read_variant — returns the cached value detected
 * by ::rbamp_begin. I-variants report @c U_rms = 0.0 and have no @c P_real.
 */
bool rbamp_has_voltage(rbamp_handle_t dev);

/* -------------------------------------------------------------------------
 * v1.3 error / event signalling (A3 / A4)
 * ------------------------------------------------------------------------- */

/**
 * @brief Read @c REG_ERROR (0x02) — the LAST register-write outcome.
 *
 * v1.3 A3: this reflects only the most recent write/command; a later
 * unrelated write clears it. Read it right after the operation you care
 * about, before the next write. For a durable signal use ::rbamp_has_error
 * (EVENT bit3). @c 0x00 = OK; @c 0xFA..0xFF are device error classes
 * (@c RBAMP_V2_DEV_ERR_*).
 */
esp_err_t rbamp_read_last_error(rbamp_handle_t dev, uint8_t *out);

/** @brief Read the sticky EVENT_FLAGS bitmap (0x2A). Test bits with @c RBAMP_EVENT_*. */
esp_err_t rbamp_read_event_flags(rbamp_handle_t dev, uint8_t *out);

/** @brief Write-1-to-clear EVENT_FLAGS bits (0x2A). Pass e.g. @c RBAMP_EVENT_ERROR. */
esp_err_t rbamp_clear_event_flags(rbamp_handle_t dev, uint8_t mask);

/**
 * @brief Durable error check (v1.3 A3) — true if EVENT_FLAGS bit3 is set.
 *
 * Unlike a @c REG_ERROR readback, EVENT bit3 is sticky (asserts on any
 * rejected write/command and survives later writes) until cleared via
 * ::rbamp_clear_error or a write-1-to-clear of @c RBAMP_EVENT_ERROR. This is
 * the right channel for ASYNC fault polling.
 *
 * @note Timing (standfw HW-verified): bit3 is immediate and reliable for
 *       command-path rejections and runtime faults. For register-WRITE
 *       rejections (e.g. an invalid @c SENSOR_CLASS enum) the async bit3 latch
 *       is **best-effort** — its timing is firmware-dependent pending the
 *       OBS-BIT3-ASYMMETRY symmetry fix, and it may not assert at all. Do NOT
 *       rely on bit3 to validate your own just-issued config write: the config
 *       setters (::rbamp_set_sensor_class / ::rbamp_set_ct_model /
 *       ::rbamp_set_ct_model_ch) read @c REG_ERROR in its synchronous window
 *       and return @c ESP_ERR_INVALID_ARG on rejection — use that return code
 *       as the authoritative write-validation result. @c rbamp_has_error is the
 *       durable async channel for command-path / runtime faults.
 */
esp_err_t rbamp_has_error(rbamp_handle_t dev, bool *out);

/** @brief Issue @c CMD_CLEAR_ERROR (0x31) — clears REG_ERROR + EVENT bit3. */
esp_err_t rbamp_clear_error(rbamp_handle_t dev);

/* -------------------------------------------------------------------------
 * v1.3 persistence & provisioning (A2 / B2)
 * ------------------------------------------------------------------------- */

/**
 * @brief Issue @c CMD_SAVE_USER_CONFIG (0x32) — persist user config (700 ms).
 *
 * Persists @c sensor_class / @c ct_model (per channel) / @c fleet_config /
 * @c group_id / @c label. Production-OK (not develop-gated). On a freshly
 * flashed module this also clears the first-boot
 * @c RBAMP_V2_DEV_ERR_FLASH_PARAMS_BAD (0xFB) state (v1.3 B2).
 */
esp_err_t rbamp_save_user_config(rbamp_handle_t dev);

/**
 * @brief Is this module provisioned (params page valid)? (v1.3 B2 / C16).
 *
 * A freshly flashed module boots with @c REG_ERROR ==
 * @c RBAMP_V2_DEV_ERR_FLASH_PARAMS_BAD (0xFB) and runs on factory defaults.
 * One ::rbamp_save_user_config clears it. Returns @c *out = false in that
 * state so a provisioning flow knows to configure + SAVE once.
 */
esp_err_t rbamp_is_provisioned(rbamp_handle_t dev, bool *out);

/**
 * @brief Read the ACTIVE I²C address from @c REG_I2C_ADDRESS (0x30).
 *
 * v1.3 A2: at boot this register is honest — it reads the flash-active
 * address (a re-addressed module reads its new address). After staging a
 * candidate via ::rbamp_prepare_address_change it echoes the candidate until
 * commit + reset. Useful to verify a two-phase change after the post-commit
 * reboot.
 */
esp_err_t rbamp_read_active_address(rbamp_handle_t dev, uint8_t *out);

/* -------------------------------------------------------------------------
 * v1.3 fleet config — GC enable, group, tick, label (multi-module)
 * ------------------------------------------------------------------------- */

/**
 * @brief Opt this module into General-Call latch reception (v1.3 C / §5).
 *
 * Writes @c REG_FLEET_CONFIG (0x27) bit0 = @p enable, persists via
 * @c CMD_SAVE_USER_CONFIG, then resets (the GC ISR is wired at boot, so a
 * reset is required for the change to take effect). Blocks ~1 s
 * (SAVE_USER_CONFIG settle + reset settle). After this the module responds
 * to ::rbamp_broadcast_latch.
 */
esp_err_t rbamp_enable_gc(rbamp_handle_t dev, bool enable);

/** @brief Read @c REG_FLEET_CONFIG (0x27). bit0 = GC enabled. */
esp_err_t rbamp_read_fleet_config(rbamp_handle_t dev, uint8_t *out);

/** @brief Set @c REG_GROUP_ID (0x28). Persist with ::rbamp_save_user_config. 0 = all-call only. */
esp_err_t rbamp_set_group_id(rbamp_handle_t dev, uint8_t group);

/** @brief Read @c REG_GROUP_ID (0x28). */
esp_err_t rbamp_read_group_id(rbamp_handle_t dev, uint8_t *out);

/**
 * @brief Read @c REG_GC_TICK (0x59) — tick of the last accepted GC frame.
 *
 * @c 0xFFFF means this module has never received a GC latch (GC disabled,
 * group mismatch, or it missed every frame). Compare against the master's
 * last broadcast tick to detect a module that dropped a fleet window.
 */
esp_err_t rbamp_read_gc_tick(rbamp_handle_t dev, uint16_t *out);

/** @brief Read the 8-byte ASCII location label (0x68). @p out must hold 9 bytes (NUL-terminated). */
esp_err_t rbamp_read_label(rbamp_handle_t dev, char out[9]);

/** @brief Write the 8-byte ASCII location label (0x68). Persist with ::rbamp_save_user_config. */
esp_err_t rbamp_write_label(rbamp_handle_t dev, const char *label);

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
 * Cleared by ::rbamp_reset_counters. Test scenarios use the reset; in
 * production take snapshot + diff for "exhaustions over interval" instead.
 *
 * @return Counter value, or 0 on null @c dev.
 */
uint32_t rbamp_retry_exhaustion_count(rbamp_handle_t dev);

/**
 * @brief Return the running sanity-reject counter.
 *
 * Incremented by 1 each time a float-typed read passes I²C transport but
 * fails the in-library plausibility check (NaN, Inf, or |value| above the
 * per-quantity sanity limit — see @c RBAMP_SANITY_LIMIT_V / @c _I / @c _P /
 * @c _PF). The read returns @c ESP_ERR_INVALID_RESPONSE; the caller normally
 * retries one period later. High counts suggest firmware-side calibration
 * drift or bus-noise corruption.
 *
 * Cleared by ::rbamp_reset_counters.
 *
 * @return Counter value, or 0 on null @c dev.
 */
uint32_t rbamp_sanity_reject_count(rbamp_handle_t dev);

/**
 * @brief Reset both diagnostic counters to zero.
 *
 * Zeroes @c retry_exhaustion_count and @c sanity_reject_count on this handle.
 * Intended for test scenarios that need a clean baseline; production
 * monitoring should snapshot + diff the counters instead, which is robust
 * against missed reset windows.
 *
 * @param[in] dev Handle. Passing NULL is a no-op.
 */
void rbamp_reset_counters(rbamp_handle_t dev);

#ifdef __cplusplus
}
#endif
