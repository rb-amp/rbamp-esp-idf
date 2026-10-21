/* Host-test stub. */
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken);
