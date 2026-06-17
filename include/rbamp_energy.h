/**
 * @file    rbamp_energy.h
 * @brief   Internal Wh accumulator API for the rbAmp ESP-IDF component.
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * Not part of the public API — users access energy via
 * ::rbamp_energy_wh / ::rbamp_energy_reset etc. in rbamp.h. This header
 * exists so rbamp.c can forward-declare the accumulator struct without
 * exposing its layout to the application.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Internal Wh accumulator (one per ::rbamp_handle_t). */
typedef struct {
    double  wh[3];   /**< Per-channel running Wh total (signed). */
    bool    enabled; /**< Master switch — false means tick() is a no-op. */
} rbamp_energy_t;

/** @brief Initialise the accumulator (zero counters, enabled by default). */
void rbamp_energy_init(rbamp_energy_t *e);

/**
 * @brief Integrate one period snapshot.
 *
 * Skipped silently if @c valid == false, @c period_ms == 0, or the
 * accumulator is disabled. Formula (SPEC §7):
 *
 *     E_Wh[ch] += avg_p_W * (period_ms / 1000) / 3600
 *
 * @param[in,out] e          Accumulator.
 * @param[in]     avg_p      Per-channel average power for the period (W).
 * @param[in]     channels   Number of valid channels in @c avg_p (1..3).
 * @param[in]     period_ms  Elapsed duration of the billing window (ms). The
 *                           caller passes the MASTER wall-clock dt computed
 *                           from @c esp_timer_get_time() across consecutive
 *                           consumed reads (see ::rbamp_read_period_snapshot,
 *                           field @c master_dt_ms) — NOT the chip's self-
 *                           reported period.
 *                           @note L9 / SPEC E.6 / F10: the device's own
 *                           REG_V03_PERIOD_LATCH_MS (0xEC) under-counts by
 *                           ~26% (HW-validated) due to timer-ISR starvation in
 *                           the module firmware, so it is DIAGNOSTIC-ONLY and
 *                           must never feed energy integration. A previous revision
 *                           wrongly used it (OI-3); fixed in 11b2a99. Future
 *                           porters: do NOT revert this to chip-period — the
 *                           accumulator is clock-agnostic and the master
 *                           wall-clock is the billing dt.
 * @param[in]     valid      REG_V03_PERIOD_VALID bit at read time.
 */
void rbamp_energy_tick(rbamp_energy_t *e,
                       const float avg_p[3],
                       uint8_t channels,
                       uint32_t period_ms,
                       bool valid);

/** @brief Read running total for one channel (0..2). 0 if out of range. */
double rbamp_energy_wh_get(const rbamp_energy_t *e, uint8_t ch);

/** @brief Zero one channel's running total. */
void rbamp_energy_reset_ch(rbamp_energy_t *e, uint8_t ch);

/** @brief Zero all channels. */
void rbamp_energy_reset_all_ch(rbamp_energy_t *e);

/** @brief Disable integration (tick() becomes a no-op; reads keep returning frozen value). */
static inline void rbamp_energy_set_enabled(rbamp_energy_t *e, bool en) { e->enabled = en; }

#ifdef __cplusplus
}
#endif
