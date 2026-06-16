# 08 · Cloud integrations

How to send `rbamp` readings to cloud platforms — AWS IoT
Core, Azure IoT Hub, Google Cloud, and serverless / managed
observability pipelines. For each platform: how to set up the
ESP-IDF project plus what you need to configure on the cloud side.

Self-hosted DIY platforms (Home Assistant, Node-RED, InfluxDB OSS)
— see [07 · DIY integrations](07_diy_integrations.md).

### Fleet → per-module MQTT topics

In the canonical deployment (mains + N sub-loads on one bus), the
master publishes readings on **a separate topic for each module**.
A convenient pattern is to iterate over the fleet using either the
module's address or its user-defined `LABEL`:

```c
char topic[64];
for (size_t i = 0; i < rbamp_fleet_count(fleet); ++i) {
    rbamp_handle_t dev = rbamp_fleet_get(fleet, i);
    char label[9]; rbamp_read_label(dev, label);
    snprintf(topic, sizeof topic, "rbamp/%s/snapshot",
             label[0] ? label : "unlabeled");
    /* publish snaps[i] as JSON to topic */
}
```

Additional topics: `rbamp/<label>/error` (driven by `rbamp_has_error`),
`rbamp/fleet/total_power`, `rbamp/fleet/total_energy_wh`,
`rbamp/fleet/balance` (mains − Σ(sub-loads) in the canonical
80% scenario). A full example with retained payloads and Home Assistant
auto-discovery is in chapter [06 · Examples](06_examples.md), scenario 1.

| Cloud | Transport | Auth | Latency | Cost |
|---|---|---|---|---|
| AWS IoT Core | MQTT/TLS | X.509 cert | low | $5/M messages |
| Azure IoT Hub | MQTT/TLS or AMQP | SAS token | low | $0.40-2/M messages |
| Google Cloud IoT (deprecated 2023) | MQTT/TLS | JWT | — | n/a |
| InfluxDB Cloud | HTTPS line-protocol | API token | medium | $250/mo+ |
| Generic webhook / REST | HTTPS POST | API key | high | depends |

> ⚠ **TLS on ESP32.** Any cloud transport over TLS adds
> ~30 kB of code + ~30 kB of heap per handshake. On very small targets
> with limited memory (the ESP32-C2 in particular, especially with
> Bluetooth enabled) the TLS heap can be a problem — see
> [10 · Troubleshooting](10_troubleshooting.md), section "TLS handshake fail".

## TLS certificates — file-based pattern via SPIFFS

Unlike the Arduino library, where certificates are usually embedded in
the firmware via PROGMEM strings, ESP-IDF prefers a **file-based
approach**: cert files are placed in a dedicated SPIFFS partition, and a
URI of the form `file:///certs/aws_root_ca.pem` is passed to the config.

### Partition table

Add an entry for the cert partition to your project's `partitions.csv`:

```text
# Name,     Type, SubType, Offset,  Size,    Flags
nvs,        data, nvs,     0x9000,  0x6000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 0x100000,
certs,      data, spiffs,  ,        0x20000,
```

In `sdkconfig.defaults`:

```text
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

### SPIFFS image with certificates

Place the cert files in a `spiffs_image/` directory next to `main/` and
enable image generation in `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES rbamp mqtt esp_http_client nvs_flash esp_wifi esp_event esp_netif
)

# SPIFFS image with certificates — built at build-time and flashed into
# the "certs" partition by one of the `idf.py spiffs-certs-flash` commands.
spiffs_create_partition_image(certs
    ${CMAKE_SOURCE_DIR}/spiffs_image
    FLASH_IN_PROJECT
)
```

After `idf.py build flash`, the files from `spiffs_image/*` live in the
`certs` partition and are accessible via VFS under `/certs/<filename>`.

### Mounting at runtime

```c
esp_vfs_spiffs_conf_t spiffs_cfg = {
    .base_path = "/certs",
    .partition_label = "certs",
    .max_files = 4,
    .format_if_mount_failed = false,
};
ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_cfg));
```

After this, the `"file:///certs/ca.pem"` URI can be passed to
`esp_mqtt_client_config_t.broker.verification.certificate_uri` and
similar fields.

---

## AWS IoT Core

AWS IoT Core uses mutual TLS with an X.509 device certificate.

### Provisioning

1. AWS Console → **IoT Core → Manage → Things → Create things → Single thing**.
2. Generate the certificate + keys; download `device.cert.pem`,
   `device.private.key`, `AmazonRootCA1.pem`.
3. Attach a policy that allows `iot:Connect`, `iot:Publish`
   on `arn:aws:iot:<region>:<acc>:topic/rbamp/+/state`.
4. Note the AWS IoT endpoint:
   `xxxxxx-ats.iot.<region>.amazonaws.com:8883`.

### Cert files in SPIFFS

Place in `spiffs_image/`:

```text
spiffs_image/
  ca.pem          # AmazonRootCA1.pem
  client.crt      # device.cert.pem
  client.key      # device.private.key
```

### ESP-IDF project

```c
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "rbamp.h"

#define AWS_URI       "mqtts://xxxxxx-ats.iot.eu-west-1.amazonaws.com:8883"
#define AWS_CLIENT_ID "rbamp-main"

static const char *TAG = "aws_iot";
static esp_mqtt_client_handle_t mqtt;

void app_main(void) {
    /* ...nvs + netif + wifi STA until IP_EVENT_STA_GOT_IP is received... */
    /* ...mount SPIFFS at /certs (see the TLS certificates section above)... */
    /* ...I²C bus + rbamp_new + rbamp_begin + set_sensor_class + set_ct_model... */
    rbamp_handle_t dev = NULL;

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = AWS_URI,
        .broker.verification.certificate_uri = "file:///certs/ca.pem",
        .credentials.client_id = AWS_CLIENT_ID,
        .credentials.authentication.certificate_uri = "file:///certs/client.crt",
        .credentials.authentication.key_uri         = "file:///certs/client.key",
        .session.keepalive = 60,
    };
    mqtt = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        rbamp_period_snapshot_t snap;
        if (rbamp_read_period_snapshot(dev, &snap, 50, false) != ESP_OK ||
            !snap.valid) continue;

        float u, freq;
        rbamp_read_voltage(dev, 0, &u);
        rbamp_read_frequency(dev, &freq);

        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"voltage\":%.1f,\"power\":%.1f,\"energy\":%.3f,\"freq\":%.1f}",
            (double)u, (double)snap.avg_p[0],
            rbamp_energy_wh(dev, 0), (double)freq);
        esp_mqtt_client_publish(mqtt, "rbamp/main/state",
                                payload, 0, /*qos*/0, /*retain*/0);
    }
}
```

### Processing on the cloud side

- Create an **IoT Rule**:
  `SELECT *, topic(2) AS device FROM 'rbamp/+/state'` → Kinesis
  Data Firehose or Lambda for storage.
- For dashboards, use AWS IoT SiteWise (industrial historian) or
  Timestream (time-series DB) → QuickSight.
- If Home Assistant runs on a Pi and consumes data from AWS,
  set up a local Mosquitto bridge (cheaper and faster than HA →
  AWS directly).

### On cost

Publishing once per minute per device, AWS IoT comes to ~525k
messages per year → ~$2.60/year/device on the "Connectivity" +
"Messaging" tier (2026, us-east-1). Timestream / Lambda cost
is separate.

---

## Azure IoT Hub

Azure IoT Hub supports MQTT 3.1.1 over TLS with SAS-token auth
(simpler than X.509 for home use cases).

### Provisioning

1. Azure Portal → **IoT Hub → Devices → New** → device ID
   `rbamp-main`, authentication = **Symmetric key**.
2. Save the connection string:
   `HostName=foo.azure-devices.net;DeviceId=rbamp-main;SharedAccessKey=…`.
3. Generate a SAS token (for example via
   `az iot hub generate-sas-token` or any Azure SDK).

### Cert + config

Place the Azure root CA (Baltimore CyberTrust Root or DigiCert
Global Root G2) in `spiffs_image/ca.pem`.

```c
#define AZ_URI        "mqtts://foo.azure-devices.net:8883"
#define AZ_USER       "foo.azure-devices.net/rbamp-main/?api-version=2021-04-12"
#define AZ_SAS        "SharedAccessSignature sr=foo.azure-devices.net%2Fdevices%2Frbamp-main&sig=…&se=…"
#define AZ_DEVICE_ID  "rbamp-main"

const esp_mqtt_client_config_t cfg = {
    .broker.address.uri = AZ_URI,
    .broker.verification.certificate_uri = "file:///certs/ca.pem",
    .credentials.client_id = AZ_DEVICE_ID,
    .credentials.username = AZ_USER,         /* SAS auth — no mTLS */
    .credentials.authentication.password = AZ_SAS,
    .session.keepalive = 60,
};

/* Publish to the Azure D2C topic: */
char topic[128];
snprintf(topic, sizeof(topic), "devices/%s/messages/events/", AZ_DEVICE_ID);
esp_mqtt_client_publish(mqtt, topic, payload, 0, /*qos*/0, /*retain*/0);
```

### SAS-token expiry

SAS tokens carry an `expiry` claim — typical lifetimes range from 1 hour
to 1 year. For an ESP32 deployment, generate a 1-year token on the
build machine and burn it into the firmware. For automatic
unattended rotation, refresh it periodically via the Azure IoT Hub
Device Provisioning Service (DPS); that is beyond the scope of the
component.

### Processing on the cloud side (Azure)

- Route messages to **Event Hubs** for high-throughput
  ingestion → Stream Analytics → Power BI dashboards.
- A cheaper alternative: messages → **Storage Account (blob)** →
  Synapse Serverless SQL for ad-hoc queries.

---

## Google Cloud IoT (DEPRECATED 2023)

Google shut down Cloud IoT Core in 2023. Migration paths:

- **MQTT broker on Compute Engine** (you deploy Mosquitto in a VM
  yourself) — the same pattern as in
  [07 · DIY integrations](07_diy_integrations.md), Home
  Assistant section, but pointing at the public IP of your VM.
- **HiveMQ Cloud / EMQX Cloud** — managed MQTT brokers, ~$10-20/mo
  on hobbyist tiers.
- **Pub/Sub over HTTPS** — publish directly to a Pub/Sub topic
  via the REST API (auth via a service-account JSON key, embedded in
  the ESP32).

For Pub/Sub over HTTPS, see the "Generic webhook / REST" section below,
substituting the Pub/Sub publish endpoint.

---

## InfluxDB Cloud (TLSv1.3 + line-protocol)

InfluxDB Cloud (Serverless tier) accepts line-protocol over
HTTPS — the same form as the OSS path in
[07 · DIY integrations](07_diy_integrations.md), InfluxDB OSS section,
but with `cloud2.influxdata.com` as the host and an API token for auth.

Place the CA for cloud2.influxdata.com (DigiCert Global Root G2 or
similar) in `spiffs_image/influx_ca.pem`.

```c
#include "esp_http_client.h"
#include "esp_log.h"

#define INFLUX_URL    "https://us-east-1-1.aws.cloud2.influxdata.com/api/v2/write?org=MyOrg&bucket=energy&precision=s"
#define INFLUX_TOKEN  "your-rw-token"

static const char *TAG = "influx_cloud";

static esp_err_t read_ca(char *buf, size_t buflen, size_t *out_len) {
    FILE *f = fopen("/certs/influx_ca.pem", "rb");
    if (!f) return ESP_FAIL;
    *out_len = fread(buf, 1, buflen - 1, f);
    buf[*out_len] = '\0';
    fclose(f);
    return ESP_OK;
}

static void push_influx_cloud(float u, float p, double e_wh) {
    static char ca_buf[2048];
    static size_t ca_len = 0;
    if (ca_len == 0) {
        if (read_ca(ca_buf, sizeof(ca_buf), &ca_len) != ESP_OK) {
            ESP_LOGE(TAG, "CA read failed");
            return;
        }
    }

    char body[256];
    snprintf(body, sizeof(body),
        "rbamp,device=main voltage=%.1f,power=%.1f,energy=%.3f",
        (double)u, (double)p, e_wh);

    esp_http_client_config_t cfg = {
        .url        = INFLUX_URL,
        .method     = HTTP_METHOD_POST,
        .cert_pem   = ca_buf,
        .timeout_ms = 10000,    /* TLS handshake may take up to 3 s */
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

/* In the 60-second loop, after rbamp_read_period_snapshot(): */
float u;
rbamp_read_voltage(dev, 0, &u);
push_influx_cloud(u, snap.avg_p[0], rbamp_energy_wh(dev, 0));
```

The free InfluxDB Cloud tier (5 GB / 30-day retention) covers
~5,000 points at a one-minute cadence per day — generous for home
use cases.

---

## Generic webhook / REST

Publishing to any HTTPS endpoint with an API key — works with
IFTTT webhooks, custom Flask / FastAPI services, or any
cloud function (AWS Lambda / Azure Functions / GCP Cloud Run)
exposed over HTTPS.

```c
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define WEBHOOK_URL "https://your-api.example.com/ingest"
#define API_KEY     "Bearer your-token-here"

static const char *TAG = "webhook";

/* CA for your HTTPS endpoint (read from /certs/webhook_ca.pem,
 * via snprintf+fopen as in push_influx_cloud). */
extern const char *get_server_ca(void);   /* implementation as in the InfluxDB section */

static void push_webhook(float u, float p, double e_wh) {
    char body[256];
    snprintf(body, sizeof(body),
        "{\"ts\":%lld,\"voltage\":%.1f,\"power\":%.1f,\"energy\":%.3f}",
        (long long)(esp_timer_get_time() / 1000000),
        (double)u, (double)p, e_wh);

    esp_http_client_config_t cfg = {
        .url        = WEBHOOK_URL,
        .method     = HTTP_METHOD_POST,
        .cert_pem   = get_server_ca(),
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", API_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "webhook HTTP %d (err %s)", status, esp_err_to_name(err));
    }
}
```

> ⚠ **`.skip_cert_common_name_check = false`** must stay
> false for production. During prototyping you can use
> `.use_global_ca_store = true` (the binary ships without a specific CA,
> which lowers TLS security) — but NEVER ship that to
> production without an explicit CA.

At a low rate (≤ once per minute), the overhead is acceptable. At
higher rates, batch the data on the ESP32 side (accumulate 10
minutes in a ring buffer, then publish as one bulk JSON) so you don't
pay for a TLS handshake per request.

---

## Hybrid: local storage + sync to cloud

For offline-tolerant deployments: log to SPIFFS once per minute and
push to the cloud once per hour. This survives WiFi outages without
losing data.

```c
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "rbamp.h"

#define LOG_PATH "/storage/log.csv"
static const char *TAG = "hybrid";

static void log_to_spiffs(const rbamp_period_snapshot_t *snap,
                          float u, double e_wh) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "fopen failed");
        return;
    }
    fprintf(f, "%lld,%.1f,%.1f,%.3f\n",
            (long long)(esp_timer_get_time() / 1000),
            (double)u, (double)snap->avg_p[0], e_wh);
    fclose(f);
}

static void sync_to_cloud_if_due(void) {
    static int64_t last_sync_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if ((now_us - last_sync_us) < 3600LL * 1000000LL) return;   /* once per hour */
    last_sync_us = now_us;

    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        push_webhook_line(line);   /* line → JSON → POST */
    }
    fclose(f);
    /* Optionally: rename(LOG_PATH, "/storage/log_pushed.csv") + truncate */
}
```

The component's accumulator `rbamp_energy_wh(dev, 0)` keeps counting
throughout the offline window — no data is lost as long as the ESP32 is
powered. The `storage` SPIFFS partition is shown in
[06 · Examples](06_examples.md), Scenario 8 (spiffs_logger).

---

## Energy budget

A TLS handshake is an expensive operation: ~3 s + ~30 kB of heap per
connection. For deep-sleep loggers (see
[06 · Examples](06_examples.md), Scenario 9):

- **Reuse the TLS session** if the sleep interval is < 24 h —
  the ESP-IDF `mbedtls` supports session resumption via the
  `mbedtls_ssl_get/set_session()` API. This shortens the handshake to
  ~500 ms.
- **Batch** several measurements to local SPIFFS into a single bulk
  POST per wake — the pattern from the "Hybrid" section above.
- **MQTT-over-TLS with a persistent session** (`session.disable_clean_session
  = true` in `esp_mqtt_client_config_t`) — the broker remembers your
  subscriptions between wakes, so you don't need to re-publish the
  discovery config.

At a 10-minute wake interval on a 2000 mAh Li-ion cell, expect ~3
months of operation on WiFi + TLS, versus ~6 months on WiFi + plain
MQTT (per the Scenario 9 budget).

---

## See also

- [06 · Examples](06_examples.md) — the base projects that the cloud
  integrations build on (especially Scenario 8 "spiffs_logger"
  for the hybrid approach)
- [07 · DIY integrations](07_diy_integrations.md) — self-hosted
  alternatives (Home Assistant / Node-RED / OpenHAB / InfluxDB OSS)
- [10 · Troubleshooting](10_troubleshooting.md) — WiFi dropouts /
  TLS handshake debugging / heap issues
