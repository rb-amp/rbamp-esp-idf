# 02 · Module Tiers

## What a tier is

**rbAmp** ships in three tiers: **BASIC**, **STANDARD**, **PRO**.
A tier is a complete bundle of **hardware revision and firmware**, not a
software flag. Moving between tiers requires a physical SKU change,
not a firmware update.

From the component and user-code perspective: **the `rbamp_*` API is
identical across all tiers**. The differences show up in what values
the module returns and how the firmware behavior interprets the data
(in particular — export to the grid).

## Current status

In the current rbAmp firmware, **only the BASIC** tier is implemented and shipping.
STANDARD and PRO are on the roadmap, not implemented.

| Tier | Shipping |
|---|---|
| **BASIC** | ✅ yes |
| **STANDARD** | ❌ planned |
| **PRO** | ❌ planned |

## BASIC — entry-level consumption-metering tier

### Hardware

A cost-optimized analog path suited to typical household loads.
The module includes an isolated analog front-end, an on-board power
regulator, and factory calibration stored in flash.

### Firmware behavior

The logic of a classic mechanical disc meter — **counting only goes
forward, export to the grid is not subtracted**.

The three "layers" of active-power values behave differently:

| Signal | Meaning | Behavior on BASIC |
|---|---|---|
| `rbamp_read_power(dev, ch, &p)` | instantaneous RT power, updated every ~200 ms | **signed** — a negative value is visible in real time (export) |
| `snap.avg_p[ch]` (inside `rbamp_read_period_snapshot`) | average power over the period | each 200 ms average-P window is **clamped** to `max(P, 0)` before being added to the period accumulator |
| `rbamp_energy_wh(dev, ch)` | Wh accumulated by the component | **monotonic** — consumption only, export is not counted |

In other words: the user **sees** export in the RT reading, but the
component's Wh counter on BASIC does not include it.

### Typical applications

- Households without their own generation (no solar panels,
  wind turbines, battery systems)
- Submeters for purely consuming loads (water heaters, motors,
  household appliances)
- Building consumption monitoring where bidirectional metering is
  not required

### Bidirectional metering on BASIC (master-side)

If you need to track export to the grid separately on a BASIC module,
**do it on the master side**. The RT power
`rbamp_read_power(dev, 0, &p)` is signed, so:

```c
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static double consume_wh = 0.0;
static double export_wh  = 0.0;

void bidirectional_loop(rbamp_handle_t dev) {
    int64_t t_prev_us = esp_timer_get_time();

    while (1) {
        /* Sample RT power at 5 Hz — matches the firmware-commit
         * cadence (200 ms per channel). Faster yields empty
         * repeated reads, slower loses resolution on sharp load
         * transitions. ±2 % accuracy is achievable for typical
         * mixed loads with an inverter. */
        vTaskDelay(pdMS_TO_TICKS(200));

        int64_t t_now_us = esp_timer_get_time();
        double  dt_s     = (double)(t_now_us - t_prev_us) / 1e6;
        t_prev_us = t_now_us;

        float p;
        if (rbamp_read_power(dev, 0, &p) != ESP_OK) continue;

        double dwh = (double)p * dt_s / 3600.0;
        if (p >= 0.0f) consume_wh += dwh;
        else           export_wh  += -dwh;
    }
}
```

A complete working example of the pattern is in [06_examples.md](06_examples.md),
the "Master-side bidirectional metering" scenario.

## STANDARD — bidirectional tier *(planned)*

> This section describes **planned** functionality. It is not
> implemented in the current rbAmp firmware.

### What will be added

- **Hardware**: an extended analog stack for accurate measurement of
  both consumption and reverse flow
- **Firmware**: bidirectional metering — two separate period
  accumulators (consumption and export), exposed separately through
  additional registers (details after the STANDARD-tier release)
- **Component API**: `rbamp_energy_wh(dev, ch)` will begin returning a
  signed net value (consumption − export) automatically, without
  master-side tricks

### Typical applications (once available)

- Homes with rooftop solar generation
- Homes with wind turbines
- Storage systems (batteries)
- Regenerative loads
- V2G (vehicle-to-grid) electric-vehicle charging

## PRO — premium tier *(planned)*

> This section describes **planned** functionality. It is not
> implemented in the current rbAmp firmware.

### What PRO will add

- **Hardware**: a PRO-grade analog front-end (lower noise, tighter
  linearity), premium factory calibration, optional extended channel
  sets
- **Firmware**: bidirectional metering (as in STANDARD) plus
  additional diagnostic features — details after the PRO-tier release
- **Component API**: extended diagnostic accessors (exact set — after
  the firmware implementation)

### PRO applications (once available)

- Submeters for commercial tenants
- Billing-grade-accuracy installations
- Test-and-measurement labs
- Energy-intensive industrial loads

## How to detect the tier at runtime

The current firmware has no explicit "tier" register. Indirect
indicators are available:

```c
/* Firmware version — opaque byte */
uint8_t fw = rbamp_firmware_version(dev);

/* Voltage-sensing presence (UI* variants vs I*-only) */
bool voltage_hw = rbamp_has_voltage_hw(dev);

/* Number of current channels (1 / 2 / 3) */
uint8_t channels = rbamp_channels(dev);

/* Full SKU — variant byte (1=UI1, 2=UI2, 3=UI3, 4=I1, 5=I2, 6=I3) */
uint8_t variant = rbamp_hw_variant(dev);
```

> **Note**: the indirect indicators above give different signals
> (firmware version, channel count, voltage-sensor presence, SKU),
> but they do **NOT** give the tier — the current firmware is always
> implicitly **BASIC**. Use the SKU label on the module enclosure as
> the source of truth for the tier. An explicit tier register will
> appear once STANDARD starts shipping.

## Where tier-dependent details are marked in the docs

In the component text and the canonical documentation, tier-dependent
features are marked explicitly, for example:

> **STANDARD / PRO only** — this register is not available on the BASIC module.

Or, conversely, BASIC-specific behavior:

> **BASIC**: the `rbamp_energy_wh(dev, ch)` counter is monotonic —
> export to the grid is not counted. For bidirectional metering, see
> the "Master-side bidirectional metering" scenario in
> [06_examples.md](06_examples.md), or use a STANDARD/PRO
> module.

## What's next

- [03 · Current-sensor selection](03_sensor_selection.md) — which SCT-013
  for which job
- [04 · Wiring](04_hardware.md) — hardware nuances
- [06 · Examples](06_examples.md) — scenarios for BASIC, including
  master-side bidirectional metering
- [09 · API reference](09_api_reference.md) — the full public API

