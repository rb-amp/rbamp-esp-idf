/**
 * @file    rbamp_fleet.h
 * @brief   Multi-module bus / fleet manager for rbAmp (v1.3).
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * A @c rbamp_fleet_t owns a collection of ::rbamp_handle_t devices sharing one
 * I²C bus and adds the multi-module concerns that don't belong on a single
 * device handle:
 *
 *  - **Discovery** — scan @c 0x08..0x77, probe each responder, bind rbAmp
 *    modules (::rbamp_fleet_scan).
 *  - **Address assignment** — give virgin modules (factory @c 0x50) distinct
 *    addresses via the v1.3 two-phase commit, with on-bus conflict checks
 *    (::rbamp_fleet_assign_address).
 *  - **GC fleet-sync** — opt every module into General-Call latch, broadcast a
 *    ticked latch, and detect modules that missed a window via @c REG_GC_TICK
 *    (::rbamp_fleet_enable_gc_all, ::rbamp_fleet_gclatch,
 *    ::rbamp_fleet_check_sync).
 *  - **Aggregation / monitoring** — sum power / energy across modules+channels
 *    and poll per-module EVENT bit3 (::rbamp_fleet_total_power,
 *    ::rbamp_fleet_total_energy_wh, ::rbamp_fleet_poll_errors).
 *
 * The fleet does NOT take ownership of device lifetime by default for handles
 * added via ::rbamp_fleet_add; handles created by ::rbamp_fleet_scan /
 * ::rbamp_fleet_assign_address are fleet-owned and freed by
 * ::rbamp_fleet_destroy.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#include "rbamp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum modules a single fleet tracks (7-bit address space is sparse). */
#ifndef RBAMP_FLEET_MAX_MODULES
#  define RBAMP_FLEET_MAX_MODULES 16
#endif

/** @brief Opaque fleet handle. Created by ::rbamp_fleet_create. */
typedef struct rbamp_fleet_obj_t *rbamp_fleet_t;

/**
 * @brief Per-module sync status after a ::rbamp_fleet_check_sync call.
 */
typedef struct {
    uint8_t  addr;        /**< Module I²C address. */
    uint16_t gc_tick;     /**< REG_GC_TICK value (0xFFFF = never received a GC frame). */
    bool     in_sync;     /**< true if gc_tick == the expected tick. */
    bool     reachable;   /**< false if the read NACKed / timed out. */
} rbamp_fleet_sync_t;

/**
 * @brief Per-module read status from a ::rbamp_fleet_poll_all call.
 *
 * A single module NACKing must NOT abort the whole poll — that module is
 * marked @c ok=false (MISS) and the loop continues. The matching
 * @c rbamp_snapshot_t carries the module's own channel count (variant-aware).
 */
typedef struct {
    uint8_t addr;     /**< Module I²C address. */
    bool    ok;       /**< true if the snapshot read fully succeeded this cycle. */
    uint8_t channels; /**< Channel count for this module (mirror of snapshot.channels). */
} rbamp_fleet_poll_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an empty fleet bound to an I²C master bus.
 *
 * @param[in]  bus I²C master bus from @c i2c_new_master_bus.
 * @param[out] out Receives the new fleet handle.
 * @return @c ESP_OK, @c ESP_ERR_INVALID_ARG, or @c ESP_ERR_NO_MEM.
 */
esp_err_t rbamp_fleet_create(i2c_master_bus_handle_t bus, rbamp_fleet_t *out);

/**
 * @brief Destroy a fleet and free all fleet-owned device handles.
 *
 * Handles added via ::rbamp_fleet_add (caller-owned) are detached but NOT
 * freed; handles produced by ::rbamp_fleet_scan / ::rbamp_fleet_assign_address
 * are freed. Passing NULL is a no-op.
 */
void rbamp_fleet_destroy(rbamp_fleet_t fleet);

/* -------------------------------------------------------------------------
 * Membership
 * ------------------------------------------------------------------------- */

/**
 * @brief Scan @c 0x08..0x77, probe responders, bind rbAmp modules.
 *
 * For each address that ACKs, a handle is created and ::rbamp_begin run; only
 * modules that pass the version probe (and report @c PRODUCT_ID == rbAmp, when
 * @p match_product is true) are added. Already-present addresses are skipped.
 *
 * @par Conflict handling (detect-and-exclude, two-tier)
 * Each responder is run through the best-effort collision probe
 * (::rbamp_fleet_check_conflict semantics):
 *  - **Tier 1 — bus healthy:** a suspected duplicate-address conflict is NOT
 *    resolved; the address is EXCLUDED from the fleet (never polled) and
 *    recorded — see ::rbamp_fleet_excluded_count / ::rbamp_fleet_excluded_addr.
 *    Healthy modules are still added; the scan returns @c ESP_OK.
 *  - **Tier 2 — bus compromised:** if the conflict has wedged the bus (a
 *    known-empty address now ACKs — SDA stuck), the ENTIRE init is aborted:
 *    every handle is freed, the fleet is left EMPTY, and the call returns
 *    @c ESP_ERR_INVALID_STATE. Safer to init nothing than a fleet on a broken
 *    bus — the caller must fix the wiring.
 * Detection is best-effort: two IDLE identical modules are near-undetectable
 * (see ::rbamp_fleet_check_conflict @warning). The real defense is the
 * one-virgin-at-a-time provisioning discipline.
 *
 * @param[in]  fleet         Fleet handle.
 * @param[in]  match_product If true, require @c REG_PRODUCT_ID == 0x01 (rbAmp
 *                           sensor) — skips rbDimmer / foreign I²C devices.
 * @param[out] added         Optional — receives the count of NEW modules added.
 * @return @c ESP_OK (even if 0 added); @c ESP_ERR_INVALID_STATE if the bus is
 *         compromised (fleet left empty); @c ESP_ERR_INVALID_ARG on null fleet.
 */
esp_err_t rbamp_fleet_scan(rbamp_fleet_t fleet, bool match_product, size_t *added);

/**
 * @brief Number of addresses excluded as suspected duplicate-address conflicts.
 *
 * Populated by ::rbamp_fleet_scan's tier-1 detect-and-exclude. These addresses
 * are NOT in the fleet and are never polled — surface them to the operator
 * ("provision one module at a time / fix wiring").
 */
size_t rbamp_fleet_excluded_count(rbamp_fleet_t fleet);

/** @brief The @p idx-th excluded address, or @c -1 if out of range. */
int rbamp_fleet_excluded_addr(rbamp_fleet_t fleet, size_t idx);

/**
 * @brief Adopt a caller-owned device handle into the fleet (not freed on destroy).
 * @return @c ESP_ERR_NO_MEM if the fleet is full, @c ESP_ERR_INVALID_STATE if
 *         the address is already tracked.
 */
esp_err_t rbamp_fleet_add(rbamp_fleet_t fleet, rbamp_handle_t dev);

/** @brief Number of modules currently in the fleet. */
size_t rbamp_fleet_count(rbamp_fleet_t fleet);

/** @brief Get the module handle at index @p idx (0..count-1), or NULL. */
rbamp_handle_t rbamp_fleet_get(rbamp_fleet_t fleet, size_t idx);

/** @brief Find a module by I²C address, or NULL if not tracked. */
rbamp_handle_t rbamp_fleet_find(rbamp_fleet_t fleet, uint8_t addr);

/**
 * @brief Read every module in one call — the fleet analog of ::rbamp_read_all.
 *
 * Iterates the fleet, filling @p snapshots[i] / @p status[i] for module i. A
 * module that NACKs this cycle is marked @c status[i].ok = false and the poll
 * continues (transient single-module read-fails recover next cycle). Each
 * snapshot is variant-aware (its own channel count).
 *
 * @warning Wedged-bus hang on a MARGINAL I²C bus — a real field risk, mitigate
 *       it. On a bus with weak pull-ups (the ESP32 internal ~45 kΩ alone is too
 *       weak — slow SDA/SCL rise), long traces, or EMI under load, the bus can
 *       wedge mid-transaction; the IDF i2c_master driver then spins on
 *       @c i2c_ll_is_bus_busy BEFORE arming the transfer, and the
 *       @c xfer_timeout_ms does NOT bound that pre-send wait, so the call (and
 *       this function) blocks indefinitely. The library cannot interrupt a
 *       blocking same-task IDF call, so its internal retry / bus-reset never
 *       runs (a full-hour soak on internal-pull-up wiring hit it ~every 3 min).
 *       Three-layer mitigation, all recommended for production:
 *         1. Proper bus pull-ups — external ~4.7 kΩ on SDA/SCL (do NOT rely on
 *            the internal pull-ups for a multi-module / long-trace bus).
 *         2. No debugger attached in production (a probe NRST-glitch that resets
 *            a module mid-I²C holds SDA and raises the hang rate).
 *         3. An APP-LEVEL task-WDT on the polling task — it auto-reboots cleanly
 *            out of a wedge (the only in-software recovery, since the library
 *            cannot break the blocking IDF call):
 * @code
 * esp_task_wdt_reconfigure(&(esp_task_wdt_config_t){
 *     .timeout_ms = 8000, .trigger_panic = true });
 * esp_task_wdt_add(NULL);                 // this (polling) task
 * for (;;) {
 *     rbamp_fleet_poll_all(fleet, snaps, st, cap, &n_ok);
 *     esp_task_wdt_reset();               // fed only if poll_all returned
 *     vTaskDelay(...);
 * }
 * @endcode
 *
 * @param[in]  fleet     Fleet handle.
 * @param[out] snapshots Caller array of ≥ ::rbamp_fleet_count entries.
 * @param[out] status    Caller array of ≥ ::rbamp_fleet_count entries.
 * @param[in]  cap       Capacity of both arrays.
 * @param[out] n_ok      Optional — count of modules that read OK this cycle.
 * @return @c ESP_OK; @c ESP_ERR_INVALID_SIZE if @p cap < module count.
 */
esp_err_t rbamp_fleet_poll_all(rbamp_fleet_t fleet,
                               rbamp_snapshot_t *snapshots,
                               rbamp_fleet_poll_t *status,
                               size_t cap, size_t *n_ok);

/* -------------------------------------------------------------------------
 * Standalone provisioning (no fleet needed)
 * ------------------------------------------------------------------------- */

/**
 * @brief Provision ONE virgin module (factory @c 0x50) to @p desired_addr.
 *
 * @warning HARD PRECONDITION: connect EXACTLY ONE virgin module to the bus
 *          before calling this. Two idle identical modules sharing @c 0x50 are
 *          a clean, stable open-drain wired-AND and are near-undetectable by
 *          register reads (verified on the bench — VERSION, UID, and live-value
 *          probes are all blind at the zero-load provisioning condition). The
 *          best-effort collision check below catches the loaded / live-signal-
 *          differing case but CANNOT be relied on for idle identical modules.
 *          The one-virgin-at-a-time discipline — not the detection — is the
 *          real defense against silent multi-module corruption.
 *
 * The safe single-call flow:
 *  1. Require something answering at @c 0x50; run the best-effort collision
 *     check (see @warning) and refuse a detected collision.
 *  2. Refuse if @p desired_addr already ACKs on the bus.
 *  3. Two-phase address commit (0x30 + magic 0xA5 + @c CMD_COMMIT_ADDR) +
 *     reset; verify the module now answers at @p desired_addr via probe
 *     (boot-window retry; NOT a @c REG_I2C_ADDRESS read).
 *  4. Positive confirmation: @c 0x50 must now be EMPTY (the module vacated it).
 *     If it still answers, abort with @c ESP_ERR_INVALID_STATE — a partial
 *     move or a second module that stayed put.
 *  5. A fresh module boots @c ERR_FLASH_PARAMS_BAD; if @p save_config is true,
 *     issue @c CMD_SAVE_USER_CONFIG to repair (0xFB→0x00) + persist.
 *  6. Return a handle bound to @p desired_addr (caller owns it —
 *     ::rbamp_del when done, or ::rbamp_fleet_add to track it).
 *
 * @param[in]  bus          I²C master bus.
 * @param[in]  desired_addr Target 7-bit address (0x08..0x77, != 0x50).
 * @param[in]  save_config  If true, SAVE_USER_CONFIG after the move.
 * @param[out] out_dev      Receives the handle bound to @p desired_addr.
 *
 * @return
 *   - @c ESP_OK on success
 *   - @c ESP_ERR_NOT_FOUND if nothing answers at @c 0x50
 *   - @c ESP_ERR_INVALID_STATE on suspected collision at 0x50, @p desired_addr
 *     already in use, or @c 0x50 still answering after the move
 *   - @c ESP_ERR_INVALID_RESPONSE if the module never ACKs at @p desired_addr
 *   - @c ESP_ERR_INVALID_ARG on bad @p desired_addr
 *   - transport errors otherwise
 */
esp_err_t rbamp_provision(i2c_master_bus_handle_t bus, uint8_t desired_addr,
                          bool save_config, rbamp_handle_t *out_dev);

/* -------------------------------------------------------------------------
 * Address assignment (virgin module provisioning)
 * ------------------------------------------------------------------------- */

/**
 * @brief Two-phase reassign @p dev to @p new_addr with on-bus conflict checks.
 *
 * Refuses if the SOURCE address is multiply-occupied (best-effort collision
 * check) or if @p new_addr already ACKs. Runs ::rbamp_prepare_address_change +
 * ::rbamp_commit_address_change (production-OK two-phase commit on v1.3), then
 * boot-window-probes the new address AND confirms the old address vacated.
 *
 * @note Field workflow for virgin modules: connect ONE new module (factory
 *       0x50) at a time, assign it a distinct address, then add the next (see
 *       ::rbamp_provision's hard one-virgin precondition).
 *
 * @return @c ESP_OK; @c ESP_ERR_INVALID_STATE on a detected collision, target
 *         in use, or the old address still answering after the move;
 *         @c ESP_ERR_INVALID_RESPONSE if the new address never ACKs; transport
 *         errors otherwise.
 */
esp_err_t rbamp_fleet_assign_address(rbamp_fleet_t fleet, rbamp_handle_t dev,
                                     uint8_t new_addr);

/**
 * @brief Detect an address collision (≥2 modules answering on one address).
 *
 * Probes @p addr and, when it ACKs, samples a LIVE per-module value
 * (@c REG_V03_I0_RMS) several times: two modules return the wired-AND of two
 * drifting floats, which is frequently non-finite / out-of-range and/or
 * chaotically varying, whereas one module returns clean stable reads.
 *
 * @warning BEST-EFFORT ONLY. Two IDLE identical modules at one address are a
 *          stable open-drain wired-AND with no live divergence — this returns
 *          @c collision = false for that case (verified on the bench). It
 *          reliably flags only modules with a differing live signal (under
 *          load). Read-only / non-destructive: safe to call before
 *          ::rbamp_provision. Do NOT use it as the sole guard against
 *          multi-module corruption — rely on one-virgin-at-a-time provisioning.
 *
 * @param[out] collision Receives true if a collision is suspected.
 */
esp_err_t rbamp_fleet_check_conflict(rbamp_fleet_t fleet, uint8_t addr,
                                     bool *collision);

/* -------------------------------------------------------------------------
 * GC fleet-sync
 * ------------------------------------------------------------------------- */

/**
 * @brief Opt every module in the fleet into General-Call latch reception.
 *
 * Calls ::rbamp_enable_gc on each (FLEET_CONFIG.bit0 + SAVE_USER_CONFIG +
 * reset). Blocks ~1 s per module. Optionally assigns @p group as the GROUP_ID
 * for all (pass 0 for all-call).
 *
 * @param[out] ok_count Optional — number of modules successfully enabled.
 */
esp_err_t rbamp_fleet_enable_gc_all(rbamp_fleet_t fleet, uint8_t group,
                                    size_t *ok_count);

/**
 * @brief Broadcast a ticked GC latch to the fleet, then settle.
 *
 * Thin wrapper over ::rbamp_broadcast_latch_group on the fleet's bus. After
 * the settle, read each module's period snapshot with @c skip_latch=true, or
 * call ::rbamp_fleet_check_sync to verify which modules accepted @p tick.
 */
esp_err_t rbamp_fleet_gclatch(rbamp_fleet_t fleet, uint8_t group, uint16_t tick,
                              uint32_t settle_ms);

/**
 * @brief Read each module's @c REG_GC_TICK and compare against @p expected_tick.
 *
 * @param[in]  fleet         Fleet handle.
 * @param[in]  expected_tick The tick last broadcast via ::rbamp_fleet_gclatch.
 * @param[out] status        Caller array of at least ::rbamp_fleet_count entries.
 * @param[in]  status_cap    Capacity of @p status.
 * @param[out] n_missed      Optional — count of reachable modules NOT in sync.
 * @return @c ESP_OK; @c ESP_ERR_INVALID_SIZE if @p status_cap < module count.
 */
esp_err_t rbamp_fleet_check_sync(rbamp_fleet_t fleet, uint16_t expected_tick,
                                 rbamp_fleet_sync_t *status, size_t status_cap,
                                 size_t *n_missed);

/* -------------------------------------------------------------------------
 * Aggregation / monitoring
 * ------------------------------------------------------------------------- */

/**
 * @brief Sum real power over all modules and all their channels (W).
 *
 * Reads RT power per channel from each module; modules that NACK are skipped
 * (their contribution is 0). I-variants contribute 0 power (no voltage).
 */
esp_err_t rbamp_fleet_total_power(rbamp_fleet_t fleet, float *out_w);

/** @brief Sum the Wh accumulator over all modules and channels. */
esp_err_t rbamp_fleet_total_energy_wh(rbamp_fleet_t fleet, double *out_wh);

/**
 * @brief Poll EVENT bit3 (durable error) on every module.
 *
 * @param[out] error_mask Optional bitmask — bit i set if module index i has an
 *                        error (only meaningful for the first 32 modules).
 * @param[out] n_errors   Optional — count of modules currently flagging an error.
 */
esp_err_t rbamp_fleet_poll_errors(rbamp_fleet_t fleet, uint32_t *error_mask,
                                  size_t *n_errors);

#ifdef __cplusplus
}
#endif
