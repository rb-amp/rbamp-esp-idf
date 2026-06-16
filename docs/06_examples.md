# 06 · Examples

This chapter opens with the **flagship scenarios** — the real
deployment patterns the library was designed for (mains + N
sub-loads on a single bus, GC-synchronized snapshots). They are
followed by single-module scenarios — from a minimal "hello world"
to a battery-powered deep-sleep logger.

Most scenarios ship as a ready-to-build IDF project in
[`examples/`](../examples/), which you can build with
`idf.py -C examples/<name> build flash monitor`. The code in this
chapter is a **distillation of the key logic**, not the full project
(`bus_cfg` / WiFi bring-up / Kconfig are omitted for brevity).

| # | Scenario | Difficulty | Source |
|:---:|---|:---:|---|
| **1** | **Mains + N sub-loads — the 80% canon** | medium | [`examples/fleet_sync/`](../examples/fleet_sync/) |
| **2** | **Provisioning workflow (virgin → fleet)** | low | [`examples/provision/`](../examples/provision/) |
| **3** | **Multi-channel mixed-CT (I3, different models)** | medium | [`examples/multi_channel/`](../examples/multi_channel/) |
| **4** | **Fleet GC sync — billing-grade synchronicity** | medium | [`examples/fleet_sync/`](../examples/fleet_sync/) |
| 5 | Quick read (single module) | minimal | [`examples/quick_read/`](../examples/quick_read/) |
| 6 | Wh meter on a 16×2 LCD | low | [`examples/lcd_period/`](../examples/lcd_period/) |
| 7 | Monitoring 3 modules (legacy sequential) | low | [`examples/multi_module/`](../examples/multi_module/) |
| 8 | MQTT per channel | medium | [`examples/mqtt_publisher/`](../examples/mqtt_publisher/) |
| 9 | Bidirectional metering on the master side | medium | (composition) |
| 10 | Whole-home energy balance (3 modules) | high | (composition) |
| 11 | Event-based logging (EMA) | medium | (composition) |
| 12 | Local CSV logger on SPIFFS | medium | [`examples/spiffs_logger/`](../examples/spiffs_logger/) |
| 13 | Battery logger with deep-sleep | medium | [`examples/deep_sleep_logger/`](../examples/deep_sleep_logger/) |
| 14 | I3 with different CT per channel (mixed-load deep-dive) | medium | (composition) |
| 15 | Sub-metering: 5×UI1 on a shared bus | high | (composition) |
| 16 | GC broadcast latch for >8 modules | high | (composition) |
| 17 | GC group filtering (multi-tenant billing) | high | (composition) |

> Scenarios 1–4 are the flagship ones, built around the library's canonical deployment. Their code uses the fleet API and is validated on bench hardware (a heterogeneous UI1+I2+I3 fleet). Scenarios 5–13 are single-module and compose patterns; 14–17 are extended fleet scenarios (a deep-dive into the capabilities introduced in 1–4). Scenario 7 is the legacy sequential per-module latch without the fleet API, kept for comparison and as a migration path.

> All scenarios assume the sensor class and CT model have already
> been configured via `rbamp_set_sensor_class()` + `rbamp_set_ct_model()`
> (see [05 · Quickstart](05_quickstart.md) Step 4). This is done once
> at installation time — the settings are stored in the module's flash
> and survive a reset.

### Extra `REQUIRES` for `main/CMakeLists.txt`

The base component pulls in `esp_driver_i2c`, `esp_timer`, `log`.
Some scenarios use additional ESP-IDF components — add them to the
`REQUIRES` of your `main/`:

| Scenario | Extra `REQUIRES` |
|---|---|
| 1 — Mains + N sub-loads (fleet) | — (`rbamp` only) |
| 2 — Provisioning workflow | — |
| 3 — Multi-channel mixed-CT (I3) | — |
| 4 — Fleet GC sync | — |
| 5 — Quick read | — |
| 6 — Wh meter on LCD | `rbamp` (the LCD driver is a separate component of your choice) |
| 7 — Multi-module (legacy sequential) | — |
| 8 — MQTT per channel | `rbamp mqtt esp_wifi esp_event esp_netif nvs_flash` |
| 9 — Bidirectional metering | — |
| 10 — Energy balance | — |
| 11 — Event-based logging | — |
| 12 — SPIFFS logger | `rbamp spiffs` |
| 13 — Deep-sleep logger | `rbamp esp_sleep esp_wifi esp_event esp_netif nvs_flash mqtt` (if you publish) |

The networking components also require `nvs_flash_init()` in `app_main`
and WiFi bring-up. For brevity, all of this is omitted in the snippets
below; the full project always lives in [`examples/<name>/`](../examples/).

> **Important note about the snippets below.** Each snippet is a
> **distillation of the key logic**, not a full `app_main`. In working
> code, the line `rbamp_handle_t dev` is always preceded by
> `i2c_master_bus_config_t bus_cfg = {...}; i2c_new_master_bus(&bus_cfg, &bus);
> rbamp_new(bus, 0x50, &dev); rbamp_begin(dev);` (for single-module) or
> `rbamp_fleet_create(bus, &fleet); rbamp_fleet_scan(fleet, true, &n);`
> (for the fleet). When work is done — `rbamp_del(dev)` /
> `rbamp_fleet_destroy(fleet)` (omitted where `app_main` enters an
> infinite loop — the handle lives until the chip reboots).

---

## Scenario 1 — Mains + N sub-loads (the 80% canon): integrated metering system

**Goal:** the library's canonical deployment — a coherent metering
system over a heterogeneous fleet, closed in a single loop:

1. **Discover** — `rbamp_fleet_scan` finds all modules, verifies
   their identity, and weeds out conflicting addresses.
2. **Configure** — `rbamp_configure_channels` sets the sensor class +
   per-channel CT models + a single terminal `SAVE_USER_CONFIG` in one
   call per multi-channel module.
3. **GC arm** — `rbamp_fleet_enable_gc_all` enables General-Call latch
   reception on every module in the fleet.
4. **Loop** — on each tick the master performs an atomic
   `_gclatch` (a synchronous snapshot across the whole fleet),
   cross-checks `_check_sync`, reads `_poll_all`, integrates energy by
   `master_dt`, and computes `_total_power` + `_total_energy_wh`.

This is the **main example of the chapter** — it shows the whole
library in action on the canonical configuration. Every other scenario
(2-17) breaks this flow into individual building blocks or augments it
with third-party integrations (LCD, MQTT, SD).

**Hardware (HW-validated):** a heterogeneous fleet of 3 modules on a
single bus, with an **external 4.7 kΩ pull-up** on SDA/SCL (see
[04 · Wiring](04_hardware.md), section "Multi-module bus — the primary
topology"):

- `@0x50` — UI1, mains meter at the service entry (one current channel + voltage).
- `@0x51` — I2, a two-channel current sub-meter (no voltage).
- `@0x52` — I3, a three-channel current sub-meter (no voltage).

The reference load on the line is ~0.57 A. Lib baseline — `94c5777`
(with the fix for the 3-channel `configure_channels` ch0 mis-bind).

### Code — integrated loop

```c
#include "rbamp.h"
#include "rbamp_fleet.h"
#include "esp_timer.h"

static const char *TAG = "metering";

/* The per-module Wh accumulator lives in the handle (the component
 * already integrates by master_dt in rbamp_read_period_snapshot —
 * here we only read it). */

static void configure_one(rbamp_handle_t dev) {
    if (!dev) return;
    uint8_t n = rbamp_channel_count(dev);
    if (n == 1) {                                /* UI1 mains */
        rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
        rbamp_set_ct_model(dev, /*example=*/3);   /* SCT-013-030 */
        rbamp_save_user_config(dev);
    } else if (n == 2) {                          /* I2 sub-meter */
        const uint8_t models[2] = { 1, 3 };       /* 5A, 30A */
        rbamp_configure_channels(dev, RBAMP_SENSOR_SCT013, models, 2);
    } else if (n == 3) {                          /* I3 sub-meter */
        const uint8_t models[3] = { 1, 3, 6 };    /* 5A, 30A, 20A */
        rbamp_configure_channels(dev, RBAMP_SENSOR_SCT013, models, 3);
    }
}

void app_main(void) {
    /* 1) bus + fleet_create — omitted for brevity */
    rbamp_fleet_t fleet;
    ESP_ERROR_CHECK(rbamp_fleet_create(bus, &fleet));

    /* 2) discover */
    size_t n_added = 0;
    if (rbamp_fleet_scan(fleet, true, &n_added) == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bus wedged — see ch.10");
        return;
    }
    ESP_LOGI(TAG, "fleet_scan: %u module(s), %u excluded",
             (unsigned)n_added,
             (unsigned)rbamp_fleet_excluded_count(fleet));

    /* 3) configure per-module (mixed-CT on the sub-meters) */
    for (size_t i = 0; i < rbamp_fleet_count(fleet); ++i) {
        configure_one(rbamp_fleet_get(fleet, i));
    }

    /* 4) arm GC across the whole fleet */
    ESP_ERROR_CHECK(rbamp_fleet_enable_gc_all(fleet, /*group=*/0));

    /* 5) loop: gclatch → check_sync → poll_all → energy + totals */
    rbamp_snapshot_t snaps[RBAMP_FLEET_MAX_MODULES];
    rbamp_fleet_poll_t status[RBAMP_FLEET_MAX_MODULES];
    uint16_t tick = 0;

    while (1) {
        rbamp_fleet_gclatch(fleet, /*group=*/0, tick);

        bool in_sync = false;
        size_t missed = SIZE_MAX;
        rbamp_fleet_check_sync(fleet, tick, &in_sync, &missed);

        size_t n_ok = 0;
        rbamp_fleet_poll_all(fleet, snaps, status,
                             RBAMP_FLEET_MAX_MODULES, &n_ok);

        /* Energy is accumulated per-handle by the component on each
         * period-snapshot read. Here we read the fleet-wide aggregates. */
        float  p_total = 0.0f;
        double e_total = 0.0;
        rbamp_fleet_total_power(fleet, &p_total);
        rbamp_fleet_total_energy_wh(fleet, &e_total);

        ESP_LOGI(TAG, "ITER %3u: GC in_sync=%s | poll n_ok=%u/%u | "
                       "P_total=%.1f W  E_total=%.3f Wh",
                 (unsigned)tick,
                 in_sync ? "3/3 skew=0" : "MISS",
                 (unsigned)n_ok,
                 (unsigned)rbamp_fleet_count(fleet),
                 p_total, e_total);

        tick++;
        vTaskDelay(pdMS_TO_TICKS(1000));   /* 1 Hz for a compact summary */
    }
}
```

### Bench output (HW-validated on the full Fix-A fleet, lib `94c5777` + firmware Fix-A)

```text
INTEGRATED METERING SYSTEM — fleet_scan: 3 module(s), 0 excluded
  module[0] @0x50  channels=1  voltage=yes    (mains meter: voltage + 1 current)
  module[1] @0x51  channels=2  voltage=no     (2-channel current sub-meter)
  module[2] @0x52  channels=3  voltage=no     (3-channel current sub-meter)
  configure_channels(2ch) → mirrors [1 2 -]
  configure_channels(3ch) → mirrors [1 2 3]
  enable_gc_all → 3/3 modules armed

ITER 1..N:  GC in_sync=3/3 skew=0 | poll n_ok=3/3 | P_total≈1260 W  | E_total: 2.49 → 4.38 Wh (accumulating)
   @0x50  V=226  I=[5.58]              P=[1256]   (mains)
   @0x51  V= —    I=[11.30, 3.75]       P=[—]      (current sub-meter)
   @0x52  V= —    I=[ 3.73, 1.87, 7.50] P=[—]      (current sub-meter; the 3rd input
                                                    is unused on this rig,
                                                    pickup ≈ zero)
```

> The bench figures come from uncalibrated DUTs and illustrate the
> shape of the data and its flow through the system — **not**
> metrological accuracy. For production accuracy, use calibrated
> modules.

### Notes for deployment

- **One synchronized loop is the whole system.** Each tick: one
  `gclatch` (a single I²C General-Call transaction) → all modules
  atomically start their period latch (`check_sync` = 3/3, skew = 0) →
  one `poll_all` reads the whole fleet → aggregation. No drift between
  modules from a chain of sequential round-trips.
- **`total_power` behavior in a heterogeneous fleet.** UI1 yields
  active power (it has voltage → it has `P`); I2/I3 are current-only,
  so their contribution to `total_power` is zero. Therefore, in the
  canon, `total_power` ≈ the power of the mains module. For **per-load**
  power on a sub-meter, compute it on the master side:
  `P_subload[k] ≈ V_mains × I_sub[k]` (an approximation, correct when
  the PFs are close; exact per-load P will only be available once a
  UI variant for sub-meters ships).
- **Energy (Wh) — master wall-clock.** `E_total` grows monotonically
  (2.55 → 6.94 Wh over the bench capture window); the component
  integrates each period by `master_dt_ms` (esp_timer), not by the
  chip-side `latch_ms` (which underreported dt by **−26 %** in the
  same session). A spurious tick adds `0 Wh` if no new period elapsed
  between polls — this is **normal** (snap.valid = 0 on that cycle).
- **MISS-resilient polling.** If one of the modules drops off the bus
  (disconnect, reboot), `poll_all` flags it with `status[i] != ESP_OK`
  and **continues** polling the rest — the master does not hang.
  Verified on bench: a module that disconnected mid-run left the fleet
  polling at `n_ok=2/3` without a stall. The same goes for `check_sync`
  — `missed_idx` points to the first out-of-sync module while the rest
  keep latching.
- **Balance analytics.** In the 80% canon:
  `balance = P_mains − Σ(V_mains × I_sub[k])` — an estimate of "the
  rest" (background consumption, unaccounted-for loads). Targeted code
  iterates over the fleet, separating the mains handle (the one with
  `has_voltage = true`) from the sub-handles (all the others).
- **Per-channel currents → load disaggregation.** Through
  `fleet_poll_all` the master sees all current channels at once
  (1+2+3 = 6 on our bench fleet), without separate per-module readers.
- **Mixed-CT on a 3-channel module requires lib ≥ `94c5777`.** This
  fix closes the ch0 mis-bind bug on the 3-channel configuration —
  before it, mirrors [1 2 3] on I3 would not settle. On I2 (2 channels)
  the fix is not needed, but the bug only existed on 3-channel.
- **Poll latency for production sizing (measured on HW).** On the bench
  @50 kHz I²C: 2 modules — avg `89.4 ms` (min 89.4 / max 89.6);
  3 modules — avg `112.1 ms` (min 112.1 / max 112.3). That is
  **≈ 22-23 ms per additional module**, linear and with very low jitter
  (min ≈ max). A single batched `fleet_poll_all` is deterministic on a
  clean bus. A 3-module fleet (~112 ms) fits within a 200 ms RT window
  — meaning a **5 Hz polling rate is achievable** without bus
  saturation. The full bus budget (when the fleet exceeds the 200 ms
  window) is in the table in [04 · Wiring](04_hardware.md), section
  "Bus energy budget".

---

## Scenario 2 — Provisioning workflow (virgin → fleet)

**Goal:** move a new module from its factory address `0x50` to a
working address and add it to an existing fleet, persisting its
configuration (CT models, label) to flash.

**Hardware (HW-validated):** a virgin module on factory `0x50`
(HW_VARIANT 3-channel). Provisioning moves it to address `0x52` and
saves the configuration to flash.

```c
#include "rbamp_fleet.h"
#include "rbamp.h"

void provision_new_module(i2c_master_bus_handle_t bus,
                          rbamp_fleet_t fleet,
                          uint8_t desired_addr,
                          const char *label /* nullable */) {
    /* PRECONDITION: exactly one virgin at 0x50 on the bus.
     * MUST one-virgin-at-a-time — see ch.04 + ch.10. */
    rbamp_handle_t dev = NULL;
    esp_err_t err = rbamp_provision(bus, desired_addr,
                                     /*save_config=*/true, &dev);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE("prov", "nobody answers at 0x50 — check VCC/I2C");
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE("prov", "0x50 or 0x%02X is already taken — "
                          "discipline violation? see ch.10",
                 desired_addr);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE("prov", "provision fail: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI("prov", "provisioned virgin → 0x%02X (+saved)", desired_addr);

    /* Label for fleet iterations (optional). */
    if (label) rbamp_write_label(dev, label);

    /* Bind the sensor class + CT model + one more SAVE_USER_CONFIG. */
    rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
    rbamp_set_ct_model(dev, /*example=*/3);   /* SCT-013-030 */
    rbamp_save_user_config(dev);

    /* Add to the fleet — from here the module is polled the usual way. */
    rbamp_fleet_add(fleet, dev);
    ESP_LOGI("prov", "module @0x%02X added to the fleet", desired_addr);
}
```

**Bench output (HW-validated success path, lib `94c5777`):**

```text
before: present(0x50)=yes  present(0x52)=no
virgin @0x50: variant=3-channel, channels=3
rbamp_provision(→0x52, save_config=true) = ESP_OK
  (lib log: "provisioned virgin → 0x52 (+saved)")
post:   handle now answers @0x52, is_provisioned=true
after:  present(0x50)=no   present(0x52)=yes
```

`is_provisioned` returns `true` after a successful call. On a module
fresh from the factory (one nothing has touched yet) the prior
`is_provisioned` will be `false` — this is the normal `0xFB`
(`ERR_FLASH_PARAMS_BAD`) state described in
[10 · Troubleshooting](10_troubleshooting.md), and is not fatal.

### Failure paths

| Error | Condition | Recovery |
|---|---|---|
| `ESP_ERR_NOT_FOUND` | Nobody answers at `0x50` | Check that the virgin module is physically connected and powered (VCC); bus-scan ([10 · Troubleshooting](10_troubleshooting.md)). |
| `ESP_ERR_INVALID_STATE` | A conflict was detected at `0x50` or `desired_addr`; different modules answer identically | **Discipline violation** — more than one virgin on the bus. Power-cycle + physically disconnect all but one + retry (see the recovery path in [10 · Troubleshooting](10_troubleshooting.md)). |
| `ESP_ERR_TIMEOUT` | The two-phase commit went through, but the module did not appear at the new address within the boot window | Power-cycle the module, retry. If it persists — flash-cycle issue. |
| `ESP_ERR_INVALID_ARG` | `desired_addr` outside `0x08..0x77` | Use a valid 7-bit address. |

A detailed failure-mode tour: [10 · Troubleshooting](10_troubleshooting.md),
section "Fleet: `rbamp_provision` returned an error".

---

## Scenario 3 — Multi-channel mixed-CT (I3, different models per channel)

**Goal:** demonstrate `rbamp_configure_channels` on an I3 — three
channels with different CT models (e.g. ch0=SCT-013-005 for a small
load, ch1=SCT-013-030 for a medium one, ch2=SCT-013-020 for a large
one) with a **single** terminal flash cycle (flash-friendly), and
confirm persistence across a reboot.

```c
#include "rbamp.h"
#include "esp_timer.h"

void configure_mixed_ct(rbamp_handle_t dev_i3) {
    /* (a) Configure — three CT models in a single call. */
    const uint8_t models[3] = { 1, 3, 6 };
    /* 1=SCT-013-005 (5A), 3=SCT-013-030 (30A), 6=SCT-013-020 (20A) */

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = rbamp_configure_channels(
        dev_i3, RBAMP_SENSOR_SCT013, models, 3);
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        uint8_t reg_err;
        rbamp_read_last_error(dev_i3, &reg_err);
        ESP_LOGE("3", "configure rejected: %s, REG_ERROR=0x%02X",
                 esp_err_to_name(err), reg_err);
        return;
    }
    ESP_LOGI("3", "configure_channels latency = %lld ms",
             (long long)((t1 - t0) / 1000));

    /* (b) Verify the mirror via registers 0x51 / 0x52 / 0x53 (RAM). */
    uint8_t m0, m1, m2;
    rbamp_read_ct_model_ch(dev_i3, 0, &m0);
    rbamp_read_ct_model_ch(dev_i3, 1, &m1);
    rbamp_read_ct_model_ch(dev_i3, 2, &m2);
    ESP_LOGI("3", "applied: [%u %u %u]  (expected [1 3 6])", m0, m1, m2);

    /* (c) Persist verify — reset + re-read the mirror after boot. */
    rbamp_reset(dev_i3);
    vTaskDelay(pdMS_TO_TICKS(300));   /* boot window */
    rbamp_read_ct_model_ch(dev_i3, 0, &m0);
    rbamp_read_ct_model_ch(dev_i3, 1, &m1);
    rbamp_read_ct_model_ch(dev_i3, 2, &m2);
    ESP_LOGI("3", "after reboot: [%u %u %u]", m0, m1, m2);
}
```

**Bench output (HW-validated on a clean-reflashed I3, lib `94c5777` +
`c8ac901`):**

```text
configure_channels(SCT013, {1, 3, 6}, 3) = ESP_OK
configure_channels latency = 1403 ms
applied: [1 3 6]  (raw 0x01 0x03 0x06)
after reboot: [1 3 6]  (persisted through CMD_RESET + boot)
```

### The "single SAVE" property — why it matters

`configure_channels` does **one** terminal `CMD_SAVE_USER_CONFIG` at
the end. The alternative — separate `set_ct_model_ch` calls for each
channel, each with its own `save_user_config` afterward — gives
**three** flash erase + write cycles.

**Bench latency comparison (on the same I3):**

| Approach | Latency | Flash cycles |
|---|---|---|
| `configure_channels(...)` — 1 terminal SAVE | **~1403 ms** | **1** |
| 3× `(set_ct_model_ch + save_user_config)` | **~3492 ms** | **3** |

3× slower — that is `(~2.5×)` × `~700 ms`. The key point is the
**3× wear-cycle difference** on the same flash page: with the typical
re-configuration of modules in the field, this turns into a meaningful
wear cost. Always use `configure_channels` when you need to set the
sensor class + per-channel CT models.

> **Valid CT model codes (Bench-characterized).** On the current bench,
> the following are validated and working for the SCT-013 family:
> `1=SCT-013-005`, `2=SCT-013-010`, `3=SCT-013-030`, `4=SCT-013-050`,
> `6=SCT-013-020`. Codes `5` (SCT-013-100) and `7` (SCT-013-060) are
> **present in the API** but **not yet characterized on the current
> rig** — attempting to use them yields `ESP_ERR_INVALID_ARG` /
> `REG_ERROR=0xFE (ERR_PARAM)`. In production, use only the
> characterized codes `{1, 2, 3, 4, 6}`.

---

## Scenario 4 — Fleet GC sync (billing-grade synchronicity)

**Goal:** demonstrate an atomic latch across the whole fleet with a
single I²C General-Call frame. Unlike the sequential per-module
`CMD_LATCH_PERIOD` (where skew accumulates on every round-trip), GC
synchronizes all modules in a **single** bus transaction.

**Hardware:** the same heterogeneous UI1+I2+I3 fleet (`@0x50/0x51/0x52`).

```c
#include "rbamp_fleet.h"

static const char *TAG = "gc_sync";

void app_main(void) {
    /* bus init + fleet_create + fleet_scan ... as in Sc.1 */

    /* Step 1: enable GC on all modules in the fleet. */
    ESP_ERROR_CHECK(rbamp_fleet_enable_gc_all(fleet, /*group_id=*/0));
    ESP_LOGI(TAG, "GC armed");

    uint16_t tick = 0;
    for (int round = 0; round < 10; ++round) {
        /* Step 2: a single broadcast latch — all modules atomically. */
        ESP_ERROR_CHECK(rbamp_fleet_gclatch(fleet, /*group=*/0, tick));

        /* Step 3: in-sync check — all modules caught the same tick. */
        bool all_in_sync = false;
        size_t missed_idx = SIZE_MAX;
        rbamp_fleet_check_sync(fleet, tick, &all_in_sync, &missed_idx);
        ESP_LOGI(TAG, "round %d  tick=%u  in_sync=%s",
                 round, tick, all_in_sync ? "3/3" : "MISS");

        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

**Bench output (10 rounds on the UI1+I2+I3 fleet, 4.7 kΩ pull-up):**

```text
rbamp_fleet_enable_gc_all() → 3/3 modules armed
round 0  tick=0  in_sync=3/3   skew=0
round 1  tick=1  in_sync=3/3   skew=0
round 2  tick=2  in_sync=3/3   skew=0
round 3  tick=3  in_sync=3/3   skew=0
round 4  tick=4  in_sync=3/3   skew=0
round 5  tick=5  in_sync=3/3   skew=0
round 6  tick=6  in_sync=3/3   skew=0
round 7  tick=7  in_sync=3/3   skew=0
round 8  tick=8  in_sync=3/3   skew=0
round 9  tick=9  in_sync=3/3   skew=0
```

**When to use GC sync:**

- **Billing-grade synchronicity.** If the per-minute/per-second
  snapshot difference between modules matters for reporting (for
  example, a balance calculation `mains − Σ(sub)` integrated over time
  — module skew introduces a systematic error), GC delivers `skew = 0`
  by bench measurement. A sequential per-module latch on the same
  3 modules accumulates 100-300 µs per module × N, which becomes
  noticeable over a 60-minute window in pinpoint analytics.
- **Large fleet.** As the number of modules grows, the difference
  between "sequential N × round_trip" and "a single GC frame" grows
  linearly.
- **Cross-deployment alignment.** If you have several ESP32s on
  different buses sharing a master clock, the GC tick can be
  synchronized via the master-side wall-clock — and then snapshots
  from **different** buses are aligned within a single network
  round-trip window.

**Witness via `check_sync`:** a check that all modules actually
received the GC frame. It may miss a module if it was busy booting /
in a SAVE cycle at the moment of the frame — `missed_idx` points to
the first out-of-sync one. In bench validation, 10 rounds in a row —
0 misses.

**Precondition.** GC must be **enabled** on every module
(`rbamp_fleet_enable_gc_all` or per-device `rbamp_enable_gc` +
SAVE_USER_CONFIG + reset). A fresh module with factory defaults does
**not** accept GC — this is by design, a guard against accidental
broadcasts on a bring-up bus.

---

## Scenario 5 — Quick read (single module)

**Goal:** print U / I / P / PF / frequency to the ESP-IDF log once a
second. The same "hello world" you wrote in
[05 · Quickstart](05_quickstart.md), but via `rbamp_read_all()` — a
single-shot read of the whole RT block into one struct.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

static const char *TAG = "quick_read";

void app_main(void) {
    /* ...bus_cfg setup, rbamp_new + rbamp_begin as in the Quickstart... */
    rbamp_handle_t dev = NULL;

    while (1) {
        rbamp_snapshot_t s;
        if (rbamp_read_all(dev, &s) != ESP_OK) {
            ESP_LOGE(TAG, "read fail");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "U=%.1f V  f=%.1f Hz  channels=%u",
                 (double)s.voltage, (double)s.frequency, s.channels);
        for (uint8_t ch = 0; ch < s.channels; ++ch) {
            ESP_LOGI(TAG, "  I%u=%.2f A  P%u=%.1f W  PF%u=%.2f",
                     ch, (double)s.current[ch],
                     ch, (double)s.power[ch],
                     ch, (double)s.power_factor[ch]);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

**What happens on the bus:** `rbamp_read_all()` on a UI3 performs ~53
single-byte transactions (13 float values × 4 bytes + 1 frequency
byte). At 50 kHz with retry headroom — on the order of 25-30 ms. If you
don't need all the values, call the per-property functions
(`rbamp_read_voltage()`, `rbamp_read_current(ch)`) — fewer bytes on the
bus.

---

## Scenario 6 — Wh meter on a 16×2 LCD

**Goal:** a Wh counter, refreshed once a minute, on a 16×2 character
LCD with a PCF8574 I²C expander (HD44780-compatible). The LCD sits on
the same I²C bus as the rbAmp. The component's key mechanism is
`rbamp_read_period_snapshot()`, which encapsulates
latch + settle + valid-check + read + Wh-tick in a single call.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

/* Minimal LCD shim — inline in the example, no third-party drivers.
 * The full lcd_print/lcd_clear functions are in examples/lcd_period/main/main.c. */

void app_main(void) {
    /* ...bus_cfg + rbamp + lcd init... */
    rbamp_handle_t dev = NULL;

    uint32_t ok_count = 0, bad_count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK
                || !snap.valid) {
            bad_count++;
            continue;
        }
        ok_count++;

        char line1[17], line2[17];
        snprintf(line1, sizeof(line1), "P=%6.1f W      ", (double)snap.avg_p[0]);
        snprintf(line2, sizeof(line2), "%8.4f Wh ok%lu", rbamp_energy_wh(dev, 0),
                 (unsigned long)ok_count);
        lcd_clear();
        lcd_print(0, 0, line1);
        lcd_print(0, 1, line2);
    }
}
```

The full IDF project — [`examples/lcd_period/`](../examples/lcd_period/).

**Handling stale snapshots.** `rbamp_read_period_snapshot()` returns
`ESP_ERR_INVALID_RESPONSE` if the module did not manage to prepare a
new snapshot by read time. The component **still records** its
timestamp (`esp_timer_get_time()`) internally — so the next successful
snapshot is not double-counted on the interval.

---

## Scenario 7 — Monitoring 3 modules (legacy sequential) on a single bus

**Goal:** poll 3 modules at addresses 0x50 / 0x51 / 0x52 from one
master. Each module must have a unique address (addresses are assigned
during the factory/integrator installation step — see
[09 · API Reference](09_api_reference.md) on the address-change methods
with their WARNING notes).

> **The canonical pattern for v1 firmware** is a sequential
> `rbamp_latch_period()` on each device + a single shared 50 ms settle
> + per-device `rbamp_read_period_snapshot(skip_latch=true)`.
> Inter-device skew at 100 kHz: ~1 ms per device, < 0.2 % of a
> 60-second period.
>
> The `rbamp_broadcast_latch(bus, timeout_ms)` function is reserved
> for v2 firmware (General-Call is disabled in the v1 module's I²C
> peripheral) — it returns `ESP_ERR_NOT_SUPPORTED`. See
> [09 · API Reference](09_api_reference.md) for details.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rbamp.h"

#define N_MODULES 3
static const char *TAG = "multi_module";
static const uint8_t ADDRS[N_MODULES] = { 0x50, 0x51, 0x52 };

void app_main(void) {
    /* ...bus_cfg setup as in the Quickstart... */
    i2c_master_bus_handle_t bus = NULL;
    rbamp_handle_t modules[N_MODULES] = { NULL };

    /* Create a handle for each module */
    for (int i = 0; i < N_MODULES; ++i) {
        if (rbamp_new(bus, ADDRS[i], &modules[i]) != ESP_OK ||
            rbamp_begin(modules[i]) != ESP_OK) {
            ESP_LOGE(TAG, "module 0x%02X init failed", ADDRS[i]);
            modules[i] = NULL;
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        /* Phase 1: sequential LATCH on each device, measure the skew */
        const int64_t sync_start_us = esp_timer_get_time();
        for (int i = 0; i < N_MODULES; ++i) {
            if (modules[i]) rbamp_latch_period(modules[i]);
        }
        const uint32_t sync_us = (uint32_t)(esp_timer_get_time() - sync_start_us);

        /* Phase 2: a single shared settle for all 3 modules */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Phase 3: read the snapshots with skip_latch=true */
        ESP_LOGI(TAG, "sync_us=%u", (unsigned)sync_us);
        for (int i = 0; i < N_MODULES; ++i) {
            if (!modules[i]) continue;
            rbamp_period_snapshot_t snap;
            if (rbamp_read_period_snapshot(modules[i], &snap, 0,
                                            /*skip_latch=*/true) != ESP_OK) {
                continue;
            }
            ESP_LOGI(TAG, "mod%d 0x%02X  P=%.0f W  Wh=%.3f",
                     i, ADDRS[i],
                     (double)snap.avg_p[0],
                     rbamp_energy_wh(modules[i], 0));
        }
    }
}
```

The full project (with bus-scan probe + error reporting) —
[`examples/multi_module/`](../examples/multi_module/).

---

## Scenario 8 — MQTT per channel

**Goal:** a UI3 module with 3 CT clamps on three independent lines.
Per-channel Wh counters, published to MQTT once a minute. Shows that
`rbamp_energy_wh(dev, ch)` works on each channel independently — no
need for a manual `total_wh[3]` array on the master side.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "rbamp.h"

static const char *TAG = "ui3_mqtt";
static const char *CH_NAMES[3] = { "main", "heatpump", "lights" };
static esp_mqtt_client_handle_t mqtt;

static void publish_channel(const char *name, float avg_p, double e_wh) {
    char topic[64], payload[96];
    snprintf(topic,   sizeof(topic),   "rbamp/%s/state", name);
    snprintf(payload, sizeof(payload),
             "{\"power\":%.1f,\"energy\":%.4f}", (double)avg_p, e_wh);
    esp_mqtt_client_publish(mqtt, topic, payload, 0, /*qos*/1, /*retain*/1);
}

void app_main(void) {
    /* ...wifi STA + mqtt_client init... */
    /* ...bus_cfg + rbamp_new + rbamp_begin (50 kHz via Kconfig)... */
    rbamp_handle_t dev = NULL;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK ||
            !snap.valid) {
            continue;
        }

        for (uint8_t ch = 0; ch < rbamp_channels(dev); ++ch) {
            publish_channel(CH_NAMES[ch], snap.avg_p[ch],
                            rbamp_energy_wh(dev, ch));
        }
    }
}
```

The full project — [`examples/mqtt_publisher/`](../examples/mqtt_publisher/).
For HA MQTT Auto-discovery based on this pattern — see
[07 · DIY integrations](07_diy_integrations.md) +
[`examples/ha_discovery/`](../examples/ha_discovery/).

---

## Scenario 9 — Bidirectional metering on the master side

**Goal:** split signed instantaneous power into gross-consume and
gross-export. Use this pattern on the BASIC tier, where the
**firmware** clips negative values in `snap.avg_p[ch]` (period-averaged
power) — sample `rbamp_read_power(dev, 0, &p)` (instantaneous power,
**signed on all tiers**) at 5 Hz on the master side and bucket it
yourself.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rbamp.h"

static const char *TAG = "bidir";

void bidir_task(void *arg) {
    rbamp_handle_t dev = arg;
    double consume_wh = 0.0, export_wh = 0.0;
    int64_t t_prev_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));   /* 5 Hz, matches the RT cadence */
        const int64_t t_now_us = esp_timer_get_time();
        const double  dt_s     = (double)(t_now_us - t_prev_us) / 1e6;
        t_prev_us = t_now_us;

        float p;
        if (rbamp_read_power(dev, 0, &p) != ESP_OK) continue;

        const double dwh = (double)p * dt_s / 3600.0;
        if (p >= 0.0f) consume_wh += dwh;
        else           export_wh  += -dwh;

        /* Print once a second */
        static int n = 0;
        if (++n % 5 == 0) {
            ESP_LOGI(TAG, "P=%.1f W   cons=%.4f Wh  exp=%.4f Wh  net=%.4f Wh",
                     (double)p, consume_wh, export_wh,
                     consume_wh - export_wh);
        }
    }
}
```

This is a composite scenario with no dedicated IDF project. On the
future STANDARD / PRO firmware tiers (planned for v1.3+) the period
accumulator `rbamp_energy_wh(dev, 0)` will already return a signed net
balance — this master-side split is only needed for separate
gross-consume / gross-export reporting.

---

## Scenario 10 — Whole-home energy balance

**Goal:** 3 modules — mains (bidirectional), solar (generation only),
loads (UI3 for per-appliance). The combined dashboard is published once
a minute.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "rbamp.h"

static rbamp_handle_t mains_dev, solar_dev, loads_dev;
static esp_mqtt_client_handle_t mqtt;

void app_main(void) {
    /* ...wifi STA + mqtt init... */
    /* ...bus_cfg + per-module rbamp_new(bus, 0x50/0x51/0x52, ...) +
     *    rbamp_begin() on each... */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        /* Sequential LATCH + shared settle (the pattern from Scenario 3) */
        rbamp_latch_period(mains_dev);
        rbamp_latch_period(solar_dev);
        rbamp_latch_period(loads_dev);
        vTaskDelay(pdMS_TO_TICKS(50));

        rbamp_period_snapshot_t sm, ss, sl;
        if (rbamp_read_period_snapshot(mains_dev, &sm, 0, true) != ESP_OK || !sm.valid ||
            rbamp_read_period_snapshot(solar_dev, &ss, 0, true) != ESP_OK || !ss.valid ||
            rbamp_read_period_snapshot(loads_dev, &sl, 0, true) != ESP_OK || !sl.valid) {
            continue;
        }

        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"mains\":%.3f,\"solar\":%.3f,"
            "\"hp\":%.3f,\"ac\":%.3f,\"ev\":%.3f}",
            rbamp_energy_wh(mains_dev, 0),
            rbamp_energy_wh(solar_dev, 0),
            rbamp_energy_wh(loads_dev, 0),
            rbamp_energy_wh(loads_dev, 1),
            rbamp_energy_wh(loads_dev, 2));
        esp_mqtt_client_publish(mqtt, "home/energy/balance",
                                payload, 0, 1, 1);
    }
}
```

For an explicit gross-consume / gross-export split on the mains —
combine with the pattern from Scenario 5 (a separate 5 Hz task on
`rbamp_read_power(mains_dev, 0, ...)`).

This is a composite scenario with no dedicated IDF project — assemble
it from [`examples/mqtt_publisher/`](../examples/mqtt_publisher/) +
[`examples/multi_module/`](../examples/multi_module/).

---

## Scenario 11 — Event-based logging (EMA)

**Goal:** on each 200 ms RT window, compare instantaneous power against
an exponential moving average. Log significant deviations — loads such
as a microwave, a kettle, or a hair dryer "appear" in the log as a
turn-on / turn-off event.

```c
#include <math.h>                /* fabsf */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rbamp.h"

#define EMA_ALPHA            0.05f
#define EVENT_THRESHOLD_W    200.0f

static const char *TAG = "event";

void event_task(void *arg) {
    rbamp_handle_t dev = arg;
    float p_ema = 0.0f;

    /* seed the EMA so the first reading is not a false event */
    if (rbamp_read_power(dev, 0, &p_ema) != ESP_OK) p_ema = 0.0f;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        float p;
        if (rbamp_read_power(dev, 0, &p) != ESP_OK) continue;

        const float delta = p - p_ema;
        p_ema = (1.0f - EMA_ALPHA) * p_ema + EMA_ALPHA * p;

        if (fabsf(delta) > EVENT_THRESHOLD_W) {
            ESP_LOGI(TAG, "%s  delta=%.0f W   P=%.0f W   EMA=%.0f W",
                     (delta > 0) ? "TURN_ON" : "TURN_OFF",
                     (double)delta, (double)p, (double)p_ema);
        }
    }
}
```

A composite scenario — no dedicated IDF project. Combine it with the
MQTT publishing from Scenario 4 for HA-side automations, or write the
events to SPIFFS using the pattern from Scenario 8 for an offline log.

---

## Scenario 12 — Local CSV logger on SPIFFS

**Goal:** write a period snapshot to a CSV file on SPIFFS once a
minute. Useful for standalone deployments without WiFi / MQTT, or as a
buffer for deferred cloud sync.

> **Requires a custom partition table** with the entry
> `storage,data,spiffs,,128K,` in `partitions.csv`. A sample is in
> `examples/spiffs_logger/partitions.csv`.

```c
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rbamp.h"

#define LOG_PATH       "/storage/log.csv"
#define ROTATE_BYTES   (64 * 1024)
static const char *TAG = "spiffs_logger";

static void rotate_if_full(void) {
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size >= ROTATE_BYTES) {
        rename(LOG_PATH, "/storage/log_prev.csv");
    }
}

void app_main(void) {
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_cfg));

    /* ...bus_cfg + rbamp_new + rbamp_begin... */
    rbamp_handle_t dev = NULL;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK ||
            !snap.valid) continue;

        rotate_if_full();
        FILE *f = fopen(LOG_PATH, "a");
        if (!f) {
            ESP_LOGE(TAG, "fopen failed");
            continue;
        }
        fprintf(f, "%lld,%.3f,%.4f,%u\n",
                (long long)(esp_timer_get_time() / 1000),
                (double)snap.avg_p[0],
                rbamp_energy_wh(dev, 0),
                (unsigned)snap.master_dt_ms);
        fclose(f);
    }
}
```

The full project — [`examples/spiffs_logger/`](../examples/spiffs_logger/).
The snippet above is a pedagogical simplification with a 4-column CSV
row. The example itself writes a **9-column** CSV row with per-channel
`avg_p` and Wh for all 3 channels — more useful for production
scenarios on UI3 modules.

For deferred cloud sync (log locally, send once an hour) — see the
section "Hybrid: local storage + sync" in
[08 · Cloud integrations](08_cloud_integrations.md).

---

## Scenario 13 — Battery logger with deep-sleep

**Goal:** an ESP32 wakes up once every 10 minutes, takes a single
period latch, publishes via WiFi+MQTT, and goes back into deep-sleep.
Energy persists through RTC memory between sleep cycles. The component's
accumulator is **disabled** — the master owns the Wh persistence
itself.

> ⚠ **Important note about deep-sleep.** On the current firmware,
> `rbamp_begin()` always issues a CMD_LATCH_PERIOD primer, which resets
> the firmware's period accumulator. This means that after a deep-sleep
> wake you cannot simply call `rbamp_read_period_snapshot()` by default
> — that call would latch again and return near-zero data ~50 ms after
> the primer. The correct pattern below uses `skip_latch=true` to read
> the data accumulated during the sleep interval, and the **master's
> own RTC timer** for the interval between wakeups (the
> `snap.master_dt_ms` field reflects only the time within the current
> wake cycle, not wake-to-wake).

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "rbamp.h"

#define WAKE_INTERVAL_US (10ULL * 60ULL * 1000000ULL)
#define RTC_MAGIC        0xCAFEFEEDu

RTC_DATA_ATTR static uint32_t rtc_magic        = 0;
RTC_DATA_ATTR static double   rtc_total_wh     = 0.0;
RTC_DATA_ATTR static bool     rtc_primer_done  = false;
RTC_DATA_ATTR static uint64_t rtc_last_wake_us = 0;

static const char *TAG = "deep_sleep";

void app_main(void) {
    /* ...bus_cfg setup... */
    i2c_master_bus_handle_t bus = NULL;
    rbamp_handle_t dev = NULL;

    if (rtc_magic != RTC_MAGIC) {
        /* Cold start — initialize RTC-RAM */
        rtc_magic = RTC_MAGIC;
        rtc_total_wh = 0.0;
        rtc_primer_done = false;
        rtc_last_wake_us = 0;
    }

    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));
    ESP_ERROR_CHECK(rbamp_begin(dev));          /* issues the primer LATCH */
    rbamp_energy_disable(dev);                  /* the master owns Wh itself */

    if (!rtc_primer_done) {
        /* First wake: the primer just latched the "dirty" data from
         * before wake. Record the time and go straight back to sleep —
         * the next wake in 10 min gets a full accumulator for the interval. */
        rtc_last_wake_us = esp_timer_get_time();
        rtc_primer_done = true;
        esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
        esp_deep_sleep_start();
    }

    /* skip_latch=true reads what the rbamp_begin() primer latched —
     * that is, the accumulator for the elapsed 10-minute sleep interval. */
    rbamp_period_snapshot_t snap;
    esp_err_t snap_err = rbamp_read_period_snapshot(dev, &snap, /*settle_ms=*/0,
                                                     /*skip_latch=*/true);
    if (snap_err != ESP_OK || !snap.valid) {
        ESP_LOGW(TAG, "snapshot fail: %s (valid=%d)",
                 rbamp_err_to_str(snap_err), snap.valid);
        esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
        esp_deep_sleep_start();
    }

    /* `snap.master_dt_ms` here is the time within the current wake (after
     * the primer); for energy integration we use `esp_timer_get_time()`
     * against `rtc_last_wake_us`. On ESP-IDF v5.x, the value returned by
     * `esp_timer_get_time()` **survives deep sleep** — the timer is
     * re-derived from the RTC slow-clock on wake. So the delta gives the
     * real wake-to-wake interval in microseconds. */
    const uint64_t now_us = esp_timer_get_time();
    const double   dt_s   = (double)(now_us - rtc_last_wake_us) / 1e6;
    rtc_last_wake_us = now_us;

    rtc_total_wh += (double)snap.avg_p[0] * dt_s / 3600.0;
    ESP_LOGI(TAG, "wake: P=%.1f W  Wh=%.4f  dt_s=%.1f",
             (double)snap.avg_p[0], rtc_total_wh, dt_s);
    publish_via_wifi(snap.avg_p[0], rtc_total_wh, dt_s);

    esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
    esp_deep_sleep_start();
}
```

The full project (with WiFi setup, MQTT publish, RTC magic-marker
guards) — [`examples/deep_sleep_logger/`](../examples/deep_sleep_logger/).

> 🛣 **Roadmap.** A future version of the component is expected to add
> `rbamp_warm_begin()` — a lightweight init variant without the
> CMD_LATCH_PERIOD primer, which will make it simpler to read
> `rbamp_read_period_snapshot()` by default after a deep-sleep wake.
> Until then — the pattern above (skip_latch=true + the master's RTC
> timer) is canonical.

**Energy budget** (ESP32-WROOM, 10-minute wakeup interval):

- Active cycle duration: ~3 s (WiFi + MQTT + I²C)
- Active current: ~80 mA average → ~0.07 mAh per wake
- Sleep current: ~10 µA (RTC + retained domains)
- ~10 mAh per day → a 2000 mAh Li-ion lasts ~6 months

---

## Scenario 14 — I3 with different CT per channel (mixed-load monitoring)

**Goal:** a single UI3 module monitors **three independent lines** on one phase with **different load levels**:
- Channel 0 — a single outlet (SCT-013-005, ≤5 A) — TV / router / standby.
- Channel 1 — the kitchen line (SCT-013-030, ≤30 A) — kettle / microwave.
- Channel 2 — a heating boiler (SCT-013-100, ≤100 A) — peak load.

All three channels are fed from **a single phase** (in phase with the U channel), so P/PF/Q are correct for all three — see the single-voltage caveat in [03 · Current sensor selection](03_sensor_selection.md#modules-with-multiple-current-channels).

```c
#include "driver/i2c_master.h"
#include "rbamp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui3-mixed";

void mixed_load_task(void *arg) {
    rbamp_handle_t dev = (rbamp_handle_t)arg;

    /* Per-channel configuration — DESCENDING ORDER is mandatory */
    rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
    rbamp_set_ct_model_ch(dev, 2, 5);   /* ch2: SCT-013-100 (boiler) */
    rbamp_set_ct_model_ch(dev, 1, 3);   /* ch1: SCT-013-030 (kitchen) */
    rbamp_set_ct_model_ch(dev, 0, 1);   /* ch0: SCT-013-005 (outlet) — last */

    while (1) {
        float u, i0, i1, i2, p0, p1, p2;
        rbamp_read_voltage(dev, 0, &u);
        rbamp_read_current(dev, 0, &i0);
        rbamp_read_current(dev, 1, &i1);
        rbamp_read_current(dev, 2, &i2);
        rbamp_read_power(dev, 0, &p0);
        rbamp_read_power(dev, 1, &p1);
        rbamp_read_power(dev, 2, &p2);

        ESP_LOGI(TAG, "U=%.1f V", (double)u);
        ESP_LOGI(TAG, "  outlet:  I=%.3f A  P=%.1f W", (double)i0, (double)p0);
        ESP_LOGI(TAG, "  kitchen: I=%.2f A  P=%.1f W", (double)i1, (double)p1);
        ESP_LOGI(TAG, "  boiler:  I=%.2f A  P=%.1f W", (double)i2, (double)p2);
        ESP_LOGI(TAG, "  total:   P=%.1f W", (double)(p0 + p1 + p2));

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
```

**When to use:** monitoring a single phase with three load groups of different current magnitude. The main advantage is **resolution** — each clamp matches its range (an SCT-013-005 gives accurate readings on a 50 W standby load, while an SCT-013-100 does not saturate on a 20 kW boiler).

---

## Scenario 15 — Sub-metering: 5×UI1 on a shared bus

**Goal:** monitor 5 independent lines in a single-phase apartment with **billing-grade synchronicity** of periods. One UI1 per line (5 modules total).

This is **strategy 1** for period sync from [04 · Wiring](04_hardware.md#period-sync--synchronizing-periods-for-billing-grade-metering): sequential latch + shared settle.

```c
#define N_METERS 5
static rbamp_handle_t meters[N_METERS];   /* initialized in app_main */

void sub_meter_billing_task(void *arg) {
    int64_t period_start_us = esp_timer_get_time();
    const int64_t PERIOD_S = 60;   /* 60-second metering period */

    while (1) {
        /* Wait until the end of the period */
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_s = (now_us - period_start_us) / 1000000;
        if (elapsed_s < PERIOD_S) {
            vTaskDelay(pdMS_TO_TICKS((PERIOD_S - elapsed_s) * 1000));
            continue;
        }

        /* Phase 1: sequential latch (skew ~5 ms between modules at 50 kHz) */
        for (int i = 0; i < N_METERS; i++) {
            rbamp_latch_period(meters[i]);
        }

        /* Phase 2: shared settle */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Phase 3: read snapshots with skip_latch=true */
        float total_wh_period = 0.0f;
        for (int i = 0; i < N_METERS; i++) {
            rbamp_period_snapshot_t snap;
            if (rbamp_read_period_snapshot(meters[i], &snap, 0, /*skip_latch=*/true) == ESP_OK
                && snap.valid) {
                float wh_this_period = snap.avg_p[0] * snap.period_ms / 3600000.0f;
                ESP_LOGI("meter", "[%d] avg_p=%.1f W  Wh_period=%.2f",
                         i, (double)snap.avg_p[0], (double)wh_this_period);
                total_wh_period += wh_this_period;
            }
        }

        ESP_LOGI("meter", "TOTAL for the period: %.2f Wh\n", (double)total_wh_period);
        period_start_us = esp_timer_get_time();
    }
}
```

**Skew analysis:** 5 modules × ~1 ms/latch = ~5 ms. Relative to the 60-s period = 0.008%. Negligible for billing.

**Bus budget**: 5 modules × ~5 ms RT block = 25 ms for a full round; this also fits at a 5 Hz polling rate (25 ms ≪ 200 ms). Bus headroom is comfortable up to ~15 UI1 modules.

---

## Scenario 16 — GC broadcast latch (billing-grade sync for >8 modules)

**Goal:** at ≥ 8 modules the skew of strategy 1 (sequential) approaches ~10 ms. For billing-grade synchronicity — **strategy 2**, General-Call broadcast.

**Preconfiguration (once per module)**:

```c
/* Enable GC reception on each module */
uint8_t fleet_config = 0x01;   /* bit0 = GC_ENABLE */
rbamp_write_reg(dev, REG_FLEET_CONFIG, &fleet_config, 1);
rbamp_save_user_config(dev);   /* CMD_SAVE_USER_CONFIG, production-OK */
rbamp_reset(dev);              /* CMD_RESET, settle ~300 ms */
```

**Runtime broadcast latch with witness**:

```c
#define N_METERS 12

esp_err_t billing_grade_latch(rbamp_handle_t meters[], int n,
                              uint16_t tick_counter) {
    /* GC frame: A5 27 <group> <tick_lo> <tick_hi> on addr 0x00 */
    uint8_t frame[5] = {
        0xA5,                       /* GC_FRAME_MAGIC */
        0x27,                       /* CMD_LATCH_PERIOD opcode */
        0x00,                       /* group = 0x00 = all-call */
        (uint8_t)(tick_counter & 0xFF),
        (uint8_t)(tick_counter >> 8),
    };

    /* ESP-IDF: write to addr 0x00 (general-call) */
    rbamp_handle_t bus_h = meters[0];   /* shared bus handle */
    esp_err_t err = rbamp_bus_general_call_write(bus_h, frame, sizeof(frame));
    if (err != ESP_OK) return err;

    /* Settle: latch + 1 commit-cycle for witness refresh */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Witness check: each module should show PERIOD_VALID = 1 */
    int n_ok = 0;
    for (int i = 0; i < n; i++) {
        uint8_t valid;
        if (rbamp_read_reg(meters[i], REG_V03_PERIOD_VALID, &valid, 1) == ESP_OK
            && valid == 1) {
            n_ok++;
        } else {
            ESP_LOGW("gc-latch", "module %d did not latch (witness=0)", i);
        }
    }

    if (n_ok < n) {
        ESP_LOGW("gc-latch", "fallback to sequential latch for %d modules",
                 n - n_ok);
        /* Strategy 1 fallback on the remaining ones */
        for (int i = 0; i < n; i++) {
            uint8_t valid;
            rbamp_read_reg(meters[i], REG_V03_PERIOD_VALID, &valid, 1);
            if (valid != 1) {
                rbamp_latch_period(meters[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}
```

**Skew analysis**: a GC frame is ~1 ms at 50 kHz → all 12 modules latch **simultaneously** at the bus level. Skew is practically 0.

**When to use**: ≥ 8 modules and/or billing-grade accuracy.

**When NOT to use**: on a mixed bus with non-rbAmp devices (GC may conflict).

---

## Scenario 17 — GC group filtering (multi-tenant billing)

**Goal**: 10 modules on a single bus, where you need to latch **subsets** independently. For example, 5 modules meter tariff A (daytime), 5 — tariff B (nighttime); they are armed and read on different schedules.

**Setup (once)**:

```c
/* Tariff A — modules 0..4 → group = 1 */
for (int i = 0; i < 5; i++) {
    uint8_t group_id = 1;
    rbamp_write_reg(meters[i], REG_GROUP_ID, &group_id, 1);
    rbamp_save_user_config(meters[i]);
    rbamp_reset(meters[i]);
}

/* Tariff B — modules 5..9 → group = 2 */
for (int i = 5; i < 10; i++) {
    uint8_t group_id = 2;
    rbamp_write_reg(meters[i], REG_GROUP_ID, &group_id, 1);
    rbamp_save_user_config(meters[i]);
    rbamp_reset(meters[i]);
}
```

**Runtime — independent latch per tariff**:

```c
void daytime_billing_cycle(uint16_t tick) {
    /* Latch tariff A only */
    uint8_t frame[5] = { 0xA5, 0x27, 0x01, (uint8_t)tick, (uint8_t)(tick >> 8) };
    rbamp_bus_general_call_write(bus_h, frame, 5);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* read the 5 modules of tariff A */
}

void nighttime_billing_cycle(uint16_t tick) {
    /* Latch tariff B only */
    uint8_t frame[5] = { 0xA5, 0x27, 0x02, (uint8_t)tick, (uint8_t)(tick >> 8) };
    rbamp_bus_general_call_write(bus_h, frame, 5);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* read the 5 modules of tariff B */
}

void all_modules_snapshot(uint16_t tick) {
    /* group = 0x00 — all-call, all 10 latch */
    uint8_t frame[5] = { 0xA5, 0x27, 0x00, (uint8_t)tick, (uint8_t)(tick >> 8) };
    rbamp_bus_general_call_write(bus_h, frame, 5);
}
```

**Benefits**: one cycle per tariff = clean separation of metering intervals. Without groups you would have to maintain parallel sequential latch loops with conflict resolution.

---

## Scenario summary table

| # | LOC | I²C bus | Period? | DRDY? | MQTT? | Persistence |
|:---:|:---:|:---:|:---:|:---:|:---:|---|
| **1** | ~120 | shared 3 modules (UI1+I2+I3) | yes (GC sync) | no | yes | RAM (master-owned) |
| **2** | ~50 | dedicated (one virgin) | no | no | no | flash (`save_config=true`) |
| **3** | ~60 | dedicated (I3) | no | no | no | flash (`SAVE_USER_CONFIG`) |
| **4** | ~50 | shared 3 modules | yes (GC) | no | no | RAM |
| 5 | ~40 | dedicated | no | no | no | no |
| 6 | ~60 | shared with LCD | yes | no | no | RAM |
| 7 | ~80 | shared 3 modules | yes | no | no | RAM |
| 8 | ~70 | dedicated | yes | no | yes | retained MQTT |
| 9 | ~50 | dedicated | no (RT) | no | no | RAM (master-owned) |
| 10 | ~80 | shared 3 modules | yes | no | yes | retained MQTT |
| 11 | ~80 | dedicated | no (RT) | yes | no | RAM |
| 12 | ~70 | dedicated | yes | no | no | SPIFFS |
| 13 | ~80 | dedicated | yes | no | yes | RTC memory + MQTT |
| 14 | ~60 | dedicated I3 | no (RT) | no | no | RAM |
| 15 | ~80 | shared 5×UI1 | yes (billing) | no | no | RAM |
| 16 | ~100 | shared 12 modules | yes (GC sync) | no | no | RAM |
| 17 | ~60 | shared 10 modules multi-tenant | yes (GC groups) | no | no | RAM |

## What's next

- [07 · DIY integrations](07_diy_integrations.md) — Home Assistant /
  Node-RED / OpenHAB based on these scenarios (including
  `examples/ha_discovery/`)
- [08 · Cloud integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB Cloud
- [09 · API Reference](09_api_reference.md) — every public function
  documented
- [10 · Troubleshooting](10_troubleshooting.md) — when the scenarios
  behave differently on your bench

