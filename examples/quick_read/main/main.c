/**
 * @file    main.c
 * @brief   Example 1 — ESP-IDF smoke test for the rbAmp client.
 *
 * Reads U / I / P / PF / frequency once per second, prints to ESP_LOG.
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "rbamp.h"

static const char *TAG = "quick_read";

#define SDA_GPIO   GPIO_NUM_21
#define SCL_GPIO   GPIO_NUM_22
#define RBAMP_ADDR 0x50

void app_main(void)
{
    /* 1. Bring up the I2C master bus (new driver). */
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

    /* 2. Create the rbAmp handle and probe. */
    rbamp_handle_t dev = NULL;
    ESP_ERROR_CHECK(rbamp_new(bus, RBAMP_ADDR, &dev));
    esp_err_t err = rbamp_begin(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin failed: %s", rbamp_err_to_str(err));
        return;
    }
    ESP_LOGI(TAG, "rbAmp @ 0x%02X  fw=0x%02X  channels=%u  voltage_hw=%s",
             rbamp_address(dev), rbamp_firmware_version(dev),
             rbamp_channels(dev),
             rbamp_has_voltage_hw(dev) ? "yes" : "no");

    /* 3. Poll loop. */
    while (true) {
        rbamp_snapshot_t s;
        if (rbamp_read_all(dev, &s) != ESP_OK) {
            ESP_LOGW(TAG, "read_all failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        char line[160];
        int n = snprintf(line, sizeof(line),
                         "U=%6.1fV  f=%4.1fHz  ",
                         (double)s.voltage, (double)s.frequency);
        for (uint8_t ch = 0; ch < s.channels && n < (int)sizeof(line) - 1; ++ch) {
            n += snprintf(line + n, sizeof(line) - n,
                          "I%u=%5.2fA P%u=%7.1fW PF%u=%+.2f  ",
                          ch, (double)s.current[ch],
                          ch, (double)s.power[ch],
                          ch, (double)s.power_factor[ch]);
        }
        ESP_LOGI(TAG, "%s", line);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* unreachable in this example */
    /* rbamp_del(dev); i2c_del_master_bus(bus); */
}
