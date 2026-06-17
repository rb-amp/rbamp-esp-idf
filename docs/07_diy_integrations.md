# 07 · DIY integrations

How to feed `rbamp` readings into popular self-hosted automation
systems. For each platform there is a minimal working ESP-IDF
project plus the matching configuration on the platform side.

Cloud / commercial integrations (AWS IoT, Azure, GCP, InfluxDB
Cloud) — [08 · Cloud integrations](08_cloud_integrations.md).

| Platform | Transport | Auto-discovery | IDF components |
|---|---|---|---|
| Home Assistant | MQTT | yes (HA MQTT Discovery) | `espressif/esp_mqtt_client` |
| ESPHome | Native API + MQTT | yes (YAML config) | (alternative framework — see below) |
| Node-RED | MQTT (or HTTP) | manual flow | `espressif/esp_mqtt_client` |
| OpenHAB | MQTT (or REST) | manual `.things` | `espressif/esp_mqtt_client` |
| Domoticz | MQTT (auto) or HTTP | yes (MQTT plugin) | `esp_mqtt_client` or `esp_http_client` |
| InfluxDB OSS + Grafana | HTTPS line-protocol | no | `esp_http_client` |

> Ready-made projects for the two main scenarios live in `examples/`:
> [`examples/mqtt_publisher/`](https://github.com/rb-amp/rbamp-esp-idf/tree/main/examples/mqtt_publisher/) (per-channel
> MQTT publisher) and [`examples/ha_discovery/`](https://github.com/rb-amp/rbamp-esp-idf/tree/main/examples/ha_discovery/)
> (HA Auto-discovery on top of mqtt_publisher).

---

## Home Assistant — MQTT Auto-discovery

HA MQTT Discovery automatically creates the device and its sensors
when the ESP32 publishes config topics. No YAML edits in HA needed.

### ESP-IDF project (`rbamp` + WiFi STA + esp_mqtt_client)

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "rbamp.h"

#define DEVICE_ID    "rbamp_main"
#define DEVICE_NAME  "Mains rbAmp"
#define MQTT_URI     "mqtt://192.168.1.10:1883"

static const char *TAG = "ha_discovery";
static esp_mqtt_client_handle_t mqtt;

static void publish_discovery_sensor(const char *key, const char *friendly,
                                     const char *unit, const char *device_class,
                                     const char *state_class) {
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/%s/config", DEVICE_ID, key);
    int n = snprintf(payload, sizeof(payload),
        "{"
          "\"name\":\"%s %s\","
          "\"unique_id\":\"%s_%s\","
          "\"state_topic\":\"rbamp/%s/state\","
          "\"value_template\":\"{{ value_json.%s }}\","
          "\"state_class\":\"%s\","
          "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"manufacturer\":\"rbAmp\","
            "\"model\":\"UI*\""
          "}",
        DEVICE_NAME, friendly,
        DEVICE_ID, key,
        DEVICE_ID, key, state_class,
        DEVICE_ID, DEVICE_NAME);
    if (unit)         n += snprintf(payload+n, sizeof(payload)-n,
                                    ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class) n += snprintf(payload+n, sizeof(payload)-n,
                                    ",\"device_class\":\"%s\"", device_class);
    snprintf(payload+n, sizeof(payload)-n, "}");
    esp_mqtt_client_publish(mqtt, topic, payload, 0, /*qos*/1, /*retain*/1);
}

static void publish_discovery_all(void) {
    publish_discovery_sensor("voltage",      "Voltage",      "V",  "voltage",      "measurement");
    publish_discovery_sensor("current",      "Current",      "A",  "current",      "measurement");
    publish_discovery_sensor("power",        "Power",        "W",  "power",        "measurement");
    publish_discovery_sensor("energy",       "Energy",       "Wh", "energy",       "total_increasing");
    publish_discovery_sensor("frequency",    "Frequency",    "Hz", "frequency",    "measurement");
    publish_discovery_sensor("power_factor", "Power Factor", NULL, "power_factor", "measurement");
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    if (event_id == MQTT_EVENT_CONNECTED) {
        publish_discovery_all();   /* publish discovery on every connect */
    }
}

void app_main(void) {
    /* ...nvs_flash_init + esp_netif_init + esp_event_loop_create_default +
     *    esp_wifi_init/start + wait for IP_EVENT_STA_GOT_IP... */
    /* (pattern from 10 · Troubleshooting, "Watchdog timeout during WiFi") */

    /* I²C bus + rbAmp handle */
    i2c_master_bus_handle_t bus = NULL;
    /* ...i2c_new_master_bus(...)... */
    rbamp_handle_t dev = NULL;
    ESP_ERROR_CHECK(rbamp_new(bus, 0x50, &dev));   /* new — never fails
                                                    * with valid arguments */

    /* rbamp_begin may return NACK / VERSION on bus chatter or a cold
     * boot of the module. A soft retry is better than an ESP_ERROR_CHECK
     * abort — in production the network stack must survive a temporary
     * loss of the slave. */
    while (rbamp_begin(dev) != ESP_OK) {
        ESP_LOGE(TAG, "rbamp_begin: %s — retry in 1 s",
                 rbamp_err_to_str(rbamp_last_error(dev)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* set_sensor_class + set_ct_model — both block ~700 ms
     * (flash write). set_sensor_class is a mandatory prerequisite
     * for set_ct_model. Log failures, but do not abort. */
    esp_err_t err;
    if ((err = rbamp_set_sensor_class(dev, RBAMP_SENSOR_SCT013)) != ESP_OK) {
        ESP_LOGE(TAG, "set_sensor_class: %s", rbamp_err_to_str(err));
    }
    if ((err = rbamp_set_ct_model(dev, 3)) != ESP_OK) {
        ESP_LOGE(TAG, "set_ct_model: %s", rbamp_err_to_str(err));
    }

    /* MQTT client */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri  = MQTT_URI,
        .credentials.client_id = DEVICE_ID,
        .session.keepalive   = 60,
        .buffer.size         = 1024,    /* room for one discovery payload */
    };
    mqtt = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt, MQTT_EVENT_CONNECTED,
                                    mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt);

    /* State-publish loop — once a minute */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK ||
            !snap.valid) continue;

        float u, i, pf, freq;
        rbamp_read_voltage(dev, 0, &u);
        rbamp_read_current(dev, 0, &i);
        rbamp_read_power_factor(dev, 0, &pf);
        rbamp_read_frequency(dev, &freq);

        char state[384];
        snprintf(state, sizeof(state),
            "{\"voltage\":%.1f,\"current\":%.3f,\"power\":%.1f,"
            "\"energy\":%.3f,\"frequency\":%.1f,\"power_factor\":%.3f}",
            (double)u, (double)i, (double)snap.avg_p[0],
            rbamp_energy_wh(dev, 0), (double)freq, (double)pf);
        char topic[64];
        snprintf(topic, sizeof(topic), "rbamp/%s/state", DEVICE_ID);
        esp_mqtt_client_publish(mqtt, topic, state, 0, /*qos*/0, /*retain*/0);
    }
}
```

Full project — [`examples/ha_discovery/`](https://github.com/rb-amp/rbamp-esp-idf/tree/main/examples/ha_discovery/).

### Result in HA

A few seconds after the first publish, HA automatically creates the
"Mains rbAmp" device with 6 sensors (Voltage, Current, Power, Energy,
Frequency, Power Factor). The Energy sensor has
`state_class: total_increasing` and the correct `device_class` —
the HA Energy Dashboard accepts it as a consumption source.

To remove the device from HA later, publish an empty payload to
`homeassistant/sensor/.../config` (the retained flag clears the entry).

### Multi-channel UI3

Repeat the `publish_discovery_sensor()` calls with suffixed keys for
channels 1 and 2:

```c
publish_discovery_sensor("current_1", "Current 1", "A",  "current", "measurement");
publish_discovery_sensor("power_1",   "Power 1",   "W",  "power",   "measurement");
publish_discovery_sensor("energy_1",  "Energy 1",  "Wh", "energy",  "total_increasing");
/* ...same for _2 */
```

And extend the state JSON with `"current_1"`, `"power_1"`,
`"energy_1"` fields, filled in from `rbamp_read_current(dev, 1, &i)` /
`snap.avg_p[1]` / `rbamp_energy_wh(dev, 1)`.

---

## ESPHome — alternative via external_components

If you use ESPHome (rather than bare ESP-IDF), there is a dedicated
`rbamp` external component that **replaces this component entirely**
and handles its own HA integration through the ESPHome native API:

```yaml
external_components:
  - source: github://rbamp/rbamp-esphome
    components: [rbamp]
    refresh: 0s

i2c:
  sda: 21
  scl: 22
  frequency: 50kHz       # see 10 · Troubleshooting on baseline mitigation

sensor:
  - platform: rbamp
    address: 0x50
    update_interval: 60s
    voltage:
      name: "rbAmp Voltage"
    current:
      name: "rbAmp Current"
    power:
      name: "rbAmp Power"
    energy:
      name: "rbAmp Energy"
    frequency:
      name: "rbAmp Frequency"
    power_factor:
      name: "rbAmp Power Factor"
```

ESPHome integrates natively with HA through the ESPHome API (not
MQTT) — the device appears in HA automatically once flashed.

The `rbamp-esphome` component lives in a separate repository — see
the [main rbAmp index](https://github.com/rb-amp/rbamp) for links.
Use ESPHome **instead of** this component; mixing ESPHome and bare
ESP-IDF in the same project is usually not worthwhile.

---

## Node-RED

Subscribe to the ESP32 MQTT topic in a flow:

```json
[
  {
    "id": "rbamp_in",
    "type": "mqtt in",
    "topic": "rbamp/main/state",
    "qos": "0",
    "datatype": "json"
  },
  {
    "id": "rbamp_chart",
    "type": "ui_chart",
    "label": "Mains Power",
    "chartType": "line",
    "ymin": "0",
    "ymax": "5000"
  },
  {
    "id": "extract_power",
    "type": "function",
    "func": "msg.payload = msg.payload.power; return msg;"
  }
]
```

Wire up `rbamp_in → extract_power → rbamp_chart` and you get a
real-time power chart. Do the same for energy / voltage / PF.

If Node-RED runs on the same Pi as the MQTT broker, use the host
`localhost`. For remote brokers use `192.168.X.Y:1883` plus
credentials if the broker requires auth.

The ESP32 side is the same sketch as in the Home Assistant section
above; the JSON payload is shared — only the consumer differs.

---

## OpenHAB

OpenHAB 4.x + MQTT binding:

```text
# things/rbamp.things
Bridge mqtt:broker:local "MQTT Broker" [ host="192.168.1.10", port=1883 ] {
    Thing topic rbamp_main "rbAmp Main" {
        Channels:
            Type number : voltage "Voltage" [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.voltage" ]
            Type number : current "Current" [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.current" ]
            Type number : power   "Power"   [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.power" ]
            Type number : energy  "Energy"  [ stateTopic="rbamp/main/state", transformationPattern="JSONPATH:$.energy" ]
    }
}
```

```text
# items/rbamp.items
Number:ElectricPotential rbAmp_Voltage "Voltage [%.1f V]"  <energy> { channel="mqtt:topic:local:rbamp_main:voltage" }
Number:ElectricCurrent   rbAmp_Current "Current [%.3f A]"  <energy> { channel="mqtt:topic:local:rbamp_main:current" }
Number:Power             rbAmp_Power   "Power   [%.1f W]"  <energy> { channel="mqtt:topic:local:rbamp_main:power" }
Number:Energy            rbAmp_Energy  "Energy  [%.3f Wh]" <energy> { channel="mqtt:topic:local:rbamp_main:energy" }
```

The ESP32 side is the same project as in the Home Assistant section
above. The JSON payload is shared — only the consumer differs.

---

## Domoticz

The MQTT Auto-discovery plugin in Domoticz understands the same
`homeassistant/...` discovery topics. Enable the plugin in the
Domoticz settings and the ESP32 project from the HA section above will
automatically register the device in Domoticz, just as it does in HA.

An alternative is the native Domoticz HTTP API via `esp_http_client`:

```c
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "domoticz";

static void publish_to_domoticz(int idx, float power, double e_wh) {
    char url[256];
    /* Energy in Wh, Power in W — Domoticz format: "POWER;ENERGY_WH" */
    snprintf(url, sizeof(url),
        "http://192.168.1.20:8080/json.htm?type=command&param=udevice"
        "&idx=%d&svalue=%.1f;%.0f", idx, (double)power, e_wh);

    esp_http_client_config_t cfg = {
        .url     = url,
        .method  = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "domoticz HTTP %d (err %s)", status, esp_err_to_name(err));
    }
}

/* In the 60-second loop: */
publish_to_domoticz(123 /* your idx */, snap.avg_p[0], rbamp_energy_wh(dev, 0));
```

Create a device in the Domoticz UI of type **General → kWh**
(incremental counter), get its idx, and hardcode it into the project.

---

## InfluxDB OSS + Grafana

Write line-protocol points to InfluxDB directly from the ESP32 via
`esp_http_client`:

```c
#include "esp_http_client.h"
#include "esp_log.h"

#define INFLUX_HOST   "192.168.1.30:8086"
#define INFLUX_ORG    "homelab"
#define INFLUX_BKT    "energy"
#define INFLUX_TOKEN  "your-token-here"

static const char *TAG = "influx";

static void push_influx(float u, float p, double e_wh) {
    char url[256], body[256];
    snprintf(url, sizeof(url),
        "http://" INFLUX_HOST "/api/v2/write?org=%s&bucket=%s&precision=s",
        INFLUX_ORG, INFLUX_BKT);
    snprintf(body, sizeof(body),
        "rbamp,device=main voltage=%.1f,power=%.1f,energy=%.3f",
        (double)u, (double)p, e_wh);

    esp_http_client_config_t cfg = {
        .url    = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", "Token " INFLUX_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 204) {
        ESP_LOGW(TAG, "influx HTTP %d (err %s)", status, esp_err_to_name(err));
    }
}

/* In the 60-second loop: */
rbamp_period_snapshot_t snap;
if (rbamp_read_period_snapshot(dev, &snap, 50, false) == ESP_OK && snap.valid) {
    float u;
    rbamp_read_voltage(dev, 0, &u);
    push_influx(u, snap.avg_p[0], rbamp_energy_wh(dev, 0));
}
```

In Grafana, add an InfluxDB datasource, then a panel with a Flux
query:

```text
from(bucket: "energy")
  |> range(start: -24h)
  |> filter(fn: (r) => r._measurement == "rbamp" and r.device == "main")
  |> filter(fn: (r) => r._field == "power")
```

For a long-running soak deployment, additionally push a diagnostic
payload to a separate measurement carrying the heap level (via
`esp_get_free_heap_size()`) and counters of successful / failed
period snapshots — this lets you watch bus + WiFi health on the same
chart as the energy data:

```c
snprintf(body, sizeof(body),
    "rbamp_diag,device=main heap_free=%u,snap_ok=%lu,snap_fail=%lu",
    (unsigned)esp_get_free_heap_size(),
    (unsigned long)snap_ok_count,
    (unsigned long)snap_fail_count);
/* ...push to InfluxDB... */
```

> **Note**: the component's internal retry/sanity counters are not
> exposed in the public API (see [10 · Troubleshooting](10_troubleshooting.md)
> "On retry/sanity counters"). Count `snap_ok_count` / `snap_fail_count`
> yourself on the application side from the return values of
> `rbamp_read_period_snapshot()`.

---

## Multi-platform — fan-out from a single ESP32

If you need HA Auto-discovery, InfluxDB, and Node-RED all at once,
publish the state JSON once to MQTT and let each consumer subscribe to
`rbamp/+/state`. The ESP32 talks only to the MQTT broker — the broker
itself fans out to subscribers. Don't push from the ESP32 to N HTTP
endpoints directly: that couples the device to specific consumers.

For a very high-rate stream (5 Hz RT), run a sidecar Python script on
the Pi alongside the broker — subscribe to the fast topic, decimate,
and republish to the slow topics. The ESP32 I²C loop (a FreeRTOS task
with `rbamp_read_*()` calls) should stay focused, free of HTTP
overhead.

---

## Links

- [06 · Examples](06_examples.md) — the base projects these
  integrations build on (including `mqtt_publisher` and `ha_discovery`)
- [08 · Cloud integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB Cloud / generic webhook
- [10 · Troubleshooting](10_troubleshooting.md) — patterns for
  MQTT disconnect, WiFi event handler, TLS heap budget
