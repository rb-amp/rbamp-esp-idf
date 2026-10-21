/* Host-test stub: logs are printf so format strings are still checked. */
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("D %s: " fmt "\n", tag, ##__VA_ARGS__)
