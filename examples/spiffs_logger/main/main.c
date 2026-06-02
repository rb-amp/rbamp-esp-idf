/**
 * @file    main.c
 * @brief   Example 6 — append one CSV row per minute to a SPIFFS log file.
 *
 * Demonstrates persistent local logging without external storage. The CSV
 * file is rotated when it exceeds 64 KiB to keep SPIFFS happy.
 *
 * Partition table requirement: an entry of type `data`, subtype `spiffs`
 * named `storage` of at least 128 KiB. The default partition tables shipped
 * with ESP-IDF do NOT include one — add the line:
 *     storage,  data, spiffs,  ,        128K,
 * to your custom partitions.csv and select it in @c menuconfig.
 *
 * Build:
 *   idf.py build flash monitor
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

#include "rbamp.h"

static const char *TAG = "spiffs_log";

#define SDA_GPIO    GPIO_NUM_21
#define SCL_GPIO    GPIO_NUM_22
#define RBAMP_ADDR  0x50

#define LOG_PATH    "/storage/rbamp.csv"
#define ROT_BYTES   (64 * 1024)

static void rotate_if_needed(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) != 0) return;
    if (st.st_size < ROT_BYTES) return;
    /* Simple rotation: rename to .1 (overwriting any previous .1). */
    rename(LOG_PATH, LOG_PATH ".1");
    ESP_LOGI(TAG, "rotated CSV at %ld bytes", (long)st.st_size);
}

void app_main(void)
{
    /* Mount SPIFFS. */
    const esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path        = "/storage",
        .partition_label  = "storage",
        .max_files        = 4,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_cfg));

    /* I2C + rbAmp. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = SDA_GPIO, .scl_io_num = SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    rbamp_handle_t dev;
    ESP_ERROR_CHECK(rbamp_new(bus, RBAMP_ADDR, &dev));
    ESP_ERROR_CHECK(rbamp_begin(dev));

    /* Write CSV header if file is fresh. */
    struct stat st;
    bool fresh = (stat(LOG_PATH, &st) != 0) || st.st_size == 0;
    if (fresh) {
        FILE *f = fopen(LOG_PATH, "w");
        if (f) {
            fprintf(f, "ms,master_dt_ms,avg_p0,avg_p1,avg_p2,max_p,wh0,wh1,wh2\n");
            fclose(f);
        }
    }

    while (true) {
        rotate_if_needed();

        rbamp_period_snapshot_t snap;
        esp_err_t err = rbamp_read_period_snapshot_simple(dev, &snap);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "snapshot: %s", rbamp_err_to_str(err));
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        FILE *f = fopen(LOG_PATH, "a");
        if (f) {
            fprintf(f, "%lld,%u,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
                    esp_timer_get_time() / 1000,
                    (unsigned)snap.master_dt_ms,
                    (double)snap.avg_p[0], (double)snap.avg_p[1], (double)snap.avg_p[2],
                    (double)snap.max_p,
                    rbamp_energy_wh(dev, 0),
                    rbamp_energy_wh(dev, 1),
                    rbamp_energy_wh(dev, 2));
            fclose(f);
            ESP_LOGI(TAG, "appended row, Wh0=%.4f", rbamp_energy_wh(dev, 0));
        } else {
            ESP_LOGE(TAG, "fopen failed");
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
