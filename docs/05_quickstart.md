# 05 · Quickstart

A five-minute hello-world: add the component to an IDF project, wire up
the module, select a sensor, get your first RT reading and your first
per-period snapshot (Wh).

The details (pinout for a specific ESP32 chip, multi-module bus,
selecting an SCT-013 by range) are in the neighboring chapters:

- [04 · Hardware connection](04_hardware.md) — hardware details and
  per-chip GPIO defaults for all 8 ESP32 chips
- [03 · Current sensor selection](03_sensor_selection.md) — which
  SCT-013 model to choose and why

## What you'll need

- An rbAmp module (any tier, UI1 for simplicity)
- Any ESP32-family chip (`esp32`, `esp32s2`, `esp32s3`,
  `esp32c2`, `esp32c3`, `esp32c6`, `esp32h2`, `esp32p4`)
- ESP-IDF **v5.2+** installed (`. ~/esp/esp-idf/export.sh` or
  `%IDF_PATH%\export.bat` on Windows)
- An SCT-013 CT clamp rated for your maximum current (5A / 10A / 30A / 50A / 100A)
- A 5 V source to power the module (from a USB-5V DevKitC or external)
- An AC circuit to measure (lamp, kettle, household appliance)

## Step 0 — Set the target

If this is a fresh ESP-IDF project and `idf.py set-target` hasn't been
run yet, do it **before the first `idf.py build`** — otherwise the
build fails with "no target specified":

```sh
idf.py set-target esp32      # or esp32s3 / esp32c3 / esp32c6 / ...
```

Without this step, the `idf.py add-dependency` below will work, but
`build` won't.

## Step 1 — Add the component to the project

### Via Component Manager (recommended)

In the project root:

```sh
idf.py add-dependency "rbamp/rbamp^1.2"
```

The component is downloaded from the ESP Component Registry on the
next build. The project manifest (`main/idf_component.yml`) is updated
automatically.

### Manually (git submodule / clone)

```sh
mkdir -p components/
cd components/
git clone https://github.com/rb-amp/rbamp-esp-idf.git rbamp
```

Or add the component path to the project root's `CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
     ${CMAKE_CURRENT_LIST_DIR}/path/to/rbamp/component)
```

### Dependency in `main/`

```cmake
# main/CMakeLists.txt
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES rbamp
)
```

The component pulls in `esp_driver_i2c`, `esp_timer`, and `log` —
that's everything you need for the basic Step 3 scenario. Network /
SPIFFS / OLED scenarios require additional `REQUIRES` — see
[06 · Examples](06_examples.md), the CMakeLists table at the start of
the chapter.

## Step 2 — Wiring

Four wires + optionally DRDY:

| rbAmp pin | ESP32 (default) | ESP32-S3 (default) | ESP32-C3 (default) |
|---|---|---|---|
| `VCC` | +5 V (4.5 to 5.5 V) | same | same |
| `GND` | GND | GND | GND |
| `SDA` | GPIO21 | GPIO8 | GPIO5 |
| `SCL` | GPIO22 | GPIO9 | GPIO6 |

Power **must be 5 V**. The I²C lines run on 3.3 V logic — directly
compatible with ESP32 GPIO. The module board already has built-in
4.7 kΩ pull-up resistors — no external ones are needed for a single
module.

The full pinout table for each ESP32 chip is in
[04 · Hardware connection](04_hardware.md).

The SCT-013 CT clamp snaps around the **line wire (L)**, with the
arrow on the clamp body **in the direction of current flow toward the
load**.

## Step 3 — First project (RT reading)

A minimal `main/main.c` — a link check with no sensor configuration:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

static const char *TAG = "app";

void app_main(void) {
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port  = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_21,            /* on ESP32 — adjust for your chip */
        .scl_io_num = GPIO_NUM_22,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    rbamp_handle_t dev = NULL;
    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));    /* default address 0x50 */

    /* rbamp_begin blocks while it tries to connect to the slave;
     * on v1.2 firmware it does a primer LATCH and a valid check ~250 ms. */
    while (rbamp_begin(dev) != ESP_OK) {
        ESP_LOGE(TAG, "rbAmp not responding — check the wires and power");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Module ready.");

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

Expected output (without sensor calibration):

```text
I (1234) app: Module ready.
I (2234) app: U=230.4 V  I=0.000 A  P=0.0 W  PF=nan
I (3234) app: U=230.4 V  I=0.000 A  P=0.0 W  PF=nan
```

> `U` shows roughly the mains voltage (220-240 V for 230 V grids) —
> this means the module is connected correctly. `I=0.000 A` even with
> a load switched on is normal at this stage: the module doesn't yet
> know which CT clamp is in use. `PF` at `I=0` is mathematically
> undefined (depends on the firmware — may be `nan`, `0`, or a
> placeholder) — the exact form of the value doesn't matter while the
> current is zero. The next step fixes this.

## Step 4 — Current sensor configuration

On v1.2 firmware, you **must** tell the module the sensor class and
model. Without this, the calibration coefficients aren't loaded and
the current readings stay at zero.

Add this to `app_main` after `rbamp_begin()`. **The while loop stays
the same as in Step 3** — only the setup changes:

```c
void app_main(void) {
    /* ...bus setup, rbamp_new, rbamp_begin as in Step 3... */

    /* Step 1: sensor class. The current rbAmp SKU is SCT-013. */
    if (rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013) != ESP_OK) {
        ESP_LOGE(TAG, "set_sensor_class failed");
        return;
    }

    /* Step 2: model. For example, SCT-013-030 for a household feed up to ~7 kW. */
    if (rbamp_set_ct_model(dev, 3) != ESP_OK) {
        ESP_LOGE(TAG, "set_ct_model failed");
        return;
    }

    ESP_LOGI(TAG, "Ready.");

    /* ...the same while loop with rbamp_read_* as in Step 3... */
}
```

Model codes:

| `code` | Model | Range | Typical use |
|:---:|---|---|---|
| 1 | SCT-013-005 | 0..5 A | Small consumers, a single outlet |
| 2 | SCT-013-010 | 0..10 A | Refrigerator, washing machine |
| 3 | SCT-013-030 | 0..30 A | Household feed up to ~7 kW |
| 4 | SCT-013-050 | 0..50 A | EV charger, electric heating |
| 5 | SCT-013-100 | 0..100 A | Main house feed |

More on the selection — [03 · Current sensor selection](03_sensor_selection.md).

> These two calls are made **once** at first installation — the choice
> is saved in the module's flash and survives a reset. Total time is
> about **1.4 seconds** (two flash writes × ~700 ms each).
>
> On subsequent firmware runs you can skip `rbamp_set_sensor_class()`
> and `rbamp_set_ct_model()` (the module remembers). But it does no
> harm either — calling again with the same value rewrites the same
> byte.

After rebooting the ESP32, the correct current value should appear:

```text
I (1234) app: Ready.
I (2234) app: U=230.4 V  I=0.523 A  P=119.8 W  PF=0.987
```

## Step 5 — Energy accounting (Wh)

The module returns only instantaneous quantities + the average power
over a period. **The component computes the Wh itself** from
`esp_timer_get_time()`:

```text
E_Wh += avg_P × master_dt_s / 3600
        [W]    [seconds]      →  [Wh]
```

where `master_dt_s` is the seconds between two successful
`rbamp_read_period_snapshot()` calls. The component keeps the
timestamp of the last latch in the handle and computes the difference.

A minimal template for periodic accounting (once a minute):

```c
void app_main(void) {
    /* ...bus + rbamp_new + rbamp_begin + set_sensor_class + set_ct_model... */

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

> After Step 4, the `rbamp_set_sensor_class()` and
> `rbamp_set_ct_model()` calls have already run once and are saved in
> the module's flash; in the template above they're there for
> subsequent firmware runs — the module ignores a repeated call with
> the same value. Error handling is partly omitted for brevity — add
> extra `if (err != ESP_OK)` guards if you want to catch rare link
> failures at startup.

What `rbamp_read_period_snapshot()` does under the hood (with
`skip_latch=false`):

1. Sends the module the period-latch command (`CMD_LATCH_PERIOD`).
2. Waits `settle_ms` (50 ms by default) while the module prepares the
   snapshot.
3. Checks the ready flag; reads the average/peak power.
4. Updates the internal Wh counter: `+= avg_p × master_dt_s / 3600`
   (if the component was built without `CONFIG_RBAMP_DISABLE_ENERGY`).

The first call after `rbamp_begin()` is a primer: the module returns
what it has accumulated since power-on (an interval unsuitable for
tariff accounting). The component knows on its own to discard this
snapshot — user code never sees it.

Expected output:

```text
I (61234) app: avg P over period: 120.18 W   accumulated: 2.0036 Wh   dt=60012 ms
I (121234) app: avg P over period: 120.21 W   accumulated: 4.0073 Wh   dt=60005 ms
...
```

## What's next

- [01 · Overview](01_overview.md) — what rbAmp is and what the component does
- [02 · Module tiers](02_tiers.md) — which tier for which task
- [06 · Examples](06_examples.md) — working scenarios: LCD meter, MQTT,
  deep-sleep, multi-module bus, event logging
- [07 · DIY integrations](07_diy_integrations.md) — Home Assistant /
  Node-RED / OpenHAB
- [08 · Cloud integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB Cloud
- [09 · API reference](09_api_reference.md) — the full public API
- [10 · Troubleshooting](10_troubleshooting.md) — if something doesn't work


---

[← Hardware Setup](04_hardware.md) | [Contents](README.md) | [Examples →](06_examples.md)
