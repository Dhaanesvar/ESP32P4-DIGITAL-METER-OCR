#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
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

#ifndef CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM
#define CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM
#endif

#ifndef CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM
#define CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM
#endif

#ifndef CONFIG_WIFI_RMT_TX_BUFFER_TYPE
#define CONFIG_WIFI_RMT_TX_BUFFER_TYPE CONFIG_ESP_WIFI_TX_BUFFER_TYPE
#endif

#ifndef CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF
#define CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF
#endif

#ifndef CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM
#define CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM
#endif

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "app_pushlog.h"
#include "app_eg91_modem.h"

static const char *TAG = "app_pushlog";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 10
#define WIFI_INIT_RETRY_INTERVAL_US (10LL * 1000000LL)

#define PUSHLOG_UPLOAD_INTERVAL_US (10LL * 60LL * 1000000LL)
#define PUSHLOG_TOKEN "unarvu2240"
#define PUSHLOG_URL_FMT_HTTPS "https://pushlog.unarvu.io/esp-cam/ingest?uid=%s&token=" PUSHLOG_TOKEN
#define PUSHLOG_URL_FMT_HTTP "http://pushlog.unarvu.io/esp-cam/ingest?uid=%s&token=" PUSHLOG_TOKEN
#define PUSHLOG_FIXED_URL_HTTPS "https://pushlog.unarvu.io/esp-cam/ingest?uid=08550001&token=unarvu2240"
#define PUSHLOG_FIXED_URL_HTTP "http://pushlog.unarvu.io/esp-cam/ingest?uid=08550001&token=unarvu2240"

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

static esp_err_t app_pushlog_upload_via_wifi(const char *url_https,
                                             const char *url_http,
                                             const uint8_t *jpeg_data,
                                             size_t jpeg_len,
                                             const char *ocr_json)
{
    esp_http_client_config_t config = {
        .url = url_https,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
#if CONFIG_ESP_TLS_INSECURE
    config.skip_cert_common_name_check = true;
#endif
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#if !CONFIG_ESP_TLS_INSECURE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        esp_http_client_set_header(client, "Accept", "*/*");
        esp_http_client_set_header(client, "Connection", "close");
        if (ocr_json && ocr_json[0] != '\0') {
            esp_http_client_set_header(client, "X-OCR-JSON", ocr_json);
        }
        esp_http_client_set_post_field(client, (const char *)jpeg_data, (int)jpeg_len);

        esp_err_t ret = esp_http_client_perform(client);
        if (ret == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            int content_len = esp_http_client_get_content_length(client);
            ESP_LOGI(TAG, "Pushlog POST via STA HTTPS status=%d resp_len=%d", status, content_len);
            esp_http_client_cleanup(client);
            if (status >= 200 && status < 300) {
                return ESP_OK;
            }
        } else {
            ESP_LOGW(TAG,
                     "STA HTTPS upload failed: %s (heap8=%u min8=%u internal=%u largest_internal=%u)",
                     esp_err_to_name(ret),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            esp_http_client_cleanup(client);
        }
    }

    esp_http_client_config_t fallback_cfg = {
        .url = url_http,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .disable_auto_redirect = true,
    };
    client = esp_http_client_init(&fallback_cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Connection", "close");
    if (ocr_json && ocr_json[0] != '\0') {
        esp_http_client_set_header(client, "X-OCR-JSON", ocr_json);
    }
    esp_http_client_set_post_field(client, (const char *)jpeg_data, (int)jpeg_len);

    esp_err_t ret_http = esp_http_client_perform(client);
    if (ret_http != ESP_OK) {
        ESP_LOGW(TAG, "STA HTTP fallback upload failed: %s", esp_err_to_name(ret_http));
        esp_http_client_cleanup(client);
        return ret_http;
    }

    int status_http = esp_http_client_get_status_code(client);
    int len_http = esp_http_client_get_content_length(client);
    ESP_LOGW(TAG, "Pushlog upload via STA HTTP fallback status=%d resp_len=%d", status_http, len_http);
    esp_http_client_cleanup(client);

    return (status_http >= 200 && status_http < 300) ? ESP_OK : ESP_FAIL;
}

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

    ESP_LOGI(TAG, "retrying STA init for fallback path");
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
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pushlog running in STA mode (temporary)");
    s_wifi_connected = false;
    s_wifi_started = false;
    s_next_wifi_init_retry_us = 0;

    ret = app_pushlog_try_start_wifi(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STA init failed at startup: %s", esp_err_to_name(ret));
        s_next_wifi_init_retry_us = esp_timer_get_time() + WIFI_INIT_RETRY_INTERVAL_US;
    }

    // Allow the first periodic check to run immediately after boot.
    s_next_capture_us = esp_timer_get_time();
    return ESP_OK;
}

bool app_pushlog_should_capture_now(void)
{
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

static void sanitize_ocr_token(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }

    size_t n = 0;
    for (size_t i = 0; src[i] != '\0' && n < (dst_size - 1); ++i) {
        char c = src[i];
        if ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            c == '_' || c == '-' || c == '.') {
            dst[n++] = c;
        }
    }
    dst[n] = '\0';
}

static void app_pushlog_build_ocr_json(const char *ocr_word,
                                       const char *meter_reading,
                                       float ocr_score,
                                       char *out_json,
                                       size_t out_size)
{
    if (!out_json || out_size < 3) {
        return;
    }

    char clean_word[40] = {0};
    char clean_reading[40] = {0};
    sanitize_ocr_token(ocr_word, clean_word, sizeof(clean_word));
    sanitize_ocr_token(meter_reading, clean_reading, sizeof(clean_reading));

    if (clean_word[0] == '\0' && clean_reading[0] == '\0') {
        snprintf(out_json, out_size, "{}");
        return;
    }

    snprintf(out_json,
             out_size,
             "{\"word\":\"%s\",\"reading\":\"%s\",\"score\":%.2f}",
             clean_word,
             clean_reading,
             (double)ocr_score);
}

esp_err_t app_pushlog_upload_jpeg(const uint8_t *jpeg_data,
                                  size_t jpeg_len,
                                  const char *ocr_word,
                                  const char *meter_reading,
                                  float ocr_score)
{
    if (!jpeg_data || jpeg_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char url_https[192] = {0};
    char url_http[192] = {0};
    char ocr_json[96] = {0};

    app_pushlog_build_ocr_json(ocr_word, meter_reading, ocr_score, ocr_json, sizeof(ocr_json));
    snprintf(url_https, sizeof(url_https), "%s", PUSHLOG_FIXED_URL_HTTPS);
    snprintf(url_http, sizeof(url_http), "%s", PUSHLOG_FIXED_URL_HTTP);

    ESP_LOGI(TAG,
             "OCR payload word=%s reading=%s score=%.2f json=%s",
             (ocr_word && ocr_word[0] != '\0') ? ocr_word : "-",
             (meter_reading && meter_reading[0] != '\0') ? meter_reading : "-",
             (double)ocr_score,
             ocr_json);

    app_pushlog_retry_wifi_if_needed();
    if (!s_wifi_connected) {
        ESP_LOGW(TAG, "STA not connected; skipping upload until Wi-Fi is up");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Pushlog upload via STA (temporary)");
    esp_err_t sta_ret = app_pushlog_upload_via_wifi(url_https,
                                                    url_http,
                                                    jpeg_data,
                                                    jpeg_len,
                                                    ocr_json);
    if (sta_ret == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Pushlog STA upload failed: %s", esp_err_to_name(sta_ret));
    return sta_ret;
}
