# 01 · Overview

## What rbAmp is

**rbAmp** is a compact hardware module for precision measurement of
AC-mains parameters over the I²C interface. It is built around a
Cortex M0+ microcontroller with an integrated isolated analog front
end and factory calibration stored in flash.

From an integrator's point of view, the module behaves like a standard
I²C slave: apply power, open the bus with `i2c_new_master_bus()`,
create a handle with `rbamp_new()` — and you get ready-to-use values in
physical units through the `rbamp_read_*()` functions. No master-side
signal processing is required.

## What rbAmp measures

| Quantity | Type | Range |
|---|---|---|
| RMS voltage U_rms | float, V | 0…300 V |
| Peak voltage U_peak | float, V | 0…450 V |
| RMS current I_rms | float, A | depends on the selected sensor — see [03_sensor_selection.md](03_sensor_selection.md) |
| Peak current I_peak | float, A | depends on the selected sensor |
| Active power P (signed in RT) | float, W | depends on the selected sensor |
| Power factor PF | float | −1…+1 |
| Mains frequency | uint8, Hz | 45…65 Hz |
| Period-averaged P | float, W | same as P |

> **The RT active-power value `P` is always signed on every module
> tier** (negative means export to the grid). The behavior of the
> period-averaged value (`avg_p[ch]` in `rbamp_period_snapshot_t`)
> **depends on the module tier** — see the "Module tiers" section below and
> [02_tiers.md](02_tiers.md).

## Connection

The module connects to the master with four wires on the LV side.
The mains side is already routed inside the enclosure and is
galvanically isolated.

| Wire | Purpose |
|---|---|
| `VCC` | **+5 V (4.5..5.5 V)**, ~15 mA current, ~25 mA peak. On-board regulator and filtering |
| `GND` | common with the master (**mandatory**) |
| `SDA`, `SCL` | I²C, **3.3 V logic, 5 V-tolerant**. Built-in 4.7 kΩ pull-ups to 3.3 V |
| `DRDY` | optional, open-drain LOW for ~10 µs every ~200 ms |

The default address is `0x50` (7-bit, changeable in the `0x08..0x77`
range), with a recommended speed of 50 kHz on ESP32 (see the section on
retry + sanity discipline below). The cold start until the first valid
measurement takes ~250 ms.

The full rules (bus length, pull-up recommendations on a multi-module
bus, wiring diagrams for the various ESP32 targets) are in
chapter [04 · Hardware connection](04_hardware.md).

## Module tiers (quick cheat sheet)

In the current firmware (v1.2) only the **BASIC** tier is implemented —
one-directional (consumption-only) metering following the logic of a
classic mechanical meter:

| Tier | RT power `P` | Per-period accumulator `avg_p[ch]` | Component Wh counter |
|---|---|---|---|
| **BASIC** | signed (negative = export to the grid, visible in real time) | each **200 ms window** of the average P is clamped to `max(P, 0)` before being added to the period accumulator | **monotonic** — meters consumption only |
| **STANDARD** | *planned for v1.3+* | *planned* — a separate export accumulator | *planned* — bidirectional metering |
| **PRO** | *planned for v1.3+* | *planned* + diagnostics | *planned* + additional counters |

Details are in chapter [02 · Module tiers](02_tiers.md). At the component
level the same `rbamp_handle_t` handle is used regardless of tier; the
differences show up in the values the module returns.

> **Bidirectional metering on BASIC**: `rbamp_energy_wh(dev, ch)` on the
> BASIC tier reports consumption only. If you need to account for export
> separately, sample the RT power with `rbamp_read_power(dev, 0, &p)` at
> ~5 Hz and split the positive and negative samples into two
> accumulator variables yourself. See the example in
> [06_examples.md](06_examples.md) (the BidirectionalEnergy scenario).

## What this component does

Its main job is to hide the protocol exchange and relieve the user
application of the chore of dealing with registers, byte order, and
the post-command settle times. In addition, the component **computes
energy (Wh)** — the module returns only the period-averaged power, and
the master performs the integration using `esp_timer_get_time()`.

### Without the component (direct access via `i2c_master_transmit_receive`)

```c
// Read U_RMS — collect 4 bytes over 4 transactions
// (SPEC §6 — no auto-increment).
i2c_master_dev_handle_t slave;
i2c_master_bus_add_device(bus, &(i2c_device_config_t){
    .device_address = 0x50, .scl_speed_hz = 50000,
}, &slave);

uint8_t buf[4] = { 0 };
for (int i = 0; i < 4; i++) {
    uint8_t reg = 0x86 + i;
    if (i2c_master_transmit_receive(slave, &reg, 1, &buf[i], 1,
                                    /*xfer_timeout_ms=*/100) != ESP_OK) {
        return;   // handle the error
    }
}
float u_rms;
memcpy(&u_rms, buf, sizeof(float));
```

### With the component

```c
float u_rms;
ESP_ERROR_CHECK(rbamp_read_voltage(dev, 0, &u_rms));
```

### Reading a period — without the component

```c
// Latch + 50 ms settle + ready-flag check + assembling the 4 avg_p bytes
uint8_t latch_cmd[2] = { 0x01, 0x27 };   // REG_COMMAND, CMD_LATCH_PERIOD
ESP_ERROR_CHECK(i2c_master_transmit(slave, latch_cmd, 2, /*xfer_timeout_ms=*/100));
int64_t t_latch_us = esp_timer_get_time();
vTaskDelay(pdMS_TO_TICKS(50));   // settle

uint8_t valid_reg = 0x07, valid_byte = 0;
i2c_master_transmit_receive(slave, &valid_reg, 1, &valid_byte, 1, /*xfer_timeout_ms=*/100);
if ((valid_byte & 0x01) == 0) {
    // the snapshot is stale — t_latch_us must STILL be recorded,
    // otherwise the next snapshot will count Wh over two periods
    return;
}

// ...read the 4 avg_p bytes, assemble the float, integrate Wh:
//   double dt_s = (esp_timer_get_time() - t_prev_latch_us) / 1e6;
//   E_Wh += avg_p * dt_s / 3600.0;
```

### Reading a period — with the component

```c
rbamp_period_snapshot_t snap;
if (rbamp_read_period_snapshot(dev, &snap, 50, false) == ESP_OK && snap.valid) {
    // snap.avg_p[0] is filled in,
    // rbamp_energy_wh(dev, 0) has increased by avg_p × master_dt_ms/1000/3600,
    // master_dt_ms is accounted for and saved for the next cycle.
}
```

Internally the component guarantees:

- A 50 ms settle after `CMD_LATCH_PERIOD` (configurable through the
  `settle_ms` parameter), and a correct snapshot read (an atomic latch
  on the module side).
- A check of the `REG_V03_PERIOD_VALID` flag before reading (the
  `snap.valid` field).
- Time-stamping against the master clock even on a stale snapshot
  (protects against double-counting Wh on the next period).
- Per-channel Wh accumulation with double precision (the `double` type
  inside the handle).
- Automatic loading of calibration coefficients when the current sensor
  is selected via `rbamp_set_sensor_class()` + `rbamp_set_ct_model()` —
  the user does not need to know about the internal calibration registers.

## When to use the component, when to access the bus directly

| Task | Component | Direct access via `i2c_master_*` |
|---|---|---|
| Reading U / I / P / PF / frequency | ✅ | only for debugging on a logic analyzer |
| Per-period energy metering | ✅ | only if Wh is stored outside the component (e.g., in a database on the master side) |
| Several modules on one bus | ✅ (sequential LATCH + shared settle, see [06_examples.md](06_examples.md)) | — |
| Bidirectional metering on BASIC | ✅ (via RT sampling — see [06_examples.md](06_examples.md)) | alternatively — your own RT-read loop + manual integration |
| Porting to another platform | n/a | ✅ — see [`rbamp-spec`](https://github.com/rb-amp/rbamp-spec) |
| Hard RAM limit | rarely matters (~85 bytes per handle) | ✅ if even those ~85 bytes are critical |

For all typical tasks in the first rows, the component is the right fit.

## Architecture

```text
┌─────────────────────────────────────────────────────┐
│  User application (app_main, FreeRTOS)              │
│   rbamp_new(bus, 0x50, &dev);                       │
│   rbamp_begin(dev);                                 │
│   rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013); │
│   rbamp_set_ct_model(dev, 3);                       │
│   rbamp_read_period_snapshot(dev, &snap, 50, false);│
│   rbamp_energy_wh(dev, 0);                          │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  Public component API (rbamp.h)                     │
│   ┌─────────────────────────────────────────────┐   │
│   │ Lifecycle: rbamp_new / rbamp_del /          │   │
│   │   rbamp_begin / rbamp_probe / rbamp_wait_ready │ │
│   │ RT reads:     rbamp_read_voltage /          │   │
│   │   rbamp_read_power(ch) / rbamp_read_all(&s) │   │
│   │ Period:       rbamp_latch_period /          │   │
│   │   rbamp_read_period_snapshot                │   │
│   │ Configuration: rbamp_set_sensor_class /     │   │
│   │   rbamp_set_ct_model / rbamp_set_ct_model_ch│   │
│   │ Multi-module: rbamp_broadcast_latch         │   │
│   │ Diagnostics:  rbamp_err_to_str(esp_err_t)   │   │
│   └─────────────────────────────────────────────┘   │
│                       │                             │
│                       ▼                             │
│   ┌─────────────────────────────────────────────┐   │
│   │ Internal layer (src/rbamp.c):               │   │
│   │  read_u8 / read_u32_le / read_float_le /    │   │
│   │  write_reg / write_cmd                      │   │
│   │  retry + sanity discipline                  │   │
│   └─────────────────────────────────────────────┘   │
│                       │                             │
│                       ▼                             │
│   ┌─────────────────────────────────────────────┐   │
│   │ Handle state (struct rbamp_obj_t):          │   │
│   │  • Wh accumulator (double[3], compile-out)  │   │
│   │  • topology, channels, has_voltage_hw       │   │
│   │  • last_latch_us (master_dt_ms tracking)    │   │
│   └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  ESP-IDF i2c_master driver (driver/i2c_master.h)    │
│   i2c_master_transmit / transmit_receive            │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  rbAmp module at address 0x50                       │
│   • U / I / P / PF measurement pipeline (~200 ms)   │
│   • Period accumulator (atomic CMD_LATCH_PERIOD)    │
└─────────────────────────────────────────────────────┘
```

## Logging and Kconfig

Unlike the Arduino library, where the log stream is configured at
runtime (`setLogStream(&Serial)`), here the standard ESP-IDF pattern is
used: the component logs under the `"rbamp"` tag via `esp_log`. **The
log level is compiled in** through the Kconfig choice
`CONFIG_RBAMP_LOG_LEVEL_*` (NONE / ERROR / WARN / INFO / DEBUG, INFO by
default). At `NONE`, log strings are removed from the binary entirely.

The runtime filter also works (via `esp_log_level_set("rbamp", ...)`),
but it is capped from above by the compile-time level.

Six `CONFIG_RBAMP_*` symbols are available in `idf.py menuconfig` →
**Component config → rbAmp client**:

| Symbol | Purpose | Default |
|---|---|---|
| `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ` | SCL frequency for the shared bus | 50000 (50 kHz) |
| `CONFIG_RBAMP_I2C_TIMEOUT_MS` | transmit/receive timeout | 100 ms |
| `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS` | retry attempts per single-byte read | 3 |
| `CONFIG_RBAMP_NACK_RETRY_GAP_MS` | gap between retry attempts | 5 ms |
| `CONFIG_RBAMP_DISABLE_ENERGY` | remove the Wh accumulator from the build | `n` |
| `CONFIG_RBAMP_LOG_LEVEL_*` | compile-time log-strings level | INFO |

For more details, see [09 · API reference](09_api_reference.md), the
"Compile-time configuration" section.

## Interaction diagrams

### The `rbamp_begin()` flow

```text
app           rbamp                i2c_master              rbAmp slave
  │             │                    │                       │
  ├─rbamp_begin(dev)►                │                       │
  │             ├─read_u8(REG_VERSION)►                      │
  │             │                    ├─tx_rx([0x03], 1) ────►│
  │             │                    │◄────────── 0x03 ──────┤  (v1.2)
  │             │                    │                       │
  │             ├─read_float_le(U_RMS)►(4 single-byte tx_rx) │
  │             │  → 226.3 V         │                       │
  │             │  → has_voltage_hw = true                   │
  │             │                    │                       │
  │             ├─write_cmd(LATCH)──►│                       │
  │             │                    ├─tx([0x01, 0x27]) ────►│
  │             │  vTaskDelay(50ms)  │                       │
  │             │                    │                       │
  │◄───ESP_OK───┤                    │                       │
```

The first latch is a primer (the module returns what has been
accumulated since power-up, which is unsuitable for tariff metering).
The component itself records that this snapshot must be discarded — the
user code never sees it.

### The `rbamp_read_period_snapshot(dev, &snap, 50, false)` flow

```text
app           rbamp                i2c_master              rbAmp slave
  │             │                    │                       │
  ├─read_period_snapshot(snap)►      │                       │
  │             ├─write_cmd(LATCH)──►│  t_now = esp_timer_get_time()
  │             │                    │                       │
  │             │  vTaskDelay(50ms)  │                       │
  │             │                    │                       │
  │             ├─read_u8(PERIOD_VALID)►│                    │
  │             │  → bit0 = 1        │                       │
  │             │                    │                       │
  │             ├─read_float_le(AVG_P0)►(4 tx_rx)            │
  │             ├─read_float_le(MAX_P0)►(4 tx_rx)            │
  │             ├─read_u32_le(LATCH_MS)►(4 tx_rx)            │
  │             │                    │                       │
  │             │  master_dt_ms = (t_now - last_latch_us)/1000│
  │             │  energy_wh[ch] += avg_p[ch]*master_dt_ms/3.6e6│
  │             │  last_latch_us = t_now                     │
  │             │                    │                       │
  │◄───ESP_OK───┤                    │                       │
```

The atomic latch on the module side guarantees that every ADC
micro-sample within a period lands in exactly one snapshot — with no
duplication and no losses at period boundaries.

## Mapping to the direct register API

A table for those migrating from their own direct-access code:

| Direct API (`i2c_master_transmit_receive`) | Component equivalent |
|---|---|
| Manual 4-byte read + `memcpy` | `rbamp_read_voltage(dev, 0, &v)` / `rbamp_read_power(dev, ch, &p)` |
| `tx([0x01, 0x27]); vTaskDelay(50)` | `rbamp_latch_period(dev); vTaskDelay(pdMS_TO_TICKS(50))` (or directly `rbamp_read_period_snapshot()`) |
| Checking `read_u8(0x07) & 1` | the `snap.valid` field after `rbamp_read_period_snapshot()` |
| Manual formula `E_Wh += avg_p * dt_s / 3600` | `rbamp_energy_wh(dev, 0)` — updated automatically |
| Manual current-sensor configuration via the direct register API | `rbamp_set_sensor_class(dev, cls)` + `rbamp_set_ct_model(dev, code)` — factory presets are loaded automatically |

## What's next

- [02 · Module tiers](02_tiers.md) — which tier suits which task
- [04 · Hardware connection](04_hardware.md) — pull-ups, bus length, ESP32-family pin-out
- [05 · Quickstart](05_quickstart.md) — your first working project
- [06 · Examples](06_examples.md) — a walkthrough of the ready-made IDF projects
- [09 · API reference](09_api_reference.md) — every public function
- [10 · Troubleshooting](10_troubleshooting.md) — when something doesn't work


---

[Contents](README.md) | [Tier Support →](02_tiers.md)
