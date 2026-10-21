/**
 * @file    rbamp.c
 * @brief   Implementation of the ESP-IDF rbAmp client component.
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * All wire-level protocol invariants from @c libs/spec/SPEC.md are enforced
 * here:
 *  - BASIC line: one I2C address phase per byte (SPEC §6) — every multi-byte
 *    read is a loop of single-byte @c i2c_master_transmit_receive calls.
 *  - STD line (v1.6.0+): 16-bit big-endian register pointer + auto-increment
 *    burst reads (registers_std.yaml); line detected once in rbamp_begin and
 *    routed internally, the 8-bit primitives refuse on an STD handle.
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
#include "rbamp_std_decode.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

static inline void _sleep_ms(uint32_t ms);   /* defined with the transport helpers */

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

    /* Product line — set by rbamp_begin from the identity probe. While std is
     * false every call takes the BASIC 8-bit path; once true, the two 8-bit
     * primitives refuse (ESP_ERR_NOT_SUPPORTED), so no BASIC-only call can put
     * an 8-bit transaction on an STD module. STD identity is cached here. */
    bool                    std;
    uint8_t                 std_version;
    uint16_t                std_caps;
    uint8_t                 std_status;
    uint8_t                 n_voltage;
    uint8_t                 n_current;
    uint8_t                 std_variant;
    uint32_t                std_active_bank;
    uint32_t                std_ota_bank_size;   /* device-reported; 0 = no OTA */
    uint8_t                 std_ota_last_reason; /* OTA_DIAG reason of the last rejection, 0 = none */
    rbamp_std_period_source_t std_period_src;    /* latch banks / persisted counters / none (from caps) */
    rbamp_std_energy_t      std_lt_anchor;       /* counter path: the previous consumed reading */
    bool                    std_lt_anchored;
    float                   std_last_avg_total;  /* counter path: avg P of the last consumed period */
    bool                    std_have_last_avg;
    uint32_t                std_fw_ver;          /* image version (map 0x35+), 0 unknown */
    uint16_t                std_cmd_seq;         /* CMD_SEQ before the last synchronous command (map 0x35+) */

    /* Data-ready: line this handle listens on (NULL = polling), its wake-up
     * semaphore, and the last snapshot sequence consumed. */
    struct _rbamp_drdy_line *drdy_line;
    SemaphoreHandle_t       drdy_sem;
    uint16_t                drdy_last_seq;
    bool                    drdy_have_seq;
    uint8_t                 drdy_pending_flags;  /* alarm flags another handle consumed for this module */

    /* BASIC digest (0x70): the field mask lives in module RAM and is lost on
     * a module restart, so the library re-writes it when a restart shows. */
    uint8_t                 digest_mask;         /* 0 = default (I + P, + U with voltage hw) */
    bool                    digest_armed;
    bool                    digest_reset_seen;   /* RESET_OCCURRED episode already re-armed */
    uint32_t                basic_last_uptime_s;
    bool                    basic_have_uptime;
    uint8_t                 begin_event_flags;   /* EVENT_FLAGS as seen by rbamp_begin (RESET_OCCURRED latch) */

    /* SCT-013 rating switch (BASIC without CAP_SCT_RATING_SELF, class 1): per-channel scale
     * relative to the gain the module applies; re-evaluated after a restart. */
    rbamp_sct_correction_t  sct[RBAMP_MAX_CHANNELS];
    bool                    sct_stale;

    /* Address-change two-step state. */
    uint8_t                 pending_addr;
    int64_t                 pending_armed_us;
    bool                    addr_change_armed;

    /* Energy dt anchor — MASTER wall-clock (esp_timer) of the previous
     * consumed period read. The chip's REG_V03_PERIOD_LATCH_MS (0xEC) is
     * diagnostic only (may read short under load), so billing energy
     * integrates over the master's own clock between successive period
     * reads — or, for the first read after rbamp_begin_resume, over the
     * module's commit-counted window (0xBE). */
    int64_t                 last_latch_us;
    bool                    have_last_latch;
    bool                    basic_resume_pending;  /* rbamp_begin_resume: next consumed
                                                    * snapshot integrates the module window */

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

/* SCL-stretch tolerance of a device handle. The driver default (2 ms) is
 * shorter than the stall a module adds while it erases a flash sector in the
 * middle of an OTA page stream: the transaction ends in the driver's
 * "I2C hardware timeout detected" and the OTA path retries it (bench: 16-18
 * such lines per 704-page image, every page recovered). The original ESP32
 * keeps the timeout in a 20-bit APB-cycle field, so it cannot wait beyond
 * ~13 ms; the other targets take the full Kconfig range. */
#ifdef CONFIG_IDF_TARGET_ESP32
#define RBAMP_SCL_WAIT_US_CAP 13000u
#else
#define RBAMP_SCL_WAIT_US_CAP 60000u
#endif
static inline uint32_t _scl_wait_us(void)
{
    return (CONFIG_RBAMP_I2C_SCL_WAIT_US > RBAMP_SCL_WAIT_US_CAP)
               ? RBAMP_SCL_WAIT_US_CAP : (uint32_t)CONFIG_RBAMP_I2C_SCL_WAIT_US;
}

/** @internal Add a device handle for @c addr on @c bus. */
static esp_err_t _add_device(i2c_master_bus_handle_t bus, uint8_t addr,
                             i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ,
        .scl_wait_us     = _scl_wait_us(),
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, out);
}

/** @internal Gate before the final attempt of a retried transaction.
 *
 * The driver already resets its state machine and clears the bus after every
 * failed transaction (i2c_master, IDF 5.2+), so an extra i2c_master_bus_reset()
 * here is redundant — and it takes no bus lock: on ESP32 it bit-bangs a STOP
 * on the live pins, and if another task has a transaction in flight that STOP
 * raises a completion interrupt the driver mis-handles (write to a NULL
 * operation buffer, StoreProhibited in the I2C ISR — seen on a host, 2026-10-18).
 * Off by default; CONFIG_RBAMP_BUS_RESET_ON_RETRY_EXHAUSTION restores it for
 * a single-task master on a driver that does not recover by itself. */
static inline bool _recover_bus(rbamp_handle_t dev)
{
#if CONFIG_RBAMP_BUS_RESET_ON_RETRY_EXHAUSTION
    return i2c_master_bus_reset(dev->bus) == ESP_OK;
#else
    (void)dev;
    return true;
#endif
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
    if (dev->std) return ESP_ERR_NOT_SUPPORTED;  /* BASIC-only path on an STD module */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        err = i2c_master_transmit_receive(
            dev->dev, &reg, 1, out, 1,
            CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt + 1 < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS) {
            _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
        }
    }
    /* Last-ditch: one more attempt after the retries (see _recover_bus). */
    if (_recover_bus(dev)) {
        _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
        err = i2c_master_transmit_receive(dev->dev, &reg, 1, out, 1,
                                          CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
    }
    dev->retry_exhaustion_count++;
    return err;
}

/** @internal Burst read of @p len bytes from @p reg — only for blocks the
 * module defines as one coherent burst (DIGEST 0x70). Same retry + bus
 * recovery discipline as _read_u8. */
static esp_err_t _read_block(rbamp_handle_t dev, uint8_t reg, uint8_t *out, size_t len)
{
    if (dev->std) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        err = i2c_master_transmit_receive(dev->dev, &reg, 1, out, len, CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        if (attempt + 1 < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS) _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
    }
    if (_recover_bus(dev)) {
        _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
        err = i2c_master_transmit_receive(dev->dev, &reg, 1, out, len, CONFIG_RBAMP_I2C_TIMEOUT_MS);
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
 * only one of two channels on a 2-module bus (bench finding #2). */
static esp_err_t _write_u8(rbamp_handle_t dev, uint8_t reg, uint8_t val)
{
    if (dev->std) return ESP_ERR_NOT_SUPPORTED;  /* BASIC-only path on an STD module */
    const uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        err = i2c_master_transmit(dev->dev, buf, 2, CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt + 1 < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS) {
            _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
        }
    }
    if (_recover_bus(dev)) {
        _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
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

/** @internal Read one channel-window field as float32 LE (senior UI5/UI7).
 *
 * The window data buffer (0x3C-0x3F) is a snapshot taken when SELECT (0x32) is
 * written, so we RE-SELECT before every read (device Rule #1). select+read is
 * one atomic section — the selector is a single shared device resource; a
 * handle must not be driven from two tasks without a caller-side mutex.
 * @p field is a CHFIELD_* code, @p ch a 0-based channel. */
static esp_err_t _read_window_field(rbamp_handle_t dev, uint8_t field,
                                    uint8_t ch, float *out, float max_abs)
{
    esp_err_t err = _write_u8(dev, RBAMP_V2_REG_CHANNEL_SELECT,
                              (uint8_t)((field << 4) | (ch & 0x0Fu)));
    if (err != ESP_OK) return err;
    return _read_float_le(dev, RBAMP_V2_REG_CHANNEL_DATA, out, max_abs);
}

/* Read one channel-window u8 field: re-select, then read the low data byte
 * (0x3C). RE-SELECT BEFORE EVERY READ — the data buffer is a snapshot taken at
 * select time, it does not track live values. */
static esp_err_t _read_window_u8(rbamp_handle_t dev, uint8_t field,
                                 uint8_t ch, uint8_t *out)
{
    esp_err_t err = _write_u8(dev, RBAMP_V2_REG_CHANNEL_SELECT,
                              (uint8_t)((field << 4) | (ch & 0x0Fu)));
    if (err != ESP_OK) return err;
    return _read_u8(dev, RBAMP_V2_REG_CHANNEL_DATA, out);
}

/* Write one channel-window field: select (field<<4)|ch, then the four data
 * bytes 0x3C-0x3F, ONE transaction each (firmware has no burst write). For the
 * u8 fields (CT_MODEL) only 0x3C carries the value and 0x3D-0x3F are zero; the
 * firmware applies the write on the terminal 0x3F transaction (SPEC
 * CHANNEL_DATA). */
static esp_err_t _write_window_field(rbamp_handle_t dev, uint8_t field,
                                     uint8_t ch, uint8_t val)
{
    esp_err_t err = _write_u8(dev, RBAMP_V2_REG_CHANNEL_SELECT,
                              (uint8_t)((field << 4) | (ch & 0x0Fu)));
    if (err != ESP_OK) return err;
    err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_CHANNEL_DATA + 0), val);
    if (err != ESP_OK) return err;
    err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_CHANNEL_DATA + 1), 0);
    if (err != ESP_OK) return err;
    err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_CHANNEL_DATA + 2), 0);
    if (err != ESP_OK) return err;
    return _write_u8(dev, (uint8_t)(RBAMP_V2_REG_CHANNEL_DATA + 3), 0);  /* applies */
}

/** @internal Sleep @c ms milliseconds via FreeRTOS. */
/* Tick-rounded sleep: pdMS_TO_TICKS rounds DOWN, so at the IDF default
 * 100 Hz tick a 5 ms request became 0 ticks = no delay at all (bench
 * 2026-10-11: the DRDY alarm check sampled the line inside the pulse). Any
 * non-zero request sleeps at least one tick; callers that need a hard
 * minimum measure against esp_timer instead (see the DRDY alarm wait). */
static inline void _sleep_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    if (ms && t == 0) t = 1;
    vTaskDelay(t);
}

/* -------------------------------------------------------------------------
 * STD line transport — 16-bit big-endian register pointer, auto-increment
 *
 * A write is [ptr_hi, ptr_lo, data...]; a read is [ptr_hi, ptr_lo] then a
 * repeated-start read of n bytes. Multi-byte fields are little-endian. The
 * module serves every read from a 128-byte coherent snapshot, so a block
 * (energy counters, a channel block, TOTALS) read in ONE transaction of at
 * most 128 bytes is consistent — split across transactions an i64 field can
 * tear (audit 2026-10-11, E5). Reads and data-register writes carry the
 * NACK-retry + bus-reset discipline of the 8-bit primitives (LESSONS L6);
 * a COMMAND write is never retried (see _std_cmd).
 * ------------------------------------------------------------------------- */

/* Largest burst per transaction = the module's coherent snapshot. Longer
 * reads are split with an explicit pointer per chunk (and lose coherence
 * across the split — keep decoded blocks within one chunk). */
#define RBAMP_STD_BURST_MAX 128u

static esp_err_t _std_txn_ex(rbamp_handle_t dev, const uint8_t *wbuf, size_t wlen,
                             uint8_t *rbuf, size_t rlen, bool retry)
{
    esp_err_t err = ESP_FAIL;
    if (!retry) {
        /* One attempt, no bus reset: COMMAND writes (a retry could land the
         * opcode twice) and polls during an expected flash stall (the poll
         * loop is the retry; a bus reset mid-stall only adds noise). */
        return rlen ? i2c_master_transmit_receive(dev->dev, wbuf, wlen, rbuf, rlen,
                                                  CONFIG_RBAMP_I2C_TIMEOUT_MS)
                    : i2c_master_transmit(dev->dev, wbuf, wlen, CONFIG_RBAMP_I2C_TIMEOUT_MS);
    }
    for (int attempt = 0; attempt < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        err = rlen ? i2c_master_transmit_receive(dev->dev, wbuf, wlen, rbuf, rlen,
                                                 CONFIG_RBAMP_I2C_TIMEOUT_MS)
                   : i2c_master_transmit(dev->dev, wbuf, wlen,
                                         CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        if (attempt + 1 < CONFIG_RBAMP_NACK_RETRY_ATTEMPTS) {
            _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
        }
    }
    if (_recover_bus(dev)) {
        _sleep_ms(CONFIG_RBAMP_NACK_RETRY_GAP_MS);
        err = rlen ? i2c_master_transmit_receive(dev->dev, wbuf, wlen, rbuf, rlen,
                                                 CONFIG_RBAMP_I2C_TIMEOUT_MS)
                   : i2c_master_transmit(dev->dev, wbuf, wlen,
                                         CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
    }
    dev->retry_exhaustion_count++;
    return err;
}

static inline esp_err_t _std_txn(rbamp_handle_t dev, const uint8_t *wbuf, size_t wlen,
                                 uint8_t *rbuf, size_t rlen)
{
    return _std_txn_ex(dev, wbuf, wlen, rbuf, rlen, true);
}

/** @internal Single-attempt read (no retry, no bus reset) — for polls while
 * the module may be stalled in a flash operation. */
static esp_err_t _std_read_once(rbamp_handle_t dev, uint16_t reg, uint8_t *buf, size_t n)
{
    const uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return _std_txn_ex(dev, ptr, 2, buf, n, false);
}

/** @internal Burst read of @p n bytes from 16-bit address @p reg. */
static esp_err_t _std_read(rbamp_handle_t dev, uint16_t reg, uint8_t *buf, size_t n)
{
    while (n) {
        const size_t k = (n > RBAMP_STD_BURST_MAX) ? RBAMP_STD_BURST_MAX : n;
        const uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
        esp_err_t err = _std_txn(dev, ptr, 2, buf, k);
        if (err != ESP_OK) return err;
        reg = (uint16_t)(reg + k);
        buf += k;
        n   -= k;
    }
    return ESP_OK;
}

/** @internal Burst write of @p n bytes to 16-bit address @p reg. */
static esp_err_t _std_write(rbamp_handle_t dev, uint16_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[2 + RBAMP_STD_BURST_MAX];
    while (n) {
        const size_t k = (n > RBAMP_STD_BURST_MAX) ? RBAMP_STD_BURST_MAX : n;
        buf[0] = (uint8_t)(reg >> 8);
        buf[1] = (uint8_t)reg;
        memcpy(&buf[2], data, k);
        esp_err_t err = _std_txn(dev, buf, 2 + k, NULL, 0);
        if (err != ESP_OK) return err;
        reg  = (uint16_t)(reg + k);
        data += k;
        n    -= k;
    }
    return ESP_OK;
}

/* Little-endian field decoders live in rbamp_std_decode.h (shared with the host tests). */
#define _le_u16 rbampi_le_u16
#define _le_i16 rbampi_le_i16
#define _le_u32 rbampi_le_u32
#define _le_i32 rbampi_le_i32
#define _le_i64 rbampi_le_i64

static esp_err_t _std_read_u8(rbamp_handle_t dev, uint16_t reg, uint8_t *out)
{
    return _std_read(dev, reg, out, 1);
}
static esp_err_t _std_read_u8_once(rbamp_handle_t dev, uint16_t reg, uint8_t *out)
{
    return _std_read_once(dev, reg, out, 1);
}
static esp_err_t _std_read_u16_once(rbamp_handle_t dev, uint16_t reg, uint16_t *out)
{
    uint8_t b[2];
    esp_err_t err = _std_read_once(dev, reg, b, 2);
    if (err == ESP_OK) *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return err;
}

static esp_err_t _std_read_u16(rbamp_handle_t dev, uint16_t reg, uint16_t *out)
{
    uint8_t b[2];
    esp_err_t err = _std_read(dev, reg, b, 2);
    if (err != ESP_OK) return err;
    *out = _le_u16(b);
    return ESP_OK;
}

static esp_err_t _std_read_u32(rbamp_handle_t dev, uint16_t reg, uint32_t *out)
{
    uint8_t b[4];
    esp_err_t err = _std_read(dev, reg, b, 4);
    if (err != ESP_OK) return err;
    *out = _le_u32(b);
    return ESP_OK;
}

static esp_err_t _std_write_u8(rbamp_handle_t dev, uint16_t reg, uint8_t val)
{
    return _std_write(dev, reg, &val, 1);
}

static esp_err_t _std_write_u32(rbamp_handle_t dev, uint16_t reg, uint32_t val)
{
    const uint8_t b[4] = { (uint8_t)val, (uint8_t)(val >> 8),
                           (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
    return _std_write(dev, reg, b, 4);
}

/** @internal Write an opcode to the STD COMMAND register — ONE attempt, no
 * transport retry: a retried COMMAND write can reach the module twice (the
 * second lands as OVERRUN or repeats a side effect). The caller sees the
 * transport error and decides. */
static esp_err_t _std_cmd(rbamp_handle_t dev, uint8_t opcode)
{
    const uint8_t buf[3] = { (uint8_t)(RBAMP_STD_REG_COMMAND >> 8), (uint8_t)RBAMP_STD_REG_COMMAND, opcode };
    return _std_txn_ex(dev, buf, sizeof buf, NULL, 0, false);
}

/* Command completion helpers (defined with the command API below). */
static esp_err_t _std_wait_idle(rbamp_handle_t dev, uint32_t timeout_ms, uint8_t opcode_for_log);
static esp_err_t _std_wait_done(rbamp_handle_t dev, uint32_t settle_ms, uint32_t timeout_ms,
                                bool tolerate_stall, uint8_t opcode_for_log);
static esp_err_t _std_cmd_sync(rbamp_handle_t dev, uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms);
static esp_err_t _std_raw_read(i2c_master_dev_handle_t h, uint16_t reg, uint8_t *buf, size_t n);

/** @internal Remember CMD_SEQ right before a COMMAND write so _std_wait_done
 * can tell this command's completion from the previous one's. */
static void _std_mark_seq(rbamp_handle_t dev)
{
    uint16_t seq = 0;
    if (dev->std_version >= 0x35 && _std_read_u16(dev, RBAMP_STD_REG_CMD_SEQ, &seq) == ESP_OK) {
        dev->std_cmd_seq = seq;
    }
}

/* Synchronous command verdict (map 0x34+): the module clears ERROR to OK when
 * it dispatches a command, so reading ERROR after the command's settle is the
 * verdict for THAT command — same discipline as the BASIC _check_write_error.
 * Commands run in the module's main loop, not in the I2C interrupt, so this
 * must be read AFTER the settle, never straight after the opcode write. On
 * maps < 0x34 the register is sticky and says nothing about this command. */
static esp_err_t _std_check_cmd_error(rbamp_handle_t dev)
{
    if (dev->std_version < 0x34) return ESP_OK;
    uint8_t e = 0;
    esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_ERROR, &e);
    if (err != ESP_OK) return err;
    switch (e) {
        case RBAMP_STD_DEV_ERR_OK:
            return ESP_OK;
        case RBAMP_STD_DEV_ERR_PARAM:
            ESP_LOGW(TAG, "module rejected command/config: ERR_PARAM (0x%02X)", e);
            return ESP_ERR_INVALID_ARG;
        case RBAMP_STD_DEV_ERR_UNKNOWN_CMD:
            ESP_LOGW(TAG, "module does not know this command (0x%02X)", e);
            return ESP_ERR_NOT_SUPPORTED;
        case RBAMP_STD_DEV_ERR_VALUE_CRC:
            ESP_LOGW(TAG, "module rejected the payload: CRC mismatch (0x%02X) — resend", e);
            return ESP_ERR_INVALID_CRC;
        case RBAMP_STD_DEV_ERR_PENDING:
            return ESP_ERR_TIMEOUT;               /* still executing (caller polls) */
        case RBAMP_STD_DEV_ERR_NVM_RATE_LIMIT:
            ESP_LOGW(TAG, "module NVM write rate-limited (0x%02X) — retry after ~5 s", e);
            return ESP_ERR_NOT_FINISHED;
        case RBAMP_STD_DEV_ERR_OP_RETRY:
            ESP_LOGW(TAG, "module busy with a staged operation (0x%02X) — retry later", e);
            return ESP_ERR_INVALID_STATE;
        case RBAMP_STD_DEV_ERR_NO_KEY:
        case RBAMP_STD_DEV_ERR_KEY_GATE:
        case RBAMP_STD_DEV_ERR_LEGAL_GATE:
            ESP_LOGW(TAG, "module refused: key / legal gate (0x%02X)", e);
            return ESP_ERR_NOT_ALLOWED;
        case RBAMP_STD_DEV_ERR_NVM_IO:
            ESP_LOGE(TAG, "module NVM write failed (0x%02X)", e);
            return ESP_FAIL;
        default:
            ESP_LOGW(TAG, "module reports ERROR=0x%02X", e);
            return ESP_ERR_INVALID_STATE;
    }
}

/* True for every ERROR value the map defines as a FINAL verdict (not PENDING,
 * not a byte from a lost pointer while the slave was stalled). */
static bool _std_err_is_verdict(uint8_t e)
{
    switch (e) {
        case RBAMP_STD_DEV_ERR_OK:
        case RBAMP_STD_DEV_ERR_NVM_IO:
        case RBAMP_STD_DEV_ERR_NO_KEY:
        case RBAMP_STD_DEV_ERR_LEGAL_GATE:
        case RBAMP_STD_DEV_ERR_NVM_RATE_LIMIT:
        case RBAMP_STD_DEV_ERR_OP_RETRY:
        case RBAMP_STD_DEV_ERR_KEY_GATE:
        case RBAMP_STD_DEV_ERR_VALUE_CRC:
        case RBAMP_STD_DEV_ERR_PARAM:
        case RBAMP_STD_DEV_ERR_UNKNOWN_CMD:
            return true;
        default:
            return false;
    }
}

/* Minimum wait between a latch request and reading the bank: the module
 * services the latch at its next measurement-window boundary (window 100 ms
 * from image 0x7C, up to ~160 ms on earlier images; 300 ms is the safe bound),
 * and its valid mask is sticky (set at the first latch ever, never cleared),
 * so elapsed time is the only freshness guarantee. */
#define RBAMP_STD_LATCH_SETTLE_MIN_MS RBAMP_STD_SETTLE_MS_LATCH_PERIOD   /* 300 from the map (yaml E7) */

/** @internal Latch one bank: target register first, opcode second — two
 * transactions, because the module dispatches the opcode from its main loop
 * and would race a burst that carried the bank byte after the opcode. */
static esp_err_t _std_latch_bank(rbamp_handle_t dev, uint8_t bank)
{
    esp_err_t err = _std_write_u8(dev, RBAMP_STD_REG_LATCH_BANK, bank);
    if (err != ESP_OK) return err;
    return _std_cmd(dev, RBAMP_STD_CMD_LATCH_PERIOD);
}


/** @internal Burst-read and decode one latch bank into @p out. */
static esp_err_t _std_read_latch_bank(rbamp_handle_t dev, uint8_t bank, rbamp_std_latch_t *out)
{
    memset(out, 0, sizeof(*out));
    out->bank     = bank;
    out->channels = dev->channels;

    uint8_t pd = 0;
    esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_PD_VALID, &pd);
    if (err != ESP_OK) return err;
    out->valid = (pd >> bank) & 1u;
    if (!out->valid) return ESP_ERR_INVALID_RESPONSE;

    const uint16_t base = RBAMP_STD_LATCH(bank);
    const size_t   len  = RBAMP_STD_LATCH_CHE_BASE
                        + (size_t)dev->channels * RBAMP_STD_LATCH_CHE_STRIDE;
    uint8_t b[RBAMP_STD_LATCH_CHE_BASE + RBAMP_MAX_CHANNELS * RBAMP_STD_LATCH_CHE_STRIDE];
    err = _std_read(dev, base, b, len);
    if (err != ESP_OK) return err;

    out->p_avg = (float)_le_i32(b + RBAMP_STD_LATCH_P)     * 1e-3f;
    out->q_avg = (float)_le_i32(b + RBAMP_STD_LATCH_Q)     * 1e-3f;
    out->s_avg = (float)_le_i32(b + RBAMP_STD_LATCH_S)     * 1e-3f;
    out->e_import_mws = _le_i64(b + RBAMP_STD_LATCH_E_POS);
    out->e_export_mws = _le_i64(b + RBAMP_STD_LATCH_E_NEG);
    out->nsamp     = _le_u32(b + RBAMP_STD_LATCH_NSAMP);
    out->win_count = _le_u32(b + RBAMP_STD_LATCH_WIN_CNT);
    out->max_p = (float)_le_i32(b + RBAMP_STD_LATCH_MAX_P) * 1e-3f;
    out->max_i = (float)_le_i32(b + RBAMP_STD_LATCH_MAX_I) * 1e-6f;
    out->max_u = (float)_le_i32(b + RBAMP_STD_LATCH_MAX_U) * 1e-3f;
    out->min_u = (float)_le_i32(b + RBAMP_STD_LATCH_MIN_U) * 1e-3f;
    for (uint8_t c = 0; c < dev->channels; ++c) {
        const uint8_t *p = b + RBAMP_STD_LATCH_CHE(c);
        out->e_import_ch_mws[c] = _le_i64(p + RBAMP_STD_LATCH_CHE_E_POS);
        out->e_export_ch_mws[c] = _le_i64(p + RBAMP_STD_LATCH_CHE_E_NEG);
        /* Contract: per-channel average P over the period = (E_pos + E_neg) / NSAMP [mW]. */
        out->p_avg_ch[c] = out->nsamp
            ? (float)((double)(out->e_import_ch_mws[c] + out->e_export_ch_mws[c])
                      / (double)out->nsamp) * 1e-3f
            : 0.0f;
    }
    return ESP_OK;
}

/** @internal One scaled fixed-point STD field (i16 or i32 LE) as float, with
 * the same plausibility gate as the BASIC float readers. */
static esp_err_t _std_read_scaled(rbamp_handle_t dev, uint16_t addr, bool is_i16,
                                  float scale, float *out, float max_abs)
{
    uint8_t b[4];
    esp_err_t err = _std_read(dev, addr, b, is_i16 ? 2u : 4u);
    if (err != ESP_OK) return err;
    const float v = is_i16 ? (float)_le_i16(b) * scale : (float)_le_i32(b) * scale;
    if (fabsf(v) > max_abs) {
        dev->sanity_reject_count++;
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = v;
    return ESP_OK;
}

/* STD identity probe — one burst of the identity block 0x0000-0x0009.
 *
 * Issued FIRST in rbamp_begin, before any 8-bit transaction, because the
 * lines cannot be told apart from the BASIC side: an STD module takes a lone
 * pointer byte as the pointer HIGH byte and answers from 0xNN00 with a
 * plausible non-zero value, so the BASIC VERSION probe would pass. This probe
 * is benign on a BASIC module: the pointer pair lands as "reg 0x00 <- 0x00",
 * a write to the read-only STATUS byte that the BASIC firmware drops without
 * raising REG_ERROR, and the 10-byte read returns its control block, which can
 * never spell the 'S','T' magic. */
static esp_err_t _std_detect(rbamp_handle_t dev, bool *is_std)
{
    uint8_t id[10];
    *is_std = false;
    esp_err_t err = _std_read(dev, RBAMP_STD_REG_ID, id, sizeof id);
    if (err != ESP_OK) return err;
    if (id[0] != 'S' || id[1] != 'T') return ESP_OK;   /* BASIC line */

    const uint8_t  ver  = id[RBAMP_STD_REG_VERSION];
    const uint16_t caps = _le_u16(&id[RBAMP_STD_REG_CAPABILITIES]);
    if (ver < 0x30 || !(caps & RBAMP_STD_CAP_ADDR16)) {
        ESP_LOGE(TAG, "STD magic but map version 0x%02X / caps 0x%04X not supported",
                 ver, caps);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ver > RBAMP_STD_MAP_VERSION) {
        ESP_LOGW(TAG, "STD map version 0x%02X is newer than this component (0x%02X)",
                 ver, (unsigned)RBAMP_STD_MAP_VERSION);
    }
    dev->std         = true;
    dev->std_version = ver;
    dev->std_caps    = caps;
    dev->std_status  = id[RBAMP_STD_REG_STATUS];
    dev->n_voltage   = id[RBAMP_STD_REG_N_VOLTAGE];
    dev->n_current   = id[RBAMP_STD_REG_N_CURRENT];
    dev->std_variant = id[RBAMP_STD_REG_VARIANT];
    *is_std = true;
    return ESP_OK;
}

/** @internal Map the STD VARIANT byte onto the public variant enum. */
static rbamp_variant_t _std_variant_map(uint8_t v)
{
    switch (v) {
        case RBAMP_STD_VARIANT_UI1:  return RBAMP_VARIANT_UI1;
        case RBAMP_STD_VARIANT_UI2:  return RBAMP_VARIANT_UI2;
        case RBAMP_STD_VARIANT_UI3:  return RBAMP_VARIANT_UI3;
        case RBAMP_STD_VARIANT_I1:   return RBAMP_VARIANT_I1;
        case RBAMP_STD_VARIANT_U3I3: return RBAMP_VARIANT_U3I3;
        case RBAMP_STD_VARIANT_U3I6: return RBAMP_VARIANT_U3I6;
        default:                     return RBAMP_VARIANT_UNKNOWN;
    }
}

static esp_err_t _std_prime_period(rbamp_handle_t dev);

/** @internal Finish begin() for an STD module: adopt discovery counts. */
static esp_err_t _std_begin(rbamp_handle_t dev)
{
    /* Channel count is discovery truth on this line. Registers of channels at
     * or beyond N_CURRENT are unspecified on the module, so a caller pin
     * cannot "over-detect harmlessly" the way it can on BASIC: N_CURRENT wins
     * and an explicit pin only earns a warning. */
    uint8_t ch = dev->n_current;
    if (ch == 0) {
        ESP_LOGE(TAG, "STD module reports 0 current channels");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ch > RBAMP_MAX_CHANNELS) {
        ESP_LOGW(TAG, "STD module has %u current channels; this component exposes "
                      "the first %u", ch, (unsigned)RBAMP_MAX_CHANNELS);
        ch = RBAMP_MAX_CHANNELS;
    }
    if (dev->topology_user_pinned && dev->channels != ch) {
        ESP_LOGW(TAG, "caller pinned %u channels but the STD module reports %u — "
                      "using the module's count", dev->channels, ch);
    }
    dev->channels       = ch;
    dev->topology       = (rbamp_topology_t)ch;  /* may be a count outside the BASIC enum (4/6) */
    dev->has_voltage_hw = dev->n_voltage > 0;
    dev->variant        = _std_variant_map(dev->std_variant);
    dev->capability     = 0;                     /* BASIC bitmap; STD caps via rbamp_std_info */
    dev->std_active_bank   = 0;
    dev->std_ota_bank_size = 0;
    if (dev->std_caps & RBAMP_STD_CAP_OTA) {
        (void)_std_read_u32(dev, RBAMP_STD_REG_OTA_ACTIVE, &dev->std_active_bank);
        /* Bank size is device-reported (differs per module family) — the
         * image helper derives the header position from it at runtime. */
        uint32_t bs = 0;
        if (_std_read_u32(dev, RBAMP_STD_REG_OTA_BANK_SIZE, &bs) == ESP_OK
                && bs != 0 && bs != 0xFFFFFFFFu
                && (bs % RBAMP_STD_OTA_PAGE_SIZE) == 0 && bs >= 2u * RBAMP_STD_OTA_PAGE_SIZE) {
            dev->std_ota_bank_size = bs;
        }
    }
    /* Period/energy mechanism is a capability, not a line property: the
     * first STD modules had latch banks, the current firmware family keeps
     * persisted counters instead (CMD_LATCH_PERIOD there is an unknown
     * command and the bank region is unmapped). Decide once, here, so no
     * latch traffic ever reaches a module without banks. */
    dev->std_period_src = (dev->std_caps & RBAMP_STD_CAP_MULTI_LATCH)    ? RBAMP_STD_PERIOD_LATCH
                        : (dev->std_caps & RBAMP_STD_CAP_ONDEVICE_ENERGY) ? RBAMP_STD_PERIOD_COUNTERS
                        : RBAMP_STD_PERIOD_NONE;
    dev->std_fw_ver = 0;
    if (dev->std_version >= 0x35) {
        uint32_t fv = 0;
        if (_std_read_u32(dev, RBAMP_STD_REG_FW_VER, &fv) == ESP_OK && fv != 0xFFFFFFFFu) dev->std_fw_ver = fv;
    }
    ESP_LOGI(TAG, "STD module: map=0x%02X variant=0x%02X V=%u I=%u caps=0x%04X status=0x%02X period=%s",
             dev->std_version, dev->std_variant, dev->n_voltage, dev->n_current,
             dev->std_caps, dev->std_status,
             dev->std_period_src == RBAMP_STD_PERIOD_LATCH    ? "latch-banks" :
             dev->std_period_src == RBAMP_STD_PERIOD_COUNTERS ? "counters" : "none");
    if (dev->std_period_src == RBAMP_STD_PERIOD_NONE) {
        ESP_LOGW(TAG, "module has neither latch banks nor on-device energy counters — "
                      "period / energy calls will return NOT_SUPPORTED");
    }
    /* Latch the event flags as found at begin (RESET_OCCURRED tells the caller
     * the module rebooted before we met it); nothing is cleared here. */
    if (dev->std_version >= 0x36) {
        (void)_std_read_u8(dev, RBAMP_STD_REG_EVENT_FLAGS, &dev->begin_event_flags);
        if (dev->begin_event_flags == 0xFF) dev->begin_event_flags = 0;
    }

    return _std_prime_period(dev);
}

/* Modules without latch banks keep persisted life-time counters instead
 * (whole Wh, monotonic, survive power loss). The ENERGY fields and LT_GEN
 * sit in one burst; UPTIME is read separately. */
static esp_err_t _std_read_energy_raw(rbamp_handle_t dev, rbamp_std_energy_t *out)
{
    if (!(dev->std_caps & RBAMP_STD_CAP_ONDEVICE_ENERGY)) return ESP_ERR_NOT_SUPPORTED;
    _Static_assert(RBAMP_STD_REG_LT_GEN >= RBAMP_STD_ENERGY_BASE + RBAMP_STD_ENERGY_E_REA_EXP + 8
                   && RBAMP_STD_REG_LT_GEN + 4 <= RBAMP_STD_ENERGY_BASE + RBAMP_STD_ENERGY_UPTIME,
                   "ENERGY block / LT_GEN layout changed - split the burst");
    uint8_t b[RBAMP_STD_REG_LT_GEN + 4 - RBAMP_STD_ENERGY_BASE];
    esp_err_t err = _std_read(dev, RBAMP_STD_ENERGY_BASE, b, sizeof b);
    if (err != ESP_OK) return err;
    uint32_t up = 0;
    err = _std_read_u32(dev, RBAMP_STD_ENERGY_BASE + RBAMP_STD_ENERGY_UPTIME, &up);
    if (err != ESP_OK) return err;
    if (up == 0xFFFFFFFFu) return ESP_ERR_INVALID_RESPONSE;   /* unprogrammed / unmapped block */
    out->act_import_wh   = _le_i64(b + RBAMP_STD_ENERGY_E_ACT_IMP);
    out->act_export_wh   = _le_i64(b + RBAMP_STD_ENERGY_E_ACT_EXP);
    out->act_net_wh      = _le_i64(b + RBAMP_STD_ENERGY_E_ACT_NET);
    out->rea_import_varh = _le_i64(b + RBAMP_STD_ENERGY_E_REA_IMP);
    out->rea_export_varh = _le_i64(b + RBAMP_STD_ENERGY_E_REA_EXP);
    out->generation      = _le_u32(b + (RBAMP_STD_REG_LT_GEN - RBAMP_STD_ENERGY_BASE));
    out->uptime_s        = up;
    out->channels        = 0;
    out->group_status    = 0;
    const bool have_mwh = dev->std_version >= 0x36;
    /* µ-resolution counters: CAPABILITIES bit 15 (image 0x7A); reactive µvarh from 0x7B. */
    const bool fine     = have_mwh && (dev->std_caps & RBAMP_STD_CAP_ENERGY_UWH) != 0;
    const bool fine_rea = fine && dev->std_fw_ver >= RBAMPI_FW_UVARH_MIN;
    uint8_t n = 0;
    if (have_mwh) {
        /* Map 0x36: mWh totals in the same block, per-channel counters in their own. */
        _Static_assert(RBAMP_STD_ENERGY_E_ACT_NET_MWH + 8 <= RBAMP_STD_REG_LT_GEN - RBAMP_STD_ENERGY_BASE,
                       "mWh fields must sit inside the ENERGY burst");
        out->act_import_mwh = _le_i64(b + RBAMP_STD_ENERGY_E_ACT_IMP_MWH);
        out->act_export_mwh = _le_i64(b + RBAMP_STD_ENERGY_E_ACT_EXP_MWH);
        out->act_net_mwh    = _le_i64(b + RBAMP_STD_ENERGY_E_ACT_NET_MWH);
        n = dev->channels;
        if (n > RBAMP_STD_GROUP_ENERGY_MWH_MAX) n = RBAMP_STD_GROUP_ENERGY_MWH_MAX;
        uint8_t g[RBAMP_MAX_CHANNELS * RBAMP_STD_GROUP_ENERGY_MWH_STRIDE];
        err = _std_read(dev, RBAMP_STD_GROUP_ENERGY_MWH_BASE, g, (size_t)n * RBAMP_STD_GROUP_ENERGY_MWH_STRIDE);
        if (err != ESP_OK) return err;
        for (uint8_t c = 0; c < n; ++c) {
            const uint8_t *p = g + (size_t)c * RBAMP_STD_GROUP_ENERGY_MWH_STRIDE;
            out->ch_import_mwh[c] = _le_i64(p + RBAMP_STD_GROUP_ENERGY_MWH_E_IMP);
            out->ch_export_mwh[c] = _le_i64(p + RBAMP_STD_GROUP_ENERGY_MWH_E_EXP);
        }
        out->channels = n;
        err = _std_read_u8(dev, RBAMP_STD_REG_LTG_STATUS, &out->group_status);
        if (err != ESP_OK) return err;
    }
    if (fine) {
        /* ENERGY +0xD0..+0xF7 in ONE transaction (coherent snapshot), then the
         * per-channel µWh block in one transaction (≤ 7 × 16 B). */
        _Static_assert(RBAMPI_ENERGY_FINE_LEN == 40 && RBAMPI_ENERGY_FINE_LEN <= RBAMP_STD_BURST_MAX,
                       "fine energy burst layout changed");
        _Static_assert(RBAMP_MAX_CHANNELS * RBAMP_STD_GROUP_ENERGY_UWH_STRIDE <= RBAMP_STD_BURST_MAX,
                       "per-channel µWh block must fit one transaction");
        uint8_t f[RBAMPI_ENERGY_FINE_LEN];
        err = _std_read(dev, RBAMP_STD_ENERGY_BASE + RBAMPI_ENERGY_FINE_OFF, f, sizeof f);
        if (err != ESP_OK) return err;
        rbampi_energy_decode_fine(out, f, fine_rea);
        if (n) {
            uint8_t gu[RBAMP_MAX_CHANNELS * RBAMP_STD_GROUP_ENERGY_UWH_STRIDE];
            err = _std_read(dev, RBAMP_STD_GROUP_ENERGY_UWH_BASE, gu, (size_t)n * RBAMP_STD_GROUP_ENERGY_UWH_STRIDE);
            if (err != ESP_OK) return err;
            rbampi_energy_decode_group_fine(out, gu, n);
        }
    }
    rbampi_energy_fill_coarse(out, fine, fine_rea, have_mwh);
    return ESP_OK;
}

/* Take the counter-path anchor: the reading every later period is measured from. */
static esp_err_t _std_lt_anchor(rbamp_handle_t dev)
{
    esp_err_t err = _std_read_energy_raw(dev, &dev->std_lt_anchor);
    if (err != ESP_OK) return err;
    dev->std_lt_anchored = true;
    dev->last_latch_us   = esp_timer_get_time();
    dev->have_last_latch = true;
    return ESP_OK;
}

/* Arm one latch bank if its bit is clear (read-modify-write so the caller's
 * other banks stay armed). *was_armed tells whether it already was. */
static esp_err_t _std_ensure_armed(rbamp_handle_t dev, uint8_t bank, bool *was_armed)
{
    uint8_t arm = 0;
    esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_PERIOD_ARM, &arm);
    if (err != ESP_OK) return err;
    const uint8_t bit = (uint8_t)(1u << bank);
    *was_armed = (arm & bit) != 0;
    if (*was_armed) return ESP_OK;
    return _std_write_u8(dev, RBAMP_STD_REG_PERIOD_ARM, (uint8_t)(arm | bit));
}

/* (Re)start the library's period path on bank 0: arm it, issue the same
 * primer latch as BASIC so the next snapshot covers a full period, and anchor
 * the master clock. Called from begin, after the library's own resets, and
 * whenever a snapshot shows the bank was disarmed behind our back. The arm
 * mask is RAM on the module — ANY module reset (power, watchdog, OTA, a reset
 * command from another master) clears it, and a latch of an unarmed bank
 * yields an empty snapshot forever, so arming in begin() alone is not enough
 * (E15, found by the esphome HIL). */
static esp_err_t _std_prime_period(rbamp_handle_t dev)
{
    switch (dev->std_period_src) {
    case RBAMP_STD_PERIOD_LATCH: {
        bool was_armed = false;
        esp_err_t err = _std_ensure_armed(dev, 0, &was_armed);
        if (err != ESP_OK) return err;
        err = _std_latch_bank(dev, 0);
        if (err != ESP_OK) return err;
        dev->last_latch_us   = esp_timer_get_time();
        dev->have_last_latch = true;
        return ESP_OK;
    }
    case RBAMP_STD_PERIOD_COUNTERS:
        /* Persisted counters need no re-priming after a reset the library
         * issued: the anchor stays valid across it (the next period just
         * reports module_restarted). Only a fresh handle takes an anchor. */
        return dev->std_lt_anchored ? ESP_OK : _std_lt_anchor(dev);
    default:
        return ESP_OK;   /* no period path on this module; each call refuses on its own */
    }
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

static esp_err_t _new(i2c_master_bus_handle_t bus, uint8_t addr,
                      rbamp_topology_t topology, bool pinned, rbamp_handle_t *out);

esp_err_t rbamp_new(i2c_master_bus_handle_t bus, uint8_t addr, rbamp_handle_t *out)
{
    /* Not pinned: begin() adopts the channel count from HW_VARIANT (0x55).
     * THREE_PHASE is only the fallback for firmware that reads 0 there —
     * over-detection is harmless because unused channels read 0.0 (SPEC §8). */
    return _new(bus, addr, RBAMP_TOPOLOGY_THREE_PHASE, false, out);
}

esp_err_t rbamp_new_with_topology(i2c_master_bus_handle_t bus, uint8_t addr,
                                  rbamp_topology_t topology, rbamp_handle_t *out)
{
    return _new(bus, addr, topology, true, out);
}

static esp_err_t _new(i2c_master_bus_handle_t bus, uint8_t addr,
                      rbamp_topology_t topology, bool pinned, rbamp_handle_t *out)
{
    if (!out || !bus || addr < 0x08 || addr > 0x77) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (topology) {  /* channel COUNT — non-contiguous, no 4/6 (SPEC §8) */
        case RBAMP_TOPOLOGY_SINGLE:
        case RBAMP_TOPOLOGY_SPLIT_PHASE:
        case RBAMP_TOPOLOGY_THREE_PHASE:
        case RBAMP_TOPOLOGY_FIVE_CH:
        case RBAMP_TOPOLOGY_SEVEN_CH:
            break;
        default:
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
    dev->topology_user_pinned = pinned;
    dev->has_voltage_hw = false;
    dev->variant = RBAMP_VARIANT_UNKNOWN;
    dev->capability = 0;
    dev->digest_mask = 0;
    dev->digest_armed = false;
    dev->digest_reset_seen = false;
    dev->basic_have_uptime = false;
    dev->addr_change_armed = false;
    dev->have_last_latch = false;
    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) {
        dev->sct[ch].status = RBAMP_SCT_PENDING;
        dev->sct[ch].scale  = 1.0f;
    }
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

/** @internal HW_VARIANT (0x55) → variant, voltage presence and, unless the
 * caller pinned a topology (rbamp_new_with_topology), the channel count.
 * Policy lives in rbampi_basic_variant_apply (host-tested). */
static void _apply_variant(rbamp_handle_t dev, uint8_t hw)
{
    const rbampi_variant_result_t r = rbampi_basic_variant_apply(
            hw, dev->topology_user_pinned, &dev->channels, &dev->has_voltage_hw, &dev->variant);
    if (r == RBAMPI_VARIANT_KEPT_PIN) {
        uint8_t ch = 0;
        (void)rbampi_basic_variant_shape(hw, &ch, NULL);
        ESP_LOGW(TAG, "device reports %u channels (variant %u) but caller "
                      "pinned %u — honoring caller pin", ch, hw, dev->channels);
    } else if (r == RBAMPI_VARIANT_APPLIED) {
        dev->topology = (rbamp_topology_t)dev->channels;
    }
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
            && rbampi_basic_variant_shape(hw, NULL, NULL)) {
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

static esp_err_t _begin(rbamp_handle_t dev, bool resume);
static esp_err_t _sct_refresh(rbamp_handle_t dev);
static esp_err_t _sct_after_bind(rbamp_handle_t dev);

esp_err_t rbamp_begin(rbamp_handle_t dev)
{
    return _begin(dev, false);
}

esp_err_t rbamp_begin_resume(rbamp_handle_t dev)
{
    return _begin(dev, true);
}

static esp_err_t _begin(rbamp_handle_t dev, bool resume)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    dev->basic_resume_pending = false;
    dev->digest_armed = false;          /* module RAM may be fresh; re-write the mask on first use */
    dev->digest_reset_seen = false;
    dev->basic_have_uptime = false;

    /* 0. Product line. The STD identity probe goes first (see _std_detect for
     *    why the order is not negotiable). A re-run of begin() re-detects. */
    dev->std = false;
    dev->std_lt_anchored = false;   /* a re-begin may face another module */
    dev->begin_event_flags = 0;
    dev->std_have_last_avg = false;
    bool is_std = false;
    esp_err_t err = _std_detect(dev, &is_std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe failed at 0x%02X: %s", dev->addr, esp_err_to_name(err));
        return err;
    }
    if (is_std) return _std_begin(dev);

    /* 1. BASIC line — probe REG_VERSION */
    uint8_t version = 0;
    err = _read_u8(dev, RBAMP_REG_VERSION, &version);
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
     *    the master's clock. rbamp_begin_resume skips this: the module kept
     *    accumulating while the master was away, and the first consumed
     *    snapshot integrates over the module's own commit-counted window. */
    if (resume) {
        dev->have_last_latch      = false;
        dev->basic_resume_pending = true;
    } else {
        err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
        if (err != ESP_OK) return err;
        dev->last_latch_us = esp_timer_get_time();
        dev->have_last_latch = true;
        _sleep_ms(RBAMP_SETTLE_MS_LATCH_PERIOD);
    }

    /* BASIC: latch the event flags as found at begin (nothing cleared). */
    (void)_read_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, &dev->begin_event_flags);

    /* 4. SCT-013 rating switch: read class / models / applied gains and set
     *    the per-channel scale (one CMD_RESET if a model was switched in this
     *    boot without a restart). */
    dev->sct_stale = true;
    return _sct_refresh(dev);
}

esp_err_t rbamp_probe(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        uint8_t id[2] = {0, 0};
        esp_err_t err = _std_read(dev, RBAMP_STD_REG_ID, id, 2);
        if (err != ESP_OK) return err;
        return (id[0] == 'S' && id[1] == 'T') ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t v = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &v);
    if (err != ESP_OK) return err;
    if (v == 0 || v == 0xFF) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

esp_err_t rbamp_wait_ready(rbamp_handle_t dev, uint32_t timeout_ms)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* BASIC: poll REG_STATUS (0x00) bit0=READY — sticky/non-destructive.
     * NOT REG_V03_STATUS (0xCE) which is "cleared on read" and races with
     * the firmware's commit thread.
     * STD: the host path comes up before the sensor path, so "ready" means
     * STATUS.sensor_ok — the module is producing measurements. */
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t status = 0;
        if (dev->std) {
            if (_std_read_u8(dev, RBAMP_STD_REG_STATUS, &status) == ESP_OK
                    && (status & RBAMP_STD_ST_SENSOR_OK)) {
                return ESP_OK;
            }
        } else if (_read_u8(dev, RBAMP_REG_STATUS, &status) == ESP_OK
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
    if (dev->std) {
        return (_std_read_u8(dev, RBAMP_STD_REG_VERSION, &v) == ESP_OK) ? v : 0;
    }
    return (_read_u8(dev, RBAMP_REG_VERSION, &v) == ESP_OK) ? v : 0;
}

rbamp_line_t rbamp_line(rbamp_handle_t dev)
{
    return (dev && dev->std) ? RBAMP_LINE_STD : RBAMP_LINE_BASIC;
}

rbamp_topology_t rbamp_topology(rbamp_handle_t dev)
{
    return dev ? dev->topology : RBAMP_TOPOLOGY_SINGLE;
}

uint8_t rbamp_channels(rbamp_handle_t dev)
{
    return dev ? dev->channels : 0;
}

rbamp_variant_t rbamp_variant(rbamp_handle_t dev)
{
    return dev ? dev->variant : RBAMP_VARIANT_UNKNOWN;
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
    if (dev->std) {
        if (phase >= dev->n_voltage) return ESP_ERR_INVALID_ARG;
        return _std_read_scaled(dev, RBAMP_STD_VOLTAGE(phase) + RBAMP_STD_VOLTAGE_U_RMS,
                                false, 1e-3f, out, RBAMP_SANITY_LIMIT_V);
    }
    if (phase != 0) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_U_RMS, out, RBAMP_SANITY_LIMIT_V);
}

esp_err_t rbamp_read_voltage_peak(rbamp_handle_t dev, uint8_t phase, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (phase >= dev->n_voltage) return ESP_ERR_INVALID_ARG;
        return _std_read_scaled(dev, RBAMP_STD_VOLTAGE(phase) + RBAMP_STD_VOLTAGE_U_PEAK,
                                false, 1e-3f, out, RBAMP_SANITY_LIMIT_V);
    }
    if (phase != 0) return ESP_ERR_INVALID_ARG;
    return _read_float_le(dev, RBAMP_REG_V03_U_PEAK, out, RBAMP_SANITY_LIMIT_V);
}

/* Per-channel scalar quantities and where each lives on each line. ONE table +
 * ONE router (_read_ch_float) so a new quantity is a table row, and no public
 * accessor can reintroduce its own routing (LESSONS L11, PUBLIC_API_TRUTH
 * channel-access canon). BASIC: ch0-2 flat register (flat_base + ch*4), ch3+
 * channel window (chfield); a quantity the BASIC line lacks has chfield 0xFF.
 * STD: fixed-point field at RBAMP_STD_CURRENT(ch) + std_off, scaled to SI. */
/* -------------------------------------------------------------------------
 * SCT-013 rating switch (libs/spec/SCT_RATING_SWITCH.md)
 *
 * BASIC firmware up to 1.4.19 applies the factory (030) gain after any restart
 * whatever model the user bound, and the raw preset (trim lost) right after a
 * model write. G/NF are readable in user mode, not writable, so the library
 * scales its outputs by preset(X) x c(X) / preset(030) instead. The decision
 * is pure (rbampi_sct_decide, host-tested); this is the transport around it.
 * ------------------------------------------------------------------------- */

#define RBAMPI_SCT_GAIN_MAX     100.0f   /* plausibility bound for an applied gain */
#define RBAMPI_SCT_REPROBE_MS   2000u    /* the module must answer again within this after CMD_RESET */

static inline float _sct_scale(rbamp_handle_t dev, uint8_t ch)
{
    return (ch < RBAMP_MAX_CHANNELS) ? dev->sct[ch].scale : 1.0f;
}

/** @internal Scale shared by every channel, or 1 when the channels differ
 * (used for module-wide quantities such as the period max power). */
static float _sct_common_scale(rbamp_handle_t dev)
{
    const float s = dev->sct[0].scale;
    for (uint8_t ch = 1; ch < dev->channels && ch < RBAMP_MAX_CHANNELS; ++ch)
        if (dev->sct[ch].scale != s) return 1.0f;
    return s;
}

/** @internal CMD_RESET, wait for the module to answer again, verify the same
 * UID, acknowledge our own RESET_OCCURRED and redo what begin() sets up
 * (primer latch, digest mask on next use). */
static esp_err_t _sct_module_restart(rbamp_handle_t dev)
{
    uint8_t uid_before[12] = {0}, uid_after[12] = {0};
    esp_err_t err = rbamp_read_uid(dev, uid_before);
    if (err != ESP_OK) return err;
    err = _write_cmd(dev, RBAMP_CMD_RESET);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_SETTLE_MS_RESET);
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)RBAMPI_SCT_REPROBE_MS * 1000;
    for (;;) {
        uint8_t v = 0;
        err = _read_u8(dev, RBAMP_REG_VERSION, &v);
        if (err == ESP_OK && v != 0 && v != 0xFF) break;
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGE(TAG, "sct: the module did not answer within %u ms of its restart", (unsigned)RBAMPI_SCT_REPROBE_MS);
            return (err == ESP_OK) ? ESP_ERR_TIMEOUT : err;
        }
        _sleep_ms(50);
    }
    err = rbamp_read_uid(dev, uid_after);
    if (err != ESP_OK) return err;
    if (memcmp(uid_before, uid_after, sizeof uid_before) != 0) {
        ESP_LOGE(TAG, "sct: a different module answered after the restart");
        return ESP_ERR_INVALID_RESPONSE;
    }
    (void)_write_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, RBAMP_V2_EVENT_RESET_OCCURRED);   /* our own restart */
    dev->digest_armed       = false;
    dev->digest_reset_seen  = false;
    dev->basic_have_uptime  = false;
    dev->basic_resume_pending = false;
    err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);     /* primer latch, as in begin() */
    if (err != ESP_OK) return err;
    dev->last_latch_us   = esp_timer_get_time();
    dev->have_last_latch = true;
    _sleep_ms(RBAMP_SETTLE_MS_LATCH_PERIOD);
    return ESP_OK;
}

static esp_err_t _sct_refresh(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    dev->sct_stale = false;
    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) {
        dev->sct[ch] = (rbamp_sct_correction_t){ .status = RBAMP_SCT_NONE, .scale = 1.0f };
    }
    if (dev->std) return ESP_OK;
#if !CONFIG_RBAMP_SCT_RATING_CORRECTION
    return ESP_OK;
#else
    if (dev->capability & RBAMPI_CAP_SCT_RATING_SELF) return ESP_OK;   /* the module corrects itself */
    uint8_t cls = 0;
    esp_err_t err = rbamp_read_sensor_class(dev, &cls);
    if (err != ESP_OK) return err;
    if (cls != (uint8_t)RBAMP_SENSOR_SCT013) return ESP_OK;                   /* factory pair: leave alone */
    bool after_reset = false;
    for (;;) {
        bool need_reset = false;
        for (uint8_t ch = 0; ch < dev->channels && ch < RBAMP_MAX_CHANNELS; ++ch) {
            uint8_t model = 0;
            float   g     = 0.0f;
            err = rbamp_read_ct_model_ch(dev, ch, &model);
            if (err != ESP_OK) return err;
            err = (ch < 3) ? _read_float_le(dev, (uint8_t)(RBAMP_REG_V03_I0_GAIN + 4u * ch), &g, RBAMPI_SCT_GAIN_MAX)
                           : _read_window_field(dev, RBAMPI_CHFIELD_GAIN, ch, &g, RBAMPI_SCT_GAIN_MAX);
            if (err == ESP_ERR_INVALID_RESPONSE) g = 0.0f;   /* implausible gain -> GAIN_UNEXPECTED below */
            else if (err != ESP_OK) return err;
            bool  nr = false;
            float sc = 1.0f;
            const rbamp_sct_status_t st = rbampi_sct_decide(dev->capability, cls, model, g, after_reset, &nr, &sc);
            dev->sct[ch] = (rbamp_sct_correction_t){ .status = st, .model = model, .scale = sc, .gain_applied = g };
            need_reset = need_reset || nr;
        }
        if (!need_reset || after_reset) break;
        ESP_LOGW(TAG, "sct: a sensor model was switched in this boot (applied gain = raw preset) — "
                      "restarting the module once to return to its calibrated gain");
        err = _sct_module_restart(dev);
        if (err != ESP_OK) return err;
        after_reset = true;
    }
    const float cal = rbampi_sct_preset_gain(RBAMPI_SCT_CAL_MODEL);
    for (uint8_t ch = 0; ch < dev->channels && ch < RBAMP_MAX_CHANNELS; ++ch) {
        const rbamp_sct_correction_t *c = &dev->sct[ch];
        switch (c->status) {
            case RBAMP_SCT_CORRECTED:
                ESP_LOGI(TAG, "ch%u: SCT-013 model %u on a 030-calibrated module (trim %+.1f %%): readings scaled x%.3f",
                         ch, c->model, (double)((c->gain_applied / cal - 1.0f) * 100.0f), (double)c->scale);
                break;
            case RBAMP_SCT_UNCALIBRATED:
                ESP_LOGW(TAG, "ch%u: model %u — the module applies the raw preset even after a restart: "
                              "unit trim unknown, readings not corrected", ch, c->model);
                break;
            case RBAMP_SCT_GAIN_UNEXPECTED:
                ESP_LOGW(TAG, "ch%u: model %u — applied gain %.4f is not within 5 %% of the 030 preset: "
                              "unexpected factory gain, readings not corrected", ch, c->model, (double)c->gain_applied);
                break;
            case RBAMP_SCT_UNKNOWN_MODEL:
                ESP_LOGW(TAG, "ch%u: model code %u has no known preset: readings not corrected", ch, c->model);
                break;
            default: break;
        }
    }
    return ESP_OK;
#endif
}

/** @internal After a model bind + save on an affected module: restart the
 * module (the only way back to its calibrated gain, spec step 1), then
 * re-evaluate. On other modules just re-evaluate. */
static esp_err_t _sct_after_bind(rbamp_handle_t dev)
{
#if CONFIG_RBAMP_SCT_RATING_CORRECTION
    if (!dev->std && !(dev->capability & RBAMPI_CAP_SCT_RATING_SELF)) {
        uint8_t cls = 0;
        if (rbamp_read_sensor_class(dev, &cls) == ESP_OK && cls == (uint8_t)RBAMP_SENSOR_SCT013) {
            esp_err_t err = _sct_module_restart(dev);
            if (err != ESP_OK) return err;
        }
    }
#endif
    dev->sct_stale = true;
    return _sct_refresh(dev);
}

esp_err_t rbamp_sct_correction(rbamp_handle_t dev, uint8_t ch, rbamp_sct_correction_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (ch >= dev->channels || ch >= RBAMP_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;
    *out = dev->sct[ch];
    return ESP_OK;
}

esp_err_t rbamp_sct_refresh(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    dev->sct_stale = true;
    return _sct_refresh(dev);
}

typedef enum { _Q_I_RMS, _Q_I_PEAK, _Q_P, _Q_PF, _Q_Q, _Q_S } _qty_t;

static const struct {
    uint8_t chfield;    /* BASIC window field, 0xFF = not on BASIC */
    uint8_t flat_base;  /* BASIC ch0 flat register */
    uint8_t std_off;    /* STD offset within the CURRENT block */
    bool    std_i16;    /* STD field width: i16 (else i32) */
    float   std_scale;  /* STD wire unit -> SI */
    float   max_abs;    /* plausibility bound */
} kQty[] = {
    [_Q_I_RMS]  = { RBAMP_V2_CHFIELD_I_RMS,  RBAMP_REG_V03_I0_RMS,  RBAMP_STD_CURRENT_I_RMS,  false, 1e-6f,          RBAMP_SANITY_LIMIT_I  },
    [_Q_I_PEAK] = { RBAMP_V2_CHFIELD_I_PEAK, RBAMP_REG_V03_I0_PEAK, RBAMP_STD_CURRENT_I_PEAK, false, 1e-6f,          RBAMP_SANITY_LIMIT_I  },
    [_Q_P]      = { RBAMP_V2_CHFIELD_P_REAL, RBAMP_REG_V03_P0_REAL, RBAMP_STD_CURRENT_P,      false, 1e-3f,          RBAMP_SANITY_LIMIT_P  },
    [_Q_PF]     = { RBAMP_V2_CHFIELD_PF,     RBAMP_REG_V03_PF0,     RBAMP_STD_CURRENT_PF,     true,  1.0f / 1024.0f, RBAMP_SANITY_LIMIT_PF },
    [_Q_Q]      = { 0xFF,                    0,                     RBAMP_STD_CURRENT_Q,      false, 1e-3f,          RBAMP_SANITY_LIMIT_P  },
    [_Q_S]      = { 0xFF,                    0,                     RBAMP_STD_CURRENT_S,      false, 1e-3f,          RBAMP_SANITY_LIMIT_P  },
};

static esp_err_t _read_ch_float(rbamp_handle_t dev, _qty_t q, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        return _std_read_scaled(dev, RBAMP_STD_CURRENT(ch) + kQty[q].std_off,
                                kQty[q].std_i16, kQty[q].std_scale, out, kQty[q].max_abs);
    }
    if (kQty[q].chfield == 0xFF) return ESP_ERR_NOT_SUPPORTED;
    if (dev->sct_stale) {
        esp_err_t e = _sct_refresh(dev);
        if (e != ESP_OK) return e;
    }
    esp_err_t err = (ch >= 3) ? _read_window_field(dev, kQty[q].chfield, ch, out, kQty[q].max_abs)
                              : _read_float_le(dev, (uint8_t)(kQty[q].flat_base + ch * 4), out, kQty[q].max_abs);
    if (err == ESP_OK && (q == _Q_I_RMS || q == _Q_I_PEAK || q == _Q_P)) *out *= _sct_scale(dev, ch);
    return err;
}

esp_err_t rbamp_read_current(rbamp_handle_t dev, uint8_t ch, float *out)
{
    return _read_ch_float(dev, _Q_I_RMS, ch, out);
}

esp_err_t rbamp_read_current_peak(rbamp_handle_t dev, uint8_t ch, float *out)
{
    return _read_ch_float(dev, _Q_I_PEAK, ch, out);
}

esp_err_t rbamp_read_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    return _read_ch_float(dev, _Q_P, ch, out);
}

esp_err_t rbamp_read_power_factor(rbamp_handle_t dev, uint8_t ch, float *out)
{
    return _read_ch_float(dev, _Q_PF, ch, out);
}

esp_err_t rbamp_read_reactive_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    return _read_ch_float(dev, _Q_Q, ch, out);
}

esp_err_t rbamp_read_apparent_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    return _read_ch_float(dev, _Q_S, ch, out);
}

esp_err_t rbamp_read_frequency(rbamp_handle_t dev, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        uint16_t chz = 0;   /* centi-hertz */
        esp_err_t err = _std_read_u16(dev, RBAMP_STD_REG_FREQ, &chz);
        if (err != ESP_OK) return err;
        if (chz == 0) {
            /* The module publishes 0 when it sees no mains — not a frequency.
             * Same shape as a plausibility reject: NaN + ESP_ERR_INVALID_RESPONSE
             * (rbamp_read_all folds it into the FREQUENCY implausible bit). */
            *out = NAN;
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out = (float)chz / 100.0f;
        return ESP_OK;
    }
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
 * stays usable (bench finding #3). A genuine transport failure (NACK after
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

static esp_err_t _std_read_ip_block(rbamp_handle_t dev, uint16_t base, rbamp_std_channel_t *out);

/* STD snapshot: the voltage block and each channel block are read as ONE
 * transaction each (the module's 128-B coherent snapshot), not field by
 * field — a per-field read mixes values from different publish windows. */
static esp_err_t _std_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out)
{
    esp_err_t err;
    if (dev->n_voltage) {
        rbamp_std_voltage_t v;
        err = rbamp_std_read_voltage(dev, 0, &v);
        if (err != ESP_OK) return err;
        out->voltage      = v.u_rms;
        out->voltage_peak = v.u_peak;
        out->implausible |= v.implausible;
    }
    /* The struct carries ch0-2 only; ch3+ (UI5/UI7) go through rbamp_read_channel. */
    for (uint8_t ch = 0; ch < dev->channels && ch < RBAMP_SNAPSHOT_CHANNELS; ++ch) {
        rbamp_std_channel_t c;
        err = _std_read_ip_block(dev, RBAMP_STD_CURRENT(ch), &c);
        if (err != ESP_OK) return err;
        out->current[ch]      = c.i_rms;
        out->current_peak[ch] = c.i_peak;
        out->power[ch]        = c.p;
        out->power_factor[ch] = c.pf;
        out->implausible     |= c.implausible;
    }
    return _fold_field(rbamp_read_frequency(dev, &out->frequency),
                       &out->frequency, &out->implausible, RBAMP_FIELD_FREQUENCY);
}

_Static_assert(sizeof(((rbamp_snapshot_t *)0)->current) / sizeof(float) == RBAMP_SNAPSHOT_CHANNELS
               && sizeof(((rbamp_snapshot_t *)0)->power_factor) / sizeof(float) == RBAMP_SNAPSHOT_CHANNELS,
               "snapshot arrays vs RBAMP_SNAPSHOT_CHANNELS");

esp_err_t rbamp_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->topology = dev->topology;
    out->channels = dev->channels;
    out->has_voltage_hw = dev->has_voltage_hw;
    if (dev->std) return _std_read_all(dev, out);

    esp_err_t err;
    err = _fold_field(rbamp_read_voltage(dev, 0, &out->voltage),
                      &out->voltage, &out->implausible, RBAMP_FIELD_VOLTAGE);
    if (err != ESP_OK) return err;
    err = _fold_field(rbamp_read_voltage_peak(dev, 0, &out->voltage_peak),
                      &out->voltage_peak, &out->implausible, RBAMP_FIELD_VOLTAGE);
    if (err != ESP_OK) return err;
    /* The struct carries ch0-2 only; ch3+ (UI5/UI7) go through rbamp_read_channel. */
    for (uint8_t ch = 0; ch < dev->channels && ch < RBAMP_SNAPSHOT_CHANNELS; ++ch) {
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
 * Channel access window (senior UI5/UI7)
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_read_channel(rbamp_handle_t dev, uint8_t ch,
                             rbamp_channel_reading_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    /* Each accessor re-selects the window internally for ch>=3 (Rule #1). */
    esp_err_t err;
    err = _fold_field(rbamp_read_current(dev, ch, &out->i_rms),
                      &out->i_rms, &out->implausible, RBAMP_FIELD_CURRENT);
    if (err != ESP_OK) return err;
    err = _fold_field(rbamp_read_current_peak(dev, ch, &out->i_peak),
                      &out->i_peak, &out->implausible, RBAMP_FIELD_CURRENT);
    if (err != ESP_OK) return err;
    err = _fold_field(rbamp_read_power(dev, ch, &out->p_real),
                      &out->p_real, &out->implausible, RBAMP_FIELD_POWER);
    if (err != ESP_OK) return err;
    return _fold_field(rbamp_read_power_factor(dev, ch, &out->pf),
                       &out->pf, &out->implausible, RBAMP_FIELD_PF);
}

esp_err_t rbamp_read_all_channels(rbamp_handle_t dev,
                                  rbamp_channel_reading_t *out, uint8_t n)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    const uint8_t count = (n < dev->channels) ? n : dev->channels;
    for (uint8_t ch = 0; ch < count; ++ch) {
        esp_err_t err = rbamp_read_channel(dev, ch, &out[ch]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t rbamp_read_commit_seq(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    /* DIGEST (0x70) layout: [STATUS_MIRROR u8][SEQ u8] — SEQ is offset 1. */
    return _read_u8(dev, (uint8_t)(RBAMP_V2_REG_DIGEST + 1), out);
}

/* -------------------------------------------------------------------------
 * Data-ready (DRDY)
 *
 * One ISR per GPIO; every handle attached to that GPIO owns a binary
 * semaphore the ISR gives on each falling edge. The wait then checks the
 * module's own sequence — on a wired-OR line a pulse may be another
 * module's. STD modules before image 0x00010075 toggle the pin differently:
 * treated as "no line" (polling) so a stale level never looks like data.
 * ------------------------------------------------------------------------- */

#define RBAMP_DRDY_MAX_LINES   4
#define RBAMP_DRDY_MAX_SUBS    8
#define RBAMP_DRDY_ALARM_MS    5          /* still LOW this long after the edge = event pending (alarm level) */
#define RBAMP_DRDY_PULSE_GUARD_US 100     /* the data-ready pulse itself is <= ~50 us LOW: look only after it */
#define RBAMP_STD_DRDY_MIN_FW  0x00010075u

struct _rbamp_drdy_line {
    int                 gpio;             /* -1 = free slot */
    volatile uint32_t   edges;
    volatile int64_t    last_edge_us;
    rbamp_handle_t      subs[RBAMP_DRDY_MAX_SUBS];
};
static struct _rbamp_drdy_line s_drdy_lines[RBAMP_DRDY_MAX_LINES] = {
    { .gpio = -1 }, { .gpio = -1 }, { .gpio = -1 }, { .gpio = -1 },
};
static portMUX_TYPE s_drdy_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR _drdy_isr(void *arg)
{
    struct _rbamp_drdy_line *line = (struct _rbamp_drdy_line *)arg;
    line->edges++;
    line->last_edge_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    for (int i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        rbamp_handle_t d = line->subs[i];
        if (d && d->drdy_sem) xSemaphoreGiveFromISR(d->drdy_sem, &woken);
    }
    if (woken) portYIELD_FROM_ISR();
}

/* A module can hold the line (alarm class): BASIC always, STD from map 0x36. */
static bool _drdy_can_alarm(rbamp_handle_t d) { return !d->std || d->std_version >= 0x36; }

static bool _drdy_line_has_alarm(const struct _rbamp_drdy_line *line)
{
    for (int i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        if (line->subs[i] && _drdy_can_alarm(line->subs[i])) return true;
    }
    return false;
}

/* The line is held LOW: some module on it has a masked event. A held line
 * hides every other module's pulse, so clear the flags of ALL modules on it;
 * this handle keeps its own, the others are parked on their handles. */
static void _drdy_service_alarm(rbamp_handle_t self, struct _rbamp_drdy_line *line, rbamp_drdy_event_t *ev)
{
    for (int i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        rbamp_handle_t d = line->subs[i];
        if (!d || !_drdy_can_alarm(d)) continue;
        uint8_t flags = 0;
        if (rbamp_read_event_flags(d, &flags) != ESP_OK || !flags) continue;
        (void)rbamp_clear_event_flags(d, flags);
        if (d == self) {
            ev->alarm        = true;
            ev->event_flags |= flags;
        } else {
            d->drdy_pending_flags |= flags;
        }
    }
}

static void _drdy_detach(rbamp_handle_t dev)
{
    struct _rbamp_drdy_line *line = dev->drdy_line;
    if (!line) return;
    bool empty = true;
    portENTER_CRITICAL(&s_drdy_mux);
    for (int i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        if (line->subs[i] == dev) line->subs[i] = NULL;
        if (line->subs[i]) empty = false;
    }
    portEXIT_CRITICAL(&s_drdy_mux);
    dev->drdy_line = NULL;
    if (empty) {
        (void)gpio_isr_handler_remove((gpio_num_t)line->gpio);
        line->gpio = -1;
    }
}

esp_err_t rbamp_set_drdy_gpio(rbamp_handle_t dev, int gpio)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    _drdy_detach(dev);
    if (gpio < 0) return ESP_OK;
    if (!GPIO_IS_VALID_GPIO(gpio)) return ESP_ERR_INVALID_ARG;
    if (dev->std && dev->std_fw_ver && dev->std_fw_ver < RBAMP_STD_DRDY_MIN_FW) {
        ESP_LOGW(TAG, "drdy: module image 0x%08" PRIX32 " drives DRDY differently — polling instead",
                 dev->std_fw_ver);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!dev->drdy_sem) {
        dev->drdy_sem = xSemaphoreCreateBinary();
        if (!dev->drdy_sem) return ESP_ERR_NO_MEM;
    }
    /* Find the line (existing GPIO or a free slot). */
    struct _rbamp_drdy_line *line = NULL, *freeslot = NULL;
    for (int i = 0; i < RBAMP_DRDY_MAX_LINES; ++i) {
        if (s_drdy_lines[i].gpio == gpio) { line = &s_drdy_lines[i]; break; }
        if (s_drdy_lines[i].gpio < 0 && !freeslot) freeslot = &s_drdy_lines[i];
    }
    const bool is_new = (line == NULL);
    if (is_new) {
        if (!freeslot) return ESP_ERR_NO_MEM;
        line = freeslot;
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) return err;
        static bool s_isr_service_tried = false;   /* once per boot — the driver logs an error on a repeat */
        if (!s_isr_service_tried) {
            s_isr_service_tried = true;
            err = gpio_install_isr_service(0);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;   /* INVALID_STATE = installed by the app */
        }
        memset(line->subs, 0, sizeof line->subs);
        line->edges = 0;
        line->last_edge_us = 0;
        line->gpio = gpio;
        err = gpio_isr_handler_add((gpio_num_t)gpio, _drdy_isr, line);
        if (err != ESP_OK) { line->gpio = -1; return err; }
    }
    bool placed = false;
    portENTER_CRITICAL(&s_drdy_mux);
    for (int i = 0; i < RBAMP_DRDY_MAX_SUBS && !placed; ++i) {
        if (!line->subs[i]) { line->subs[i] = dev; placed = true; }
    }
    portEXIT_CRITICAL(&s_drdy_mux);
    if (!placed) {
        if (is_new) { (void)gpio_isr_handler_remove((gpio_num_t)gpio); line->gpio = -1; }
        return ESP_ERR_NO_MEM;
    }
    dev->drdy_line = line;
    (void)xSemaphoreTake(dev->drdy_sem, 0);   /* start clean */
    ESP_LOGI(TAG, "drdy: 0x%02X listens on GPIO%d%s", dev->addr, gpio,
             _drdy_line_has_alarm(line) ? " (alarm level enabled)" : "");
    return ESP_OK;
}

esp_err_t rbamp_read_data_seq(rbamp_handle_t dev, uint16_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (dev->std_version < 0x36) return ESP_ERR_NOT_SUPPORTED;   /* no publish counter before map 0x36 */
        return _std_read_u16(dev, RBAMP_STD_REG_WIN_SEQ, out);
    }
    uint8_t s8 = 0;
    esp_err_t err = rbamp_read_commit_seq(dev, &s8);
    if (err == ESP_OK) *out = s8;
    return err;
}

/* Fresh if the sequence moved since the last consumed one; fills seq/missed. */
static esp_err_t _drdy_check_seq(rbamp_handle_t dev, rbamp_drdy_event_t *ev, bool *fresh)
{
    uint16_t seq = 0;
    esp_err_t err = rbamp_read_data_seq(dev, &seq);
    if (err != ESP_OK) return err;
    *fresh = !dev->drdy_have_seq || seq != dev->drdy_last_seq;
    if (*fresh) {
        const uint16_t step = dev->drdy_have_seq
            ? (uint16_t)(dev->std ? (uint16_t)(seq - dev->drdy_last_seq) : (uint8_t)(seq - dev->drdy_last_seq))
            : 1u;
        ev->seq    = seq;
        ev->missed = step ? (uint16_t)(step - 1u) : 0u;
        dev->drdy_last_seq = seq;
        dev->drdy_have_seq = true;
    }
    return ESP_OK;
}

esp_err_t rbamp_wait_data_ready(rbamp_handle_t dev, uint32_t timeout_ms, rbamp_drdy_event_t *ev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    rbamp_drdy_event_t local;
    if (!ev) ev = &local;
    memset(ev, 0, sizeof(*ev));
    if (dev->std && dev->std_version < 0x36) return ESP_ERR_NOT_SUPPORTED;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    if (!dev->drdy_line) {
        /* Polling fallback. */
        for (;;) {
            if (dev->drdy_pending_flags) {
                ev->alarm = true; ev->event_flags |= dev->drdy_pending_flags; dev->drdy_pending_flags = 0;
            }
            bool fresh = false;
            esp_err_t err = _drdy_check_seq(dev, ev, &fresh);
            if (err != ESP_OK) return err;
            if (fresh || ev->alarm) { ev->fresh = fresh; return ESP_OK; }
            if (esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;
            _sleep_ms(CONFIG_RBAMP_DRDY_POLL_MS);
        }
    }

    struct _rbamp_drdy_line *line = dev->drdy_line;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now >= deadline_us) return ESP_ERR_TIMEOUT;
        const TickType_t ticks = pdMS_TO_TICKS((uint32_t)((deadline_us - now) / 1000) + 1);
        if (xSemaphoreTake(dev->drdy_sem, ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
        const int64_t edge_us = line->last_edge_us;

        /* Flags another handle already consumed for this module. */
        if (dev->drdy_pending_flags) {
            ev->alarm        = true;
            ev->event_flags |= dev->drdy_pending_flags;
            dev->drdy_pending_flags = 0;
        }
        /* Alarm level: the line stays LOW while a masked event is pending. */
        if (_drdy_line_has_alarm(line)) {
            /* First look just past the pulse (busy-wait of at most ~100 us):
             * HIGH again = plain data-ready, no latency added. Still LOW = a
             * candidate alarm — confirm a hard 5 ms after the EDGE (measured,
             * not "a tick later": at 100 Hz one tick is 10 ms and 5 ms would
             * round to none). */
            const int64_t past_pulse_us = edge_us + RBAMP_DRDY_PULSE_GUARD_US;
            const int64_t t = esp_timer_get_time();
            if (t < past_pulse_us) esp_rom_delay_us((uint32_t)(past_pulse_us - t));
            if (gpio_get_level((gpio_num_t)line->gpio) == 0) {
                while (esp_timer_get_time() - edge_us < (int64_t)RBAMP_DRDY_ALARM_MS * 1000) vTaskDelay(1);
                if (gpio_get_level((gpio_num_t)line->gpio) == 0) _drdy_service_alarm(dev, line, ev);
            }
        }
        bool fresh = false;
        esp_err_t err = _drdy_check_seq(dev, ev, &fresh);
        if (err != ESP_OK) return err;
        if (fresh || ev->alarm) {
            ev->fresh      = fresh;
            ev->by_edge    = true;
            ev->latency_us = (uint32_t)(esp_timer_get_time() - edge_us);
            return ESP_OK;
        }
        /* Not ours (another module on the line pulsed): keep waiting. */
    }
}

esp_err_t rbamp_read_all_ready(rbamp_handle_t dev, uint32_t timeout_ms,
                               rbamp_snapshot_t *out, rbamp_drdy_event_t *ev)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = rbamp_wait_data_ready(dev, timeout_ms, ev);
    if (err != ESP_OK) return err;
    return rbamp_read_all(dev, out);
}

/* -------------------------------------------------------------------------
 * Period metering
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_latch_period(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (dev->std_period_src == RBAMP_STD_PERIOD_LATCH) return _std_latch_bank(dev, 0);
        /* Counter modules have nothing to latch: the period is read on demand. */
        return (dev->std_period_src == RBAMP_STD_PERIOD_COUNTERS) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    return _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
}

esp_err_t rbamp_is_period_valid(rbamp_handle_t dev, bool *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t v = 0;
    if (dev->std) {
        if (dev->std_period_src != RBAMP_STD_PERIOD_LATCH) {
            if (dev->std_period_src == RBAMP_STD_PERIOD_NONE) return ESP_ERR_NOT_SUPPORTED;
            *out = dev->std_lt_anchored;     /* counters: valid once anchored */
            return ESP_OK;
        }
        /* Bank 0 has a snapshot. Sticky on the module — not a freshness flag. */
        esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_PD_VALID, &v);
        if (err != ESP_OK) return err;
        *out = (v & 0x01) != 0;
        return ESP_OK;
    }
    esp_err_t err = _read_u8(dev, RBAMP_REG_V03_PERIOD_VALID, &v);
    if (err != ESP_OK) return err;
    *out = (v & 0x01) != 0;
    return ESP_OK;
}

esp_err_t rbamp_read_period_avg_power(rbamp_handle_t dev, uint8_t ch, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (dev->std_period_src != RBAMP_STD_PERIOD_LATCH) {
            /* Counters are totals only: a per-channel average exists just on
             * a single-channel module, and only once a period was consumed. */
            if (dev->std_period_src != RBAMP_STD_PERIOD_COUNTERS || dev->channels != 1)
                return ESP_ERR_NOT_SUPPORTED;
            if (!dev->std_have_last_avg) return ESP_ERR_INVALID_STATE;
            *out = dev->std_last_avg_total;
            return ESP_OK;
        }
        rbamp_std_latch_t snap;
        esp_err_t err = _std_read_latch_bank(dev, 0, &snap);
        if (err != ESP_OK) return err;
        *out = snap.p_avg_ch[ch];
        return ESP_OK;
    }
    if (dev->sct_stale) {
        esp_err_t e = _sct_refresh(dev);
        if (e != ESP_OK) return e;
    }
    esp_err_t err;
    /* ch3+ (senior UI5/UI7): the same latched avg P via window field 13. */
    if (ch >= 3) {
        err = _read_window_field(dev, RBAMP_V2_CHFIELD_PERIOD_AVG_P, ch, out,
                                 RBAMP_SANITY_LIMIT_P);
    } else {
        /* Non-contiguous register addresses per SPEC §7: ch0=0xDC, ch1=0xC2, ch2=0xC6. */
        uint8_t reg;
        switch (ch) {
            case 0: reg = RBAMP_REG_V03_PERIOD_AVG_P_F0; break;
            case 1: reg = RBAMP_REG_V03_PERIOD_AVG_P_F1; break;
            case 2: reg = RBAMP_REG_V03_PERIOD_AVG_P_F2; break;
            default: return ESP_ERR_INVALID_ARG;
        }
        err = _read_float_le(dev, reg, out, RBAMP_SANITY_LIMIT_P);
    }
    if (err == ESP_OK) *out *= _sct_scale(dev, ch);   /* SCT-013 rating correction (1.0 when none) */
    return err;
}

esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (dev->std_period_src != RBAMP_STD_PERIOD_LATCH) return ESP_ERR_NOT_SUPPORTED;
        return _std_read_scaled(dev, RBAMP_STD_LATCH(0) + RBAMP_STD_LATCH_MAX_P,
                                false, 1e-3f, out, RBAMP_SANITY_LIMIT_P);
    }
    esp_err_t err = _read_float_le(dev, RBAMP_REG_V03_PERIOD_MAX_P_F0, out, RBAMP_SANITY_LIMIT_P);
    if (err == ESP_OK) *out *= _sct_common_scale(dev);   /* module-wide: only when every channel shares one scale */
    return err;
}

/** @internal STD arm of rbamp_read_period_snapshot — latch bank 0. */
static esp_err_t _std_read_period_snapshot(rbamp_handle_t dev,
                                           rbamp_period_snapshot_t *out,
                                           uint16_t settle_ms,
                                           bool skip_latch)
{
    esp_err_t err;
    if (!skip_latch) {
        err = _std_latch_bank(dev, 0);
        if (err != ESP_OK) return err;
        /* The latch is serviced at the module's next window boundary; a
         * shorter settle would read the PREVIOUS snapshot as fresh. */
        if (settle_ms < RBAMP_STD_LATCH_SETTLE_MIN_MS) settle_ms = RBAMP_STD_LATCH_SETTLE_MIN_MS;
    }

    /* Master-clock window since the previous consumed read (the module has no
     * clock at all on this line, so this is not just preferred, it is the
     * only dt there is). */
    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = 0;
    if (dev->have_last_latch && now_us > dev->last_latch_us) {
        dt_ms = (uint32_t)((now_us - dev->last_latch_us) / 1000);
    }

    _sleep_ms(settle_ms);

    if (!skip_latch) {
        err = _std_check_cmd_error(dev);   /* map 0x34+: LATCH_PERIOD accepted */
        if (err != ESP_OK) return err;
    }

    rbamp_std_latch_t snap;
    err = _std_read_latch_bank(dev, 0, &snap);
    if (err == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGW(TAG, "period STALE — bank 0 never latched (accumulator rolls into the next cycle)");
        return err;
    }
    if (err != ESP_OK) return err;
    if (snap.win_count == 0) {
        /* Empty snapshot: either no window elapsed yet, or the bank is no
         * longer armed — the module lost its RAM arm mask in a reset (E15).
         * Tell them apart from the mask; on a lost arm re-arm + re-prime and
         * drop this window (no energy integrated, anchor restarted). */
        bool was_armed = true;
        err = _std_ensure_armed(dev, 0, &was_armed);
        if (err != ESP_OK) return err;
        if (!was_armed) {
            ESP_LOGW(TAG, "period: bank 0 was DISARMED (module reset?) — re-armed, "
                          "this window is skipped");
            err = _std_latch_bank(dev, 0);     /* primer for the new period */
            if (err != ESP_OK) return err;
            dev->last_latch_us   = esp_timer_get_time();
            dev->have_last_latch = true;
        } else {
            ESP_LOGW(TAG, "period EMPTY — no window elapsed since arming");
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint8_t ch = 0; ch < dev->channels && ch < RBAMP_SNAPSHOT_CHANNELS; ++ch) {
        out->avg_p[ch] = snap.p_avg_ch[ch];
    }
    out->max_p        = snap.max_p;
    out->latch_ms     = 0;       /* no module-side clock on this line */
    out->master_dt_ms = dt_ms;
    out->valid        = true;
    out->avg_p_total  = snap.p_avg;
    {
        double wh_in = 0, wh_out = 0;
        if (rbamp_std_latch_wh(&snap, dt_ms, RBAMP_STD_TOTAL, &wh_in, &wh_out) == ESP_OK)
            out->wh_net = wh_in + wh_out;   /* the export bucket is <= 0 */
    }

#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_tick(&dev->energy, snap.p_avg_ch, dev->channels, dt_ms, out->valid);
#endif
    dev->last_latch_us   = now_us;
    dev->have_last_latch = true;
    return ESP_OK;
}

/** @internal STD arm of rbamp_read_period_snapshot for modules with
 * persisted counters and no latch banks: a period is the difference of two
 * counter readings on the master's schedule. Wh resolution before map 0x36 (mWh from 0x36), so the
 * caller's period should be minutes, not seconds. */
static esp_err_t _std_read_period_counters(rbamp_handle_t dev, rbamp_period_snapshot_t *out)
{
    if (!dev->std_lt_anchored) {
        esp_err_t err = _std_lt_anchor(dev);
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_RESPONSE;   /* first read only anchors */
    }
    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = 0;
    if (now_us > dev->last_latch_us) dt_ms = (uint32_t)((now_us - dev->last_latch_us) / 1000);

    rbamp_std_energy_t cur, d;
    bool restarted = false;
    esp_err_t err = _std_read_energy_raw(dev, &cur);
    if (err != ESP_OK) return err;
    err = rbamp_std_energy_delta(&dev->std_lt_anchor, &cur, &d, &restarted);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "period: life-time counters discontinuous (generation %" PRIu32 " -> %" PRIu32
                      ") — re-anchored, this period is skipped",
                 dev->std_lt_anchor.generation, cur.generation);
        dev->std_lt_anchor = cur;
        dev->last_latch_us = now_us;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (restarted) {
        ESP_LOGW(TAG, "period: module restarted (uptime now %" PRIu32 " s) — counters persisted, "
                      "period kept; re-apply its clock and RAM settings", cur.uptime_s);
    }
    if (cur.act_import_wh < dev->std_lt_anchor.act_import_wh
            || cur.act_import_mwh < dev->std_lt_anchor.act_import_mwh) {
        ESP_LOGW(TAG, "period: a counter resumed below the previous reading (power cut?) — "
                      "that part is lost, the difference is clamped to 0");
    }
    /* Map 0x36: mWh totals and per-channel counters; before: whole Wh, totals only. */
    const bool   mwh    = dev->std_version >= 0x36;
    const double net_wh = d.fine ? (double)d.act_net_uwh / 1e6
                        : mwh    ? (double)d.act_net_mwh / 1000.0 : (double)d.act_net_wh;
    const float  avg    = dt_ms ? (float)(net_wh * 3.6e6 / (double)dt_ms) : 0.0f;
    float avg_ch[RBAMP_MAX_CHANNELS];
    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) {
        if (ch >= dev->channels)   { avg_ch[ch] = 0.0f; continue; }
        if (dev->channels == 1)    { avg_ch[ch] = avg;  continue; }   /* one channel = the total */
        if (mwh && d.channels > ch) {
            const double ch_wh = d.fine ? (double)(d.ch_import_uwh[ch] - d.ch_export_uwh[ch]) / 1e6
                                        : (double)(d.ch_import_mwh[ch] - d.ch_export_mwh[ch]) / 1000.0;
            avg_ch[ch] = dt_ms ? (float)(ch_wh * 3.6e6 / (double)dt_ms) : 0.0f;
        } else {
            avg_ch[ch] = NAN;   /* no usable per-channel counters for this period */
        }
    }
    if (mwh && dev->channels > 1 && d.channels == 0) {
        ESP_LOGW(TAG, "period: per-channel counters unavailable for this period (channel count changed)");
    }
    for (uint8_t ch = 0; ch < 3; ++ch) out->avg_p[ch] = avg_ch[ch];
    out->max_p            = NAN;     /* no period envelope on the counter path */
    out->latch_ms         = 0;
    out->master_dt_ms     = dt_ms;
    out->valid            = true;
    out->avg_p_total      = avg;
    out->wh_net           = net_wh;
    out->module_restarted = restarted;
#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_add_wh(&dev->energy, RBAMP_ENERGY_CH_TOTAL, net_wh);
    if (dev->channels == 1) {
        rbamp_energy_add_wh(&dev->energy, 0, net_wh);
    } else if (mwh && d.channels) {
        for (uint8_t ch = 0; ch < d.channels && ch < RBAMP_MAX_CHANNELS; ++ch) {
            rbamp_energy_add_wh(&dev->energy, ch,
                                d.fine ? (double)(d.ch_import_uwh[ch] - d.ch_export_uwh[ch]) / 1e6
                                       : (double)(d.ch_import_mwh[ch] - d.ch_export_mwh[ch]) / 1000.0);
        }
    }
#endif
    dev->std_lt_anchor      = cur;
    dev->last_latch_us      = now_us;
    dev->have_last_latch    = true;
    dev->std_last_avg_total = avg;
    dev->std_have_last_avg  = true;
    return ESP_OK;
}

esp_err_t rbamp_read_uptime_s(rbamp_handle_t dev, uint32_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) return _std_read_u32(dev, RBAMP_STD_ENERGY_BASE + RBAMP_STD_ENERGY_UPTIME, out);
    return _read_u32_le(dev, RBAMP_V2_REG_UPTIME_S, out);
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
    if (dev->std) {
        switch (dev->std_period_src) {
        case RBAMP_STD_PERIOD_LATCH:    return _std_read_period_snapshot(dev, out, settle_ms, skip_latch);
        case RBAMP_STD_PERIOD_COUNTERS: return _std_read_period_counters(dev, out);
        default:                        return ESP_ERR_NOT_SUPPORTED;
        }
    }

    esp_err_t err;

    /* 1. Latch (skip if caller already issued rbamp_latch_period or
     *    rbamp_broadcast_latch in the batched multi-module pattern). */
    if (!skip_latch) {
        err = _write_cmd(dev, RBAMP_CMD_LATCH_PERIOD);
        if (err != ESP_OK) return err;
    }

    /* 2. Capture the master-clock reference for this read's energy window and
     *    compute dt against the previous consumed read. The master's esp_timer
     *    is the billing window; the chip's 0xEC is diagnostic only. */
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

    /* 5. Read avg_p for every channel (ch0-2 flat, ch3+ via window field 13 —
     *    the device latches avg P atomically across ALL channels). The public
     *    snapshot struct exposes only ch0-2; the full vector feeds the energy
     *    accumulator so ch3+ Wh is available via rbamp_energy_wh. latch_ms
     *    (0xEC) is diagnostic only; NOT used for energy. */
    float avg_all[RBAMP_MAX_CHANNELS] = {0};
    for (uint8_t ch = 0; ch < dev->channels; ++ch) {
        err = rbamp_read_period_avg_power(dev, ch, &avg_all[ch]);
        if (err != ESP_OK) return err;
        if (ch < RBAMP_SNAPSHOT_CHANNELS) out->avg_p[ch] = avg_all[ch];
    }
    err = rbamp_read_period_max_power(dev, &out->max_p);
    if (err != ESP_OK) return err;
    err = rbamp_read_period_latch_ms(dev, &out->latch_ms);
    if (err != ESP_OK) return err;

    /* 5b. The module's own view of the latched window: commits (0xBE, latched
     *     with the snapshot; 100 ms each on current images, 200 ms on pre-v1.2
     *     — inferred from latch_ms of the same snapshot) and seconds since boot
     *     (0x46; reads 0 on firmware without it). After rbamp_begin_resume the
     *     first consumed snapshot integrates over this window instead of the
     *     master dt. */
    uint32_t commit_cnt = 0, uptime_s = 0;
    err = _read_u32_le(dev, RBAMP_REG_V03_PERIOD_COMMIT_CNT, &commit_cnt);
    if (err != ESP_OK) return err;
    (void)_read_u32_le(dev, RBAMP_V2_REG_UPTIME_S, &uptime_s);
    if (uptime_s != 0) {
        if (dev->basic_have_uptime && uptime_s < dev->basic_last_uptime_s) {
            dev->digest_armed = false;   /* module restarted: RAM mask gone, applied gains back to factory */
            dev->sct_stale    = true;
        }
        dev->basic_last_uptime_s = uptime_s;
        dev->basic_have_uptime   = true;
    }
    rbampi_basic_window_t win;
    rbampi_basic_window_decode(commit_cnt, out->latch_ms, uptime_s, &win);
    out->module_window_ms = win.window_ms;
    out->module_commit_ms = win.commit_ms;
    out->module_uptime_s  = win.uptime_s;
    out->window_from_boot = win.from_boot;
    out->window_long      = win.is_long;
    dt_ms = rbampi_basic_period_dt(&dev->basic_resume_pending, dt_ms, &win, &out->module_restarted);

    out->master_dt_ms = dt_ms;   /* energy window: master clock, or the module window on resume */
    out->valid = true;
    for (uint8_t ch = 0; ch < dev->channels; ++ch) out->avg_p_total += avg_all[ch];
    out->wh_net = (double)out->avg_p_total * (double)dt_ms / 3.6e6;

    /* 6. Integrate Wh using the MASTER dt, then advance the anchor. */
#if !defined(CONFIG_RBAMP_DISABLE_ENERGY)
    rbamp_energy_tick(&dev->energy, avg_all, dev->channels,
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
    /* Counter-path multi-channel modules publish no per-channel energy: say
     * so with NaN rather than a zero that reads as "nothing consumed". */
    if (dev->std && dev->std_period_src == RBAMP_STD_PERIOD_COUNTERS && dev->std_version < 0x36
            && dev->channels > 1 && ch != RBAMP_ENERGY_CH_TOTAL && ch < dev->channels) {
        return NAN;
    }
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
 * Synchronous write-verify (bench harness finding #1).
 *
 * Read REG_ERROR (0x02) IMMEDIATELY after our own validating write/command,
 * before any other data transaction. In that narrow window REG_ERROR is the
 * synchronous truth (last-write-outcome): an invalid enum / rejected command
 * shows @c ERR_PARAM right away, whereas the durable EVENT bit3 lags by a
 * publish window or more for register-write rejections (it only settles on the device's periodic
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
    if (dev->std) {
        /* Same precondition on the STD line: CFG_CLASS 0 = UNSET (map default). */
        uint8_t cls = 0;
        esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_CFG_CLASS, &cls);
        if (err != ESP_OK) return err;
        if (cls == RBAMP_STD_SENSOR_UNSET) {
            ESP_LOGW(TAG, "set_ct_model refused: sensor class is UNSET "
                          "(call rbamp_set_sensor_class first)");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
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

/* NOTE: the client-side CT-preset whitelist (_ct_model_valid) was removed in
 * v1.5.0. The set of accepted (class, model) codes is firmware runtime truth
 * (ct_presets.c) — a code without a preset row returns ERR_PARAM (0xFE), which
 * we surface via _check_write_error. A hand-maintained mirror here drifted six
 * models behind the firmware in two days (2026-09-03); the device is the sole
 * authority. Model semantics (names/descriptors) come from the generated
 * rbamp_sensor_models.h, never from a local list. */

esp_err_t rbamp_read_sensor_class(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) return _std_read_u8(dev, RBAMP_STD_REG_CFG_CLASS, out);
    return _read_u8(dev, RBAMP_REG_SENSOR_CLASS, out);
}

/** @internal Read-before-write guard for one channel's CT model. *skip = the
 * module already holds this model (nothing to write). ESP_ERR_INVALID_STATE =
 * factory-paired module (class 2/3) and @p force is false. */
static esp_err_t _ct_model_guard(rbamp_handle_t dev, uint8_t channel, uint8_t code,
                                 bool force, bool *skip)
{
    *skip = false;
    uint8_t cls = 0, cur = 0;
    esp_err_t err = rbamp_read_sensor_class(dev, &cls);
    if (err != ESP_OK) return err;
    err = rbamp_read_ct_model_ch(dev, channel, &cur);
    if (err != ESP_OK) cur = 0xFF;   /* unreadable mirror: never mistaken for "same" */
    switch (rbampi_ct_model_write_policy(cls, cur, code, force)) {
        case RBAMPI_WRITE_NOOP:
            *skip = true;
            return ESP_OK;
        case RBAMPI_WRITE_PAIRED:
            ESP_LOGW(TAG, "set_ct_model ch%u refused: the module is calibrated with its "
                          "fitted sensor (class %u); use rbamp_set_ct_model_ch_force in service",
                     channel, cls);
            return ESP_ERR_INVALID_STATE;
        default:
            return ESP_OK;
    }
}

esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev, rbamp_sensor_class_t cls)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if ((uint8_t)cls > (uint8_t)RBAMP_SENSOR_BUILTIN_CT) return ESP_ERR_INVALID_ARG;
    /* Read before write: an unchanged value is a no-op (the firmware clears the
     * CT models on ANY class write), a factory-paired class is not rewritten. */
    uint8_t cur = 0;
    esp_err_t gerr = rbamp_read_sensor_class(dev, &cur);
    if (gerr != ESP_OK) return gerr;
    switch (rbampi_sensor_class_write_policy(cur, (uint8_t)cls)) {
        case RBAMPI_WRITE_NOOP:   return ESP_OK;
        case RBAMPI_WRITE_PAIRED:
            ESP_LOGW(TAG, "set_sensor_class refused: the module is calibrated with its "
                          "fitted sensor (class %u)", cur);
            return ESP_ERR_INVALID_STATE;
        default: break;
    }
    if (dev->std) {
        /* CFG_CLASS is metadata on this line (no preset table); persisted by
         * CMD_SAVE_CONFIG, whose verdict rbamp_save_user_config checks. */
        esp_err_t err = _std_write_u8(dev, RBAMP_STD_REG_CFG_CLASS, (uint8_t)cls);
        if (err != ESP_OK) return err;
        return rbamp_save_user_config(dev);
    }
    esp_err_t err = _write_u8(dev, RBAMP_REG_SENSOR_CLASS, (uint8_t)cls);
    if (err != ESP_OK) return err;
    /* Synchronous verify before the save — a rejected enum is caught here
     * (immediate), not via the EVENT bit3 that lags a publish window, and we skip the
     * flash write entirely on rejection. */
    err = _check_write_error(dev);
    if (err != ESP_OK) return err;
    /* SENSOR_CLASS is user_config persistence: persist via CMD_SAVE_USER_CONFIG
     * (available on production firmware). CMD_SAVE_GAINS is a silent no-op on
     * production firmware (latent bug fixed in v1.5.0). */
    return rbamp_save_user_config(dev);
}

esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->std) return rbamp_set_ct_model_ch(dev, 0, code);
    /* No client-side code range — the device validates (class, model) and raises
     * ERR_PARAM for a code without a preset row (surfaced by _check_write_error). */
    esp_err_t err = _check_sensor_class_set(dev);
    if (err != ESP_OK) return err;
    bool skip = false;
    err = _ct_model_guard(dev, 0, code, false, &skip);
    if (err != ESP_OK) return err;
    if (skip) return ESP_OK;
    /* v1.3 A1: REG_CT_MODEL (0x05) is PURE STAGING — a bare write no longer
     * applies the preset. Binding happens exclusively via CMD_SET_CT_MODEL_CHn.
     * The single-arg form targets channel 0 (the only channel on UI1/I1). */
    err = _write_u8(dev, RBAMP_REG_CT_MODEL, code);
    if (err != ESP_OK) return err;
    err = _write_cmd(dev, RBAMP_V2_CMD_SET_CT_MODEL_CH0);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_V2_SETTLE_MS_SET_CT_MODEL_CH0);
    /* The CMD validates the (class, model) preset — verify before persist. */
    err = _check_write_error(dev);
    if (err != ESP_OK) return err;
    /* CT_MODEL is user_config persistence — see rbamp_set_sensor_class. */
    err = rbamp_save_user_config(dev);
    if (err != ESP_OK) return err;
    return _sct_after_bind(dev);
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
/* ch0-2 flat CMD bind: stage REG_CT_MODEL, issue CMD_SET_CT_MODEL_CHn (which
 * device-validates the (class, model) pair and raises ERR_PARAM on a bad code),
 * verify. No save. v1.2+ only (the per-channel opcodes don't exist earlier). */
static esp_err_t _bind_ct_model_ch_flat(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    uint8_t version = 0;
    esp_err_t err = _read_u8(dev, RBAMP_REG_VERSION, &version);
    if (err != ESP_OK) return err;
    if (version < 0x03) {
        ESP_LOGW(TAG, "set_ct_model_ch refused: firmware 0x%02X < v1.2", version);
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = _check_sensor_class_set(dev);   /* class must be set (v1.2+) */
    if (err != ESP_OK) return err;

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
    /* The CMD validates the (class, model) preset — verify before persist. */
    return _check_write_error(dev);
}

/* ch3+ (senior SKU) bind via the channel window, field 15 = CT_MODEL (fw
 * v1.4.18+). The window write applies the preset immediately (NF+GAIN+shape for
 * that channel); the device raises ERR_PARAM (0xFE) on an unknown (class, model)
 * and keeps the channel's previous model — surfaced, not masked. No save. */
static esp_err_t _bind_ct_model_ch_window(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    esp_err_t err = _check_sensor_class_set(dev);
    if (err != ESP_OK) return err;
    err = _write_window_field(dev, RBAMP_V2_CHFIELD_CT_MODEL, channel, code);
    if (err != ESP_OK) return err;
    return _check_write_error(dev);   /* surface ERR_PARAM, do not mask */
}

/* Route one channel's CT-model apply (no save): ch0-2 = flat CMD path, ch3+ =
 * window field 15. ONE internal branch point per the PUBLIC_API_TRUTH
 * channel-access canon — a single public setter, guard hidden inside. */
/* STD: the CT model is a REFERENCE (SKU metadata) — it sets no gain; the
 * module's calibration lives in the CAL block. Per-channel map entry
 * CHMAP_MODEL(c) is what rbamp_read_ct_model_ch reads back; the module
 * persists only the single CFG_CT_MODEL byte and re-seeds CHMAP_MODEL(0) from
 * it at boot, so channel 0 also writes CFG_CT_MODEL (channels 1+ hold their
 * entry until reboot on current module firmware). No save here. */
static esp_err_t _std_apply_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    /* From map 0x35 the capability bits are honest: a module that does not
     * persist a per-channel CT model does not take the write either — refuse
     * before any traffic instead of failing the readback afterwards (L4). */
    if (dev->std_version >= 0x35 && !(dev->std_caps & RBAMP_STD_CAP_CHMAP_PERSIST)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = _check_sensor_class_set(dev);
    if (err != ESP_OK) return err;
    err = _std_write_u8(dev, RBAMP_STD_CHMAP_MODEL(channel), code);
    if (err != ESP_OK) return err;
    if (channel == 0) err = _std_write_u8(dev, RBAMP_STD_REG_CFG_CT_MODEL, code);
    return err;
}

static esp_err_t _apply_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    if (dev->std) return _std_apply_ct_model_ch(dev, channel, code);
    return (channel < 3) ? _bind_ct_model_ch_flat(dev, channel, code)
                         : _bind_ct_model_ch_window(dev, channel, code);
}

/* Forward decl — the public setter readback-verifies via this (defined below). */
static esp_err_t _verify_ct_bind(rbamp_handle_t dev, uint8_t ch, uint8_t expected);

static esp_err_t _set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code, bool force)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* Validate against the module's actual channel count (variant/topology),
     * NOT a hardcoded 3 — ch3+ are real on senior UI5/UI7 SKUs. No client-side
     * code range: the device validates (class, model). */
    if (channel >= dev->channels) return ESP_ERR_INVALID_ARG;
    bool skip = false;
    esp_err_t err = _ct_model_guard(dev, channel, code, force, &skip);
    if (err != ESP_OK) return err;
    if (skip) return ESP_OK;
    err = _apply_ct_model_ch(dev, channel, code);
    if (err != ESP_OK) return err;
    /* Readback-confirm the applied model before persisting (SPEC test rule: a
     * write is not accepted until confirmed by readback). A STABLE WRONG mirror
     * is a genuine miss; a torn read under bus load trusts the accepted CMD. */
    esp_err_t v = _verify_ct_bind(dev, channel, code);
    if (v == ESP_ERR_INVALID_STATE) return v;
    /* CT_MODEL is user_config persistence — CMD_SAVE_USER_CONFIG (ungated),
     * which persists ct_model_ch[] for all channels. */
    err = rbamp_save_user_config(dev);
    if (err != ESP_OK) return err;
    return _sct_after_bind(dev);
}

esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    return _set_ct_model_ch(dev, channel, code, false);
}

esp_err_t rbamp_set_ct_model_ch_force(rbamp_handle_t dev, uint8_t channel, uint8_t code)
{
    return _set_ct_model_ch(dev, channel, code, true);
}

esp_err_t rbamp_read_ct_model_ch(rbamp_handle_t dev, uint8_t channel, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (channel >= dev->channels) return ESP_ERR_INVALID_ARG;
    if (dev->std) return _std_read_u8(dev, RBAMP_STD_CHMAP_MODEL(channel), out);
    /* ch3+ (senior SKU): applied model is read back through the window, field 15.
     * There is no flat mirror for seniors (0x51-0x53 cover ch0-2 only). */
    if (channel >= 3)
        return _read_window_u8(dev, RBAMP_V2_CHFIELD_CT_MODEL, channel, out);
    /* Applied-model mirrors are contiguous: CH0=0x51, CH1=0x52, CH2=0x53.
     * A/B double-read — a torn mirror would read as a false bind-fail. */
    return _read_u8_ab(dev, (uint8_t)(RBAMP_V2_REG_CT_MODEL_CH0 + channel), out);
}

/** @internal Post-bind verify of one channel's applied-model mirror.
 *
 * Distinguishes the three outcomes a verify can have under a busy multi-module
 * bus (bench finding — a torn mirror read must NOT read as a
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
    if ((uint8_t)cls > (uint8_t)RBAMP_SENSOR_BUILTIN_CT) return ESP_ERR_INVALID_ARG;
    uint8_t nch = dev->channels;
    if (n > nch) n = nch;  /* variant-aware clamp */

    /* Decide the whole request before the first write: a module calibrated
     * with its fitted sensor (class 2/3) is refused up front when anything
     * would change, so nothing is half-applied; an all-equal request is a
     * no-op with no bus traffic beyond the reads. */
    uint8_t cur_cls = 0, cur_models[RBAMP_MAX_CHANNELS];
    esp_err_t err = rbamp_read_sensor_class(dev, &cur_cls);
    if (err != ESP_OK) return err;
    for (uint8_t ch = 0; ch < n; ++ch) {
        cur_models[ch] = 0xFF;   /* unreadable mirror never counts as "same" */
        if (models[ch] != 0 && rbamp_read_ct_model_ch(dev, ch, &cur_models[ch]) != ESP_OK)
            cur_models[ch] = 0xFF;
    }
    switch (rbampi_configure_policy(cur_cls, (uint8_t)cls, cur_models, models, n)) {
        case RBAMPI_WRITE_NOOP:   return ESP_OK;
        case RBAMPI_WRITE_PAIRED:
            ESP_LOGW(TAG, "configure_channels refused: the module is calibrated with its "
                          "fitted sensor (class %u); nothing written", cur_cls);
            return ESP_ERR_INVALID_STATE;
        default: break;
    }

    err = rbamp_set_sensor_class(dev, cls);
    if (err != ESP_OK) return err;
    bool any_bound = false;
    /* Order-independent (v1.3 A1 / Fix A): REG_CT_MODEL is pure staging and the
     * per-channel CMD binds only its own channel, so binding in natural order
     * is correct. (The pre-Fix-A ch0 auto-apply that made order matter is gone;
     * HW-confirmed ascending {1,3,6} → [1,3,6] on Fix-A firmware.) */
    for (uint8_t ch = 0; ch < n; ++ch) {
        if (models[ch] == 0) continue;  /* skip unconfigured channel */
        /* Same read-before-write guard as the single setter: a model the
         * channel already holds is not re-applied (that would replace the
         * unit's calibration with the generic preset), a factory-paired
         * module is not reconfigured. */
        bool skip = false;
        err = _ct_model_guard(dev, ch, models[ch], false, &skip);
        if (err != ESP_OK) return err;
        if (skip) continue;
        /* Bind WITHOUT a per-channel SAVE — batched behind one terminal save
         * below to spare flash endurance (was 1 + n flash cycles per call).
         * Routes ch0-2 → flat CMD, ch3+ → window field 15. */
        err = _apply_ct_model_ch(dev, ch, models[ch]);
        if (err != ESP_OK) return err;
        any_bound = true;

        /* Post-bind verify (bench finding #2) — the CT_MODEL_CHn mirror
         * reflects the RAM-applied preset, so this works before the save. Only
         * a STABLE WRONG mirror value is a genuine half-apply → re-bind once,
         * then hard-fail. A mirror that merely won't read cleanly (torn under
         * bus load) is NOT a failure: trust the accepted+verified CMD. */
        esp_err_t v = _verify_ct_bind(dev, ch, models[ch]);
        if (v == ESP_ERR_INVALID_STATE) {
            err = _apply_ct_model_ch(dev, ch, models[ch]);  /* re-bind once */
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
    /* Single terminal persist for all channels bound this call. CT_MODEL is
     * user_config — CMD_SAVE_USER_CONFIG persists ct_model_ch[] for all channels
     * (available on production firmware; CMD_SAVE_GAINS is a silent no-op there). */
    if (!any_bound) return ESP_OK;
    err = rbamp_save_user_config(dev);
    if (err != ESP_OK) return err;
    return _sct_after_bind(dev);
}

esp_err_t rbamp_save_gains(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        /* CMD_SAVE_CAL: persist the CAL block (gains, noise floors, THD floors).
         * Completion = CMD_STATE BUSY → 0, then the verdict (no fixed sleep). */
        return _std_cmd_sync(dev, RBAMP_STD_CMD_SAVE_CAL, RBAMP_STD_SETTLE_MS_SAVE_CAL, 2000);
    }
    esp_err_t err = _write_cmd(dev, RBAMP_CMD_SAVE_GAINS);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_SETTLE_MS_SAVE_GAINS);  /* 700 ms flash erase + write */
    return ESP_OK;
}

/* Window in which a module must re-appear at its new address after the reset
 * that applies it (reboot + bus re-enumeration). */
#define RBAMP_STD_ADDR_CONFIRM_MS 3000u

/** @internal Does an STD module answer 'ST' at @p addr? Polled every 200 ms up
 * to @p timeout_ms (the module is off the bus while it reboots). */
static esp_err_t _std_probe_st(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t timeout_ms)
{
    i2c_master_dev_handle_t h = NULL;
    esp_err_t err = _add_device(bus, addr, &h);
    if (err != ESP_OK) return err;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t id[2] = { 0, 0 };
    for (;;) {
        err = _std_raw_read(h, RBAMP_STD_REG_ID, id, sizeof id);
        if (err == ESP_OK || esp_timer_get_time() >= deadline_us) break;
        _sleep_ms(200);
    }
    (void)i2c_master_bus_rm_device(h);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
    return (id[0] == 'S' && id[1] == 'T') ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t rbamp_prepare_address_change(rbamp_handle_t dev, uint8_t new_addr)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (new_addr < 0x08 || new_addr > 0x77 || new_addr == dev->addr) {
        return ESP_ERR_INVALID_ARG;
    }
    /* v1.3 A2 / truth-doc §6.1: the two-phase address commit (magic-armed
     * CMD_COMMIT_ADDR) is production-OK — field-swap of a production spare is
     * supported, no factory-mode gate. (Was factory-gated under the v1.2
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
    if (dev->std) {
        /* STD: CFG_I2C_ADDR → CMD_SAVE_CONFIG, synchronous (BUSY → 0, then the
         * verdict: ERR_PARAM = out-of-range address, module unchanged; 0xF9 =
         * NVM rate limit → ESP_ERR_NOT_FINISHED, prepare again and retry after
         * ~5 s) → CMD_RESET → the module must answer 'ST' at the NEW address
         * before this handle switches to it. On any failure the handle keeps
         * the old address. */
        err = _std_write_u8(dev, RBAMP_STD_REG_CFG_I2C_ADDR, new_addr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "address change: CFG_I2C_ADDR write failed (%s); device unchanged",
                     esp_err_to_name(err));
            return err;
        }
        err = _std_cmd_sync(dev, RBAMP_STD_CMD_SAVE_CONFIG, RBAMP_STD_SETTLE_MS_SAVE_CONFIG, 2000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "address change: SAVE_CONFIG failed (%s); device unchanged",
                     esp_err_to_name(err));
            return err;
        }
        err = _std_cmd(dev, RBAMP_STD_CMD_RESET);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "address change: RESET write failed (%s) — the module adopts the new "
                          "address at its next restart", esp_err_to_name(err));
        }
        _sleep_ms(RBAMP_STD_SETTLE_MS_RESET);
        err = _std_probe_st(dev->bus, new_addr, RBAMP_STD_ADDR_CONFIRM_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "address change: no module answers 'ST' at 0x%02X (%s) — handle stays at 0x%02X",
                     new_addr, esp_err_to_name(err), dev->addr);
            return err;
        }
        goto rebind;
    }
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

rebind:
    /* Re-bind the dev handle at the new address. */
    ;
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
    /* STD: the module reset behind the address change cleared its RAM arm
     * mask — restart the period path at the new address (E15). */
    if (dev->std) return _std_prime_period(dev);
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
    if (dev->std) {
        esp_err_t err = _std_wait_idle(dev, 1000, RBAMP_STD_CMD_RESET);   /* never write while BUSY */
        if (err != ESP_OK) return err;
        err = _std_cmd(dev, RBAMP_STD_CMD_RESET);
        if (err != ESP_OK) return err;
        _sleep_ms(RBAMP_STD_SETTLE_MS_RESET);
        /* The reset cleared the module's RAM arm mask — restart the period
         * path so latch energy keeps counting (E15). */
        return _std_prime_period(dev);
    }
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
        .scl_wait_us     = _scl_wait_us(),
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
    if (dev->std) {
        esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_VARIANT, &hw);
        if (err != ESP_OK) return err;
        *out = _std_variant_map(hw);
        return ESP_OK;
    }
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_HW_VARIANT, &hw);
    if (err != ESP_OK) return err;
    *out = rbampi_basic_variant_shape(hw, NULL, NULL) ? (rbamp_variant_t)hw : RBAMP_VARIANT_UNKNOWN;
    return ESP_OK;
}

esp_err_t rbamp_read_capability(rbamp_handle_t dev, uint16_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    /* STD: the RBAMP_STD_CAP_* bitmap (different bit meanings — see rbamp_std_info). */
    if (dev->std) return _std_read_u16(dev, RBAMP_STD_REG_CAPABILITIES, out);
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
    if (dev->std) return _std_read(dev, RBAMP_STD_REG_UID, out, RBAMP_STD_REG_UID_SIZE);
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
    if (dev->std) return _std_read_u8(dev, RBAMP_STD_REG_ERROR, out);
    return _read_u8(dev, RBAMP_V2_REG_ERROR, out);
}

/* STD event block exists from map 0x36 (image 0x75 implements it; earlier
 * 0x36 images read the registers as 0, which is harmless). */
static inline bool _std_has_events(rbamp_handle_t dev) { return dev->std_version >= 0x36; }

esp_err_t rbamp_read_event_flags(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (!_std_has_events(dev)) return ESP_ERR_NOT_SUPPORTED;
        return _std_read_u8(dev, RBAMP_STD_REG_EVENT_FLAGS, out);
    }
    return _read_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, out);
}

esp_err_t rbamp_clear_event_flags(rbamp_handle_t dev, uint8_t mask)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* Write-1-to-clear: writing the mask back clears exactly those bits. */
    if (dev->std) {
        if (!_std_has_events(dev)) return ESP_ERR_NOT_SUPPORTED;
        return _std_write_u8(dev, RBAMP_STD_REG_EVENT_FLAGS, mask);
    }
    if (mask & RBAMP_V2_EVENT_RESET_OCCURRED) {
        dev->digest_armed = false;      /* a restart was acknowledged: the digest mask is gone */
        dev->digest_reset_seen = false;
        dev->sct_stale = true;          /* ... and the applied gains are the factory ones again */
    }
    return _write_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, mask);
}

/* -------------------------------------------------------------------------
 * BASIC digest
 * ------------------------------------------------------------------------- */

static uint8_t _digest_effective_mask(rbamp_handle_t dev)
{
    if (dev->digest_mask) return dev->digest_mask;
    return (uint8_t)(RBAMP_V2_DIGEST_I_RMS | RBAMP_V2_DIGEST_P_REAL
                     | (dev->has_voltage_hw ? RBAMP_V2_DIGEST_U_RMS : 0u));
}

static bool _digest_available(rbamp_handle_t dev)
{
    return !dev->std && (dev->capability & RBAMP_V2_CAP_DIGEST) != 0;
}

/* Write the mask and verify the module took it (ERR_PARAM = a field the
 * variant does not have). */
static esp_err_t _digest_arm(rbamp_handle_t dev)
{
    const uint8_t mask = _digest_effective_mask(dev);
    esp_err_t err = _write_u8(dev, RBAMP_V2_REG_DIGEST_CONFIG, mask);
    if (err != ESP_OK) return err;
    err = _check_write_error(dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "digest: module rejected field mask 0x%02X", mask);
        return ESP_ERR_INVALID_ARG;
    }
    dev->digest_armed = true;
    return ESP_OK;
}

esp_err_t rbamp_set_digest_fields(rbamp_handle_t dev, uint8_t fields)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (fields == 0 || (fields & ~RBAMPI_DIGEST_MASK_ALL)) return ESP_ERR_INVALID_ARG;
    dev->digest_mask  = fields;
    dev->digest_armed = false;
    if (dev->variant == RBAMP_VARIANT_UNKNOWN && dev->capability == 0) return ESP_OK;  /* before begin: stored */
    if (!_digest_available(dev)) return ESP_ERR_NOT_SUPPORTED;
    return _digest_arm(dev);
}

esp_err_t rbamp_read_digest(rbamp_handle_t dev, rbamp_digest_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!_digest_available(dev)) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err;
    if (dev->sct_stale) {
        err = _sct_refresh(dev);
        if (err != ESP_OK) return err;
    }
    if (!dev->digest_armed) {
        err = _digest_arm(dev);
        if (err != ESP_OK) return err;
    }
    const uint8_t mask = _digest_effective_mask(dev);
    const uint8_t n_ch = dev->channels < RBAMPI_DIGEST_MAX_CH ? dev->channels : (uint8_t)RBAMPI_DIGEST_MAX_CH;
    rbampi_digest_layout_t L;
    if (!rbampi_digest_layout(mask, n_ch, &L)) return ESP_ERR_INVALID_STATE;
    uint8_t b[RBAMP_V2_REG_DIGEST_SIZE] = {0};
    _Static_assert(2u + 2u * RBAMPI_DIGEST_MAX_CH + 2u + 2u * RBAMPI_DIGEST_MAX_CH + RBAMPI_DIGEST_MAX_CH
                   <= RBAMP_V2_REG_DIGEST_SIZE, "digest layout exceeds the block");
    err = _read_block(dev, RBAMP_V2_REG_DIGEST, b, L.len);
    if (err != ESP_OK) return err;

    /* A module restart empties DIGEST_CONFIG. The STATUS mirror says whether
     * events are pending; only then is EVENT_FLAGS read (one extra byte), and
     * a RESET_OCCURRED re-arms the mask once per episode and re-reads. */
    if (b[0] & RBAMPI_STATUS_EVENTS_PENDING) {
        uint8_t flags = 0;
        if (_read_u8(dev, RBAMP_V2_REG_EVENT_FLAGS, &flags) == ESP_OK
                && rbampi_digest_needs_rearm(&dev->digest_reset_seen, true, flags)) {
            dev->sct_stale = true;           /* the restart also reverted the applied gains */
            err = _sct_refresh(dev);
            if (err != ESP_OK) return err;
            err = _digest_arm(dev);
            if (err != ESP_OK) return err;
            err = _read_block(dev, RBAMP_V2_REG_DIGEST, b, L.len);
            if (err != ESP_OK) return err;
        }
    } else {
        dev->digest_reset_seen = false;
    }
    rbampi_digest_decode(b, mask, n_ch, out);
    for (uint8_t ch = 0; ch < n_ch; ++ch) {            /* SCT-013 rating correction */
        const float sc = _sct_scale(dev, ch);
        if (sc == 1.0f) continue;
        out->i_a[ch] *= sc;
        float pw = (float)out->p_w[ch] * sc;
        if (pw > 32767.0f) pw = 32767.0f; else if (pw < -32768.0f) pw = -32768.0f;
        out->p_w[ch] = (int16_t)pw;                     /* truncated toward zero, like the module */
    }
    return ESP_OK;
}

uint8_t rbamp_begin_event_flags(rbamp_handle_t dev)
{
    return dev ? dev->begin_event_flags : 0;
}

esp_err_t rbamp_read_event_mask(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        if (!_std_has_events(dev)) return ESP_ERR_NOT_SUPPORTED;
        return _std_read_u8(dev, RBAMP_STD_REG_EVENT_MASK, out);
    }
    return _read_u8(dev, RBAMP_V2_REG_EVENT_MASK, out);
}

esp_err_t rbamp_set_event_mask(rbamp_handle_t dev, uint8_t mask)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err;
    if (dev->std) {
        if (!_std_has_events(dev)) return ESP_ERR_NOT_SUPPORTED;
        err = _std_write_u8(dev, RBAMP_STD_REG_EVENT_MASK, mask);
    } else {
        err = _write_u8(dev, RBAMP_V2_REG_EVENT_MASK, mask);
    }
    if (err != ESP_OK) return err;
    uint8_t rb = 0;
    err = rbamp_read_event_mask(dev, &rb);
    if (err != ESP_OK) return err;
    return (rb == mask) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t rbamp_set_event_thresholds(rbamp_handle_t dev, float i_amps, float p_watts)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) {
        /* The module compares its own (uncorrected) values: express the true-amp
         * thresholds in the module's scale. With mixed sensors the largest scale
         * is used, so a channel with a smaller one alarms early, never late. */
        float smax = 1.0f;
        for (uint8_t ch = 0; ch < dev->channels && ch < RBAMP_MAX_CHANNELS; ++ch)
            if (dev->sct[ch].scale > smax) smax = dev->sct[ch].scale;
        i_amps  /= smax;
        p_watts /= smax;
        /* BASIC v1.3: THRESH_I_HI u16 LE in 0.01 A (max over channels),
         * THRESH_P_HI u16 LE in W (SIGNED sum of channels); 0xFFFF = off. */
        const uint16_t i_raw = (!(i_amps > 0.0f) || i_amps >= 655.34f) ? 0xFFFFu : (uint16_t)(i_amps * 100.0f + 0.5f);
        const uint16_t p_raw = (!(p_watts > 0.0f) || p_watts >= 65534.5f) ? 0xFFFFu : (uint16_t)(p_watts + 0.5f);
        esp_err_t err = _write_u8(dev, RBAMP_V2_REG_THRESH_I_HI, (uint8_t)i_raw);
        if (err == ESP_OK) err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_THRESH_I_HI + 1), (uint8_t)(i_raw >> 8));
        if (err == ESP_OK) err = _write_u8(dev, RBAMP_V2_REG_THRESH_P_HI, (uint8_t)p_raw);
        if (err == ESP_OK) err = _write_u8(dev, (uint8_t)(RBAMP_V2_REG_THRESH_P_HI + 1), (uint8_t)(p_raw >> 8));
        return err;
    }
    if (!_std_has_events(dev)) return ESP_ERR_NOT_SUPPORTED;
    const uint32_t i_ua = (!(i_amps > 0.0f) || i_amps > 4000.0f) ? 0xFFFFFFFFu : (uint32_t)(i_amps * 1e6f);
    const uint32_t p_mw = (!(p_watts > 0.0f) || p_watts > 4000000.0f) ? 0xFFFFFFFFu : (uint32_t)(p_watts * 1e3f);
    esp_err_t err = _std_write_u32(dev, RBAMP_STD_REG_THRESH_I, i_ua);
    if (err != ESP_OK) return err;
    return _std_write_u32(dev, RBAMP_STD_REG_THRESH_P, p_mw);
}

esp_err_t rbamp_has_error(rbamp_handle_t dev, bool *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t flags = 0;
    esp_err_t err = rbamp_read_event_flags(dev, &flags);   /* both lines, same bit */
    if (err != ESP_OK) return err;
    *out = (flags & RBAMP_V2_EVENT_ERROR) != 0;
    return ESP_OK;
}

esp_err_t rbamp_clear_error(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->std) return rbamp_clear_event_flags(dev, RBAMP_STD_EVT_ERROR);   /* W1C is the STD way */
    esp_err_t err = _write_cmd(dev, RBAMP_V2_CMD_CLEAR_ERROR);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_V2_SETTLE_MS_CLEAR_ERROR);
    return ESP_OK;
}

esp_err_t rbamp_save_user_config(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        /* STD: CMD_SAVE_CONFIG persists the CFG block (read-modify-write, the
         * calibration page is untouched). Completion = CMD_STATE BUSY → 0,
         * then the verdict: ERR_PARAM on a bad config value, 0xF9 rate limit
         * → ESP_ERR_NOT_FINISHED (retry after ~5 s). */
        return _std_cmd_sync(dev, RBAMP_STD_CMD_SAVE_CONFIG, RBAMP_STD_SETTLE_MS_SAVE_CONFIG, 2000);
    }
    esp_err_t err = _write_cmd(dev, RBAMP_V2_CMD_SAVE_USER_CONFIG);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_V2_SETTLE_MS_SAVE_USER_CONFIG);  /* 700 ms flash */
    return ESP_OK;
}

esp_err_t rbamp_is_provisioned(rbamp_handle_t dev, bool *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) {
        /* STD: "provisioned" = calibration page valid (STATUS.cal_ok). */
        uint8_t st = 0;
        esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_STATUS, &st);
        if (err != ESP_OK) return err;
        *out = (st & RBAMP_STD_ST_CAL_OK) != 0;
        return ESP_OK;
    }
    uint8_t e = 0;
    esp_err_t err = _read_u8(dev, RBAMP_V2_REG_ERROR, &e);
    if (err != ESP_OK) return err;
    *out = (e != RBAMP_V2_DEV_ERR_FLASH_PARAMS_BAD);
    return ESP_OK;
}

esp_err_t rbamp_read_active_address(rbamp_handle_t dev, uint8_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (dev->std) return _std_read_u8(dev, RBAMP_STD_REG_CFG_I2C_ADDR, out);
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
        case ESP_ERR_INVALID_RESPONSE:  return "Stale, empty or out-of-range response (period not ready / sanity reject / "
                                               "period skipped after a module reset)";
        case ESP_ERR_INVALID_STATE:     return "Wrong state or call sequence (sensor class UNSET / address-commit not armed / "
                                               "latch bank re-armed after a module reset — drop this period / bus conflict)";
        case ESP_ERR_TIMEOUT:           return "Bus, arm-window or OTA-status timeout";
        case ESP_ERR_NO_MEM:            return "Out of memory";
        case ESP_ERR_NOT_SUPPORTED:     return "Feature not available on this product line or firmware version";
        case ESP_ERR_INVALID_VERSION:   return "OTA image version below the running one (rejected)";
        case ESP_ERR_INVALID_CRC:       return "Module rejected the value (range / CRC check) or the OTA image (bad CRC or header)";
        case ESP_ERR_INVALID_MAC:       return "OTA image rejected by the module (authentication: MAC / seal / signature)";
        case ESP_ERR_NOT_ALLOWED:       return "Module refused the operation (key / legal gate)";
        case ESP_ERR_NOT_FOUND:         return "Not present on the module (no such record / no certificate / no device key)";
        case ESP_ERR_NOT_FINISHED:      return "Module NVM write rate-limited — retry after ~5 s";
        case ESP_ERR_INVALID_SIZE:      return "OTA image does not fit the module's bank";
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

/* -------------------------------------------------------------------------
 * STD line — block reads
 * ------------------------------------------------------------------------- */

/** @internal Plausibility gate for one decoded STD field: NaN + mask bit. */
static void _std_sane(rbamp_handle_t dev, float *v, float max_abs,
                      uint8_t *mask, uint8_t bit)
{
    if (!isfinite(*v) || fabsf(*v) > max_abs) {
        dev->sanity_reject_count++;
        *v = NAN;
        *mask |= bit;
    }
}

/* Size of the CURRENT / TOTALS block we decode: up to and including Q1. */
#define RBAMP_STD_IP_BLOCK_LEN  (RBAMP_STD_CURRENT_Q1 + 4u)

/** @internal Decode one I+P block (CURRENT or TOTALS — same layout). */
static esp_err_t _std_read_ip_block(rbamp_handle_t dev, uint16_t base,
                                    rbamp_std_channel_t *out)
{
    uint8_t b[RBAMP_STD_IP_BLOCK_LEN];
    esp_err_t err = _std_read(dev, base, b, sizeof b);
    if (err != ESP_OK) return err;
    memset(out, 0, sizeof(*out));
    out->i_rms  = (float)_le_i32(b + RBAMP_STD_CURRENT_I_RMS)  * 1e-6f;
    out->p      = (float)_le_i32(b + RBAMP_STD_CURRENT_P)      * 1e-3f;
    out->q      = (float)_le_i32(b + RBAMP_STD_CURRENT_Q)      * 1e-3f;
    out->s      = (float)_le_i32(b + RBAMP_STD_CURRENT_S)      * 1e-3f;
    out->pf     = (float)_le_i16(b + RBAMP_STD_CURRENT_PF)     / 1024.0f;
    out->i_peak = (float)_le_i32(b + RBAMP_STD_CURRENT_I_PEAK) * 1e-6f;
    out->e_import_mws = _le_i64(b + RBAMP_STD_CURRENT_E_POS);
    out->e_export_mws = _le_i64(b + RBAMP_STD_CURRENT_E_NEG);
    out->thd_i  = (float)_le_u16(b + RBAMP_STD_CURRENT_THD_I)  / 100.0f;
    out->pf1    = (float)_le_i16(b + RBAMP_STD_CURRENT_PF1)    / 1024.0f;
    out->q1     = (float)_le_i32(b + RBAMP_STD_CURRENT_Q1)     * 1e-3f;
    _std_sane(dev, &out->i_rms,  RBAMP_SANITY_LIMIT_I,  &out->implausible, RBAMP_FIELD_CURRENT);
    _std_sane(dev, &out->i_peak, RBAMP_SANITY_LIMIT_I,  &out->implausible, RBAMP_FIELD_CURRENT);
    _std_sane(dev, &out->p,      RBAMP_SANITY_LIMIT_P,  &out->implausible, RBAMP_FIELD_POWER);
    _std_sane(dev, &out->q,      RBAMP_SANITY_LIMIT_P,  &out->implausible, RBAMP_FIELD_POWER);
    _std_sane(dev, &out->s,      RBAMP_SANITY_LIMIT_P,  &out->implausible, RBAMP_FIELD_POWER);
    _std_sane(dev, &out->q1,     RBAMP_SANITY_LIMIT_P,  &out->implausible, RBAMP_FIELD_POWER);
    _std_sane(dev, &out->pf,     RBAMP_SANITY_LIMIT_PF, &out->implausible, RBAMP_FIELD_PF);
    _std_sane(dev, &out->pf1,    RBAMP_SANITY_LIMIT_PF, &out->implausible, RBAMP_FIELD_PF);
    return ESP_OK;
}

esp_err_t rbamp_std_info(rbamp_handle_t dev, rbamp_std_info_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    out->map_version  = dev->std_version;
    out->capabilities = dev->std_caps;
    out->status       = dev->std_status;
    out->n_voltage    = dev->n_voltage;
    out->n_current    = dev->n_current;
    out->variant      = dev->std_variant;
    out->active_bank  = dev->std_active_bank;
    out->ota_bank_size = dev->std_ota_bank_size;
    out->period_source = dev->std_period_src;
    return ESP_OK;
}

esp_err_t rbamp_std_read_channel(rbamp_handle_t dev, uint8_t ch, rbamp_std_channel_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (ch >= dev->channels) return ESP_ERR_INVALID_ARG;
    return _std_read_ip_block(dev, RBAMP_STD_CURRENT(ch), out);
}

/* TOTALS blocks carry no energy accumulators (E_POS / E_NEG are not populated
 * by the module — energy lives in the counters at 0x0800, rbamp_std_read_energy).
 * Report 0 so a reader never mistakes the unpopulated fields for live data. */
static esp_err_t _std_read_totals_block(rbamp_handle_t dev, uint8_t k, rbamp_std_channel_t *out)
{
    esp_err_t err = _std_read_ip_block(dev, RBAMP_STD_TOTALS(k), out);
    if (err != ESP_OK) return err;
    out->e_import_mws = 0;
    out->e_export_mws = 0;
    return ESP_OK;
}

esp_err_t rbamp_std_read_totals(rbamp_handle_t dev, uint8_t k, rbamp_std_channel_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    /* k=0 grand total is always populated; per-phase totals (1..3) are
     * published from map 0x36 (before that they read 0): on a single-phase
     * module k=1 equals channel 0 and k=2/3 read 0. Below 0x36 only
     * multi-phase modules had them. */
    if (k != 0 && dev->std_version < 0x36) return ESP_ERR_NOT_SUPPORTED;
    if (k != 0 && dev->std_version >= 0x36) {
        if (k >= RBAMP_STD_TOTALS_MAX) return ESP_ERR_INVALID_ARG;
        return _std_read_totals_block(dev, k, out);
    }
    if (k != 0 && (dev->n_voltage < 2 || k > dev->n_voltage || k >= RBAMP_STD_TOTALS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    return _std_read_totals_block(dev, k, out);
}

esp_err_t rbamp_std_read_voltage(rbamp_handle_t dev, uint8_t v, rbamp_std_voltage_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (v >= dev->n_voltage) return ESP_ERR_INVALID_ARG;
    uint8_t b[RBAMP_STD_VOLTAGE_ANGLE + 2u];   /* U_RMS .. ANGLE in one burst (ANGLE reads 0 before image 0x7A) */
    esp_err_t err = _std_read(dev, RBAMP_STD_VOLTAGE(v), b, sizeof b);
    if (err != ESP_OK) return err;
    memset(out, 0, sizeof(*out));
    out->u_rms  = (float)_le_i32(b + RBAMP_STD_VOLTAGE_U_RMS)  * 1e-3f;
    out->u_peak = (float)_le_i32(b + RBAMP_STD_VOLTAGE_U_PEAK) * 1e-3f;
    out->thd_u  = (float)_le_u16(b + RBAMP_STD_VOLTAGE_THD_U)  / 100.0f;
    out->angle_valid = rbampi_angle_decode(_le_i16(b + RBAMP_STD_VOLTAGE_ANGLE),
                                           dev->std_fw_ver >= RBAMPI_FW_ANGLE_MIN, &out->angle_deg);
    _std_sane(dev, &out->u_rms,  RBAMP_SANITY_LIMIT_V, &out->implausible, RBAMP_FIELD_VOLTAGE);
    _std_sane(dev, &out->u_peak, RBAMP_SANITY_LIMIT_V, &out->implausible, RBAMP_FIELD_VOLTAGE);
    return ESP_OK;
}

esp_err_t rbamp_std_read_health(rbamp_handle_t dev, rbamp_std_health_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_STATUS, &out->status);
    if (err != ESP_OK) return err;
    uint8_t b[8];   /* FRAME_CNT + CRC_ERR are adjacent u32s */
    err = _std_read(dev, RBAMP_STD_REG_FRAME_CNT, b, sizeof b);
    if (err != ESP_OK) return err;
    out->frame_count = _le_u32(b);
    out->crc_errors  = _le_u32(b + 4);
    if (dev->std_version < 0x35) return ESP_OK;   /* the rest of the block is not mapped below 0x35 */
    out->time_valid = (out->status & RBAMP_STD_ST_TIME_VALID) != 0;
    out->nvm_fault  = (out->status & RBAMP_STD_ST_NVM_FAULT) != 0;
    out->maint      = (out->status & RBAMP_STD_ST_MAINT) != 0;
    uint8_t n[4];   /* LTA_STATUS, LTA_SRC, CAL_SRC, LTG_STATUS are adjacent (LTG reads 0 below 0x36) */
    _Static_assert(RBAMP_STD_REG_LTG_STATUS == RBAMP_STD_REG_LTA_STATUS + 3, "NVM status bytes moved");
    err = _std_read(dev, RBAMP_STD_REG_LTA_STATUS, n, sizeof n);
    if (err != ESP_OK) return err;
    out->lta_status   = n[0];
    out->lta_src      = n[1];
    out->cal_src      = n[2];
    out->group_status = (dev->std_version >= 0x36) ? n[3] : 0;
    err = _std_read_u16(dev, RBAMP_STD_REG_LEGAL_NOEN, &out->legal_noen);
    if (err != ESP_OK) return err;
    /* RING_DROP .. LOST_FRAMES sit in one vendor-diag run: one burst. */
    uint8_t d[RBAMP_STD_REG_LOST_FRAMES + 4 - RBAMP_STD_REG_RING_DROP];
    err = _std_read(dev, RBAMP_STD_REG_RING_DROP, d, sizeof d);
    if (err != ESP_OK) return err;
    out->ring_drop   = _le_u16(d);
    out->lost_frames = _le_u32(d + (RBAMP_STD_REG_LOST_FRAMES - RBAMP_STD_REG_RING_DROP));
    err = _std_read_u32(dev, RBAMP_STD_ENERGY_BASE + RBAMP_STD_ENERGY_UPTIME, &out->uptime_s);
    if (err != ESP_OK) return err;
    if (dev->std_fw_ver >= 0x00010076u) {   /* RESET_CAUSE u8 + pad + BOOT_COUNT u16 in one small burst */
        _Static_assert(RBAMP_STD_REG_BOOT_COUNT == RBAMP_STD_REG_RESET_CAUSE + 2, "RESET_CAUSE / BOOT_COUNT moved");
        uint8_t r[4];
        err = _std_read(dev, RBAMP_STD_REG_RESET_CAUSE, r, sizeof r);
        if (err != ESP_OK) return err;
        out->reset_cause = r[0];
        out->boot_count  = _le_u16(r + 2);
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * STD line — clock / identity / history / authenticity (map 0x35+)
 * ------------------------------------------------------------------------- */

static inline uint64_t _le_u48(const uint8_t *p)
{
    return (uint64_t)_le_u32(p) | ((uint64_t)_le_u16(p + 4) << 32);
}

static esp_err_t _std_gate_35(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std || dev->std_version < 0x35) return ESP_ERR_NOT_SUPPORTED;
    return ESP_OK;
}

/* Poll the command-state register until BUSY clears (map 0x35+; a no-op
 * before). Used BEFORE a COMMAND write: a command written while the module is
 * busy is dropped (OVERRUN, bit1). */
static esp_err_t _std_wait_idle(rbamp_handle_t dev, uint32_t timeout_ms, uint8_t opcode_for_log)
{
    if (dev->std_version < 0x35) return ESP_OK;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        uint8_t st = 0;
        esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_CMD_STATE, &st);
        if (err == ESP_OK && !(st & 0x01u)) return ESP_OK;
        if (err != ESP_OK && err != ESP_FAIL && err != ESP_ERR_TIMEOUT) return err;
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGE(TAG, "command 0x%02X: module still busy with the previous command after %" PRIu32 " ms",
                     opcode_for_log, timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        _sleep_ms(10);
    }
}

/* Completion of the command just written = CMD_STATE bit0 (BUSY) back to 0,
 * never ERROR alone: before image 0x79 ERROR reads 0x00 while a long command
 * (COMMIT ~2 s, saves, page program) is still running; from 0x79 it stays
 * PENDING until the result. CMD_SEQ (completed-command counter) must have
 * advanced as well — a BUSY=0 sampled before the main loop picked the opcode
 * up is not completion. OVERRUN with no CMD_SEQ advance = our opcode was
 * dropped. With @p tolerate_stall transport errors count as "busy" (the slave
 * stalls while it programs flash) and the retry counter is left untouched.
 * Maps < 0x35 have neither register: the map's settle is all there is. */
static esp_err_t _std_wait_done(rbamp_handle_t dev, uint32_t settle_ms, uint32_t timeout_ms,
                                bool tolerate_stall, uint8_t opcode_for_log)
{
    if (dev->std_version < 0x35) {
        _sleep_ms(settle_ms);
        return ESP_OK;
    }
    const uint32_t exhaust     = dev->retry_exhaustion_count;
    const int64_t  start_us    = esp_timer_get_time();
    const int64_t  deadline_us = start_us + (int64_t)timeout_ms * 1000;
    const uint16_t seq0        = dev->std_cmd_seq;
    bool idle_seen = false;
    int64_t idle_us = 0;
    _sleep_ms(10);
    for (;;) {
        uint8_t  st  = 0;
        uint16_t seq = seq0;
        esp_err_t err = tolerate_stall ? _std_read_u8_once(dev, RBAMP_STD_REG_CMD_STATE, &st)
                                       : _std_read_u8(dev, RBAMP_STD_REG_CMD_STATE, &st);
        if (err == ESP_OK && !(st & 0x01u)) {
            const esp_err_t e2 = tolerate_stall ? _std_read_u16_once(dev, RBAMP_STD_REG_CMD_SEQ, &seq)
                                                : _std_read_u16(dev, RBAMP_STD_REG_CMD_SEQ, &seq);
            if (e2 == ESP_OK && seq != seq0) {
                dev->std_cmd_seq = seq;
                if (tolerate_stall) dev->retry_exhaustion_count = exhaust;
                return ESP_OK;
            }
            if (!idle_seen) { idle_seen = true; idle_us = esp_timer_get_time(); }
            /* BUSY=0 but CMD_SEQ unchanged: not picked up yet, or dropped. */
            if ((st & 0x02u) || esp_timer_get_time() - idle_us > 200 * 1000) {
                if (tolerate_stall) dev->retry_exhaustion_count = exhaust;
                if (st & 0x02u) {
                    ESP_LOGE(TAG, "command 0x%02X: dropped by the module (OVERRUN — written while busy)",
                             opcode_for_log);
                    return ESP_ERR_INVALID_STATE;
                }
                if (e2 != ESP_OK) return ESP_OK;          /* no CMD_SEQ this time — trust BUSY=0 */
                ESP_LOGD(TAG, "command 0x%02X: CMD_SEQ did not advance (%u)", opcode_for_log, seq0);
                return ESP_OK;                            /* trivial command, counter not stepped */
            }
        } else if (err != ESP_OK && !tolerate_stall && err != ESP_FAIL && err != ESP_ERR_TIMEOUT) {
            return err;
        }
        if (esp_timer_get_time() >= deadline_us) {
            if (tolerate_stall) dev->retry_exhaustion_count = exhaust;
            ESP_LOGE(TAG, "command 0x%02X: not completed within %" PRIu32 " ms (CMD_STATE=0x%02X, %s)",
                     opcode_for_log, timeout_ms, st, esp_err_to_name(err));
            return ESP_ERR_TIMEOUT;
        }
        _sleep_ms(tolerate_stall ? 20 : 10);
    }
}

/* Synchronous command: wait idle → opcode (one write, no retry) → completion
 * by CMD_STATE/CMD_SEQ → ERROR verdict for THAT command (PENDING is re-read
 * for up to 100 ms). Timeouts are floored at 1 s (contract). */
static esp_err_t _std_cmd_sync(rbamp_handle_t dev, uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms)
{
    if (timeout_ms < 1000) timeout_ms = 1000;
    esp_err_t err = _std_wait_idle(dev, timeout_ms, opcode);
    if (err != ESP_OK) return err;
    _std_mark_seq(dev);
    err = _std_cmd(dev, opcode);
    if (err != ESP_OK) return err;
    err = _std_wait_done(dev, settle_ms, timeout_ms, false, opcode);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 10; ++i) {
        err = _std_check_cmd_error(dev);
        if (err != ESP_ERR_TIMEOUT) break;   /* PENDING right after completion: re-read */
        _sleep_ms(10);
    }
    return err;
}

esp_err_t rbamp_std_wait_idle(rbamp_handle_t dev, uint32_t timeout_ms)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    return _std_wait_idle(dev, timeout_ms ? timeout_ms : 1000u, 0);
}

esp_err_t rbamp_std_set_time(rbamp_handle_t dev, uint32_t unix_epoch)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    err = _std_write_u32(dev, RBAMP_STD_REG_TIME_SET, unix_epoch);
    if (err != ESP_OK) return err;
    err = _std_cmd_sync(dev, RBAMP_STD_CMD_SET_TIME, RBAMP_STD_SETTLE_MS_SET_TIME, 1000);
    if (err == ESP_ERR_INVALID_CRC) {
        /* The module reuses the value-CRC code for "epoch before 2020". */
        ESP_LOGW(TAG, "set_time: module refused epoch %" PRIu32, unix_epoch);
        return ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) return err;
    /* Image 0x73 published TIME_VALID at its next 200 ms pass, later images
     * at the verdict: re-read tolerantly for up to 300 ms. */
    uint8_t st = 0;
    const int64_t deadline_us = esp_timer_get_time() + 300 * 1000;
    do {
        err = _std_read_u8(dev, RBAMP_STD_REG_STATUS, &st);
        if (err != ESP_OK) return err;
        if (st & RBAMP_STD_ST_TIME_VALID) return ESP_OK;
        _sleep_ms(20);
    } while (esp_timer_get_time() < deadline_us);
    ESP_LOGW(TAG, "set_time: accepted but TIME_VALID still clear (status 0x%02X)", st);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t rbamp_std_read_time(rbamp_handle_t dev, uint32_t *unix_epoch, bool *valid)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    if (!unix_epoch && !valid) return ESP_ERR_INVALID_ARG;
    uint32_t t = 0; uint8_t st = 0;
    err = _std_read_u32(dev, RBAMP_STD_ENERGY_BASE + RBAMP_STD_ENERGY_RTC_EPOCH, &t);
    if (err != ESP_OK) return err;
    err = _std_read_u8(dev, RBAMP_STD_REG_STATUS, &st);
    if (err != ESP_OK) return err;
    if (unix_epoch) *unix_epoch = t;
    if (valid) *valid = (st & RBAMP_STD_ST_TIME_VALID) != 0;
    return ESP_OK;
}

esp_err_t rbamp_std_read_image_version(rbamp_handle_t dev, uint32_t *out)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    if (!out) return ESP_ERR_INVALID_ARG;
    err = _std_read_u32(dev, RBAMP_STD_REG_FW_VER, out);
    if (err == ESP_OK && *out == 0xFFFFFFFFu) *out = 0;
    return err;
}

/* ---- history ---- */

/* Select the ring on the module (period byte; the back index when a fetch follows). */
static esp_err_t _std_hist_select(rbamp_handle_t dev, rbamp_std_hist_period_t period, uint16_t back, bool with_back)
{
    if (period > RBAMP_STD_HIST_EVENTS) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _std_write_u8(dev, RBAMP_STD_REG_HIST_PERIOD, (uint8_t)period);
    if (err != ESP_OK || !with_back) return err;
    const uint8_t b[2] = { (uint8_t)back, (uint8_t)(back >> 8) };
    return _std_write(dev, RBAMP_STD_REG_HIST_BACK, b, sizeof b);
}

esp_err_t rbamp_std_hist_info(rbamp_handle_t dev, rbamp_std_hist_period_t period, rbamp_std_hist_info_t *out)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    if (!out) return ESP_ERR_INVALID_ARG;
    /* The module fills COUNT / CAP / RECSZ / OLDEST / NEWEST for the selected
     * ring only when it serves a HIST_READ (bench 2026-10-11, E20 with root) —
     * selecting the period alone leaves them 0. Fetch the newest record as
     * the selector; "no record" verdicts are fine here. */
    err = _std_hist_select(dev, period, 0, true);
    if (err != ESP_OK) return err;
    err = _std_cmd_sync(dev, RBAMP_STD_CMD_HIST_READ, RBAMP_STD_SETTLE_MS_HIST_READ, 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG && err != ESP_ERR_INVALID_CRC && err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    _Static_assert(RBAMP_STD_REG_HIST_NEWEST == RBAMP_STD_REG_HIST_COUNT + 10, "HIST block layout changed");
    uint8_t b[14];   /* COUNT u16 · CAP u16 · RECSZ u16 · OLDEST u32 · NEWEST u32 */
    err = _std_read(dev, RBAMP_STD_REG_HIST_COUNT, b, sizeof b);
    if (err != ESP_OK) return err;
    out->count       = _le_u16(b);
    out->capacity    = _le_u16(b + 2);
    out->record_size = _le_u16(b + 4);
    out->oldest_ts   = _le_u32(b + 6);
    out->newest_ts   = _le_u32(b + 10);
    return ESP_OK;
}

esp_err_t rbamp_std_hist_read_raw(rbamp_handle_t dev, rbamp_std_hist_period_t period, uint16_t back,
                                  uint8_t rec[RBAMP_STD_REG_HIST_REC_SIZE], uint8_t *flags)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    if (!rec) return ESP_ERR_INVALID_ARG;
    err = _std_hist_select(dev, period, back, true);
    if (err != ESP_OK) return err;
    err = _std_cmd_sync(dev, RBAMP_STD_CMD_HIST_READ, RBAMP_STD_SETTLE_MS_HIST_READ, 1000);
    /* No such record: the module answers PARAM (index beyond the ring) or the
     * value-check code (empty ring, bench 2026-10-11) — both mean "not there". */
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_CRC) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    uint8_t f = 0;
    err = _std_read_u8(dev, RBAMP_STD_REG_HIST_FLAGS, &f);
    if (err != ESP_OK) return err;
    if (flags) *flags = f;
    if (!(f & 0x01u)) return ESP_ERR_NOT_FOUND;
    return _std_read(dev, RBAMP_STD_REG_HIST_REC, rec, RBAMP_STD_REG_HIST_REC_SIZE);
}

/* Energy-period record: fixed part per the generated HIST_REC offsets, then
 * per phase imp u32 · exp u32 (0.01 Wh, this period); the phase count follows
 * the module's record size (56 B + 8 B per phase). */
#define _HIST_FIXED_LEN RBAMP_STD_HIST_REC_PH_IMP_CWH
_Static_assert(RBAMP_STD_HIST_REC_PH_EXP_CWH == RBAMP_STD_HIST_REC_PH_IMP_CWH + 4 * RBAMP_STD_HIST_PHASES,
               "HIST_REC per-phase layout changed");

esp_err_t rbamp_std_hist_read(rbamp_handle_t dev, rbamp_std_hist_period_t period, uint16_t back,
                              rbamp_std_hist_record_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (period == RBAMP_STD_HIST_EVENTS) return ESP_ERR_INVALID_ARG;   /* rbamp_std_event_read */
    uint8_t rec[RBAMP_STD_REG_HIST_REC_SIZE];
    uint8_t f = 0;
    esp_err_t err = rbamp_std_hist_read_raw(dev, period, back, rec, &f);
    if (err != ESP_OK) return err;
    uint16_t recsz = 0;
    err = _std_read_u16(dev, RBAMP_STD_REG_HIST_RECSZ, &recsz);
    if (err != ESP_OK) return err;
    if (recsz < _HIST_FIXED_LEN || recsz > RBAMP_STD_REG_HIST_REC_SIZE) return ESP_ERR_INVALID_RESPONSE;
    memset(out, 0, sizeof(*out));
    out->ts       = _le_u32(rec + RBAMP_STD_HIST_REC_TS);
    out->flags    = _le_u16(rec + RBAMP_STD_HIST_REC_FLAGS);
    out->n        = _le_u16(rec + RBAMP_STD_HIST_REC_N);
    out->from_ram = (f & 0x02u) != 0;
    out->v_rms    = (float)_le_u16(rec + RBAMP_STD_HIST_REC_V_DV) * 0.1f;
    out->i_total  = (float)_le_u32(rec + RBAMP_STD_HIST_REC_I_MA) * 1e-3f;
    out->p_avg    = (float)_le_i32(rec + RBAMP_STD_HIST_REC_P_W);
    out->q_avg    = (float)_le_i32(rec + RBAMP_STD_HIST_REC_Q_VAR);
    out->s_avg    = (float)_le_i32(rec + RBAMP_STD_HIST_REC_S_VA);
    out->pf       = (float)_le_i16(rec + RBAMP_STD_HIST_REC_PF) / 1024.0f;
    out->p_min    = (float)_le_i32(rec + RBAMP_STD_HIST_REC_MIN_P_W);
    out->p_max    = (float)_le_i32(rec + RBAMP_STD_HIST_REC_MAX_P_W);
    out->i_min    = (float)_le_u32(rec + RBAMP_STD_HIST_REC_MIN_I_MA) * 1e-3f;
    out->i_max    = (float)_le_u32(rec + RBAMP_STD_HIST_REC_MAX_I_MA) * 1e-3f;
    out->e_import_wh = _le_u48(rec + RBAMP_STD_HIST_REC_E_IMP_WH);
    out->e_export_wh = _le_u48(rec + RBAMP_STD_HIST_REC_E_EXP_WH);
    uint8_t nph = (uint8_t)((recsz - _HIST_FIXED_LEN) / 8u);
    if (nph > RBAMP_STD_HIST_PHASES) nph = RBAMP_STD_HIST_PHASES;
    out->phases = nph;
    for (uint8_t p = 0; p < nph; ++p) {
        out->ph_import_wh[p] = (double)_le_u32(rec + _HIST_FIXED_LEN + 4u * p) / 100.0;
        out->ph_export_wh[p] = (double)_le_u32(rec + _HIST_FIXED_LEN + 4u * nph + 4u * p) / 100.0;
    }
    return ESP_OK;
}

/* Event record (packed, LE, 32 B): ts u32 · uptime u32 · type u8 · flags u8 ·
 * boot u16 · a_before u32 · a_after u32 · e_before u48 · e_after u48. */
esp_err_t rbamp_std_event_read(rbamp_handle_t dev, uint16_t back, rbamp_std_event_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t rec[RBAMP_STD_REG_HIST_REC_SIZE];
    esp_err_t err = rbamp_std_hist_read_raw(dev, RBAMP_STD_HIST_EVENTS, back, rec, NULL);
    if (err != ESP_OK) return err;
    memset(out, 0, sizeof(*out));
    out->ts          = _le_u32(rec + RBAMP_STD_EVENT_REC_TS);
    out->uptime_s    = _le_u32(rec + RBAMP_STD_EVENT_REC_UPTIME_S);
    out->type        = rec[RBAMP_STD_EVENT_REC_TYPE];
    out->flags       = rec[RBAMP_STD_EVENT_REC_FLAGS];
    out->boot        = _le_u16(rec + RBAMP_STD_EVENT_REC_BOOT);
    out->a_before    = _le_u32(rec + RBAMP_STD_EVENT_REC_A_BEFORE);
    out->a_after     = _le_u32(rec + RBAMP_STD_EVENT_REC_A_AFTER);
    out->e_before_wh = _le_u48(rec + RBAMP_STD_EVENT_REC_E_BEFORE);
    out->e_after_wh  = _le_u48(rec + RBAMP_STD_EVENT_REC_E_AFTER);
    return ESP_OK;
}

/* ---- authenticity ---- */

esp_err_t rbamp_std_read_certificate(rbamp_handle_t dev, rbamp_std_certificate_t *out)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    err = _std_read(dev, RBAMP_STD_REG_SEC_CERT, out->raw, RBAMP_STD_REG_SEC_CERT_SIZE);
    if (err != ESP_OK) return err;
    err = _std_read_u8(dev, RBAMP_STD_REG_SEC_CERT_STATE, &out->state);
    if (err != ESP_OK) return err;
    if (out->state == 0xFF) out->state = 0;
    /* 'CRT1' · ver · variant · hw_rev · rsv · UID[12] · batch · issued · pubkey[32] · mfr key id · signature[64] */
    if (memcmp(out->raw, "CRT1", 4) != 0) return ESP_ERR_NOT_FOUND;
    out->version = out->raw[4];
    out->variant = out->raw[5];
    out->hw_rev  = out->raw[6];
    memcpy(out->uid, out->raw + 8, sizeof out->uid);
    out->batch   = _le_u32(out->raw + 20);
    out->issued  = _le_u32(out->raw + 24);
    memcpy(out->device_pubkey, out->raw + 28, sizeof out->device_pubkey);
    out->mfr_key_id = _le_u32(out->raw + 60);
    return ESP_OK;
}

esp_err_t rbamp_std_attest(rbamp_handle_t dev, const uint8_t challenge[32], rbamp_std_attestation_t *out)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    if (!challenge || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    err = _std_write(dev, RBAMP_STD_REG_SEC_CHALLENGE_SIG, challenge, 32);
    if (err != ESP_OK) return err;
    err = _std_cmd_sync(dev, RBAMP_STD_CMD_ATTEST, RBAMP_STD_SETTLE_MS_ATTEST, 2000);
    if (err == ESP_ERR_NOT_ALLOWED) {
        uint8_t e = 0;
        if (_std_read_u8(dev, RBAMP_STD_REG_ERROR, &e) == ESP_OK && e == RBAMP_STD_DEV_ERR_NO_KEY) {
            return ESP_ERR_NOT_FOUND;   /* unprovisioned: no device key */
        }
    }
    if (err != ESP_OK) return err;
    /* The signing may finish after the command verdict: poll the attestation
     * state (0 none · 1 done · 2 refused) for up to a second. */
    uint8_t st = 0;
    const int64_t deadline_us = esp_timer_get_time() + 1000 * 1000;
    do {
        err = _std_read_u8(dev, RBAMP_STD_REG_SEC_ATT_STATE, &st);
        if (err != ESP_OK) return err;
        if (st == 1 || st == 2) break;
        _sleep_ms(20);
    } while (esp_timer_get_time() < deadline_us);
    if (st != 1) {
        ESP_LOGW(TAG, "attest: module did not report the attestation as done (state %u)", st);
        return ESP_ERR_INVALID_STATE;
    }
    err = _std_read(dev, RBAMP_STD_REG_SEC_CHALLENGE_SIG, out->signature, sizeof out->signature);
    if (err != ESP_OK) return err;
    err = _std_read(dev, RBAMP_STD_REG_SEC_ATT_DATA, out->data, sizeof out->data);
    if (err != ESP_OK) return err;
    out->fw_version  = _le_u32(out->data + 0);
    out->generation  = _le_u32(out->data + 4);
    out->e_import_wh = _le_i64(out->data + 8);
    out->e_export_wh = _le_i64(out->data + 16);
    return ESP_OK;
}

/* ---- identity / commissioning helpers ---- */

esp_err_t rbamp_std_command(rbamp_handle_t dev, uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    return _std_cmd_sync(dev, opcode, settle_ms, timeout_ms ? timeout_ms : 1000u);   /* waits idle first */
}

esp_err_t rbamp_std_hist_flush(rbamp_handle_t dev)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    return _std_cmd_sync(dev, RBAMP_STD_CMD_HIST_FLUSH, RBAMP_STD_SETTLE_MS_HIST_FLUSH, 2000);
}

esp_err_t rbamp_std_locate(rbamp_handle_t dev)
{
    esp_err_t err = _std_gate_35(dev);
    if (err != ESP_OK) return err;
    return _std_cmd_sync(dev, RBAMP_STD_CMD_IDENTIFY, RBAMP_STD_SETTLE_MS_IDENTIFY, 1000);
}

esp_err_t rbamp_std_identify(rbamp_handle_t dev, rbamp_std_identity_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    memset(out, 0, sizeof(*out));
    out->map_version  = dev->std_version;
    out->capabilities = dev->std_caps;
    out->variant      = dev->std_variant;
    out->n_voltage    = dev->n_voltage;
    out->n_current    = dev->n_current;
    out->i2c_addr     = dev->addr;
    esp_err_t err = rbamp_read_uid(dev, out->uid);
    if (err != ESP_OK) return err;
    if (dev->std_version >= 0x35) {
        err = _std_read_u32(dev, RBAMP_STD_REG_FW_VER, &out->fw_version);
        if (err != ESP_OK) return err;
        if (out->fw_version == 0xFFFFFFFFu) out->fw_version = 0;
    }
    if (dev->std_version >= 0x36) {
        err = _std_read_u32(dev, RBAMP_STD_REG_BOARD_ID, &out->board_id);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* One raw read on a temporary device handle (no retry layer: a scan must be
 * fast and a NACK is the answer). */
static esp_err_t _std_raw_read(i2c_master_dev_handle_t h, uint16_t reg, uint8_t *buf, size_t n)
{
    const uint8_t p[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(h, p, sizeof p, buf, n, CONFIG_RBAMP_I2C_TIMEOUT_MS);
}

esp_err_t rbamp_std_probe(i2c_master_bus_handle_t bus, uint8_t addr, rbamp_std_identity_t *out)
{
    if (!bus || !out) return ESP_ERR_INVALID_ARG;
    if (addr < 0x08 || addr > 0x77) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->i2c_addr = addr;
    i2c_master_dev_handle_t h = NULL;
    esp_err_t err = _add_device(bus, addr, &h);
    if (err != ESP_OK) return err;
    uint8_t b[RBAMP_STD_REG_VARIANT + 1];   /* ID .. VARIANT in one burst */
    err = _std_raw_read(h, RBAMP_STD_REG_ID, b, sizeof b);
    if (err != ESP_OK) {
        (void)i2c_master_bus_rm_device(h);
        return ESP_ERR_NOT_FOUND;            /* nothing acknowledges there */
    }
    if (b[0] != 'S' || b[1] != 'T') {
        (void)i2c_master_bus_rm_device(h);
        return ESP_ERR_INVALID_RESPONSE;     /* something else lives at this address */
    }
    out->map_version  = b[RBAMP_STD_REG_VERSION];
    out->capabilities = _le_u16(b + RBAMP_STD_REG_CAPABILITIES);
    out->n_voltage    = b[RBAMP_STD_REG_N_VOLTAGE];
    out->n_current    = b[RBAMP_STD_REG_N_CURRENT];
    out->variant      = b[RBAMP_STD_REG_VARIANT];
    err = _std_raw_read(h, RBAMP_STD_REG_UID, out->uid, sizeof out->uid);
    uint8_t v[4];
    if (err == ESP_OK && out->map_version >= 0x35) {
        err = _std_raw_read(h, RBAMP_STD_REG_FW_VER, v, sizeof v);
        if (err == ESP_OK) out->fw_version = (_le_u32(v) == 0xFFFFFFFFu) ? 0 : _le_u32(v);
    }
    if (err == ESP_OK && out->map_version >= 0x36) {
        err = _std_raw_read(h, RBAMP_STD_REG_BOARD_ID, v, sizeof v);
        if (err == ESP_OK) out->board_id = _le_u32(v);
    }
    (void)i2c_master_bus_rm_device(h);
    return err;
}

/* ---- board id / addressing (map 0x36+) ---- */

static esp_err_t _std_gate_36(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std || dev->std_version < 0x36) return ESP_ERR_NOT_SUPPORTED;
    return ESP_OK;
}

esp_err_t rbamp_std_read_board_id(rbamp_handle_t dev, uint32_t *out)
{
    esp_err_t err = _std_gate_36(dev);
    if (err != ESP_OK) return err;
    if (!out) return ESP_ERR_INVALID_ARG;
    return _std_read_u32(dev, RBAMP_STD_REG_BOARD_ID, out);
}

esp_err_t rbamp_std_ota_board_ok(rbamp_handle_t dev, uint32_t image_board_word)
{
    uint32_t mine = 0;
    esp_err_t err = rbamp_std_read_board_id(dev, &mine);
    if (err != ESP_OK) return err;
    if (mine != image_board_word) {
        ESP_LOGE(TAG, "ota: image is for board 0x%08" PRIX32 ", this module is 0x%08" PRIX32 " — not sent",
                 image_board_word, mine);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t rbamp_std_set_address_by_uid(i2c_master_bus_handle_t bus, uint8_t shared_addr,
                                       const uint8_t uid[12], uint8_t new_addr, uint32_t restart_ms)
{
    if (!bus || !uid) return ESP_ERR_INVALID_ARG;
    if (shared_addr < 0x08 || shared_addr > 0x77 || new_addr < 0x08 || new_addr > 0x77) return ESP_ERR_INVALID_ARG;
    _Static_assert(RBAMP_STD_REG_ADDR_UID_I2C == RBAMP_STD_REG_ADDR_UID + RBAMP_STD_REG_ADDR_UID_SIZE
                   && RBAMP_STD_REG_ADDR_UID_UNIT == RBAMP_STD_REG_ADDR_UID_I2C + 1,
                   "ADDR_UID / ADDR_UID_I2C / ADDR_UID_UNIT must be contiguous");
    /* Write-only on the shared address: UID + new I2C address + unit 0 (keep)
     * in one burst, then the command. No read here — several modules may
     * answer and their data would collide. No retry layer either: a NACK on a
     * shared address means nobody is there. */
    i2c_master_dev_handle_t h = NULL;
    esp_err_t err = _add_device(bus, shared_addr, &h);
    if (err != ESP_OK) return err;
    uint8_t w[2 + RBAMP_STD_REG_ADDR_UID_SIZE + 2] = {
        (uint8_t)(RBAMP_STD_REG_ADDR_UID >> 8), (uint8_t)RBAMP_STD_REG_ADDR_UID,
    };
    memcpy(w + 2, uid, RBAMP_STD_REG_ADDR_UID_SIZE);
    w[2 + RBAMP_STD_REG_ADDR_UID_SIZE]     = new_addr;
    w[2 + RBAMP_STD_REG_ADDR_UID_SIZE + 1] = 0;
    err = i2c_master_transmit(h, w, sizeof w, CONFIG_RBAMP_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        const uint8_t c[3] = { (uint8_t)(RBAMP_STD_REG_COMMAND >> 8), (uint8_t)RBAMP_STD_REG_COMMAND,
                               RBAMP_STD_CMD_ADDR_BY_UID };
        err = i2c_master_transmit(h, c, sizeof c, CONFIG_RBAMP_I2C_TIMEOUT_MS);
    }
    (void)i2c_master_bus_rm_device(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "addr_by_uid: no module acknowledged on 0x%02X (%s)", shared_addr, esp_err_to_name(err));
        return err;
    }
    _sleep_ms(RBAMP_STD_SETTLE_MS_ADDR_BY_UID + (restart_ms ? restart_ms : 1500u));

    /* Probe the new address: identity block first, then the UID must match. */
    err = _add_device(bus, new_addr, &h);
    if (err != ESP_OK) return err;
    const uint8_t p_id[2]  = { (uint8_t)(RBAMP_STD_REG_ID >> 8),  (uint8_t)RBAMP_STD_REG_ID };
    const uint8_t p_uid[2] = { (uint8_t)(RBAMP_STD_REG_UID >> 8), (uint8_t)RBAMP_STD_REG_UID };
    uint8_t id[RBAMP_STD_REG_ID_SIZE] = {0};
    uint8_t got[RBAMP_STD_REG_UID_SIZE] = {0};
    err = ESP_FAIL;
    for (int i = 0; i < 5 && err != ESP_OK; ++i) {
        err = i2c_master_transmit_receive(h, p_id, sizeof p_id, id, sizeof id, CONFIG_RBAMP_I2C_TIMEOUT_MS);
        if (err != ESP_OK) _sleep_ms(200);
    }
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(h, p_uid, sizeof p_uid, got, sizeof got, CONFIG_RBAMP_I2C_TIMEOUT_MS);
    }
    (void)i2c_master_bus_rm_device(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "addr_by_uid: nothing answers at 0x%02X after the restart", new_addr);
        return ESP_ERR_NOT_FOUND;
    }
    if (id[0] != 'S' || id[1] != 'T' || memcmp(got, uid, sizeof got) != 0) {
        ESP_LOGW(TAG, "addr_by_uid: the module at 0x%02X is not the one addressed (UID differs)", new_addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * STD line — latch banks
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_std_read_energy(rbamp_handle_t dev, rbamp_std_energy_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    memset(out, 0, sizeof(*out));
    return _std_read_energy_raw(dev, out);
}

/* rbamp_std_energy_delta: see rbamp_std_decode.c (pure, host-tested). */

esp_err_t rbamp_std_arm_banks(rbamp_handle_t dev, uint8_t mask)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std || dev->std_period_src != RBAMP_STD_PERIOD_LATCH) return ESP_ERR_NOT_SUPPORTED;
    if (!(mask & 0x01u)) {
        ESP_LOGW(TAG, "arm mask 0x%02X clears bank 0 — rbamp_read_period_snapshot will report EMPTY", mask);
    }
    return _std_write_u8(dev, RBAMP_STD_REG_PERIOD_ARM, mask);
}

esp_err_t rbamp_std_latch(rbamp_handle_t dev, uint8_t bank)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std || dev->std_period_src != RBAMP_STD_PERIOD_LATCH) return ESP_ERR_NOT_SUPPORTED;
    if (bank >= RBAMP_STD_LATCH_MAX) return ESP_ERR_INVALID_ARG;
    return _std_latch_bank(dev, bank);
}

esp_err_t rbamp_std_read_latch(rbamp_handle_t dev, uint8_t bank, rbamp_std_latch_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std || dev->std_period_src != RBAMP_STD_PERIOD_LATCH) return ESP_ERR_NOT_SUPPORTED;
    if (bank >= RBAMP_STD_LATCH_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _std_read_latch_bank(dev, bank, out);
    if (err != ESP_OK) return err;
    if (out->win_count == 0) {
        /* Same E15 check as the period path: an empty snapshot from a bank
         * that is no longer armed means the module reset. Re-arm it and
         * primer-latch so the caller's NEXT period is whole; the caller must
         * drop this one and restart its own dt anchor. */
        bool was_armed = true;
        err = _std_ensure_armed(dev, bank, &was_armed);
        if (err != ESP_OK) return err;
        if (!was_armed) {
            ESP_LOGW(TAG, "latch bank %u was DISARMED (module reset?) — re-armed and "
                          "primer-latched; drop this period", bank);
            err = _std_latch_bank(dev, bank);
            if (err != ESP_OK) return err;
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

esp_err_t rbamp_std_latch_wh(const rbamp_std_latch_t *snap, uint32_t dt_ms, uint8_t ch,
                             double *wh_import, double *wh_export)
{
    if (!snap) return ESP_ERR_INVALID_ARG;
    if (ch != RBAMP_STD_TOTAL && ch >= snap->channels) return ESP_ERR_INVALID_ARG;
    if (snap->nsamp == 0) return ESP_ERR_INVALID_STATE;
    const int64_t e_pos = (ch == RBAMP_STD_TOTAL) ? snap->e_import_mws : snap->e_import_ch_mws[ch];
    const int64_t e_neg = (ch == RBAMP_STD_TOTAL) ? snap->e_export_mws : snap->e_export_ch_mws[ch];
    /* mW·samples → Wh: /1000 (mW→W) /3600 (s→h) × seconds-per-sample (dt/nsamp). */
    const double per_sample_h = ((double)dt_ms / 1000.0) / (double)snap->nsamp / 3600.0;
    if (wh_import) *wh_import = (double)e_pos / 1000.0 * per_sample_h;
    if (wh_export) *wh_export = (double)e_neg / 1000.0 * per_sample_h;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * STD line — configuration / calibration commands
 * ------------------------------------------------------------------------- */

/* CFG_START_UA exists from image 0x7B (the register is reserved before — a
 * write there would be silently ignored, so refuse without bus traffic). */
static bool _std_has_start_ua(rbamp_handle_t dev)
{
    return (dev->std_caps & RBAMP_STD_CAP_ENERGY_UWH) && dev->std_fw_ver >= RBAMPI_FW_START_UA_MIN;
}

esp_err_t rbamp_std_read_config(rbamp_handle_t dev, rbamp_std_config_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    uint8_t b[RBAMP_STD_REG_CFG_HARM - RBAMP_STD_REG_CFG_CLASS + 1u];   /* 0x0070..0x0076 */
    esp_err_t err = _std_read(dev, RBAMP_STD_REG_CFG_CLASS, b, sizeof b);
    if (err != ESP_OK) return err;
    /* Offsets relative to CFG_CLASS (0x70): CT_MODEL +1, TIER +2, I2C_ADDR +3,
     * RATE +4 (u16), HARM +6. */
    out->sensor_class = b[0];
    out->ct_model     = b[1];
    out->tier         = b[2];
    out->i2c_addr     = b[3];
    out->rate_ms      = _le_u16(b + 4);
    out->harmonics    = (b[6] & 0x01u) != 0;
    out->start_ua_supported = _std_has_start_ua(dev);
    out->start_ua           = 0;
    if (out->start_ua_supported) {
        err = _std_read_u16(dev, RBAMP_STD_REG_CFG_START_UA, &out->start_ua);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t rbamp_std_get_start_current(rbamp_handle_t dev, uint16_t *ua)
{
    if (!dev || !ua) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (!_std_has_start_ua(dev)) return ESP_ERR_NOT_SUPPORTED;
    return _std_read_u16(dev, RBAMP_STD_REG_CFG_START_UA, ua);
}

esp_err_t rbamp_std_set_start_current(rbamp_handle_t dev, uint16_t ua)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (!_std_has_start_ua(dev)) return ESP_ERR_NOT_SUPPORTED;
    if (!rbampi_start_ua_valid(ua)) return ESP_ERR_INVALID_ARG;   /* no bus write */
    const uint8_t w[2] = { (uint8_t)ua, (uint8_t)(ua >> 8) };
    esp_err_t err = _std_write(dev, RBAMP_STD_REG_CFG_START_UA, w, sizeof w);
    if (err != ESP_OK) return err;
    uint16_t rb = 0xFFFF;
    err = _std_read_u16(dev, RBAMP_STD_REG_CFG_START_UA, &rb);
    if (err != ESP_OK) return err;
    return (rb == ua) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t rbamp_std_read_phase_seq(rbamp_handle_t dev, rbamp_std_phase_seq_t *out)
{
    if (!dev || !out) return ESP_ERR_INVALID_ARG;
    *out = RBAMP_STD_PHASE_SEQ_NA;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (dev->std_fw_ver < RBAMPI_FW_ANGLE_MIN) return ESP_ERR_NOT_SUPPORTED;   /* byte reads 0 there: no bus traffic */
    uint8_t raw = 0;
    esp_err_t err = _std_read_u8(dev, RBAMP_STD_REG_PHASE_SEQ, &raw);
    if (err != ESP_OK) return err;
    *out = rbampi_phase_seq_decode(raw);
    return ESP_OK;
}

esp_err_t rbamp_std_set_harmonics(rbamp_handle_t dev, bool enable)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (!(dev->std_caps & RBAMP_STD_CAP_THD)) return ESP_ERR_NOT_SUPPORTED;
    /* RAM-only switch (not persisted); bit 0 = base-harmonics extraction. */
    esp_err_t err = _std_write_u8(dev, RBAMP_STD_REG_CFG_HARM, enable ? 0x01u : 0x00u);
    if (err != ESP_OK) return err;
    uint8_t rb = 0xFF;
    err = _std_read_u8(dev, RBAMP_STD_REG_CFG_HARM, &rb);
    if (err != ESP_OK) return err;
    return ((rb & 0x01u) == (enable ? 0x01u : 0x00u)) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t rbamp_std_cal_thd_floor(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (!(dev->std_caps & RBAMP_STD_CAP_THD)) return ESP_ERR_NOT_SUPPORTED;
    return _std_cmd_sync(dev, RBAMP_STD_CMD_CAL_THD_FLOOR, RBAMP_STD_SETTLE_MS_CAL_THD_FLOOR, 2000);
}

/* -------------------------------------------------------------------------
 * STD line — firmware update (two-phase OTA over I2C)
 *
 * BEGIN erases the inactive bank (synchronous on the module, which is OFF
 * THE BUS meanwhile; OTA_STATUS reads READY afterwards) → pages of 256 B via OTA_IDX + OTA_PAYLOAD +
 * BLOCK_WR → COMMIT verifies CRC + anti-downgrade and ARMS the swap without
 * rebooting (OTA_STATUS 2 ok / 3 err / 4 downgrade) → ACTIVATE reboots into
 * the armed bank when the host chooses. A bad new image is rolled back by the
 * module's bootloader; calibration lives outside the banks and survives.
 * ------------------------------------------------------------------------- */

static esp_err_t _std_ota_gate(rbamp_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!dev->std) return ESP_ERR_NOT_SUPPORTED;
    if (dev->std_version < 0x33 || !(dev->std_caps & RBAMP_STD_CAP_OTA)) {
        return ESP_ERR_NOT_SUPPORTED;   /* two-phase layout from map 0x33 */
    }
    return ESP_OK;
}

/* While the module erases or programs flash its I2C slave STALLS (the
 * interrupt handler lives in the flash being written): transactions NACK or
 * clock-stretch into a driver timeout for ~0.8 s on an erase and tens of ms
 * per page (bench, 2026-09-29). Those transport errors mean "busy", not
 * "gone" — the poll below tolerates them until the deadline, and the retry
 * counter is restored afterwards so an expected stall does not read as bus
 * trouble in diagnostics. */

/** @internal Poll OTA_STATUS until it is one of @p accept (bit per value). */
static esp_err_t _std_ota_wait(rbamp_handle_t dev, uint32_t timeout_ms,
                               uint8_t accept, uint8_t *last)
{
    const int64_t  start_us    = esp_timer_get_time();
    const int64_t  deadline_us = start_us + (int64_t)timeout_ms * 1000;
    const uint32_t exhaust     = dev->retry_exhaustion_count;
    uint8_t   st  = 0xFF;
    esp_err_t err = ESP_FAIL;
    do {
        err = _std_read_u8_once(dev, RBAMP_STD_REG_OTA_STATUS, &st);   /* no bus reset mid-stall */
        if (err == ESP_OK && st < 8 && (accept & (1u << st))) {
            dev->retry_exhaustion_count = exhaust;
            *last = st;
            return ESP_OK;
        }
        if (err != ESP_OK) st = 0xFF;   /* stalled — keep polling */
        _sleep_ms(50);
    } while (esp_timer_get_time() < deadline_us);
    dev->retry_exhaustion_count = exhaust;
    *last = st;
    ESP_LOGE(TAG, "ota: status=%u (last read %s) after %lld ms", st,
             esp_err_to_name(err), (long long)((esp_timer_get_time() - start_us) / 1000));
    return ESP_ERR_TIMEOUT;
}

/* Command verdict after a flash-programming command: completion FIRST
 * (CMD_STATE BUSY → 0 + CMD_SEQ, stall-tolerant — before image 0x79 ERROR
 * reads 0x00 while the command still runs, so ERROR alone is never
 * completion), then the ERROR verdict. Two things still make a single ERROR
 * read unreliable: (1) it can land while the slave is stalled and come back
 * with a byte from a lost register pointer (bench saw 0x53 = the ID byte);
 * (2) ERROR = PENDING until the result is in. Anything that is not a final
 * verdict code is re-read until @p timeout_ms. Maps < 0x35 (no CMD_STATE)
 * keep the settle + verdict-code loop. */
static esp_err_t _std_ota_cmd_verdict(rbamp_handle_t dev, uint32_t settle_ms, uint32_t timeout_ms)
{
    const uint32_t exhaust = dev->retry_exhaustion_count;
    const int64_t  t0_us   = esp_timer_get_time();
    esp_err_t err = _std_wait_done(dev, settle_ms, timeout_ms, true, 0);
    if (err != ESP_OK) return err;
    const int64_t spent_ms = (esp_timer_get_time() - t0_us) / 1000;
    const int64_t deadline_us = esp_timer_get_time()
                              + (int64_t)((spent_ms < timeout_ms) ? (timeout_ms - (uint32_t)spent_ms) : 100u) * 1000;
    do {
        uint8_t e = 0xFF;
        err = _std_read_u8_once(dev, RBAMP_STD_REG_ERROR, &e);
        if (err == ESP_OK && _std_err_is_verdict(e)) {
            dev->retry_exhaustion_count = exhaust;
            return _std_check_cmd_error(dev);   /* maps the code; second read is cheap */
        }
        _sleep_ms(10);
    } while (esp_timer_get_time() < deadline_us);
    dev->retry_exhaustion_count = exhaust;
    ESP_LOGE(TAG, "ota: no command verdict within %" PRIu32 " ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/* OTA_DIAG reason names — log text only; the map lists the codes in the
 * register note, the public API returns the raw byte. */
static const char *_std_ota_reason_name(uint8_t r)
{
    static const char *const kName[] = {
        "none", "RANGE", "ERASE", "ERASE_VERIFY", "RESEND", "PROG", "VERIFY", "HDR",
        "SECTOR", "CRC", "MAC", "DOWNGRADE", "SWAP", "SEAL", "SIG", "BOARD",
    };
    return (r < sizeof kName / sizeof kName[0]) ? kName[r] : "?";
}

/* Read the first byte of OTA_DIAG (the reason) on modules that have the OTA
 * extension block (map 0x35+); 0 when unavailable. Remembered on the handle. */
static uint8_t _std_ota_fetch_reason(rbamp_handle_t dev)
{
    uint8_t r = 0;
    if (dev->std_version >= 0x35 && _std_read_u8(dev, RBAMP_STD_REG_OTA_DIAG, &r) != ESP_OK) r = 0;
    if (r == 0xFF) r = 0;   /* unmapped on this module */
    dev->std_ota_last_reason = r;
    return r;
}

/* Verdict window for one page: the module may hold BLOCK_WR pending for its
 * per-page throttle (>= 250 ms on signed-image modules) before programming. */
#define RBAMP_STD_OTA_PAGE_VERDICT_MS   3000u
#define RBAMP_STD_OTA_CMD_VERDICT_MS    2000u
/* Trial window after ACTIVATE: the new image must run this long without a
 * reset before the bootloader confirms it (contract item 9). */
#define RBAMP_STD_OTA_CONFIRM_MS        10000u

/** @internal Device-reported bank size, validated; 0 bytes written on error. */
static esp_err_t _std_ota_bank_size(rbamp_handle_t dev, uint32_t *bank)
{
    esp_err_t err = _std_read_u32(dev, RBAMP_STD_REG_OTA_BANK_SIZE, bank);
    if (err != ESP_OK) return err;
    const uint32_t b = *bank;
    if (b == 0 || b == 0xFFFFFFFFu || (b % RBAMP_STD_OTA_PAGE_SIZE) != 0
            || b < 2u * RBAMP_STD_OTA_PAGE_SIZE) {
        ESP_LOGE(TAG, "ota: module reports no usable bank size (0x%08" PRIX32 ")", b);
        return ESP_ERR_NOT_SUPPORTED;
    }
    dev->std_ota_bank_size = b;
    return ESP_OK;
}

esp_err_t rbamp_std_ota_status(rbamp_handle_t dev, uint8_t *out)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (!out) return ESP_ERR_INVALID_ARG;
    return _std_read_u8(dev, RBAMP_STD_REG_OTA_STATUS, out);
}

esp_err_t rbamp_std_ota_begin(rbamp_handle_t dev, uint32_t timeout_ms)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (timeout_ms == 0) timeout_ms = 5000;
    err = _std_wait_idle(dev, 1000, RBAMP_STD_CMD_OTA_BEGIN);
    if (err != ESP_OK) return err;
    _std_mark_seq(dev);
    err = _std_cmd(dev, RBAMP_STD_CMD_OTA_BEGIN);
    if (err != ESP_OK) return err;
    /* OTA_STATUS reads 1 both while the erase runs and once it is done, so
     * polling alone cannot tell "ready" from "busy" (a stale 1 from an
     * earlier session would pass immediately). Sleep the map's settle for the
     * erase first, THEN poll — the poll then only covers a slow erase and
     * tolerates the stall (see _std_ota_wait). */
    _sleep_ms(RBAMP_STD_SETTLE_MS_OTA_BEGIN);
    uint8_t st = 0;
    err = _std_ota_wait(dev, timeout_ms, (uint8_t)(1u << RBAMP_STD_OTA_READY), &st);
    if (err != ESP_OK) return err;
    return _std_ota_cmd_verdict(dev, 0, RBAMP_STD_OTA_CMD_VERDICT_MS);
}

esp_err_t rbamp_std_ota_write_page(rbamp_handle_t dev, uint32_t page_idx,
                                   const uint8_t page[RBAMP_STD_REG_OTA_PAYLOAD_SIZE])
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (!page) return ESP_ERR_INVALID_ARG;
    err = _std_write_u32(dev, RBAMP_STD_REG_OTA_IDX, page_idx);
    if (err != ESP_OK) return err;
    /* One resend on a payload-CRC reject (the module compared what it
     * received against its expected CRC and refused the page). */
    for (int attempt = 0; attempt < 2; ++attempt) {
        err = _std_write(dev, RBAMP_STD_REG_OTA_PAYLOAD, page, RBAMP_STD_REG_OTA_PAYLOAD_SIZE);
        if (err != ESP_OK) return err;
        if (dev->std_version >= 0x35) {
            /* Optional end-to-end check (OTA extension block): the module
             * compares its received CRC-32 with this and refuses the page
             * with ERROR 0xFC on a mismatch — then we resend. */
            err = _std_write_u32(dev, RBAMP_STD_REG_OTA_PCRC_EXP,
                                 rbamp_std_crc32(page, RBAMP_STD_REG_OTA_PAYLOAD_SIZE));
            if (err != ESP_OK) return err;
        }
        _std_mark_seq(dev);
        err = _std_cmd(dev, RBAMP_STD_CMD_OTA_BLOCK_WR);
        if (err != ESP_OK) return err;
        /* Map settle (30 ms as of E12) covers the page-programming stall; the
         * verdict helper re-reads on an impossible byte and waits out PENDING. */
        err = _std_ota_cmd_verdict(dev, RBAMP_STD_SETTLE_MS_OTA_BLOCK_WR,
                                   RBAMP_STD_OTA_PAGE_VERDICT_MS);
        if (err != ESP_ERR_INVALID_CRC) break;
        ESP_LOGW(TAG, "ota page %" PRIu32 ": payload CRC rejected — resending", page_idx);
    }
    if (err != ESP_OK) return err;
    /* After every page the session must still be open: OTA_STATUS == 1
     * (READY). 0 = session lost (module restarted / aborted), 3 = page failed
     * — either way the stream cannot continue; restart from BEGIN. */
    uint8_t st = 0;
    err = _std_read_u8(dev, RBAMP_STD_REG_OTA_STATUS, &st);
    if (err != ESP_OK) return err;
    if (st != RBAMP_STD_OTA_READY) {
        const uint8_t r = _std_ota_fetch_reason(dev);
        ESP_LOGE(TAG, "ota page %" PRIu32 ": session not open after the page (status %u, reason %u %s) — restart from BEGIN",
                 page_idx, st, r, _std_ota_reason_name(r));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t rbamp_std_ota_commit(rbamp_handle_t dev, uint32_t timeout_ms)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    /* Signed-image modules erase the untouched sectors and verify MAC +
     * signature at COMMIT; allow well over the plain CRC check's time. */
    if (timeout_ms == 0) timeout_ms = 15000;
    if (timeout_ms < 3000) timeout_ms = 3000;   /* contract: COMMIT >= 3 s */
    err = _std_wait_idle(dev, 1000, RBAMP_STD_CMD_OTA_COMMIT);
    if (err != ESP_OK) return err;
    _std_mark_seq(dev);
    err = _std_cmd(dev, RBAMP_STD_CMD_OTA_COMMIT);
    if (err != ESP_OK) return err;
    _sleep_ms(RBAMP_STD_SETTLE_MS_OTA_COMMIT);
    uint8_t st = 0;
    err = _std_ota_wait(dev, timeout_ms,
                        (uint8_t)((1u << RBAMP_STD_OTA_OK) | (1u << RBAMP_STD_OTA_ERR)
                                | (1u << RBAMP_STD_OTA_DOWNGRADE)), &st);
    if (err != ESP_OK) return err;
    switch (st) {
        case RBAMP_STD_OTA_OK:
            dev->std_ota_last_reason = 0;
            return _std_ota_cmd_verdict(dev, 0, RBAMP_STD_OTA_CMD_VERDICT_MS);   /* armed */
        case RBAMP_STD_OTA_DOWNGRADE:
            ESP_LOGW(TAG, "ota commit: image version below the running one — rejected");
            return ESP_ERR_INVALID_VERSION;
        default: {
            /* Rejected. Modules with the OTA extension block say why: an
             * authentication failure (MAC / seal / signature) is a different
             * problem from a corrupt image, so it gets its own error code. */
            const uint8_t r = _std_ota_fetch_reason(dev);
            ESP_LOGE(TAG, "ota commit: image rejected — reason %u (%s)", r, _std_ota_reason_name(r));
            if (r == RBAMP_STD_OTA_RSN_BOARD) return ESP_ERR_INVALID_ARG;   /* image for another board */
            return (r == RBAMP_STD_OTA_RSN_MAC || r == RBAMP_STD_OTA_RSN_SEAL || r == RBAMP_STD_OTA_RSN_SIG)
                   ? ESP_ERR_INVALID_MAC : ESP_ERR_INVALID_CRC;
        }
    }
}

esp_err_t rbamp_std_ota_diag(rbamp_handle_t dev, uint8_t *reason)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (!reason) return ESP_ERR_INVALID_ARG;
    if (dev->std_version < 0x35) return ESP_ERR_NOT_SUPPORTED;
    *reason = _std_ota_fetch_reason(dev);
    return ESP_OK;
}

esp_err_t rbamp_std_ota_abort(rbamp_handle_t dev)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (dev->std_version < 0x35) return ESP_ERR_NOT_SUPPORTED;   /* FW_ABORT is a map-0x35 command */
    err = _std_wait_idle(dev, 1000, RBAMP_STD_CMD_FW_ABORT);
    if (err != ESP_OK) return err;
    _std_mark_seq(dev);
    err = _std_cmd(dev, RBAMP_STD_CMD_FW_ABORT);
    if (err != ESP_OK) return err;
    return _std_ota_cmd_verdict(dev, RBAMP_STD_SETTLE_MS_FW_ABORT, RBAMP_STD_OTA_CMD_VERDICT_MS);
}

esp_err_t rbamp_std_ota_staged_version(rbamp_handle_t dev, uint32_t *out)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (!out) return ESP_ERR_INVALID_ARG;
    if (dev->std_version < 0x35) return ESP_ERR_NOT_SUPPORTED;
    return _std_read_u32(dev, RBAMP_STD_REG_FW_STAGED_VERSION, out);
}

/** @internal Stream the first @p n_pages pages of @p img ascending, padding
 * with 0xFF past @p len; blank pages are skipped except page 0 when @p force0
 * is set, since a prepared image carries its nonce there. Progress counts
 * every page. */
static esp_err_t _std_ota_stream_pages(rbamp_handle_t dev, const uint8_t *img, size_t len,
                                       uint32_t n_pages, uint32_t total, bool force0,
                                       rbamp_std_ota_progress_cb_t cb, void *ctx)
{
    const uint32_t page_sz = RBAMP_STD_OTA_PAGE_SIZE;
    uint8_t page[RBAMP_STD_OTA_PAGE_SIZE];
    for (uint32_t idx = 0; idx < n_pages; ++idx) {
        const size_t off = (size_t)idx * page_sz;
        const size_t n   = (off >= len) ? 0 : ((len - off < page_sz) ? (len - off) : page_sz);
        memset(page, 0xFF, sizeof page);
        if (n) memcpy(page, img + off, n);
        bool blank = !(force0 && idx == 0);   /* erased flash already reads 0xFF */
        for (uint32_t i = 0; i < page_sz && blank; ++i) blank = (page[i] == 0xFF);
        if (!blank) {
            esp_err_t err = rbamp_std_ota_write_page(dev, idx, page);
            if (err != ESP_OK) return err;
        }
        if (cb) cb(idx + 1, total, ctx);
    }
    return ESP_OK;
}

esp_err_t rbamp_std_ota_write_prepared(rbamp_handle_t dev, const uint8_t *image, size_t size,
                                       rbamp_std_ota_progress_cb_t cb, void *ctx)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (!image || size == 0) return ESP_ERR_INVALID_ARG;
    uint32_t bank = 0;
    err = _std_ota_bank_size(dev, &bank);
    if (err != ESP_OK) return err;
    /* A prepared image IS the bank: same size, header already in its last
     * page. Anything else is the wrong file (or the raw-image path). */
    if (size != bank) {
        ESP_LOGE(TAG, "ota: prepared image is %u B but the module's bank is 0x%08" PRIX32
                      " — wrong file for this module?", (unsigned)size, bank);
        return ESP_ERR_INVALID_SIZE;
    }
    err = rbamp_std_ota_begin(dev, 0);
    if (err != ESP_OK) return err;
    const uint32_t n_pages = bank / RBAMP_STD_OTA_PAGE_SIZE;
    err = _std_ota_stream_pages(dev, image, size, n_pages, n_pages, true, cb, ctx);
    if (err != ESP_OK) return err;
    return rbamp_std_ota_commit(dev, 0);
}

esp_err_t rbamp_std_ota_write_prepared_pair(rbamp_handle_t dev,
                                            const uint8_t *bank_a, size_t size_a,
                                            const uint8_t *bank_b, size_t size_b,
                                            rbamp_std_ota_progress_cb_t cb, void *ctx)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    if (dev->std_version < 0x35) return ESP_ERR_NOT_SUPPORTED;   /* needs FW_ACTIVE_BANK */
    uint8_t active = 0xFF;
    err = _std_read_u8(dev, RBAMP_STD_REG_FW_ACTIVE_BANK, &active);
    if (err != ESP_OK) return err;
    if (active > 1) {
        ESP_LOGE(TAG, "ota: module reports no running bank id (0x%02X)", active);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* Write the image built for the bank that is NOT running. */
    ESP_LOGI(TAG, "ota: running bank %c — writing the bank-%c image", active ? 'B' : 'A',
             active ? 'A' : 'B');
    return active ? rbamp_std_ota_write_prepared(dev, bank_a, size_a, cb, ctx)
                  : rbamp_std_ota_write_prepared(dev, bank_b, size_b, cb, ctx);
}

esp_err_t rbamp_std_ota_activate(rbamp_handle_t dev)
{
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    uint8_t st = 0;
    err = _std_read_u8(dev, RBAMP_STD_REG_OTA_STATUS, &st);
    if (err != ESP_OK) return err;
    if (st != RBAMP_STD_OTA_OK) {
        ESP_LOGW(TAG, "ota activate: nothing armed (status=%u) — commit first", st);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t staged = 0;
    if (dev->std_version >= 0x35
            && _std_read_u32(dev, RBAMP_STD_REG_FW_STAGED_VERSION, &staged) != ESP_OK) staged = 0;
    if (staged == 0xFFFFFFFFu) staged = 0;
    const uint32_t before = dev->std_active_bank;
    err = _std_wait_idle(dev, 1000, RBAMP_STD_CMD_OTA_ACTIVATE);
    if (err != ESP_OK) return err;
    const int64_t t_act_us = esp_timer_get_time();
    err = _std_cmd(dev, RBAMP_STD_CMD_OTA_ACTIVATE);
    if (err != ESP_OK) return err;
    /* The module resets into the armed bank and re-enumerates on the bus
     * after REBOOT_MS; then rediscover it (new firmware may report a newer
     * map) and confirm the running bank changed. */
    _sleep_ms(RBAMP_STD_REBOOT_MS_OTA_ACTIVATE);
    bool is_std = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        err = _std_detect(dev, &is_std);
        if (err == ESP_OK && is_std) break;
        _sleep_ms(500);
    }
    if (err != ESP_OK) return err;
    if (!is_std) return ESP_ERR_INVALID_RESPONSE;
    err = _std_begin(dev);
    if (err != ESP_OK) return err;
    if (dev->std_active_bank == before) {
        ESP_LOGE(TAG, "ota activate: still running bank 0x%08" PRIX32 " (bootloader rollback?)", before);
        return ESP_ERR_INVALID_STATE;
    }
    /* Trial window: the new image must run ~10 s undisturbed before the
     * bootloader confirms it — a reset inside the window rolls back. Hold the
     * caller here (nothing on this handle can reset the module meanwhile),
     * then verify: the running image is the one that was staged and the swap
     * is no longer pending. */
    while (esp_timer_get_time() - t_act_us < (int64_t)RBAMP_STD_OTA_CONFIRM_MS * 1000) _sleep_ms(100);
    if (staged && dev->std_fw_ver && dev->std_fw_ver != staged) {
        ESP_LOGE(TAG, "ota activate: running image 0x%08" PRIX32 " is not the staged 0x%08" PRIX32 " (rolled back?)",
                 dev->std_fw_ver, staged);
        return ESP_ERR_INVALID_STATE;
    }
    if (dev->std_version >= 0x35) {
        uint32_t pending = 0;
        if (_std_read_u32(dev, RBAMP_STD_REG_SWAP_PENDING, &pending) == ESP_OK
                && pending != 0 && pending != 0xFFFFFFFFu) {
            ESP_LOGE(TAG, "ota activate: swap still pending (0x%08" PRIX32 ") after the trial window", pending);
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGI(TAG, "ota activate: running bank 0x%08" PRIX32 " image 0x%08" PRIX32 " map=0x%02X — confirmed",
             dev->std_active_bank, dev->std_fw_ver, dev->std_version);
    return ESP_OK;
}

uint32_t rbamp_std_crc32(const uint8_t *data, size_t len)
{
    /* Reflected CRC-32 with the map's parameters (zlib-compatible). */
    uint32_t crc = RBAMP_STD_OTA_CRC_INIT;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (RBAMP_STD_OTA_CRC_POLY & (0u - (crc & 1u)));
        }
    }
    return crc ^ RBAMP_STD_OTA_CRC_XOROUT;
}

#if defined(RBAMP_STD_OTA_IMG_MAGIC) && defined(RBAMP_STD_OTA_HEADER_SIZE)

void rbamp_std_ota_make_header(uint8_t out[RBAMP_STD_OTA_HEADER_SIZE], uint32_t version,
                               uint32_t len, uint32_t crc32)
{
    const uint32_t w[4] = { RBAMP_STD_OTA_IMG_MAGIC, version, len, crc32 };
    for (int i = 0; i < 4; ++i) {
        out[4 * i + 0] = (uint8_t)w[i];
        out[4 * i + 1] = (uint8_t)(w[i] >> 8);
        out[4 * i + 2] = (uint8_t)(w[i] >> 16);
        out[4 * i + 3] = (uint8_t)(w[i] >> 24);
    }
}

esp_err_t rbamp_std_ota_write_image(rbamp_handle_t dev, const uint8_t *image, size_t len,
                                    uint32_t version, rbamp_std_ota_progress_cb_t cb, void *ctx)
{
    /* Host-built (unsigned) images cannot pass COMMIT on the current module
     * firmware — every release image is vendor-prepared and signed. Kept for
     * ABI; refused before anything touches the module (audit 2026-10-11, E6). */
    (void)image; (void)len; (void)version; (void)cb; (void)ctx;
    esp_err_t err = _std_ota_gate(dev);
    if (err != ESP_OK) return err;
    ESP_LOGE(TAG, "ota: host-built images are not accepted by the module — use rbamp_std_ota_write_prepared");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif /* RBAMP_STD_OTA_IMG_MAGIC && RBAMP_STD_OTA_HEADER_SIZE */

void rbamp_reset_counters(rbamp_handle_t dev)
{
    if (!dev) return;
    dev->retry_exhaustion_count = 0;
    dev->sanity_reject_count = 0;
}
