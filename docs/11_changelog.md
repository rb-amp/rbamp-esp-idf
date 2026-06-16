# 11 · Changelog

## 1.3.0 (2026-06-16) — Public release

First public release of the `rbamp` component for ESP-IDF v5.x.

### Key features

- **Fleet manager**: manage a group of modules on a single I²C bus
  through a single `rbamp_fleet_t` handle. Discovery with conflict detection
  and Tier-2 wedge canary, MISS-resilient `_poll_all`, aggregation of
  total-power / total-energy / errors, broadcast-latch for
  billing-grade synchrony. See chapter
  [09 · API Reference](09_api_reference.md), section "Fleet manager".
- **Provisioning**: `rbamp_provision` — a single-call workflow for
  reassigning a virgin module to its working address, with optional
  persistence of the configuration to flash. Discipline: one virgin at a time
  on the bus (a normative MUST).
- **Multi-channel configuration**: `rbamp_configure_channels` —
  atomic per-channel setup of CT models in a single operation with
  a single terminal flash cycle (flash-friendly).
- **Error model v1.3**: separates the last-write-outcome
  (`REG_ERROR (0x02)` — synchronous channel, validated by setters) from the
  durable async signal (`EVENT_FLAGS.bit3` — sticky, for
  runtime facts). The corresponding functions are `rbamp_read_last_error`,
  `rbamp_has_error`, `rbamp_clear_error`, `rbamp_read_event_flags`.
- **Identity / feature-detect**: `rbamp_read_variant`,
  `rbamp_read_capability` (12-bit map), `rbamp_has_voltage`,
  `rbamp_read_product_id`, `rbamp_read_uid`, `rbamp_is_provisioned`.

### Canon reflected in the documentation

- **Multi-module topology is the library's primary operating mode.**
  The "one module on the bus" scenario is the minimal case, not the
  canon. The canonical deployment is **a mains module at the service
  entrance plus N modules on individual loads**, all on a single I²C bus
  under a single fleet handle.
- **Supported SKUs on current hardware**:
  `UI1` (mains meter: voltage + current + power + PF + energy),
  `I1` / `I2` / `I3` (sub-meters: current only, per channel). `UI2` / `UI3`
  are on the roadmap.
- **Energy and power are computed only on UI variants** (which have a
  voltage front-end). On I variants, `rbamp_read_power` / `_power_factor`
  return `0.0f`; fleet aggregation of total-power / energy correctly
  sums and ignores the zero contributions.
- **CT-binding is pure-staging** (v1.3 canon): writing `REG_CT_MODEL` does NOT
  apply the value automatically; binding happens through the per-channel
  `CMD_SET_CT_MODEL_CHn`. The binding order is arbitrary
  (the "ascending/descending order" anachronisms from pre-v1.3 have been removed).
- **Address change is production-OK**: two-phase commit + magic + reset
  works in production (no develop-mode required).
- **Master wall-clock = canonical timebase for Wh**: the chip software
  timer `PERIOD_LATCH_MS (0xEC)` is diagnostic-only and may underreport
  by up to 25-30% under load (SysTick starvation by design). The master
  uses its own `esp_timer_get_time()` or wall-clock for billing.

### Documentation — new / reworked chapters

- `01_overview` — lead reworked to be multi-module-first; added
  an ASCII diagram of the 80% scenario.
- `03_sensor_selection` — sweep on CT order; new section
  "Multi-channel modules — I2 and I3 (current-only)" with a
  UI1+I3 disaggregation pattern.
- `04_hardware` — the section "Multi-module bus — primary topology"
  promoted to the main operating mode; explicit requirement for external ~4.7 kΩ
  pull-ups on the fleet bus; provisioning workflow via
  `rbamp_provision`.
- `09_api_reference` — added sections: "Identity and
  provisioning", "Error model v1.3", "Multi-channel configuration",
  "Fleet manager — managing multiple modules", "Per-device
  fleet config".
- `10_troubleshooting` — added sections "IDF i2c_master hangs
  on a marginal bus — three-layer mitigation" (with an HW-validated
  hang-rate table), "Fleet: a module ended up excluded", "Fleet:
  rbamp_fleet_scan returned INVALID_STATE", "Fleet: rbamp_provision
  returned an error".
- `11_changelog` — this file.
