/* Host-test stub. */
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define configTICK_RATE_HZ 100u
#define portTICK_PERIOD_MS (1000u / configTICK_RATE_HZ)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portENTER_CRITICAL_ISR(m) ((void)(m))
#define portEXIT_CRITICAL_ISR(m) ((void)(m))
#define portYIELD_FROM_ISR(...) ((void)0)
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
