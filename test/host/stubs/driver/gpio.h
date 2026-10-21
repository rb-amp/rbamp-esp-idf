/* Host-test stub. */
#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_4  4
#define GPIO_NUM_18 18
#define GPIO_NUM_21 21
#define GPIO_NUM_22 22
#define GPIO_NUM_25 25
#define GPIO_NUM_26 26
int gpio_get_level(gpio_num_t gpio);
#include <stdint.h>
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_NEGEDGE = 2 } gpio_int_type_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;
typedef void (*gpio_isr_t)(void *arg);
esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t isr, void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio);
#define GPIO_IS_VALID_GPIO(g) ((g) >= 0 && (g) < 40)
