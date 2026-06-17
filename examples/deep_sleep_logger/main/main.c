/**
 * @file    main.c
 * @brief   Example 7 — battery-friendly periodic logger using ESP32 deep sleep.
 *
 * Canonical rbAmp deep-sleep pattern (mirrors the Arduino library's
 * 07_DeepSleepLogger example; documented in docs/06_examples.md Sc9):
 *
 *   - RTC magic-marker (RTC_MAGIC) cold-boot vs warm-wake disambiguation.
 *   - First-wake gate: ::rbamp_begin issues a primer LATCH, capture the
 *     wake timestamp, deep-sleep without reading. The next wake gets a clean
 *     interval's worth of accumulated period data.
 *   - Warm wake: ::rbamp_begin's primer just latched the firmware accumulator
 *     covering the previous sleep interval. Read it with @c skip_latch=true
 *     to consume the data BEFORE a default snapshot would re-latch and
 *     discard it.
 *   - dt source: master-side @c esp_timer_get_time delta against the
 *     RTC_DATA_ATTR-persisted previous-wake timestamp. The library's
 *     @c snap.master_dt_ms reflects only the current wake (begin() reset
 *     it), NOT the wake-to-wake interval, so we cannot use it for energy
 *     integration across deep sleep.
 *   - Library accumulator OFF (::rbamp_energy_disable) — master fully owns
 *     Wh persistence in @c rtc_total_wh (RTC slow-RAM, survives deep sleep,
 *     zeroed on power-cycle via the RTC_MAGIC check).
 *
 * Target: ESP32. Power-down current during deep sleep: ~10 µA.
 *
 * For a multi-channel variant: replicate @c rtc_total_wh into a per-channel
 * array (matching Arduino library's `rtc_total_wh[3]`) and loop
 * @c rbamp_channels(dev). The single-channel form below matches the
 * 06_examples.md Sc9 pedagogical snippet.
 *
 * Build:
 *   idf.py build flash monitor
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "rbamp.h"

static const char *TAG = "deep_sleep";

#define SDA_GPIO        GPIO_NUM_21
#define SCL_GPIO        GPIO_NUM_22
#define RBAMP_ADDR      0x50
#define WAKE_INTERVAL_US (10ULL * 60ULL * 1000000ULL)   /* 10 minutes */
#define RTC_MAGIC        0xCAFEFEEDu

/* RTC slow-RAM — survives deep sleep, lost on power-cycle (or magic mismatch). */
RTC_DATA_ATTR static uint32_t rtc_magic        = 0;
RTC_DATA_ATTR static double   rtc_total_wh     = 0.0;
RTC_DATA_ATTR static bool     rtc_primer_done  = false;
RTC_DATA_ATTR static uint64_t rtc_last_wake_us = 0;
RTC_DATA_ATTR static uint32_t boot_count       = 0;

/* Stub for the publish step — replace with WiFi + MQTT (see examples/mqtt_publisher)
 * for a production logger. Kept as ESP_LOGI here to keep the example focused on the
 * deep-sleep correctness pattern. */
static void publish(float avg_p_w, double total_wh, double dt_s)
{
    ESP_LOGI(TAG, "P=%.1f W  Wh=%.4f  dt=%.1f s",
             (double)avg_p_w, total_wh, dt_s);
}

static void sleep_and_exit(void)
{
    ESP_LOGI(TAG, "sleeping %llu s...", WAKE_INTERVAL_US / 1000000ULL);
    esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
    esp_deep_sleep_start();
    /* never returns */
}

void app_main(void)
{
    boot_count++;
    ESP_LOGI(TAG, "boot #%u", (unsigned)boot_count);

    /* Cold-boot detection — RTC magic mismatch means power-cycle or first ever
     * boot. Zero all RTC state so we start from a clean energy total. */
    if (rtc_magic != RTC_MAGIC) {
        rtc_magic        = RTC_MAGIC;
        rtc_total_wh     = 0.0;
        rtc_primer_done  = false;
        rtc_last_wake_us = 0;
        ESP_LOGI(TAG, "RTC magic mismatch — cold boot, state reset");
    }

    /* I2C bus. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = SDA_GPIO, .scl_io_num = SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed; sleeping");
        sleep_and_exit();
    }

    rbamp_handle_t dev;
    if (rbamp_new(bus, RBAMP_ADDR, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "rbamp_new failed");
        sleep_and_exit();
    }
    esp_err_t begin_err = rbamp_begin(dev);
    if (begin_err != ESP_OK) {
        ESP_LOGE(TAG, "rbamp_begin failed: %s", rbamp_err_to_str(begin_err));
        sleep_and_exit();
    }

    /* Library accumulator OFF — master owns Wh persistence via rtc_total_wh. */
    rbamp_energy_disable(dev);

    if (!rtc_primer_done) {
        /* First wake after cold boot: begin()'s primer just latched whatever was
         * accumulated since power-on (a few ms of boot junk). Capture the wake
         * timestamp and sleep — the NEXT wake will see a clean WAKE_INTERVAL_US
         * sleep interval's worth of data. */
        rtc_last_wake_us = esp_timer_get_time();
        rtc_primer_done  = true;
        ESP_LOGI(TAG, "primer wake — seeding rtc_last_wake_us and sleeping");
        sleep_and_exit();
    }

    /* Warm wake: begin()'s primer just latched the firmware accumulator
     * which covers the previous sleep interval. Read it with skip_latch=true
     * so we don't issue ANOTHER latch (which would overwrite this data
     * with the ~50 ms post-primer accumulator). */
    rbamp_period_snapshot_t snap;
    esp_err_t err = rbamp_read_period_snapshot(dev, &snap, /*settle_ms=*/0,
                                                /*skip_latch=*/true);
    if (err != ESP_OK || !snap.valid) {
        ESP_LOGW(TAG, "snapshot fail: %s (valid=%d)", rbamp_err_to_str(err), snap.valid);
        sleep_and_exit();
    }

    /* snap.master_dt_ms is wall-clock time WITHIN the current wake (begin()
     * just reset it). For energy integration we need the actual wake-to-wake
     * interval — use the RTC-RAM timestamp that survived deep sleep. */
    const uint64_t now_us = esp_timer_get_time();
    const double   dt_s   = (double)(now_us - rtc_last_wake_us) / 1e6;
    rtc_last_wake_us = now_us;

    rtc_total_wh += (double)snap.avg_p[0] * dt_s / 3600.0;
    publish(snap.avg_p[0], rtc_total_wh, dt_s);

    sleep_and_exit();
}
