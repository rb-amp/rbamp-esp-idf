/**
 * @file    main.c
 * @brief   Example 9 — STD-line meter: the quantities only the STD line has.
 *
 * The same rbamp API serves both product lines; rbamp_begin() detects which
 * one is on the bus. This example shows what the STD line adds on top of the
 * U / I / P / PF of quick_read:
 *   - one-burst channel blocks with reactive / apparent power, THD and the
 *     displacement power factor (harmonics switched on for the run),
 *   - a latch bank of your own: a master-timed period with average power,
 *     min/max voltage and import/export energy in Wh,
 *   - module health (sensor frames must keep counting).
 * On a BASIC-line module it prints a note and falls back to quick_read
 * behaviour — nothing fails, the STD calls just report "not supported".
 *
 * Wiring (defaults below — adjust @c SDA_GPIO / @c SCL_GPIO for your board):
 *   rbAmp SDA -> GPIO21
 *   rbAmp SCL -> GPIO22
 *   rbAmp GND -> GND
 *
 * Build:
 *   idf.py set-target esp32
 *   idf.py build flash monitor
 */
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "rbamp.h"

static const char *TAG = "std_meter";

#define SDA_GPIO    GPIO_NUM_21
#define SCL_GPIO    GPIO_NUM_22
#define RBAMP_ADDR  0x50

#define POLL_MS     1000   /* live readings */
#define PERIOD_MS   60000  /* energy period on latch bank 1 (bank 0 is the library's) */
#define ENERGY_BANK 1

void app_main(void)
{
    /* 1. I2C master bus (new driver). */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = SDA_GPIO,
        .scl_io_num        = SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* 2. Handle + discovery. The line is detected here. */
    rbamp_handle_t dev = NULL;
    ESP_ERROR_CHECK(rbamp_new(bus, RBAMP_ADDR, &dev));
    esp_err_t err = rbamp_begin(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin failed: %s", rbamp_err_to_str(err));
        return;
    }
    const bool std = (rbamp_line(dev) == RBAMP_LINE_STD);
    rbamp_std_period_source_t psrc = RBAMP_STD_PERIOD_NONE;
    ESP_LOGI(TAG, "rbAmp @ 0x%02X  line=%s  fw=0x%02X  channels=%u  voltage_hw=%s",
             rbamp_address(dev), std ? "STD" : "BASIC", rbamp_firmware_version(dev),
             rbamp_channels(dev), rbamp_has_voltage_hw(dev) ? "yes" : "no");

    if (!std) {
        ESP_LOGW(TAG, "BASIC-line module: reactive/apparent power, THD and latch banks "
                      "are not available — see the quick_read example for this module");
    } else {
        rbamp_std_info_t info;
        rbamp_std_info(dev, &info);
        psrc = info.period_source;
        ESP_LOGI(TAG, "STD map 0x%02X  V=%u I=%u  caps=0x%04X", info.map_version,
                 info.n_voltage, info.n_current, info.capabilities);

        /* Harmonics (THD, displacement PF) are off by default on the regular
         * tier to save module CPU; switch them on for this run. The setting is
         * RAM-only: it stays on until the module resets or you call
         * rbamp_std_set_harmonics(dev, false) — this example never exits. */
        err = rbamp_std_set_harmonics(dev, true);
        if (err != ESP_OK) ESP_LOGW(TAG, "harmonics not enabled: %s", rbamp_err_to_str(err));

        /* Period energy depends on what the module offers: modules with latch
         * banks get a bank of our own next to the library's bank 0 (primer
         * latch so the first period starts now); modules that keep persisted
         * energy counters instead need nothing armed — the library's period
         * snapshot reads the counter difference. */
        if (psrc == RBAMP_STD_PERIOD_LATCH) {
            ESP_ERROR_CHECK(rbamp_std_arm_banks(dev, (1u << 0) | (1u << ENERGY_BANK)));
            ESP_ERROR_CHECK(rbamp_std_latch(dev, ENERGY_BANK));
        } else if (psrc == RBAMP_STD_PERIOD_COUNTERS) {
            rbamp_std_energy_t lt;
            if (rbamp_std_read_energy(dev, &lt) == ESP_OK) {
                ESP_LOGI(TAG, "life-time: import %lld Wh  export %lld Wh  net %lld Wh  (gen %" PRIu32 ", up %" PRIu32 " s)",
                         (long long)lt.act_import_wh, (long long)lt.act_export_wh,
                         (long long)lt.act_net_wh, lt.generation, lt.uptime_s);
            }
        } else {
            ESP_LOGW(TAG, "module offers no period energy path");
        }
    }
    int64_t period_start_us = esp_timer_get_time();

    /* 3. Poll loop. */
    while (true) {
        if (!std) {
            rbamp_snapshot_t s;
            if (rbamp_read_all(dev, &s) == ESP_OK) {
                ESP_LOGI(TAG, "U=%6.1fV f=%4.1fHz I0=%5.2fA P0=%7.1fW PF0=%+.2f",
                         (double)s.voltage, (double)s.frequency, (double)s.current[0],
                         (double)s.power[0], (double)s.power_factor[0]);
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        /* Voltage block(s) + every current channel in one burst each. */
        rbamp_std_voltage_t v;
        if (rbamp_std_read_voltage(dev, 0, &v) == ESP_OK) {
            float f = 0;
            (void)rbamp_read_frequency(dev, &f);
            ESP_LOGI(TAG, "U=%6.1fV (peak %6.1fV, THD %4.1f%%)  f=%5.2fHz",
                     (double)v.u_rms, (double)v.u_peak, (double)v.thd_u, (double)f);
        }
        for (uint8_t ch = 0; ch < rbamp_channels(dev); ++ch) {
            rbamp_std_channel_t c;
            if (rbamp_std_read_channel(dev, ch, &c) != ESP_OK) continue;
            ESP_LOGI(TAG, "ch%u: I=%6.3fA  P=%7.1fW  Q=%7.1fvar  S=%7.1fVA  PF=%+.3f  "
                          "PF1=%+.3f  THD=%4.1f%%%s",
                     ch, (double)c.i_rms, (double)c.p, (double)c.q, (double)c.s, (double)c.pf,
                     (double)c.pf1, (double)c.thd_i, c.implausible ? "  (implausible field)" : "");
        }

        /* Health: the frame counter must move between polls. */
        rbamp_std_health_t h;
        if (rbamp_std_read_health(dev, &h) == ESP_OK && !(h.status & RBAMP_STD_ST_SENSOR_OK)) {
            ESP_LOGW(TAG, "sensor not OK (status=0x%02X)", h.status);
        }

        /* Energy period: latch our bank every PERIOD_MS, read it after the
         * module's window boundary, convert with the master-measured dt. */
        const int64_t now_us = esp_timer_get_time();
        if (psrc == RBAMP_STD_PERIOD_COUNTERS && now_us - period_start_us >= (int64_t)PERIOD_MS * 1000) {
            /* Counter modules: Wh resolution before map 0x36 (mWh from 0x36) — keep the period long. */
            period_start_us = now_us;
            rbamp_period_snapshot_t ps;
            if (rbamp_read_period_snapshot(dev, &ps, 0, false) == ESP_OK) {
                ESP_LOGI(TAG, "period %" PRIu32 " ms: avgP=%7.1fW  net %.0f Wh  total so far %.0f Wh%s",
                         ps.master_dt_ms, (double)ps.avg_p_total, ps.wh_net,
                         rbamp_energy_wh(dev, RBAMP_ENERGY_CH_TOTAL),
                         ps.module_restarted ? "  (module restarted in this period)" : "");
            }
        } else if (psrc == RBAMP_STD_PERIOD_LATCH && now_us - period_start_us >= (int64_t)PERIOD_MS * 1000) {
            const uint32_t dt_ms = (uint32_t)((now_us - period_start_us) / 1000);
            if (rbamp_std_latch(dev, ENERGY_BANK) == ESP_OK) {
                period_start_us = now_us;
                vTaskDelay(pdMS_TO_TICKS(RBAMP_STD_SETTLE_MS_LATCH_PERIOD));
                rbamp_std_latch_t lt;
                double wh_in = 0, wh_out = 0;
                if (rbamp_std_read_latch(dev, ENERGY_BANK, &lt) == ESP_OK
                        && rbamp_std_latch_wh(&lt, dt_ms, RBAMP_STD_TOTAL, &wh_in, &wh_out) == ESP_OK) {
                    ESP_LOGI(TAG, "period %" PRIu32 " ms: avgP=%7.1fW  maxP=%7.1fW  "
                                  "U %5.1f..%5.1fV  import %.4f Wh  export %.4f Wh",
                             dt_ms, (double)lt.p_avg, (double)lt.max_p, (double)lt.min_u,
                             (double)lt.max_u, wh_in, wh_out);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
