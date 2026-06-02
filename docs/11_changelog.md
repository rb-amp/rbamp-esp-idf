# 11 · Changelog

Release notes for the `rbamp` ESP-IDF component. The component's SemVer
is **independent** of the rbAmp protocol version — a single component
version may support several module firmware versions (through
version-gated guards).

## v1.2.0 — 2026-05-30 (public API surface expansion)

A minor release — additive extensions to the component's public surface
with no change to existing behavior. All three additions bring the
ESP-IDF component into cross-platform alignment with the Arduino library.

### Added

- **`RBAMP_SETTLE_MS_*` promoted to the public header** —
  seven settle-time constants are now exported for raw-API users
  (`RBAMP_SETTLE_MS_LATCH_PERIOD`,
  `_RESET`, `_SAVE_GAINS`, `_FACTORY_RESET`,
  `_SET_CT_MODEL_CH0` / `CH1` / `CH2`). Their source is the
  `@defgroup rbamp_settle_ms` block in `include/rbamp.h`. The
  component's wrapper functions hold the correct delays internally; the
  constants are only needed for raw bus access.
- **`rbamp_retry_exhaustion_count(dev)` + `rbamp_sanity_reject_count(dev)`** —
  public getters for the two diagnostic counters. They are monotonic
  (never reset outside an `rbamp_del` + `rbamp_new` cycle).
  A cross-platform mirror of the Arduino `dev.retryExhaustionCount()` /
  `dev.sanityRejectCount()`. For the canonical snapshot+diff pattern,
  see [10 · Troubleshooting](10_troubleshooting.md), the
  "Monitoring the counters" section.
- **`rbamp_set_ct_model_ch` ascending-order detection** — the component
  now tracks the order of per-channel calls through an internal tracker
  (`ct_model_seq_last_ch` + `ct_model_seq_active`). When it detects
  ascending order instead of the permitted descending order, it
  returns `ESP_ERR_INVALID_STATE` with an explicit ESP_LOGW string
  identifying the cause (see
  [10 · Troubleshooting](10_troubleshooting.md), the section
  "`rbamp_set_sensor_class` / `rbamp_set_ct_model*` returns
  `ESP_ERR_INVALID_STATE`"). `rbamp_set_sensor_class()` resets the
  batch tracker, which lets you start from any channel after re-setting
  the class.
- **`rbamp_err_to_str(ESP_ERR_INVALID_STATE)` updated** to the new
  umbrella meaning: *"Wrong call sequence (check log: develop mode /
  sensor class UNSET / CT model ascending order)"*. Disambiguation
  happens through the log string printed under the `"rbamp"` tag
  immediately before the return — a standard ESP-IDF idiom (cf.
  `esp_driver_i2c` with `ESP_ERR_TIMEOUT`).

### Changed

- `idf_component.yml`: `version: "1.1.0"` → `"1.2.0"`.
- `CMakeLists.txt` install slug: `rbamp/rbamp^1.1` → `^1.2`.
- README + Quickstart install commands updated to `^1.2`.

### Compatibility

Fully backward-compatible with v1.0.0 and v1.1.0 — the new APIs are
additive and the existing methods do not change behavior. v1.0/v1.1
users can upgrade with no code changes (the new getters are called only
if the application uses them).

## v1.1.0 — 2026-05-29 (v1.2 firmware parity)

A minor release — additive extensions to the public API for v1.2
firmware. Backward-compatible with v1.1.0 and v1.0.0 — nothing is
broken, new methods are added. Existing v1.0.0 users can upgrade with no
code changes (the new methods are called only if the application uses
them).

### Added

- **`rbamp_set_sensor_class(dev, cls)`** — sets the current-sensor
  class. Mandatory on v1.2+ firmware BEFORE `rbamp_set_ct_model*()`. On
  v1.0 / v1.1 it is a no-op and returns `ESP_OK` (backward compat).
- **`rbamp_set_ct_model_ch(dev, channel, code)`** — per-channel
  selection of the SCT-013 model on v1.2+. It uses the opcodes
  `CMD_SET_CT_MODEL_CH0/1/2`. On v1.0 / v1.1 it returns
  `ESP_ERR_NOT_SUPPORTED`. It enforces the descending-order convention
  for multi-channel scenarios (see
  [03 · Current sensor selection](03_sensor_selection.md)).
- **`rbamp_sensor_class_t` enum** — `RBAMP_SENSOR_UNSET=0`,
  `RBAMP_SENSOR_SCT013=1`, `RBAMP_SENSOR_WIRED_CT=2` (reserved),
  `RBAMP_SENSOR_BUILTIN_CT=3` (reserved).
- **`@warning` Doxygen blocks** in `rbamp.h` for public-with-warning
  methods (`rbamp_factory_reset`, `rbamp_prepare_address_change`,
  `rbamp_commit_address_change`, `rbamp_save_gains`) — verbatim from
  perimeter §7.
- **`rbamp_set_ct_model(dev, code)` guard on v1.2+ firmware**:
  returns `ESP_ERR_INVALID_STATE` if `rbamp_set_sensor_class` was not
  called first. On v1.0 / v1.1 the guard is skipped (the legacy path is
  preserved).

### Changed

- `idf_component.yml`: `version: "1.0.0"` → `"1.1.0"`.

### Example-sketch package

- **`examples/multi_module/`** rewritten to the canonical pattern of
  sequential per-device LATCH + shared settle (`rbamp_broadcast_latch`
  is still reserved for v2 firmware and returns `ESP_ERR_NOT_SUPPORTED` —
  the header comment was updated).
- **`examples/deep_sleep_logger/`** rewritten to the canonical pattern
  with `RTC_DATA_ATTR uint64_t rtc_last_wake_us` + `skip_latch=true`
  reads. Mirrors the Arduino library's F-4 fix (cross-platform
  consistency).
- **`examples/spiffs_logger/`** now ships with a real
  `partitions.csv` + `sdkconfig.defaults` — the example is runnable
  out-of-the-box via `idf.py build flash monitor`.

### Build

All 7 examples pass `idf.py build` clean on the `esp32` target with
ESP-IDF v5.4.1. `--warnings all` with no warnings.

## v1.0.0 — 2026-05-25 (first public release)

The first public release of the `rbamp` ESP-IDF component. It
implements the canonical rbAmp API for **protocol v1.0** with
forward-readiness to v1.1.

### Features

**Opaque `rbamp_handle_t`** — created via `rbamp_new()` /
`rbamp_new_with_topology()`, freed by `rbamp_del()`.

**`esp_err_t` returns** — uniform with the rest of ESP-IDF;
`ESP_ERROR_CHECK()` or manual handling via `rbamp_err_to_str(err)`.

**Full rbAmp v1.0 API:**

- **Lifecycle**: `rbamp_new`, `rbamp_new_with_topology`,
  `rbamp_del`, `rbamp_begin`, `rbamp_probe`, `rbamp_wait_ready`,
  `rbamp_firmware_version`, `rbamp_topology`, `rbamp_channels`,
  `rbamp_has_voltage_hw`, `rbamp_address`
- **RT reads** (200 ms refresh): `rbamp_read_voltage`,
  `rbamp_read_voltage_peak`, `rbamp_read_current`,
  `rbamp_read_current_peak`, `rbamp_read_power`,
  `rbamp_read_power_factor`, `rbamp_read_frequency`,
  `rbamp_read_all`
- **Per-period metering**: `rbamp_latch_period`, `rbamp_is_period_valid`,
  `rbamp_read_period_avg_power`, `rbamp_read_period_max_power`,
  `rbamp_read_period_latch_ms`, `rbamp_read_period_snapshot`,
  `rbamp_read_period_snapshot_simple`
- **Energy metering (Wh, master-side)**: `rbamp_energy_wh`,
  `rbamp_energy_reset`, `rbamp_energy_reset_all`,
  `rbamp_energy_disable`, `rbamp_energy_enable`. Compile-out
  via `CONFIG_RBAMP_DISABLE_ENERGY=y`.
- **Sensor configuration**: `rbamp_set_ct_model(dev, code)` for
  channel 0 (the legacy single-arg form)
- **Public-with-warning** (factory / integrator territory):
  `rbamp_save_gains`, `rbamp_prepare_address_change`,
  `rbamp_commit_address_change`, `rbamp_factory_reset`, `rbamp_reset`
- **Multi-module bus**: `rbamp_broadcast_latch(bus, timeout_ms)`
  — returns `ESP_ERR_NOT_SUPPORTED` on v1 firmware (reserved for v2)
- **Diagnostics**: `rbamp_err_to_str(err)` (the only public
  diagnostic function; the internal retry/sanity counters are not
  exposed in the API)

**POD structs:** `rbamp_snapshot_t` (one shot of the RT block via
`rbamp_read_all`), `rbamp_period_snapshot_t` (a latched period snapshot).

**Enum:** `rbamp_topology_t` — `SINGLE` / `SPLIT_PHASE` / `THREE_PHASE`.

**Retry + sanity discipline** are built in:

- Per-byte retry on ESP32 (3 attempts × 5 ms gap by default, via
  Kconfig)
- A loose-sanity filter on float reads (`!isfinite(x) || |x| > 10000`)
- 50 kHz by default via Kconfig to mitigate the NACK pattern with the
  ESP-IDF v5 `i2c_master` driver

**Logging via `esp_log`** under the `"rbamp"` tag with a
compile-time strip via `CONFIG_RBAMP_LOG_LEVEL_*` (NONE / ERROR /
WARN / INFO / DEBUG).

**Kconfig** — 6 symbols in `idf.py menuconfig` → **Component config
→ rbAmp client**:

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ` | 50000 | SCL frequency |
| `CONFIG_RBAMP_I2C_TIMEOUT_MS` | 100 | transmit/receive timeout |
| `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS` | 3 | retry attempts per single-byte read |
| `CONFIG_RBAMP_NACK_RETRY_GAP_MS` | 5 | gap between retry attempts |
| `CONFIG_RBAMP_DISABLE_ENERGY` | `n` | remove the Wh accumulator from the build |
| `CONFIG_RBAMP_LOG_LEVEL_*` | INFO | compile-time log-strings level |

### Supported target platforms

Per `idf_component.yml`: **all 8 chips of the ESP32 family** (ESP32,
ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C3, ESP32-C6, ESP32-H2,
ESP32-P4). Minimum IDF version: **v5.2** (requires
`driver/i2c_master.h`).

The ESP8266 RTOS SDK is not supported — for ESP8266 use the rbAmp
Arduino library via `arduino-esp8266`.

### Examples (7 ready-made IDF projects)

- `examples/quick_read/` — smoke test (U / I / P / PF once a second)
- `examples/lcd_period/` — a 60-second Wh meter on a 16×2 LCD (PCF8574 + HD44780)
- `examples/multi_module/` — 3 modules on one bus
- `examples/mqtt_publisher/` — an `esp_mqtt_client_*` publisher
- `examples/ha_discovery/` — Home Assistant MQTT Auto-discovery
- `examples/spiffs_logger/` — per-minute CSV append to SPIFFS
- `examples/deep_sleep_logger/` — Wh persistence across deep-sleep using RTC memory

### Documentation

11 reference chapters in [`docs/`](.) cover: overview, tiers, sensor
selection, hardware connection, quickstart, examples, DIY and cloud
integrations, the API reference, and troubleshooting.

### Known limitations

Deliberate omissions from v1.0 — tracked for future minor releases:

- `rbamp_broadcast_latch()` always returns `ESP_ERR_NOT_SUPPORTED`
  (General-Call is disabled in the I²C peripheral of the v1 module).
  Once the module firmware enables GC, the function will start working
  with no API change.
- Reactive power is not read — it is a STANDARD / PRO tier feature, not
  exposed in protocol v1.x.
- Dimmer control is not implemented — it is out of scope for v1 (see the
  future companion `rbamp_dimmer` component).
- Deep sleep on ESP32 requires the canonical pattern with
  `skip_latch=true` (see [06 · Examples](06_examples.md), Scenario 9) —
  a simplified `rbamp_warm_begin()` will arrive in v1.2+ once the
  firmware adds the corresponding accessor.

### Firmware compatibility matrix

| Component version | Firmware version | Behavior |
|---|---|---|
| 1.0 | 1.0 | The constructor topology hint is used. Fully functional. |
| 1.0 | 1.1 | The constructor hint is used; `REG_TOPOLOGY` is ignored. Identical to 1.0/1.0. |
| 1.0 | 1.2 | Per-channel `rbamp_set_ct_model_ch` is absent (v1.0 does not know about the new opcodes). The single-arg `rbamp_set_ct_model` works (legacy path). |
| 1.1 | 1.0 | `rbamp_set_sensor_class` — accepted, but the firmware does not use the register (the write is "harmless"; the call itself still blocks for ~705 ms on `CMD_SAVE_GAINS`). `rbamp_set_ct_model_ch` → `ESP_ERR_NOT_SUPPORTED`. The single-arg `rbamp_set_ct_model` works (legacy path). `rbamp_read_power` is **unsigned** (always ≥ 0). |
| 1.1 | 1.1 | Same as 1.1/1.0; `REG_TOPOLOGY` is ignored by the v1.1 component. |
| 1.1 | 1.2 | Per-channel `rbamp_set_ct_model_ch` works. `rbamp_set_sensor_class` is **mandatory** before `rbamp_set_ct_model` AND `rbamp_set_ct_model_ch` — otherwise both functions return `ESP_ERR_INVALID_STATE`. `rbamp_read_power(ch)` becomes **signed** (negative = export), and `rbamp_energy_wh(ch)` is signed too. |
| 1.2+ (planned) | 1.x | Extended diagnostics, `rbamp_warm_begin()` for deep sleep, additional accessors. |

### Bench validation

The v1.0.0 release passed regression bench acceptance: the component is
installed on an ESP32 target platform with a real DUT module and meets
the criteria:

- **`idf.py build` clean** on all 7 examples (`--warnings all`,
  0 errors, 0 warnings)
- **`rbamp_probe` answers `ESP_OK` within ≤ 100 ms**
- **`rbamp_read_all` matches a reference meter to ±2 %** on a purely
  resistive load
- **`rbamp_read_period_snapshot` `valid=true`** + `master_dt_ms`
  matches `esp_timer_get_time()` to ±1 ms
- **Sequential multi-module LATCH skew < 0.2 %** on a 100 kHz bus

The specific accuracy numbers (V / I / P / PF against a calibrated
reference) will be published once the bench-measurement program is
complete (the IP-001 + IP-010 program — for a discussion of the dual-CT
pattern and operation at low currents, see
[03 · Current sensor selection](03_sensor_selection.md)).

## Future releases — planned

### v1.1.x (patch — bug fixes only)

- TBD based on user reports.

### v1.2.0 (minor — additive, after firmware v1.1 / v1.3 ships)

- **`rbamp_begin()` reads `REG_TOPOLOGY`** when
  `rbamp_firmware_version(dev) >= 0x02` and uses it as authoritative
  (the constructor hint becomes a fallback).
- **`rbamp_warm_begin(dev)`** — lightweight initialization without the
  CMD_LATCH_PERIOD primer, for deep-sleep wake scenarios (it simplifies
  the pattern in [06 · Examples](06_examples.md), Scenario 9 — it
  removes the need for `skip_latch=true` on a warm wake).
- **Per-channel polarity-invert config flag** — an accessor to correct
  the CT clamp orientation without physically reinstalling it. Until it
  ships, the workaround is to invert the sign on the master side:
  `p = -p; pf = -pf;` (see
  [10 · Troubleshooting](10_troubleshooting.md), the section
  "PF stays strictly −1.0 on a purely consumptive load").
- **STANDARD / PRO tiers via firmware** — `rbamp_energy_wh(dev, ch)`
  will start returning a **signed** accumulator with no need for the
  master-side consume/export split from the current Scenario 5.
- Additionally: `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=1` will become the
  default on ESP32 if firmware ≥ v0x02 is detected in `rbamp_begin()`
  (the slave-side NACK fix removes the need for retry).

### v2.0.0 (major — breaking, after firmware v2 ships)

- `rbamp_broadcast_latch()` actually transmits over I²C via
  General-Call (once firmware v2 enables GC).
- Reactive power register: `rbamp_read_reactive_power(dev, ch, &q)`.
- Dimmer control in a companion component: `rbamp_dimmer_*`.
- A possible non-breaking optimization: switching single-byte reads to
  burst reads of a contiguous float block for a ~4× speedup — the
  public API is preserved.

## ESP Component Registry

Published on `components.espressif.com` under the ID `rbamp/rbamp`.
Install via:

```sh
idf.py add-dependency "rbamp/rbamp^1.2"
```

or manually as a git submodule in `components/rbamp/`.

## Bug reports + contributing

Open an issue:
[github.com/rb-amp/rbamp-esp-idf/issues](https://github.com/rb-amp/rbamp-esp-idf/issues)

Include in the issue the diagnostic set from [10 · Troubleshooting](10_troubleshooting.md),
the section "When to contact support".

Pull requests are welcome — the component is pure C99 with
ESP-IDF-standard dependencies (`esp_driver_i2c`, `esp_timer`,
`freertos`, `esp_log`). Before submitting, run `idf.py build` on all 7
examples against a real DUT (the PR template will walk you through the
steps).

## License

MIT — see [LICENSE](../LICENSE).


---

[← Troubleshooting](10_troubleshooting.md) | [Contents](README.md)
