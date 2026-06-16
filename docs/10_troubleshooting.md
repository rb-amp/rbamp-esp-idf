# 10 · Troubleshooting

This chapter is organized by **the symptoms you observe**, not by
internal error classification. If you're here, something isn't
working right. Find your symptom in the list below, jump to the
matching section, and follow the diagnostic procedure.

Most bench problems fall into three categories:

1. **Bus-level** — the module doesn't respond over I²C, reads return
   `ESP_FAIL` or `ESP_ERR_INVALID_RESPONSE`.
2. **Data-level** — the module responds, but the numbers are wrong:
   zeros, strange values, wrong sign.
3. **Application-level** — the link is unstable, the Wh counter
   drifts, the project hangs.

Shortcut for the impatient: in steady state, the public `rbamp_*`
functions should return `ESP_OK`. If you frequently see `ESP_FAIL`
or `ESP_ERR_INVALID_RESPONSE` in the logs, jump straight to the
"Module doesn't respond over I²C" section below.

## Module doesn't respond over I²C

**What you see:** `rbamp_begin(dev)` returns `ESP_FAIL` or
`ESP_ERR_INVALID_RESPONSE`, or RT reads regularly propagate
`ESP_FAIL` into your code.

### Step 1 — Bus scan

First, confirm the module is even present on the bus. ESP-IDF
v5.x provides `i2c_master_probe()` for exactly this purpose:

```c
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "i2c_scan";

void bus_scan(i2c_master_bus_handle_t bus) {
    ESP_LOGI(TAG, "Scanning...");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, /*xfer_timeout_ms=*/100) == ESP_OK) {
            ESP_LOGI(TAG, "Found 0x%02X", addr);
        }
    }
}
```

Expected output: `Found 0x50` (or another address, if you changed it).

**If nothing is found** — the problem is in the wiring or power:

- SDA / SCL are not swapped (see [04 · Wiring](04_hardware.md)
  for per-chip GPIO defaults).
- Both lines have a pull-up to 3.3 V (the module board has built-in
  4.7 kΩ — for a single module, external ones aren't needed).
- The module's power pin actually has 5 V (4.5–5.5 V).
- No other master (debug probe, second ESP32) is sitting on the
  same lines.
- `flags.enable_internal_pullup = true` in your
  `i2c_master_bus_config_t` (if there are no external pull-ups).

**If the module is found but at an unexpected address** — someone
previously re-addressed it on the bench. Update the second argument
of `rbamp_new`:

```c
rbamp_new(bus, 0x52, &dev);   /* address from the bus scan */
```

### Step 2 — ESP32 baseline NACK pattern

If the bus scan finds the module but `rbamp_read_voltage(dev, 0, &v)`
periodically returns `ESP_FAIL` — this is a **documented baseline
pattern**: the ESP-IDF v5 `i2c_master` driver has quirks when
working with rbAmp on the current firmware (~20 % NACK rate at
100 kHz).

The mitigation is already built into the component
(`CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=3` by default, 50 kHz via
`CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000`), but under heavy load the
retry budget can run out.

**What to do via `idf.py menuconfig` → Component config → rbAmp client:**

1. **Confirm the 50 kHz default**:

   ```text
   CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000
   ```

2. **Raise the retry count for heavy load**:

   ```text
   CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=5
   ```

3. **If needed — increase the timeout**:

   ```text
   CONFIG_RBAMP_I2C_TIMEOUT_MS=200
   ```

4. **Enable DEBUG level for the `rbamp` tag** for diagnostics:

   ```text
   CONFIG_RBAMP_LOG_LEVEL_DEBUG=y
   ```

   You can also do this at runtime — `esp_log_level_set("rbamp", ESP_LOG_DEBUG)`.

> **About the retry/sanity counters.** The component exports public
> getters `rbamp_retry_exhaustion_count(dev)` and
> `rbamp_sanity_reject_count(dev)` — they match the Arduino
> `dev.retryExhaustionCount()` / `dev.sanityRejectCount()`
> cross-platform. The canonical snapshot + diff pattern for
> production monitoring is described in the "Monitoring the counters"
> section below.

## Fleet: a module ended up in `excluded` after `rbamp_fleet_scan`

**What you see:** `rbamp_fleet_scan(fleet, ...)` ran and returned
`ESP_OK`, but `rbamp_fleet_count(fleet)` is less than the number of
modules you physically connected. `rbamp_fleet_excluded_count(fleet)`
> 0; iterate over `rbamp_fleet_excluded_addr(fleet, i)` —
each excluded address means "a module was detected but NOT added to
the fleet — a conflict is suspected".

**Cause.** The conflict detector works on two independent signals:
- **identity-consistency**: HW_VARIANT ↔ CAPABILITY bit-map are inconsistent — a sign that two different physical modules are answering at one address (the response partially "fuses").
- **live-value**: at one address, incompatible "live" readings are
  observed — for example, a differing `LABEL` or a flapping variant.

If at least one of the signals fires, the address is excluded from
the fleet, and the library does NOT rely on that module's readings.
If both signals are quiet but the modules are **identically**
configured (same variant, same CT_MODEL, same LABEL), the detector
**won't tell them apart** — this is a physically undetectable case
(see below on discipline).

> ⚠ Detection is **best-effort**. It's impossible to guarantee the
> fleet catches two **identical** modules at one address — which is
> exactly why provisioning discipline exists (see the next section).

**What to do.**

1. Physically disconnect all modules except one.
2. Connect the rest **one at a time**, calling `rbamp_fleet_scan`
   again after each. The module after whose connection an `excluded`
   appears is conflicting with one already connected.
3. Re-provision the conflicting module onto a free address via
   `rbamp_provision(bus, new_address, ...)` (requires that it be the
   only one on `0x50` — see provisioning discipline below).
4. After re-addressing, repeat `rbamp_fleet_scan` — `excluded_count`
   should be `0`.

## Fleet: `rbamp_fleet_scan` returned `ESP_ERR_INVALID_STATE` + empty fleet

**What you see:** `rbamp_fleet_scan` returned `ESP_ERR_INVALID_STATE`,
`rbamp_fleet_count(fleet)` is 0. No modules were added to the fleet,
even though they're physically connected.

**Cause.** Tier-2 wedge-canary — the library performed a bus check
of "is there a response at an address that **must not** answer"
(the canary). If the canary got an ACK, the bus is not in a normal
state: either electrical noise is producing spurious ACKs, or all
addresses are "stuck" as a result of a wedge, or there's a topology
error (several modules with the same code at once, plus SDA/SCL being
held down). In that state you can't trust **any** scan result — so
the fleet stays empty; better to abort than to do a knowingly
incorrect initialization.

**What to do.**

1. **Power-cycle** the whole bus (cut power to the modules for a few
   seconds, restart the ESP32). This clears the stuck states.
2. Connect the modules **strictly one at a time**, running
   `rbamp_fleet_scan` (single-module bring-up) after each and
   checking the return:
   - `ESP_OK` + `count=1` + `excluded=0` → bus is clean, module OK.
   - `ESP_ERR_INVALID_STATE` persists → check the pull-ups
     (see the i2c-hang section above — an external 4.7 kΩ resistor on
     SDA/SCL is mandatory).
3. Once single-module bring-up passes for each module individually,
   add them all to the bus at once and run `rbamp_fleet_scan` again.
   If you get `INVALID_STATE` again, the problem is in the bus
   electrics (length, interference, a missing pull-up), not in the
   individual modules.

## Fleet: `rbamp_provision` returned an error

**What you see:** the call `rbamp_provision(bus, desired_address,
save_config, &out)` did not finish with `ESP_OK`. Depending on the
returned code:

| `esp_err_t` | What happened | What to do |
|---|---|---|
| `ESP_ERR_NOT_FOUND` | Nothing answered on `0x50` (factory default) | Confirm the virgin module is physically connected and powered (VCC); check the I²C wiring (see "Module doesn't respond over I²C"). |
| `ESP_ERR_INVALID_STATE` | A conflict was detected on `0x50` **or** the desired address is already taken | There's more than one virgin module on the bus **or** the desired address is already taken by another module. Disconnect all the "extra" modules (see below). |
| `ESP_ERR_TIMEOUT` | The address-commit command went through, but the module didn't appear at the new address within the boot window | Power-cycle the module and retry. If it recurs — likely an incomplete flash operation (power dropped during `CMD_SAVE_USER_CONFIG` or `CMD_COMMIT_ADDR`, so the flash block may be partially written). On the next boot the module returns `REG_ERROR = 0xFB (ERR_FLASH_PARAMS_BAD)`, loads factory defaults, and reappears at `0x50`. Re-provision from scratch; if that also fails, send it in for service. |
| `ESP_ERR_INVALID_ARG` | `desired_addr` is outside the valid range `0x08..0x77` | Use a valid address. |

> ⚠ **Provisioning discipline — MUST one virgin at a time.**
>
> The most common source of `INVALID_STATE` during provisioning is
> **more than one** module on the bus with the factory address `0x50`.
> Distinguishing them over I²C is **physically impossible**
> (open-drain wired-AND — both modules ACK identically, and the
> data read-back comes back "fused").
>
> **So the rule is strict (a normative MUST, not a recommendation):**
> at the moment of the `rbamp_provision` call there must be **exactly
> one** module on `0x50` (new, or returned to factory defaults via a
> separate bench tool). No "I'm sure these two modules are different —
> I'll just split them now" — it won't work; the collision will show
> up later, when you already think the fleet is clean.
>
> **Recovery path on a suspected discipline violation:**
> 1. Power-cycle all modules.
> 2. Physically disconnect all but the one "virgin" module (the one
>    you want to re-address).
> 3. Call `rbamp_provision` on the single-module bus.
> 4. Add the rest of the modules one at a time, provisioning each
>    separately.

## Current reads zero on a working load

**What you see:** `rbamp_read_current(dev, 0, &i)` returns
`ESP_OK` but `i = 0.000` (or a very small value), even though a real
consumer is on (kettle, lamp, iron).
`rbamp_read_power_factor(dev, 0, &pf)` may show a strange value
(`nan` / `0` / `±1`) — that's a consequence; the cause is in the
current.

### Diagnostic procedure

1. **Check that the sensor class and CT model are configured:**

   ```c
   esp_err_t err = rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
   if (err != ESP_OK) {
       ESP_LOGE(TAG, "set_sensor_class: %s", rbamp_err_to_str(err));
   }
   err = rbamp_set_ct_model(dev, 3);   /* or your model */
   if (err != ESP_OK) {
       ESP_LOGE(TAG, "set_ct_model: %s", rbamp_err_to_str(err));
   }
   ```

   Without these two calls the calibration coefficients aren't
   loaded, and current always reads as zero. This step is done
   **once** at first installation — the choice is saved to the
   module's flash.

   > **Gotcha about `RBAMP_SENSOR_UNSET`.** If you accidentally call
   > `rbamp_set_sensor_class(dev, RBAMP_SENSOR_UNSET)` — the function
   > returns `ESP_OK`, but effectively resets the class
   > configuration. A subsequent `rbamp_set_ct_model*()` will start
   > returning `ESP_ERR_INVALID_STATE`. The fix is to call
   > `rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013)` again, then
   > `rbamp_set_ct_model()` again.

2. **Check that the CT model matches the load.** A large CT clamp
   (for example, SCT-013-100, 100 A) on a small load (a 50 W lamp =
   ~0.2 A) produces a signal at the edge of the noise floor, and the
   readings will be zero. Pick the **smallest** CT model that covers
   your maximum expected current. The full table is in
   [03 · Current sensor selection](03_sensor_selection.md).

   If you have a multi-channel module (UI2 / UI3) and you want to see
   both small loads and peak spikes, consider a dual-CT pattern: a
   small clamp (e.g., SCT-013-005) on a dedicated channel for the
   low range + a large one (SCT-013-030/100) on another channel for
   the high range; the master picks the value by a threshold. The
   pattern is discussed in
   [03 · Current sensor selection](03_sensor_selection.md), section
   "Dual-CT topology".

3. **Check the clamp orientation.** The arrow on the clamp body must
   point **in the direction of current flow toward the load**. If
   the clamp is "backwards", `rbamp_read_current()` gives the correct
   value in absolute terms, but `rbamp_read_power()` gives a negative
   value on a consumer load. Confirmation:
   `rbamp_read_power_factor()` will show exactly −1.0 on a resistive
   load (instead of +1.0). The fix: either physically re-fit the
   clamp (unclip, turn it so the arrow points the right way, clip it
   back), or invert the sign on the master side.

### If all three steps pass but the current is still zero

Then the signal at the ADC really is below the noise floor. Check:

- **Is current actually flowing** — measure with a multimeter (DC
  clamp / AC clamp meter) on the same wire.
- **Is the clamp intact** — at the clamp connector there should be
  an AC voltage proportional to the current (a few millivolts for
  consumer loads).
- **Are you clamping the right wire** — the line (phase), not
  neutral (although a clamp on neutral will work too — it measures
  the current amplitude, not direction).

## Readings jump around or return `ESP_FAIL`

**What you see:** `rbamp_read_voltage(dev, ...)` or
`rbamp_read_current(dev, ...)` periodically return `ESP_FAIL`. The
value in `*out` is undefined after an error.

`ESP_FAIL` from the component covers two classes of problem:

- **NACK after retry exhaustion** — the link is unstable.
- **Sanity filter rejected the value** — a float `NaN` / `Inf` /
  `|x| > 10000` came off the bus (a clearly non-physical value).

The ESP-IDF API doesn't distinguish them via separate `esp_err_t`
codes (unlike Arduino's `RB_ERR_NACK` / `RB_ERR_NON_PHYSICAL`) — both
return `ESP_FAIL`. You can tell them apart by the **diagnostic
counters** (see the "Monitoring the counters" section below). Both
symptoms are treated the same way:

1. **Confirm the 50 kHz default** + raise retry to 5 via Kconfig —
   see the previous section.
2. **Reduce the load density on the bus** — if your loop does many
   single-byte reads, try `rbamp_read_all()` (one I²C transaction
   instead of ~13).
3. **Check `CONFIG_RBAMP_I2C_TIMEOUT_MS`** — if the device responds
   slower than usual (long cable, poor contact), increase the
   timeout.
4. **Enable DEBUG logging** via
   `CONFIG_RBAMP_LOG_LEVEL_DEBUG=y` or
   `esp_log_level_set("rbamp", ESP_LOG_DEBUG)` — the component will
   print details of the retry cycle and sanity rejects under the
   `"rbamp"` tag.

## Monitoring the counters

The component exports two public monotonic counters for production
bus-health monitoring:

```c
uint32_t rbamp_retry_exhaustion_count(rbamp_handle_t dev);
uint32_t rbamp_sanity_reject_count(rbamp_handle_t dev);
```

`retry_exhaustion_count` is incremented when a single-byte
transaction has exhausted the `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS`
budget and returned `ESP_FAIL`. `sanity_reject_count` is incremented
when the sanity filter discards a float (`!isfinite(x) || |x| > 10000`).

The canonical pattern is snapshot + diff once every 60 seconds, to
get an "event rate per minute":

```c
uint32_t prev_retry  = rbamp_retry_exhaustion_count(dev);
uint32_t prev_sanity = rbamp_sanity_reject_count(dev);
/* ...60 s of normal operation (do N rbamp_read_* calls)... */
uint32_t now_retry  = rbamp_retry_exhaustion_count(dev);
uint32_t now_sanity = rbamp_sanity_reject_count(dev);
uint32_t retry_delta  = now_retry  - prev_retry;
uint32_t sanity_delta = now_sanity - prev_sanity;
if (retry_delta > THRESHOLD || sanity_delta > THRESHOLD) {
    ESP_LOGW(TAG, "bus health: retry+%u sanity+%u per minute",
             retry_delta, sanity_delta);
}
```

**Alarm thresholds** depend on the workload. For a dense soak pattern
(like the Arduino SoakMonitor — ~70 bytes per cycle once every
5 minutes):

- `retry_delta > 1/hour` — benign noise, can be ignored.
- `retry_delta` consistently **> 5/hour** — worth investigating (bad
  contacts, bus capacitance, temperature drift of the pull-ups).
- `sanity_delta > 0` in steady state — almost always a sign of
  electromagnetic interference on the bus or a firmware regression.

The counters **only increase** (never reset) — to reset them you
must recreate the handle via `rbamp_del(dev)` +
`rbamp_new(...)`. This is by design, so you don't lose history to
races between "reset" and "read into the logger".

> Cross-platform, these counters match the semantics of the Arduino
> `dev.retryExhaustionCount()` / `dev.sanityRejectCount()` — the same
> names (snake_case vs camelCase), the same thresholds work the same
> way.

## Power Factor looks wrong

**What you see:** `rbamp_read_power_factor(dev, 0, &pf)` returns
`ESP_OK` but `pf` doesn't match the load type.

Expectations by load:

| Load | Expected PF |
|---|---|
| Kettle, iron, incandescent lamp | +0.95 .. +1.0 (resistive) |
| Fridge, compressor motor | +0.6 .. +0.85 (inductive) |
| LED lamp, TV (switching PSU) | +0.5 .. +0.95 (non-linear) |
| PV inverter exporting power | negative PF |

### PF = nan or 0 when I = 0

PF is defined as `P / (U × I)`. At zero current `I=0` the math is
undefined. The specific returned value depends on the firmware (it
may be `0` or a placeholder near zero) — this is normal as long as
the current really is zero. Once current appears, a valid PF
appears. **The component's sanity filter does not reject this value**
(so the `sanityRejectCount` equivalent in IDF doesn't increment) —
it's a valid measure of an undefined quantity, not a link artifact.

### PF strictly −1.0 on a purely consumer load

The clamp is fitted "backwards" — the arrow points away from the
direction of current flow toward the load. The fix: either re-fit
the clamp correctly, or handle the sign on the master side
(`p = -p; pf = -pf;` if you know the load can't be a PV inverter).

### PF floats between +0.3 and +0.7 on a resistive load

- **Possible cause:** the CT clamp is on a wire of a **different
  phase** than the one the module's U input is connected to. In a
  distribution panel with several phases it's easy to mistakenly
  clamp onto the wrong phase — a 120° or 180° phase shift between U
  and I gives exactly these PF values. The fix is to install the
  module so the U input and the CT are on the same phase.
- **Alternative:** the load really isn't purely resistive. Repeat the
  test with a known-resistive load (an electric kettle at full
  power).

## Period snapshots are always `stale`

**What you see:** `rbamp_read_period_snapshot(dev, ...)` returns
`ESP_ERR_INVALID_RESPONSE`, or `*out.valid == false`. This means the
module didn't finish integrating the previous period by the time of
the next read.

The component guards against double-counting Wh: on a stale, it
records a master timestamp (`esp_timer_get_time()`), and the next
successful snapshot covers one period, not two.

**Acceptable:** rare stales (1–2 per hour at a 60 s cadence).
**Not acceptable:** consecutive stales — that means the firmware is
unresponsive or the master is polling too often.

### Cadence-check procedure

1. **Check the cadence:** 60 s between latches is comfortable; 30 s
   is marginal; < 10 s guarantees stales.
2. **Check the module's responsiveness** between snapshots:

   ```c
   if (rbamp_probe(dev) == ESP_OK) {
       ESP_LOGI(TAG, "alive");
   }
   ```

3. **Check the flag directly** — `rbamp_is_period_valid()` does a
   single-byte read with no side effects:

   ```c
   bool valid;
   if (rbamp_is_period_valid(dev, &valid) == ESP_OK && valid) {
       /* avg_p[] can be read */
   }
   ```

4. **Check the firmware version** — `rbamp_firmware_version(dev) >= 0x02`
   shows fewer stales than 0x01.

### Special case — deep-sleep wake

If you use a deep-sleep pattern, the **default**
`rbamp_read_period_snapshot()` right after `rbamp_begin()` will
always be stale (or give near-zero values) — `begin()` does an
approximate LATCH that resets the firmware accumulator. The canonical
pattern uses `skip_latch=true` + the master's own RTC-saved
timestamp — see [06 · Examples](06_examples.md), Scenario 9.

## Wh accounting drifts from a reference

**What you see:** `rbamp_energy_wh(dev, 0)` after several hours /
days of operation diverges from the utility meter reading or a
reference meter (Kill-A-Watt, etc.).

### First rule out the trivial

1. **Current sensor calibration:** confirm that
   `rbamp_set_sensor_class()` and `rbamp_set_ct_model()` were called
   with the correct CT model (see
   [03 · Current sensor selection](03_sensor_selection.md)).
   Without this the RMS current is computed with a default floor and
   the power value will be systematically biased.
2. **Dropped stale snapshots:** if `rbamp_energy_wh()` is
   consistently below the reference, these may be missed intervals —
   the snapshot came in stale, the component guarded against
   double-counting, but the interval measurement was lost. Check the
   cadence (see above).
3. **Master clock drift:** `esp_timer_get_time()` is reliable on its
   own. But if the master goes into deep sleep without saving RTC,
   the interval between the previous and next latch is lost. Use the
   RTC_DATA_ATTR pattern from Scenario 9 for deep-sleep scenarios.

### Accumulator precision

The Wh accumulator inside the handle is stored as a 64-bit `double`.
Drift < 1 LSB / year at a 60 s cadence — for typical bench scenarios
this causes no problems.

If you built the component with `CONFIG_RBAMP_DISABLE_ENERGY=y`, the
energy storage and API are removed from the binary entirely. In that
case you must keep your own Wh counter in user code.

### `snap.latch_ms` (chip software timer) — diagnostic, not for billing

⚠ **`snap.latch_ms` (register 0xEC) diverges from the master
wall-clock by 25–30%** under normal ISR load. This is a **chip
software timer based on SysTick (HAL_GetTick)**; it's not a measure
of chip quality, not oscillator drift, not data loss — it's a
by-design characteristic of SysTick starvation on this MCU/ISR
profile (root E.6/F10 HW-result 2026-06-14).

The divergence **does not mean data loss**. RMS/power are sampled on
the `TIM3 TRGO` hardware timer (IRQ-independent, wall-accurate);
commits are also wall-accurate (`PERIOD_COMMIT_CNT 0xBE` increments
5.02 times/s — exactly 200 ms).

**Use the master wall-clock for energy integration**:

```c
// CORRECT — master wall-clock
int64_t t_now_us = esp_timer_get_time();
double  dt_s     = (t_now_us - t_prev_latch_us) / 1e6;
energy_wh[ch] += avg_p[ch] * dt_s / 3600.0;
t_prev_latch_us = t_now_us;

// WRONG — chip software timer undercounts by 25–30 %
double dt_chip_s = snap.latch_ms / 1000.0;
energy_wh[ch] += avg_p[ch] * dt_chip_s / 3600.0;  // understated
```

### Acceptable divergence: chip timebase vs master clock

**Canon**: up to **30 %** undercount of the chip timebase under load
is **normal** (SysTick starvation, by design). Earlier user
documentation cited 3 % — that's an **erroneous number** that would
have mass-rejected healthy chips. Use 30 % as the acceptance
threshold; below that, the chip is OK.

## ESP32 project hangs after a few minutes

**What you see:** an ESP32 project starts normally, then goes into a
hang / restart loop / WDT timeout after 1–30 minutes.

### IDF i2c_master hangs on a marginal bus — three-layer mitigation

**What you see specifically:** an ESP32 project (classic v1.0, IDF
v5.4.1, new i2c_master driver) polls rbAmp fine for several minutes,
then "dead" hangs in the middle of an `rbamp_*_read_*` call or
`rbamp_fleet_poll_all`. The task-WDT (if you have it enabled) catches
the hang; otherwise the project simply stops responding. The decoded
backtrace ends at `i2c_ll_is_bus_busy` (file
`i2c_master.c:526`, pre-send bus-busy wait).

**What's happening:** on a marginal bus (weak pull-ups, long traces,
EMI, interference from ZC edges) `SDA`/`SCL` can momentarily "stick"
in an indeterminate state. Before each transaction, IDF i2c_master
spins in the `i2c_ll_is_bus_busy` spin loop waiting for the bus to
free up — **this spin is NOT bounded** by the `xfer_timeout_ms`
parameter. If bus-busy lasts longer than the spin can run without
servicing other tasks, the FreeRTOS IDLE task doesn't get the CPU,
and the whole app hangs.

**This is not a "debugger-only artifact".** Validated on the bench:
on the same hardware harness and load —

| Bus configuration | Hang rate |
|---|---|
| 2 weak module pull-ups + debugger NRST attached | ~1 hang / 2.1 min |
| 1 weak module pull-up, no debugger | ~1 hang / 3.3 min (≈ 12 hangs per hour) |
| + external **4.7 kΩ pull-up** on DUT SDA/SCL | **0 hangs** (5-min + 25-min runs, 0 reboots) |

The correlation is clear — a bus-integrity fix removes the hang
entirely. The library logic is not at fault: the per-module mechanics
(`fleet_poll_all`, MISS/desync, leak checks) stayed clean in every run
right up to the hang.

**Three-layer mitigation (apply all three layers):**

1. **External pull-up (~4.7 kΩ) on SDA and SCL** to 3.3 V at a single
   point on the bus. ESP32-internal pull-ups (~45 kΩ) and the ones
   built into the modules (~10 kΩ) are too weak for a multi-module bus
   with real length. Cut the built-in ones on all modules but one (or
   on all of them — then you rely only on the externals). See
   [04 · Wiring](04_hardware.md) section "Multi-module bus — primary
   topology".
2. **Don't run a production build with the debugger NRST attached.**
   In a debug session the reset line can hold SDA/SCL in an
   indeterminate state on detach/attach, provoking a bus wedge on the
   next transaction. In a field build (no debugger) this factor goes
   away.
3. **App-level task-WDT on the poll task** — defense-in-depth. If for
   some reason the pull-ups remain marginal, or another wedge cause
   arises, the WDT reboots the project, and if a supervisor is present
   (e.g., an app rolling-bootloader or systemd-like supervisor) it
   comes back up. On a bench soak over an hour the WDT caught and
   recovered **all 12** hangs in the configuration without an external
   pull-up.

```c
#include "esp_task_wdt.h"

void app_main(void) {
    /* Layer 3 setup — defense-in-depth. */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 10000,           /* 10 s with plenty of margin */
        .idle_core_mask = 0,
        .trigger_panic = true,         /* on timeout — panic+reboot */
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wdt_cfg));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));   /* this task is under the WDT */

    /* ... bus / fleet init ... */

    while (1) {
        rbamp_fleet_poll_all(fleet, snaps, status, RBAMP_FLEET_MAX_MODULES, &n_ok);
        /* ... processing ... */
        esp_task_wdt_reset();            /* "I'm alive" — every cycle */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

**What NOT to do**: don't try to work around the problem with a
larger `xfer_timeout_ms` — it doesn't bound the bus-busy pre-spin,
that's a different code path in the driver. The fix is bus integrity
(layer 1) + supervisor (layer 3).

### Watchdog timeout while connecting to WiFi

**Cause:** an unbounded WiFi-connect wait loop inside an event
handler triggers the task-WDT after ~5 s (default).

**Fix:** use an event-driven pattern instead of a busy-wait. The
ESP-IDF WiFi driver emits `WIFI_EVENT_STA_DISCONNECTED` and
`IP_EVENT_STA_GOT_IP` events — react to them via
`esp_event_handler_register()` instead of polling in a loop.

Minimal pattern:

```c
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();   /* auto-reconnect */
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* In app_main, wait for the event with a timeout: */
EventBits_t bits = xEventGroupWaitBits(
    wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
if (!(bits & WIFI_CONNECTED_BIT)) {
    esp_restart();
}
```

### MQTT broker disconnects every ~15 s

**Cause:** in most cases this is **not keepalive**. The default
`session.keepalive` in `esp_mqtt_client_config_t` is 120 s, and
`disable_keepalive` defaults to `false` (i.e., keepalive is on).
Explicitly rewriting these fields with the same values changes
nothing — if the connection dies every 15 s, the real cause is in
the network (an intermediate NAT/firewall dropping idle TCP) or a
WiFi drop.

**Fix:** register `MQTT_EVENT_DISCONNECTED` and reconnect explicitly.
`esp_mqtt_client_start()` launches its own task internally; you don't
need to tick a separate `mqtt.loop()` — just reconnect on the event.

```c
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW("mqtt", "disconnected — reconnecting on timeout");
        /* esp-mqtt reconnects automatically with backoff;
         * this handler is the place to clean up application state
         * (retained flags, discovery payload caches). */
    }
}

const esp_mqtt_client_config_t cfg = {
    .broker.address.uri = "mqtt://192.168.1.10:1883",
    /* leave keepalive at the default — 120 s;
     * for aggressive NATs, lower it to 30 s: */
    /* .session.keepalive = 30, */
};
esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                mqtt_event_handler, NULL);
esp_mqtt_client_start(client);
```

If the disconnects coincide in time with WiFi drops, the real cause
is in WiFi (RSSI, a congested channel), not in MQTT. Check the
`MQTT_EVENT_ERROR` payload and the WiFi logs before touching
keepalive.

### TLS handshake fail (cloud integrations)

**Cause:** the ESP32 heap is insufficient for the TLS handshake
(~30 kB required). Often combined with WiFi + MQTT + buffers, leaving
< 20 kB.

**Fix:**

- Use `WiFi.mode(WIFI_MODE_STA)` (not `STA+AP`).
- Disable BLE if you don't need it: `CONFIG_BT_ENABLED=n` in
  sdkconfig.
- Reduce the MQTT buffer sizes in `esp_mqtt_client_config_t`.
- Move to ESP32-S2 (no Bluetooth — an extra ~30 kB of heap).

If `esp_get_free_heap_size()` shows < 25 kB before
`esp_mqtt_client_start()` or `esp_http_client_perform()`, the TLS
handshake will most likely fail.

## Error handling: `REG_ERROR` last-write + `EVENT_FLAGS.bit3` durable (v1.3)

**v1.3 canon (HW-confirmed)**: `REG_ERROR (0x02)` is
**last-write-outcome**, **NOT sticky**. On each register write the
firmware sets `reg_error = ERR_OK`; a subsequent unrelated write
**clears** the previous error.

**Durable error signal** = `EVENT_FLAGS (0x2A) bit3` — sticky (W1C),
asserted immediately on any rejected write/command.

**Pattern 1 — one-off post-operation capture** (if the outcome of
THIS specific operation matters):

```c
esp_err_t err = rbamp_save_user_config(dev);
uint8_t dev_err;
rbamp_read_u8(dev, 0x02, &dev_err);   /* IMMEDIATELY, before the next write */
```

**Pattern 2 — durable error monitoring** (canonical for long-running):

```c
uint8_t event_flags;
rbamp_read_u8(dev, 0x2A, &event_flags);
if (event_flags & (1 << 3)) {
    uint8_t last_err;
    rbamp_read_u8(dev, 0x02, &last_err);   /* what was the last rejected */
    /* handle the error */
    rbamp_write_u8(dev, 0x2A, (1 << 3));   /* W1C clear bit3 */
    /* or CMD_CLEAR_ERROR */
}
```

> ⚠ **REVERSAL** of the previous "sticky" documentation (v1.3 A3
> root canon). REG_ERROR is last-write-outcome, not sticky.
> Durability is provided by EVENT bit3.

> ⚠ **bit3 is an async channel, not for synchronous write
> validation**: on a rejected write, bit3 is set **with a delay**
> relative to the moment the I²C transaction returns. Don't poll
> bit3 right after a write to check that write's outcome — for the
> outcome of **your own write** use **Pattern 1** (a one-off
> `REG_ERROR` capture immediately after the operation) or the
> setter's return code. `bit3` is for long-running monitoring of
> async facts (runtime, command-path), not for validating an
> operation you just performed.

**Caveat:** `DEV_ERR_CLONE (0xF9)` is NOT cleared via
`CMD_CLEAR_ERROR` — an anti-clone sentinel (only reboot or factory
reset).

## Fresh-flash module shows `0xFB` on first boot (NORMAL)

**What you see:** a freshly flashed module on first boot shows
`REG_ERROR = 0xFB` (`DEV_ERR_FLASH_PARAMS_BAD`) + EVENT bit3.

**Cause:** params page uninitialized → factory defaults loaded. This
is **NORMAL** for a virgin module, not fatal.

**Solution:** configure sensor_class + ct_model + (optionally)
address, then `CMD_SAVE_USER_CONFIG`. After a successful SAVE →
`REG_ERROR = 0x00`, bit3 clear.

## `rbamp_set_sensor_class` / `rbamp_set_ct_model*` returns `ESP_ERR_INVALID_STATE`

**What you see:** one of the configuration calls returns
`ESP_ERR_INVALID_STATE`.

`ESP_ERR_INVALID_STATE` from this group of functions is an **umbrella
code** for three distinct causes. You can tell them apart by the
`ESP_LOGW` line the component prints under the `"rbamp"` tag right
before returning:

| Log line (ESP_LOGW, TAG="rbamp") | Cause | Solution |
|---|---|---|
| `set_ct_model_ch refused: sensor class is UNSET (call rbamp_set_sensor_class first)` | Precondition: the sensor class must be set before writing the CT model | Call `rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013)` before `rbamp_set_ct_model*()`. |
| `set_ct_model_ch refused: channel %u after %u (descending order required; call rbamp_set_sensor_class first to reset batch)` | Per-channel calls were made in **ascending** order (e.g., `ch0` → `ch1` → `ch2`) — violating the legacy side-effect on channel 0 | Reorder them descending: `ch2` → `ch1` → `ch0`. Alternatively, call `rbamp_set_sensor_class()` again, which resets the batch tracker and lets you start from any channel. |

There's also the case of a **handle that's initialized but
`rbamp_begin()` hasn't been called** — this returns
`ESP_ERR_INVALID_STATE` without an ESP_LOGW line (the handle is still
"cold"). Solution: call `rbamp_begin(dev)` after `rbamp_new()` before
any `rbamp_set_*`.

> **Disambiguation-via-log pattern** is a standard ESP-IDF idiom:
> compare it with `esp_driver_i2c`, which returns `ESP_ERR_TIMEOUT`
> for both an ACK timeout and a bus-busy timeout, distinguishing them
> by the log. If you see `ESP_ERR_INVALID_STATE` with no clear cause,
> raise the log level via
> `esp_log_level_set("rbamp", ESP_LOG_WARN)` (or `_DEBUG`) and repeat
> the call: the component will tell you which of the three cases you
> have. `rbamp_err_to_str(ESP_ERR_INVALID_STATE)` returns a generic
> description: *"Wrong call sequence (check log: develop mode /
> sensor class UNSET / CT model ascending order)"*.

## `rbamp_set_sensor_class` / `rbamp_set_ct_model*` returns `ESP_ERR_INVALID_ARG`

**What you see:** the call returns `ESP_ERR_INVALID_ARG`.

Possible causes:

1. **Invalid model code** — the valid range is 1..5 (see the table
   in [03 · Current sensor selection](03_sensor_selection.md)).
   Values 0 and 6+ return `ESP_ERR_INVALID_ARG`.
2. **Invalid channel index** in the per-channel form
   `rbamp_set_ct_model_ch(dev, channel, code)` — must be <
   `rbamp_channels(dev)`.
3. **A reserved `rbamp_sensor_class_t` value** —
   `RBAMP_SENSOR_WIRED_CT` or `RBAMP_SENSOR_BUILTIN_CT` are not yet
   supported (not on the current SKU); use `RBAMP_SENSOR_SCT013`.
4. **`dev == NULL` or `out == NULL`** — confirm that `rbamp_new()`
   succeeded (returned `ESP_OK`).

## `rbamp_commit_address_change` returns `ESP_ERR_TIMEOUT`

If you have a module with develop mode enabled and `prepare` passed
but `commit` returns `ESP_ERR_TIMEOUT`, the "arming" window has
expired (5 seconds after `prepare`). `rbamp_probe()` **won't help**
here (the module responds; the problem is in the state machine). The
fix: call `rbamp_prepare_address_change()` again and immediately —
**in the same `app_main` iteration**, with no network calls between
them — call `rbamp_commit_address_change()`. WiFi / MQTT / any
blocking I/O between `prepare` and `commit` is the main source of
the window expiring.

For more on the public-with-warning methods, see
[09 · API reference](09_api_reference.md), section "Sensor
configuration".

## Reads fail when switching relay loads (EMI transients)

**What you see:** `rbamp_read_*` periodically return `ESP_FAIL` /
`ESP_ERR_INVALID_RESPONSE`, but **only while a relay/inductive load
is actively switching** on the same setup. Steady-state reads are
reliable.

**Cause (C.12 HW-confirmed):** during relay switching (a 5A load) EMI
fails ~67% of I²C transactions (~8/12 in the hw measurement). The bus
self-recovers, **no wedge occurs**, no module is lost — but the
current read fails.

**Solution — application-level retry**:

```c
esp_err_t robust_read_voltage(rbamp_handle_t dev, float *v) {
    for (int attempt = 0; attempt < 5; attempt++) {
        esp_err_t err = rbamp_read_voltage(dev, 0, v);
        if (err == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(20));   // short pause before retry
    }
    return ESP_FAIL;
}
```

> ⚠ **A master-side `bus_reset` alone is not enough**: when an EMI
> glitch overlaps the driver's retry window, you need an explicit
> application-level retry with a pause.

Alternative: if the master sees relay-switching events — delay the
reads by ~50-100 ms after commutation.

## Task-WDT reboot under EMI / heavy load

**What you see:** the ESP32 unexpectedly reboots with reason
`TASK_WDT`. It usually happens during EMI events (relay switching,
inductive transients).

**Cause (standfw [SKILL-NEW][BENCH-RESULT]):** under EMI-slowed reads
(NACK-retry + bus_reset cycles) the ESP-IDF `i2c_master` driver can
stretch a transaction to ~100 ms+. If a task runs a tight bulk-read
loop without yields — task-WDT timeout.

**Solution — yield in the bulk-read loop**:

```c
// WRONG — no yield, under EMI → WDT reboot
for (int i = 0; i < N_METERS; i++) {
    rbamp_read_period_snapshot(meters[i], &snaps[i], 0, true);
}

// CORRECT — yield every N reads (or every iteration)
for (int i = 0; i < N_METERS; i++) {
    rbamp_read_period_snapshot(meters[i], &snaps[i], 0, true);
    vTaskDelay(1);   // yield + WDT feed
}
```

Alternative: `esp_task_wdt_reset()` explicitly if the task is
WDT-tracked.

> Source: standfw `DimmerI2C-Test/I2C_MASTER_LESSONS.md` (bus-announced [SKILL-NEW]).

## Error-code summary table

The `rbamp_*` functions return standard `esp_err_t` values.
`rbamp_err_to_str(err)` returns a human-readable string.

| Code | When | Where to look |
|---|---|---|
| `ESP_OK` (0) | success | — |
| `ESP_FAIL` (-1) | NACK after retry; or the sanity filter rejected the value | "Module doesn't respond over I²C" section |
| `ESP_ERR_INVALID_ARG` (0x102) | `dev == NULL`, `channel`/`phase` out of range, `code` outside 1..5, reserved `cls` | check the call arguments |
| `ESP_ERR_TIMEOUT` (0x107) | `rbamp_wait_ready` expired (module not responding) OR the 5 s window between `prepare_address_change` and `commit_address_change` expired | for wait_ready — via `rbamp_probe()`; for commit — re-arm: `prepare` + `commit` back-to-back with no blocking I/O between them |
| `ESP_ERR_INVALID_STATE` (0x103) | precondition: `set_ct_model*` before `set_sensor_class` | the `set_*` section above |
| `ESP_ERR_NOT_SUPPORTED` (0x106) | the function is unavailable on the current SKU (e.g., the voltage API on an I* variant) | check `rbamp_hw_variant(dev)` |
| `ESP_ERR_INVALID_RESPONSE` (0x108) | period snapshot stale; or `REG_VERSION` = 0/0xFF on `rbamp_begin` | "Period snapshots are always stale" / "Module doesn't respond" section |
| `ESP_ERR_NO_MEM` (0x101) | failed to allocate memory for the handle | check the heap budget (`esp_get_free_heap_size()`) |

## Bus-level debug with a logic analyzer

For deep debugging, when the component can no longer tell you what's
happening on the wire, capture SDA + SCL with a logic analyzer
(Saleae, DSLogic Plus, Sigrok-compatible):

- Sample rate ≥ 1 MS/s at 100 kHz I²C; ≥ 4 MS/s at 400 kHz.
- The I²C decoder in Sigrok / Saleae will show ACK / NACK for each
  byte + the address phase.
- Compare your code's calls (`rbamp_read_voltage()`, etc.) with the
  expected byte sequence — they should match exactly. Burst-reads
  (several bytes in one transaction via address auto-increment) are
  supported.

If the component's behavior diverges from the capture, open an issue
with the capture file attached (`.sal` / `.dsl` / `.csv`).

## When to contact support

If you've gone through the relevant section above and the problem
persists, open an issue:

[github.com/rb-amp/rbamp-esp-idf/issues](https://github.com/rb-amp/rbamp-esp-idf/issues)

In the issue, include:

- **Target SoC** (ESP32 / S2 / S3 / C2 / C3 / C6 / H2 / P4) +
  **ESP-IDF version** (output of `idf.py --version`).
- **rbamp component version** (for example, 1.1.0).
- **Module firmware version** — `rbamp_firmware_version(dev)` in the
  logs.
- **A minimal IDF project** reproducing the problem (~30 LOC in
  `main/main.c`).
- **The `esp_err_t` values + `rbamp_err_to_str(err)`** at the time of
  the failure, from the logs under the `rbamp` tag.
- **Relevant Kconfig**: `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ`,
  `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS`, `CONFIG_RBAMP_LOG_LEVEL_*`.
- **(If available)** a logic-analyzer capture file from the moment of
  the failure.

## Links

- [05 · Quickstart](05_quickstart.md) — your first working project
- [09 · API reference](09_api_reference.md) — full API +
  warnings on the public-with-WARNING methods + Kconfig section
- [03 · Current sensor selection](03_sensor_selection.md) — table of
  SCT-013 models, dual-CT topology for a wide dynamic range,
  approaches to improving sensitivity at low currents

