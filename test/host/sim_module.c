#include "sim_module.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rbamp.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

uint8_t  sim_regs[256];
unsigned sim_txn, sim_select_writes, sim_cmd_writes;
void   (*sim_on_command)(uint8_t opcode);

void sim_reset_image(void)
{
    memset(sim_regs, 0, sizeof sim_regs);
    sim_txn = sim_select_writes = sim_cmd_writes = 0;
    sim_on_command = NULL;
}
void  sim_set_f32(uint8_t reg, float v) { memcpy(&sim_regs[reg], &v, 4); }
float sim_get_f32(uint8_t reg) { float v; memcpy(&v, &sim_regs[reg], 4); return v; }
void  sim_set_u16(uint8_t reg, uint16_t v) { sim_regs[reg] = (uint8_t)v; sim_regs[(uint8_t)(reg + 1)] = (uint8_t)(v >> 8); }
uint16_t sim_get_u16(uint8_t reg) { return (uint16_t)(sim_regs[reg] | (sim_regs[(uint8_t)(reg + 1)] << 8)); }
float sim_window_value(uint8_t field, uint8_t ch) { return 100.0f + 10.0f * field + ch; }

/* ---- I2C ---- */
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out)
{ (void)bus; (void)cfg; *out = (i2c_master_dev_handle_t)(uintptr_t)0x51; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { (void)dev; return ESP_OK; }
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus) { (void)bus; return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int t) { (void)bus; (void)addr; (void)t; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *w, size_t n, int t)
{
    (void)dev; (void)t; sim_txn++;
    if (n >= 2) {
        sim_regs[w[0]] = w[1];
        if (w[0] == RBAMP_V2_REG_CHANNEL_SELECT) {
            sim_select_writes++;
            sim_set_f32(RBAMP_V2_REG_CHANNEL_DATA, sim_window_value((uint8_t)(w[1] >> 4), (uint8_t)(w[1] & 0x0F)));
        } else if (w[0] == RBAMP_REG_COMMAND) {
            sim_cmd_writes++;
            if (sim_on_command) sim_on_command(w[1]);
        }
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, int t)
{
    (void)dev; (void)wl; (void)t; sim_txn++;
    for (size_t i = 0; i < rl; ++i) r[i] = sim_regs[(uint8_t)(w[0] + i)];
    return ESP_OK;
}
/* ---- RTOS / platform ---- */
static TickType_t ticks;
void vTaskDelay(TickType_t t) { ticks += t; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 10000; }
void esp_rom_delay_us(uint32_t us) { (void)us; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return malloc(1); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return malloc(1); }
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdFALSE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *w) { (void)s; (void)w; return pdTRUE; }
esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
esp_err_t gpio_install_isr_service(int f) { (void)f; return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t g, gpio_isr_t i, void *a) { (void)g; (void)i; (void)a; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t g) { (void)g; return ESP_OK; }
int gpio_get_level(gpio_num_t g) { (void)g; return 1; }
const char *esp_err_to_name(esp_err_t e) { static char b[16]; snprintf(b, sizeof b, "0x%x", e); return b; }
