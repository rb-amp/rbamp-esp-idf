# rbAmp — ESP-IDF component

[![IDF ≥ 5.2](https://img.shields.io/badge/esp--idf-%E2%89%A5%205.2-blue)](https://docs.espressif.com/projects/esp-idf/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Native ESP-IDF v5.2+ client component for the **rbAmp** I²C energy / power-factor
sensor (and dimmer) module. Built on the modern `i2c_master` driver
(`driver/i2c_master.h`); the legacy `driver/i2c.h` API is not used.

The library exposes an opaque-handle C API (`esp_err_t` returns,
output-via-pointer) and covers single-module metering, multi-channel modules,
and multi-module fleets on one bus.

## Features

- **Real-time metering** — voltage, current, real power, power factor and
  mains frequency, per channel.
- **Period energy** — latched average power and a Wh accumulator integrated
  from the device's canonical period duration.
- **Multi-channel** — variant-aware (UI1/UI2/I1/I2/I3); per-channel reads and
  mixed current-transformer configuration.
- **Fleet / multi-module** — bus scan with duplicate-address detect-and-exclude,
  one-virgin-at-a-time provisioning (two-phase address commit), General-Call
  fleet-synchronous latch, and aggregated power / energy.
- **Robust transport** — per-byte NACK retry, bus-reset recovery, per-quantity
  sanity bounds, and synchronous config-write verification.

## Install

From the ESP Component Registry:

```sh
idf.py add-dependency "rbamp/rbamp^1.3"
```

Or vendor the `rbamp` directory into your project's `components/`.

## Quick start

```c
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = GPIO_NUM_21,
        .scl_io_num        = GPIO_NUM_22,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    rbamp_handle_t dev;
    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));   /* default address */
    ESP_ERROR_CHECK(rbamp_begin(dev));             /* detect variant + prime */

    while (1) {
        rbamp_snapshot_t s;
        if (rbamp_read_all(dev, &s) == ESP_OK) {
            ESP_LOGI("app", "U=%.1f V  I=%.3f A  P=%.1f W  PF=%.2f  f=%.0f Hz",
                     (double)s.voltage, (double)s.current[0],
                     (double)s.power[0], (double)s.power_factor[0],
                     (double)s.frequency);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    rbamp_del(dev);
}
```

## API overview

All functions are declared in the three public headers; each is fully
documented with Doxygen.

| Header | Surface |
|---|---|
| [`include/rbamp.h`](include/rbamp.h) | Lifecycle (`rbamp_new` / `rbamp_begin` / `rbamp_del`), real-time reads (`rbamp_read_all`, `rbamp_read_voltage/current/power/power_factor/frequency`), period + energy (`rbamp_read_period_snapshot`, `rbamp_energy_wh`), configuration (`rbamp_set_sensor_class`, `rbamp_set_ct_model[_ch]`, `rbamp_configure_channels`), identity / capability (`rbamp_read_variant/capability/uid`), error + event signalling (`rbamp_has_error`, `rbamp_read_event_flags`, `rbamp_clear_error`), provisioning (`rbamp_prepare/commit_address_change`, `rbamp_save_user_config`), General-Call latch, diagnostics. |
| [`include/rbamp_fleet.h`](include/rbamp_fleet.h) | Multi-module bus manager: `rbamp_fleet_create`, `rbamp_fleet_scan`, `rbamp_provision`, `rbamp_fleet_assign_address`, `rbamp_fleet_poll_all`, `rbamp_fleet_enable_gc_all` / `rbamp_fleet_gclatch` / `rbamp_fleet_check_sync`, `rbamp_fleet_total_power` / `rbamp_fleet_total_energy_wh`, conflict detect-and-exclude. |
| [`include/rbamp_energy.h`](include/rbamp_energy.h) | Internal Wh accumulator (used through the `rbamp_energy_*` wrappers in `rbamp.h`). |

## Examples

Eight runnable projects under [`examples/`](examples/):

| Example | Shows |
|---|---|
| `quick_read` | Smoke test — U / I / P / PF once per second. |
| `lcd_period` | 60-second Wh counter on a character LCD. |
| `multi_module` | Several modules on one bus, period-synced. |
| `mqtt_publisher` | Publish power / Wh over MQTT. |
| `ha_discovery` | Home Assistant MQTT auto-discovery. |
| `spiffs_logger` | Per-minute CSV logging to SPIFFS. |
| `deep_sleep_logger` | RTC-persisted Wh across deep sleep. |
| `fleet_sync` | Fleet scan → General-Call sync → aggregated power / energy. |

## Configuration

`idf.py menuconfig` → *Component config → rbAmp client*: default I²C frequency,
transaction timeout, NACK retry attempts / gap, log level, and an option to
compile out the energy accumulator.

## Supported targets

`esp32`, `esp32s2`, `esp32s3`, `esp32c2`, `esp32c3`, `esp32c6`, `esp32h2`.
(`esp32p4` requires IDF ≥ 5.3; re-enabled once the minimum is bumped.)

## ESP-IDF version policy

- **Minimum:** ESP-IDF v5.2 — uses the `i2c_master` driver; the legacy
  `driver/i2c.h` is not supported.
- **ESP8266 RTOS SDK:** not supported — use the rbAmp Arduino library on the
  arduino-esp8266 core instead.

## License

MIT — see [LICENSE](LICENSE).
