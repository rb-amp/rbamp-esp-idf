# 05 · Quickstart

A five-minute hello-world: add the component to an IDF project, wire up the
module, select a sensor, take your first RT reading and your first per-period
snapshot (Wh).

The details (pinout for a specific ESP32 chip, multi-module bus, picking an
SCT-013 by range) live in the adjacent chapters:

- [04 · Wiring](04_hardware.md) — hardware details and per-chip GPIO defaults
  for all supported ESP32 chips
- [03 · Current sensor selection](03_sensor_selection.md) — which SCT-013 model
  to use and why

## What you'll need

- An rbAmp module (any module line; for simplicity a **UI1** — one voltage input plus one current channel; other variants are listed in [03](03_sensor_selection.md#sku-lineup--what-fits-the-task))
- Any ESP32-family chip (`esp32`, `esp32s2`, `esp32s3`,
  `esp32c2`, `esp32c3`, `esp32c6`, `esp32h2`)
- ESP-IDF **v5.2+** installed (`. ~/esp/esp-idf/export.sh` or
  `%IDF_PATH%\export.bat` on Windows)
- An SCT-013 CT clamp rated for your maximum current (5A / 10A / 30A / 50A / 100A)
- A 5 V supply to power the module (from a USB-5V DevKitC or external)
- An AC circuit to measure (a lamp, a kettle, a household appliance)

## Step 0 — Set the target

If this is a fresh ESP-IDF project and `idf.py set-target` hasn't been run yet,
do it **before the first `idf.py build`** — otherwise the build fails with
"no target specified":

```sh
idf.py set-target esp32      # or esp32s3 / esp32c3 / esp32c6 / ...
```

Without this step the build fails.

## Step 1 — Add the component to the project

### As a git dependency (recommended)

In your project's `main/idf_component.yml` (create the file if it does not
exist). The ESP-IDF Component Manager fetches the tagged release:

```yaml
dependencies:
  rbamp:
    git: https://github.com/rb-amp/rbamp-esp-idf.git
    version: v1.7.2
```

The component is downloaded on the next `idf.py build` (or
`idf.py reconfigure`).

### Manually (git submodule / clone)

```sh
mkdir -p components/
cd components/
git clone https://github.com/rb-amp/rbamp-esp-idf.git rbamp
```

Or add the component path to the project root's `CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
     ${CMAKE_CURRENT_LIST_DIR}/components/rbamp)
# the directory must be named `rbamp`
```

### The dependency in `main/`

```cmake
# main/CMakeLists.txt
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES rbamp
)
```

The component pulls in `esp_driver_i2c`, `esp_timer` and `log` — that's
everything the basic Step 3 scenario needs. The networking / SPIFFS / OLED
scenarios require additional `REQUIRES` — see
[06 · Examples](06_examples.md), the CMakeLists table at the start of the chapter.

## Step 2 — Wiring

Four wires + optional DRDY:

| rbAmp pin | ESP32 (default) | ESP32-S3 (default) | ESP32-C3 (default) |
|---|---|---|---|
| `VCC` | +5 V (4.5 to 5.5 V) | same | same |
| `GND` | GND | GND | GND |
| `SDA` | GPIO21 | GPIO8 | GPIO5 |
| `SCL` | GPIO22 | GPIO9 | GPIO6 |

The supply **must be 5 V**. The I²C lines run at 3.3 V logic — directly
compatible with ESP32 GPIO. The module board already has built-in 4.7 kΩ
pull-up resistors — for a single module no external ones are needed.

The full pinout table for each ESP32 chip is in
[04 · Wiring](04_hardware.md).

> ⚠ **Mains voltage can kill.** Disconnect and lock out the circuit before
> connecting the L/N terminals or fitting the clamp; have a qualified person
> do mains wiring and any work inside a panel. Clamp **ONE conductor only
> (L)** — never the whole cable. Fuse the L tap per local rules.

The SCT-013 CT clamp closes around the **line conductor (L)**, with the arrow
on the clamp body pointing **in the direction of current toward the load**.
A UI module also needs its L/N voltage connection; the CT clamp plugs into the
module's 3.5 mm CT jack (labelled `I0`, `I1`, … on the PCB); the mains side has `L` and `N` terminals. See
[04 · Wiring, "Mains and CT connection"](04_hardware.md#mains-and-ct-connection).

## Step 3 — First project (first reading)

A minimal `main/main.c`: bring up the bus, tell the module which CT clamp you
fitted, then print U / I / P / PF once a second.

```c
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

static const char *TAG = "app";

void app_main(void) {
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port  = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_21,            /* ESP32 — adjust for your chip */
        .scl_io_num = GPIO_NUM_22,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* optional with the module's own pull-ups */
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    rbamp_handle_t dev = NULL;
    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));    /* default address 0x50 */

    /* rbamp_begin blocks while it tries to reach the slave;
     * it runs a primer-LATCH and a valid-check, ~250 ms. */
    while (rbamp_begin(dev) != ESP_OK) {
        ESP_LOGE(TAG, "rbAmp not responding — check wiring and power");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Module ready.");

    /* Sensor configuration (see Step 4): class first, then model.
     * Code 3 = SCT-013-030; use the code of the clamp you fitted. */
    if (rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013) != ESP_OK ||
        rbamp_set_ct_model(dev, 3) != ESP_OK) {
        ESP_LOGE(TAG, "sensor configuration failed");
        return;
    }
    ESP_LOGI(TAG, "Sensor configured.");

    while (1) {
        float u = NAN, i = NAN, p = NAN, pf = NAN;
        if (rbamp_read_voltage(dev, 0, &u)        != ESP_OK) u  = NAN;
        if (rbamp_read_current(dev, 0, &i)        != ESP_OK) i  = NAN;
        if (rbamp_read_power(dev, 0, &p)          != ESP_OK) p  = NAN;
        if (rbamp_read_power_factor(dev, 0, &pf)  != ESP_OK) pf = NAN;

        ESP_LOGI(TAG, "U=%.1f V  I=%.3f A  P=%.1f W  PF=%.3f",
                 (double)u, (double)i, (double)p, (double)pf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

Build and flash + open the monitor:

```sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

(On Windows — `idf.py -p COM3 ...`.)

Expected output with a load switched on (values depend on your load):

```text
I (1234) app: Module ready.
I (2634) app: Sensor configured.
I (3634) app: U=230.4 V  I=0.523 A  P=119.8 W  PF=0.987
```

> `U` shows roughly the mains voltage (220-240 V for 230 V grids) — that means
> the module is wired correctly.
>
> **If `I` stays at `0.000 A` with a load on:** the module does not know which
> CT clamp is in use (the sensor configuration step was skipped or failed) —
> without it the current readings stay at zero. `PF` is undefined when
> `I=0` and may read `nan`, `0`, or a placeholder; ignore it until the
> current is non-zero. See [10 · Troubleshooting](10_troubleshooting.md),
> "Current reads zero on a working load".

## Step 4 — What the sensor configuration does

The two calls in Step 3 tell the module the class and model of the sensor.
Without them the calibration coefficients aren't loaded and the current
readings stay at zero. Order matters: class first, then model.

```c
/* Step A: sensor class. The current rbAmp SKU uses SCT-013 clamps. */
rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013);
/* Step B: model. For example, SCT-013-030 for a household feed up to ~7 kW. */
rbamp_set_ct_model(dev, 3);
```

Model codes:

| `code` | Model | Range | Typical use |
|:---:|---|---|---|
| 1 | SCT-013-005 | 0..5 A | Small loads, a single outlet |
| 2 | SCT-013-010 | 0..10 A | Refrigerator, washing machine |
| 3 | SCT-013-030 | 0..30 A | Household feed up to ~7 kW |
| 4 | SCT-013-050 | 0..50 A | EV charger, electric heating |
| 5 | SCT-013-100 | 0..100 A | Main service entry of a house or small office |
| 6 | SCT-013-020 | 0..20 A | Water heater, boiler, large appliance |
| 7 | SCT-013-060 | 0..60 A | Industrial sub-meter |

More on selection (including the other supported codes) —
[03 · Current sensor selection](03_sensor_selection.md).

> These two calls are made **once** at first installation — the choice is saved
> to the module's flash and survives a reset. Total time is about
> **1.4 seconds** (two flash writes × ~700 ms each).
>
> On subsequent firmware runs you can skip `rbamp_set_sensor_class()` and
> `rbamp_set_ct_model()` (the module remembers). But it does no harm either —
> calling again with the same value does not write.

## Step 5 — Energy accounting (Wh)

The module returns only instantaneous quantities + the average power over a
period. **The Wh themselves are computed by the component** from
`esp_timer_get_time()`:

```text
E_Wh += avg_P × master_dt_s / 3600
        [W]    [seconds]      →  [Wh]
```

where `master_dt_s` is the number of seconds between two successful calls to
`rbamp_read_period_snapshot()`. The component keeps the timestamp of the last
latch in the handle and computes the difference itself.

A minimal template for periodic accounting (once a minute):

```c
void app_main(void) {
    /* ...bus + rbamp_new + rbamp_begin + set_sensor_class + set_ct_model (Step 3)... */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));   /* 60-second period */

        rbamp_period_snapshot_t snap;
        esp_err_t err = rbamp_read_period_snapshot(dev, &snap, 50, false);
        if (err != ESP_OK || !snap.valid) {
            ESP_LOGW(TAG, "snapshot error: %s", rbamp_err_to_str(err));
            continue;
        }

        ESP_LOGI(TAG, "avg P over period: %.2f W   accumulated: %.4f Wh   dt=%u ms",
                 (double)snap.avg_p[0],
                 rbamp_energy_wh(dev, 0),
                 (unsigned)snap.master_dt_ms);
    }
}
```

> After Step 3 the calls to `rbamp_set_sensor_class()` and
> `rbamp_set_ct_model()` have already run once and are saved to the module's
> flash; in the template above they are there for the firmware re-run — the
> module ignores a repeated call with the same value. Error handling is partly
> omitted for brevity — add extra `if (err != ESP_OK)` guards if you want to
> catch the rare communication glitches at startup.

What `rbamp_read_period_snapshot()` does under the hood (with
`skip_latch=false`):

1. Sends the module the period-latch command (`CMD_LATCH_PERIOD`).
2. Waits `settle_ms` (50 ms by default) while the module prepares the snapshot.
3. Checks the ready flag; reads the average/peak power.
4. Updates the internal Wh counter: `+= avg_p × master_dt_s / 3600`
   (if the component is built without `CONFIG_RBAMP_DISABLE_ENERGY`).

> **The Wh counter lives in RAM.** `rbamp_energy_wh()` restarts at 0 after
> every ESP32 reset or power loss; persist it yourself if you need it to
> survive (see [06 · Examples](06_examples.md), Scenario 13).

The first call after `rbamp_begin()` is a primer: the module returns whatever
it has accumulated since power-on (an interval unsuitable for tariff
accounting). The component knows on its own that this snapshot must be
discarded — user code never sees it.

Expected output:

```text
I (61234) app: avg P over period: 120.18 W   accumulated: 2.0036 Wh   dt=60012 ms
I (121234) app: avg P over period: 120.21 W   accumulated: 4.0073 Wh   dt=60005 ms
...
```

## Step 6 — Quickstart for a fleet of N modules

This quickstart shows working with a **single** module. The real canonical
scenario for the library is **several** modules on one bus under a single
fleet-handle (mains + N sub-loads). The full example is in
[06 · Examples](06_examples.md), scenario 1. > **Prerequisites for a fleet.** All modules ship at address `0x50`, and
> `rbamp_fleet_scan` excludes modules that share an address. Give each
> module its own address first, **one module on the bus at a time**
> ([06 · Examples](06_examples.md), Scenario 2, `rbamp_provision`), and set
> the sensor class and CT model on each module ([03](03_sensor_selection.md)).
> Otherwise the fleet comes up empty or excluded, and currents read 0.
> Power and Wh need a **UI** variant: a "mains minus sum of loads" balance
> needs a UI module on every load.

A minimal fleet skeleton:

```c
#include "rbamp_fleet.h"

void app_main(void) {
    /* ... bus init as in Step 3 ... */

    rbamp_fleet_t fleet;
    ESP_ERROR_CHECK(rbamp_fleet_create(bus, &fleet));

    size_t added = 0;
    esp_err_t scan = rbamp_fleet_scan(fleet, /*match_product=*/true, &added);
    if (scan == ESP_ERR_INVALID_STATE) {
        ESP_LOGE("app", "bus wedged — see ch.10 \"Fleet: INVALID_STATE\"");
        return;
    }
    ESP_LOGI("app", "fleet: %u modules; excluded: %u",
             (unsigned)added,
             (unsigned)rbamp_fleet_excluded_count(fleet));

    rbamp_snapshot_t snaps[RBAMP_FLEET_MAX_MODULES];
    rbamp_fleet_poll_t status[RBAMP_FLEET_MAX_MODULES];

    while (1) {
        size_t n_ok = 0;
        rbamp_fleet_poll_all(fleet, snaps, status,
                             RBAMP_FLEET_MAX_MODULES, &n_ok);

        float p_total;
        rbamp_fleet_total_power(fleet, &p_total);
        ESP_LOGI("app", "fleet: %u/%u modules OK, total P = %.1f W",
                 (unsigned)n_ok, (unsigned)rbamp_fleet_count(fleet), p_total);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

In the canonical 80% scenario one of the modules is a `UI1` on the mains feed
(it provides `total_power` and `total_energy_wh`), and the rest are `I2`/`I3`
current sub-meters (current only — no power or Wh; use UI modules where you need power or energy per load). The details and advanced scenarios (mains+sub-loads
disaggregation, GC sync for tight snapshots, the provisioning
workflow) are in chapter 06.

## What's next

- [01 · Overview](01_overview.md) — what rbAmp is and what the component does
- [02 · Module lines](02_tiers.md) — which tier for which task
- [06 · Examples](06_examples.md) — working scenarios: **mains + N
  sub-loads (the 80% canon)**, provisioning workflow, multi-channel
  mixed-CT, fleet GC sync, MQTT, deep-sleep
- [07 · DIY integrations](07_diy_integrations.md) — Home Assistant /
  Node-RED / OpenHAB
- [08 · Cloud integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB Cloud
- [09 · API reference](09_api_reference.md) — the full public API
  (including the Fleet manager + per-device fleet config + multi-channel
  configuration + the v1.3 error model + identification and provisioning)
- [10 · Troubleshooting](10_troubleshooting.md) — if something doesn't work
  (i2c-hang three-layer mitigation, fleet conflict / wedge /
  provisioning failure modes)

