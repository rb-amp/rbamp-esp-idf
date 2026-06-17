/**
 * @file    main.c
 * @brief   Example 8 — multi-module fleet: scan → GC fleet-sync → aggregate.
 *
 * Demonstrates the v1.3 fleet manager (rbamp_fleet.h):
 *   1. Scan the bus and bind every rbAmp module (PRODUCT_ID confirmed).
 *   2. Opt the whole fleet into General-Call latch (FLEET_CONFIG + reset).
 *   3. Each cycle: broadcast a ticked GC latch, verify every module accepted
 *      the tick (missed-frame detection via REG_GC_TICK), then aggregate
 *      fleet-wide power + energy.
 *
 * Wiring: N rbAmp modules on one I²C bus, each with a DISTINCT address (use
 * the fleet provisioning flow — connect one virgin module at a time and call
 * rbamp_fleet_assign_address — before running this loop).
 *
 * Example output:
 *   fleet: 3 modules
 *   tick 41: 3/3 in sync   P_total=417.0 W   Wh_total=12.84
 *   tick 42: 2/3 in sync   (0x52 missed: gc_tick=41)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "rbamp_fleet.h"

static const char *TAG = "fleet_sync";

#define SDA_GPIO    GPIO_NUM_21
#define SCL_GPIO    GPIO_NUM_22
#define GC_GROUP    0x00         /* all-call */
#define WINDOW_MS   60000        /* 60 s billing window */

void app_main(void)
{
    /* 1. Bring up the shared bus. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port  = I2C_NUM_0,
        .sda_io_num = SDA_GPIO,
        .scl_io_num = SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* 2. Create the fleet and discover modules. */
    rbamp_fleet_t fleet;
    ESP_ERROR_CHECK(rbamp_fleet_create(bus, &fleet));

    size_t added = 0;
    ESP_ERROR_CHECK(rbamp_fleet_scan(fleet, /*match_product=*/true, &added));
    size_t n = rbamp_fleet_count(fleet);
    ESP_LOGI(TAG, "fleet: %u modules", (unsigned)n);
    if (n == 0) {
        ESP_LOGE(TAG, "no modules found — check wiring / addresses");
        return;
    }

    /* 3. Opt the whole fleet into GC latch (one-time; persisted + reset). */
    size_t gc_ok = 0;
    ESP_ERROR_CHECK(rbamp_fleet_enable_gc_all(fleet, GC_GROUP, &gc_ok));
    ESP_LOGI(TAG, "GC enabled on %u/%u modules", (unsigned)gc_ok, (unsigned)n);

    /* 4. Fleet-synchronous windowing. */
    uint16_t tick = 0;
    rbamp_fleet_sync_t status[RBAMP_FLEET_MAX_MODULES];

    while (true) {
        tick++;

        /* Broadcast a ticked latch to the whole fleet, then settle. */
        ESP_ERROR_CHECK(rbamp_fleet_gclatch(fleet, GC_GROUP, tick, /*settle_ms=*/50));

        /* Verify every module accepted this tick. */
        size_t missed = 0;
        ESP_ERROR_CHECK(rbamp_fleet_check_sync(fleet, tick, status,
                                               RBAMP_FLEET_MAX_MODULES, &missed));

        /* Aggregate fleet-wide power + energy. */
        float p_total = 0.0f;
        double wh_total = 0.0;
        rbamp_fleet_total_power(fleet, &p_total);
        rbamp_fleet_total_energy_wh(fleet, &wh_total);

        ESP_LOGI(TAG, "tick %u: %u/%u in sync   P_total=%.1f W   Wh_total=%.2f",
                 tick, (unsigned)(n - missed), (unsigned)n,
                 (double)p_total, wh_total);

        for (size_t i = 0; i < n; ++i) {
            if (!status[i].in_sync) {
                ESP_LOGW(TAG, "  0x%02X missed: gc_tick=%u%s",
                         status[i].addr, status[i].gc_tick,
                         status[i].reachable ? "" : " (unreachable)");
            }
        }

        /* Poll per-module durable error (EVENT bit3). */
        size_t n_err = 0;
        rbamp_fleet_poll_errors(fleet, NULL, &n_err);
        if (n_err) ESP_LOGW(TAG, "  %u module(s) flagging an error", (unsigned)n_err);

        vTaskDelay(pdMS_TO_TICKS(WINDOW_MS));
    }
}
