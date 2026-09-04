# Changelog

All notable changes to this component are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project adheres to
[Semantic Versioning](https://semver.org/).

## 1.5.0 — Per-channel CT models + sensor-model registry

Adds per-channel CT-model assignment for every channel (including senior-SKU
channels 3+) via the channel window (field 15), and moves the CT-model list off
a hand-maintained table onto the generated registry. Firmware: v1.4.18+.

### Added
- **Per-channel CT model for all channels.** `rbamp_set_ct_model_ch()` /
  `rbamp_read_ct_model_ch()` / `rbamp_configure_channels()` now accept any
  channel up to the module's channel count. Senior channels 3+ are configured
  through the channel window (field 15). Channel index is validated against the
  detected channel count, not a hardcoded 3.
- **Generated sensor-model registry** — `rbamp_sensor_models.h` (from
  `libs/spec/sensor_models.yaml`): model lookup via
  `rbamp_sensor_model_lookup(class, code)`. Unknown codes are rejected by the
  device (`ERR_PARAM`) and surfaced to the caller.
- Channel-window read/write helpers; four per-channel float readers consolidated
  behind one routing point.

### Fixed
- **CT model / sensor class now persist in production.** Prior versions persisted
  sensor class and CT model via `CMD_SAVE_GAINS`, which is factory-gated — on a
  production module the save was a silent no-op, so the binding applied to RAM
  but was lost on the next reboot. These registers are `user_config`, so they now
  persist via `CMD_SAVE_USER_CONFIG` (ungated). No API change; same ~700 ms
  settle. **Users of prior versions in production silently lost the CT model
  binding on every reboot — update before shipping devices.**

## 1.4.0 — Senior SKUs (UI5 / UI7)

Adds support for the 5- and 7-channel senior SKUs (UI5, UI7). Auto-detection
via `REG_TOPOLOGY` / `REG_HW_VARIANT`; all real-time and period readers work
uniformly for channels 0..N-1.

## 1.3.0 — Initial public release

First public release of the rbAmp ESP-IDF client component: real-time metering,
period energy (Wh), multi-channel and multi-module fleet support, provisioning,
and General-Call fleet synchronisation, on the ESP-IDF v5.2+ `i2c_master`
driver.
