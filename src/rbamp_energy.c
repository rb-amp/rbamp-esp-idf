/**
 * @file    rbamp_energy.c
 * @brief   Implementation of the Wh accumulator. See rbamp_energy.h.
 *
 * @see SPEC §7 — Period-metering state machine.
 */
#include "rbamp_energy.h"

void rbamp_energy_init(rbamp_energy_t *e)
{
    e->wh[0] = 0.0;
    e->wh[1] = 0.0;
    e->wh[2] = 0.0;
    e->enabled = true;
}

void rbamp_energy_tick(rbamp_energy_t *e,
                       const float avg_p[3],
                       uint8_t channels,
                       uint32_t period_ms,
                       bool valid)
{
    if (!e->enabled || !valid || period_ms == 0) {
        return;
    }
    const double dt_s = (double)period_ms / 1000.0;
    const uint8_t n = (channels > 3) ? 3 : channels;
    for (uint8_t ch = 0; ch < n; ++ch) {
        /* E_Wh += avg_p_W * dt_s / 3600 — see SPEC §7. */
        e->wh[ch] += (double)avg_p[ch] * dt_s / 3600.0;
    }
}

double rbamp_energy_wh_get(const rbamp_energy_t *e, uint8_t ch)
{
    if (ch > 2) {
        return 0.0;
    }
    return e->wh[ch];
}

void rbamp_energy_reset_ch(rbamp_energy_t *e, uint8_t ch)
{
    if (ch <= 2) {
        e->wh[ch] = 0.0;
    }
}

void rbamp_energy_reset_all_ch(rbamp_energy_t *e)
{
    e->wh[0] = e->wh[1] = e->wh[2] = 0.0;
}
