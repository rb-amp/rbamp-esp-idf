/**
 * @file    main.c
 * @brief   Example 5 — MQTT publisher with Home Assistant auto-discovery.
 *
 * Builds on the @c mqtt_publisher example and adds retained discovery
 * messages under the @c homeassistant/sensor/... topic prefix, so a Home
 * Assistant instance with MQTT discovery enabled creates the rbAmp sensors
 * automatically on boot.
 *
 * Each channel publishes:
 *   homeassistant/sensor/<id>_ch<n>_power/config    (one-shot, retained)
 *   homeassistant/sensor/<id>_ch<n>_energy/config   (one-shot, retained)
 *   rbamp/<id>/ch<n>/power_w                        (state, every minute)
 *   rbamp/<id>/ch<n>/energy_wh                      (state, every minute)
 *
 * Configure WIFI / MQTT credentials below.
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

static const char *TAG = "ha_disc";

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"
#define MQTT_URI        "mqtt://192.168.1.10:1883"
#define DEVICE_ID       "rbamp-esp32-01"

#define SDA_GPIO        GPIO_NUM_21
#define SCL_GPIO        GPIO_NUM_22
#define RBAMP_ADDR      0x50

static EventGroupHandle_t s_wifi_eg;
#define WIFI_BIT BIT0

static void on_wifi(void *a, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_eg, WIFI_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_BIT);
    }
}

static void wifi_up(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL);
    wifi_config_t sta = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(s_wifi_eg, WIFI_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

/* Build and publish one discovery message; returns the published packet id. */
static int publish_discovery(esp_mqtt_client_handle_t mqtt,
                             uint8_t ch, const char *kind,
                             const char *unit, const char *device_class,
                             const char *state_class, uint8_t fw)
{
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_ch%u_%s/config", DEVICE_ID, ch, kind);
    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"rbAmp ch%u %s\","
            "\"state_topic\":\"rbamp/%s/ch%u/%s_%s\","
            "\"unit_of_measurement\":\"%s\","
            "\"device_class\":\"%s\","
            "\"state_class\":\"%s\","
            "\"unique_id\":\"%s_ch%u_%s\","
            "\"device\":{"
                "\"identifiers\":[\"%s\"],"
                "\"name\":\"rbAmp %s\","
                "\"model\":\"rbAmp\","
                "\"manufacturer\":\"rbAmp\","
                "\"sw_version\":\"0x%02X\""
            "}"
        "}",
        ch, kind,
        DEVICE_ID, ch, kind, (strcmp(kind, "power") == 0) ? "w" : "wh",
        unit, device_class, state_class,
        DEVICE_ID, ch, kind,
        DEVICE_ID, DEVICE_ID, fw);
    /* retain = 1 so HA recovers entities after a broker restart. */
    return esp_mqtt_client_publish(mqtt, topic, payload, 0, 1, 1);
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    wifi_up();

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.client_id = DEVICE_ID,
    };
    esp_mqtt_client_handle_t mqtt = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));

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

    /* One-shot discovery for every populated channel. */
    const uint8_t fw = rbamp_firmware_version(dev);
    for (uint8_t ch = 0; ch < rbamp_channels(dev); ++ch) {
        publish_discovery(mqtt, ch, "power",  "W",  "power",  "measurement", fw);
        publish_discovery(mqtt, ch, "energy", "Wh", "energy", "total_increasing", fw);
    }
    ESP_LOGI(TAG, "discovery published for %u channels", rbamp_channels(dev));

    /* Periodic state. */
    char topic[96], payload[24];
    while (true) {
        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot_simple(dev, &snap) == ESP_OK) {
            for (uint8_t ch = 0; ch < rbamp_channels(dev); ++ch) {
                snprintf(topic, sizeof(topic), "rbamp/%s/ch%u/power_w", DEVICE_ID, ch);
                snprintf(payload, sizeof(payload), "%.2f", (double)snap.avg_p[ch]);
                esp_mqtt_client_publish(mqtt, topic, payload, 0, 0, 0);

                snprintf(topic, sizeof(topic), "rbamp/%s/ch%u/energy_wh", DEVICE_ID, ch);
                snprintf(payload, sizeof(payload), "%.4f", rbamp_energy_wh(dev, ch));
                esp_mqtt_client_publish(mqtt, topic, payload, 0, 0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
