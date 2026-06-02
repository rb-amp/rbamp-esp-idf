# 04 · Hardware Setup

This chapter describes the physical connection of the **rbAmp**
module to ESP32-family boards: the LV-side pinout, power
requirements, I²C voltage-level compatibility, bus length, and the
standard default pins for all 8 ESP32 chips (esp32, esp32s2, esp32s3,
esp32c2, esp32c3, esp32c6, esp32h2, esp32p4).

The mains side of the module is already routed inside the enclosure
and galvanically isolated — the user works only with the LV side
(power + I²C + optional `DRDY`).

## LV-side pinout

The module runs on 3.3 V I²C logic and is **5 V tolerant** on the
signal lines. All LV pins are fully galvanically isolated from mains.

| Pin | Signal | Purpose |
|---|---|---|
| `VCC` | +5 V (4.5..5.5 V) | Module power; at least 4.5 V for correct operation |
| `GND` | Ground | Common with the master — **mandatory** |
| `SDA` | I²C SDA | I²C data |
| `SCL` | I²C SCL | I²C clock |
| `DRDY` | Data-ready (optional) | Open-drain, LOW pulse ~10 µs every ~200 ms |

## Power

### VCC parameters

- **Nominal voltage**: 5 V DC
- **Allowed range**: 4.5..5.5 V
- **Current draw**: ~15 mA typical, ~25 mA peak on flash writes
- **Ripple**: < 50 mV peak-to-peak (for ADC accuracy)

The module carries an onboard regulator, RC filter, ceramic
capacitors, and a ferrite bead — no additional filtering is required
from the user. The module can be powered from the `+5V` or `+5V0`
line of any DevKitC board (USB-5V powered).

### I²C voltage-level compatibility

- `SDA` / `SCL` operate at **3.3 V logic** (the built-in pull-ups
  pull the lines up to 3.3 V).
- The lines are **5 V tolerant**, but the ESP32 family already runs
  at 3.3 V anyway — no level shifters are required.
- The ESP-IDF `i2c_master` driver uses open-drain outputs on the
  GPIO — compatible with the module's built-in pull-ups.

### Power-on behavior

- Cold start: ~250 ms until the first valid result.
- After a reset (software or brown-out): ~250 ms to recover.
- Until the first valid result, registers read as `0` and the
  `DATA_VALID` flag = 0.

The component itself waits for the module to be ready inside
`rbamp_begin(dev)` — user code simply calls `rbamp_begin()` and
carries on.

### Isolation

Inside the module there is a galvanic isolation barrier between the
mains side (CT clip, voltage divider) and the LV side (I²C).
Consequences:

- The module's `GND` is **not connected** to the mains line or
  neutral.
- Connecting the LV side to the ESP32 is safe — there is no risk of a
  short circuit through ground.

> ⚠ Do not open the module enclosure — doing so voids the factory
> calibration.

## Built-in pull-up resistors — on the board

**Important**: every rbAmp module has **built-in 4.7 kΩ pull-up
resistors on SDA and SCL to 3.3 V**. For a **single module** on the
bus, external pull-ups are not required — connect the ESP32
directly.

The ESP-IDF `i2c_master_bus_config_t` also lets you enable the
**internal GPIO pull-ups** via `flags.enable_internal_pullup = true`.
They are weak (~50 kΩ on the ESP32) and work in parallel with the
module's external 4.7 kΩ — the combination yields an effective
~4.4 kΩ, which is acceptable. For production deployments prefer
relying on the module's external 4.7 kΩ; the internal GPIO pull-ups
are for bench testing without a module on hand (for example, when
you are just checking a bus scan).

### When to disable the built-in pull-ups

If there are several rbAmp modules on the bus (or other I²C devices
with their own pull-ups), connecting them in parallel gives too low
an effective resistance, which:

- Increases bus current draw
- May exceed the maximum sink current of the I²C master's output
  stage
- Adds capacitive load on a long bus while doing little for noise
  immunity

Solution: on the bottom side of each module's board, near the pull-up
resistors, there are **jumper bridges** (silkscreen `Pull-Up`). Cut
them with a sharp blade:

- Leave the pull-ups enabled on **only one** module (or move them to
  a single point near the ESP32).
- Cut the bridges on the remaining modules.

### Rule of thumb

| rbAmp modules on the bus | Pull-ups |
|:---:|---|
| 1 | Leave as-is (the built-in 4.7 kΩ work) |
| 2 | Leave on one, cut on the other |
| 3+ | Cut on all modules, install one external 2.2…4.7 kΩ pair near the ESP32 |

> **If in doubt**: measure the resistance between SDA and VCC with
> the devices powered off. It should be in the **1.5..4.7 kΩ** range.
> Lower means too many active pull-ups in parallel.

## I²C bus parameters

- **Default address**: `0x50` (7-bit)
- **Address range**: `0x08..0x77` (reassigned at the factory)
- **Recommended speed on ESP32**: **50 kHz** (see the section on the
  retry+sanity discipline below)
- **Pointer auto-increment**: **NO**. Each byte read is a separate
  transaction with an explicit register address. The component does
  this correctly itself; user code never sees it.

### Bus length

I²C is a short bus (~30 cm by default). Tested topologies for rbAmp:

| Cable | Max length | Speed |
|---|---|---|
| Standard JST / flat 4-wire | up to 0.3 m | 100 kHz |
| Twisted pair UTP (cat-5/5e/6) — SDA+GND and SCL+GND in **separate pairs** | up to 1 m | 100 kHz |
| Twisted pair + I²C buffer (PCA9515 / TCA9617) | up to 3 m | 100 kHz |
| Differential bus (PCA9615 / LTC4332) | up to 100 m | 100 kHz |

> For lengths over 0.3 m, **twisted pair is mandatory**: SDA and SCL
> must be in **different** pairs, each with its own ground. SDA and
> SCL in the same pair create cross-coupled capacitance that distorts
> the edges.

### Multi-module bus

Several rbAmp modules can share a single I²C bus:

- **Number of modules**: up to ~16 (limited by total bus capacitance
  — ≤ 400 pF at 100 kHz)
- **Addresses**: each module has its own 7-bit address. All modules
  ship from the factory at `0x50` — readdress them one at a time
  before connecting them in parallel (a factory / installer
  operation).
- **Pull-ups**: follow the rule above — cut the built-in ones on all
  modules but one, or move them to a single point.
- **Syncing peak periods**: see
  [06 · Examples](06_examples.md), the "Monitoring multiple
  modules" scenario — sequential `rbamp_latch_period()` + a shared
  `vTaskDelay(50)` settle, then `rbamp_read_period_snapshot(skip_latch=true)`
  on each device. The skew between modules at 100 kHz is ~1 ms per
  device, < 0.2 % of the 60-second period.

> **Changing the address** is a factory / installer operation that
> is not part of the regular user API. See the `@warning` notice in
> [09 · API Reference](09_api_reference.md) on
> `rbamp_prepare_address_change` / `rbamp_commit_address_change`.
> If a deployed module needs a different address, contact your
> supplier.

## Pinout on the ESP32 family

All 8 ESP32 chips are supported (per `idf_component.yml`). Below are
the recommended default pins for I²C; for production deployments,
choose the GPIO based on your board's specific layout.

### ESP32 (original / DevKitC / WROOM)

```c
const i2c_master_bus_config_t bus_cfg = {
    .i2c_port  = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_21,
    .scl_io_num = GPIO_NUM_22,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
```

| Pin | GPIO |
|---|---|
| SDA | GPIO21 |
| SCL | GPIO22 |
| DRDY | any (e.g., GPIO15 — supports interrupts) |

### ESP32-S2

The same default GPIO21/22 as the original ESP32. The S2 has no
Bluetooth — an extra ~30 kB of heap is available for TLS scenarios.

### ESP32-S3

The S3 has no hardware-fixed I²C defaults — any GPIO via the matrix.
On most WROOM-S3 dev boards, GPIO8/GPIO9 are a comfortable choice
(check your board's schematic: on the N16R8 these pins may go to QSPI
flash; USB-JTAG occupies GPIO19/20). Adjust the `sda_io_num` /
`scl_io_num` fields in `i2c_master_bus_config_t`:

```c
.sda_io_num = GPIO_NUM_8,
.scl_io_num = GPIO_NUM_9,
```

The S3 has USB-OTG — usable for debugging without an external
USB-UART chip.

### ESP32-C2 / C3 / C6 / H2

RISC-V single-core chips (the C3 and C6 are single-core; the H2 is
single-core with BLE+802.15.4; the C2 is single-core with limited
RAM). The I²C pins are not hardware-fixed — example for the
**ESP32-C3** (on the C3-SuperMini, GPIO5/GPIO6 are comfortable; on
the C3-DevKitM-1, USB-JTAG occupies GPIO18/19, and GPIO8/9 are
strapping):

```c
const i2c_master_bus_config_t bus_cfg = {
    .i2c_port  = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_5,
    .scl_io_num = GPIO_NUM_6,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
```

For the other chips, check the target-specific ESP-IDF documentation
(`docs.espressif.com → ESP-IDF v5.2+ → your chip → API
reference → peripherals → I2C`).

### ESP32-P4 — not in targets yet

The ESP32-P4 is temporarily removed from the list of supported chips:
P4 support arrives in ESP-IDF v5.3+, while the component's minimum is
v5.2. To avoid giving a v5.2 user a build that fails with "target
esp32p4 not in idf", the P4 will be brought back as soon as the
minimum IDF is raised to v5.3.

## ESP32 baseline NACK pattern — 50 kHz bus speed

With the ESP-IDF v5 `i2c_master` driver, the current rbAmp firmware
exhibits a **~20 % NACK rate at 100 kHz** and **< 5 % at 50 kHz**.
The component addresses this in two layers:

1. **A default bus speed of 50 kHz** via the Kconfig symbol
   `CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000`. The component requests
   this frequency when it adds its device handle to the bus that you
   create.
2. **Per-byte retry** — `CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=3` × a 5 ms
   gap. At 50 kHz the residual error rate is < 0.8 %.

Override via `idf.py menuconfig` if needed:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=50000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=5     # for dense workloads
CONFIG_RBAMP_NACK_RETRY_GAP_MS=5
CONFIG_RBAMP_I2C_TIMEOUT_MS=200
```

Once firmware v1.1+ ships with the slave-side NACK fix, 100 kHz will
become the working default speed:

```text
CONFIG_RBAMP_DEFAULT_I2C_FREQ_HZ=100000
CONFIG_RBAMP_NACK_RETRY_ATTEMPTS=1
```

For more on diagnosing bus-level issues, see
[10 · Troubleshooting](10_troubleshooting.md).

## DATA_READY (DRDY)

An optional pin for polling optimization. If the application polls the
module no more often than once every 200 ms, `DRDY` can be left
unconnected.

### Electrical parameters

- **Output type**: open-drain (no active pull-up to VCC)
- **Idle level**: HIGH (requires a pull-up on the master side —
  internal `gpio_pullup_en()` or an external resistor; 10 kΩ to
  3.3 V is recommended)
- **Ready pulse**: LOW for ~10 µs after the RT registers are
  updated with fresh data
- **Pulse rate**: ~5 Hz (one pulse per ~200 ms RT window)

### Semantics

A falling edge on `DRDY` guarantees that **all RT registers are
synchronized and published** (the firmware updates them atomically
in the ISR before pulling the pin low). After the falling edge, the
master can read the RT block without risk of getting a split sample.

### Interrupt pattern — FreeRTOS semaphore

ESP-IDF offers a clean model: an ISR in IRAM gives a binary
semaphore, and a separate FreeRTOS task waits on the semaphore and
reads RT.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#define DRDY_GPIO GPIO_NUM_4   /* a free GPIO; do not use
                                 * GPIO0/2/12/15 — strapping pins
                                 * (ESP32 classic), they can break boot
                                 * if the open-drain DRDY goes LOW
                                 * during reset */
static SemaphoreHandle_t drdy_sem;

static void IRAM_ATTR drdy_isr_handler(void *arg) {
    BaseType_t hi_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(drdy_sem, &hi_task_woken);
    if (hi_task_woken) portYIELD_FROM_ISR();
}

void bidir_task(void *arg) {
    rbamp_handle_t dev = arg;
    while (1) {
        if (xSemaphoreTake(drdy_sem, portMAX_DELAY) == pdTRUE) {
            float p;
            if (rbamp_read_power(dev, 0, &p) == ESP_OK) {
                /* integrate + publish */
            }
        }
    }
}

void app_main(void) {
    /* ...rbamp_new + rbamp_begin... */

    drdy_sem = xSemaphoreCreateBinary();

    /* The ISR service is installed ONCE for the whole project — if another
     * component (LCD, encoder, whatever) has already installed it,
     * gpio_install_isr_service() returns ESP_ERR_INVALID_STATE.
     * That is normal, don't crash on it. ESP_INTR_FLAG_IRAM keeps the
     * ISR dispatcher in IRAM — important on single-core C3/C6/H2/S2,
     * where non-IRAM code is unavailable while the flash cache is disabled. */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << DRDY_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_cfg);
    gpio_isr_handler_add(DRDY_GPIO, drdy_isr_handler, NULL);

    /* Priority 6 — above idle (0), below WiFi (23) and the standard
     * MQTT (5+). 4096 bytes of stack is enough for rbamp_read_* +
     * ESP_LOG; if you add MQTT publish directly in this task,
     * raise it to 6144. On dual-core ESP32/S3, for a clean
     * separation from the WiFi task (usually core 0) you can pin
     * this task to core 1: xTaskCreatePinnedToCore(..., 1).
     * On single-core C3/C6/H2/S2, keep xTaskCreate. */
    xTaskCreate(bidir_task, "bidir", 4096, dev, 6, NULL);
}
```

`DRDY` is **optional** — polling at any rate ≤ 5 Hz works without it
too. The component does not depend on `DRDY` in any of its paths.

## References

- [05 · Quickstart](05_quickstart.md) — your first working project
- [06 · Examples](06_examples.md) — working scenarios (including the
  multi-module bus)
- [09 · API Reference](09_api_reference.md) — the "Kconfig
  symbols" section for detailed configuration of the I²C parameters
- [10 · Troubleshooting](10_troubleshooting.md) — bus-level debug, NACK
  pattern, retry+sanity discipline


---

[← Sensor Selection](03_sensor_selection.md) | [Contents](README.md) | [Quickstart →](05_quickstart.md)
