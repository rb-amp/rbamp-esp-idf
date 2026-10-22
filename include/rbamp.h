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
 * - BASIC line: one I2C address phase per byte — no auto-increment (SPEC §6).
 * - STD line: 16-bit big-endian pointer, burst reads; detected by ::rbamp_begin
 *   (see ::rbamp_line_t) and routed internally — same API for both lines.
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
#include <stddef.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#include "rbamp_registers.h"     /* auto-generated; do not edit */
#include "rbamp_registers_v2.h"  /* auto-generated; v1.3 wire contract — do not edit */
#include "rbamp_sensor_models.h" /* auto-generated from sensor_models.yaml — model
                                  * names/descriptors + rbamp_sensor_model_lookup();
                                  * do not edit */
#include "rbamp_std_registers.h" /* auto-generated from registers_std.yaml — STD
                                  * line (16-bit family map); do not edit */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Topology enumeration
 * ------------------------------------------------------------------------- */

/**
 * @brief Variant topology — adopted from the module in ::rbamp_begin
 *        (::rbamp_new), or pinned by the caller (::rbamp_new_with_topology).
 *
 * Reflects how many independent current channels the device exposes.
 * Voltage hardware presence is a separate flag — see ::rbamp_has_voltage_hw.
 *
 * The value is the channel COUNT — derived from @c REG_HW_VARIANT (0x55) on
 * v1.3+ firmware, or the constructor fallback (THREE_PHASE) on older firmware
 * that returns 0 there.
 * Channels 0-2 are read from the flat RT block; channels 3+ (senior UI5/UI7
 * SKUs) are reachable ONLY through the channel access window — see
 * ::rbamp_read_channel. The enum is intentionally non-contiguous (no 4/6):
 * senior parts step 3 → 5 → 7.
 */
typedef enum {
    RBAMP_TOPOLOGY_SINGLE      = 1, /**< 1 current channel (UI1 / I1). */
    RBAMP_TOPOLOGY_SPLIT_PHASE = 2, /**< 2 current channels (UI2 / I2). */
    RBAMP_TOPOLOGY_THREE_PHASE = 3, /**< 3 current channels (UI3 / I3). */
    RBAMP_TOPOLOGY_FIVE_CH     = 5, /**< 5 current channels (UI5 senior SKU). */
    RBAMP_TOPOLOGY_SEVEN_CH    = 7, /**< 7 current channels (UI7 senior SKU). */
} rbamp_topology_t;

/** @brief Largest channel count any SKU exposes — size channel arrays with this. */
#define RBAMP_MAX_CHANNELS 7
/** Channels carried by ::rbamp_snapshot_t (ch0-2). A module with more channels
 *  (UI5/UI7) reports its full count in @c channels; read ch3+ with
 *  ::rbamp_read_channel / ::rbamp_read_all_channels. */
#define RBAMP_SNAPSHOT_CHANNELS 3

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
 * Class 1 is a module the user fits with an SCT-013 of their choice; the
 * model selects the sensor type. Classes 2 and 3 are modules that ship with
 * their sensor and are calibrated together with it: the library does not
 * rewrite their class or model (::rbamp_set_sensor_class and
 * ::rbamp_set_ct_model_ch return @c ESP_ERR_INVALID_STATE); a service
 * re-pairing goes through ::rbamp_set_ct_model_ch_force.
 */
typedef enum {
    RBAMP_SENSOR_UNSET       = 0, /**< Default after factory reset. */
    RBAMP_SENSOR_SCT013      = 1, /**< SCT-013 current transformer fitted by the user. */
    RBAMP_SENSOR_WIRED_CT    = 2, /**< Wired CT shipped with the module, calibrated as a pair. */
    RBAMP_SENSOR_BUILTIN_CT  = 3, /**< Built-in CT, calibrated as a pair. */
} rbamp_sensor_class_t;

/**
 * @brief Hardware build variant — wire values for @c REG_HW_VARIANT (0x55).
 *
 * Source of truth for channel count and whether voltage / power are present
 * (v1.3 B1). UI-variants measure voltage + power; I-variants are current-only
 * (no @c U_rms, no @c P_real). Branch on this (and ::rbamp_read_capability)
 * — never on @c REG_VERSION heuristics.
 */
typedef enum {
    RBAMP_VARIANT_UNKNOWN = 0, /**< Not yet read / unsupported firmware. */
    RBAMP_VARIANT_UI1     = 1, /**< 1 current + voltage + power. */
    RBAMP_VARIANT_UI2     = 2, /**< 2 current + voltage + power. */
    RBAMP_VARIANT_UI3     = 3, /**< 3 current + voltage + power. */
    RBAMP_VARIANT_I1      = 4, /**< 1 current, no voltage / power. */
    RBAMP_VARIANT_I2      = 5, /**< 2 current, no voltage / power. */
    RBAMP_VARIANT_I3      = 6, /**< 3 current, no voltage / power. */
    RBAMP_VARIANT_UI5     = 7, /**< 5 current + voltage + power (senior SKU). */
    RBAMP_VARIANT_UI7     = 8, /**< 7 current + voltage + power (senior SKU). */
    RBAMP_VARIANT_U3I3    = 9, /**< STD line: 3 voltage + 3 current (three-phase meter). */
    RBAMP_VARIANT_U3I6    = 10,/**< STD line: 3 voltage + 6 current. */
} rbamp_variant_t;

/* -------------------------------------------------------------------------
 * Product line (BASIC / STD) — detected by rbamp_begin
 * ------------------------------------------------------------------------- */

/**
 * @brief Which register model the module behind a handle speaks.
 *
 * One handle type and one API serve both lines; ::rbamp_begin discovers the
 * line from the module's identity block and routes every call internally.
 * - @c RBAMP_LINE_BASIC — 8-bit register pointer, float registers, channel
 *   access window for senior channels (the v1.x contract in @c rbamp_registers_v2.h).
 * - @c RBAMP_LINE_STD — 16-bit big-endian register pointer with auto-increment,
 *   fixed-point registers, per-channel blocks at fixed family addresses
 *   (@c rbamp_std_registers.h). Reactive / apparent power, THD, signed energy
 *   and the multi-bank period latch exist only on this line.
 *
 * Calls that have no counterpart on the detected line return
 * @c ESP_ERR_NOT_SUPPORTED — never a wrong-register read.
 */
typedef enum {
    RBAMP_LINE_BASIC = 0, /**< 8-bit map (default until ::rbamp_begin runs). */
    RBAMP_LINE_STD   = 1, /**< 16-bit family map. */
} rbamp_line_t;

/**
 * @brief How an STD module delivers period energy — decided by ::rbamp_begin
 *        from the capability bits, exposed in ::rbamp_std_info_t.
 *
 * - @c RBAMP_STD_PERIOD_LATCH: the module has latch banks
 *   (@c RBAMP_STD_CAP_MULTI_LATCH). A period is latch-to-latch on bank 0,
 *   averages and per-channel energy come from the bank.
 * - @c RBAMP_STD_PERIOD_COUNTERS: no latch banks, but persisted life-time
 *   counters (@c RBAMP_STD_CAP_ONDEVICE_ENERGY). A period is the difference
 *   of two counter readings (Wh; mWh from map 0x36), taken on the master's schedule; no
 *   latch command, nothing to arm, nothing lost across a module restart.
 * - @c RBAMP_STD_PERIOD_NONE: neither — the period / energy calls return
 *   @c ESP_ERR_NOT_SUPPORTED.
 */
typedef enum {
    RBAMP_STD_PERIOD_NONE     = 0,
    RBAMP_STD_PERIOD_LATCH    = 1,
    RBAMP_STD_PERIOD_COUNTERS = 2,
} rbamp_std_period_source_t;

/**
 * @brief STD-line identity, cached by ::rbamp_begin (no bus traffic to read).
 *
 * Feature gating on this line goes by @c capabilities (@c RBAMP_STD_CAP_*) and
 * @c map_version, never by guessing. Channel gating goes by @c n_voltage /
 * @c n_current: registers of channels at or beyond those counts are
 * unspecified on the module and the library refuses to read them.
 */
typedef struct {
    uint8_t  map_version;  /**< Register-map version (@c RBAMP_STD_REG_VERSION). */
    uint16_t capabilities; /**< @c RBAMP_STD_CAP_* bitmap. */
    uint8_t  status;       /**< @c RBAMP_STD_ST_* bits at begin() time (live copy: ::rbamp_std_read_health). */
    uint8_t  n_voltage;    /**< Populated voltage channels (0..8). */
    uint8_t  n_current;    /**< Populated current channels (1..64; this component exposes up to ::RBAMP_MAX_CHANNELS). */
    uint8_t  variant;      /**< Raw @c RBAMP_STD_VARIANT_* byte (also mapped into ::rbamp_read_variant). */
    uint32_t active_bank;  /**< Running firmware bank base (@c RBAMP_STD_REG_OTA_ACTIVE), 0 if the module has no OTA capability. */
    uint32_t ota_bank_size;/**< Flash bank size the module reports (image + header must fit), 0 if none. */
    rbamp_std_period_source_t period_source; /**< Which period/energy mechanism this module offers. */
} rbamp_std_info_t;

/**
 * @brief One STD current channel (or a TOTALS aggregate) — output of
 *        ::rbamp_std_read_channel / ::rbamp_std_read_totals.
 *
 * Decoded from one burst read of the channel block, so the fields are as
 * coherent as the module publishes them (fields are published byte-wise per
 * publish window — 100 ms across the family (BASIC images before v1.2 and STD images up to 0x7B: 200 ms);
 * for a strictly atomic multi-field snapshot use the period latch). Fixed-point wire units are converted to SI here.
 * Fields that trip a sanity bound are set to @c NaN with the matching
 * @c RBAMP_FIELD_* bit in @c implausible, exactly like ::rbamp_snapshot_t.
 *
 * @note @c thd_i, @c pf1 and @c q1 read 0 unless the module's harmonics
 *       extraction is enabled (@c RBAMP_STD_REG_CFG_HARM bit 0; off by default
 *       on the regular tier).
 * @note @c e_import_mws / @c e_export_mws are the module's raw signed energy
 *       accumulators in mW·samples. The module has no clock, so converting to
 *       Wh needs the sample count and the master's own period timing (period
 *       latch path). For monitoring, integrate @c p over your poll interval.
 */
typedef struct {
    float   i_rms;         /**< RMS current, A. */
    float   i_peak;        /**< Peak current, A — reads 0 on module map 0x34 and earlier (not published); use a latch bank's @c max_i for the peak over a period. */
    float   p;             /**< Active power, W (signed). */
    float   q;             /**< Reactive power, var (signed). */
    float   s;             /**< Apparent power, VA. */
    float   pf;            /**< Power factor (-1..+1). */
    float   thd_i;         /**< Current THD, % (0 when harmonics are off). */
    float   pf1;           /**< Displacement (fundamental) power factor (0 when harmonics are off). */
    float   q1;            /**< Fundamental reactive power, var (0 when harmonics are off). */
    int64_t e_import_mws;  /**< Import energy accumulator, mW·samples (>= 0). */
    int64_t e_export_mws;  /**< Export energy accumulator, mW·samples (<= 0). */
    uint8_t implausible;   /**< Bitmask of @c RBAMP_FIELD_* classes set to NaN. 0 = all OK. */
} rbamp_std_channel_t;

/** @brief One STD voltage channel — output of ::rbamp_std_read_voltage. */
typedef struct {
    float   u_rms;         /**< RMS voltage, V. */
    float   u_peak;        /**< Peak voltage, V — reads 0 on module map 0x34 and earlier (not published); use a latch bank's @c max_u / @c min_u for the extremes over a period. */
    float   thd_u;         /**< Voltage THD, % (0 when harmonics are off). */
    uint8_t implausible;   /**< @c RBAMP_FIELD_VOLTAGE if a bound tripped. */
    float   angle_deg;     /**< Fundamental phase angle of this voltage input against input 0, degrees (-180, 180]; input 0 reads 0. Forward sequence: input 1 ≈ -120, input 2 ≈ +120. 0 when @c angle_valid is false. */
    bool    angle_valid;   /**< true when the module publishes the angle: module firmware 0x0001007A or newer, this input and input 0 above 50 V. Single-voltage modules: false. */
} rbamp_std_voltage_t;

/** @brief Order of the voltage inputs of a three-phase module — output of ::rbamp_std_read_phase_seq. */
typedef enum {
    RBAMP_STD_PHASE_SEQ_NA       = 0, /**< Not available: fewer than three voltage inputs, or one of them below 50 V. */
    RBAMP_STD_PHASE_SEQ_FORWARD  = 1, /**< L1-L2-L3 (input 1 lags input 0 by ~120°). */
    RBAMP_STD_PHASE_SEQ_REVERSE  = 2, /**< L1-L3-L2. */
    RBAMP_STD_PHASE_SEQ_ABNORMAL = 3, /**< No 120° pattern, e.g. two inputs wired to the same phase. */
} rbamp_std_phase_seq_t;

/**
 * @brief STD module health — output of ::rbamp_std_read_health (live).
 *
 * The fields after @c crc_errors exist on map 0x35+ modules and read zero /
 * false on older ones. Alert on: @c nvm_fault or @c lta_status >= 2 (energy
 * store), @c cal_src == 3 (stored calibration is for another range, defaults
 * in use, @c cal_ok clear too), @c lost_frames / @c ring_drop growing,
 * @c time_valid false (set the clock), @c uptime_s decreasing (restart).
 */
typedef struct {
    uint8_t  status;       /**< @c RBAMP_STD_ST_* bits: cal_ok / sensor_ok / mains_ok / locked / time_valid. */
    uint32_t frame_count;  /**< Sensor frame counter — must grow between polls (liveness). */
    uint32_t crc_errors;   /**< Sensor frame CRC error counter. */
    bool     time_valid;   /**< Module clock is set (STATUS bit 4); false after every module reset — call ::rbamp_std_set_time. */
    bool     nvm_fault;    /**< Energy store fault or corrupt (STATUS bit 5; details in @c lta_status). */
    uint8_t  lta_status;   /**< Life-time energy store: 0 ok · 1 new (never written) · 2 fault · 3 corrupt (service). */
    uint8_t  lta_src;      /**< Life-time counters loaded from: 0 none · 1 NVM · 2 backup domain · 3 legacy · 4 rollback anchor. */
    uint8_t  cal_src;      /**< Calibration in use: 0 built-in defaults · 1 flash · 2 EEPROM · 3 stored cal is for another range (defaults in use). */
    uint32_t lost_frames;  /**< Sensor frames lost since boot — should stay flat. */
    uint16_t ring_drop;    /**< Acquisition ring overruns since boot — should stay flat. */
    uint32_t uptime_s;     /**< Seconds since the module booted. */
    bool     maint;        /**< Maintenance mode active (STATUS bit 6). */
    uint8_t  group_status; /**< Map 0x36+: per-channel energy store: 0 ok · 1 new · 2 fault · 3 restarted at 0. */
    uint16_t legal_noen;   /**< Metrology-relevant operations executed outside maintenance mode (report-only gate). */
    uint8_t  reset_cause;  /**< Image 0x76+: cause of THIS boot, @c RBAMP_STD_RST_* bits (PIN · POWER · SOFTWARE = reset / activate / address change · IWDG · WWDG · LOW_POWER · OPTION_BYTE); PIN accompanies most internal resets, judge by the other bits. 0 below 0x76. */
    uint16_t boot_count;   /**< Image 0x76+: persisted boot counter (low 16 bits) — a master that missed a restart sees it step. 0 below 0x76. */
} rbamp_std_health_t;

/** @brief Channel selector for ::rbamp_std_latch_wh meaning "all channels". */
#define RBAMP_STD_TOTAL 0xFFu

/**
 * @brief One latched period from an STD latch bank — output of
 *        ::rbamp_std_read_latch.
 *
 * The module keeps 8 independent banks. A bank accumulates while its bit is
 * set in the arm mask (::rbamp_std_arm_banks); ::rbamp_std_latch snapshots
 * AND resets it at the next measurement-window boundary, so a period is the
 * interval between two latches of the same bank, measured by the MASTER's
 * clock (the module has none). Totals are averages over the period; energy is
 * the module's raw signed accumulator in mW·samples, converted with
 * ::rbamp_std_latch_wh. Per-channel average power is derived as
 * (E_import + E_export) / nsamp per the module contract.
 *
 * @note The module's "valid" flag only says the bank has EVER been latched;
 *       it is not a freshness signal. Read a bank at least one measurement
 *       window after latching it — the library's 300 ms settle is a safe
 *       upper bound for every image (the window is 100 ms from image 0x7C,
 *       up to ~160 ms before). @c win_count == 0 means the bank
 *       was not armed or no window elapsed since it was armed.
 */
typedef struct {
    float    p_avg;        /**< Average active power over the period, W (signed). */
    float    q_avg;        /**< Average reactive power, var (signed). */
    float    s_avg;        /**< Average apparent power, VA. */
    float    max_p;        /**< Peak active power in the period, W. */
    float    max_i;        /**< Peak RMS current, A. */
    float    max_u;        /**< Peak RMS voltage, V. */
    float    min_u;        /**< Minimum RMS voltage, V. */
    int64_t  e_import_mws; /**< Import energy over the period, mW·samples (>= 0). */
    int64_t  e_export_mws; /**< Export energy over the period, mW·samples (<= 0). */
    uint32_t nsamp;        /**< Sensor samples in the period. */
    uint32_t win_count;    /**< Measurement windows in the period (0 = not armed / nothing elapsed). */
    uint8_t  channels;     /**< Entries valid in the per-channel arrays (= ::rbamp_channels). */
    uint8_t  bank;         /**< Bank this snapshot came from (0..7). */
    bool     valid;        /**< Module reports the bank has a snapshot (see note). */
    float    p_avg_ch[RBAMP_MAX_CHANNELS];          /**< Per-channel average active power, W. */
    int64_t  e_import_ch_mws[RBAMP_MAX_CHANNELS];   /**< Per-channel import energy, mW·samples. */
    int64_t  e_export_ch_mws[RBAMP_MAX_CHANNELS];   /**< Per-channel export energy, mW·samples. */
} rbamp_std_latch_t;

/**
 * @brief Life-time energy counters of an STD module that keeps them on
 *        device (@c RBAMP_STD_CAP_ONDEVICE_ENERGY) — output of
 *        ::rbamp_std_read_energy.
 *
 * Whole Wh / varh, monotonic, persisted across power loss. Import and export
 * are separate magnitudes; @c act_net_wh = import - export. A period's energy
 * is the difference of two readings (::rbamp_std_energy_delta applies the
 * continuity rules): @c generation changes when the counters were reset or
 * adopted on the module — never take a difference across it; @c uptime_s
 * decreasing means the module restarted — the counters are persisted, so the
 * difference is still good, but the module's clock and RAM settings are
 * gone. After a power cut a counter may resume slightly below your last
 * reading (up to ~1 s of energy): ::rbamp_std_energy_delta clamps such a
 * difference to 0 and the interval is kept.
 *
 * From map 0x36 the same block also carries mWh-resolution totals and
 * per-channel counters (@c channels > 0): channel c = current channel c (on
 * a three-phase module: phase c). @c group_status reports the per-channel
 * store (3 = it restarted at 0 and has not been persisted yet); a restart
 * is visible as a generation change or an uptime drop — the same rules as
 * for the totals apply, nothing extra to do for the per-channel part.
 *
 * The @c *_uwh / @c *_uvarh fields are always filled, so a caller can use
 * them on every module: modules that publish µ-resolution counters (module
 * firmware 0x0001007A and newer, @c RBAMP_STD_CAP_ENERGY_UWH) deliver them
 * from the same snapshot as the coarse ones and set @c fine; on older
 * modules they are the coarse counters scaled (mWh × 1000, or Wh × 10^6
 * below map 0x36; varh × 10^6) and @c fine is false. Reactive µvarh comes
 * from the module from firmware 0x0001007B (@c fine_reactive); before that it
 * is varh × 10^6. mWh = floor(µWh / 1000) per import/export counter; net is
 * import − export at every resolution (so net mWh can differ from
 * floor(net µWh / 1000) by one when net is negative). After a module
 * restart a µ counter resumes from the last saved value — up to about one
 * second of energy below your last reading; ::rbamp_std_energy_delta clamps
 * that to 0 like the coarse counters.
 */
typedef struct {
    int64_t  act_import_wh;   /**< Active energy imported, Wh (>= 0). */
    int64_t  act_export_wh;   /**< Active energy exported, Wh, magnitude (>= 0). */
    int64_t  act_net_wh;      /**< import - export, Wh (signed). */
    int64_t  rea_import_varh; /**< Reactive energy imported, varh. */
    int64_t  rea_export_varh; /**< Reactive energy exported, varh, magnitude. */
    uint32_t generation;      /**< Life-time generation counter (@c RBAMP_STD_REG_LT_GEN). */
    uint32_t uptime_s;        /**< Seconds since the module booted. */
    int64_t  act_import_mwh;  /**< Map 0x36+: imported, mWh (0 below). */
    int64_t  act_export_mwh;  /**< Map 0x36+: exported, mWh, magnitude (0 below). */
    int64_t  act_net_mwh;     /**< Map 0x36+: import - export, mWh (0 below). */
    uint8_t  channels;        /**< Map 0x36+: entries valid in the per-channel arrays (0 below, or when the per-channel store is unusable). */
    uint8_t  group_status;    /**< Map 0x36+: per-channel store: 0 ok · 1 new · 2 fault · 3 restarted at 0. */
    int64_t  ch_import_mwh[RBAMP_MAX_CHANNELS]; /**< Map 0x36+: per-channel imported, mWh. */
    int64_t  ch_export_mwh[RBAMP_MAX_CHANNELS]; /**< Map 0x36+: per-channel exported, mWh, magnitude. */
    bool     fine;            /**< Active µWh fields (grand and per channel) come from the module; false = scaled from the coarse counters. */
    bool     fine_reactive;   /**< Reactive µvarh fields come from the module (firmware 0x0001007B+); false = varh × 10^6. */
    int64_t  act_import_uwh;  /**< Active energy imported, µWh (>= 0). */
    int64_t  act_export_uwh;  /**< Active energy exported, µWh, magnitude (>= 0). */
    int64_t  act_net_uwh;     /**< import − export, µWh (signed). */
    int64_t  rea_import_uvarh;/**< Reactive energy imported, µvarh. */
    int64_t  rea_export_uvarh;/**< Reactive energy exported, µvarh, magnitude. */
    int64_t  ch_import_uwh[RBAMP_MAX_CHANNELS]; /**< Per-channel imported, µWh (valid for @c channels entries). */
    int64_t  ch_export_uwh[RBAMP_MAX_CHANNELS]; /**< Per-channel exported, µWh, magnitude. */
} rbamp_std_energy_t;

/** @brief STD configuration block — output of ::rbamp_std_read_config. */
typedef struct {
    uint8_t  sensor_class; /**< ::rbamp_sensor_class_t value; 0 = unset. Persisted. */
    uint8_t  ct_model;     /**< CT model reference for channel 0 (metadata, sets no gain). Persisted. */
    uint8_t  tier;         /**< 0 = regular, 1 = industrial. Persisted. */
    uint8_t  i2c_addr;     /**< Persisted 7-bit address (applies on reset). */
    uint16_t rate_ms;      /**< Report-rate hint, ms (RAM only). */
    bool     harmonics;    /**< Base-harmonics extraction enabled (RAM only; see ::rbamp_std_set_harmonics). */
    uint16_t start_ua;     /**< Starting current for energy, µA (0 = off); 0 when @c start_ua_supported is false. Persisted. See ::rbamp_std_set_start_current. */
    bool     start_ua_supported; /**< The module has the starting-current setting (firmware 0x0001007B+). */
} rbamp_std_config_t;

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
    RBAMP_CAP_SEAL            = RBAMP_V2_CAP_SEAL,            /**< Tamper seal — reserved; not implemented in v1.4.x firmware (no seal command handler, no boot-time verification). */
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
    RBAMP_EVENT_NVM_FAULT      = RBAMP_STD_EVT_NVM_FAULT,       /**< STD only: energy store fault. */
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
 * All fields are SI units. Refreshed by the device firmware every 100 ms
 * (BASIC images before v1.2 and STD images up to 0x7B: 200 ms).
 * Unused channels (per ::rbamp_channels) are zeroed.
 *
 * A single out-of-bound field no longer fails the whole snapshot (bench
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
    float current[RBAMP_SNAPSHOT_CHANNELS];       /**< RMS current per channel, A. NaN if implausible. */
    float current_peak[RBAMP_SNAPSHOT_CHANNELS];  /**< Peak current per channel, A. NaN if implausible. */
    float power[RBAMP_SNAPSHOT_CHANNELS];         /**< Real power per channel, W (signed). NaN if implausible. */
    float power_factor[RBAMP_SNAPSHOT_CHANNELS];  /**< Power factor per channel (-1..+1). NaN if implausible. */
    float frequency;        /**< REG_AC_FREQ, Hz. NaN if implausible. */
    rbamp_topology_t topology; /**< Variant. */
    uint8_t channels;       /**< The module's channel count (1, 2, 3, 5 or 7). The arrays above hold ch0-2 only (::RBAMP_SNAPSHOT_CHANNELS); read ch3+ with rbamp_read_channel. */
    bool has_voltage_hw;
    uint8_t implausible;    /**< Bitmask of @c RBAMP_FIELD_* classes set to NaN. 0 = all OK. */
} rbamp_snapshot_t;

/**
 * @brief Single-channel real-time reading — output of ::rbamp_read_channel.
 *
 * Any channel (0..::rbamp_channels-1). For channels 0-2 the four fields come
 * from the flat RT block; for channels 3+ (senior UI5/UI7) they come from the
 * channel access window, re-selected per field. A field that trips its sanity
 * bound is set to @c NaN and its @c RBAMP_FIELD_* bit raised in @c implausible,
 * exactly like ::rbamp_snapshot_t. These are LIVE instantaneous values; a sweep
 * across channels reads adjacent publish windows and is NOT a simultaneous
 * snapshot (for aligned energy across channels use the period path).
 */
typedef struct {
    float i_rms;         /**< RMS current, A. NaN if implausible. */
    float i_peak;        /**< Peak current, A. NaN if implausible. */
    float p_real;        /**< Real power, W (signed). NaN if implausible. */
    float pf;            /**< Power factor (-1..+1). NaN if implausible. */
    uint8_t implausible; /**< Bitmask of @c RBAMP_FIELD_* classes set to NaN. 0 = all OK. */
} rbamp_channel_reading_t;

/**
 * @brief Period-metering snapshot — output of ::rbamp_read_period_snapshot.
 *
 * Energy integrates over @c master_dt_ms — the master's own wall-clock window
 * between successive consumed reads (esp_timer). @c latch_ms is diagnostic
 * only: it may read short under load; the energy window is @c master_dt_ms
 * or @c module_window_ms.
 *
 * @c module_window_ms is the module's own measure of the latched window
 * (BASIC: commits counted on the module's measurement clock, 100 ms each on
 * current firmware and 200 ms on pre-v1.2 firmware — the library infers which
 * from the same snapshot and reports it in @c module_commit_ms; 0 on STD).
 * Normally it cross-checks @c master_dt_ms. After ::rbamp_begin_resume the
 * first consumed snapshot integrates over it instead (@c master_dt_ms is set
 * equal to it), which recovers the energy accumulated while the master was
 * off. @c window_from_boot then says whether that window started at the
 * module's boot (a module restart, or the first latch since boot).
 *
 * @note @c avg_p exposes channels 0-2 only. On senior UI5/UI7 SKUs the device
 *       latches avg power for ALL channels atomically; channels 3+ are read via
 *       ::rbamp_read_period_avg_power and their Wh via ::rbamp_energy_wh (the
 *       accumulator integrates every channel). @c max_p stays a global (ch0).
 */
typedef struct {
    float avg_p[3];      /**< Average real power over the latched period, ch0-2, W. */
    float max_p;         /**< Peak real power on channel 0 during the period, W. */
    uint32_t latch_ms;   /**< Chip-reported period duration, ms — diagnostic only (may read short under load). */
    uint32_t master_dt_ms; /**< Master wall-clock window since the previous consumed read, ms — the energy dt. */
    bool valid;          /**< true if REG_V03_PERIOD_VALID bit0 == 1 at read time. */
    float avg_p_total;   /**< Average real power over the period summed over ALL channels, W (signed). */
    double wh_net;       /**< Net energy of this period as the library accounted it, Wh (signed): avg power x dt, or the module's own counter difference on modules that keep energy on device. */
    bool module_restarted; /**< The module rebooted inside this period (counter-based STD modules; BASIC: the first snapshot after ::rbamp_begin_resume when the window started at the module's boot). Its clock and RAM settings need re-applying. */
    uint32_t module_window_ms; /**< The module's own length of the latched window, ms (BASIC: commit count x module_commit_ms; 0 on STD). */
    uint16_t module_commit_ms; /**< Length of one module commit as inferred for this snapshot: 100 (current firmware) or 200 (pre-v1.2); 0 on STD. */
    uint32_t module_uptime_s;  /**< Module seconds since boot, read with the snapshot; 0 when the firmware does not publish it. */
    bool window_from_boot;     /**< The latched window started at the module's boot (module_window_ms >= uptime - 1.5 s). Always false when module_uptime_s is 0. */
    bool window_long;          /**< module_window_ms exceeds 6 h — the module's average loses precision over very long windows (about 0.6 % at 6 h, 2.6 % at 24 h). */
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
 * The channel count and voltage presence are taken from the module in
 * ::rbamp_begin (HW_VARIANT 0x55): a UI1 reports 1 channel, a UI3 3, and so
 * on. Only when the module does not publish its variant (pre-v1.3 firmware
 * reads 0) does the handle fall back to ::RBAMP_TOPOLOGY_THREE_PHASE; see
 * SPEC §8 for why over-detection is harmless there. Use
 * ::rbamp_new_with_topology to pin a count regardless of what the module says.
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
 * Pins the channel count: ::rbamp_begin keeps it even when the module reports
 * a different one (a warning is logged). Variant and voltage presence still
 * follow the module. Needed only for pre-v1.3 firmware that does not publish
 * HW_VARIANT, or to deliberately expose fewer/more channels than the module has.
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
 * @brief ::rbamp_begin without the primer LATCH — keep the window the module
 *        accumulated while the master was away.
 *
 * A BASIC module keeps averaging until the next LATCH, so after a master
 * reboot or outage the module still holds the whole window since the last
 * consumed snapshot. ::rbamp_begin would latch and discard it; this call does
 * not. The first consumed ::rbamp_read_period_snapshot after it integrates
 * over the module's own window (@c module_window_ms, see
 * ::rbamp_period_snapshot_t): @c wh_net = avg_p_total x module_window_ms,
 * @c master_dt_ms = module_window_ms, @c module_restarted = window_from_boot.
 * Every later snapshot uses the master clock as usual. A stale first read
 * (PERIOD_VALID 0, empty accumulator) is dropped like any other and the next
 * valid read still uses the module window.
 *
 * On firmware without the uptime register @c window_from_boot and
 * @c module_restarted stay false: the recovered window may then include a
 * module restart that the library cannot see. STD modules: identical to
 * ::rbamp_begin (their energy lives in counters on the module).
 */
esp_err_t rbamp_begin_resume(rbamp_handle_t dev);

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

/** @return Topology — from the module after ::rbamp_begin, or caller-pinned via
 *  ::rbamp_new_with_topology; THREE_PHASE before begin / on firmware without HW_VARIANT. */
rbamp_topology_t rbamp_topology(rbamp_handle_t dev);

/** @return Number of valid current channels (1..3 junior, 5/7 senior UI5/UI7). */
uint8_t rbamp_channels(rbamp_handle_t dev);

/** @return Module variant cached by ::rbamp_begin (BASIC: HW_VARIANT 0x55; STD:
 *  VARIANT register). ::RBAMP_VARIANT_UNKNOWN before begin or when the module
 *  does not publish it. No bus traffic — ::rbamp_read_variant re-reads the register. */
rbamp_variant_t rbamp_variant(rbamp_handle_t dev);

/** @return true if voltage sensing hardware was detected. */
bool rbamp_has_voltage_hw(rbamp_handle_t dev);

/** @return Current 7-bit slave address (updates after ::rbamp_commit_address_change). */
uint8_t rbamp_address(rbamp_handle_t dev);

/**
 * @return Product line detected by ::rbamp_begin (::RBAMP_LINE_BASIC until it
 *         runs). See ::rbamp_line_t for what differs between the lines.
 */
rbamp_line_t rbamp_line(rbamp_handle_t dev);

/* -------------------------------------------------------------------------
 * Real-time reads (SPEC §12 — RT block, refreshed on device every 100 ms;
 * (BASIC images before v1.2 and STD images up to 0x7B: 200 ms))
 * ------------------------------------------------------------------------- */

/** @brief Read RMS voltage (REG_V03_U_RMS, 0x86) in V. */
esp_err_t rbamp_read_voltage(rbamp_handle_t dev, uint8_t phase, float *out);

/**
 * @brief Read peak voltage (REG_V03_U_PEAK, 0x8A) in V.
 * @par STD line
 * The live peak register is not published on module map 0x34 and earlier
 * (reads 0.0). For the peak over a period use a latch bank
 * (::rbamp_std_read_latch → @c max_u / @c min_u).
 */
esp_err_t rbamp_read_voltage_peak(rbamp_handle_t dev, uint8_t phase, float *out);

/* Per-channel readers below accept @c ch 0..::rbamp_channels-1. Channels 0-2
 * come from the flat RT block; channels 3+ (senior UI5/UI7) are fetched
 * transparently through the channel access window (::rbamp_read_channel). */

/** @brief Read RMS current for one channel (REG_V03_I*_RMS / window) in A. */
esp_err_t rbamp_read_current(rbamp_handle_t dev, uint8_t ch, float *out);

/**
 * @brief Read peak current for one channel (REG_V03_I*_PEAK / window) in A.
 * @par STD line
 * The live peak register is not published on module map 0x34 and earlier
 * (reads 0.0). For the peak over a period use a latch bank
 * (::rbamp_std_read_latch → @c max_i).
 */
esp_err_t rbamp_read_current_peak(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read real power for one channel (REG_V03_P*_REAL / window) in W (signed). */
esp_err_t rbamp_read_power(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read power factor for one channel (REG_V03_PF* / window) in -1..+1. */
esp_err_t rbamp_read_power_factor(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read mains frequency (REG_AC_FREQ, 0x20) in Hz.
 * @note STD line: the module publishes 0 when it sees no mains; that is not a
 *       frequency — the call returns @c ESP_ERR_INVALID_RESPONSE with @p *out
 *       = NaN (::rbamp_read_all sets the FREQUENCY implausible bit instead). */
esp_err_t rbamp_read_frequency(rbamp_handle_t dev, float *out);

/**
 * @brief Read reactive power of channel @p ch in var (signed).
 * @return @c ESP_ERR_NOT_SUPPORTED on the BASIC line (no reactive register).
 */
esp_err_t rbamp_read_reactive_power(rbamp_handle_t dev, uint8_t ch, float *out);

/**
 * @brief Read apparent power of channel @p ch in VA.
 * @return @c ESP_ERR_NOT_SUPPORTED on the BASIC line (no apparent register).
 */
esp_err_t rbamp_read_apparent_power(rbamp_handle_t dev, uint8_t ch, float *out);

/**
 * @brief One-shot read of the full RT block into a snapshot struct.
 *
 * Unused channels (per ::rbamp_channels) are filled with 0. On a module with
 * more than ::RBAMP_SNAPSHOT_CHANNELS channels (UI5/UI7) only ch0-2 are read
 * into the struct; ch3+ are not touched by this call — read them with
 * ::rbamp_read_channel / ::rbamp_read_all_channels. A field that trips
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
 * Channel access window (senior UI5/UI7 SKUs)
 * -------------------------------------------------------------------------
 * Channels 0-2 live in the flat RT block; channels 3+ are reachable ONLY
 * through the window (register 0x32 select → 0x3C-0x3F data). The window data
 * buffer is a snapshot taken at select time, so the driver RE-SELECTS before
 * every field read (device Rule #1). select+read is one atomic section: the
 * selector is a single shared device resource, so do not share one handle
 * across tasks without your own mutex. All ::rbamp_read_current /
 * _current_peak / _power / _power_factor accessors route ch3+ here
 * automatically; the calls below fetch a whole channel or full fleet at once.
 */

/**
 * @brief Read all four RT fields of one channel into a struct.
 *
 * @param[in]  dev Handle.
 * @param[in]  ch  Channel 0..::rbamp_channels-1.
 * @param[out] out Reading; per-field NaN + @c implausible bits on sanity reject.
 * @return @c ESP_OK (check @c implausible), @c ESP_ERR_INVALID_ARG on a bad
 *         channel, or the first underlying transport error.
 */
esp_err_t rbamp_read_channel(rbamp_handle_t dev, uint8_t ch,
                             rbamp_channel_reading_t *out);

/**
 * @brief Read every populated channel into a caller array.
 *
 * @param[in]  dev Handle.
 * @param[out] out Array of at least @p n entries (size with ::RBAMP_MAX_CHANNELS).
 * @param[in]  n   Capacity of @p out; filled with @c min(n, ::rbamp_channels).
 * @return @c ESP_OK, @c ESP_ERR_INVALID_ARG, or the first transport error.
 */
esp_err_t rbamp_read_all_channels(rbamp_handle_t dev,
                                  rbamp_channel_reading_t *out, uint8_t n);

/**
 * @brief Read the digest sequence byte (DIGEST offset 1, 0x71).
 *
 * A @c uint8_t that increments once per measurement window — every 100 ms on
 * current firmware, 200 ms on images before v1.2 (wraps 255→0).
 * Read it before and after a multi-register sweep: an unchanged value proves
 * the reads came from one commit. The digest carries NO CRC.
 *
 * @param[in]  dev Handle.
 * @param[out] out Sequence byte.
 * @return @c ESP_OK or a transport error.
 */
esp_err_t rbamp_read_commit_seq(rbamp_handle_t dev, uint8_t *out);

/* -------------------------------------------------------------------------
 * Data-ready (DRDY) — the family's "new snapshot" signal
 *
 * Every module drives an open-drain DRDY output: idle HIGH (master pull-up),
 * a ~10 us LOW pulse each time a complete measurement snapshot is published
 * (every 100 ms across the family (BASIC images before v1.2 and STD images up to 0x7B: 200 ms)),
 * wired-OR across modules on one line. BASIC modules also
 * hold the line solid LOW while an unmasked event is pending (alarm). The
 * library turns the pulse into a wake-up so a reader never reads the same
 * snapshot twice nor straddles a publish, and reports missed snapshots. On
 * STD modules the publish counter (WIN_SEQ) says which module pulsed on a
 * shared line and how many snapshots were missed. Without a DRDY wire the
 * same calls poll the counters (CONFIG_RBAMP_DRDY_POLL_MS).
 *
 * STD modules before image 0x00010075 drive the pin differently: the
 * library treats them as having no DRDY line and polls.
 * ------------------------------------------------------------------------- */

/** @brief What ::rbamp_wait_data_ready saw. */
typedef struct {
    bool     fresh;        /**< A new snapshot of THIS module is available (always true on ESP_OK). */
    bool     by_edge;      /**< Woken by the DRDY edge (false = found by polling). */
    uint16_t seq;          /**< Snapshot sequence now (STD: WIN_SEQ; BASIC: commit sequence). */
    uint16_t missed;       /**< Snapshots published since the previous call that were not read (0 = none). */
    bool     alarm;        /**< The line was held LOW (masked event pending on this module); @c event_flags holds what was read and cleared. */
    uint8_t  event_flags;  /**< EVENT_FLAGS consumed from this module on an alarm (@c RBAMP_EVENT_*), else 0. */
    uint32_t latency_us;   /**< Edge to return, us (0 when polling). */
} rbamp_drdy_event_t;

/**
 * @brief Attach (or detach with -1) the DRDY line of this module. The GPIO is
 *        configured as input with pull-up and a falling-edge interrupt; any
 *        number of handles may share one GPIO (wired-OR line). Call after
 *        ::rbamp_begin (the line's electrical contract depends on the
 *        detected module).
 * @return @c ESP_ERR_NO_MEM if more than 4 distinct lines or 8 handles per
 *         line are in use.
 */
esp_err_t rbamp_set_drdy_gpio(rbamp_handle_t dev, int gpio);

/**
 * @brief Block until the module has published a snapshot newer than the
 *        last one this call returned (or the last read through
 *        ::rbamp_read_all_ready), at most @p timeout_ms.
 *
 * With a DRDY line: wakes on the falling edge, then verifies the module's
 * sequence advanced (a pulse on a shared line may belong to another module
 * — the call keeps waiting). If the line is still LOW 5 ms after the edge
 * a masked event is pending on some module of the line: the flags of every
 * module attached to it are read and cleared (write-1-to-clear, which
 * releases the line — a held line would hide the other modules' pulses);
 * this module's flags come back in @p ev with @c alarm set, the other
 * modules' flags are handed to their own handles and reported by their
 * next wait. Without a line: polls the sequence every
 * @c CONFIG_RBAMP_DRDY_POLL_MS.
 *
 * @param[out] ev  Details (may be NULL).
 * @return @c ESP_OK (fresh snapshot); @c ESP_ERR_TIMEOUT; @c ESP_ERR_NOT_SUPPORTED
 *         on STD modules without a publish counter (map < 0x36).
 */
esp_err_t rbamp_wait_data_ready(rbamp_handle_t dev, uint32_t timeout_ms, rbamp_drdy_event_t *ev);

/**
 * @brief ::rbamp_wait_data_ready followed by ::rbamp_read_all: one coherent
 *        read per published snapshot, never the same snapshot twice.
 */
esp_err_t rbamp_read_all_ready(rbamp_handle_t dev, uint32_t timeout_ms,
                               rbamp_snapshot_t *out, rbamp_drdy_event_t *ev);

/** @brief Current snapshot sequence without waiting (STD: WIN_SEQ; BASIC: commit sequence). */
esp_err_t rbamp_read_data_seq(rbamp_handle_t dev, uint16_t *out);

/* -------------------------------------------------------------------------
 * BASIC digest — one transaction per published window
 * ------------------------------------------------------------------------- */

/** @brief Fields a BASIC module packs into its digest (::rbamp_set_digest_fields). */
typedef enum {
    RBAMP_DIGEST_I  = RBAMP_V2_DIGEST_I_RMS, /**< RMS current per channel, 0.01 A steps. */
    RBAMP_DIGEST_U  = RBAMP_V2_DIGEST_U_RMS, /**< RMS voltage, 0.1 V steps (modules with voltage sensing only). */
    RBAMP_DIGEST_P  = RBAMP_V2_DIGEST_P_REAL,/**< Real power per channel, whole watts, signed (|P| below 1 W reads 0). */
    RBAMP_DIGEST_PF = RBAMP_V2_DIGEST_PF,    /**< |power factor| per channel, 0.01 steps. */
} rbamp_digest_field_t;

/**
 * @brief One published window of a BASIC module, read in a single transaction
 *        (::rbamp_read_digest). Only the first three channels; the module packs
 *        the digest together with the window result, so all fields belong to
 *        the window numbered @c seq.
 */
typedef struct {
    uint8_t  status;      /**< STATUS mirror at the moment of the publish. */
    uint8_t  seq;         /**< Window sequence (one step per published window, wraps 255→0). */
    uint8_t  n_ch;        /**< Channels present in the digest: min(module channels, 3). */
    uint8_t  fields;      /**< Which ::rbamp_digest_field_t bits were read. */
    float    i_a[3];      /**< RMS current, A (0.01 A resolution); 0 when the field is off. */
    float    u_v;         /**< RMS voltage, V (0.1 V resolution); 0 when the field is off or the module has no voltage sensing. */
    int16_t  p_w[3];      /**< Real power, W, signed, truncated toward 0 by the module (|P| < 1 W reads 0); 0 when off. */
    float    pf[3];       /**< |Power factor| 0..1 (0.01 resolution); 0 when off. */
} rbamp_digest_t;

/**
 * @brief Choose the fields a BASIC module packs into its digest.
 *
 * Default when never called: current + real power, plus voltage on modules
 * with voltage sensing (::rbamp_has_voltage_hw). The choice is written to the
 * module on the next ::rbamp_read_digest (or now, when the handle is already
 * begun on a BASIC module with the digest capability); the module keeps it in
 * RAM only, so the library writes it again after a detected module restart
 * (a cleared @c RBAMP_EVENT_RESET_OCCURRED, a pending RESET event seen in the
 * digest status, or a decreasing uptime in ::rbamp_read_period_snapshot).
 *
 * @param[in] fields OR of ::rbamp_digest_field_t, not 0.
 * @return @c ESP_ERR_INVALID_ARG for 0 or unknown bits, or when the module
 *         rejects a field it does not have (voltage on a current-only module);
 *         @c ESP_ERR_NOT_SUPPORTED on STD or without the digest capability.
 */
esp_err_t rbamp_set_digest_fields(rbamp_handle_t dev, uint8_t fields);

/**
 * @brief Read the digest block of a BASIC module: status, window sequence and
 *        the chosen fields for channels 0-2 in ONE I²C transaction.
 *
 * Meant for reading every published window (100 ms on current firmware) on
 * DRDY: ::rbamp_wait_data_ready then ::rbamp_read_digest gives the signed real
 * power of each window for a few bus bytes, where ::rbamp_read_all costs one
 * transaction per field. Senior channels 3+ are not in the digest (use
 * ::rbamp_read_channel). Real power is whole watts: a load below 1 W reads 0.
 *
 * @return @c ESP_OK; @c ESP_ERR_NOT_SUPPORTED on STD modules or BASIC firmware
 *         without the digest capability (@c RBAMP_CAP_DIGEST); transport errors.
 */
esp_err_t rbamp_read_digest(rbamp_handle_t dev, rbamp_digest_t *out);

/**
 * @brief Human firmware name for a BASIC @c REG_VERSION byte
 *        (::rbamp_firmware_version). Pure lookup, no bus traffic.
 *
 * 0x0B → "1.4.19", 0x0A → "1.4.18", 0x07 → "1.4.15-1.4.17",
 * 0x05 → "1.4.4-1.4.14", 0x04 → "1.2-1.4.3", 0x00..0x03 → "<1.2";
 * any other byte → NULL (unknown / not a BASIC module).
 */
const char *rbamp_basic_fw_name(uint8_t version);

/* -------------------------------------------------------------------------
 * Period metering (SPEC §7)
 * ------------------------------------------------------------------------- */

/** @brief Issue @c CMD_LATCH_PERIOD (write 0x27 to REG_COMMAND). Does not wait.
 *  STD: latches bank 0; a no-op (@c ESP_OK) on modules that meter with
 *  counters instead of latch banks. */
esp_err_t rbamp_latch_period(rbamp_handle_t dev);

/** @brief Read @c REG_V03_PERIOD_VALID (0x07) bit 0.
 *  STD counter modules: true once the library holds a counter anchor (always after ::rbamp_begin). */
esp_err_t rbamp_is_period_valid(rbamp_handle_t dev, bool *out);

/** @brief Read latched average power for one channel (W). ch0-2 flat (non-contiguous
 *         registers); ch3+ via the channel window (period field).
 *  STD counter modules: the last consumed period's average for single-channel
 *  modules; @c ESP_ERR_NOT_SUPPORTED on multi-channel ones (no per-channel counters). */
esp_err_t rbamp_read_period_avg_power(rbamp_handle_t dev, uint8_t ch, float *out);

/** @brief Read @c REG_V03_PERIOD_MAX_P_F0 (0xE0) — ch0 peak power this period (W).
 *  STD counter modules: @c ESP_ERR_NOT_SUPPORTED (no period envelope). */
esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out);

/** @brief Read @c REG_V03_PERIOD_LATCH_MS (0xEC) — device-reported period duration (diagnostic). */
esp_err_t rbamp_read_period_latch_ms(rbamp_handle_t dev, uint32_t *out);

/**
 * @brief One-shot period snapshot: latch, settle, valid-check, read, integrate.
 *
 * Recommended entry point for period metering. Sequence:
 * 1. Write @c CMD_LATCH_PERIOD (skipped if @c skip_latch is true).
 * 2. Capture the master wall-clock and compute @c master_dt_ms since the
 *    previous consumed read (the energy window — esp_timer; the
 *    chip's 0xEC is diagnostic only).
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
 * @par STD line
 * Uses latch bank 0 (armed by ::rbamp_begin). The module defers the latch to
 * its next measurement-window boundary, so the library waits at least 300 ms
 * regardless of a smaller @p settle_ms (a safe upper bound for every image:
 * the window is 100 ms from image 0x7C, up to ~160 ms before); with
 * @p skip_latch the caller owns that timing (latch, wait >= 300 ms, then
 * read). @c avg_p per channel is
 * the period's signed energy divided by its sample count; @c max_p is the
 * bank's peak; @c latch_ms is 0 (the module has no clock — @c master_dt_ms is
 * the period). Returns @c ESP_ERR_INVALID_RESPONSE if bank 0 reports no
 * elapsed window; if that is because a module reset cleared the (RAM) arm
 * mask, the library re-arms bank 0, primer-latches and restarts the master
 * clock anchor — that one window is not integrated, the next is whole again.
 * Keep bit 0 set in ::rbamp_std_arm_banks.
 *
 * @par STD line without latch banks (period_source == RBAMP_STD_PERIOD_COUNTERS)
 * No latch is sent and @p settle_ms / @p skip_latch are ignored: the call
 * reads the module's persisted life-time counters and reports the difference
 * since the previous consumed read. @c avg_p_total and @c wh_net carry the
 * period (Wh counters before map 0x36: choose a period long enough that one Wh is a
 * small fraction of it — minutes, not seconds); @c avg_p[] holds the same
 * value for a single-channel module, the per-channel counter differences
 * from map 0x36 (mWh resolution), and @c NaN on multi-channel modules below
 * map 0x36 (no per-channel counters there); @c max_p is @c NaN. @c module_restarted flags a reboot inside the period — the
 * counters survive it, the difference is kept. Returns
 * @c ESP_ERR_INVALID_RESPONSE and re-anchors (that one period is dropped)
 * when the module's generation counter changed (its life-time counters were
 * reset or adopted) or a counter went backwards. @c ESP_ERR_NOT_SUPPORTED
 * if the module offers neither banks nor counters.
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
 * @brief Seconds since the module booted (BASIC: UPTIME_S 0x46; STD: the
 *        energy block's UPTIME). Reads 0 on BASIC firmware that does not
 *        publish it.
 */
esp_err_t rbamp_read_uptime_s(rbamp_handle_t dev, uint32_t *out);

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

/** Channel selector meaning "all channels" for ::rbamp_energy_wh / ::rbamp_energy_reset. */
#ifndef RBAMP_ENERGY_CH_TOTAL
#define RBAMP_ENERGY_CH_TOTAL 0xFFu
#endif

#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
/**
 * @brief Read the running total Wh for one channel, or for all channels with
 *        @c ch == ::RBAMP_ENERGY_CH_TOTAL.
 *
 * Updated automatically by ::rbamp_read_period_snapshot. On STD modules that
 * keep energy on device (no latch banks) the total is the sum of the module's
 * own counter differences — exact, nothing integrated on the host — and the
 * per-channel values come from the module's per-channel counters on map 0x36+,
 * are @c NaN on multi-channel modules below that (no per-channel counters),
 * and equal the total on single-channel modules.
 * To opt out per-handle
 * call ::rbamp_energy_disable. To remove the accumulator from the build
 * entirely, enable @c CONFIG_RBAMP_DISABLE_ENERGY in menuconfig — this also
 * removes the ::rbamp_energy_* prototypes from this header (linker would
 * otherwise emit unresolved-symbol errors).
 *
 * @param[in] dev Handle.
 * @param[in] ch  Channel index 0..::rbamp_channels-1 (up to 6 on UI7).
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
 * @brief Read the module's current sensor class (::rbamp_sensor_class_t value).
 *
 * BASIC: @c REG_SENSOR_CLASS (0x25). STD: the module's class config field.
 */
esp_err_t rbamp_read_sensor_class(rbamp_handle_t dev, uint8_t *out);

/**
 * @brief Current limits of a sensor model, from the generated model registry.
 *
 * Pure lookup by (class, model code) — no bus traffic. Any output may be NULL.
 *   - @p nominal_a  rated RMS current, A;
 *   - @p fs_a       RMS current at which a sine wave starts to clip, A;
 *   - @p peak_a     instantaneous clip ceiling, A — a distorted load clips at
 *                   @c peak_a / crest factor.
 * A value of 0 means not known for that model (legacy toroid / burden codes).
 *
 * @return true if the (class, code) pair is in the registry, false otherwise
 *         (outputs are then 0).
 */
bool rbamp_sensor_model_limits(uint8_t sensor_class, uint8_t code,
                               float *nominal_a, float *fs_a, float *peak_a);

/**
 * @brief Rated RMS current of a sensor model, A (0 = not known).
 *
 * Shorthand for the @c nominal_a output of ::rbamp_sensor_model_limits. This
 * is the sensor's rating, which can differ from the amps in the model's name
 * (e.g. the "5 A" clamp is rated 4.6 A) — use ::rbamp_sensor_model_lookup for
 * the name.
 */
float rbamp_sensor_model_rated_a(uint8_t sensor_class, uint8_t code);

/* -------------------------------------------------------------------------
 * SCT-013 rating switch — correction relative to the gain the module applies
 * (BASIC firmware up to 1.4.19, sensor class 1)
 * ------------------------------------------------------------------------- */

/**
 * @brief Outcome of the SCT-013 rating correction for one channel.
 *
 * A BASIC module of sensor class 1 is calibrated at the factory with an
 * SCT-013-030 (model 3). On firmware up to 1.4.19 a switch to another model
 * leaves the module applying the 030 calibration after a restart, so a 100 A
 * sensor would read about a third of the true current. The library reads the
 * gain the module actually applies at ::rbamp_begin and after every detected
 * module restart and scales current, power, period power, energy and the
 * digest by preset(model) x c(model) / preset(030) — see
 * libs/spec/SCT_RATING_SWITCH.md. Voltage and power factor are not scaled.
 * A module that reports CAPABILITY bit 12 (CAP_SCT_RATING_SELF, firmware
 * 1.4.20 and later) corrects itself; the library then applies no scale and
 * a model write needs no restart. @c CONFIG_RBAMP_SCT_RATING_CORRECTION turns
 * the mechanism off.
 */
typedef enum {
    RBAMP_SCT_NONE = 0,         /**< No correction: STD line, class 2/3, no model bound, model 3, module with CAP_SCT_RATING_SELF, or disabled by Kconfig. Scale 1. */
    RBAMP_SCT_CORRECTED,        /**< Scale applied to this channel's current/power readings. */
    RBAMP_SCT_UNCALIBRATED,     /**< After a reset the module still applies the raw preset: the unit trim is unknown. Scale 1, warning logged. */
    RBAMP_SCT_GAIN_UNEXPECTED,  /**< The applied gain is more than 5 % off the 030 preset: not a 030-calibrated module. Scale 1, warning logged. */
    RBAMP_SCT_UNKNOWN_MODEL,    /**< Model code without a known preset. Scale 1, warning logged. */
    RBAMP_SCT_PENDING,          /**< Not evaluated yet (before ::rbamp_begin). Scale 1. */
} rbamp_sct_status_t;

typedef struct {
    rbamp_sct_status_t status;
    uint8_t model;         /**< CT model the module reports for the channel. */
    float   scale;         /**< Factor applied to current, power, energy and digest (1.0 unless CORRECTED). */
    float   gain_applied;  /**< Gain the module applies (0 when not read). */
} rbamp_sct_correction_t;

/**
 * @brief The cached rating correction of channel @p ch — no bus traffic.
 *
 * Evaluated by ::rbamp_begin, by ::rbamp_set_ct_model* and after a detected
 * module restart (RESET_OCCURRED acknowledged, RESET pending in the digest,
 * uptime decrease). @c ESP_ERR_NOT_SUPPORTED on the STD line.
 */
esp_err_t rbamp_sct_correction(rbamp_handle_t dev, uint8_t ch, rbamp_sct_correction_t *out);

/**
 * @brief Re-read class, models and applied gains now and recompute the scales.
 *
 * Issues one CMD_RESET (and re-probes the module, same UID) when a channel's
 * applied gain is still the raw preset of a model other than 030, i.e. the
 * model was switched in this boot without a restart. Called internally where
 * needed; exposed for a host that knows the module restarted by other means.
 * On the STD line and on a module with CAP_SCT_RATING_SELF it is a no-op returning @c ESP_OK.
 */
esp_err_t rbamp_sct_refresh(rbamp_handle_t dev);

/**
 * @brief Set the sensor class and persist it.
 *
 * Reads the current class first. Writing the value the module already holds
 * is a no-op with no bus write — on the module ANY class write, even an
 * unchanged one, clears every channel's CT model and its calibration shape.
 * A module whose class is 2 or 3 (calibrated together with its fitted sensor)
 * is not rewritten: @c ESP_ERR_INVALID_STATE.
 *
 * Otherwise writes @c REG_SENSOR_CLASS (0x25), verifies the device accepted
 * it, and persists with @c CMD_SAVE_USER_CONFIG (~700 ms). Setting the class
 * also clears the CT models device-side, so set the models afterwards.
 *
 * On v1.2+ firmware (@c rbamp_firmware_version >= 0x03) a set class is a
 * precondition for ::rbamp_set_ct_model and ::rbamp_set_ct_model_ch — calling
 * either with @c REG_SENSOR_CLASS still @c RBAMP_SENSOR_UNSET returns
 * @c ESP_ERR_INVALID_STATE. On v1.0 / v1.1 firmware the register has no
 * functional effect.
 *
 * @param[in] dev Handle.
 * @param[in] cls Sensor class.
 *
 * @return
 *   - @c ESP_OK on success, or when the module already has this class
 *   - @c ESP_ERR_INVALID_ARG on null @c dev, unrecognised @c cls, or device
 *     ERR_PARAM rejection of the write
 *   - @c ESP_ERR_INVALID_STATE when the module's current class is 2 or 3
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 *
 * @par STD line
 * Writes @c CFG_CLASS (metadata, no preset table) and persists with
 * @c CMD_SAVE_CONFIG; the module's command verdict is checked (map 0x34+).
 */
esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev, rbamp_sensor_class_t cls);

/**
 * @brief Set the CT model on channel 0 (single-arg form).
 *
 * Stages @c REG_CT_MODEL (0x05), applies via @c CMD_SET_CT_MODEL_CH0, then
 * persists with @c CMD_SAVE_USER_CONFIG (~700 ms). The model set accepted for
 * the active class is firmware runtime truth — there is no client-side list;
 * an unknown (class, model) is rejected device-side (@c ERR_PARAM) and surfaced.
 * For model codes/descriptors see the generated @c rbamp_sensor_models.h
 * (::rbamp_sensor_model_lookup).
 *
 * v1.2+ firmware precondition: ::rbamp_set_sensor_class MUST be called first.
 * Otherwise this returns @c ESP_ERR_INVALID_STATE without writing. On v1.0 /
 * v1.1 firmware the precondition is skipped (backward compatibility — the
 * device-side callback has no guard).
 *
 * Read-before-write guard, as in ::rbamp_set_ct_model_ch: the model the
 * channel already holds is not re-applied (no bus write), and a module of
 * class 2 or 3 is not reconfigured (@c ESP_ERR_INVALID_STATE).
 *
 * For multi-channel modules use ::rbamp_set_ct_model_ch — this single-arg form
 * targets channel 0.
 *
 * @param[in] dev  Handle.
 * @param[in] code CT preset code for the active sensor class (see
 *                 @c rbamp_sensor_models.h).
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev
 *   - @c ESP_ERR_INVALID_STATE if v1.2+ firmware reports @c REG_SENSOR_CLASS ==
 *     UNSET, or the device rejects the (class, model) pair
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 *
 * @par STD line
 * Same as ::rbamp_set_ct_model_ch on channel 0.
 */
esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code);

/**
 * @brief Set the CT model on a specific channel (v1.2+ firmware).
 *
 * One public call for ANY channel; the routing is internal (PUBLIC_API_TRUTH
 * channel-access canon):
 *   - ch0-2: stages @c REG_CT_MODEL, applies via @c CMD_SET_CT_MODEL_CH0/1/2
 *     (0x28/0x29/0x2A).
 *   - ch3+ (senior UI5/UI7): applies via the channel window, field 15
 *     (@c CHANNEL_SELECT = (15<<4)|ch, then @c CHANNEL_DATA), firmware v1.4.18+.
 * Either way the applied model is read-back-confirmed, then persisted with
 * @c CMD_SAVE_USER_CONFIG (ungated; persists @c ct_model_ch[] for all channels).
 * The accepted model set is firmware runtime truth — no client list; an unknown
 * (class, model) is rejected device-side (@c ERR_PARAM) and surfaced. Model
 * codes/descriptors: generated @c rbamp_sensor_models.h.
 *
 * @note Order-independent (v1.3 A1 / Fix A). @c REG_CT_MODEL is PURE STAGING for
 *       the ch0-2 path; each per-channel apply binds ONLY its own channel, so
 *       mixed-CT configuration can be done in any order and never clobbers
 *       another channel:
 * @code
 * rbamp_set_ct_model_ch(dev, 0, 3);  // ch0
 * rbamp_set_ct_model_ch(dev, 1, 3);  // ch1
 * rbamp_set_ct_model_ch(dev, 4, 8);  // ch4 (senior) — different model, fine
 * @endcode
 *       For a one-call bulk variant see ::rbamp_configure_channels.
 *
 * Requires @c rbamp_firmware_version() >= 0x03 (v1.2) for the ch0-2 path; ch3+
 * needs the field-15 firmware (v1.4.18+). The same v1.2+ sensor-class
 * precondition as ::rbamp_set_ct_model applies.
 *
 * Read-before-write guard. Applying a model sets that channel's generic
 * preset, replacing whatever calibration the unit carries for it, and that
 * happens on the module even when the model does not change. So:
 *   - the model the channel already holds (::rbamp_read_ct_model_ch) is a
 *     no-op with no bus write — re-applying a stored configuration on every
 *     reconnect is safe;
 *   - a module of class 2 or 3 (calibrated together with its fitted sensor)
 *     returns @c ESP_ERR_INVALID_STATE; ::rbamp_set_ct_model_ch_force is the
 *     service path.
 *
 * @param[in] dev     Handle.
 * @param[in] channel 0 .. ::rbamp_channels-1 (validated against the module's
 *                    actual channel count, not a hardcoded 3).
 * @param[in] code    CT preset code for the active class (see
 *                    @c rbamp_sensor_models.h).
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_INVALID_ARG on null @c dev or @c channel >= ::rbamp_channels
 *   - @c ESP_ERR_NOT_SUPPORTED on firmware older than v1.2 (ch0-2 path)
 *   - @c ESP_ERR_INVALID_STATE if @c REG_SENSOR_CLASS == @c UNSET on v1.2+,
 *     the device rejects the (class, model) pair, or the applied model fails
 *     read-back confirmation
 *   - @c ESP_FAIL or @c ESP_ERR_TIMEOUT on I2C transport failure
 *
 * @par STD line
 * The CT model is a REFERENCE only (SKU metadata): it sets no gain — the
 * module carries its own per-channel calibration. Writes the channel's map
 * entry (read back by ::rbamp_read_ct_model_ch) and, for channel 0, the
 * persisted @c CFG_CT_MODEL byte, then @c CMD_SAVE_CONFIG. On current
 * single-channel module firmware only channel 0's model survives a reset.
 * The sensor-class precondition applies (class 0 = unset).
 * @note STD modules from map 0x35 that do not persist a per-channel CT model
 *       (capability @c RBAMP_STD_CAP_CHMAP_PERSIST clear) return
 *       @c ESP_ERR_NOT_SUPPORTED before any bus traffic.
 *
 * @note On a BASIC module without CAP_SCT_RATING_SELF (firmware up to
 *       1.4.19) with sensor class 1 the call ends with
 *       a module restart (CMD_RESET, about half a second, same UID verified):
 *       that is the only way the module returns to its calibrated gain after
 *       a model write, and the library then evaluates the rating correction
 *       (::rbamp_sct_correction). The module's RAM state (digest mask, event
 *       mask, thresholds) is re-applied by the library where it owns it; the
 *       period accumulators restart, as after any module reset.
 */
esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code);

/**
 * @brief Service path: set a channel's CT model without the pairing guard.
 *
 * Same as ::rbamp_set_ct_model_ch but writes regardless of the module's class
 * and of the model currently held. Replaces the paired calibration of that
 * channel with the model's generic preset — use only when re-pairing a
 * module with a new sensor in service, followed by a calibration.
 */
esp_err_t rbamp_set_ct_model_ch_force(rbamp_handle_t dev, uint8_t channel, uint8_t code);

/**
 * @brief Read the CT model ACTUALLY APPLIED to @p channel.
 *
 * Bind-verify: after ::rbamp_set_ct_model_ch, read this to confirm the preset
 * took. @c 0 = unset. ch0-2 read the flat mirrors (0x51/0x52/0x53); ch3+ read
 * back through the channel window (field 15). Distinct from the staged
 * @c REG_CT_MODEL (0x05).
 *
 * @param[in]  channel 0 .. ::rbamp_channels-1.
 * @param[out] out     Receives the applied model code.
 *
 * @par STD line
 * Reads the channel's map entry (@c CHMAP_MODEL).
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
 *       terminal @c CMD_SAVE_USER_CONFIG (plus the one in @c set_sensor_class),
 *       so an N-channel configure costs 2 persist cycles, not 1+N. Each bind is
 *       synchronously verified and read-back-confirmed before the save.
 * @note Covers ALL channels of the module (variant-aware, clamped to
 *       ::rbamp_channels): ch0-2 via the flat CMD path, ch3+ (senior UI5/UI7)
 *       via the channel window (field 15). A mixed per-channel sensor set is a
 *       first-class configuration.
 *
 * @note Read-before-write (see ::rbamp_set_sensor_class and
 *       ::rbamp_set_ct_model_ch): the whole request is decided before the
 *       first write. A class or model the module already holds is not
 *       rewritten, so re-applying the same configuration costs only reads and
 *       no flash cycle; on a module of class 2 or 3 a request that would
 *       change the class or any requested model is refused up front with
 *       @c ESP_ERR_INVALID_STATE and nothing is written.
 *
 * @param[in] dev    Handle.
 * @param[in] cls    Sensor class to apply first.
 * @param[in] models Array of CT model codes (see @c rbamp_sensor_models.h),
 *                   one per channel.
 * @param[in] n      Length of @p models.
 *
 * @return @c ESP_OK; @c ESP_ERR_INVALID_STATE if a channel could not be bound
 *         or the module is calibrated with its fitted sensor (class 2 / 3);
 *         propagated transport / validation errors otherwise.
 *
 * @par STD line
 * Same sequence; models are reference metadata (see ::rbamp_set_ct_model_ch).
 */
esp_err_t rbamp_configure_channels(rbamp_handle_t dev, rbamp_sensor_class_t cls,
                                   const uint8_t *models, uint8_t n);

/**
 * @brief Bare @c CMD_SAVE_GAINS — flush factory gain/noise registers to flash (700 ms block).
 *
 * @warning FACTORY path: @c CMD_SAVE_GAINS is blocked in
 *          production firmware and silently no-ops there. It is NOT the path for
 *          user configuration — sensor class and CT models are @c user_config
 *          persistence and are saved by ::rbamp_save_user_config (called
 *          internally by ::rbamp_set_sensor_class / ::rbamp_set_ct_model /
 *          ::rbamp_set_ct_model_ch / ::rbamp_configure_channels). This bare call
 *          is relevant ONLY on a unit whose owner has manually written
 *          non-public calibration registers via raw access — an out-of-warranty
 *          operation that bypasses the SKU-matched preset table and can produce
 *          wrong readings with no obvious warning. Each call is a flash erase +
 *          write (~700 ms); endurance is finite (~10 000 cycles/page) — never in
 *          a loop.
 *
 * @par STD line
 * Issues @c CMD_SAVE_CAL (persists the calibration block: gains, noise and
 * THD floors) and checks the command verdict. Not gated.
 */
esp_err_t rbamp_save_gains(rbamp_handle_t dev);

/**
 * @brief Arm an I2C address change (step 1 of 2).
 *
 * Validates the new address range and records the arm timestamp. Caller must
 * call ::rbamp_commit_address_change within 5 seconds.
 *
 * @note v1.3: this is **production-OK** (truth-doc §6.1) — the two-phase
 *       magic-armed @c CMD_COMMIT_ADDR is no longer factory-gated, so
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
 *
 * @par STD line
 * Writes @c CFG_I2C_ADDR, persists with @c CMD_SAVE_CONFIG — synchronously:
 * the library waits for the module to finish and reads the verdict (an
 * out-of-range address → @c ESP_ERR_INVALID_ARG, the module's NVM rate limit
 * → @c ESP_ERR_NOT_FINISHED: prepare again and retry after ~5 s; nothing
 * changes in both cases) — then @c CMD_RESET, and switches the handle only
 * after a module answers 'ST' at the new address (within ~3 s). If nothing
 * answers there, @c ESP_ERR_NOT_FOUND and the handle keeps the old address.
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
 *
 * @par STD line
 * No factory-reset command exists: returns @c ESP_ERR_NOT_SUPPORTED.
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

/* Events are one model on both lines (BASIC 0x2A/0x2B, STD 0x00C0/0x00C1
 * from map 0x36, same bit positions): sticky flags, write-1-to-clear; the
 * bits also set in the mask hold the DRDY line LOW (alarm) until cleared or
 * unmasked. Mask and thresholds are RAM on the module: re-arm them after
 * every module reset (RBAMP_EVENT_RESET_OCCURRED tells you). */

/** @brief Read the sticky EVENT_FLAGS bitmap. Test bits with @c RBAMP_EVENT_*.
 *  STD: map 0x36+ (else @c ESP_ERR_NOT_SUPPORTED). */
esp_err_t rbamp_read_event_flags(rbamp_handle_t dev, uint8_t *out);

/** @brief Write-1-to-clear EVENT_FLAGS bits. Pass e.g. @c RBAMP_EVENT_ERROR. */
esp_err_t rbamp_clear_event_flags(rbamp_handle_t dev, uint8_t mask);

/**
 * @brief EVENT_FLAGS as ::rbamp_begin found them (both lines; 0 if not
 *        readable). @c RBAMP_EVENT_RESET_OCCURRED here means the module had
 *        rebooted before the library met it — re-apply its RAM settings
 *        (clock, event mask, thresholds). The library clears nothing at begin.
 */
uint8_t rbamp_begin_event_flags(rbamp_handle_t dev);

/** @brief Read EVENT_MASK: which flags hold the DRDY line LOW (alarm class). */
esp_err_t rbamp_read_event_mask(rbamp_handle_t dev, uint8_t *out);

/** @brief Set EVENT_MASK (RAM on the module; 0 = the line is never held). Readback-verified. */
esp_err_t rbamp_set_event_mask(rbamp_handle_t dev, uint8_t mask);

/**
 * @brief Set the event thresholds (RAM on the module): the highest current
 *        channel above @p i_amps sets @c RBAMP_EVENT_THRESH_I, the total
 *        active power above @p p_watts sets @c RBAMP_EVENT_THRESH_P. Pass a
 *        value <= 0 or @c NaN to disable that threshold.
 *
 * Resolution and sign differ by line: STD compares |grand P| (direction
 * does not matter) with 1 mW / 1 uA steps; BASIC compares the SIGNED sum of
 * channel powers (export never trips it) with 1 W / 0.01 A steps, so values
 * above 655 A or 65534 W are clamped to "disabled" there.
 * @return @c ESP_ERR_NOT_SUPPORTED on STD below map 0x36.
 */
esp_err_t rbamp_set_event_thresholds(rbamp_handle_t dev, float i_amps, float p_watts);

/**
 * @brief Durable error check (v1.3 A3) — true if EVENT_FLAGS bit3 is set.
 *
 * Unlike a @c REG_ERROR readback, EVENT bit3 is sticky (asserts on any
 * rejected write/command and survives later writes) until cleared via
 * ::rbamp_clear_error or a write-1-to-clear of @c RBAMP_EVENT_ERROR. This is
 * the right channel for ASYNC fault polling.
 *
 * @note Timing (bench-verified on hardware): bit3 is immediate and reliable for
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
 * @c group_id / @c label. Production-OK (not factory-gated). On a freshly
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
 *
 * @par STD line
 * True when the module reports its calibration page valid (@c STATUS.cal_ok).
 * Reliable from module firmware 0x00010079; older images also set the bit on
 * factory-default gains, so there "provisioned" cannot be told from
 * "uncalibrated" through this call (use ::rbamp_std_read_health's
 * calibration source instead). From firmware 0x0001007A the bit comes from a
 * marker the module writes when calibration is saved: a unit calibrated
 * under older firmware reads false after the update until one
 * ::rbamp_save_gains on it (its calibration itself is intact).
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
 *
 * @par STD line
 * Reads the persisted @c CFG_I2C_ADDR (applies at the next reset).
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

/* -------------------------------------------------------------------------
 * STD line — block reads (16-bit family map)
 *
 * Every function here returns ESP_ERR_NOT_SUPPORTED on a BASIC-line handle.
 * The scalar API above (rbamp_read_current / _power / _voltage / ...) works
 * on both lines; use these when you want a whole channel in one burst, or the
 * quantities that only the STD line measures (Q / S / THD / signed energy).
 * ------------------------------------------------------------------------- */

/**
 * @brief Cached STD identity from ::rbamp_begin — no bus traffic.
 * @return @c ESP_ERR_NOT_SUPPORTED on a BASIC handle or before ::rbamp_begin.
 */
esp_err_t rbamp_std_info(rbamp_handle_t dev, rbamp_std_info_t *out);

/**
 * @brief Read one current channel block (I / P / Q / S / PF / peak / energy /
 *        harmonics) in a single burst.
 * @param[in] ch 0 .. ::rbamp_channels-1.
 */
esp_err_t rbamp_std_read_channel(rbamp_handle_t dev, uint8_t ch, rbamp_std_channel_t *out);

/**
 * @brief Read an aggregate block: @p k = 0 grand total (sum of all current
 *        channels), 1..3 per-phase totals A/B/C (multi-phase modules only —
 *        gated on the module's voltage channel count).
 * @note Per-phase totals (k = 1..3) are published from map 0x36 — on a
 *       single-phase module k = 1 equals channel 0 and k = 2/3 read 0; below
 *       0x36 they return @c ESP_ERR_NOT_SUPPORTED (k = 0 always works).
 * @note The energy fields (@c e_import_mws / @c e_export_mws) are NOT
 *       populated for aggregate blocks and read 0 — energy comes from the
 *       persistent counters (::rbamp_std_read_energy).
 */
esp_err_t rbamp_std_read_totals(rbamp_handle_t dev, uint8_t k, rbamp_std_channel_t *out);

/**
 * @brief Read one voltage channel block (RMS / peak / THD / phase angle) in one burst.
 * @param[in] v 0 .. @c n_voltage-1 (see ::rbamp_std_info).
 * @note @c angle_deg / @c angle_valid: three-phase modules with firmware
 *       0x0001007A or newer; otherwise @c angle_valid is false and the rest
 *       of the block is unaffected.
 */
esp_err_t rbamp_std_read_voltage(rbamp_handle_t dev, uint8_t v, rbamp_std_voltage_t *out);

/**
 * @brief Phase sequence of a three-phase module, derived by the module from
 *        the voltage angles (::rbamp_std_voltage_t.angle_deg).
 * @return @c ESP_ERR_NOT_SUPPORTED (and @c *out = @c RBAMP_STD_PHASE_SEQ_NA)
 *         on a module with firmware older than 0x0001007A — no bus traffic;
 *         @c ESP_OK otherwise, with @c RBAMP_STD_PHASE_SEQ_NA on
 *         single-voltage modules or while an input is below 50 V.
 */
esp_err_t rbamp_std_read_phase_seq(rbamp_handle_t dev, rbamp_std_phase_seq_t *out);

/**
 * @brief Live module health: status bits + sensor frame / CRC-error counters.
 *
 * Treat readings as valid when @c status has @c RBAMP_STD_ST_SENSOR_OK and
 * @c frame_count grows between two polls.
 */
esp_err_t rbamp_std_read_health(rbamp_handle_t dev, rbamp_std_health_t *out);

/* -------------------------------------------------------------------------
 * STD line — latch banks (8 independent master-timed periods)
 *
 * Typical use: arm the banks you need once, then per bank on your own
 * schedule: rbamp_std_latch(bank) → wait >= 300 ms (safe upper bound of one
 * measurement window on every image) → rbamp_std_read_latch
 * → rbamp_std_latch_wh with the master-measured dt since the previous latch.
 * Bank 0 is also what rbamp_read_period_snapshot / rbamp_energy_wh use.
 *
 * Only on modules with RBAMP_STD_CAP_MULTI_LATCH (rbamp_std_info_t
 * .period_source == RBAMP_STD_PERIOD_LATCH). The three bank calls return
 * ESP_ERR_NOT_SUPPORTED without bus traffic on any other module — use
 * rbamp_std_read_energy / rbamp_std_energy_delta there.
 * ------------------------------------------------------------------------- */

/**
 * @brief Read the module's persisted life-time energy counters.
 *
 * Each decoded block is one transaction (the module serves it from a
 * coherent snapshot): the Wh/mWh block, the per-channel mWh block and, on
 * modules with µ-resolution counters, the µWh/µvarh block and the
 * per-channel µWh block. The µ fields are filled on every module (see
 * ::rbamp_std_energy_t — @c fine tells whether the module supplied them).
 * @return @c ESP_ERR_NOT_SUPPORTED unless the module reports
 *         @c RBAMP_STD_CAP_ONDEVICE_ENERGY; @c ESP_ERR_INVALID_RESPONSE if
 *         the block reads as unprogrammed.
 */
esp_err_t rbamp_std_read_energy(rbamp_handle_t dev, rbamp_std_energy_t *out);

/**
 * @brief Energy between two counter readings with the continuity rules
 *        applied: same @c generation, no counter going backwards.
 *
 * @param[in]  prev              Earlier reading.
 * @param[in]  now               Later reading.
 * @param[out] delta             now - prev per counter (import/export magnitudes,
 *                               net, reactive); @c generation and @c uptime_s are
 *                               copied from @p now. May be NULL.
 * @param[out] module_restarted  true if @c uptime_s went down (module rebooted
 *                               between the readings; counters are persisted,
 *                               the delta is still valid). May be NULL.
 * A counter that went down is clamped to a 0 difference (power-cut resume
 * rule), per channel too, and at every resolution (Wh, mWh, µWh, varh,
 * µvarh); every net field is the import difference minus the export
 * difference, never the difference of the module's net counters.
 * Per-channel differences are produced when both readings carry the same
 * channel count; otherwise @c delta->channels is 0. @c delta->fine /
 * @c fine_reactive are true only when both readings were fine.
 * @return @c ESP_ERR_INVALID_STATE if the generation differs — do not account
 *         that interval; start again from @p now.
 */
esp_err_t rbamp_std_energy_delta(const rbamp_std_energy_t *prev, const rbamp_std_energy_t *now,
                                 rbamp_std_energy_t *delta, bool *module_restarted);

/**
 * @brief Set the accumulate mask: bit b = bank b accumulates (newly armed
 *        banks start clean; disarmed banks freeze, last snapshot stays).
 * @note  Bit 0 must stay set if you use ::rbamp_read_period_snapshot.
 * @warning The mask is RAM on the module: any module reset (power cycle,
 *          watchdog, OTA activation, a reset command) clears it, and a latch
 *          of an unarmed bank returns an empty snapshot. The library re-arms
 *          bank 0 by itself (in ::rbamp_begin, after its own resets, and when
 *          a period snapshot comes back empty). For your own banks
 *          ::rbamp_std_read_latch detects the lost arm, re-arms and
 *          primer-latches, and returns @c ESP_ERR_INVALID_STATE for that one
 *          period — drop it and restart your dt anchor.
 */
esp_err_t rbamp_std_arm_banks(rbamp_handle_t dev, uint8_t mask);

/**
 * @brief Request snapshot + reset of @p bank at the module's next window
 *        boundary. Does not wait. Read it >= 300 ms later (a safe upper
 *        bound: the window is 100 ms from image 0x7C, up to ~160 ms before).
 */
esp_err_t rbamp_std_latch(rbamp_handle_t dev, uint8_t bank);

/**
 * @brief Read the last snapshot of @p bank (one burst: totals + per-channel
 *        energy for every populated channel).
 * @return @c ESP_ERR_INVALID_RESPONSE if the module has never latched this
 *         bank (@c valid == false); other fields are still zeroed.
 *         @c ESP_ERR_INVALID_STATE if the snapshot is empty because the bank
 *         had been disarmed by a module reset — the library has re-armed and
 *         primer-latched it; skip this period and restart your dt anchor.
 */
esp_err_t rbamp_std_read_latch(rbamp_handle_t dev, uint8_t bank, rbamp_std_latch_t *out);

/**
 * @brief Convert a snapshot's energy buckets to Wh over the master-measured
 *        period @p dt_ms.
 *
 * Wh = E[mW·samples] / 1000 / 3600 · (dt / nsamp): the sample count and the
 * master's dt remove any dependence on the module's sample rate.
 *
 * @param[in]  snap       Snapshot from ::rbamp_std_read_latch.
 * @param[in]  dt_ms      Master wall-clock between the previous and this latch of the bank.
 * @param[in]  ch         Channel index, or ::RBAMP_STD_TOTAL for the bank totals.
 * @param[out] wh_import  Import energy, Wh (>= 0). May be NULL.
 * @param[out] wh_export  Export energy, Wh (<= 0). May be NULL.
 * @return @c ESP_ERR_INVALID_STATE if @c nsamp is 0 (nothing accumulated).
 */
esp_err_t rbamp_std_latch_wh(const rbamp_std_latch_t *snap, uint32_t dt_ms, uint8_t ch,
                             double *wh_import, double *wh_export);

/* -------------------------------------------------------------------------
 * STD line — clock, identity, command completion (map 0x35+)
 * ------------------------------------------------------------------------- */

/**
 * @brief Set the module clock (unix epoch, seconds) and wait for completion.
 *
 * The clock is RAM: it is invalid after every module reset, so call this after
 * ::rbamp_begin and whenever ::rbamp_std_health_t::time_valid is false (or a
 * period reports @c module_restarted). Without a valid clock the module
 * records no history. The first set after a reset closes the running history
 * interval without carrying it.
 * @return @c ESP_ERR_INVALID_ARG if the module refuses the epoch (before 2020);
 *         @c ESP_ERR_INVALID_STATE if the clock did not become valid;
 *         @c ESP_ERR_NOT_SUPPORTED below map 0x35.
 */
esp_err_t rbamp_std_set_time(rbamp_handle_t dev, uint32_t unix_epoch);

/** @brief Read the module clock (unix epoch) and whether it is valid. */
esp_err_t rbamp_std_read_time(rbamp_handle_t dev, uint32_t *unix_epoch, bool *valid);

/**
 * @brief Running firmware image version (u32 from the image header, e.g.
 *        0x00010072). Distinct from ::rbamp_firmware_version, which is the
 *        register-map version on this line. 0 if the module cannot tell.
 */
esp_err_t rbamp_std_read_image_version(rbamp_handle_t dev, uint32_t *out);

/**
 * @brief Everything a master needs to recognise a module — output of
 *        ::rbamp_std_identify (on a handle) and ::rbamp_std_probe (on a bare
 *        address, for scanning). The UID is the identity: addresses can change.
 */
typedef struct {
    uint8_t  i2c_addr;     /**< Address this came from. */
    uint8_t  map_version;  /**< Register-map version. */
    uint16_t capabilities; /**< @c RBAMP_STD_CAP_* bits. */
    uint8_t  variant;      /**< @c RBAMP_STD_VARIANT_* code. */
    uint8_t  n_voltage;    /**< Voltage channels. */
    uint8_t  n_current;    /**< Current channels. */
    uint8_t  uid[12];      /**< Chip UID. */
    uint32_t fw_version;   /**< Running image version (map 0x35+; 0 if unknown). */
    uint32_t board_id;     /**< Board id (map 0x36+; 0 below). */
} rbamp_std_identity_t;

/** @brief Identity of the module behind @p dev (two to four short reads). */
esp_err_t rbamp_std_identify(rbamp_handle_t dev, rbamp_std_identity_t *out);

/**
 * @brief Probe one address on @p bus without a handle — the building block
 *        of a scan. Fast: no retries, a NACK is the answer.
 * @return @c ESP_OK with @p out filled; @c ESP_ERR_NOT_FOUND if nothing
 *         acknowledges; @c ESP_ERR_INVALID_RESPONSE if something answers
 *         that is not an STD module (a BASIC module, another chip).
 */
esp_err_t rbamp_std_probe(i2c_master_bus_handle_t bus, uint8_t addr, rbamp_std_identity_t *out);

/**
 * @brief Write one command opcode and wait for its completion: waits for the
 *        module to be idle first (a command written while it is busy is
 *        dropped), sleeps @p settle_ms, waits for BUSY to clear, then reads
 *        the verdict. @p timeout_ms 0 = 1 s. Verdicts: @c ESP_ERR_INVALID_ARG
 *        (bad parameter), @c ESP_ERR_NOT_SUPPORTED (unknown command),
 *        @c ESP_ERR_INVALID_CRC (value check), @c ESP_ERR_NOT_FINISHED (NVM
 *        write rate-limited — retry after ~5 s), @c ESP_ERR_NOT_ALLOWED (key
 *        or maintenance gate), @c ESP_ERR_TIMEOUT (still busy).
 */
esp_err_t rbamp_std_command(rbamp_handle_t dev, uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms);

/** @brief Write the energy accumulators to NVM now (also implied by reset / OTA commit). */
esp_err_t rbamp_std_hist_flush(rbamp_handle_t dev);

/** @brief Blink the module's LEDs for ~10 s so a person can find it. */
esp_err_t rbamp_std_locate(rbamp_handle_t dev);

/**
 * @brief Board id of the running image (map 0x36+): a word that encodes the
 *        hardware the image was built for. A firmware image is accepted only
 *        when its board word matches — see ::rbamp_std_ota_board_ok.
 */
esp_err_t rbamp_std_read_board_id(rbamp_handle_t dev, uint32_t *out);

/**
 * @brief Give one module on a SHARED address its own address, selected by
 *        UID (map 0x36+). Several modules may sit on @p shared_addr (e.g. the
 *        factory default); only the one whose UID matches takes @p new_addr,
 *        persists it and restarts, the others ignore the command. Nothing is
 *        read on the shared address (that would collide); after the restart
 *        the library probes @p new_addr and checks the UID there.
 *
 * Needs no handle: pass the bus. Get UIDs from a module you can talk to
 * alone (::rbamp_read_uid), or from the production record.
 *
 * @param[in] restart_ms  Time to allow for the module's restart; 0 = 1500 ms.
 * @return @c ESP_ERR_NOT_FOUND if nothing answers at @p new_addr afterwards;
 *         @c ESP_ERR_INVALID_RESPONSE if a module answers there but its UID
 *         is not @p uid.
 */
esp_err_t rbamp_std_set_address_by_uid(i2c_master_bus_handle_t bus, uint8_t shared_addr,
                                       const uint8_t uid[12], uint8_t new_addr, uint32_t restart_ms);

/**
 * @brief Wait until the module has finished the command it is executing
 *        (command-state BUSY clear). Every command issued through this
 *        library already waits; use this only around commands you write to
 *        @c RBAMP_STD_REG_COMMAND yourself — a command written while BUSY is
 *        dropped (OVERRUN). Returns immediately on modules without the
 *        command-state register.
 */
esp_err_t rbamp_std_wait_idle(rbamp_handle_t dev, uint32_t timeout_ms);

/* -------------------------------------------------------------------------
 * STD line — on-device history and event log (map 0x35+)
 *
 * The module keeps the LAST 15-minute record in RAM (gone at a module
 * reset — a live indicator, not history) and an hourly ring in NVM (depth
 * follows the NVM size, ~7 h on the current part) that survives resets and
 * is the ring to BACKFILL from; both are written only while the module's
 * clock is valid, plus a never-erased event log of metrology-relevant changes.
 * Each HOUR record carries the life-time counters ABSOLUTE at period end:
 * the period's grand energy is the difference of two consecutive records
 * (resets and gaps stay visible). Backfill after a master outage: walk
 * `back` from 0 (newest) until the timestamps you already have.
 * ------------------------------------------------------------------------- */

typedef enum {
    RBAMP_STD_HIST_MIN15  = 0, /**< 15-minute records (RAM, lost at module reset). */
    RBAMP_STD_HIST_HOUR   = 1, /**< Hourly records (NVM ring). */
    RBAMP_STD_HIST_EVENTS = 2, /**< Event log (NVM, never erased) — read with ::rbamp_std_event_read. */
} rbamp_std_hist_period_t;

/** @brief Ring state for one history period — output of ::rbamp_std_hist_info. */
typedef struct {
    uint16_t count;        /**< Records available (0 while the ring is being cleared). */
    uint16_t capacity;     /**< Ring depth. */
    uint16_t record_size;  /**< Bytes per record (80 for energy periods, 32 for events). */
    uint32_t oldest_ts;    /**< Timestamp of the oldest record, unix. */
    uint32_t newest_ts;    /**< Timestamp of the newest record, unix. */
} rbamp_std_hist_info_t;

/* Record flags (::rbamp_std_hist_record_t::flags) are the generated
 * @c RBAMP_STD_HF_PARTIAL / POWERUP / CLOCK / LTA_JUMP. Take a period's energy
 * from two consecutive records only when their timestamps differ by exactly
 * one period and neither carries LTA_JUMP. */

/** Per-phase columns a history record can carry. */
#define RBAMP_STD_HIST_PHASES  3

/** @brief One decoded energy-period record — output of ::rbamp_std_hist_read. */
typedef struct {
    uint32_t ts;           /**< Period start, unix. */
    uint16_t flags;        /**< @c RBAMP_STD_HF_* bits. */
    uint16_t n;            /**< Samples (MIN15) or 15-minute children (HOUR: 4 = complete). */
    bool     from_ram;     /**< Record came from the RAM ring (not yet persisted). */
    float    v_rms;        /**< Average voltage of channel 0, V. */
    float    i_total;      /**< Average total current, A. */
    float    p_avg;        /**< Average grand active power, W (signed). */
    float    q_avg;        /**< Average grand reactive power, var. */
    float    s_avg;        /**< Average grand apparent power, VA. */
    float    pf;           /**< Average power factor. */
    float    p_min, p_max; /**< Grand active power envelope, W. */
    float    i_min, i_max; /**< Total current envelope, A. */
    uint64_t e_import_wh;  /**< Life-time import counter ABSOLUTE at period end, Wh. */
    uint64_t e_export_wh;  /**< Life-time export counter ABSOLUTE at period end, Wh. */
    uint8_t  phases;       /**< Entries valid in the per-phase arrays (record size dependent). */
    double   ph_import_wh[RBAMP_STD_HIST_PHASES]; /**< Per-phase energy OF THIS PERIOD, Wh (import). */
    double   ph_export_wh[RBAMP_STD_HIST_PHASES]; /**< Per-phase energy OF THIS PERIOD, Wh (export). */
} rbamp_std_hist_record_t;

/** @brief Ring state of one history period (also selects it on the module). */
esp_err_t rbamp_std_hist_info(rbamp_handle_t dev, rbamp_std_hist_period_t period,
                              rbamp_std_hist_info_t *out);

/**
 * @brief Fetch and decode record @p back (0 = newest) of @p period
 *        (MIN15 or HOUR; events go through ::rbamp_std_event_read).
 * @return @c ESP_ERR_NOT_FOUND if there is no such record.
 */
esp_err_t rbamp_std_hist_read(rbamp_handle_t dev, rbamp_std_hist_period_t period, uint16_t back,
                              rbamp_std_hist_record_t *out);

/**
 * @brief Fetch record @p back of @p period undecoded: @p rec receives the
 *        module's record bytes (@c RBAMP_STD_REG_HIST_REC_SIZE buffer; the
 *        meaningful length is ::rbamp_std_hist_info_t::record_size),
 *        @p flags the module's record flags (bit 0 valid, bit 1 from RAM).
 */
esp_err_t rbamp_std_hist_read_raw(rbamp_handle_t dev, rbamp_std_hist_period_t period, uint16_t back,
                                  uint8_t rec[RBAMP_STD_REG_HIST_REC_SIZE], uint8_t *flags);

/* Event types: the generated @c RBAMP_STD_EV_* constants (rbamp_std_registers.h):
 * LT_RESET / LT_ADOPT (a_before/a_after = generation), LT_LATE_LOAD, LT_MIGRATE,
 * CAL_SAVE (cal CRC-32), FW_COMMIT (image version), HIST_CLEAR, CFG_LATE_LOAD,
 * NVM_AUTH (a_before = rejected count, a_after = first address), LTA_ROLLBACK /
 * EVLOG_ROLLBACK (a_before = store seq, a_after = anchor seq). */
/* Event flags (::rbamp_std_event_t::flags): generated @c RBAMP_STD_EVF_MAINT (maintenance mode was active). */

/** @brief One decoded event-log record — output of ::rbamp_std_event_read. */
typedef struct {
    uint32_t ts;           /**< Unix time (0 = clock was not set). */
    uint32_t uptime_s;     /**< Module uptime at the event. */
    uint8_t  type;         /**< @c RBAMP_STD_EV_*. */
    uint8_t  flags;        /**< @c RBAMP_STD_EVF_*. */
    uint16_t boot;         /**< Boot counter (low 16 bits). */
    uint32_t a_before;     /**< Type-specific value before the change. */
    uint32_t a_after;      /**< Type-specific value after the change. */
    uint64_t e_before_wh;  /**< Grand import counter before, Wh. */
    uint64_t e_after_wh;   /**< Grand import counter after, Wh. */
} rbamp_std_event_t;

/** @brief Fetch and decode event @p back (0 = newest). @c ESP_ERR_NOT_FOUND if none. */
esp_err_t rbamp_std_event_read(rbamp_handle_t dev, uint16_t back, rbamp_std_event_t *out);

/* -------------------------------------------------------------------------
 * STD line — authenticity (map 0x35+)
 *
 * A provisioned module carries a device certificate signed by the
 * manufacturer key and answers a challenge with an Ed25519 signature by the
 * device key named in that certificate. The library fetches both; verifying
 * them (Ed25519, e.g. libsodium from the component registry) is the caller's
 * job: 1) certificate bytes [0, 64) are signed by the manufacturer public key
 * with the signature in bytes [64, 128); 2) the attestation signature covers
 * RBAMP_STD_ATTEST_MSG_TAG || challenge (32) || UID (12, from the
 * certificate) || data (24, as returned). The attested data binds the
 * firmware version and the life-time counters to the device key.
 * ------------------------------------------------------------------------- */

/* The attested-message prefix is the generated string constant
 * @c RBAMP_STD_ATTEST_MSG_TAG (frozen; every shipped unit signs it). */

/** @brief Device certificate — output of ::rbamp_std_read_certificate. */
typedef struct {
    uint8_t  raw[RBAMP_STD_REG_SEC_CERT_SIZE]; /**< The 128 certificate bytes as read. */
    uint8_t  state;         /**< Module's own check: 0 none · 1 ok · 2 bad · 3 not checked yet (~3 s after boot). */
    uint8_t  version;       /**< Certificate format version. */
    uint8_t  variant;       /**< Module variant code in the certificate. */
    uint8_t  hw_rev;        /**< Hardware revision in the certificate. */
    uint8_t  uid[12];       /**< Chip UID the certificate was issued for (compare with ::rbamp_read_uid). */
    uint32_t batch;         /**< Production batch. */
    uint32_t issued;        /**< Issue time, unix. */
    uint8_t  device_pubkey[32]; /**< Device Ed25519 public key. */
    uint32_t mfr_key_id;    /**< Identifier of the manufacturer key that signed it. */
} rbamp_std_certificate_t;

/**
 * @brief Read the device certificate.
 * @return @c ESP_ERR_NOT_FOUND if the module has none (unprovisioned; @c state
 *         is still filled); @c ESP_ERR_NOT_SUPPORTED below map 0x35.
 */
esp_err_t rbamp_std_read_certificate(rbamp_handle_t dev, rbamp_std_certificate_t *out);

/** @brief Attestation answer — output of ::rbamp_std_attest. */
typedef struct {
    uint8_t  signature[64]; /**< Ed25519 signature over the attested message. */
    uint8_t  data[24];      /**< Attested data exactly as signed. */
    uint32_t fw_version;    /**< From @c data: running image version. */
    uint32_t generation;    /**< From @c data: life-time generation. */
    int64_t  e_import_wh;   /**< From @c data: life-time import, Wh. */
    int64_t  e_export_wh;   /**< From @c data: life-time export, Wh. */
} rbamp_std_attestation_t;

/**
 * @brief Challenge the module (32 random bytes of your choice — keep your
 *        copy, the module overwrites the challenge area with the signature)
 *        and fetch the signed answer (~60 ms).
 * @return @c ESP_ERR_NOT_FOUND if the module has no device key;
 *         @c ESP_ERR_INVALID_STATE if the module did not report the
 *         attestation as done.
 */
esp_err_t rbamp_std_attest(rbamp_handle_t dev, const uint8_t challenge[32], rbamp_std_attestation_t *out);

/* -------------------------------------------------------------------------
 * STD line — configuration / calibration
 *
 * The scalar configuration API (rbamp_set_sensor_class, rbamp_set_ct_model_ch,
 * rbamp_configure_channels, rbamp_save_user_config, rbamp_save_gains,
 * rbamp_prepare/commit_address_change, rbamp_reset) works on both lines; see
 * each function's "STD line" note. These are STD-only additions.
 * ------------------------------------------------------------------------- */

/** @brief Read the configuration block (one burst, plus the starting-current register where the module has it). */
esp_err_t rbamp_std_read_config(rbamp_handle_t dev, rbamp_std_config_t *out);

/**
 * @brief Read the starting current for energy, µA (0 = off).
 *
 * Below this channel current the module counts no energy for that channel
 * (import or export, grand and per channel); readings are not affected.
 * Default 400 µA.
 * @return @c ESP_ERR_NOT_SUPPORTED on a module without the setting
 *         (firmware older than 0x0001007B) — no bus traffic.
 */
esp_err_t rbamp_std_get_start_current(rbamp_handle_t dev, uint16_t *ua);

/**
 * @brief Set the starting current for energy: 0 = off, or 10 .. 2540 µA in
 *        steps of 10 µA. Applies at once; ::rbamp_save_user_config persists it.
 *        Read back and confirmed.
 * @return @c ESP_ERR_INVALID_ARG for a value outside that set — nothing is
 *         written; @c ESP_ERR_NOT_SUPPORTED as for ::rbamp_std_get_start_current;
 *         @c ESP_ERR_INVALID_STATE if the module did not take the value.
 */
esp_err_t rbamp_std_set_start_current(rbamp_handle_t dev, uint16_t ua);

/**
 * @brief Switch base-harmonics extraction (THD / displacement PF / Q1) on or off.
 *
 * Off by default on the regular tier (saves module CPU); the harmonics fields
 * of ::rbamp_std_channel_t / ::rbamp_std_read_voltage read 0 while off.
 * RAM-only — reverts to off at module reset. Read back and confirmed.
 * @return @c ESP_ERR_NOT_SUPPORTED without the THD capability.
 */
esp_err_t rbamp_std_set_harmonics(rbamp_handle_t dev, bool enable);

/**
 * @brief Calibrate the sensor-chain THD floor: captures the current smoothed
 *        raw THD as the floor, persists it, applies it live.
 *
 * Precondition: clean mains and NO load on the CT — the captured value is
 * subtracted (in quadrature) from every later THD reading. Blocks for the
 * persist settle; verdict via the module's error register (map 0x34+).
 * @return @c ESP_ERR_NOT_SUPPORTED without the THD capability.
 */
esp_err_t rbamp_std_cal_thd_floor(rbamp_handle_t dev);

/* -------------------------------------------------------------------------
 * STD line — firmware update over I2C (two-phase OTA)
 *
 * Sequence: rbamp_std_ota_begin → rbamp_std_ota_write_page ×N (pages of 256
 * bytes; blank 0xFF pages may be skipped) → rbamp_std_ota_commit (verifies,
 * ARMS the swap, no reboot) → rbamp_std_ota_activate (reboots into the new
 * image when you are ready; the module is off the bus for a few seconds,
 * then runs a ~10 s trial before the image is confirmed).
 * rbamp_std_ota_write_prepared does begin + pages + commit for a
 * vendor-prepared image in one call; host-built images are not accepted
 * (rbamp_std_ota_write_image returns ESP_ERR_NOT_SUPPORTED).
 * Every command completes synchronously inside the library (the module's
 * command-state register, not a fixed delay); after every page the session
 * must still be open or the call fails with ESP_ERR_INVALID_STATE — restart
 * from begin. A bad image is rolled back by the module itself; calibration
 * survives.
 * All return ESP_ERR_NOT_SUPPORTED unless the module reports the OTA
 * capability on map 0x33 or later.
 * ------------------------------------------------------------------------- */

/** @brief Read the module's OTA state (@c RBAMP_STD_OTA_IDLE / READY / OK / ERR / DOWNGRADE). */
esp_err_t rbamp_std_ota_status(rbamp_handle_t dev, uint8_t *out);

/**
 * @brief Erase the inactive bank and wait until the module reports READY.
 * @param[in] timeout_ms 0 = default (5 s; the erase takes ~1.2 s).
 * @return @c ESP_ERR_TIMEOUT if READY was not reached.
 */
esp_err_t rbamp_std_ota_begin(rbamp_handle_t dev, uint32_t timeout_ms);

/**
 * @brief Write one 256-byte page at index @p page_idx (offset / 256 within the bank).
 * @return @c ESP_ERR_INVALID_STATE if the module flags a write error.
 */
esp_err_t rbamp_std_ota_write_page(rbamp_handle_t dev, uint32_t page_idx,
                                   const uint8_t page[RBAMP_STD_REG_OTA_PAYLOAD_SIZE]);

/**
 * @brief Verify the written image and arm the bank swap (no reboot).
 * @param[in] timeout_ms 0 = default (3 s).
 * @return @c ESP_OK armed · @c ESP_ERR_INVALID_CRC bad image (CRC / header) ·
 *         @c ESP_ERR_INVALID_MAC authentication failed (MAC / seal / signature,
 *         modules with signed images) · @c ESP_ERR_INVALID_VERSION image older
 *         than the running one (rejected) · @c ESP_ERR_TIMEOUT.
 *         The module's reason code is available via ::rbamp_std_ota_diag.
 */
esp_err_t rbamp_std_ota_commit(rbamp_handle_t dev, uint32_t timeout_ms);

/**
 * @brief Reboot into the armed image, wait for the module to come back
 *        (~4 s), rediscover it, then hold through the ~10 s trial window and
 *        confirm: running bank changed, running image = the staged version,
 *        swap no longer pending.
 *
 * Blocks for about 10 s in total. Do not reset or power-cycle the module in
 * that window — the bootloader rolls back an unconfirmed image. The handle
 * stays valid; identity is refreshed (::rbamp_std_info reflects the new
 * firmware).
 * @return @c ESP_ERR_INVALID_STATE if nothing was armed, if the module came
 *         back on the previous bank or image (rollback), or if the swap is
 *         still pending after the trial window.
 */
esp_err_t rbamp_std_ota_activate(rbamp_handle_t dev);

/** @brief CRC-32 (zlib-compatible) — the checksum the module verifies at commit. */
uint32_t rbamp_std_crc32(const uint8_t *data, size_t len);

/** @brief Progress callback for ::rbamp_std_ota_write_image (pages done / total). */
typedef void (*rbamp_std_ota_progress_cb_t)(uint32_t done, uint32_t total, void *ctx);

/**
 * @brief Stream a PREPARED bank image as-is: a file produced by the module
 *        vendor's build tooling that already is a whole bank (signed and, where
 *        the module requires it, encrypted) with its header in the last page.
 *
 * Begin, pages 0 .. size/256-1 ascending (page 0 always sent, other all-0xFF
 * pages skipped), commit. Nothing is added or patched by the library — such
 * images cannot be built on the host, and a module that requires them rejects
 * a host-built header at commit. Does NOT activate.
 *
 * @param[in] image Prepared image for the bank that is NOT running (vendor
 *                  tooling produces one file per bank).
 * @param[in] size  Must equal the bank size the module reports
 *                  (::rbamp_std_info_t::ota_bank_size); otherwise
 *                  @c ESP_ERR_INVALID_SIZE and nothing is written.
 */
esp_err_t rbamp_std_ota_write_prepared(rbamp_handle_t dev, const uint8_t *image, size_t size,
                                       rbamp_std_ota_progress_cb_t cb, void *ctx);

/**
 * @brief Like ::rbamp_std_ota_write_prepared, given both bank images: the
 *        library asks the module which bank runs and streams the other one.
 * @return @c ESP_ERR_NOT_SUPPORTED on modules that do not report their running
 *         bank id (map < 0x35) — use ::rbamp_std_ota_write_prepared and pick by
 *         ::rbamp_std_info_t::active_bank yourself.
 */
esp_err_t rbamp_std_ota_write_prepared_pair(rbamp_handle_t dev,
                                            const uint8_t *bank_a, size_t size_a,
                                            const uint8_t *bank_b, size_t size_b,
                                            rbamp_std_ota_progress_cb_t cb, void *ctx);

/**
 * @brief Pre-check before streaming a prepared image: the image's board word
 *        (shipped next to the image file by the vendor tooling; it is not
 *        readable from the sealed image) must equal the module's board id.
 *        A mismatch would only be found at commit (reason BOARD) after the
 *        whole image was sent.
 * @return @c ESP_OK if it matches; @c ESP_ERR_INVALID_ARG if not (logged);
 *         @c ESP_ERR_NOT_SUPPORTED below map 0x36 (no board id to compare).
 */
esp_err_t rbamp_std_ota_board_ok(rbamp_handle_t dev, uint32_t image_board_word);

/**
 * @brief Close an update session and disarm an armed image (the module keeps
 *        running what it runs). Map 0x35+ only (@c ESP_ERR_NOT_SUPPORTED below).
 */
esp_err_t rbamp_std_ota_abort(rbamp_handle_t dev);

/**
 * @brief Version of the armed (committed, not yet activated) image; 0 when
 *        nothing is armed. Map 0x35+ only.
 */
esp_err_t rbamp_std_ota_staged_version(rbamp_handle_t dev, uint32_t *out);

/**
 * @brief Why the module rejected the last page or commit: the reason byte of
 *        its OTA diagnostics block (0 = none). Map 0x35+ only.
 *
 * Codes a host sees on a bad image: 7 header, 9 CRC, 10 MAC, 13 seal,
 * 14 signature, 8 header page never sent, 11 downgrade; 1–6 are flash or
 * transport faults on a page. ::rbamp_std_ota_commit already maps 10/13/14
 * to @c ESP_ERR_INVALID_MAC and the rest to @c ESP_ERR_INVALID_CRC and logs
 * the name; call this when you need the code itself.
 */
esp_err_t rbamp_std_ota_diag(rbamp_handle_t dev, uint8_t *reason);

#if defined(RBAMP_STD_OTA_IMG_MAGIC) && defined(RBAMP_STD_OTA_HEADER_SIZE)
/** @brief Build the image header (magic / version / length / crc32, little-endian). */
void rbamp_std_ota_make_header(uint8_t out[RBAMP_STD_OTA_HEADER_SIZE], uint32_t version,
                               uint32_t len, uint32_t crc32);

/**
 * @brief Host-built (unsigned) image update — NOT supported by current module
 *        firmware: every release image is vendor-prepared and signed, and the
 *        module refuses a host-built header at commit. Returns
 *        @c ESP_ERR_NOT_SUPPORTED before touching the module (kept for ABI).
 *        Use ::rbamp_std_ota_write_prepared.
 */
esp_err_t rbamp_std_ota_write_image(rbamp_handle_t dev, const uint8_t *image, size_t len,
                                    uint32_t version, rbamp_std_ota_progress_cb_t cb, void *ctx);
#endif

#ifdef __cplusplus
}
#endif
