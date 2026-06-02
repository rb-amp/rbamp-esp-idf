# rbamp — ESP-IDF component for rbAmp I²C AC sensor / dimmer modules

[![protocol: 1.2](https://img.shields.io/badge/protocol-1.2-blue)](https://rbamp.com/docs/modules-basic-standard-api-reference)
[![ESP-IDF: >=5.2](https://img.shields.io/badge/ESP--IDF-%3E%3D5.2-orange)](https://docs.espressif.com/projects/esp-idf/)
[![license: MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

ESP-IDF C component for the rbAmp I²C AC sensor / dimmer module. Built on the
modern `i2c_master` driver (`esp_driver_i2c`, stable since IDF v5.2). Read RMS
voltage / current, signed active power, power factor, line frequency, and
tariff-period energy snapshots over I²C.

## Supported targets

ESP-IDF **>= 5.2** · `esp32`, `esp32s2`, `esp32s3`, `esp32c2`, `esp32c3`, `esp32c6`, `esp32h2`

## Installation

From the ESP Component Registry:

```bash
idf.py add-dependency "rbamp/rbamp^1.2"
```

## Quick start

```c
#include "driver/i2c_master.h"
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
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    rbamp_handle_t dev = NULL;
    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));
    ESP_ERROR_CHECK(rbamp_begin(dev));

    rbamp_snapshot_t s;
    if (rbamp_read_all(dev, &s) == ESP_OK) {
        printf("U=%.1f V  P0=%.1f W\n", (double)s.voltage, (double)s.power[0]);
    }
}
```

See [examples/](examples/) for 7 complete projects: quick read, multi-module
broadcast, LCD period display, MQTT publisher, Home Assistant discovery,
deep-sleep logger, and SPIFFS logger.

## Documentation

- In-repo guide: [docs/](docs/README.md) — overview, hardware, quickstart, API reference, troubleshooting
- Hosted protocol spec & API reference: <https://rbamp.com/docs/modules-basic-standard-api-reference>

## License

MIT. See [LICENSE](LICENSE).
