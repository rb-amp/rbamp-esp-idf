# 06 · Examples

This chapter walks through 9 working scenarios — from a minimal
"hello world" to a battery-powered deep-sleep logger. Most
scenarios have a ready-to-build IDF project in [`examples/`](../examples/)
that you can build and flash with
`idf.py -C examples/<name> build flash monitor`. The code below is a
**distillation of the key logic**, not the full project (`bus_cfg` /
WiFi bring-up / Kconfig are omitted for brevity).

| # | Scenario | Difficulty | Source |
|:---:|---|:---:|---|
| 1 | Quick read | minimal | [`examples/quick_read/`](../examples/quick_read/) |
| 2 | Wh meter on a 16×2 LCD | low | [`examples/lcd_period/`](../examples/lcd_period/) |
| 3 | Monitoring 3 modules on one bus | low | [`examples/multi_module/`](../examples/multi_module/) |
| 4 | UI3 + MQTT per channel | medium | [`examples/mqtt_publisher/`](../examples/mqtt_publisher/) |
| 5 | Bidirectional metering on the master side | medium | (composition) |
| 6 | Whole-home energy balance (3 modules) | high | (composition) |
| 7 | Event detection (EMA) | medium | (composition) |
| 8 | Local CSV logger on SPIFFS | medium | [`examples/spiffs_logger/`](../examples/spiffs_logger/) |
| 9 | Battery logger with deep-sleep | medium | [`examples/deep_sleep_logger/`](../examples/deep_sleep_logger/) |

> All scenarios assume that the sensor class and CT model have already
> been configured via `rbamp_set_sensor_class()` + `rbamp_set_ct_model()`
> (see [05 · Quickstart](05_quickstart.md), Step 4). This is done once
> at installation — the settings are stored in the module's flash and
> survive a reset.

### Additional `REQUIRES` for `main/CMakeLists.txt`

The base component pulls in `esp_driver_i2c`, `esp_timer`, and `log`.
Some scenarios use additional ESP-IDF components — add them to the
`REQUIRES` of your `main/`:

| Scenario | Additional `REQUIRES` |
|---|---|
| 1 — Quick read | — (just `rbamp`) |
| 2 — Wh meter on LCD | `rbamp` (the LCD driver is a separate component of your choice) |
| 3 — Multi-module | — |
| 4 — UI3 + MQTT | `rbamp mqtt esp_wifi esp_event esp_netif nvs_flash` |
| 5 — Bidirectional metering | — |
| 6 — Energy balance | — |
| 7 — Event detection | — |
| 8 — SPIFFS logger | `rbamp spiffs` |
| 9 — Deep-sleep logger | `rbamp esp_sleep esp_wifi esp_event esp_netif nvs_flash mqtt` (if you publish) |

The network components also require `nvs_flash_init()` in `app_main`
plus WiFi bring-up. For brevity, all of this is omitted in the snippets
below; the full project is always in [`examples/<name>/`](../examples/).

> **Important note about the snippets below.** Each snippet is a
> **distillation of the key logic**, not a full `app_main`. In working
> code, the line `rbamp_handle_t dev` is always preceded by
> `i2c_master_bus_config_t bus_cfg = {...}; i2c_new_master_bus(&bus_cfg, &bus);
> rbamp_new(bus, 0x50, &dev); rbamp_begin(dev);` — as shown in Scenario 1
> below. When you are done, call `rbamp_del(dev)` (we omit it where
> `app_main` enters an infinite loop — the handle lives until the chip
> reboots).

---

## Scenario 1 — Quick read

**Goal:** print U / I / P / PF / frequency to the ESP-IDF log once per
second. This is the very "hello world" you wrote in
[05 · Quickstart](05_quickstart.md), but via `rbamp_read_all()` — a
single-shot read of the entire RT block into one struct.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

static const char *TAG = "quick_read";

void app_main(void) {
    /* ...bus_cfg setup, rbamp_new + rbamp_begin as in the Quickstart... */
    rbamp_handle_t dev = NULL;

    while (1) {
        rbamp_snapshot_t s;
        if (rbamp_read_all(dev, &s) != ESP_OK) {
            ESP_LOGE(TAG, "read fail");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "U=%.1f V  f=%.1f Hz  channels=%u",
                 (double)s.voltage, (double)s.frequency, s.channels);
        for (uint8_t ch = 0; ch < s.channels; ++ch) {
            ESP_LOGI(TAG, "  I%u=%.2f A  P%u=%.1f W  PF%u=%.2f",
                     ch, (double)s.current[ch],
                     ch, (double)s.power[ch],
                     ch, (double)s.power_factor[ch]);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

**What happens on the bus:** on a UI3, `rbamp_read_all()` performs ~53
single-byte transactions (13 float values × 4 bytes + 1 byte for
frequency). At 50 kHz with retry headroom, that is on the order of
25–30 ms. If you don't need all the values, call the per-property
functions (`rbamp_read_voltage()`, `rbamp_read_current(ch)`) — fewer
bytes on the bus.

---

## Scenario 2 — Wh meter on a 16×2 LCD

**Goal:** a Wh counter, refreshed once per minute, on a 16×2 character
LCD with a PCF8574 I²C expander (HD44780-compatible). The LCD sits on
the same I²C bus as the rbAmp. The component's key mechanism —
`rbamp_read_period_snapshot()` — encapsulates
latch + settle + valid-check + read + Wh-tick in a single call.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rbamp.h"

/* Minimal LCD shim — inlined in the example, no third-party drivers.
 * The full lcd_print/lcd_clear functions are in examples/lcd_period/main/main.c. */

void app_main(void) {
    /* ...bus_cfg + rbamp + lcd init... */
    rbamp_handle_t dev = NULL;

    uint32_t ok_count = 0, bad_count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK
                || !snap.valid) {
            bad_count++;
            continue;
        }
        ok_count++;

        char line1[17], line2[17];
        snprintf(line1, sizeof(line1), "P=%6.1f W      ", (double)snap.avg_p[0]);
        snprintf(line2, sizeof(line2), "%8.4f Wh ok%lu", rbamp_energy_wh(dev, 0),
                 (unsigned long)ok_count);
        lcd_clear();
        lcd_print(0, 0, line1);
        lcd_print(0, 1, line2);
    }
}
```

The full IDF project is in [`examples/lcd_period/`](../examples/lcd_period/).

**Handling stale snapshots.** `rbamp_read_period_snapshot()` returns
`ESP_ERR_INVALID_RESPONSE` if the module hasn't finished preparing a new
snapshot by the time it is read. The component **still records** its own
timestamp (`esp_timer_get_time()`) internally — so the next successful
snapshot won't be double-counted against the interval.

---

## Scenario 3 — Monitoring 3 modules on one bus

**Goal:** poll 3 modules at addresses 0x50 / 0x51 / 0x52 from a single
master. Each module must have a unique address (addresses are assigned
during factory/integrator installation — see
[09 · API Reference](09_api_reference.md) for the address-change methods,
with their WARNING notices).

> **Canonical pattern for v1 firmware** — a sequential
> `rbamp_latch_period()` on each device + one shared 50 ms settle +
> a per-device `rbamp_read_period_snapshot(skip_latch=true)`.
> Inter-device skew at 100 kHz: ~1 ms per device, < 0.2 % of the
> 60-second period.
>
> The `rbamp_broadcast_latch(bus, timeout_ms)` function is reserved
> for v2 firmware (General-Call is disabled in the I²C peripheral of
> the v1 module) — it returns `ESP_ERR_NOT_SUPPORTED`. See
> [09 · API Reference](09_api_reference.md) for details.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rbamp.h"

#define N_MODULES 3
static const char *TAG = "multi_module";
static const uint8_t ADDRS[N_MODULES] = { 0x50, 0x51, 0x52 };

void app_main(void) {
    /* ...bus_cfg setup as in the Quickstart... */
    i2c_master_bus_handle_t bus = NULL;
    rbamp_handle_t modules[N_MODULES] = { NULL };

    /* Create a handle for each module */
    for (int i = 0; i < N_MODULES; ++i) {
        if (rbamp_new(bus, ADDRS[i], &modules[i]) != ESP_OK ||
            rbamp_begin(modules[i]) != ESP_OK) {
            ESP_LOGE(TAG, "module 0x%02X init failed", ADDRS[i]);
            modules[i] = NULL;
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        /* Phase 1: sequential LATCH on each device, measure skew */
        const int64_t sync_start_us = esp_timer_get_time();
        for (int i = 0; i < N_MODULES; ++i) {
            if (modules[i]) rbamp_latch_period(modules[i]);
        }
        const uint32_t sync_us = (uint32_t)(esp_timer_get_time() - sync_start_us);

        /* Phase 2: one shared settle for all 3 modules */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Phase 3: read the snapshots with skip_latch=true */
        ESP_LOGI(TAG, "sync_us=%u", (unsigned)sync_us);
        for (int i = 0; i < N_MODULES; ++i) {
            if (!modules[i]) continue;
            rbamp_period_snapshot_t snap;
            if (rbamp_read_period_snapshot(modules[i], &snap, 0,
                                            /*skip_latch=*/true) != ESP_OK) {
                continue;
            }
            ESP_LOGI(TAG, "mod%d 0x%02X  P=%.0f W  Wh=%.3f",
                     i, ADDRS[i],
                     (double)snap.avg_p[0],
                     rbamp_energy_wh(modules[i], 0));
        }
    }
}
```

The full project (with a bus-scan probe + error reporting) is in
[`examples/multi_module/`](../examples/multi_module/).

---

## Scenario 4 — UI3 + MQTT per channel

**Goal:** a UI3 module with 3 CT clamps on three independent lines.
Per-channel Wh counters, published to MQTT once per minute. It shows
that `rbamp_energy_wh(dev, ch)` works independently on each channel —
you don't need a manual `total_wh[3]` array on the master side.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "rbamp.h"

static const char *TAG = "ui3_mqtt";
static const char *CH_NAMES[3] = { "main", "heatpump", "lights" };
static esp_mqtt_client_handle_t mqtt;

static void publish_channel(const char *name, float avg_p, double e_wh) {
    char topic[64], payload[96];
    snprintf(topic,   sizeof(topic),   "rbamp/%s/state", name);
    snprintf(payload, sizeof(payload),
             "{\"power\":%.1f,\"energy\":%.4f}", (double)avg_p, e_wh);
    esp_mqtt_client_publish(mqtt, topic, payload, 0, /*qos*/1, /*retain*/1);
}

void app_main(void) {
    /* ...wifi STA + mqtt_client init... */
    /* ...bus_cfg + rbamp_new + rbamp_begin (50 kHz via Kconfig)... */
    rbamp_handle_t dev = NULL;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK ||
            !snap.valid) {
            continue;
        }

        for (uint8_t ch = 0; ch < rbamp_channels(dev); ++ch) {
            publish_channel(CH_NAMES[ch], snap.avg_p[ch],
                            rbamp_energy_wh(dev, ch));
        }
    }
}
```

The full project is in [`examples/mqtt_publisher/`](../examples/mqtt_publisher/).
For HA MQTT auto-discovery built on this pattern, see
[07 · DIY Integrations](07_diy_integrations.md) +
[`examples/ha_discovery/`](../examples/ha_discovery/).

---

## Scenario 5 — Bidirectional metering on the master side

**Goal:** split signed instantaneous power into gross-consume and
gross-export. Use this pattern on the BASIC tier, where the **firmware**
clips negative values in `snap.avg_p[ch]` (the period-averaged power) —
sample `rbamp_read_power(dev, 0, &p)` (instantaneous power, **signed on
all tiers**) at 5 Hz on the master side and bucket it yourself.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rbamp.h"

static const char *TAG = "bidir";

void bidir_task(void *arg) {
    rbamp_handle_t dev = arg;
    double consume_wh = 0.0, export_wh = 0.0;
    int64_t t_prev_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));   /* 5 Hz, matches the RT cadence */
        const int64_t t_now_us = esp_timer_get_time();
        const double  dt_s     = (double)(t_now_us - t_prev_us) / 1e6;
        t_prev_us = t_now_us;

        float p;
        if (rbamp_read_power(dev, 0, &p) != ESP_OK) continue;

        const double dwh = (double)p * dt_s / 3600.0;
        if (p >= 0.0f) consume_wh += dwh;
        else           export_wh  += -dwh;

        /* Print once per second */
        static int n = 0;
        if (++n % 5 == 0) {
            ESP_LOGI(TAG, "P=%.1f W   cons=%.4f Wh  exp=%.4f Wh  net=%.4f Wh",
                     (double)p, consume_wh, export_wh,
                     consume_wh - export_wh);
        }
    }
}
```

This is a composite scenario with no dedicated IDF project. On the
future STANDARD / PRO firmware tiers (planned for v1.3+), the period
accumulator `rbamp_energy_wh(dev, 0)` will already return a signed net
balance — this master-side split is only needed for separate
gross-consume / gross-export reporting.

---

## Scenario 6 — Whole-home energy balance

**Goal:** 3 modules — mains (bidirectional), solar (generation only),
loads (UI3 for per-appliance metering). A combined dashboard is
published once per minute.

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "rbamp.h"

static rbamp_handle_t mains_dev, solar_dev, loads_dev;
static esp_mqtt_client_handle_t mqtt;

void app_main(void) {
    /* ...wifi STA + mqtt init... */
    /* ...bus_cfg + per-module rbamp_new(bus, 0x50/0x51/0x52, ...) +
     *    rbamp_begin() on each... */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        /* Sequential LATCH + shared settle (the pattern from Scenario 3) */
        rbamp_latch_period(mains_dev);
        rbamp_latch_period(solar_dev);
        rbamp_latch_period(loads_dev);
        vTaskDelay(pdMS_TO_TICKS(50));

        rbamp_period_snapshot_t sm, ss, sl;
        if (rbamp_read_period_snapshot(mains_dev, &sm, 0, true) != ESP_OK || !sm.valid ||
            rbamp_read_period_snapshot(solar_dev, &ss, 0, true) != ESP_OK || !ss.valid ||
            rbamp_read_period_snapshot(loads_dev, &sl, 0, true) != ESP_OK || !sl.valid) {
            continue;
        }

        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"mains\":%.3f,\"solar\":%.3f,"
            "\"hp\":%.3f,\"ac\":%.3f,\"ev\":%.3f}",
            rbamp_energy_wh(mains_dev, 0),
            rbamp_energy_wh(solar_dev, 0),
            rbamp_energy_wh(loads_dev, 0),
            rbamp_energy_wh(loads_dev, 1),
            rbamp_energy_wh(loads_dev, 2));
        esp_mqtt_client_publish(mqtt, "home/energy/balance",
                                payload, 0, 1, 1);
    }
}
```

For an explicit gross-consume / gross-export split on the mains,
combine this with the pattern from Scenario 5 (a separate 5 Hz task on
`rbamp_read_power(mains_dev, 0, ...)`).

This is a composite scenario with no dedicated IDF project — assemble it
from [`examples/mqtt_publisher/`](../examples/mqtt_publisher/) +
[`examples/multi_module/`](../examples/multi_module/).

---

## Scenario 7 — Event detection (EMA)

**Goal:** on every 200 ms RT window, compare instantaneous power against
an exponential moving average. Log significant deviations — loads such
as a microwave, kettle, or hair dryer "appear" in the log as a
turn-on / turn-off event.

```c
#include <math.h>                /* fabsf */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rbamp.h"

#define EMA_ALPHA            0.05f
#define EVENT_THRESHOLD_W    200.0f

static const char *TAG = "event";

void event_task(void *arg) {
    rbamp_handle_t dev = arg;
    float p_ema = 0.0f;

    /* seed the EMA so the first reading isn't a false event */
    if (rbamp_read_power(dev, 0, &p_ema) != ESP_OK) p_ema = 0.0f;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        float p;
        if (rbamp_read_power(dev, 0, &p) != ESP_OK) continue;

        const float delta = p - p_ema;
        p_ema = (1.0f - EMA_ALPHA) * p_ema + EMA_ALPHA * p;

        if (fabsf(delta) > EVENT_THRESHOLD_W) {
            ESP_LOGI(TAG, "%s  delta=%.0f W   P=%.0f W   EMA=%.0f W",
                     (delta > 0) ? "TURN_ON" : "TURN_OFF",
                     (double)delta, (double)p, (double)p_ema);
        }
    }
}
```

A composite scenario — no dedicated IDF project. Combine it with the
MQTT publishing from Scenario 4 for HA-side automations, or write the
events to SPIFFS using the pattern from Scenario 8 for an offline log.

---

## Scenario 8 — Local CSV logger on SPIFFS

**Goal:** write a period snapshot to a CSV file on SPIFFS once per
minute. Useful for standalone deployments without WiFi / MQTT, or as a
buffer for deferred cloud sync.

> **Requires a custom partition table** with the entry
> `storage,data,spiffs,,128K,` in `partitions.csv`. A sample is in
> `examples/spiffs_logger/partitions.csv`.

```c
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rbamp.h"

#define LOG_PATH       "/storage/log.csv"
#define ROTATE_BYTES   (64 * 1024)
static const char *TAG = "spiffs_logger";

static void rotate_if_full(void) {
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size >= ROTATE_BYTES) {
        rename(LOG_PATH, "/storage/log_prev.csv");
    }
}

void app_main(void) {
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_cfg));

    /* ...bus_cfg + rbamp_new + rbamp_begin... */
    rbamp_handle_t dev = NULL;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK ||
            !snap.valid) continue;

        rotate_if_full();
        FILE *f = fopen(LOG_PATH, "a");
        if (!f) {
            ESP_LOGE(TAG, "fopen failed");
            continue;
        }
        fprintf(f, "%lld,%.3f,%.4f,%u\n",
                (long long)(esp_timer_get_time() / 1000),
                (double)snap.avg_p[0],
                rbamp_energy_wh(dev, 0),
                (unsigned)snap.master_dt_ms);
        fclose(f);
    }
}
```

The full project is in [`examples/spiffs_logger/`](../examples/spiffs_logger/).
The snippet above is a teaching simplification with a 4-column CSV row.
The example itself writes a **9-column** CSV row with per-channel `avg_p`
and Wh for all 3 channels — more useful for production scenarios on UI3
modules.

For deferred cloud sync (log locally, send once per hour), see the
"Hybrid: local storage + sync" section in
[08 · Cloud Integrations](08_cloud_integrations.md).

---

## Scenario 9 — Battery logger with deep-sleep

**Goal:** the ESP32 wakes up once every 10 minutes, performs a single
period latch, publishes over WiFi+MQTT, and goes back into deep-sleep.
Energy is persisted in RTC memory between sleep cycles. The component's
accumulator is **disabled** — the master itself owns the Wh persistence.

> ⚠ **Important note about deep-sleep and v1.0/v1.1 firmware.** On the
> current firmware, `rbamp_begin()` always issues a CMD_LATCH_PERIOD
> primer that resets the firmware's period accumulator. This means that
> after a deep-sleep wake you cannot simply call
> `rbamp_read_period_snapshot()` with its defaults — that call would
> re-latch and return near-zero data, ~50 ms after the primer. The
> correct pattern below uses `skip_latch=true` to read the data
> accumulated during the sleep interval, and the **master's own RTC
> timer** for the interval between wakeups (the `snap.master_dt_ms`
> field reflects only the time within the current wake cycle, not
> wake-to-wake).

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "rbamp.h"

#define WAKE_INTERVAL_US (10ULL * 60ULL * 1000000ULL)
#define RTC_MAGIC        0xCAFEFEEDu

RTC_DATA_ATTR static uint32_t rtc_magic        = 0;
RTC_DATA_ATTR static double   rtc_total_wh     = 0.0;
RTC_DATA_ATTR static bool     rtc_primer_done  = false;
RTC_DATA_ATTR static uint64_t rtc_last_wake_us = 0;

static const char *TAG = "deep_sleep";

void app_main(void) {
    /* ...bus_cfg setup... */
    i2c_master_bus_handle_t bus = NULL;
    rbamp_handle_t dev = NULL;

    if (rtc_magic != RTC_MAGIC) {
        /* Cold start — initialize RTC RAM */
        rtc_magic = RTC_MAGIC;
        rtc_total_wh = 0.0;
        rtc_primer_done = false;
        rtc_last_wake_us = 0;
    }

    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));
    ESP_ERROR_CHECK(rbamp_begin(dev));          /* issues the primer LATCH */
    rbamp_energy_disable(dev);                  /* the master owns Wh itself */

    if (!rtc_primer_done) {
        /* First wake: the primer just latched the "dirty" data from
         * before the wake. Record the time and go straight back to sleep —
         * the next wake in 10 min will get a full accumulator for the interval. */
        rtc_last_wake_us = esp_timer_get_time();
        rtc_primer_done = true;
        esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
        esp_deep_sleep_start();
    }

    /* skip_latch=true reads what the rbamp_begin() primer latched —
     * i.e. the accumulator for the 10-minute sleep interval that just passed. */
    rbamp_period_snapshot_t snap;
    esp_err_t snap_err = rbamp_read_period_snapshot(dev, &snap, /*settle_ms=*/0,
                                                     /*skip_latch=*/true);
    if (snap_err != ESP_OK || !snap.valid) {
        ESP_LOGW(TAG, "snapshot fail: %s (valid=%d)",
                 rbamp_err_to_str(snap_err), snap.valid);
        esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
        esp_deep_sleep_start();
    }

    /* `snap.master_dt_ms` here is the time within the current wake (after
     * the primer); for energy integration we use `esp_timer_get_time()`
     * against `rtc_last_wake_us`. On ESP-IDF v5.x the value returned by
     * `esp_timer_get_time()` **survives deep sleep** — the timer is
     * re-derived from the RTC slow clock on wake. So the delta gives the
     * real wake-to-wake interval in microseconds. */
    const uint64_t now_us = esp_timer_get_time();
    const double   dt_s   = (double)(now_us - rtc_last_wake_us) / 1e6;
    rtc_last_wake_us = now_us;

    rtc_total_wh += (double)snap.avg_p[0] * dt_s / 3600.0;
    ESP_LOGI(TAG, "wake: P=%.1f W  Wh=%.4f  dt_s=%.1f",
             (double)snap.avg_p[0], rtc_total_wh, dt_s);
    publish_via_wifi(snap.avg_p[0], rtc_total_wh, dt_s);

    esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
    esp_deep_sleep_start();
}
```

The full project (with WiFi setup, MQTT publish, and RTC magic-marker
guards) is in [`examples/deep_sleep_logger/`](../examples/deep_sleep_logger/).

> 🛣 **Roadmap.** A future component version (v1.2+) is expected to add
> `rbamp_warm_begin()` — a lightweight init variant without the
> CMD_LATCH_PERIOD primer, which will make it easier to read
> `rbamp_read_period_snapshot()` with its defaults after a deep-sleep
> wake. Until then, the pattern above (skip_latch=true + the master's
> RTC timer) is canonical.

**Energy budget** (ESP32-WROOM, 10-minute wake interval):

- Active cycle duration: ~3 s (WiFi + MQTT + I²C)
- Active current: ~80 mA average → ~0.07 mAh per wake
- Sleep current: ~10 µA (RTC + retained domains)
- ~10 mAh per day → a 2000 mAh Li-ion battery lasts ~6 months

---

## Scenario summary table

| # | LOC | I²C bus | Period? | DRDY? | MQTT? | Persistence |
|:---:|:---:|:---:|:---:|:---:|:---:|---|
| 1 | ~40 | dedicated | no | no | no | none |
| 2 | ~60 | shared with LCD | yes | no | no | RAM |
| 3 | ~80 | shared, 3 modules | yes | no | no | RAM |
| 4 | ~70 | dedicated | yes | no | yes | retained MQTT |
| 5 | ~50 | dedicated | no (RT) | no | no | RAM (master-owned) |
| 6 | ~80 | shared, 3 modules | yes | no | yes | retained MQTT |
| 7 | ~40 | dedicated | no (RT) | no | no | RAM |
| 8 | ~70 | dedicated | yes | no | no | SPIFFS |
| 9 | ~80 | dedicated | yes | no | yes | RTC memory + MQTT |

## What's next

- [07 · DIY Integrations](07_diy_integrations.md) — Home Assistant /
  Node-RED / OpenHAB built on these scenarios (including
  `examples/ha_discovery/`)
- [08 · Cloud Integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB Cloud
- [09 · API Reference](09_api_reference.md) — every public function
  documented
- [10 · Troubleshooting](10_troubleshooting.md) — when scenarios
  misbehave on your bench


---

[← Quickstart](05_quickstart.md) | [Contents](README.md) | [DIY Integrations →](07_diy_integrations.md)
