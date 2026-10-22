# 11 · Changelog

This chapter mirrors [`CHANGELOG.md`](https://github.com/rb-amp/rbamp-esp-idf/blob/main/CHANGELOG.md)
in the repository root. Public releases resume with version 1.7.2;
intermediate versions were released internally only.

## 1.7.2 — SCT-013 rating correction, resume after master downtime, one-transaction window reads (2026-10-21)

First public release since 1.5.0. It carries 1.6.0 – 1.7.2 together: the STD
module line behind the same handle, the data-ready line, events, µWh energy
and phase quantities, a channel count that follows the module (1, 2, 3, 5 or
7), the sensor-model registry with clip limits, one-transaction window reads,
resume after a master outage, and the SCT-013 rating correction for current-
line modules whose firmware keeps the 30 A calibration (firmware reporting
the self-correction capability corrects itself). Hardware validation of this line was done
on a host running ESP-IDF 5.4.1.

### Added
- SCT-013-060 and -100 are accepted (they were refused client-side as reserved); sensor-model clip limits updated to measured values.
- SCT-013 rating switch: a current-sensor
  module of class 1 is calibrated with the 30 A sensor, and firmware up to
  1.4.19 keeps that calibration after the user binds another model. The
  library now reads the gain the module applies at `rbamp_begin()`, after a
  model write and after every detected module restart, and scales current,
  peak current, real power, period average power, the energy it integrates,
  the digest and the event thresholds to the selected model. Voltage and power factor are
  not scaled. `rbamp_sct_correction()` reports the outcome per channel
  (`rbamp_sct_correction_t`: status, model, scale, applied gain) and
  `rbamp_sct_refresh()` re-evaluates on demand. A gain still equal to the raw
  preset (model switched in this boot without a restart) triggers one
  CMD_RESET; an unexpected applied gain, an unknown model, or a
  raw preset that survives the reset leave the readings unscaled with a
  warning. A module that reports the self-correction capability
  (firmware 1.4.20 and later) corrects itself and gets scale 1 — detected by
  the capability, never by VERSION.
  `CONFIG_RBAMP_SCT_RATING_CORRECTION` (default on) turns the mechanism off.
- `rbamp_begin_resume()` — `rbamp_begin()` without the primer latch. A BASIC
  module keeps averaging until the next latch, so after a master reboot or
  outage the first consumed snapshot integrates over the module's own window
  and recovers the energy of the downtime. Later snapshots use the master
  clock as before. STD modules behave exactly as with `rbamp_begin()`.
- `rbamp_period_snapshot_t`: `module_window_ms` (the module's own length of
  the latched window, 0 on STD; the module counts commits of 100 ms on
  current firmware and 200 ms on pre-v1.2 firmware — the library tells
  which from the snapshot itself and reports it in `module_commit_ms`),
  `module_uptime_s`, `window_from_boot` (the
  window started at the module's boot: a restart, or the first latch since
  boot) and `window_long` (over 6 h, where the module's average loses
  precision). On the first snapshot after `rbamp_begin_resume()`,
  `module_restarted` = `window_from_boot`.
- `rbamp_read_uptime_s()` — seconds since the module booted (BASIC and STD).
  Reads 0 on BASIC firmware that does not publish it; then
  `window_from_boot` stays false.
- `rbamp_read_digest()` — one window of a BASIC module in a single
  transaction: status, window sequence, and the chosen fields (current,
  voltage, signed real power in whole watts, power factor) for channels 0-2.
  Made for reading every published window on the data-ready line.
  `rbamp_set_digest_fields()` chooses the fields; the library writes the
  choice to the module and writes it again after a detected module restart.
  Needs BASIC firmware with the digest capability; not available on STD.
- `rbamp_basic_fw_name()` — the firmware release name for a BASIC version
  byte ("1.4.19" for 0x0B, and so on).
- `rbamp_read_sensor_class()` — the module's current sensor class.
- `rbamp_sensor_model_limits()` — a sensor model's rated RMS current, the
  RMS current at which a sine starts to clip, and the instantaneous clip
  ceiling, in A, from the generated model registry (0 when not known).
  `rbamp_sensor_model_rated_a()` is the rated-current shorthand; note the
  rating can differ from the amps in the model's name.
- Generated sensor-model registry refreshed with those limits.
- `rbamp_set_ct_model_ch_force()` — service path that sets a channel's model
  regardless of the module's class and current model. It replaces the
  calibration the unit carries for that channel with the model's generic
  preset; follow it with a calibration.

### Changed
- `rbamp_set_ct_model()`, `rbamp_set_ct_model_ch()` and
  `rbamp_configure_channels()` on a class-1 module without
  the self-correction capability (firmware up to 1.4.19) end with a module restart (CMD_RESET, the same UID verified, about a second): the only way the module returns to its calibrated gain after a
  model write. The library re-applies what it owns (primer latch, digest mask
  on next use); the period accumulators restart as after any module reset.
- The transport no longer calls `i2c_master_bus_reset()` on its own before
  the final retry attempt of a failed transaction. The driver resets its
  state machine after every failed transaction already, and the explicit
  reset takes no bus lock: when another task has a transaction in flight it
  drives a STOP on the live pins, which the driver's interrupt handler can
  take for the end of a read and crash in the ISR (seen on a host with a
  hot-plugged module). `CONFIG_RBAMP_BUS_RESET_ON_RETRY_EXHAUSTION` restores
  the old behaviour for a single-task master.
- `rbamp_set_sensor_class()`, `rbamp_set_ct_model()`,
  `rbamp_set_ct_model_ch()` and `rbamp_configure_channels()` read before they
  write. A class or model the module already holds is not rewritten (no bus
  write): on the module a class write clears the CT models even when the
  value is unchanged, and a model write re-applies the generic preset over
  the unit's own calibration even when the model is unchanged, so
  re-applying a stored configuration on every reconnect used to degrade the
  readings. A module of class 2 or 3 (shipped and calibrated together with
  its sensor) is not reconfigured: `ESP_ERR_INVALID_STATE`.
  `rbamp_configure_channels()` decides the whole request before its first
  write, so on such a module nothing is half-applied.
- `latch_ms` documentation: diagnostic only, may read short under load; the
  energy-integration window is `master_dt_ms` or `module_window_ms`.
- Documentation and the Kconfig help now state the module's publish window
  as 100 ms across the family (older images: 200 ms). The 300 ms settle the
  library applies after an STD latch is described as the safe upper bound it
  is. No timing behaviour changed.

### Fixed
- `rbamp_read_all()` / `rbamp_read_all_ready()` on a module with more than
  three channels (UI5/UI7, BASIC or STD) wrote channels 3+ past the
  three-entry arrays of `rbamp_snapshot_t`: `frequency`, `topology` and
  `channels` were overwritten (the host then saw `channels` = 0) and the last
  channel landed four bytes past the struct, on the caller's stack. The call
  now fills ch0-2 only (`RBAMP_SNAPSHOT_CHANNELS`), exactly as the period
  snapshot does; ch3+ stay on `rbamp_read_channel()` /
  `rbamp_read_all_channels()`. Found on a host with a 7-channel module. A host
  simulation of the whole component against a fake register image now runs
  `read_all` for 1/3/5/7 channels with guard words after the struct.

## 1.7.1 — Channel count follows the module (2026-10-18)

### Fixed
- `rbamp_new()` created a handle whose channel count was fixed at three, so a
  single-channel module (UI1, I1) still reported three channels after
  `rbamp_begin()`. The default constructor now adopts the channel count and
  voltage presence from the module's variant register; three channels remain
  only the fallback for firmware that does not publish a variant.
  `rbamp_new_with_topology()` keeps pinning the count as before (a warning is
  logged when the module disagrees). Reported from a host running a UI1;
  validated there: a UI1 module reports one channel and the UI1 variant after
  `rbamp_begin()`.
- `rbamp_read_variant()` on the BASIC line returned UNKNOWN for the UI5 / UI7
  codes; it now maps every known code.

### Added
- `rbamp_variant_t rbamp_variant(rbamp_handle_t dev)` — the variant cached by
  `rbamp_begin()`, without bus traffic. `RBAMP_VARIANT_UNKNOWN` before
  `rbamp_begin()` or when the module does not publish its variant.
- Host unit tests for the variant policy of both constructors
  (`test/host/test_std_decode.c`).

## 1.7.0 — µWh energy, starting current, phase angles and sequence (2026-10-17)

Support for module image version 0x0001007A / 0x0001007B. Every addition is
gated on what the module reports; older modules behave exactly as before.

Validated on STD modules with module image version 0x0001007B on a host
application: starting-current get/set and read-back, phase angles and phase
sequence on a three-phase module, fine energy flags, energy delta across
module restarts.

### Added
- **Energy at µWh / µvarh resolution** in `rbamp_std_energy_t`:
  `act_import_uwh`, `act_export_uwh`, `act_net_uwh`, `rea_import_uvarh`,
  `rea_export_uvarh`, `ch_import_uwh[]`, `ch_export_uwh[]`, plus `fine` /
  `fine_reactive`. The fields are filled on every module: from the module's
  own µ counters when it publishes them (module image version 0x0001007A; reactive from 0x0001007B), otherwise scaled from the coarse
  counters with `fine` false — callers can use the µ fields unconditionally.
  Read as coherent snapshots (one transaction per block). `rbamp_std_energy_delta` applies the same rules to them
  (generation change → no delta, uptime drop = restart, a counter below the
  previous reading → 0, net = import − export). `rbamp_read_period_snapshot`
  on the counter path uses the µWh difference when available.
- **Starting current for energy**: `rbamp_std_get_start_current` /
  `rbamp_std_set_start_current` (µA; 0 = off; a value outside the set the
  module accepts → `ESP_ERR_INVALID_ARG` without a bus write; read back and
  confirmed; `rbamp_save_user_config` persists it). Below this channel
  current the module counts no energy; readings are not affected. Also in `rbamp_std_config_t` (`start_ua`, `start_ua_supported`).
  `ESP_ERR_NOT_SUPPORTED` without bus traffic before module image version 0x0001007B.
- **Voltage phase angle**: `rbamp_std_voltage_t.angle_deg` / `angle_valid`
  (degrees against voltage input 0; module image version 0x0001007A+, three-phase
  modules; otherwise `angle_valid` false). Same single-burst read as before.
- **Phase sequence**: `rbamp_std_read_phase_seq` →
  `rbamp_std_phase_seq_t` (`RBAMP_STD_PHASE_SEQ_NA` / `FORWARD` / `REVERSE`
  / `ABNORMAL`); `ESP_ERR_NOT_SUPPORTED` without bus traffic before
  module image version 0x0001007A.
- Host unit test of the pure decode helpers: `test/host/run.sh` (gcc, no
  ESP-IDF). The energy delta rules now live in `src/rbamp_std_decode.c`.

### Changed
- `rbamp_is_provisioned` (STD line) documents the module image version 0x0001007A
  calibration marker: a unit calibrated under older firmware reads false
  after the update until one `rbamp_save_gains`.

### Upgrade note
- The energy snapshot structure (`rbamp_std_energy_t`) gained fields in this
  version. If your application saves it as a raw binary blob (NVS, file),
  store only the fields you need in your own fixed format; a blob saved by an
  older version will not match the new size.

## 1.6.1 — Command completion, coherent reads, OTA session checks (2026-10-11)

Library-side fixes from the firmware/library audit of 2026-10-11. No new API;
behaviour of existing calls on the STD line is tightened.

### Changed
- **Command completion** is the module's command-state register (busy flag
  cleared and the completed-command counter advanced), never the error
  register alone; the verdict is read only after completion. Saves
  (`rbamp_save_gains`, `rbamp_save_user_config`), `rbamp_std_cal_thd_floor`,
  `rbamp_reset`, address change and every OTA command use it — no fixed
  settle sleeps on modules that publish the register, timeouts of at least
  1 s (3 s for OTA commit). A command written while the module is busy is
  waited out first; a command the module dropped (overrun) fails with
  `ESP_ERR_INVALID_STATE` instead of returning the previous command's verdict.
- **A command write is never retried** on a transport error (a retry could
  deliver the opcode twice). Data-register reads and writes keep the retry.
- **Block reads are one transaction** — the module serves
  each read from a coherent snapshot, so energy counters, channel blocks and
  totals no longer tear across bursts. `rbamp_read_all` on an STD module reads
  the voltage block and each channel block whole instead of field by field.
- **OTA**: after every page the session must still be open (status 1);
  otherwise `ESP_ERR_INVALID_STATE` with the module's reason logged — restart
  from begin. Polls during a flash stall no longer reset the bus.
  `rbamp_std_ota_activate` holds through the module's ~10 s trial window and
  confirms the running image is the staged version and the swap is no longer
  pending. `rbamp_std_ota_write_image` (host-built, unsigned image) returns
  `ESP_ERR_NOT_SUPPORTED` before touching the module — only vendor-prepared
  images are accepted by the module firmware.
- **Address change** (`rbamp_commit_address_change`, STD): the config save is
  synchronous (rate limit → `ESP_ERR_NOT_FINISHED`, retry after ~5 s), and the
  handle switches to the new address only after a module answers there;
  otherwise `ESP_ERR_NOT_FOUND` and the handle keeps the old address.
- `rbamp_std_read_totals`: the energy fields of aggregate blocks read 0 (not
  populated by the module — use `rbamp_std_read_energy`).
- `rbamp_read_frequency` (STD): 0 from the module means no mains →
  `ESP_ERR_INVALID_RESPONSE` / NaN (`rbamp_read_all` flags it implausible).
- `rbamp_is_provisioned` (STD): documented as reliable from module image version 0x00010079 (older images also report calibration valid on default gains).

## 1.6.0 — STD module line, data-ready line, events (2026-10-11)

### Added
- **STD module line** (16-bit register map) behind the same handle and API as the
  original line: `rbamp_std_probe` / `rbamp_std_identify` / `rbamp_std_info`,
  per-channel real-time readings incl. reactive and apparent power
  (`rbamp_read_reactive_power`, `rbamp_read_apparent_power`,
  `rbamp_std_read_channel`, `rbamp_std_read_voltage`), per-phase and grand
  totals (`rbamp_std_read_totals`), configuration and harmonics
  (`rbamp_std_read_config`, `rbamp_std_set_harmonics`, `rbamp_std_cal_thd_floor`).
- **Energy on modules without latch banks**: the period source is chosen from
  the module capabilities at `rbamp_begin` (`rbamp_std_info_t.period_source`);
  period energy is the difference of two readings of the persistent counters,
  with restart and counter-reset rules. `rbamp_std_read_energy` (Wh / varh,
  plus mWh per channel and per group on map 0x36+), `rbamp_std_energy_delta`,
  `rbamp_energy_add_wh`, `RBAMP_ENERGY_CH_TOTAL`. `rbamp_period_snapshot_t`
  gained `avg_p_total`, `wh_net` and `module_restarted`.
- **Latch banks** on modules that have them: `rbamp_std_arm_banks`,
  `rbamp_std_latch`, `rbamp_std_read_latch`, `rbamp_std_latch_wh`.
- **Module clock, history and event log**: `rbamp_std_set_time`,
  `rbamp_std_read_time`, `rbamp_std_read_image_version`, `rbamp_std_hist_info`,
  `rbamp_std_hist_read`, `rbamp_std_hist_read_raw`, `rbamp_std_event_read`,
  `rbamp_std_hist_flush` (hourly ring is persistent; the 15-minute record is
  the last one only).
- **Health**: `rbamp_std_read_health` extended with status bits, lifetime and
  calibration sources, frame counters, uptime, and reset cause + boot count
  (module image version 0x00010076 and newer, `RBAMP_STD_RST_*`).
- **Commands and service**: `rbamp_std_command`, `rbamp_std_wait_idle`,
  `rbamp_std_locate`, `rbamp_std_read_board_id`, `rbamp_std_ota_board_ok`
  (board check before an OTA stream), `rbamp_std_set_address_by_uid`.
- **OTA**: dual-bank image update incl. prepared signed images
  (`rbamp_std_ota_begin` … `rbamp_std_ota_activate`,
  `rbamp_std_ota_write_prepared`, `rbamp_std_ota_write_prepared_pair`,
  `rbamp_std_ota_diag`, `rbamp_std_ota_staged_version`, `rbamp_std_ota_abort`).
- **Certificate and attestation**: `rbamp_std_read_certificate`,
  `rbamp_std_attest` (signature verification is done by the application).
- **Data-ready line (DRDY), both module lines**: `rbamp_set_drdy_gpio`,
  `rbamp_wait_data_ready`, `rbamp_read_all_ready`, `rbamp_read_data_seq`.
  One open-drain line may be shared by several modules; the library verifies
  the publish sequence, counts missed windows, reports latency and detects a
  held-low alarm. Polling fallback via `CONFIG_RBAMP_DRDY_POLL_MS`.
  STD modules need module image version 0x00010075 or newer.
- **Events on both lines**: `rbamp_read_event_mask`, `rbamp_set_event_mask`,
  `rbamp_set_event_thresholds`, `rbamp_begin_event_flags`,
  `RBAMP_EVENT_NVM_FAULT`; `rbamp_read_event_flags` / `rbamp_clear_event_flags`
  / `rbamp_has_error` / `rbamp_clear_error` now cover the STD line.
- Kconfig: `RBAMP_I2C_SCL_WAIT_US` (clock-stretch budget, default 10 ms) and
  `RBAMP_DRDY_POLL_MS`.
- Example `std_meter`.

### Changed
- Period calls on modules without latch banks return `ESP_ERR_NOT_SUPPORTED`
  instead of silently reporting an invalid period.
- `rbamp_set_ct_model_ch` returns `ESP_ERR_NOT_SUPPORTED` on STD modules whose
  channel map is not writable.
- `rbamp_err_to_str` covers `ESP_ERR_NOT_FOUND`, `ESP_ERR_NOT_FINISHED` and
  `ESP_ERR_INVALID_CRC`.
- `rbamp_period_snapshot_t` and `rbamp_std_health_t` gained fields
  (source-compatible; rebuild dependants).
- Component now requires `esp_driver_gpio` (built-in IDF component).

## 1.5.0 — Per-channel CT model on every channel (2026-09-04)

### Added
- `rbamp_set_ct_model_ch` / `rbamp_read_ct_model_ch` / `rbamp_configure_channels`
  accept any channel up to the detected channel count (channels 3+ through the
  channel window).
- Model codes and descriptors come from the generated `rbamp_sensor_models.h`
  (`rbamp_sensor_model_lookup`); unknown codes are rejected by the module and
  surfaced to the caller.

### Fixed
- Sensor class and CT model now persist on production modules: the previous
  save opcode did not persist to flash on production firmware, so the binding
  applied to RAM and was lost on the next reboot. No API change, same ~700 ms
  settle.

## 1.4.0 — Five- and seven-channel modules

### Added
- Topology `FIVE_CH` / `SEVEN_CH`, variants `UI5` / `UI7`,
  `RBAMP_MAX_CHANNELS` = 7; `rbamp_read_channel`, `rbamp_read_all_channels`,
  `rbamp_read_commit_seq`. The period snapshot integrates all channels.

## 1.3.0 (2026-06-16) — Public release

First public release of the `rbamp` component for ESP-IDF v5.x.

### Key features

- **Fleet manager**: manage a group of modules on a single I²C bus
  through a single `rbamp_fleet_t` handle. Discovery with conflict detection
  and Tier-2 wedge canary, MISS-resilient `_poll_all`, aggregation of
  total-power / total-energy / errors, broadcast-latch for
  tight synchrony. See chapter
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
  `rbamp_read_capability` (feature map), `rbamp_has_voltage`,
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
  followed in later firmware.
- **Energy and power are computed only on UI variants** (which have a
  voltage front-end). On I variants, `rbamp_read_power` / `_power_factor`
  return `0.0f`; fleet aggregation of total-power / energy correctly
  sums and ignores the zero contributions.
- **CT-binding is pure-staging** (v1.3 canon): writing `REG_CT_MODEL` does NOT
  apply the value automatically; binding happens through the per-channel
  `CMD_SET_CT_MODEL_CHn`. The binding order is arbitrary
  (the "ascending/descending order" anachronisms from pre-v1.3 have been removed).
- **Address change is production-OK**: two-phase commit + magic + reset
  works in production (no factory-mode required).
- **Master wall-clock = canonical timebase for Wh**: the chip software
  timer `PERIOD_LATCH_MS (0xEC)` is diagnostic-only; do not use it for
  energy. The master uses its own `esp_timer_get_time()` or wall-clock.

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
