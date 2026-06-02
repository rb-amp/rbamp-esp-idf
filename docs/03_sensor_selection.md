# 03 · Current sensor selection

This chapter answers two questions:

1. Which current sensor to choose for a given load.
2. How to tell the rbAmp module about your choice so that the
   factory calibration for that combination loads automatically.

The physical connection of the sensor (clamp orientation, L/N
polarity check) is covered in [04_hardware.md](04_hardware.md). This
chapter is about **model selection** and **API calls**.

## Sensor class

rbAmp modules work with CT clamps of the **SCT-013** family. The
sensor class is determined by the module's hardware revision and is
fixed at the factory — the user reports their choice via
`rbamp_set_sensor_class()` before selecting a specific CT clamp
model.

## Choosing the SCT-013 model

The SCT-013 family has five models, differing in the maximum primary
current:

| `code` | Model | Current range | Typical use |
|:---:|---|---|---|
| **1** | SCT-013-005 | 0…5 A | Small loads — lamps, low-power electronics, a single switch |
| **2** | SCT-013-010 | 0…10 A | A single medium-power appliance — refrigerator, washing machine, air conditioner up to 2 kW |
| **3** | SCT-013-030 | 0…30 A | A medium household's mains feed — up to ~7 kW |
| **4** | SCT-013-050 | 0…50 A | A large feed — electric heating, EV charger, a house with peak loads |
| **5** | SCT-013-100 | 0…100 A | A main feed for a house or small office — up to ~23 kW |

### How to pick the right model

The basic rule:

1. **Determine the maximum current** that can flow in the circuit
   (the largest load + 30 % headroom).
2. Choose the model whose range this value fits into.
3. **Don't add more than 5× headroom.** An SCT-013-100 clamp on a
   circuit with a maximum current of 5 A will work, but it will give
   low resolution and a large error at typical values.

### Headroom

An SCT-013 clamp operates without saturation within its rated range.
Brief peaks (compressor inrush, inductive load) may exceed the
rating by 5–7× — this is **normal**, the clamp physically
withstands it, but the measurement becomes nonlinear above the
rating.

If your load has a peak current above the clamp's rating, choose the
next size up. For example, for a washing machine with a 12 A inrush
current (but a 2–3 A rating while running), SCT-013-030 is better
than SCT-013-005.

## How to tell the module about your choice

**Two calls in the correct order are mandatory** — first
`rbamp_set_sensor_class()`, then `rbamp_set_ct_model()`. If you call
`rbamp_set_ct_model()` before `rbamp_set_sensor_class()` on v1.2+
firmware, the component returns `ESP_ERR_INVALID_STATE` and the
preset is NOT loaded.

```c
#include "driver/i2c_master.h"
#include "rbamp.h"

void setup_sensor(rbamp_handle_t dev) {
    /* Step 1: choose the sensor family.
     * MANDATORY before rbamp_set_ct_model(). */
    esp_err_t err = rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
    if (err != ESP_OK) {
        ESP_LOGE("app", "set_sensor_class: %s", rbamp_err_to_str(err));
        return;
    }

    /* Step 2: choose the model within the family.
     * 1 = SCT-013-005, 2 = SCT-013-010, 3 = SCT-013-030,
     * 4 = SCT-013-050, 5 = SCT-013-100. */
    err = rbamp_set_ct_model(dev, 3);   /* e.g., SCT-013-030 */
    if (err != ESP_OK) {
        /* Possible causes:
         *   ESP_ERR_INVALID_STATE — rbamp_set_sensor_class() was not called
         *                            before rbamp_set_ct_model() (on v1.2+ firmware)
         *   ESP_ERR_INVALID_ARG   — code outside the 1..5 range
         *   ESP_FAIL              — communication error (NACK after retries)
         */
        ESP_LOGE("app", "set_ct_model: %s", rbamp_err_to_str(err));
        return;
    }
}
```

> **The component deliberately does NOT call
> `rbamp_set_sensor_class()` on your behalf.** If this step is
> skipped, `rbamp_set_ct_model()` returns `ESP_ERR_INVALID_STATE`
> without writing to flash. This is done so that the behavior is
> predictable and explicit — no "magic" in the public API.

After these two calls:

- The module saves both values to flash — the configuration survives
  a reset, a power cycle, and a firmware re-flash.
- From the factory preset table, the calibration coefficients for
  this specific combination (sensor class + model) are loaded. You
  don't need to touch any manual calibration registers.
- The next call to `rbamp_read_current(dev, 0, &i)` already returns
  a value in amperes with the correct scaling.

**The total time for both calls** is about **1.4 seconds** (two
flash-write operations × ~700 ms each, limited by the flash page
erase time). It's done **once** at first installation; the
configuration is saved to flash and is not repeated.

> If you already chose the sensor on the first run and are simply
> rebooting the ESP32 — you don't need to repeat the
> `rbamp_set_sensor_class()` and `rbamp_set_ct_model()` calls, the
> module remembers the previous choice. But it's not harmful either —
> calling again with the same value simply rewrites the same byte.

### Verifying the setup

A simple sanity check after `rbamp_set_ct_model()`:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "rbamp_check";

void sanity_check(rbamp_handle_t dev) {
    ESP_LOGI(TAG, "Ready. Connect a purely resistive load "
                  "(e.g., an incandescent lamp).");
    ESP_LOGI(TAG, "Expecting a stable PF ~= 1.0 and a positive P.");

    while (1) {
        float u = NAN, i = NAN, p = NAN, pf = NAN;
        rbamp_read_voltage(dev, 0, &u);
        rbamp_read_current(dev, 0, &i);
        rbamp_read_power(dev, 0, &p);
        rbamp_read_power_factor(dev, 0, &pf);

        ESP_LOGI(TAG, "U=%.1f V  I=%.2f A  P=%.1f W  PF=%.2f",
                 (double)u, (double)i, (double)p, (double)pf);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
```

On a purely resistive load (incandescent lamp, electric kettle,
heating element), expect:

- `U` ≈ 220–240 V (for 230 V grids)
- `I` ≈ corresponds to the load's power (P / U)
- `P` > 0 and stable
- `PF` ≈ 1.0 (definitely positive)

If something doesn't add up, see [10_troubleshooting.md](10_troubleshooting.md).

## Modules with multiple current channels

On the `UI2`, `UI3`, `I2`, `I3` modules, each current channel has an
**independent** SCT-013 model selection. You can connect, for
example, an SCT-013-005 on channel 0 (a single outlet), an
SCT-013-030 on channel 1 (the electric stove line), and an
SCT-013-100 on channel 2 (the main feed).

The API for per-channel selection is `rbamp_set_ct_model_ch(dev, channel, code)`:

```c
rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);   /* once for all */

/* IMPORTANT: assign channels from highest to lowest (descending order).
 * First channel 2, then 1, then 0. See below for why. */
rbamp_set_ct_model_ch(dev, 2, 5);   /* channel 2: SCT-013-100 */
rbamp_set_ct_model_ch(dev, 1, 3);   /* channel 1: SCT-013-030 */
rbamp_set_ct_model_ch(dev, 0, 1);   /* channel 0: SCT-013-005 */
```

> ⚠ **Order matters.** Each `rbamp_set_ct_model_ch(dev, channel,
> code)` call also applies the same `code` to **channel 0** as a side
> effect — this is the legacy compatibility path with v1.1 firmware
> (the single-arg `rbamp_set_ct_model(dev, code)` always wrote to
> channel 0 directly). If you assign channels in forward order
> `(0,1) → (1,3) → (2,5)`, the last call overwrites ch0 with the
> value 5, and the final state will be `ch0=5, ch1=3, ch2=5` —
> incorrect.
>
> **The correct order is from the highest channel to the lowest**:
> the last call, with `channel=0`, fixes the final ch0 value.

If all channels get the **same** model, the order doesn't matter
(the side effect is idempotent):

```c
rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
rbamp_set_ct_model_ch(dev, 0, 1);
rbamp_set_ct_model_ch(dev, 1, 1);   /* ch0 is overwritten with the same value */
rbamp_set_ct_model_ch(dev, 2, 1);
```

The single-parameter `rbamp_set_ct_model(dev, code)` remains for
backward compatibility with UI1 modules (it applies to channel 0).
On multi-channel modules it is equivalent to
`rbamp_set_ct_model_ch(dev, 0, code)`.

> On v1.0/v1.1 firmware (REG_VERSION < 0x03) the per-channel opcodes
> `CMD_SET_CT_MODEL_CHn` do not exist —
> `rbamp_set_ct_model_ch(dev, channel, code)` returns
> `ESP_ERR_NOT_SUPPORTED` with no write. Use the single-parameter
> `rbamp_set_ct_model(dev, code)`, which writes to channel 0 via the
> legacy path.

## Advanced setup: two clamps of different ratings on one wire

> ⚙ **An additional pattern, not a basic one.** This section
> describes an optional technique for improving resolution at low
> currents. For most installations, a single clamp matched to the
> load range is sufficient. Use dual-CT only if you have a specific
> accuracy requirement at currents < 1 A.

### When this applies

- Multi-channel modules **UI2 / UI3** on v1.2+ firmware.
- The same wire needs to be measured both for small loads (≤ 1 A)
  and for peak events (≥ 5 A) with equal quality.
- A typical example: an apartment feed where during the day there's
  50–100 W of standby, and in the evening — a kettle or electric
  stove starting up at 3+ kW.

### The idea

**Two** SCT-013 clamps of different ratings are installed on the
same wire:

- Channel 0 — a small clamp (e.g., SCT-013-005, 5 A): it sees small
  currents with better resolution and a lower noise floor.
- Channel 1 — a large clamp (e.g., SCT-013-030 or higher): it
  handles currents above the small clamp's overload point without
  saturation.

The master itself chooses which channel to use depending on the
current value — while the small clamp is in its linear range, its
reading is more accurate; when exceeded, it switches to the large
one.

### Configuration (descending order — mandatory)

First the sensor class **once**, then the models **from the highest
channel to the lowest** — otherwise the legacy `REG_CT_MODEL` side
effect will overwrite channel 0 with the value of the last call (see
the warning in the previous section, "Modules with multiple current
channels").

```c
rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);   /* once */

/* First channel 1 (the large clamp), then channel 0 (the small one) —
 * so that the final ch0 value is correct. */
rbamp_set_ct_model_ch(dev, 1, 3);   /* ch1 = SCT-013-030 (0..30 A) */
rbamp_set_ct_model_ch(dev, 0, 1);   /* ch0 = SCT-013-005 (0..5 A)  */
```

Final state: `ch0 = SCT-013-005`, `ch1 = SCT-013-030`. ✓

### Aggregation logic on the master side

The simplest pattern is to switch on a threshold:

```c
float read_combined_current(rbamp_handle_t dev) {
    float i_low  = NAN;
    float i_high = NAN;
    esp_err_t err_low  = rbamp_read_current(dev, 0, &i_low);   /* small */
    esp_err_t err_high = rbamp_read_current(dev, 1, &i_high);  /* large */

    /* While the small clamp is far from saturation, it gives better
     * accuracy at low currents. We switch to the large one as we
     * approach the overload point.
     *
     * The 4.5 A threshold for the SCT-013-005 is PROVISIONAL; the exact
     * value will be determined by bench validation (see below on IP-010).
     * The behavior near the threshold is a matter of measurement, not
     * estimation. */
    if (err_low == ESP_OK && i_low < 4.5f) {
        return i_low;
    }
    return (err_high == ESP_OK) ? i_high : NAN;
}
```

> 📷 **An installation diagram is forthcoming.** Two clamps on one
> wire are physically possible on most household-gauge cables, but
> they need a bit of room in the panel. A detailed diagram —
> including the arrow orientation of both clamps and the permissible
> distances between them — will appear here as it is prepared.


> ⚙ **Bench validation.** The exact figures for the dual-CT pattern
> (behavior near the threshold, temperature drift, divergence of the
> two clamps in the overlapping range) are established by the IP-010
> measurement program (the one following IP-001). Until it is
> complete, treat dual-CT as a pilot pattern; for critical
> applications, a single clamp matched to the upper load range is
> preferable.

### Approaches to improving sensitivity at low currents

If your load has a large dynamic range (for example, 1 W standby for
a router vs. a 2000 W immersion heater on the same outlet), a single
clamp sized for the upper limit loses the lower currents in the
noise.

Three strategies in increasing order of complexity:

1. **Size the CT for the maximum, not "with headroom".** The most
   common mistake is to put an SCT-013-100 (100 A) on a household
   outlet with a typical draw of 0.5–10 A. The signal sits in the
   lower 1–10 % of the ADC range — where the noise becomes comparable
   to the signal. For a household scenario (16 A outlet), SCT-013-030
   is optimal; for connecting a single device (≤ 5 A) — SCT-013-005.
2. **Dual-CT topology** (requires a UI2/UI3 SKU): a small clamp for
   the lower range + a large one for the upper, with the master
   choosing based on a threshold. See the "Dual-CT topology" section
   above — the pattern is a pilot, the numbers refined by the IP-010
   program.
3. **Bench calibration of the noise floor** (factory-side): IP-001
   characterizes the noise floor on a test bench; the results are
   baked into the firmware's calibration array. On the user side,
   nothing needs to be done beyond `setSensorClass()` +
   `setCTModel()`. Until the program is complete, specific low-current
   accuracy figures are not published.

## What's next

- [04 · Hardware connection](04_hardware.md) — physically connecting
  the clamp, arrow orientation, L/N polarity
- [05 · Quickstart](05_quickstart.md) — a full first-light project
- [06 · Examples](06_examples.md) — working scenarios for different
  loads
- [10 · Troubleshooting](10_troubleshooting.md) — what to do if the
  readings are odd (negative PF, unstable I, etc.)


---

[← Tier Support](02_tiers.md) | [Contents](README.md) | [Hardware Setup →](04_hardware.md)
