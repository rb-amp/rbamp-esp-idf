# 02 · Module Tiers

## What a tier is

**rbAmp** ships in three tiers: **BASIC**, **STANDARD**, **PRO**.
A tier is a complete bundle of **hardware revision and firmware**, not
a software flag. Moving between tiers requires a physical change of SKU,
not a firmware update.

From the perspective of the component and your application code: **the
`rbamp_*` API is identical across all tiers**. The differences show up
in which values the module returns and how the firmware's behavior
interprets the data (in particular, export to the grid).

## Current firmware state (v1.2)

Firmware v1.2 implements and ships **only the BASIC** tier. STANDARD and
PRO are on the roadmap and are not implemented in the current firmware.

| Tier | Firmware | Shipping |
|---|---|---|
| **BASIC** | v1.2 | ✅ yes |
| **STANDARD** | planned for v1.3+ | ❌ no |
| **PRO** | planned for v1.3+ | ❌ no |

## BASIC — the entry-level consumer-metering tier

### Hardware

A cost-optimized analog signal path, suitable for typical residential
loads. The module includes an isolated analog front-end, an on-board
power regulator, and factory calibration stored in flash memory.

### Firmware behavior

The logic of a classic mechanical disc meter — **the count only moves
forward; export to the grid is not subtracted**.

The three "layers" of active-power values behave differently:

| Signal | Meaning | Behavior on BASIC |
|---|---|---|
| `rbamp_read_power(dev, ch, &p)` | instantaneous RT power, updated ~200 ms | **signed** — a negative value is visible in real time (export) |
| `snap.avg_p[ch]` (inside `rbamp_read_period_snapshot`) | average power over the period | each 200 ms average-P window is **clamped** to `max(P, 0)` before being added to the period accumulator |
| `rbamp_energy_wh(dev, ch)` | Wh accumulated by the component | **monotonic** — consumption only; export is not counted |

In other words: the user **sees** the export in the RT reading, but it
is not present in the component's Wh counter on BASIC.

### Typical applications

- Households without their own generation (no solar panels, wind
  turbines, or battery systems)
- Submeters for purely consuming loads (water heaters, motors,
  household appliances)
- Building consumption monitoring where bidirectional metering is not
  required

### Bidirectional metering on BASIC (master-side)

If you need to separately track export to the grid on a BASIC module,
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
        /* Sample the RT power at 5 Hz — matches the firmware commit
         * cadence (200 ms per channel). Faster gives empty repeat
         * reads, slower loses resolution on sharp load transitions.
         * ±2 % accuracy is achievable for typical mixed loads with an
         * inverter. */
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

A full working example of this pattern is in [06_examples.md](06_examples.md),
the "Bidirectional metering on the master side" scenario.

## STANDARD — the bidirectional tier *(planned, v1.3+)*

> This section describes **planned** functionality. It is not
> implemented in the current v1.2 firmware.

### What will be added

- **Hardware**: an extended analog stack for accurate measurement of
  both consumption and reverse flow
- **Firmware**: bidirectional metering — two separate per-period
  accumulators (consumption and export), exposed separately through
  additional registers (details in the specification after the v1.3+
  release)
- **Component API**: `rbamp_energy_wh(dev, ch)` will start returning a
  signed net value (consumption − export) automatically, without
  master-side tricks

### Typical applications (when it ships)

- Homes with rooftop solar generation
- Homes with wind turbines
- Storage systems (batteries)
- Regenerative loads
- V2G (vehicle-to-grid) EV charging

## PRO — the premium tier *(planned, v1.3+)*

> This section describes **planned** functionality. It is not
> implemented in the current v1.2 firmware.

### What PRO will add

- **Hardware**: a PRO-grade analog front-end (lower noise, tighter
  linearity), premium factory calibration, optional extended channel
  sets
- **Firmware**: bidirectional metering (like STANDARD) plus additional
  diagnostic features — details in the specification after the v1.3+
  release
- **Component API**: extended diagnostic accessors (the exact set will
  follow once it is implemented in firmware)

### PRO applications (when it ships)

- Submeters for commercial tenants
- Billing-grade-accuracy installations
- Test-and-measurement laboratories
- Energy-intensive industrial loads

## How to determine the tier at runtime

In firmware v1.2 there is no explicit "tier" register. Indirect
indicators:

```c
/* Firmware version — on v1.2 == 0x03 */
uint8_t fw = rbamp_firmware_version(dev);

/* Presence of a voltage sensor (UI* variants vs. I*-only) */
bool voltage_hw = rbamp_has_voltage_hw(dev);

/* Number of current channels (1 / 2 / 3) */
uint8_t channels = rbamp_channels(dev);
```

> **Note (v1.2)**: the indirect indicators above give different signals
> (firmware version, channel count, presence of a voltage sensor), but
> they do **NOT** give the tier — on v1.2 firmware the tier is
> implicitly always **BASIC**. Use the SKU label on the module's
> enclosure as the source of truth. An explicit tier register will be
> added in firmware v1.3+ when STANDARD starts shipping.

## Where tier-dependent items are flagged in the docs

In the component text and the canonical documentation, tier-dependent
features are flagged explicitly, for example:

> **STANDARD / PRO only** — this register is not available on a BASIC
> module.

Or, conversely, BASIC-specific behavior:

> **BASIC**: the `rbamp_energy_wh(dev, ch)` counter is monotonic —
> export to the grid is not counted. For bidirectional metering, see the
> "Bidirectional metering on the master side" scenario in
> [06_examples.md](06_examples.md), or use a STANDARD/PRO module.

## What's next

- [03 · Current Sensor Selection](03_sensor_selection.md) — which
  SCT-013 for which task
- [04 · Hardware Connection](04_hardware.md) — hardware nuances
- [06 · Examples](06_examples.md) — scenarios for BASIC, including
  master-side bidirectional metering
- [09 · API Reference](09_api_reference.md) — the full public API


---

[← Overview](01_overview.md) | [Contents](README.md) | [Sensor Selection →](03_sensor_selection.md)
