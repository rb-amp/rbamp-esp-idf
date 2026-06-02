/**
 * @file    main.c
 * @brief   Example 2 — 60-second period energy meter, output to a 16x2 I2C LCD.
 *
 * Demonstrates rbamp_read_period_snapshot() + library-owned Wh accumulator.
 * The LCD driver is intentionally a tiny inline shim (PCF8574 + HD44780) so
 * the example has no third-party dependencies — replace with a richer driver
 * for production use.
 *
 * Wiring (shared I2C bus):
 *   SDA -> GPIO21    (rbAmp @ 0x50  +  LCD backpack @ 0x27)
 *   SCL -> GPIO22
 *
 * Build:
 *   idf.py build flash monitor
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "rbamp.h"

static const char *TAG = "lcd_period";

#define SDA_GPIO    GPIO_NUM_21
#define SCL_GPIO    GPIO_NUM_22
#define RBAMP_ADDR  0x50
#define LCD_ADDR    0x27
#define LCD_TIMEOUT 50

/* ---- Minimal HD44780 over PCF8574 backpack ---- */
/*  bit layout: D7 D6 D5 D4 BL EN RW RS                                 */
static i2c_master_dev_handle_t s_lcd;

static void _lcd_write_4(uint8_t nib, bool rs)
{
    const uint8_t bl_en = 0x08 | 0x04;  /* backlight + EN */
    const uint8_t bl    = 0x08;
    const uint8_t rs_b  = rs ? 0x01 : 0x00;
    uint8_t b[2] = { (nib << 4) | bl_en | rs_b, (nib << 4) | bl | rs_b };
    (void)i2c_master_transmit(s_lcd, b, sizeof(b), LCD_TIMEOUT);
}

static void _lcd_send(uint8_t v, bool rs)
{
    _lcd_write_4(v >> 4, rs);
    _lcd_write_4(v & 0x0F, rs);
    vTaskDelay(1);
}

static void lcd_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LCD_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_lcd) != ESP_OK) {
        s_lcd = NULL;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    _lcd_write_4(0x03, false); vTaskDelay(pdMS_TO_TICKS(5));
    _lcd_write_4(0x03, false); vTaskDelay(pdMS_TO_TICKS(1));
    _lcd_write_4(0x03, false); vTaskDelay(pdMS_TO_TICKS(1));
    _lcd_write_4(0x02, false);                                 /* 4-bit mode */
    _lcd_send(0x28, false);   /* function set: 2 lines, 5x8 */
    _lcd_send(0x0C, false);   /* display on, cursor off */
    _lcd_send(0x06, false);   /* entry mode: increment */
    _lcd_send(0x01, false);   /* clear */
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void lcd_print(uint8_t row, const char *s)
{
    if (!s_lcd) return;
    _lcd_send(row == 0 ? 0x80 : 0xC0, false);
    for (int i = 0; i < 16 && s[i]; ++i) _lcd_send((uint8_t)s[i], true);
    /* pad to 16 with spaces so we overwrite previous text */
    for (int i = (int)strlen(s); i < 16; ++i) _lcd_send(' ', true);
}

/* ---- App ---- */

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

    /* LCD calls disabled — bench has no LCD on the I2C bus, only the DUT @ 0x50.
     * Acceptance-test variant: 10-s period, log master_dt_ms drift instead of
     * driving the LCD. Restore lcd_init/lcd_print for real LCD use. */

    rbamp_handle_t dev;
    ESP_ERROR_CHECK(rbamp_new(bus, RBAMP_ADDR, &dev));
    ESP_ERROR_CHECK(rbamp_begin(dev));

    const uint32_t PERIOD_MS = 10000;
    uint32_t ok = 0, stale = 0;
    while (true) {
        rbamp_period_snapshot_t snap;
        esp_err_t err = rbamp_read_period_snapshot_simple(dev, &snap);
        if (err == ESP_ERR_INVALID_RESPONSE) {
            stale++;
            ESP_LOGW(TAG, "STALE  ok=%lu stale=%lu",
                     (unsigned long)ok, (unsigned long)stale);
        } else if (err == ESP_OK) {
            ok++;
            int32_t drift = (int32_t)snap.master_dt_ms - (int32_t)PERIOD_MS;
            ESP_LOGI(TAG, "ok=%lu  master_dt=%lu ms  chip_latch=%lu ms  drift=%+ld ms  "
                          "avg_p[0]=%.2f W  Wh0=%.5f",
                     (unsigned long)ok,
                     (unsigned long)snap.master_dt_ms,
                     (unsigned long)snap.latch_ms,
                     (long)drift,
                     (double)snap.avg_p[0],
                     rbamp_energy_wh(dev, 0));
        }
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}
