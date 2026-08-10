#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#ifndef CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 10
#endif

#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 32
#endif

#ifndef CONFIG_ESP_WIFI_TX_BUFFER_TYPE
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 1
#endif

#ifndef CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF 1
#endif

#ifndef CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM 7
#endif

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "app_pushlog.h"

static const char *TAG = "app_pushlog";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 10
#define WIFI_INIT_RETRY_INTERVAL_US (10LL * 1000000LL)

#define PUSHLOG_UPLOAD_INTERVAL_US (10LL * 60LL * 1000000LL)
#define PUSHLOG_TOKEN "unarvu2240"
#define PUSHLOG_URL_FMT "https://pushlog.unarvu.io/esp-cam/ingest?uid=%s&token=" PUSHLOG_TOKEN

#define WIFI_SSID "Cre8IOT_2.4G"
#define WIFI_PASS "A2240624@2024"

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static bool s_wifi_connected;
static int64_t s_next_capture_us;
static int64_t s_next_wifi_init_retry_us;
static bool s_wifi_handlers_registered;
static bool s_wifi_netif_created;
static bool s_wifi_started;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static esp_err_t app_pushlog_try_start_wifi(bool wait_for_ip);
static void app_pushlog_retry_wifi_if_needed(void);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGW(TAG, "retry Wi-Fi connection (%d/%d)", s_wifi_retry_num, WIFI_MAX_RETRY);
        } else {
            s_next_wifi_init_retry_us = esp_timer_get_time() + WIFI_INIT_RETRY_INTERVAL_US;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t app_pushlog_try_start_wifi(bool wait_for_ip)
{
    esp_err_t ret;

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            ESP_LOGE(TAG, "failed to create Wi-Fi event group");
            return ESP_ERR_NO_MEM;
        }
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_wifi_netif_created) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "failed to create default Wi-Fi STA netif");
            return ESP_FAIL;
        }
        s_wifi_netif_created = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_wifi_init failed (hosted link may be down): %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_wifi_handlers_registered) {
        ret = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &s_wifi_event_instance);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &s_ip_event_instance);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(ret));
            return ret;
        }

        s_wifi_handlers_registered = true;
    }

    wifi_config_t wifi_config = { 0 };
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(ret));
    }

    s_wifi_started = true;
    s_wifi_retry_num = 0;

    if (wait_for_ip) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(20000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi ready for Pushlog uploads");
        } else {
            ESP_LOGW(TAG, "Wi-Fi not connected yet, uploads will retry later");
        }
    }

    return ESP_OK;
}

static void app_pushlog_retry_wifi_if_needed(void)
{
    if (s_wifi_connected) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us < s_next_wifi_init_retry_us) {
        return;
    }

    ESP_LOGI(TAG, "retrying Wi-Fi init/start");
    esp_err_t ret = app_pushlog_try_start_wifi(false);
    if (ret != ESP_OK) {
        s_next_wifi_init_retry_us = now_us + WIFI_INIT_RETRY_INTERVAL_US;
    } else if (!s_wifi_connected && s_wifi_started) {
        esp_wifi_connect();
        s_next_wifi_init_retry_us = now_us + WIFI_INIT_RETRY_INTERVAL_US;
    }
}

esp_err_t app_pushlog_init(void)
{
    esp_err_t ret = app_pushlog_try_start_wifi(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "initial Wi-Fi start failed, continuing and retrying in background");
        s_next_wifi_init_retry_us = esp_timer_get_time() + WIFI_INIT_RETRY_INTERVAL_US;
    }

    // Allow the first periodic check to run immediately after boot.
    s_next_capture_us = esp_timer_get_time();
    return ESP_OK;
}

bool app_pushlog_should_capture_now(void)
{
    app_pushlog_retry_wifi_if_needed();

    int64_t now_us = esp_timer_get_time();

    if (s_next_capture_us == 0) {
        s_next_capture_us = now_us + PUSHLOG_UPLOAD_INTERVAL_US;
        return false;
    }

    if (now_us < s_next_capture_us) {
        return false;
    }

    do {
        s_next_capture_us += PUSHLOG_UPLOAD_INTERVAL_US;
    } while (s_next_capture_us <= now_us);

    return true;
}

esp_err_t app_pushlog_upload_jpeg(const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (!jpeg_data || jpeg_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    app_pushlog_retry_wifi_if_needed();

    if (!s_wifi_connected) {
        ESP_LOGW(TAG, "skip upload: Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t mac[6] = {0};
    char uid[13] = {0};
    char url[192] = {0};

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(uid, sizeof(uid), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(url, sizeof(url), PUSHLOG_URL_FMT, uid);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_post_field(client, (const char *)jpeg_data, (int)jpeg_len);

    esp_err_t ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Pushlog POST failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int status = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "Pushlog POST status=%d resp_len=%d", status, content_len);

    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
