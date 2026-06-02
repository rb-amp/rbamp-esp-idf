# 10 · Troubleshooting

This chapter is organized by **the symptoms you observe**, not by an
internal error taxonomy. If you're here, something isn't working as
expected. Find your symptom in the list below, jump to the matching
section, and walk through the diagnostic procedure.

Most bench problems fall into three categories:

1. **Bus-level** — the module doesn't respond over I²C; reads return
   `ESP_FAIL` or `ESP_ERR_INVALID_RESPONSE`.
2. **Data-level** — the module responds, but the numbers are wrong:
   zeros, odd values, the wrong sign.
3. **Application-level** — the link is unstable, the Wh counter
   drifts, or the project hangs.

Shortcut for the impatient: in steady state, the public `rbamp_*`
functions should return `ESP_OK`. If your logs frequently show
`ESP_FAIL` or `ESP_ERR_INVALID_RESPONSE`, jump straight to the
"Module doesn't respond over I²C" section below.

## Module doesn't respond over I²C

**What you see:** `rbamp_begin(dev)` returns `ESP_FAIL` or
`ESP_ERR_INVALID_RESPONSE`, or RT reads regularly propagate
`ESP_FAIL` up to user code.

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

- SDA / SCL aren't swapped (see [04 · Hardware connection](04_hardware.md)
  for per-chip GPIO defaults).
- Both lines have a pull-up to 3.3 V (the module board has built-in
  4.7 kΩ pull-ups — no external ones needed for a single module).
- The module's power pin actually carries 5 V (4.5–5.5 V).
- No other master (debug probe, a second ESP32) is hanging on the
  same lines.
- `flags.enable_internal_pullup = true` in your
  `i2c_master_bus_config_t` (if there are no external pull-ups).

**If the module is found, but at an unexpected address** — someone
re-addressed it earlier on the bench. Update the second argument of
`rbamp_new`:

```c
rbamp_new(bus, 0x52, &dev);   /* address from the bus scan */
```

### Step 2 — ESP32 baseline NACK pattern

If the bus scan finds the module, but `rbamp_read_voltage(dev, 0, &v)`
periodically returns `ESP_FAIL`, this is a **documented baseline
pattern**: the ESP-IDF v5 `i2c_master` driver has quirks when working
with rbAmp on the current firmware (~20 % NACK rate at 100 kHz).

The mitigation is already built into the component
(`CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=3` by default, 50 kHz via
`CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000`), but under heavy bus load
the retry budget can run out.

**What to do via `idf.py menuconfig` → Component config → rbAmp client:**

1. **Confirm the 50 kHz default**:

   ```text
   CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000
   ```

2. **Raise the number of retry attempts for heavy load**:

   ```text
   CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=5
   ```

3. **Increase the timeout if needed**:

   ```text
   CONFIG_RBAMP_I2C_TIMEOUT_MS=200
   ```

4. **Enable DEBUG level for the `rbamp` tag** for diagnostics:

   ```text
   CONFIG_RBAMP_LOG_LEVEL_DEBUG=y
   ```

   You can also do this at runtime — `esp_log_level_set("rbamp", ESP_LOG_DEBUG)`.

Once firmware v1.1+ ships with the slave-side NACK fix, 100 kHz
becomes a working speed:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=100000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=1
```

> **About the retry/sanity counters.** As of v1.2.0, the component
> exposes public getters `rbamp_retry_exhaustion_count(dev)` and
> `rbamp_sanity_reject_count(dev)` — cross-platform equivalents of
> Arduino's `dev.retryExhaustionCount()` / `dev.sanityRejectCount()`.
> The canonical snapshot + diff pattern for production monitoring
> is described in the "Monitoring the counters" section below.

## Current reads zero on a live load

**What you see:** `rbamp_read_current(dev, 0, &i)` returns `ESP_OK`
but `i = 0.000` (or a very small value), even though a real consumer
(kettle, lamp, iron) is switched on. `rbamp_read_power_factor(dev, 0, &pf)`
may show an odd value (`nan` / `0` / `±1`) — that's a consequence;
the root cause is the current.

### Diagnostic procedure

1. **Verify that the sensor class and CT model are configured:**

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

   On v1.2+ firmware, without these two calls the calibration
   coefficients aren't loaded, and the current always reads as zero.
   This step is done **once** at first installation — the choice is
   saved in the module's flash.

   > **Gotcha with `RBAMP_SENSOR_UNSET`.** If you accidentally call
   > `rbamp_set_sensor_class(dev, RBAMP_SENSOR_UNSET)`, the function
   > returns `ESP_OK` but effectively clears the class configuration.
   > A subsequent `rbamp_set_ct_model*()` on v1.2+ will start
   > returning `ESP_ERR_INVALID_STATE`. The fix is to call
   > `rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013)` again, then
   > `rbamp_set_ct_model()` once more.

2. **Verify that the CT model matches the load.** A large CT clamp
   (e.g. SCT-013-100, 100 A) on a small load (a 50 W lamp = ~0.2 A)
   produces a signal at the edge of the noise floor, and the readings
   will be zero. Choose the **smallest** CT model that covers your
   maximum expected current. The full table is in
   [03 · Current sensor selection](03_sensor_selection.md).

   If you have a multi-channel module (UI2 / UI3) and want to see
   both small loads and peak surges, consider the dual-CT pattern:
   a small clamp (e.g. SCT-013-005) on a dedicated channel for the
   low range + a large one (SCT-013-030/100) on another channel for
   the high range; the master picks the value by threshold. The
   pattern is discussed in
   [03 · Current sensor selection](03_sensor_selection.md), the
   "Dual-CT topology" section.

3. **Check the clamp orientation.** The arrow on the clamp body must
   point **in the direction of current flow toward the load**. If the
   clamp is "backwards", `rbamp_read_current()` gives the correct
   value by magnitude, but `rbamp_read_power()` gives a negative
   value on a consumer load. Confirmation:
   `rbamp_read_power_factor()` shows exactly −1.0 on a resistive load
   (instead of +1.0). Fix: either physically reinstall the clamp
   (unclip it, turn it so the arrow points the right way, clip it
   back), or invert the sign on the master side.

### If all three steps pass and the current is still zero

That means the ADC signal really is below the noise floor. Check:

- **Whether current is actually flowing** — measure with a multimeter
  (DC clamp / AC clamp meter) on the same wire.
- **Whether the clamp is intact** — the clamp connector should carry
  an AC voltage proportional to the current (a few millivolts for
  consumer loads).
- **Whether you're clamping the right wire** — the line (L), not
  neutral (although a clamp on neutral works too — it measures
  current magnitude, not direction).

## Readings jump around or return `ESP_FAIL`

**What you see:** `rbamp_read_voltage(dev, ...)` or
`rbamp_read_current(dev, ...)` periodically return `ESP_FAIL`. The
value in `*out` is undefined after an error.

`ESP_FAIL` from the component covers two classes of problem:

- **NACK after retry exhaustion** — the link is unstable.
- **The sanity filter rejected the value** — the bus delivered a float
  `NaN` / `Inf` / `|x| > 10000` (an obviously non-physical value).

The ESP-IDF API doesn't distinguish these via separate `esp_err_t`
codes (unlike Arduino's `RB_ERR_NACK` / `RB_ERR_NON_PHYSICAL`) — both
return `ESP_FAIL`. You can tell them apart via the **diagnostic
counters** (see the "Monitoring the counters" section below). Both
symptoms are treated the same way:

1. **Confirm the 50 kHz default** + raise retries to 5 via Kconfig —
   see the previous section.
2. **Reduce the bus load density** — if your loop does many
   single-byte reads, try `rbamp_read_all()` (one I²C transaction
   instead of ~13).
3. **Check `CONFIG_RBAMP_I2C_TIMEOUT_MS`** — if the device responds
   more slowly than usual (long cable, poor contact), increase the
   timeout.
4. **Enable DEBUG logging** via `CONFIG_RBAMP_LOG_LEVEL_DEBUG=y` or
   `esp_log_level_set("rbamp", ESP_LOG_DEBUG)` — the component will
   print retry-loop details and sanity rejects under the `"rbamp"`
   tag.

## Monitoring the counters

The component exposes two public monotonic counters for production
bus-health monitoring:

```c
uint32_t rbamp_retry_exhaustion_count(rbamp_handle_t dev);
uint32_t rbamp_sanity_reject_count(rbamp_handle_t dev);
```

`retry_exhaustion_count` increments when a single-byte transaction
exhausts the `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS` budget and returns
`ESP_FAIL`. `sanity_reject_count` increments when the sanity filter
discards a float (`!isfinite(x) || |x| > 10000`).

The canonical pattern is snapshot + diff once every 60 seconds, to
get an "events-per-minute" rate:

```c
uint32_t prev_retry  = rbamp_retry_exhaustion_count(dev);
uint32_t prev_sanity = rbamp_sanity_reject_count(dev);
/* ...60 s of normal operation (make N rbamp_read_* calls)... */
uint32_t now_retry  = rbamp_retry_exhaustion_count(dev);
uint32_t now_sanity = rbamp_sanity_reject_count(dev);
uint32_t retry_delta  = now_retry  - prev_retry;
uint32_t sanity_delta = now_sanity - prev_sanity;
if (retry_delta > THRESHOLD || sanity_delta > THRESHOLD) {
    ESP_LOGW(TAG, "bus health: retry+%u sanity+%u per minute",
             retry_delta, sanity_delta);
}
```

**Alert thresholds** depend on the workload. For a dense soak pattern
(like the Arduino SoakMonitor — ~70 bytes per cycle once every
5 minutes):

- `retry_delta > 1/hour` — benign noise, safe to ignore.
- `retry_delta` consistently **> 5/hour** — worth investigating (bad
  contacts, bus capacitance, temperature drift of the pull-ups).
- `sanity_delta > 0` in steady state — almost always a sign of
  electromagnetic interference on the bus or a firmware regression.

The counters **only increase** (they never reset) — to reset them you
must recreate the handle via `rbamp_del(dev)` + `rbamp_new(...)`. This
is by design, so you don't lose history to races between "reset" and
"read into the logger".

> Cross-platform, these counters match the semantics of Arduino's
> `dev.retryExhaustionCount()` / `dev.sanityRejectCount()` — the same
> names (snake_case vs. camelCase), the same thresholds apply.

## Power factor looks odd

**What you see:** `rbamp_read_power_factor(dev, 0, &pf)` returns
`ESP_OK` but `pf` doesn't match the load type.

Expectations by load:

| Load | Expected PF |
|---|---|
| Kettle, iron, incandescent lamp | +0.95 .. +1.0 (resistive) |
| Refrigerator, compressor motor | +0.6 .. +0.85 (inductive) |
| LED lamp, TV (switching PSU) | +0.5 .. +0.95 (non-linear) |
| PV inverter exporting power | negative PF |

### PF = nan or 0 when I = 0

PF is defined as `P / (U × I)`. With zero current `I=0`, the math is
undefined. The exact returned value depends on the firmware (it may
be `0` or a placeholder near zero) — this is normal as long as the
current really is zero. Once there's current, there's a valid PF.
**The component's sanity filter does not reject this value** (so the
IDF equivalent of `sanityRejectCount` does not increment) — it's a
valid measure of an undefined quantity, not a link artifact.

### PF is exactly −1.0 on a purely consumer load

The clamp is installed "backwards" — the arrow doesn't point in the
direction of current flow toward the load. Fix: either reinstall the
clamp correctly, or handle the sign on the master side
(`p = -p; pf = -pf;` if you know the load can't be a PV inverter).

### PF floats between +0.3 and +0.7 on a resistive load

- **Possible cause:** the voltage reference is taken from a different
  phase. This matters for split-phase (240 V in the US) and 3-phase
  grids, where the module reads U from one phase while the CT clamp
  hangs on another — a 120° or 180° phase shift between U and I yields
  exactly these PF values. Fix: install the module so the U input and
  the CT hang on the same phase.
- **Alternative:** the load really isn't purely resistive. Repeat the
  test with a known resistive load (an electric kettle at full power).

## Period snapshots are always `stale`

**What you see:** `rbamp_read_period_snapshot(dev, ...)` returns
`ESP_ERR_INVALID_RESPONSE`, or `*out.valid == false`. This means the
module didn't finish integrating the previous period by the time of
the next read.

The component guards against double-counting Wh: on a stale snapshot
it records the master timestamp (`esp_timer_get_time()`), and the
next successful snapshot covers one period, not two.

**Acceptable:** rare stale snapshots (1–2 per hour at a 60 s cadence).
**Not acceptable:** consecutive stale snapshots — that means the
firmware is unresponsive or the master polls too often.

### Cadence check procedure

1. **Check the cadence:** 60 s between latches is comfortable; 30 s is
   marginal; < 10 s guarantees stale snapshots.
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
       /* safe to read avg_p[] */
   }
   ```

4. **Check the firmware version** — `rbamp_firmware_version(dev) >= 0x02`
   shows fewer stale snapshots than 0x01.

### Special case — deep-sleep wake

If you use a deep-sleep pattern, the **default**
`rbamp_read_period_snapshot()` after `rbamp_begin()` will always be
stale (or give near-zero values) — `begin()` does a primer LATCH that
resets the firmware accumulator. The canonical pattern uses
`skip_latch=true` + your own RTC-saved master timestamp — see
[06 · Examples](06_examples.md), Scenario 9.

## Wh accounting drifts from the reference

**What you see:** `rbamp_energy_wh(dev, 0)`, after several hours /
days of operation, diverges from the utility meter or a reference
meter (Kill-A-Watt or similar).

### First rule out the trivial

1. **Current-sensor calibration:** make sure `rbamp_set_sensor_class()`
   and `rbamp_set_ct_model()` were called with the correct CT model
   (see [03 · Current sensor selection](03_sensor_selection.md)).
   Without this, RMS current is computed with the default floor and
   the power value is systematically skewed.
2. **Dropped stale snapshots:** if `rbamp_energy_wh()` is consistently
   below the reference, it may be missed intervals — the snapshot came
   back stale, the component guarded against double-counting, but the
   interval measurement was lost. Check the cadence (see above).
3. **Master clock drift:** `esp_timer_get_time()` is reliable by
   itself. But if the master goes into deep sleep without saving the
   RTC, the interval between the previous and next latch is lost. Use
   the RTC_DATA_ATTR pattern from Scenario 9 for deep-sleep scenarios.

### Accumulator precision

The Wh accumulator inside the handle is stored as a 64-bit `double`.
Drift < 1 LSB / year at a 60 s cadence — no problem for typical bench
scenarios.

If you built the component with `CONFIG_RBAMP_DISABLE_ENERGY=y`, the
energy storage and API are fully removed from the binary. In that
case you must keep your own Wh counter in user code.

## ESP32 project hangs after a few minutes

**What you see:** the ESP32 project starts up fine, then goes into a
hang / restart loop / WDT timeout after 1–30 minutes.

### Watchdog timeout while connecting to WiFi

**Cause:** an unbounded wait loop for the WiFi connection inside the
event handler triggers the task WDT after ~5 s (default).

**Fix:** use an event-driven pattern instead of a busy-wait. The
ESP-IDF WiFi driver emits `WIFI_EVENT_STA_DISCONNECTED` and
`IP_EVENT_STA_GOT_IP` events — react to them via
`esp_event_handler_register()` instead of polling in the loop.

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
`disable_keepalive` defaults to `false` (i.e. keepalive is enabled).
Explicitly rewriting these fields with the same values changes
nothing — if the connection dies every 15 s, the real cause is the
network (an intermediate NAT/firewall dropping idle TCP) or a WiFi
drop.

**Fix:** register `MQTT_EVENT_DISCONNECTED` and reconnect explicitly.
`esp_mqtt_client_start()` runs its own task internally; you don't need
a separate `mqtt.loop()` tick — only reconnection on the event.

```c
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW("mqtt", "disconnected — reconnecting on timeout");
        /* esp-mqtt reconnects automatically with backoff;
         * this handler is the place to clean up application state
         * (retained flags, cached discovery payloads). */
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
is WiFi (RSSI, a congested channel), not MQTT. Check the
`MQTT_EVENT_ERROR` payload and the WiFi logs before touching
keepalive.

### TLS handshake fail (cloud integrations)

**Cause:** insufficient ESP32 heap for the TLS handshake (~30 kB
required). Often combined with WiFi + MQTT + buffers, leaving < 20 kB.

**Fix:**

- Use `WiFi.mode(WIFI_MODE_STA)` (not `STA+AP`).
- Disable BLE if you don't need it: `CONFIG_BT_ENABLED=n` in sdkconfig.
- Reduce the MQTT buffer sizes in `esp_mqtt_client_config_t`.
- Switch to the ESP32-S2 (no Bluetooth — an extra ~30 kB of heap).

If `esp_get_free_heap_size()` shows < 25 kB before
`esp_mqtt_client_start()` or `esp_http_client_perform()`, the TLS
handshake will most likely fail.

## `rbamp_set_sensor_class` / `rbamp_set_ct_model*` returns `ESP_ERR_INVALID_STATE`

**What you see:** one of the configuration calls returns
`ESP_ERR_INVALID_STATE`.

`ESP_ERR_INVALID_STATE` from this group of functions is an **umbrella
code** for three distinct causes. You can tell them apart by the
`ESP_LOGW` line the component prints under the `"rbamp"` tag right
before returning:

| Log line (ESP_LOGW, TAG="rbamp") | Cause | Fix |
|---|---|---|
| `set_ct_model_ch refused: firmware 0x%02X < v1.2` | The per-channel API was called on firmware older than v1.2 (the `CMD_SET_CT_MODEL_CHn` opcodes don't exist) | Use the single-argument `rbamp_set_ct_model(dev, code)` (it works on v1.0+ via the legacy path), or update the module firmware to v1.2+. |
| `set_ct_model_ch refused: sensor class is UNSET (call rbamp_set_sensor_class first)` | Firmware v1.2+ requires a precondition: the sensor class must be set before writing the CT model | Call `rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013)` before `rbamp_set_ct_model*()`. |
| `set_ct_model_ch refused: channel %u after %u (descending order required; call rbamp_set_sensor_class first to reset batch)` | The per-channel calls were made in **ascending** order (e.g. `ch0` → `ch1` → `ch2`) — this breaks the legacy side effect on channel 0 | Reorder them in descending order: `ch2` → `ch1` → `ch0`. Alternatively, call `rbamp_set_sensor_class()` again, which resets the batch tracker and lets you start from any channel. |

There's also the case where the **handle is initialized but
`rbamp_begin()` hasn't been called** — this returns
`ESP_ERR_INVALID_STATE` with no ESP_LOGW line (the handle is still
"cold"). Fix: call `rbamp_begin(dev)` after `rbamp_new()` before any
`rbamp_set_*`.

> **The log-based disambiguation pattern** is a standard ESP-IDF
> idiom: compare with `esp_driver_i2c`, which returns
> `ESP_ERR_TIMEOUT` for both an ACK timeout and a bus-busy timeout,
> distinguishing them via the log. If you see `ESP_ERR_INVALID_STATE`
> with no obvious cause, raise the log level via
> `esp_log_level_set("rbamp", ESP_LOG_WARN)` (or `_DEBUG`) and repeat
> the call: the component will tell you which of the three cases
> applies. `rbamp_err_to_str(ESP_ERR_INVALID_STATE)` returns a
> generic description: *"Wrong call sequence (check log: develop
> mode / sensor class UNSET / CT model ascending order)"*.

## `rbamp_set_sensor_class` / `rbamp_set_ct_model*` returns `ESP_ERR_INVALID_ARG`

**What you see:** the call returns `ESP_ERR_INVALID_ARG`.

Possible causes:

1. **Invalid model code** — the valid range is 1..5 (see the table in
   [03 · Current sensor selection](03_sensor_selection.md)). Values 0
   and 6+ return `ESP_ERR_INVALID_ARG`.
2. **Invalid channel index** in the per-channel form
   `rbamp_set_ct_model_ch(dev, channel, code)` — it must be <
   `rbamp_channels(dev)`.
3. **A reserved `rbamp_sensor_class_t` value** —
   `RBAMP_SENSOR_WIRED_CT` or `RBAMP_SENSOR_BUILTIN_CT` aren't
   supported yet (not on the current SKU); use `RBAMP_SENSOR_SCT013`.
4. **`dev == NULL` or `out == NULL`** — verify that `rbamp_new()`
   succeeded (returned `ESP_OK`).

## `rbamp_set_ct_model_ch` returns `ESP_ERR_NOT_SUPPORTED`

**What you see:** the per-channel form returns
`ESP_ERR_NOT_SUPPORTED`.

**Cause:** the module firmware is too old for the per-channel commands
(the `CMD_SET_CT_MODEL_CH0/1/2` opcodes appeared in v1.2). On
v1.0 / v1.1 this command doesn't exist.

**What to do:**

- **Check the version:** `rbamp_firmware_version(dev)` should return
  `0x03` or higher for the per-channel form.
- **Use the legacy single-arg form** `rbamp_set_ct_model(dev, code)`
  — it works on all firmware versions, but configures only channel 0.
- **Update the module firmware** to v1.2+ if you need full per-channel
  configuration.

## `rbamp_prepare_address_change` / `rbamp_commit_address_change` returns `ESP_ERR_INVALID_STATE`

**What you see:** the I²C address-change methods return
`ESP_ERR_INVALID_STATE`.

> ⚠ **Develop-mode-only operation.** Changing the address requires the
> module to be in develop mode (an internal flag, set at the factory).
> On a standard production module this flag is **not set**, and these
> methods return `ESP_ERR_INVALID_STATE` — this is expected behavior,
> not a bug. The `rbamp_prepare_address_change()` +
> `rbamp_commit_address_change()` pair is intended for factory
> provisioning and integrator bench operations, not for user code. If
> a deployed module needs a different I²C address, the documented path
> is reconfiguration through the factory rig (outside the component's
> scope).

### `rbamp_commit_address_change` returns `ESP_ERR_TIMEOUT`

If you have a module with develop mode enabled and `prepare`
succeeded, but `commit` returns `ESP_ERR_TIMEOUT`, the "arming"
window has expired (5 seconds after `prepare`). `rbamp_probe()`
**won't help** here (the module responds; the problem is in the state
machine). Fix: call `rbamp_prepare_address_change()` again and
immediately, **in the same `app_main` iteration**, with no network
calls in between, call `rbamp_commit_address_change()`. WiFi / MQTT /
any blocking I/O between `prepare` and `commit` is the main source of
window expiry.

For more on the public-with-warning methods, see
[09 · API reference](09_api_reference.md), the "Sensor
configuration" section.

## Error-code summary table

The `rbamp_*` functions return standard `esp_err_t` values.
`rbamp_err_to_str(err)` returns a human-readable string.

| Code | When | Where to look |
|---|---|---|
| `ESP_OK` (0) | success | — |
| `ESP_FAIL` (-1) | NACK after retry; or the sanity filter rejected the value | the "Module doesn't respond over I²C" section |
| `ESP_ERR_INVALID_ARG` (0x102) | `dev == NULL`, `channel`/`phase` out of range, `code` out of 1..5, a reserved `cls` | check the call arguments |
| `ESP_ERR_TIMEOUT` (0x107) | `rbamp_wait_ready` expired (module not responding) OR the 5 s window between `prepare_address_change` and `commit_address_change` expired | for wait_ready — via `rbamp_probe()`; for commit — re-arm: `prepare` + `commit` back-to-back with no blocking I/O between them |
| `ESP_ERR_INVALID_STATE` (0x103) | precondition: `set_ct_model*` before `set_sensor_class`; address change on a production module | the `set_*` / address-change sections above |
| `ESP_ERR_NOT_SUPPORTED` (0x106) | per-channel `set_ct_model_ch` on v1.0/1.1; `rbamp_broadcast_latch` on v1 | check `rbamp_firmware_version(dev)` |
| `ESP_ERR_INVALID_RESPONSE` (0x108) | period snapshot stale; or `REG_VERSION` = 0/0xFF on `rbamp_begin` | the "Period snapshots are always stale" / "Module doesn't respond" sections |
| `ESP_ERR_NO_MEM` (0x101) | failed to allocate memory for the handle | check the heap budget (`esp_get_free_heap_size()`) |

## Bus-level debug with a logic analyzer

For deep debugging, when the component can no longer tell you what's
happening on the wire, capture SDA + SCL with a logic analyzer
(Saleae, DSLogic Plus, Sigrok-compatible):

- Sample rate ≥ 1 MS/s at 100 kHz I²C; ≥ 4 MS/s at 400 kHz.
- The I²C decoder in Sigrok / Saleae will show ACK / NACK on every
  byte + the address phase.
- Compare your code's calls (`rbamp_read_voltage()`, etc.) with the
  expected byte sequence — they should match exactly (single-byte per
  address phase, no auto-increment).

If the component's behavior diverges from the capture, open an issue
with the capture file attached (`.sal` / `.dsl` / `.csv`).

## When to contact support

If you've worked through the relevant section above and the problem
persists, open an issue:

[github.com/rb-amp/rbamp-esp-idf/issues](https://github.com/rb-amp/rbamp-esp-idf/issues)

In the issue, include:

- **Target SoC** (ESP32 / S2 / S3 / C2 / C3 / C6 / H2 / P4) +
  **ESP-IDF version** (output of `idf.py --version`).
- **rbamp component version** (e.g. 1.1.0).
- **Module firmware version** — `rbamp_firmware_version(dev)` from the
  logs.
- **A minimal IDF project** reproducing the problem (~30 LOC in
  `main/main.c`).
- **The `esp_err_t` values + `rbamp_err_to_str(err)`** at the moment
  of failure, from the logs under the `rbamp` tag.
- **Relevant Kconfig**: `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ`,
  `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS`, `CONFIG_RBAMP_LOG_LEVEL_*`.
- **(If available)** a logic-analyzer capture of the failure moment.

## Links

- [05 · Quickstart](05_quickstart.md) — your first working project
- [09 · API reference](09_api_reference.md) — the full API +
  warnings on the public-with-WARNING methods + the Kconfig section
- [03 · Current sensor selection](03_sensor_selection.md) — the
  SCT-013 model table, dual-CT topology for a wide dynamic range, and
  approaches to boosting sensitivity at low currents


---

[← API Reference](09_api_reference.md) | [Contents](README.md) | [Changelog →](11_changelog.md)
