/**
 * @file    main.c
 * @brief   Example 4 — Wi-Fi STA + MQTT publisher.
 *
 * Publishes per-channel real power and accumulated Wh once per minute under:
 *   rbamp/<DEVICE_ID>/ch<n>/power_w
 *   rbamp/<DEVICE_ID>/ch<n>/energy_wh
 *
 * Configure the four constants below for your network and broker.
 *
 * For Home Assistant auto-discovery, see the @c ha_discovery example.
 *
 * Build:
 *   idf.py build flash monitor
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "rbamp.h"

static const char *TAG = "mqtt_pub";

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"
#define MQTT_URI        "mqtt://192.168.1.10:1883"
#define DEVICE_ID       "rbamp-esp32-01"

#define SDA_GPIO        GPIO_NUM_21
#define SCL_GPIO        GPIO_NUM_22
#define RBAMP_ADDR      0x50

static EventGroupHandle_t s_wifi_eg;
#define WIFI_BIT_CONNECTED BIT0

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected; retrying");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_eg, WIFI_BIT_CONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "got IP");
        xEventGroupSetBits(s_wifi_eg, WIFI_BIT_CONNECTED);
    }
}

static void wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL));

    wifi_config_t sta = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_eg, WIFI_BIT_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

void app_main(void)
{
    /* NVS — required by Wi-Fi. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init();

    /* MQTT. */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.client_id = DEVICE_ID,
    };
    esp_mqtt_client_handle_t mqtt = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));

    /* I2C + rbAmp. */
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
    rbamp_handle_t dev;
    ESP_ERROR_CHECK(rbamp_new(bus, RBAMP_ADDR, &dev));
    ESP_ERROR_CHECK(rbamp_begin(dev));

    while (true) {
        rbamp_period_snapshot_t snap;
        esp_err_t err = rbamp_read_period_snapshot_simple(dev, &snap);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "snapshot: %s", rbamp_err_to_str(err));
        } else {
            char topic[96], payload[24];
            for (uint8_t ch = 0; ch < rbamp_channels(dev); ++ch) {
                snprintf(topic, sizeof(topic),
                         "rbamp/%s/ch%u/power_w", DEVICE_ID, ch);
                snprintf(payload, sizeof(payload), "%.2f", (double)snap.avg_p[ch]);
                esp_mqtt_client_publish(mqtt, topic, payload, 0, 0, 0);

                snprintf(topic, sizeof(topic),
                         "rbamp/%s/ch%u/energy_wh", DEVICE_ID, ch);
                snprintf(payload, sizeof(payload), "%.4f", rbamp_energy_wh(dev, ch));
                esp_mqtt_client_publish(mqtt, topic, payload, 0, 0, 0);
            }
            ESP_LOGI(TAG, "published ch0 P=%.1fW Wh=%.4f",
                     (double)snap.avg_p[0], rbamp_energy_wh(dev, 0));
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
