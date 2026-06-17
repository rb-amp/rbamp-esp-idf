/**
 * @file    main.c
 * @brief   Example 3 — three rbAmp modules on one bus, period-synced via
 *          PER-DEVICE SEQUENTIAL LATCH (truth-doc §18.5 Strategy 1).
 *
 * Sync strategy:
 *   This example uses per-device sequential @c rbamp_latch_period in a tight
 *   loop, then one shared settle, then per-device
 *   @c rbamp_read_period_snapshot with @c skip_latch=true. Skew at 100 kHz
 *   is roughly 200-400 µs per I²C transaction (write 2 bytes + STOP) — far
 *   below the 200 ms RT window, so energy integration error is < 0.2 % per
 *   period. For billing-grade sync (≥ 8 modules) use Strategy 2 — opt each
 *   module in to GC via @c REG_FLEET_CONFIG bit 0, then call
 *   ::rbamp_broadcast_latch (truth-doc §5).
 *
 * Demonstrates:
 *   - Multiple rbamp_handle_t sharing the same i2c_master bus.
 *   - Per-device sequential LATCH with shared settle.
 *   - rbamp_read_period_snapshot(skip_latch=true) — read without re-latching
 *     after the shared LATCH burst.
 *
 * Each module must have a UNIQUE I²C address; see
 * ::rbamp_prepare_address_change / ::rbamp_commit_address_change for the
 * reassignment flow (production-OK two-phase commit on v1.3).
 *
 * Example output:
 *   sync_us=312  0x50 ok dt=60012 P0=  234W  Wh0=12.345
 *                0x51 ok dt=60012 P0=  108W  Wh0= 4.072
 *                0x52 ok dt=60012 P0=   75W  Wh0= 1.954
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "rbamp.h"

static const char *TAG = "multi_module";

#define SDA_GPIO    GPIO_NUM_21
#define SCL_GPIO    GPIO_NUM_22

static const uint8_t kAddresses[] = { 0x50, 0x51, 0x52 };
#define N_MODULES (sizeof(kAddresses) / sizeof(kAddresses[0]))

void app_main(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = SDA_GPIO,
        .scl_io_num        = SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    rbamp_handle_t modules[N_MODULES] = { 0 };
    size_t n_alive = 0;
    for (size_t i = 0; i < N_MODULES; ++i) {
        if (rbamp_new(bus, kAddresses[i], &modules[i]) != ESP_OK) continue;
        if (rbamp_begin(modules[i]) != ESP_OK) {
            ESP_LOGW(TAG, "module 0x%02X did not respond", kAddresses[i]);
            rbamp_del(modules[i]);
            modules[i] = NULL;
            continue;
        }
        ESP_LOGI(TAG, "module 0x%02X channels=%u", kAddresses[i],
                 rbamp_channels(modules[i]));
        n_alive++;
    }
    if (n_alive == 0) {
        ESP_LOGE(TAG, "no modules found — check wiring/addresses");
        return;
    }

    while (true) {
        /* 1. Sequential LATCH — measure skew so caller knows how much error
         *    creeps into per-channel dt. For ≥8 modules switch to
         *    rbamp_broadcast_latch() (GC opt-in required, truth-doc §5). */
        const int64_t sync_start_us = esp_timer_get_time();
        for (size_t i = 0; i < N_MODULES; ++i) {
            if (modules[i]) rbamp_latch_period(modules[i]);
        }
        const uint32_t sync_us = (uint32_t)(esp_timer_get_time() - sync_start_us);

        /* 2. Single shared settle for all modules. */
        vTaskDelay(pdMS_TO_TICKS(50));

        /* 3. Read each module's snapshot — skip the latch, already happened. */
        ESP_LOGI(TAG, "sync_us=%u", (unsigned)sync_us);
        for (size_t i = 0; i < N_MODULES; ++i) {
            if (!modules[i]) continue;
            rbamp_period_snapshot_t snap;
            esp_err_t err = rbamp_read_period_snapshot(modules[i], &snap, 0, true);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "0x%02X read: %s",
                         rbamp_address(modules[i]), rbamp_err_to_str(err));
                continue;
            }
            char buf[128];
            int n = snprintf(buf, sizeof(buf),
                             "0x%02X ok dt=%u",
                             rbamp_address(modules[i]),
                             (unsigned)snap.master_dt_ms);
            for (uint8_t ch = 0; ch < rbamp_channels(modules[i]); ++ch) {
                n += snprintf(buf + n, sizeof(buf) - n,
                              " P%u=%6.0fW", ch, (double)snap.avg_p[ch]);
            }
            for (uint8_t ch = 0; ch < rbamp_channels(modules[i]); ++ch) {
                n += snprintf(buf + n, sizeof(buf) - n,
                              " Wh%u=%.3f", ch, rbamp_energy_wh(modules[i], ch));
            }
            ESP_LOGI(TAG, "%s", buf);
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
