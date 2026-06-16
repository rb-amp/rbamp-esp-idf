# 09 · API Reference

The complete public API of the `rbamp` component for ESP-IDF.
The source is the header file [`include/rbamp.h`](../include/rbamp.h)
with Doxygen comments.

This chapter is a reference: signatures, return values, side
effects, edge cases. For working examples go to
[06 · Examples](06_examples.md); for a quick start, see
[05 · Quickstart](05_quickstart.md).

## Header file

`#include "rbamp.h"` — the single public header. It transitively
pulls in `esp_err.h` for `esp_err_t` and `driver/i2c_master.h` for
the `i2c_master_bus_handle_t` type. No additional includes are
required.

The API is wrapped in an `extern "C"` block — compatible with both
C and C++ callers.

## General idioms

The API follows standard ESP-IDF patterns:

- **Opaque handle**: `rbamp_handle_t` — a typedef for
  `struct rbamp_obj_t *`; the user does not reach into its internals.
  Created via `rbamp_new()`, freed via `rbamp_del()`.
- **`esp_err_t` returns** — `ESP_OK` on success, one of the
  `ESP_ERR_*` codes on failure. Use `ESP_ERROR_CHECK()` for
  abort-on-error handling, or check manually.
- **Output-via-pointer**: functions that return a value take a
  pointer to a buffer as the last parameter. This lets a single call
  return both an `esp_err_t` and a value.
- **Human-readable error strings**: `rbamp_err_to_str(err)` returns
  a pointer to a static string. It never returns `NULL`.

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

Passed to `rbamp_new_with_topology()` as a hint about the number of
channels. On v1 firmware `rbamp_begin()` cannot reliably detect the
variant, so the hint is required.

### `rbamp_sensor_class_t`

```c
typedef enum {
    RBAMP_SENSOR_UNSET      = 0,  /* Factory default value */
    RBAMP_SENSOR_SCT013     = 1,  /* SCT-013 series (the only supported SKU) */
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

Returned by `rbamp_read_all()`. All fields are in SI units. Unused
channels (beyond `channels`) are zero-filled.

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
    uint32_t latch_ms;     /* ms — chip software timer (0xEC); DIAGNOSTIC, not billing */
    uint32_t master_dt_ms; /* ms — master wall-clock dt since the last latch (CANONICAL) */
    bool valid;            /* true if the latch-ready flag was set */
} rbamp_period_snapshot_t;
```

> ⚠ **`latch_ms` (register 0xEC) is diagnostic, not billing.** It is
> a chip software timer based on SysTick (HAL_GetTick). Under normal
> ISR load it undercounts by 25–30 % (root E.6/F10 HW result,
> SysTick starvation by design). **Do not use it for energy
> integration.** The canonical dt for billing is `master_dt_ms` (or
> your own `esp_timer_get_time()` delta).

## Lifecycle

### `rbamp_new`

```c
esp_err_t rbamp_new(i2c_master_bus_handle_t bus,
                    uint8_t addr,
                    rbamp_handle_t *out);
```

Creates an opaque handle for a slave device. Internally it creates
an `i2c_master_dev_handle_t` via `i2c_master_bus_add_device()`. The
topology defaults to `RBAMP_TOPOLOGY_THREE_PHASE` (it harmlessly
"over-specifies" single-channel SKUs — unused channels read `0.0 A`
in the RT block).

> **Caution with the per-channel API under the default topology.**
> With the default `THREE_PHASE` hint, `rbamp_channels(dev)` returns
> `3` **even on a UI1 SKU**. This means `rbamp_set_ct_model_ch(dev, 2,
> code)` on a UI1 module will pass the index guard and try to write a
> preset to a physically absent channel — on the device side this is
> a no-op, but you will not see an explicit error either. If you know
> the module SKU at compile time, use
> `rbamp_new_with_topology(..., RBAMP_TOPOLOGY_SINGLE, ...)` and
> catch any attempt to address ch1/ch2 at the `rbamp_channels(dev)`
> guard.

| Parameter | Description |
|---|---|
| `bus` | I²C bus handle created via `i2c_new_master_bus()`. |
| `addr` | 7-bit slave address. Range 0x08..0x77, default 0x50. |
| `out` | Where to write the new handle. |

**Returns**: `ESP_OK` on success; `ESP_ERR_NO_MEM` if memory could
not be allocated; `ESP_ERR_INVALID_ARG` if `bus == NULL` or
`out == NULL` or `addr` is out of range; the `esp_err_t` from
`i2c_master_bus_add_device()` on a bus-driver error.

### `rbamp_new_with_topology`

```c
esp_err_t rbamp_new_with_topology(i2c_master_bus_handle_t bus,
                                  uint8_t addr,
                                  rbamp_topology_t topology,
                                  rbamp_handle_t *out);
```

Same as `rbamp_new`, but with an explicit topology hint. Use it if
you know the module SKU at compile time (for example,
`RBAMP_TOPOLOGY_SINGLE` for UI1).

### `rbamp_del`

```c
void rbamp_del(rbamp_handle_t dev);
```

Frees the handle and removes the associated `i2c_master_dev_handle_t`
from the bus. Safe to call with `NULL` — it is a no-op.

### `rbamp_begin`

```c
esp_err_t rbamp_begin(rbamp_handle_t dev);
```

Probes the device, locks in the topology, and primes LATCH.

Sequence:

1. Read `REG_VERSION` (0x03). If the device did not ACK — `ESP_FAIL`.
   If it returned `0x00` / `0xFF` — `ESP_ERR_INVALID_RESPONSE`.
2. Cache the topology from the constructor (the
   `rbamp_new_with_topology` argument, or the default THREE_PHASE) —
   on the current firmware `begin()` does not auto-probe the variant.
3. Read `U_rms` to determine whether a voltage sensor is present
   (threshold 1.0 V).
4. Write `CMD_LATCH_PERIOD` as a primer + wait 50 ms. The first
   snapshot after power-on is discarded.
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
version (not `0` and not `0xFF`); `ESP_FAIL` otherwise.

### `rbamp_wait_ready`

```c
esp_err_t rbamp_wait_ready(rbamp_handle_t dev, uint32_t timeout_ms);
```

Polls the module's ready flag until bit 0 is set. Useful after
power-on — the module may need up to 200 ms for the first RT window.

**Returns**: `ESP_OK` if the bit is seen before `timeout_ms`
expires; `ESP_ERR_TIMEOUT` otherwise.

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
after `rbamp_begin()` — an opaque byte; `0` is returned if
`rbamp_begin()` has not yet been called or failed.

## Real-time reads (RT block, 200 ms refresh)

All functions return `esp_err_t`. On `ESP_OK` the value is written
to the `out` pointer; on error `*out` is undefined. Under the hood
there is a retry+sanity discipline for resilience against the rare
link glitches in the ESP-IDF I²C stack.

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
| `rbamp_read_voltage` | RMS voltage, V | `phase = 0` only on v1 |
| `rbamp_read_voltage_peak` | Peak voltage, V | |
| `rbamp_read_current` | RMS current on the channel, A | `ch` < `rbamp_channels(dev)` |
| `rbamp_read_current_peak` | Peak current on the channel, A | |
| `rbamp_read_power` | Active power, W **(signed)** | Negative = export (PV / bidirectional installs) |
| `rbamp_read_power_factor` | Power factor | Dimensionless, −1..+1 |
| `rbamp_read_frequency` | Mains frequency, Hz | 50.0 or 60.0 on a healthy grid |

If `ch` or `phase` is out of range, returns `ESP_ERR_INVALID_ARG`.

### One-shot read of the whole RT block

```c
esp_err_t rbamp_read_all(rbamp_handle_t dev, rbamp_snapshot_t *out);
```

Equivalent to sequential
`rbamp_read_voltage` + `rbamp_read_voltage_peak` +
`rbamp_read_current(0..N)` + `rbamp_read_current_peak(0..N)` +
`rbamp_read_power(0..N)` + `rbamp_read_power_factor(0..N)` +
`rbamp_read_frequency`. Unused channels (beyond
`rbamp_channels(dev)`) are zero-filled.

**Returns**: `ESP_OK` on success; the `esp_err_t` from the first
sub-operation that failed otherwise.

## Per-period accounting

See [01 · Overview](01_overview.md) for the big picture and
[05 · Quickstart](05_quickstart.md) Step 5 for a minimal template.

### `rbamp_latch_period`

```c
esp_err_t rbamp_latch_period(rbamp_handle_t dev);
```

Writes `CMD_LATCH_PERIOD` (0x27) to `REG_COMMAND`. It does not
wait — the caller must allow a 50 ms settle and check
`rbamp_is_period_valid()` before reading.

For most use cases, use `rbamp_read_period_snapshot()` — it
encapsulates the entire sequence.

### `rbamp_is_period_valid`

```c
esp_err_t rbamp_is_period_valid(rbamp_handle_t dev, bool *out);
```

Reads the latch-ready bit. `*out = true` if the last snapshot is
fresh.

### `rbamp_read_period_avg_power`

```c
esp_err_t rbamp_read_period_avg_power(rbamp_handle_t dev,
                                      uint8_t ch, float *out);
```

Average active power on the channel over the latched period. Must be
called after `rbamp_latch_period()` + a 50 ms settle + a valid
check.

### `rbamp_read_period_max_power`

```c
esp_err_t rbamp_read_period_max_power(rbamp_handle_t dev, float *out);
```

Peak instantaneous power on channel 0 over the latched period.
`ch=0` only on v1 firmware.

### `rbamp_read_period_latch_ms`

```c
esp_err_t rbamp_read_period_latch_ms(rbamp_handle_t dev, uint32_t *out);
```

The period duration as seen by the **device**, in ms.

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

**The recommended entry point** for per-period accounting. The full
sequence under the hood:

1. If `skip_latch == true` — skip the LATCH write (used after a
   manual series of LATCH commands for multi-module sync).
2. Otherwise — write `CMD_LATCH_PERIOD`.
3. Capture `esp_timer_get_time()` to compute `master_dt_ms`.
4. `vTaskDelay(pdMS_TO_TICKS(settle_ms))` — 50 ms by default.
5. Check the latch-ready flag. If it is 0 — set
   `ESP_ERR_INVALID_RESPONSE` and record the timestamp (so the next
   snapshot does not double-count `dt`).
6. Read `avg_p[0..channels-1]` + `max_p` + `latch_ms`.
7. Update the timestamp + invoke the energy integrator for all
   channels (if the component is built without
   `CONFIG_RBAMP_DISABLE_ENERGY`).

| Parameter | Description |
|---|---|
| `out` | Output structure. |
| `settle_ms` | Wait after LATCH (50 recommended). |
| `skip_latch` | If `true` — do not write LATCH, only read (for the multi-module pattern). |

**Returns**: `ESP_OK` if `out->valid == true`;
`ESP_ERR_INVALID_RESPONSE` if the snapshot is stale; the `esp_err_t`
from bus operations otherwise.

> **Recommended cadence.** Between latch calls: **60 s is
> comfortable, 30 s is marginal, < 10 s guarantees stale reads**. The
> firmware integrates the periodic accumulator internally; calling
> `rbamp_read_period_snapshot()` more often than the firmware can
> prepare the next period will always return a stale snapshot
> (`ESP_ERR_INVALID_RESPONSE`). For high-rate telemetry use the RT
> functions `rbamp_read_power(ch)` / `rbamp_read_all()` — they
> refresh at ~5 Hz.

### Inline helper

```c
static inline esp_err_t
rbamp_read_period_snapshot_simple(rbamp_handle_t dev,
                                  rbamp_period_snapshot_t *out);
```

A convenience wrapper for the default arguments: `settle_ms=50`,
`skip_latch=false`. Use it for ordinary periodic polling.

## Energy accounting (master-side accumulator)

If the component is built without `CONFIG_RBAMP_DISABLE_ENERGY=y`,
accessor functions are available for the Wh accumulator inside the
handle:

```c
double rbamp_energy_wh        (rbamp_handle_t dev, uint8_t ch);
void   rbamp_energy_reset     (rbamp_handle_t dev, uint8_t ch);
void   rbamp_energy_reset_all (rbamp_handle_t dev);
void   rbamp_energy_disable   (rbamp_handle_t dev);
void   rbamp_energy_enable    (rbamp_handle_t dev);
```

The accumulator is updated automatically on every successful
`rbamp_read_period_snapshot()`. It is signed — a negative value
means net export.

| Function | Description |
|---|---|
| `rbamp_energy_wh(dev, ch)` | Current Wh total on the channel. If `ch` is out of range — `0.0`. |
| `rbamp_energy_reset(dev, ch)` | Zero a single channel. |
| `rbamp_energy_reset_all(dev)` | Zero all channels. |
| `rbamp_energy_disable(dev)` | Disable automatic integration. Useful when the master maintains its own Wh persistence (deep-sleep — see [06 · Examples](06_examples.md) Scenario 9). |
| `rbamp_energy_enable(dev)` | Re-enable automatic integration. |

Integration formula:

```text
wh[ch] += snap.avg_p[ch] × master_dt_ms / 1000 / 3600
         [W]              [milliseconds]              → [Wh]
```

When built with `CONFIG_RBAMP_DISABLE_ENERGY=y`, both the functions
(prototypes and storage) **are removed from the binary** via `#if`
guards — an attempt to call `rbamp_energy_wh()` produces a linker
error "unresolved external".

## Identification and provisioning

These functions let the master learn **what exactly** is connected
to the bus (variant, capability bit-map, UID) and prepare a new or
freshly flashed module for operation (provisioning + saving the
configuration to flash).

### `rbamp_read_variant`

```c
esp_err_t rbamp_read_variant(rbamp_handle_t dev, rbamp_variant_t *out);
```

Reads the `HW_VARIANT (0x55)` register and maps it into the
`rbamp_variant_t` enum (`RBAMP_VARIANT_UI1` / `_I1` / `_I2` / `_I3`,
etc.). For compatibility with older code that reads the "raw" byte,
you can cast it back to `uint8_t`. Values:

| Code | SKU | I channels | U channel |
|---|---|---|---|
| `1` | UI1 | 1 | yes |
| `4` | I1 | 1 | no |
| `5` | I2 | 2 | no |
| `6` | I3 | 3 | no |

Codes `2` (UI2) and `3` (UI3) are reserved for roadmap variants and
do not appear on current hardware. The behavior is read-only,
ROM-stored, changed only by factory reflashing.

### `rbamp_read_capability`

```c
esp_err_t rbamp_read_capability(rbamp_handle_t dev, uint16_t *out);
```

Reads the 12-bit feature map from the `CAPABILITY (0x57/0x58)`
registers, LE u16. Bit 8 (`0x0100`) indicates the presence of a
voltage front-end: if the bit is clear, the module does not compute
P / PF / Q and the values of those registers are `0.0f`. The full
list of bits and their semantics is in the section
[«CAPABILITY (0x57/0x58, u16 LE) — feature-detect map»](#capability-0x570x58-u16-le--feature-detect-map) below.

### `rbamp_has_voltage`

```c
bool rbamp_has_voltage(rbamp_handle_t dev);
```

A convenience helper over `rbamp_read_capability` — returns `true`
if the module has a U channel (that is, `(cap >> 8) & 1`). It is
cached in the handle on the first call; subsequent calls do not
perform an I²C operation.

### `rbamp_read_product_id`

```c
esp_err_t rbamp_read_product_id(rbamp_handle_t dev, uint8_t *out);
```

Reads `PRODUCT_ID (0x54)` — a byte identifier of the family
(rbAmp = `0x01`). Used by the fleet scanner to confirm that the
device at an address is really an rbAmp, and not some other I²C
slave that happened to answer on the same address.

### `rbamp_read_uid`

```c
esp_err_t rbamp_read_uid(rbamp_handle_t dev, uint8_t uid[12]);
```

Reads the 96-bit unique MCU identifier from `UID (0x5C..0x67)` in a
single burst operation. The UID is guaranteed unique per module and
does not change on reflashing — handy for serial logs, master-side
anti-clone checks, and identification when replacing a module in a
deployed fleet.

### `rbamp_is_provisioned`

```c
esp_err_t rbamp_is_provisioned(rbamp_handle_t dev, bool *out);
```

Checks whether the module has gone through initial provisioning.
Technically it checks `REG_ERROR != 0xFB (ERR_FLASH_PARAMS_BAD)` on
a freshly booted module — `0xFB` is set by the firmware when the
user-config block is empty / corrupt. A fresh-from-factory module
returns `false` — it needs `rbamp_save_user_config` (or provisioning
via the fleet) after the first CT-model and address setup.

> On v1.3 this code is **normal** for a virgin module (not fatal).
> Do not abort provisioning on the first occurrence of `0xFB`.

### `rbamp_save_user_config`

```c
esp_err_t rbamp_save_user_config(rbamp_handle_t dev);
```

Issues `CMD_SAVE_USER_CONFIG (opcode 0x32)` — saves the user-config
block to flash: `SENSOR_CLASS`, `CT_MODEL` per channel,
`I2C_ADDRESS`, `FLEET_CONFIG`, `GROUP_ID`, `LABEL`. It **does not
save** factory-cal parameters (gain, NF, phase) — those are under a
separate `CMD_SAVE_GAINS`, which is locked in production mode.

It blocks the thread for ~700 ms (flash erase + write cycle). During
the call, do not try to access the module from other tasks.

Returns:
- `ESP_OK` — configuration saved, survives a power cycle.
- `ESP_ERR_TIMEOUT` — the module did not respond in time; power may
  have dropped during the flash cycle. Re-verify via reset +
  read-back.
- `ESP_FAIL` — the module returned `REG_ERROR != OK` after the
  command. Check `rbamp_read_last_error` for details.

> **Read-back ≠ persistence**: confirming that a value was written
> to flash requires a **reboot + re-read**. See the section
> [Error model v1.3](#error-model-v13) below and
> [03 · Current sensor selection](03_sensor_selection.md#production-vs-develop-mode-persistence-reference).

## Error model v1.3

Errors have **two independent channels** that work in different
ways. Understanding the difference is critical to correctly
interpreting the module's state.

| | Channel A — client-side, sync | Channel B — DUT-side, durable async |
|---|---|---|
| **Where it lives** | Validation inside the component (lib) | `REG_ERROR (0x02)` + `EVENT_FLAGS.bit3` (`0x2A`) |
| **When it fires** | Before the I²C transaction (lib intercepts) | After the FW accepts a write and rejects it |
| **What the master sees** | `ESP_ERR_INVALID_ARG` from the setter; `REG_ERROR` **stays 0x00** | `REG_ERROR` = the immediate rejection code; `bit3` latches **~200-300 ms** after the transaction |
| **Sticky** | no (just a return code) | `REG_ERROR` last-write; `bit3` sticky W1C |
| **Typical path** | Argument outside the allowed bounds (channel ∉ {0,1,2}, model ∉ {1..7}, ...) | Raw register write that the component does not pre-validate (`rbamp_set_digest_config` with an invalid mask, a custom low-level write) |

### Channel A — client-side validation (component setters)

All component setters (`rbamp_set_sensor_class`,
`rbamp_set_ct_model_ch`, `rbamp_configure_channels`, etc.) first
**check the arguments** against the allowed values. If an argument
is out of range, the setter returns `ESP_ERR_INVALID_ARG`
**without performing an I²C transaction at all**. Accordingly,
`REG_ERROR` on the device **is not touched**:

```c
esp_err_t err = rbamp_set_ct_model_ch(dev, /*channel=*/99, /*code=*/3);
/* err == ESP_ERR_INVALID_ARG  (lib client-side validation) */

uint8_t reg_err;
rbamp_read_last_error(dev, &reg_err);
/* reg_err == 0x00 — REG_ERROR untouched, lib intercepted before I²C */
```

This means: if you want to know **exactly why** a setter refused,
the reliable source is the function's **return code** + checking
your own argument. `REG_ERROR` will tell you nothing in this
scenario (the next successful transaction sets it).

### Channel B — DUT-side rejection (durable async)

If a write **went through I²C** (the lib did not intercept it
because the argument looked valid), but the **firmware rejected**
it, the second channel kicks in:

- **`REG_ERROR (0x02)`** immediately holds the reason code (for
  example, `0xFE = ERR_PARAM`). This is the last-write outcome: any
  subsequent successful write overwrites it. You must read it
  **right after** your own write, before any other transaction.
- **`EVENT_FLAGS.bit3` (`rbamp_has_error`)** — sticky W1C. **It does
  not latch instantly**: on the bench a delay of **~200-300 ms** was
  measured between the rejected transaction and bit3 being set. Right
  after the write bit3 is still `0`; after 300 ms it is `1`. After
  that `bit3` holds across any number of reads.

Bench output (lib c8ac901, raw write `DIGEST_CONFIG = 0xFF`):

```text
raw write 0x29=0xFF → read_last_error → REG_ERROR=0xFE (immediate)
has_error(bit3):     immediate=0, +300 ms poll#1=1, poll#2=1
```

> ⚠ **Do not use `has_error` to validate a write you just made.** For
> the outcome of **that specific write**, only
> `rbamp_read_last_error` read **immediately** after the write,
> before any other transaction, is reliable (plus the setter's
> return code, if it has one). `has_error` is an async channel for
> polling-style monitoring of "did anything fault between two
> checks", not for synchronous validation.

### Clearing a durable error — clear both (REG_ERROR and bit3)

`bit3` is re-picked-up by the firmware every ~200-300 ms from the
current state of `REG_ERROR`. So a plain `clear_event_flags(1<<3)`
**will not work** — bit3 is immediately **re-set** from the
still-uncleared `REG_ERROR`:

```text
state: REG_ERROR=0xFE, bit3=1

clear_event_flags(1<<3)          → bit3=1 still (re-latched from REG_ERROR=0xFE)

clear_error  +  clear_event_flags(1<<3)
                                  → bit3=0 (cleared cleanly)
```

The correct order: first `rbamp_clear_error()` (clears `REG_ERROR`),
then `rbamp_clear_event_flags(1<<3)` (clears the sticky latch). If
you want atomicity, there must be no rejected write between them.

### `rbamp_read_last_error`

```c
esp_err_t rbamp_read_last_error(rbamp_handle_t dev, uint8_t *out);
```

Reads `REG_ERROR (0x02)` directly — the outcome of the **last
transaction successfully accepted by the bus** (a write or command).
Not sticky: any subsequent successful write overwrites it with its
own outcome.

Used in Channel B (DUT-side rejection) for the extended reason code.

### `rbamp_has_error`

```c
esp_err_t rbamp_has_error(rbamp_handle_t dev, bool *out);
```

A convenience helper over `EVENT_FLAGS.bit3` (`ERROR_OCCURRED`,
register `0x2A`). Returns `true` if bit3 is set. Sticky W1C; not
cleared until an explicit clear (with a caveat — see the "Clearing a
durable error" section above).

Used for polling-style monitoring of "did anything fault" — for
example, in long-running loops where intermediate writes may have
clobbered REG_ERROR, but bit3 preserves the fact that at least one
of them was rejected.

### `rbamp_read_event_flags` / `rbamp_clear_event_flags`

```c
esp_err_t rbamp_read_event_flags(rbamp_handle_t dev, uint8_t *out);
esp_err_t rbamp_clear_event_flags(rbamp_handle_t dev, uint8_t mask);
```

`EVENT_FLAGS (0x2A)` — a byte of sticky flags; the specific bits are
described in the specification (including `ERROR_OCCURRED` bit3).
Clearing is write-1-to-clear: write a mask with ones in the bits you
want to reset.

It helps separate causes: you can first read all flags, then reset
only the handled ones, leaving the unhandled ones for the next
polling cycle.

### `rbamp_clear_error`

```c
esp_err_t rbamp_clear_error(rbamp_handle_t dev);
```

Issues `CMD_CLEAR_ERROR (opcode 0x31)` — zeroes `REG_ERROR`. **Needed
to clear a durable error** in tandem with `clear_event_flags(1<<3)`
(see above). Without `clear_error`, a plain `clear_event_flags(1<<3)`
will leave bit3 at `1` after ~200-300 ms because of the re-latch.

## Multi-channel configuration

On multi-channel modules (`I2`, `I3`), each current channel has its
own CT-sensor model. The atomic utility for bulk configuration is
`rbamp_configure_channels`.

### `rbamp_configure_channels`

```c
esp_err_t rbamp_configure_channels(rbamp_handle_t dev,
                                   rbamp_sensor_class_t cls,
                                   const uint8_t *models,
                                   uint8_t n);
```

Parameters:
- `cls` — the sensor class, applied to all channels (only
  `RBAMP_SENSOR_SCT013` is valid right now).
- `models` — an array of `n` CT-model codes, one per channel.
- `n` — the array length. **Variant-clamped**: if `n` is greater
  than the number of channels on this variant (for example, `n=3` on
  `I2`), the function clamps to the actual channel count; the extra
  array elements are ignored. If `n` is less than the number of
  channels, the missing channels are left untouched (they keep their
  previous model).

Internally the function performs:

1. `set_sensor_class(cls)` once.
2. For each channel `k < min(n, n_channels)`: stage `REG_CT_MODEL = models[k]` + `CMD_SET_CT_MODEL_CHk`.
3. **One** terminal `CMD_SAVE_USER_CONFIG` at the end — that is,
   **one** flash erase + write cycle for the whole configuration,
   instead of `n` separate cycles. This is the "flash-friendly"
   property.

Returns:
- `ESP_OK` — configuration applied and saved.
- `ESP_ERR_INVALID_ARG` — invalid `cls` or `models[k]` (validation
  at the bind step, see the "Error model" section above).
- Other ESP-IDF codes — bus error, timeout, etc.

### `rbamp_read_ct_model_ch`

```c
esp_err_t rbamp_read_ct_model_ch(rbamp_handle_t dev,
                                 uint8_t channel,
                                 uint8_t *out);
```

Reads the applied (verify-mirror) CT-sensor model on a specific
channel from register `0x51` (ch0) / `0x52` (ch1) / `0x53` (ch2).
This is a **read-back from RAM**, not a persistence confirmation —
to confirm a flash save, use a reboot + re-read.

## Fleet manager — managing multiple modules

The Fleet API is the library's primary high-level surface. It
manages a collection of modules on a shared I²C bus: discovery,
provisioning, poll aggregation, error aggregation, synchronous
latches.

All fleet functions are thread-safe via an internal mutex;
concurrent calls from different tasks are serialized. A single owner
of the `fleet` from one task plus read-only access (for example,
`_count`, `_get`) from others is acceptable, but coordinate complex
multi-step operations explicitly.

### Lifecycle

```c
esp_err_t rbamp_fleet_create(i2c_master_bus_handle_t bus,
                             rbamp_fleet_t *out);
void      rbamp_fleet_destroy(rbamp_fleet_t fleet);
```

`create` takes a bus and returns an **empty** fleet. `destroy` frees
all managed handles and the fleet itself — after the call, any
handle pointers obtained via `rbamp_fleet_get` are **invalid**.

### Discovery — `rbamp_fleet_scan`

```c
esp_err_t rbamp_fleet_scan(rbamp_fleet_t fleet,
                           bool match_product,
                           size_t *added);
```

Probes the bus across the range `0x08..0x77`; for each address that
ACKs:

1. Checks identity (`PRODUCT_ID`, `HW_VARIANT`, `CAPABILITY`). If
   `match_product = true`, it requires a match with the rbAmp
   PRODUCT_ID.
2. Runs the conflict detector (two independent signals; see
   [10 · Troubleshooting](10_troubleshooting.md), section "Fleet: a
   module ended up in excluded"). Suspicious addresses go into the
   `excluded` list, not the fleet.
3. Performs a Tier-2 wedge canary: it probes an address where there
   should be guaranteed to be no one. If the canary gets an ACK, it
   returns `ESP_ERR_INVALID_STATE` — the bus is wedged, the fleet
   stays empty (see the diagnostics in ch. 10).

After success, `*added` holds the number of modules added to the
fleet. `excluded_count` (see below) is available through a separate
getter.

**Bench output (HW-validated on a heterogeneous fleet UI1+I2+I3 with
4.7 kΩ external pull-ups):**

```text
rbamp_fleet_scan() → 3 module(s), 0 excluded
  module[0] @0x50  channels=1  voltage=yes   (UI1 — mains)
  module[1] @0x51  channels=2  voltage=no    (I2 — sub-meter)
  module[2] @0x52  channels=3  voltage=no    (I3 — sub-meter)
rbamp_fleet_count()          = 3
rbamp_fleet_excluded_count() = 0
```

In this scenario an address conflict would land in the excluded list
(`excluded_count > 0`), and a wedged bus would land in
`ESP_ERR_INVALID_STATE` with an empty fleet (see
[10 · Troubleshooting](10_troubleshooting.md)).

### Module access

```c
size_t          rbamp_fleet_count(rbamp_fleet_t fleet);
rbamp_handle_t  rbamp_fleet_get(rbamp_fleet_t fleet, size_t index);
rbamp_handle_t  rbamp_fleet_find(rbamp_fleet_t fleet, uint8_t address);
esp_err_t       rbamp_fleet_add(rbamp_fleet_t fleet, rbamp_handle_t dev);

size_t          rbamp_fleet_excluded_count(rbamp_fleet_t fleet);
esp_err_t       rbamp_fleet_excluded_addr(rbamp_fleet_t fleet,
                                          size_t index,
                                          uint8_t *out);
```

`_get` returns `NULL` on out-of-range; `_find` returns `NULL` if the
address is not in the fleet (for example, because it ended up in
excluded). `_add` lets you manually add a handle created separately
(for example, after manual provisioning).

### Polling — `rbamp_fleet_poll_all`

```c
esp_err_t rbamp_fleet_poll_all(rbamp_fleet_t fleet,
                               rbamp_snapshot_t *snaps,
                               rbamp_fleet_poll_t *status,
                               size_t capacity,
                               size_t *n_ok);
```

Polls **all** modules in the fleet in a single call. For each module
a full RT read (`rbamp_read_all`) is performed, attaching the result
to the corresponding `snaps[i]` element. In parallel, `status[i]`
receives the per-module result: `ESP_OK`, `ESP_ERR_TIMEOUT` (module
silent), `ESP_FAIL` (sanity reject), etc.

After return, `*n_ok` is the number of modules for which a valid
snapshot was obtained (status[i] == ESP_OK).

**MISS-resilient**: one silent module does not cancel polling the
rest. This matters for a production fleet — losing a single module
(a wiring failure, a burnout) must not knock out the entire
monitoring.

### Aggregation

```c
esp_err_t rbamp_fleet_total_power(rbamp_fleet_t fleet, float *out_w);
esp_err_t rbamp_fleet_total_energy_wh(rbamp_fleet_t fleet, double *out_wh);
esp_err_t rbamp_fleet_poll_errors(rbamp_fleet_t fleet,
                                  uint32_t *error_mask,
                                  size_t *n_errors);
```

`_total_power` sums the active power across all channels of all
modules. On I variants there is no active power → they contribute
`0`. In the canonical deployment (UI1 mains + I2/I3 sub-meters) the
result is the active power of the mains module.

`_total_energy_wh` similarly sums the handles' Wh accumulators. It
also works correctly: I variants contribute `0`, UI1 contributes
billing energy.

`_poll_errors` iterates over the fleet, calling `rbamp_has_error` on
each module. `*error_mask` is a bitmask of the modules flagged with
`EVENT bit3`, by their position in the fleet (meaningful for the
first 32 modules); `*n_errors` is the total number of modules with an
error flag at the moment of polling. Either pointer may be passed as
`NULL` if not needed.

### Addressing and conflict checking

```c
esp_err_t rbamp_fleet_assign_address(rbamp_fleet_t fleet,
                                     rbamp_handle_t dev,
                                     uint8_t new_addr);
esp_err_t rbamp_fleet_check_conflict(rbamp_fleet_t fleet,
                                     uint8_t addr,
                                     bool *collision);
```

`_assign_address` re-addresses an existing fleet module to a new
address: internally — two-phase commit + reset; the handle in the
fleet is updated automatically (subsequent calls keep working).

`_check_conflict` — a best-effort read-only check **for a specific
address**: call it once per address of interest. It reads identity
(`PRODUCT_ID` / `HW_VARIANT` / `CAPABILITY`) and compares it against
the fleet's cached state. If an inconsistency is detected (two
devices answering on the same address with "merged" bytes; identity
differing from the cached state) — `*collision = true`. To sweep all
known addresses, iterate over the fleet yourself: `for i in
0..count, check_conflict(fleet, addr, &col)`. It does **not
guarantee** catching identical modules — see the provisioning
discipline.

### Provisioning — `rbamp_provision`

```c
esp_err_t rbamp_provision(i2c_master_bus_handle_t bus,
                          uint8_t desired_addr,
                          bool save_config,
                          rbamp_handle_t *out);
```

Provisions **exactly one** virgin module (at factory `0x50`) onto
the specified address. Internally:

1. Checks that there is a response at `0x50` (`ESP_ERR_NOT_FOUND` if
   not).
2. Checks that there is **no one** yet at `desired_addr`
   (`ESP_ERR_INVALID_STATE` if occupied).
3. Performs a two-phase address commit.
4. Resets the module and waits for it to appear at `desired_addr` in
   the boot window (`ESP_ERR_TIMEOUT` if it does not appear).
5. Creates a handle; if `save_config = true` — calls
   `rbamp_save_user_config` (persist to flash).
6. Returns the ready handle via `*out`.

> ⚠ **MUST: exactly one virgin on the bus at the moment of the
> call.** Two modules at `0x50` at the same time are physically
> indistinguishable over I²C — they cannot be distinguished even
> with special hardware. The guarantee is on the operator, not the
> library. For recovery, see
> [10 · Troubleshooting](10_troubleshooting.md), section "Fleet:
> rbamp_provision returned an error".

### Fleet GC sync — synchronous snapshots across the whole fleet

```c
esp_err_t rbamp_fleet_enable_gc_all(rbamp_fleet_t fleet,
                                    uint8_t group,
                                    size_t *ok_count);
esp_err_t rbamp_fleet_gclatch(rbamp_fleet_t fleet,
                              uint8_t group,
                              uint16_t tick,
                              uint32_t settle_ms);
esp_err_t rbamp_fleet_check_sync(rbamp_fleet_t fleet,
                                 uint16_t expected_tick,
                                 rbamp_fleet_sync_t *status,
                                 size_t status_cap,
                                 size_t *n_missed);
```

For billing-grade snapshot synchrony (skew < 1 ms across the whole
fleet) an I²C General-Call broadcast latch is used.

1. `_enable_gc_all` sets `FLEET_CONFIG.bit0` + `GROUP_ID` on all
   modules in the fleet and issues `CMD_SAVE_USER_CONFIG` on each
   (persistent). A reboot of the modules is **mandatory** after
   enable — this is done automatically. It is enough to call it once
   during the initial fleet setup. `*ok_count` (optional) is the
   number of modules for which enable succeeded.
2. `_gclatch` sends the GC frame `A5 27 group tick_lo tick_hi` to
   address `0x00` — all modules with GC enabled and a matching
   `group` instantly perform `LATCH_PERIOD`. `settle_ms` is the pause
   after the frame (typically 50 ms) before verification.
3. `_check_sync` reads `GC_TICK (0x59)` on each module into
   `rbamp_fleet_sync_t status[status_cap]`. The per-module result:

   ```c
   typedef struct {
       uint8_t  addr;        /* module I²C address */
       uint16_t gc_tick;     /* REG_GC_TICK; 0xFFFF = never received a GC */
       bool     in_sync;     /* true if gc_tick == expected_tick */
       bool     reachable;   /* false if the read NACKed / timed out */
   } rbamp_fleet_sync_t;
   ```

   `*n_missed` is the number of modules with `in_sync = false` (or
   simply not responding). If all are `in_sync`, `n_missed == 0`.

> Don't confuse them: the per-device `rbamp_broadcast_latch` (see the
> "Multi-module bus" section below) is **one** frame across the bus;
> fleet GC sync is **the same** frame, but the fleet API additionally
> provides a witness check (that it landed on all expected modules).
> See also [04 · Wiring](04_hardware.md) on the GC preconditions.

## Per-device fleet config

These functions work with a **single** module directly (without a
fleet) — for cases where you configure an individual module before
adding it to a fleet, or want fine-grained control.

### `rbamp_enable_gc` / `rbamp_read_fleet_config`

```c
esp_err_t rbamp_enable_gc(rbamp_handle_t dev, bool enable);
esp_err_t rbamp_read_fleet_config(rbamp_handle_t dev, uint8_t *out);
```

`enable_gc(dev, true)` writes `1` to `FLEET_CONFIG.bit0 (0x27)` — the
module will start accepting GC latches after the next
`CMD_SAVE_USER_CONFIG` + reset. `enable_gc(dev, false)` mirrors this
by clearing the bit (the module stops accepting GC after reset).
`read_fleet_config` returns the current config byte — useful for
read-back confirmation.

### `rbamp_set_group_id` / `rbamp_read_group_id`

```c
esp_err_t rbamp_set_group_id(rbamp_handle_t dev, uint8_t group_id);
esp_err_t rbamp_read_group_id(rbamp_handle_t dev, uint8_t *out);
```

`GROUP_ID (0x28)` — the GC-frame filter: the module reacts only to
GC frames with a matching group_id. Default = `0x00` (broadcast,
reacts to any). Used for multi-tenant buses — for example, a "phase
A" group and a "phase B" group on one shared bus.

### `rbamp_read_gc_tick`

```c
esp_err_t rbamp_read_gc_tick(rbamp_handle_t dev, uint16_t *out);
```

Reads `GC_TICK (0x59)` — the last tick value received from a GC
frame. Used for missed-frame detection (if two modules have a
different tick, someone missed a frame).

### `rbamp_read_label` / `rbamp_write_label`

```c
esp_err_t rbamp_read_label(rbamp_handle_t dev, char out[9]);
esp_err_t rbamp_write_label(rbamp_handle_t dev, const char *label);
```

`LABEL (0x68..0x6F)` — an 8-byte user-label string (ASCII,
zero-padded). It helps identify modules in a fleet — for example,
"MAINS", "BOILER", "AC1". `read_label` always returns a 9-byte array
(8 data bytes + a terminating `\0`). `write_label` takes a
null-terminated string — extra bytes are truncated.

> Writing is **byte-at-a-time** (no auto-increment on write — see
> "Register writes" below). After writing, you need
> `rbamp_save_user_config` so the label survives a reset.

### `rbamp_broadcast_latch_group`

```c
esp_err_t rbamp_broadcast_latch_group(i2c_master_bus_handle_t bus,
                                      uint8_t group_id,
                                      uint16_t tick);
```

A low-level send of the GC frame `A5 27 group tick_lo tick_hi`
directly through the `bus` handle (without a fleet). Useful if the
fleet has not been created yet, or if you want manual control over
the tick counter.

### `rbamp_reset_counters`

```c
esp_err_t rbamp_reset_counters(rbamp_handle_t dev);
```

Resets the module's diagnostic counters (i2c-error count,
peripheral-reinit count). It does not affect user-config or
calibration. Used for bench tests and periodic counter resets in
long production soaks.

## Sensor configuration

A two-step sequence: first the sensor class, then the model. **Both
steps are required.**

For a detailed model-selection guide, see
[03 · Current sensor selection](03_sensor_selection.md).

### `rbamp_set_sensor_class`

```c
esp_err_t rbamp_set_sensor_class(rbamp_handle_t dev,
                                 rbamp_sensor_class_t cls);
```

Sets the current-sensor class with a save to flash. Blocking,
~705 ms (writing `REG_SENSOR_CLASS` + `CMD_SAVE_GAINS` + a 700 ms
flash erase).

It must be called **before** `rbamp_set_ct_model*()`. Otherwise
those functions return `ESP_ERR_INVALID_STATE`. It also resets
`REG_CT_MODEL` to 0 on the device side.

Passing `RBAMP_SENSOR_UNSET` is accepted, but it effectively clears
the class configuration — a subsequent `rbamp_set_ct_model*()` will
again require a valid class. See
[10 · Troubleshooting](10_troubleshooting.md), section "Current
reads zero", for the "accidentally wrote UNSET" symptom.

**Returns**: `ESP_OK` on success; `ESP_ERR_INVALID_ARG` if `cls` is
not one of {`UNSET`, `SCT013`} (the reserved values are not
supported on the current SKU); I²C error codes on a link failure.

### `rbamp_set_ct_model`

```c
esp_err_t rbamp_set_ct_model(rbamp_handle_t dev, uint8_t code);
```

The single-parameter form — sets the CT-clamp model **on channel 0
only**. Convenient for single-channel SKUs; on multi-channel ones it
is equivalent to `rbamp_set_ct_model_ch(dev, 0, code)`. Blocking,
~705 ms.

| `code` | Model |
|:---:|---|
| 1 | SCT-013-005 |
| 2 | SCT-013-010 |
| 3 | SCT-013-030 |
| 4 | SCT-013-050 |
| 5 | SCT-013-100 |

`code` outside the range 1..5 → `ESP_ERR_INVALID_ARG`.

> **Precondition.** `rbamp_set_sensor_class()` must have been called
> successfully before this function — otherwise it returns
> `ESP_ERR_INVALID_STATE`.

For multi-channel modules, use the per-channel form below.

### `rbamp_set_ct_model_ch`

```c
esp_err_t rbamp_set_ct_model_ch(rbamp_handle_t dev,
                                uint8_t channel, uint8_t code);
```

The per-channel form. Sets the CT-clamp model on a specific channel.

Under the hood: write `REG_CT_MODEL` → command
`CMD_SET_CT_MODEL_CH0/1/2` (0x28/0x29/0x2A) according to `channel` →
5 ms settle → `CMD_SAVE_GAINS` → 700 ms. Blocking, ~705 ms.

> **Important: call order matters.** Writing `REG_CT_MODEL`
> also triggers a reg-write side effect that unconditionally applies
> the preset to channel 0. That is, `rbamp_set_ct_model_ch(dev, 1, code)`
> correctly configures channel 1, but **also clobbers channel 0**
> with the same preset.
>
> To correctly configure all channels with different models, call
> them **in descending channel-index order**:
>
> ```c
> rbamp_set_ct_model_ch(dev, 2, 5);  /* channel 2 = SCT-013-100 */
> rbamp_set_ct_model_ch(dev, 1, 3);  /* channel 1 = SCT-013-030 */
> rbamp_set_ct_model_ch(dev, 0, 1);  /* channel 0 = SCT-013-005 */
> ```

`ESP_ERR_INVALID_STATE` guards the `rbamp_set_sensor_class()`
precondition.

### `rbamp_save_gains`

```c
esp_err_t rbamp_save_gains(rbamp_handle_t dev);
```

A "bare" `CMD_SAVE_GAINS` write with no accompanying register
changes.

> ⚠ **Normally called inside the component.** A bare
> `rbamp_save_gains()` is appropriate ONLY if the caller manually
> wrote to non-public calibration registers via raw bus access —
> this is an out-of-warranty operation, it bypasses the per-SKU
> preset table. Incorrect values will produce wrong current/power
> readings with no explicit warnings. Each call performs a flash
> erase+write cycle (~700 ms); flash endurance is finite (~10,000
> cycles per page) — **do not call it in a loop.**

### `rbamp_prepare_address_change` / `rbamp_commit_address_change`

Changing the module's I²C address is done via a **two-phase commit**
in production mode. The firmware deliberately excludes the address
from the general `CMD_SAVE_USER_CONFIG` / `CMD_SAVE_GAINS` flow — a
change requires an explicit magic-armed sequence.

```c
esp_err_t rbamp_prepare_address_change(rbamp_handle_t dev,
                                       uint8_t new_addr);
esp_err_t rbamp_commit_address_change(rbamp_handle_t dev);
```

**Wire protocol under the hood:**

1. `prepare`: validate the range of `new_addr` (0x08..0x77, ≠
   current) → write `new_addr` to `REG_I2C_ADDRESS` (0x30) — it
   lands in RAM.
2. `prepare`: write `0xA5` to `REG_ADDR_COMMIT_MAGIC` (0x31) — this
   "arms" the commit. The armed-state window is 5 seconds.
3. `commit`: check the freshness of the arming → send
   `CMD_COMMIT_ADDR` (opcode 0x30) → persist to flash → `CMD_RESET`
   + ~400 ms boot → update the handle's internal address field.

After a successful `commit`, all calls on this handle instance are
addressed to the new address. The operation works in production mode
(not gated), which supports the field-swap of production-spare
modules.

> ⚠ **If the armed window (5 s) has expired** — `commit` returns
> `ESP_ERR_TIMEOUT`, the arming flag is cleared, and you must start
> over with `prepare`.

> ⚠ **Persistence is confirmed only after a reset.** Before the
> reset, `rbamp_read_*` may return the new values from RAM, but the
> flash write might not have finished. The component itself performs
> a built-in `CMD_RESET` + boot-wait inside `commit`, so after a
> successful return the state is guaranteed persisted.

> ⚠ **Restart and re-enumeration after commit.** After a successful
> commit, the device resets and re-enumerates at the NEW address.
> Subsequent calls on this handle instance are addressed to the new
> address transparently — but any OTHER master on the bus (a Python
> script, an ESP-IDF component on another MCU, a debug probe) will
> keep thinking the device is at the old address until its internal
> state is updated manually.

### `rbamp_factory_reset`

```c
esp_err_t rbamp_factory_reset(rbamp_handle_t dev);
```

The `CMD_FACTORY_RESET` (0xAA) command + a 1500 ms wait.

> ⚠ **Destructive operation.** It erases ALL flash parameters (CT
> model, sensor class, calibration coefficients, I²C address). The
> module returns to factory defaults — `rbamp_sensor_class_t` becomes
> `RBAMP_SENSOR_UNSET`, `REG_CT_MODEL` becomes 0. Any setting
> previously applied via `rbamp_set_sensor_class()` /
> `rbamp_set_ct_model*()` disappears. The next user MUST re-apply
> both `rbamp_set_sensor_class()` and `rbamp_set_ct_model*()` before
> accounting becomes operational again. This is **not a "soft
> restart"** — for a soft restart use `rbamp_reset()`.
> `rbamp_factory_reset()` is reserved for recovery from a known-bad
> state or handing the module over to another user / installation.

### `rbamp_reset`

```c
esp_err_t rbamp_reset(rbamp_handle_t dev);
```

The `CMD_RESET` (0x01) command + a 100 ms wait. A soft restart of
the device without losing flash parameters.

## Multi-module bus

### `rbamp_broadcast_latch`

```c
esp_err_t rbamp_broadcast_latch(i2c_master_bus_handle_t bus,
                                uint32_t timeout_ms);
```

Reserved for future firmware versions. On v1 firmware the function
**always returns `ESP_ERR_NOT_SUPPORTED`** without touching the bus
(General-Call is disabled in the v1 module's I²C peripheral).

To synchronize the period across several modules, use a sequential
series of `rbamp_latch_period()` on each device, a common 50 ms
settle, and a per-device
`rbamp_read_period_snapshot(skip_latch=true)` — see
[06 · Examples](06_examples.md), scenario "Monitoring multiple
modules".

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

Typical use:

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

Public getters for two diagnostic counters:

- **`rbamp_retry_exhaustion_count(dev)`** — a monotonic counter of
  "retry budget exhausted" events. Each increment = one single-byte
  transaction that, after `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS`
  attempts, never got an ACK from the device.
- **`rbamp_sanity_reject_count(dev)`** — a monotonic counter of
  rejections by the sanity filter on float reads
  (`!isfinite(x) || |x| > 10000`). Each increment = one float that
  came off the bus, was judged not to resemble a physical value, and
  was discarded.

Both counters **only increase** — they are never reset (except by
re-creating the handle via `rbamp_del` + `rbamp_new`). The canonical
"rate over an interval" pattern is snapshot + diff: take snapshots at
T₀ and T₁, subtract for the delta:

```c
uint32_t prev = rbamp_retry_exhaustion_count(dev);
/* ...60 s of normal operation... */
uint32_t delta = rbamp_retry_exhaustion_count(dev) - prev;
if (delta > THRESHOLD) { /* alert */ }
```

These names match the Arduino library cross-platform
(`dev.retryExhaustionCount()` / `dev.sanityRejectCount()` — the same
semantics, snake_case). For practical workloads and alarm
thresholds, see [10 · Troubleshooting](10_troubleshooting.md),
section "Monitoring counters".

### Logging via `esp_log`

The component logs under the tag `"rbamp"`. The log level is
compiled in via Kconfig — the runtime
`esp_log_level_set("rbamp", ...)` works but is capped from above by
the compile-time level.

```c
esp_log_level_set("rbamp", ESP_LOG_DEBUG);   /* for debugging */
```

## Error codes

`rbamp_*` functions return standard `esp_err_t` values. The specific
codes and their semantics in the rbamp context:

| Code | When | Where to look |
|---|---|---|
| `ESP_OK` (0) | Success | — |
| `ESP_FAIL` (-1) | The I²C transaction failed after retries; or the sanity filter rejected a value | wiring, power, bus speed — section "The module does not respond over I²C" in [10 · Troubleshooting](10_troubleshooting.md) |
| `ESP_ERR_INVALID_ARG` (0x102) | Invalid argument: `dev == NULL`, `channel` or `phase` out of range, `code` outside 1..5, a reserved `cls` | check the call arguments |
| `ESP_ERR_TIMEOUT` (0x107) | `rbamp_wait_ready` expired; the `rbamp_commit_address_change` window (5 s) expired | check responsiveness via `rbamp_probe()` |
| `ESP_ERR_INVALID_STATE` (0x103) | Precondition not met: `set_ct_model*` before `set_sensor_class` | section "`rbamp_set_*` returns `ESP_ERR_INVALID_STATE`" in [10 · Troubleshooting](10_troubleshooting.md) |
| `ESP_ERR_NOT_SUPPORTED` (0x106) | The function is unavailable on this SKU (for example, the voltage API on an I* variant) | check `rbamp_hw_variant(dev)` |
| `ESP_ERR_INVALID_RESPONSE` (0x108) | Period snapshot stale (ready flag = 0); or `REG_VERSION` = 0/0xFF on `rbamp_begin` | section "Period snapshots are always stale" / "Cannot exit rbamp_begin" in [10 · Troubleshooting](10_troubleshooting.md) |
| `ESP_ERR_NO_MEM` (0x101) | Could not allocate memory for the handle | check the heap budget |

All returned codes can be printed human-readably via
`rbamp_err_to_str()`.

## Compile-time configuration (Kconfig)

Six `CONFIG_RBAMP_*` symbols are available in `idf.py menuconfig` →
**Component config → rbAmp client**:

| Symbol | Type | Range / choice | Default | Purpose |
|---|---|---|---|---|
| `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ` | int | 50000–1000000 Hz | 50000 | SCL frequency. 50 kHz is the mandate for the current firmware on ESP32. |
| `CONFIG_RBAMP_I2C_TIMEOUT_MS` | int | 10–1000 ms | 100 | `i2c_master_transmit/receive` timeout. |
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
# leave CONFIG_RBAMP_DISABLE_ENERGY disabled (default n)
```

For a bench test under heavy load:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=5
CONFIG_RBAMP_I2C_TIMEOUT_MS=200
CONFIG_RBAMP_LOG_LEVEL_DEBUG=y
```

## Settle-time constants

The component uses the following settle times internally when
executing commands (for a user working on top of the raw API via
`i2c_master_transmit_receive` who wants to honor the same timings):

The library exports a small set of settle-time constants (ms) for
raw-API users who send commands to the device directly and must wait
until the device finishes processing before reading the affected
registers. The component's wrapper functions (`rbamp_save_gains`,
`rbamp_factory_reset`, `rbamp_set_ct_model_ch`, etc.) hold the
correct delay internally — these constants are only needed if you
bypass the wrappers.

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
[`include/rbamp.h`](../include/rbamp.h). The numbers are the
specification minimums of the rbAmp protocol; you cannot go shorter,
you may go longer.

## Wire-protocol details

For porting to platforms where the component is not used, or for
debugging on a logic analyzer:

### Register reads — auto-increment

A burst read `<addr> + N bytes` returns N consecutive bytes starting
at `addr`. Example: a single transaction `i2c_master_transmit_receive(slave, {0x86}, 1, buf, 16, ...)` reads
16 bytes starting at 0x86 (U_RMS + U_PEAK + I_RMS + I_PEAK on UI1).

### Register writes — byte-at-a-time (NOT auto-increment)

⚠ **Writes are NOT auto-increment.** A burst WRITE `<addr> + N bytes` lands **only the first byte** — bytes 1..N are dropped.

**A multi-byte register is written byte by byte** — a separate single-byte I²C transaction per address:

```c
/* WRONG — data loss */
uint8_t label[8] = "MyMeter1";
uint8_t buf[9] = { 0x68, 'M', 'y', 'M', 'e', 't', 'e', 'r', '1' };
i2c_master_transmit(slave, buf, 9, 100);   /* only 'M' lands */

/* CORRECT — byte-at-a-time loop */
for (int i = 0; i < 8; i++) {
    uint8_t tx[2] = { 0x68 + i, label[i] };
    i2c_master_transmit(slave, tx, 2, 100);
}
```

This affects writable multi-byte registers: `LABEL` (0x68-0x6F), `U_GAIN` / `I_GAIN` (f32, develop-gated), `NF` (u16), `THRESH` (u16). Reads are burst-OK (auto-increment works on READ).

### Reading unmapped registers

Any read from an unmapped address returns `0x00`. The firmware
**never** NACKs on register reads. Detection of a missing device
must go through the I²C address-frame ACK, **not** through register
probing.

### Variant detection

The authoritative SKU byte is register `0x55` (`rbamp_hw_variant()`):

| value | SKU |
|---|---|
| `0x01` | UI1 (1× I + U) |
| `0x02` | UI2 (2× I + U) |
| `0x03` | UI3 (3× I + U) — **roadmap, not buildable on the current MCU package** |
| `0x04` | I1  (1× I, no U) |
| `0x05` | I2  (2× I, no U) |
| `0x06` | I3  (3× I, no U) |

Register `0x24` (`rbamp_topology()`) is auxiliary; it returns only the channel count (1/2/3) and does not distinguish the presence of U.

> ⚠ **`REG_SENSOR_CLASS (0x25)` is NOT a variant discriminator** (it is user-config provisioned). The variant invariant = **HW_VARIANT (0x55) + CAPABILITY (0x57/0x58)** — both read-only.

### CAPABILITY (0x57/0x58, u16 LE) — feature-detect map

A 12-bit map of the device's capabilities. The canonical way to feature-detect (instead of a version-numerical compare):

| bit | flag | meaning |
|---|---|---|
| 0 | EXT_ADDRESSING | extended addressing |
| 1 | GC_LATCH | GC latch supported |
| 2 | GC_GROUP_FILTER | GROUP_ID filter works |
| 3 | DIGEST | digest block (0x70-0x85) |
| 4 | EVENTS | EVENT_FLAGS (0x2A) supported |
| 5 | UID_ARBITRATION | UID arbitration |
| 6 | SEAL | seal mechanism |
| 7 | TWO_PHASE_ADDR | two-phase address change |
| 8 | **ZC_PHASE_OFFSET = voltage-HW** | the module has a U channel |
| 9 | SAVE_USER_CONFIG | `CMD_SAVE_USER_CONFIG` |
| 10 | CLEAR_ERROR | `CMD_CLEAR_ERROR` |
| 11 | IAP | in-application programming |

**Pseudocode for `has_voltage`** (without reading HW_VARIANT):

```c
/* Option 1 — read only the high byte (bit 8 = bit 0 of byte 0x58). */
uint8_t cap_hi;
rbamp_read_u8(dev, 0x58, &cap_hi);
bool has_voltage = (cap_hi & 0x01) != 0;

/* Option 2 — read the full 16-bit CAPABILITY (LE). */
uint16_t cap;
rbamp_read_capability(dev, &cap);
bool has_voltage_2 = (cap & 0x0100) != 0;
```

Both options are equivalent. A full 16-bit read of `0x57..0x58` (LE) yields the canonical `0x069E` / `0x079E` (see the table below).

**HW_VARIANT ⟷ CAPABILITY coherence**: HW_VARIANT 1..3 (UI*) → bit8 set; HW_VARIANT 4..6 (I*) → bit8 clear. An inconsistency = a hardware anomaly.

**Canonical CAPABILITY values** (root 2026-06-16):

| Variant group | CAPABILITY (0x57/0x58 u16 LE) |
|---|---|
| Current-only (I1/I2/I3) | `0x069E` |
| U variants (UI1/UI2) | `0x079E` |

`0x079E` = `0x069E | 0x0100` (bit8 voltage-HW added).

### Atomicity / read-freeze (B1)

- **V03 float RMS block** (0x86+: U_RMS, U_PEAK, I_RMS, I_PEAK, P, PF, freq) — **read-freeze present**, the burst read is atomic.
- **Digest block** (0x70-0x85) — read-freeze present.
- **GC_TICK (0x59) / address-mirror (0x30) / CT-model mirrors (0x51-0x53)** — read-freeze **absent**; for a reliable read, use the **read-twice-agree** pattern (read twice, consider the value valid only if they match).

### NF-clamp semantics

`I_RMS = 0x00000000` means "**signal below the noise floor**" (the FW computes `rms_corr² = max(0, raw² − nf²)`), NOT "no data" / "error". This is a **valid measurement** for no-load scenarios.

### Voltage-only registers on current-only variants

`U_RMS`, `U_PEAK` on I1/I2/I3 read as `0`. They are valid only when `CAPABILITY` bit8 = 1 (voltage-HW). `REG_AC_FREQ` is ZC-sourced and available **on all variants** (including I*).

### General-Call broadcast latch (multi-module synchronization)

**Opt-in per-module** via `bit0` of register `REG_FLEET_CONFIG (0x27)` — `GC_ENABLE`. **Default OFF.**
Enabled via `CMD_SAVE_USER_CONFIG` + `CMD_RESET` (soft or hard). After a reset
the module listens for the broadcast frame on the I²C general-call address `0x00`:

```
addr=0x00 | A5 27 <group> <tick_lo> <tick_hi>
```

- `A5` — magic.
- `27` — the `CMD_LATCH_PERIOD` opcode.
- `<group>` — `0x00` = all-call, otherwise must match `REG_GROUP_ID` (0x28).
- `<tick_lo/hi>` — a 16-bit period counter (master sync).

The frame only latches the period; it never carries destructive opcodes.

**Failure-mode detection (two-level, C.10 HW-confirmed)**:

1. **GC-address NACK** (immediate, at the bus level): if GC is disabled across the **whole fleet** — `i2c_master_transmit` to `0x00` returns `ESP_FAIL` / `ESP_ERR_INVALID_STATE`. This is a **hard error, not a silent drop**. The lib treats the NACK as "GC not enabled" (it requires the enable+reset preconditions).
2. **Per-module witness** (after settle): if no NACK occurred — the master reads `REG_V03_PERIOD_VALID (0x07)` on each expected slave: `1` = latch succeeded, `0` = GC disabled on this specific module, or the period is empty.

## Links

- [05 · Quickstart](05_quickstart.md) — your first working project
- [06 · Examples](06_examples.md) — working scenarios
- [10 · Troubleshooting](10_troubleshooting.md) — decoding errors and
  resolving common problems
- The source header file with Doxygen comments:
  [`include/rbamp.h`](../include/rbamp.h)

