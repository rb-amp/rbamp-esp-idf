# 09 · API Reference

Complete public API of the `rbamp` component v1.1.0 for ESP-IDF.
Source — the header file [`include/rbamp.h`](../include/rbamp.h)
with Doxygen comments.

This chapter is a reference: signatures, return values, side
effects, edge cases. For working examples, see
[06 · Examples](06_examples.md); for a quick start, see
[05 · Quickstart](05_quickstart.md).

## Header file

`#include "rbamp.h"` — the only public header. It transitively
pulls in `esp_err.h` for `esp_err_t` and `driver/i2c_master.h` for
the `i2c_master_bus_handle_t` type. No additional includes are
required.

The API is wrapped in an `extern "C"` block — compatible with both
C and C++ callers.

## General idioms

The API follows standard ESP-IDF patterns:

- **Opaque handle**: `rbamp_handle_t` — a typedef for `struct rbamp_obj_t *`;
  the user never reaches into its internals. Created via
  `rbamp_new()`, freed via `rbamp_del()`.
- **`esp_err_t` returns** — `ESP_OK` on success, one of the `ESP_ERR_*`
  codes on failure. Use `ESP_ERROR_CHECK()` for abort-on-error
  handling, or check manually.
- **Output-via-pointer**: functions that return a value take a
  pointer to a buffer as their last parameter. This lets a single
  call return both an `esp_err_t` and a value.
- **Human-readable error strings**: `rbamp_err_to_str(err)`
  returns a pointer to a static string. It never returns `NULL`.

## Types

### `rbamp_handle_t`

```c
typedef struct rbamp_obj_t *rbamp_handle_t;
```

An opaque handle to a slave device. The user has no access to its
fields. Created via `rbamp_new()`, freed via `rbamp_del()`.

### `rbamp_topology_t`

```c
typedef enum {
    RBAMP_TOPOLOGY_SINGLE      = 1,  /* 1 current channel (UI1 / I1) */
    RBAMP_TOPOLOGY_SPLIT_PHASE = 2,  /* 2 current channels (UI2 / I2) */
    RBAMP_TOPOLOGY_THREE_PHASE = 3,  /* 3 current channels (UI3 / I3) */
} rbamp_topology_t;
```

Passed to `rbamp_new_with_topology()` as a hint about the channel
count. On v1 firmware, `rbamp_begin()` cannot reliably detect the
variant, so the hint is required.

### `rbamp_sensor_class_t`

```c
typedef enum {
    RBAMP_SENSOR_UNSET      = 0,  /* Factory default */
    RBAMP_SENSOR_SCT013     = 1,  /* SCT-013 series (the only SKU on v1.2) */
    RBAMP_SENSOR_WIRED_CT   = 2,  /* Reserved — general-purpose wired CT */
    RBAMP_SENSOR_BUILTIN_CT = 3,  /* Reserved — on-board built-in CT */
} rbamp_sensor_class_t;
```

Only `RBAMP_SENSOR_UNSET` and `RBAMP_SENSOR_SCT013` are meaningful
for the current firmware. The reserved values are present in the
header for forward compatibility with future SKUs — for now,
passing them to `rbamp_set_sensor_class()` is not supported and
returns `ESP_ERR_INVALID_ARG`.

### `rbamp_snapshot_t`

Returned by `rbamp_read_all()`. All fields are in SI units.
Unused channels (beyond `channels`) are filled with zeros.

```c
typedef struct {
    float voltage;          /* V — RMS voltage */
    float voltage_peak;     /* V — peak voltage */
    float current[3];       /* A — RMS current per channel */
    float current_peak[3];  /* A — peak current per channel */
    float power[3];         /* W — active power per channel (signed) */
    float power_factor[3];  /* dimensionless, −1..+1 */
    float frequency;        /* Hz — mains frequency */
    rbamp_topology_t topology;
    uint8_t channels;       /* 1..3 — number of valid channels */
    bool has_voltage_hw;    /* true if a voltage sensor is present */
} rbamp_snapshot_t;
```

### `rbamp_period_snapshot_t`

Returned by `rbamp_read_period_snapshot()`. The energy-accounting
primitive.

```c
typedef struct {
    float avg_p[3];        /* W — average active power over the period */
    float max_p;           /* W — peak instantaneous power on channel 0 */
    uint32_t latch_ms;     /* ms — period duration (device's view) */
    uint32_t master_dt_ms; /* ms — master's wall-clock dt since the last latch */
    bool valid;            /* true if the latch-ready flag was set */
} rbamp_period_snapshot_t;
```

> For energy integration, use `master_dt_ms`, not `latch_ms` — the
> module's internal timer has limited accuracy.

## Lifecycle

### `rbamp_new`

```c
esp_err_t rbamp_new(i2c_master_bus_handle_t bus,
                    uint8_t addr,
                    rbamp_handle_t *out);
```

Creates an opaque handle for a slave device. Internally it creates
an `i2c_master_dev_handle_t` via `i2c_master_bus_add_device()`.
Topology defaults to `RBAMP_TOPOLOGY_THREE_PHASE` (harmlessly
"over-declaring" single-channel SKUs — unused channels read
`0.0 A` in the RT block).

> **Watch out for the per-channel API under the default topology.**
> With the default `THREE_PHASE` hint, `rbamp_channels(dev)` returns `3`
> **even on a UI1 SKU**. This means that `rbamp_set_ct_model_ch(dev, 2,
> code)` on a UI1 module passes the index guard and tries to write a
> preset to a physically absent channel — on the device side this is a
> no-op, but you won't see an explicit error either. If you know the
> module's SKU at compile time, use
> `rbamp_new_with_topology(..., RBAMP_TOPOLOGY_SINGLE, ...)` and you'll
> catch any attempt to address ch1/ch2 at the `rbamp_channels(dev)`
> guard.

| Parameter | Description |
|---|---|
| `bus` | The I²C bus handle created via `i2c_new_master_bus()`. |
| `addr` | 7-bit slave address. Range 0x08..0x77, default 0x50. |
| `out` | Where to store the new handle. |

**Returns**: `ESP_OK` on success; `ESP_ERR_NO_MEM` if memory
allocation failed; `ESP_ERR_INVALID_ARG` if `bus == NULL` or
`out == NULL` or `addr` is out of range; the `esp_err_t` from
`i2c_master_bus_add_device()` on a bus-driver error.

### `rbamp_new_with_topology`

```c
esp_err_t rbamp_new_with_topology(i2c_master_bus_handle_t bus,
                                  uint8_t addr,
                                  rbamp_topology_t topology,
                                  rbamp_handle_t *out);
```

Same as `rbamp_new`, but with an explicit topology hint. Use it
when you know the module's SKU at compile time (for example,
`RBAMP_TOPOLOGY_SINGLE` for UI1).

### `rbamp_del`

```c
void rbamp_del(rbamp_handle_t dev);
```

Frees the handle and removes the associated `i2c_master_dev_handle_t`
from the bus. Safe to call with `NULL` — it's a no-op.

### `rbamp_begin`

```c
esp_err_t rbamp_begin(rbamp_handle_t dev);
```

Probes the device, fixes the topology, and primes the LATCH.

Sequence:

1. Read `REG_VERSION` (0x03). If the device did not ACK — `ESP_FAIL`.
   If it returned `0x00` / `0xFF` — `ESP_ERR_INVALID_RESPONSE`.
2. Cache the topology from the constructor (the
   `rbamp_new_with_topology` argument, or the default THREE_PHASE) —
   on the current firmware, `begin()` does not auto-probe the variant.
3. Read `U_rms` to determine whether a voltage sensor is present
   (threshold 1.0 V).
4. Write `CMD_LATCH_PERIOD` as a primer and wait 50 ms. The first
   snapshot after power-up is discarded.
5. Save `esp_timer_get_time()` for subsequent energy integration.

Idempotent: safe to call repeatedly.

**Returns**: `ESP_OK` on success; the error codes above otherwise.

### `rbamp_probe`

```c
esp_err_t rbamp_probe(rbamp_handle_t dev);
```

A lightweight liveness check. A single read of `REG_VERSION` with no
side effects.

**Returns**: `ESP_OK` if the slave ACKed and reported a supported
version (neither `0` nor `0xFF`); `ESP_FAIL` otherwise.

### `rbamp_wait_ready`

```c
esp_err_t rbamp_wait_ready(rbamp_handle_t dev, uint32_t timeout_ms);
```

Polls the module's ready flag until bit 0 is set. Useful after
power-up — the module may need up to 200 ms for its first RT window.

**Returns**: `ESP_OK` if the bit is seen before `timeout_ms` elapses;
`ESP_ERR_TIMEOUT` otherwise.

### Handle-state getters

```c
uint8_t          rbamp_firmware_version(rbamp_handle_t dev);
rbamp_topology_t rbamp_topology         (rbamp_handle_t dev);
uint8_t          rbamp_channels         (rbamp_handle_t dev);
bool             rbamp_has_voltage_hw   (rbamp_handle_t dev);
uint8_t          rbamp_address          (rbamp_handle_t dev);
```

All return cached values without touching the bus.
`rbamp_firmware_version()` returns the `REG_VERSION` byte cached
after `rbamp_begin()`: `0x01` on v1.0, `0x02` on v1.1, `0x03` on v1.2.
`0` is returned if `rbamp_begin()` has not yet been called or it
failed.

## Real-time reads (RT block, 200 ms refresh)

All functions return `esp_err_t`. On `ESP_OK`, the value is written
to the `out` pointer; on error, `*out` is undefined. Under the hood
there is a retry+sanity discipline for resilience against rare
communication glitches in the ESP-IDF I²C stack.

### Per-channel reads

```c
esp_err_t rbamp_read_voltage      (rbamp_handle_t dev, uint8_t phase, float *out);
esp_err_t rbamp_read_voltage_peak (rbamp_handle_t dev, uint8_t phase, float *out);
esp_err_t rbamp_read_current      (rbamp_handle_t dev, uint8_t ch,    float *out);
esp_err_t rbamp_read_current_peak (rbamp_handle_t dev, uint8_t ch,    float *out);
esp_err_t rbamp_read_power        (rbamp_handle_t dev, uint8_t ch,    float *out);
esp_err_t rbamp_read_power_factor (rbamp_handle_t dev, uint8_t ch,    float *out);
esp_err_t rbamp_read_frequency    (rbamp_handle_t dev,                float *out);
```

| Function | Return value | Notes |
|---|---|---|
| `rbamp_read_voltage` | RMS voltage, V | Only `phase = 0` on v1 |
| `rbamp_read_voltage_peak` | Peak voltage, V | |
| `rbamp_read_current` | RMS current on the channel, A | `ch` < `rbamp_channels(dev)` |
| `rbamp_read_current_peak` | Peak current on the channel, A | |
| `rbamp_read_power` | Active power, W **(signed)** | Negative = export (PV / bidirectional installations) |
| `rbamp_read_power_factor` | Power factor | Dimensionless, −1..+1 |
| `rbamp_read_frequency` | Mains frequency, Hz | 50.0 or 60.0 on a healthy grid |

If `ch` or `phase` is out of range, returns `ESP_ERR_INVALID_ARG`.

### One-shot read of the entire RT block

```c
esp_err_t rbamp_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out);
```

Equivalent to the sequential
`rbamp_read_voltage` + `rbamp_read_voltage_peak` +
`rbamp_read_current(0..N)` + `rbamp_read_current_peak(0..N)` +
`rbamp_read_power(0..N)` + `rbamp_read_power_factor(0..N)` +
`rbamp_read_frequency`. Unused channels (beyond
`rbamp_channels(dev)`) are filled with zeros.

**Returns**: `ESP_OK` on success; the `esp_err_t` from the first
failed sub-operation otherwise.

## Period accounting

See [01 · Overview](01_overview.md) for the big picture and
[05 · Quickstart](05_quickstart.md) Step 5 for a minimal template.

### `rbamp_latch_period`

```c
esp_err_t rbamp_latch_period(rbamp_handle_t dev);
```

Writes `CMD_LATCH_PERIOD` (0x27) to `REG_COMMAND`. Does not wait —
the caller must allow a 50 ms settle and check
`rbamp_is_period_valid()` before reading.

For most tasks, use `rbamp_read_period_snapshot()` — it encapsulates
the entire sequence.

### `rbamp_is_period_valid`

```c
esp_err_t rbamp_is_period_valid(rbamp_handle_t dev, bool *out);
```

Reads the latch-ready bit. `*out = true` if the latest snapshot is
fresh.

### `rbamp_read_period_avg_power`

```c
esp_err_t rbamp_read_period_avg_power(rbamp_handle_t dev,
                                      uint8_t ch, float *out);
```

Average active power on the channel over the latched period. Must be
called after `rbamp_latch_period()` + 50 ms settle + valid-check.

### `rbamp_read_period_max_power`

```c
esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out);
```

Peak instantaneous power on channel 0 over the latched period. Only
`ch=0` on v1 firmware.

### `rbamp_read_period_latch_ms`

```c
esp_err_t rbamp_read_period_latch_ms(rbamp_handle_t dev, uint32_t *out);
```

The period duration from the **device's** point of view, in ms.

> **Diagnostic value.** The module's internal timer has limited
> accuracy (a few percent). For energy integration, use
> `master_dt_ms` from `rbamp_period_snapshot_t`, not this field.

### `rbamp_read_period_snapshot`

```c
esp_err_t rbamp_read_period_snapshot(rbamp_handle_t dev,
                                     rbamp_period_snapshot_t *out,
                                     uint16_t settle_ms,
                                     bool skip_latch);
```

**The recommended entry point** for period accounting. The full
sequence under the hood:

1. If `skip_latch == true` — skip the LATCH write (used after a
   manual series of LATCH commands for multi-module sync).
2. Otherwise — write `CMD_LATCH_PERIOD`.
3. Capture `esp_timer_get_time()` to compute `master_dt_ms`.
4. `vTaskDelay(pdMS_TO_TICKS(settle_ms))` — 50 ms by default.
5. Check the latch-ready flag. If 0 — set
   `ESP_ERR_INVALID_RESPONSE` and record the timestamp (so the next
   snapshot does not double-count the `dt`).
6. Read `avg_p[0..channels-1]` + `max_p` + `latch_ms`.
7. Update the timestamp and call the energy integrator for all
   channels (if the component was built without
   `CONFIG_RBAMP_DISABLE_ENERGY`).

| Parameter | Description |
|---|---|
| `out` | Output structure. |
| `settle_ms` | Wait after LATCH (50 recommended). |
| `skip_latch` | If `true` — don't write LATCH, only read (for the multi-module pattern). |

**Returns**: `ESP_OK` if `out->valid == true`;
`ESP_ERR_INVALID_RESPONSE` if the snapshot is stale;
the `esp_err_t` from the bus operations otherwise.

> **Recommended cadence.** Between latch calls: **60 s — comfortable,
> 30 s — borderline, < 10 s — guaranteed stale**. The firmware
> integrates a periodic accumulator internally; calling
> `rbamp_read_period_snapshot()` more often than the firmware can
> prepare the next period will always return a stale snapshot
> (`ESP_ERR_INVALID_RESPONSE`). For high-rate telemetry, use the
> RT functions `rbamp_read_power(ch)` / `rbamp_read_all()` — they
> refresh at ~5 Hz.

### Inline helper

```c
static inline esp_err_t
rbamp_read_period_snapshot_simple(rbamp_handle_t dev,
                                  rbamp_period_snapshot_t *out);
```

A convenience wrapper for the default arguments: `settle_ms=50`,
`skip_latch=false`. Used for ordinary periodic polling.

## Energy accounting (master-side accumulator)

If the component is built without `CONFIG_RBAMP_DISABLE_ENERGY=y`,
accessor functions for the Wh accumulator inside the handle are
available:

```c
double rbamp_energy_wh        (rbamp_handle_t dev, uint8_t ch);
void   rbamp_energy_reset     (rbamp_handle_t dev, uint8_t ch);
void   rbamp_energy_reset_all (rbamp_handle_t dev);
void   rbamp_energy_disable   (rbamp_handle_t dev);
void   rbamp_energy_enable    (rbamp_handle_t dev);
```

The accumulator updates automatically on every successful
`rbamp_read_period_snapshot()`. It is signed — a negative value
means net export.

| Function | Description |
|---|---|
| `rbamp_energy_wh(dev, ch)` | The current Wh total on the channel. If `ch` is out of range — `0.0`. |
| `rbamp_energy_reset(dev, ch)` | Zero out a single channel. |
| `rbamp_energy_reset_all(dev)` | Zero out all channels. |
| `rbamp_energy_disable(dev)` | Disable automatic integration. Useful when the master maintains its own Wh persistence (deep-sleep — see [06 · Examples](06_examples.md) Scenario 9). |
| `rbamp_energy_enable(dev)` | Restore automatic integration. |

The integration formula:

```text
wh[ch] += snap.avg_p[ch] × master_dt_ms / 1000 / 3600
         [W]              [milliseconds]              → [Wh]
```

When built with `CONFIG_RBAMP_DISABLE_ENERGY=y`, both functions (the
prototypes and the storage) are **removed from the binary** via `#if`
guards — attempting to call `rbamp_energy_wh()` produces a linker
error "unresolved external".

## Sensor configuration

A two-step sequence: first the sensor class, then the model. On
v1.2+ firmware both steps are **mandatory**; on v1.0/v1.1 the first
step is silently swallowed (backward compatibility).

For a detailed model-selection guide, see
[03 · Current sensor selection](03_sensor_selection.md).

### `rbamp_set_sensor_class`

```c
esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev,
                                 rbamp_sensor_class_t cls);
```

Sets the current-sensor class and persists it to flash. Blocking,
~705 ms (write `REG_SENSOR_CLASS` + `CMD_SAVE_GAINS` + 700 ms flash
erase).

On v1.2+ it must be called **before** `rbamp_set_ct_model*()` —
including the legacy single-arg form. Otherwise those functions
return `ESP_ERR_INVALID_STATE`. It also resets `REG_CT_MODEL` to 0
on the device side.

On v1.0/v1.1 the register is accepted but unused by the firmware —
the write is "harmless" in terms of its effect on the device, but
**the call itself still blocks for ~705 ms** (the component performs
`CMD_SAVE_GAINS` anyway and waits for the flash erase). If you are
writing code targeting v1.0/v1.1 exclusively, the call can be
skipped — there will be no functional difference.

Passing `RBAMP_SENSOR_UNSET` is accepted but effectively resets the
class configuration (on v1.2+ this means a subsequent
`rbamp_set_ct_model*()` will again require a valid class). See
[10 · Troubleshooting](10_troubleshooting.md), the "Current reads
zero" section, for the "accidentally wrote UNSET" symptom.

**Returns**: `ESP_OK` on success; `ESP_ERR_INVALID_ARG` if `cls` is
not in {`UNSET`, `SCT013`} (the reserved values are unsupported on
the current SKU); I²C error codes on a communication failure.

### `rbamp_set_ct_model`

```c
esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code);
```

The single-parameter form — sets the CT clamp model **on channel 0
only** (the legacy path). Blocking, ~705 ms.

| `code` | Model |
|:---:|---|
| 1 | SCT-013-005 |
| 2 | SCT-013-010 |
| 3 | SCT-013-030 |
| 4 | SCT-013-050 |
| 5 | SCT-013-100 |

`code` outside the 1..5 range → `ESP_ERR_INVALID_ARG`.

> **Precondition on v1.2+ firmware.** `rbamp_set_sensor_class()`
> must have been called successfully before this function — otherwise
> it returns `ESP_ERR_INVALID_STATE`, including from the legacy
> single-arg form. On v1.0/v1.1 the precondition does not apply (the
> class register is accepted but unused by the firmware).

For multi-channel modules, use the per-channel form below.

### `rbamp_set_ct_model_ch`

```c
esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev,
                                uint8_t channel, uint8_t code);
```

The per-channel form (v1.2+ firmware). Sets the CT clamp model on a
specific channel.

Under the hood: write `REG_CT_MODEL` → command
`CMD_SET_CT_MODEL_CH0/1/2` (0x28/0x29/0x2A) according to `channel` →
5 ms settle → `CMD_SAVE_GAINS` → 700 ms. Blocking, ~705 ms.

> **Important: call order matters.** Writing `REG_CT_MODEL` also
> triggers a legacy callback on the device side that unconditionally
> applies the preset to channel 0. That is,
> `rbamp_set_ct_model_ch(dev, 1, code)` correctly configures
> channel 1, but **also clobbers channel 0** with the same preset.
>
> To correctly configure all channels with different models, call
> **in descending channel-index order**:
>
> ```c
> rbamp_set_ct_model_ch(dev, 2, 5);  /* channel 2 = SCT-013-100 */
> rbamp_set_ct_model_ch(dev, 1, 3);  /* channel 1 = SCT-013-030 */
> rbamp_set_ct_model_ch(dev, 0, 1);  /* channel 0 = SCT-013-005 */
> ```

Requires `rbamp_firmware_version(dev) >= 0x03` (v1.2). On older
firmware — `ESP_ERR_NOT_SUPPORTED` with no write. The same
`ESP_ERR_INVALID_STATE` guard on the `rbamp_set_sensor_class()`
precondition applies.

### `rbamp_save_gains`

```c
esp_err_t rbamp_save_gains(rbamp_handle_t dev);
```

A "bare" `CMD_SAVE_GAINS` write with no accompanying register
changes.

> ⚠ **Normally called inside the component.** A bare
> `rbamp_save_gains()` is appropriate ONLY if the caller has manually
> written to non-public calibration registers via raw bus access —
> this is an out-of-warranty operation that bypasses the SKU-tuned
> preset table. Incorrect values will produce wrong current/power
> readings with no explicit warnings. Each call performs a flash
> erase+write cycle (~700 ms); flash endurance is finite (~10,000
> cycles per page) — **do not call it in a loop.**

### `rbamp_prepare_address_change`

```c
esp_err_t rbamp_prepare_address_change(rbamp_handle_t dev,
                                       uint8_t new_addr);
```

Step 1 of 2 to change the module's I²C address.

Procedure: validate the `new_addr` range (0x08..0x77, ≠ current) →
check the device mode → record the "arm" timestamp. The caller must
call `rbamp_commit_address_change()` within 5 seconds, otherwise the
arming expires.

> ⚠ **Develop-mode-only operation.** Changing the address requires
> the module to be in develop mode (an internal flag set at the
> factory). On a standard production module this flag is **not set**,
> and the method returns `ESP_ERR_INVALID_STATE` — the device will
> NOT accept the address change. The
> `rbamp_prepare_address_change()` + `rbamp_commit_address_change()`
> method pair is intended for factory provisioning and integrator
> bench operations, not for user code. If a deployed module needs a
> different I²C address, the documented path is reconfiguration via
> the factory bench (outside the component's scope).

### `rbamp_commit_address_change`

```c
esp_err_t rbamp_commit_address_change(rbamp_handle_t dev);
```

Step 2 of 2. Must be called within **5 seconds** of
`rbamp_prepare_address_change()`. If the window has expired —
`ESP_ERR_TIMEOUT`, the arming is reset, and you must start over with
`rbamp_prepare_address_change()` (calling `commit` again without a
fresh `prepare` also returns `ESP_ERR_TIMEOUT`).

Procedure: check the freshness of the arming → write
`REG_I2C_ADDRESS` (0x30) → `CMD_SAVE_GAINS` + 700 ms → `CMD_RESET` +
100 ms → update the internal address field. After this, all calls on
this instance address the new address.

> ⚠ **Develop-mode-only operation.** Changing the address requires
> the module to be in develop mode (an internal flag set at the
> factory). On a standard production module this flag is **not set**,
> and the method returns `ESP_ERR_INVALID_STATE` — the device will
> NOT accept the address change. The
> `rbamp_prepare_address_change()` + `rbamp_commit_address_change()`
> method pair is intended for factory provisioning and integrator
> bench operations, not for user code. If a deployed module needs a
> different I²C address, the documented path is reconfiguration via
> the factory bench (outside the component's scope).

An additional nuance for `rbamp_commit_address_change()`:

> ⚠ **Restart and re-enumeration after commit.** After a successful
> commit the device resets and re-enumerates at the NEW address.
> Subsequent calls on this handle instance address the new address
> transparently — but any OTHER master on the bus (a Python script,
> an ESP-IDF component on another MCU, a debug probe) will keep
> thinking the device is at the old address until its internal state
> is updated manually.

If the arming window has expired, returns `ESP_ERR_TIMEOUT` and
resets the arming flag — a fresh `rbamp_prepare_address_change()`
will be required.

### `rbamp_factory_reset`

```c
esp_err_t rbamp_factory_reset(rbamp_handle_t dev);
```

The `CMD_FACTORY_RESET` (0xAA) command + a 1500 ms wait.

> ⚠ **Destructive operation.** It erases ALL flash parameters (CT
> model, sensor class, calibration coefficients, I²C address). The
> module reverts to factory defaults — `rbamp_sensor_class_t` becomes
> `RBAMP_SENSOR_UNSET`, `REG_CT_MODEL` becomes 0. Any configuration
> previously applied via `rbamp_set_sensor_class()` /
> `rbamp_set_ct_model*()` is gone. The next user MUST re-apply both
> `rbamp_set_sensor_class()` and `rbamp_set_ct_model*()` before
> accounting becomes operational again. This is **not a "soft
> restart"** — for a soft restart, use `rbamp_reset()`.
> `rbamp_factory_reset()` is reserved for recovery from a known-bad
> state or for handing the module off to another user / installation.

### `rbamp_reset`

```c
esp_err_t rbamp_reset(rbamp_handle_t dev);
```

The `CMD_RESET` (0x01) command + a 100 ms wait. A soft restart of
the device with no loss of flash parameters.

## Multi-module bus

### `rbamp_broadcast_latch`

```c
esp_err_t rbamp_broadcast_latch(i2c_master_bus_handle_t bus,
                                uint32_t timeout_ms);
```

Reserved for future firmware versions. On v1 firmware the function
**always returns `ESP_ERR_NOT_SUPPORTED`** without touching the bus
(General-Call is disabled in the I²C peripheral of the v1 module).

To synchronize the period across multiple modules, use a sequential
series of `rbamp_latch_period()` on each device, a shared 50 ms
settle, and a per-device
`rbamp_read_period_snapshot(skip_latch=true)` — see
[06 · Examples](06_examples.md), the "Monitoring multiple modules"
scenario.

## Diagnostics

### `rbamp_err_to_str`

```c
const char *rbamp_err_to_str(esp_err_t err);
```

Returns a pointer to a static string with a human-readable
description of the error code. It never returns `NULL`. It uses the
standard `esp_err_to_name()` for system codes and extends the
descriptions for rbamp-specific semantics (for example, what
`ESP_ERR_INVALID_STATE` means in the context of `set_ct_model_ch`
without `set_sensor_class`).

Typical usage:

```c
esp_err_t err = rbamp_read_period_snapshot(dev, &snap, 50, false);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "snapshot failed: %s", rbamp_err_to_str(err));
}
```

### `rbamp_retry_exhaustion_count` / `rbamp_sanity_reject_count`

```c
uint32_t rbamp_retry_exhaustion_count(rbamp_handle_t dev);
uint32_t rbamp_sanity_reject_count(rbamp_handle_t dev);
```

Public getters for two diagnostic counters (v1.2.0+):

- **`rbamp_retry_exhaustion_count(dev)`** — a monotonic counter of
  "retry budget exhausted" events (the NACK discipline of SPEC §B.5).
  Each increment = one single-byte transaction that, after
  `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS` attempts, still received no ACK
  from the device.
- **`rbamp_sanity_reject_count(dev)`** — a monotonic counter of
  rejections by the sanity filter on float reads
  (`!isfinite(x) || |x| > 10000`). Each increment = one float that
  arrived from the bus, was judged not to resemble a physical value,
  and was discarded.

Both counters are **monotonically increasing** — they never reset
(except by recreating the handle via `rbamp_del` + `rbamp_new`). The
canonical "rate over an interval" pattern is snapshot + diff: take
snapshots at T₀ and T₁ and subtract for the delta:

```c
uint32_t prev = rbamp_retry_exhaustion_count(dev);
/* ...60 s of normal operation... */
uint32_t delta = rbamp_retry_exhaustion_count(dev) - prev;
if (delta > THRESHOLD) { /* alert */ }
```

These names match the Arduino library cross-platform
(`dev.retryExhaustionCount()` / `dev.sanityRejectCount()` — the same
semantics, snake_case). For practical workloads and alert
thresholds, see [10 · Troubleshooting](10_troubleshooting.md), the
"Monitoring the counters" section.

### Logging via `esp_log`

The component logs under the tag `"rbamp"`. The log level is
compiled in via Kconfig — runtime `esp_log_level_set("rbamp", ...)`
works but is capped from above by the compile-time level.

```c
esp_log_level_set("rbamp", ESP_LOG_DEBUG);   /* for debugging */
```

## Error codes

The `rbamp_*` functions return standard `esp_err_t` values. The
specific codes and their semantics in the rbamp context:

| Code | When | Where to look |
|---|---|---|
| `ESP_OK` (0) | Success | — |
| `ESP_FAIL` (-1) | An I²C transaction failed after retries; or the sanity filter rejected a value | wiring, power, bus speed — the "Module doesn't respond over I²C" section in [10 · Troubleshooting](10_troubleshooting.md) |
| `ESP_ERR_INVALID_ARG` (0x102) | Invalid argument: `dev == NULL`, `channel` or `phase` out of range, `code` outside 1..5, `cls` reserved | check the call arguments |
| `ESP_ERR_TIMEOUT` (0x107) | `rbamp_wait_ready` timed out; the `rbamp_commit_address_change` window (5 s) expired | check responsiveness via `rbamp_probe()` |
| `ESP_ERR_INVALID_STATE` (0x103) | Precondition not met: `set_ct_model*` before `set_sensor_class` on v1.2+; address-change on a production module (`REG_MODE == 0`) | the "`rbamp_set_*` returns `ESP_ERR_INVALID_STATE`" section in [10 · Troubleshooting](10_troubleshooting.md) |
| `ESP_ERR_NOT_SUPPORTED` (0x106) | The function is unavailable on the current firmware: per-channel `set_ct_model_ch` on v1.0/1.1; `rbamp_broadcast_latch` on v1 | check `rbamp_firmware_version(dev)` |
| `ESP_ERR_INVALID_RESPONSE` (0x108) | Period snapshot stale (ready flag = 0); or `REG_VERSION` = 0/0xFF on `rbamp_begin` | the "Period snapshots are always stale" / "Can't get out of rbamp_begin" sections in [10 · Troubleshooting](10_troubleshooting.md) |
| `ESP_ERR_NO_MEM` (0x101) | Failed to allocate memory for the handle | check the heap budget |

All returned codes can be printed in human-readable form via
`rbamp_err_to_str()`.

## Compile-time configuration (Kconfig)

Six `CONFIG_RBAMP_*` symbols are available in `idf.py menuconfig` →
**Component config → rbAmp client**:

| Symbol | Type | Range / choice | Default | Purpose |
|---|---|---|---|---|
| `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ` | int | 50000–1000000 Hz | 50000 | SCL frequency. 50 kHz is mandatory for the current firmware on ESP32. |
| `CONFIG_RBAMP_I2C_TIMEOUT_MS` | int | 10–1000 ms | 100 | Timeout for `i2c_master_transmit/receive`. |
| `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS` | int | 1–10 | 3 | Attempts per single-byte read. |
| `CONFIG_RBAMP_NACK_RETRY_GAP_MS` | int | 1–100 ms | 5 | Gap between retry attempts. |
| `CONFIG_RBAMP_DISABLE_ENERGY` | bool | n/y | n | Removes the Wh accumulator from compilation entirely (both storage and API). |
| `CONFIG_RBAMP_LOG_LEVEL_*` | choice | NONE / ERROR / WARN / INFO / DEBUG | INFO | Compile-time `LOG_LOCAL_LEVEL` for the `rbamp` tag. |

A sample `sdkconfig.defaults` for a typical production scenario:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=3
CONFIG_RBAMP_NACK_RETRY_GAP_MS=5
CONFIG_RBAMP_I2C_TIMEOUT_MS=100
CONFIG_RBAMP_LOG_LEVEL_INFO=y
# CONFIG_RBAMP_DISABLE_ENERGY left off (default n)
```

For a bench test under heavy load:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=5
CONFIG_RBAMP_I2C_TIMEOUT_MS=200
CONFIG_RBAMP_LOG_LEVEL_DEBUG=y
```

After firmware v1.1+ ships with the slave-side NACK fix, 100 kHz
becomes the working default speed:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=100000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=1
```

## Settle-time constants

The component uses the following settle times internally when
executing commands (for a user who works on top of the raw API via
`i2c_master_transmit_receive` and wants to honor the same timings):

The library exports a small set of settle-time constants (ms) for
raw-API users who send commands to the device directly and must wait
for the device to finish processing before reading the affected
registers. The component's wrapper functions
(`rbamp_save_gains`, `rbamp_factory_reset`, `rbamp_set_ct_model_ch`,
etc.) hold the correct delay internally — these constants are needed
only if you bypass the wrappers.

| Macro | Purpose | ms |
|---|---|---|
| `RBAMP_SETTLE_MS_LATCH_PERIOD` | After `CMD_LATCH_PERIOD` | 50 |
| `RBAMP_SETTLE_MS_RESET` | After `CMD_RESET` | 100 |
| `RBAMP_SETTLE_MS_SAVE_GAINS` | After `CMD_SAVE_GAINS` (flash erase+write) | 700 |
| `RBAMP_SETTLE_MS_FACTORY_RESET` | After `CMD_FACTORY_RESET` | 1500 |
| `RBAMP_SETTLE_MS_SET_CT_MODEL_CH0` | After `CMD_SET_CT_MODEL_CH0` | 5 |
| `RBAMP_SETTLE_MS_SET_CT_MODEL_CH1` | After `CMD_SET_CT_MODEL_CH1` | 5 |
| `RBAMP_SETTLE_MS_SET_CT_MODEL_CH2` | After `CMD_SET_CT_MODEL_CH2` | 5 |

The source of truth is the `@defgroup rbamp_settle_ms` block in
[`include/rbamp.h`](../include/rbamp.h) (promoted to the public API
in v1.2.0). The numbers are the rbAmp protocol's specification
minimums; shorter is not allowed, longer is fine.

## References

- [05 · Quickstart](05_quickstart.md) — your first working project
- [06 · Examples](06_examples.md) — working scenarios
- [10 · Troubleshooting](10_troubleshooting.md) — decoding errors and
  resolving common problems
- The source header file with Doxygen comments:
  [`include/rbamp.h`](../include/rbamp.h)


---

[← Cloud Integrations](08_cloud_integrations.md) | [Contents](README.md) | [Troubleshooting →](10_troubleshooting.md)
