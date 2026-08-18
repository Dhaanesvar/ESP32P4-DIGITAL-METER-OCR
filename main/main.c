/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"

#include "app_pushlog.h"
#include "app_pushlog_camera.h"
#include "app_pushlog_web.h"
#include "app_eg91_modem.h"

static const char *TAG = "main";

#define EG91_UART_PORT           UART_NUM_1
#define EG91_UART_TXD            GPIO_NUM_54
#define EG91_UART_RXD            GPIO_NUM_53
#define EG91_STATUS_GPIO         GPIO_NUM_52
#define EG91_UART_BAUDRATE       115200
#define EG91_UART_RX_BUF_SIZE    2048
#define EG91_AT_RESPONSE_TIMEOUT_MS  1500
#define EG91_MONITOR_INTERVAL_MS     (15 * 60 * 1000)
#define EG91_HTTP_TIMEOUT_MS         30000
#define EG91_PDP_APN                 "internet"
#define EG91_PDP_AUTH                0
#define EG91_HEALTH_FAIL_REBOOT_THRESHOLD 3
#define EG91_PDP_ACTIVATE_RETRIES 3

static bool s_eg91_uart_ready = false;
static SemaphoreHandle_t s_eg91_uart_mutex = NULL;
static bool s_eg91_last_uart_busy = false;

static void eg91_log_test_box(const char *title, const char *const *cmds, const bool *results, size_t count)
{
    ESP_LOGI(TAG, "+-------------------- %s --------------------+", title);
    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "| %-12s : %-4s                         |", cmds[i], results[i] ? "PASS" : "FAIL");
    }
    ESP_LOGI(TAG, "+----------------------------------------------------------+");
}

static void configure_runtime_log_levels(void)
{
    // Keep monitor concise: show modem AT and upload flow, suppress noisy AI/video logs.
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("app_pushlog", ESP_LOG_INFO);
    esp_log_level_set("pushlog_cam", ESP_LOG_INFO);
}

typedef struct {
    char host[96];
    char path[192];
} eg91_url_parts_t;

static int eg91_collect_response(int wait_ms, char *out, size_t out_size, bool log_chunks)
{
    uint8_t rx_buf[256];
    int64_t start = esp_timer_get_time();
    int64_t wait_us = (int64_t)wait_ms * 1000;
    int total = 0;

    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }

    while ((esp_timer_get_time() - start) < wait_us) {
        int len = uart_read_bytes(EG91_UART_PORT, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            rx_buf[len] = '\0';

            if (log_chunks) {
                ESP_LOGI(TAG, "EG91 << %s", (char *)rx_buf);
            }

            if (out != NULL && out_size > 0 && total < (int)(out_size - 1)) {
                int copy_len = len;
                int free_space = (int)(out_size - 1) - total;
                if (copy_len > free_space) {
                    copy_len = free_space;
                }
                if (copy_len > 0) {
                    memcpy(&out[total], rx_buf, copy_len);
                    total += copy_len;
                    out[total] = '\0';
                }
            }

            start = esp_timer_get_time();
        }
    }

    return total;
}

static bool eg91_send_at_locked(const char *cmd, int wait_ms, char *response, size_t response_size)
{
    ESP_LOGI(TAG, "EG91 >> %s", cmd);
    uart_write_bytes(EG91_UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(EG91_UART_PORT, "\r\n", 2);
    int rx_len = eg91_collect_response(wait_ms, response, response_size, true);
    if (rx_len <= 0) {
        ESP_LOGW(TAG, "EG91 no response for command: %s", cmd);
    }
    return rx_len > 0;
}

static bool eg91_parse_url(const char *url, eg91_url_parts_t *parts)
{
    if (!url || !parts) {
        return false;
    }

    const char *start = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
    } else {
        return false;
    }

    const char *slash = strchr(start, '/');
    size_t host_len = slash ? (size_t)(slash - start) : strlen(start);
    if (host_len == 0 || host_len >= sizeof(parts->host)) {
        return false;
    }

    memcpy(parts->host, start, host_len);
    parts->host[host_len] = '\0';

    if (slash && *slash) {
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(parts->path)) {
            return false;
        }
        memcpy(parts->path, slash, path_len + 1);
    } else {
        strcpy(parts->path, "/");
    }

    return true;
}

static bool eg91_expect_ok_locked(const char *cmd, int wait_ms)
{
    char resp[512];
    bool got = eg91_send_at_locked(cmd, wait_ms, resp, sizeof(resp));
    return got && strstr(resp, "OK") != NULL && strstr(resp, "ERROR") == NULL;
}

static bool eg91_expect_ok_resp_locked(const char *cmd, int wait_ms, char *resp, size_t resp_size)
{
    bool got = eg91_send_at_locked(cmd, wait_ms, resp, resp_size);
    return got && strstr(resp, "OK") != NULL && strstr(resp, "ERROR") == NULL;
}

static bool eg91_wait_for_locked(const char *token, int wait_ms, char *resp, size_t resp_size)
{
    int len = eg91_collect_response(wait_ms, resp, resp_size, true);
    return len > 0 && strstr(resp, token) != NULL;
}

static bool eg91_activate_pdp_locked(char *resp, size_t resp_size)
{
    char cmd[192];
    const int pdp_types[] = {1, 3}; // 1=IPV4, 3=IPV4V6
    const int auth_types[] = {EG91_PDP_AUTH, 1}; // 0=None, 1=PAP

    for (int attempt = 1; attempt <= EG91_PDP_ACTIVATE_RETRIES; ++attempt) {
        ESP_LOGI(TAG, "EG91 PDP activation attempt %d/%d", attempt, EG91_PDP_ACTIVATE_RETRIES);

        (void)eg91_expect_ok_locked("AT+QIDEACT=1", 8000);
        vTaskDelay(pdMS_TO_TICKS(350));

        for (size_t pt = 0; pt < sizeof(pdp_types) / sizeof(pdp_types[0]); ++pt) {
            for (size_t at = 0; at < sizeof(auth_types) / sizeof(auth_types[0]); ++at) {
                int pdp_type = pdp_types[pt];
                int auth_type = auth_types[at];

                snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,%d,\"%s\",\"\",\"\",%d", pdp_type, EG91_PDP_APN, auth_type);
                if (!eg91_expect_ok_resp_locked(cmd, 4000, resp, resp_size)) {
                    ESP_LOGW(TAG, "EG91 QICSGP failed (attempt %d, pdp=%d auth=%d): %s",
                             attempt,
                             pdp_type,
                             auth_type,
                             resp);
                    continue;
                }

                if (!eg91_expect_ok_resp_locked("AT+QIACT=1", 18000, resp, resp_size)) {
                    ESP_LOGW(TAG, "EG91 QIACT failed (attempt %d, pdp=%d auth=%d): %s",
                             attempt,
                             pdp_type,
                             auth_type,
                             resp);
                    if (eg91_send_at_locked("AT+QIGETERROR", 3000, resp, resp_size)) {
                        ESP_LOGW(TAG, "EG91 QIGETERROR: %s", resp);
                    }
                    continue;
                }

                if (eg91_expect_ok_resp_locked("AT+QIACT?", 4000, resp, resp_size) && strstr(resp, "+QIACT: 1,1,1") != NULL) {
                    return true;
                }

                // Some firmware variants return a different +QIACT format; accept any active context line.
                if (strstr(resp, "+QIACT: 1,") != NULL) {
                    return true;
                }

                ESP_LOGW(TAG, "EG91 PDP appears inactive after QIACT (attempt %d, pdp=%d auth=%d): %s",
                         attempt,
                         pdp_type,
                         auth_type,
                         resp);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(900));
    }

    return false;
}

bool app_eg91_modem_is_ready(void)
{
    return s_eg91_uart_ready;
}

esp_err_t app_eg91_modem_http_post_jpeg(const char *url,
                                        const uint8_t *jpeg_data,
                                        size_t jpeg_len,
                                        const char *ocr_json,
                                        int *http_status)
{
    if (!url || !jpeg_data || jpeg_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_eg91_uart_ready || !s_eg91_uart_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    eg91_url_parts_t parts = {0};
    if (!eg91_parse_url(url, &parts)) {
        ESP_LOGE(TAG, "invalid URL for EG91 upload");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_eg91_uart_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    char resp[768];
    char cmd[192];

    do {
        bool at_ok = false;
        for (int i = 0; i < 3; ++i) {
            uart_write_bytes(EG91_UART_PORT, "\r", 1);
            vTaskDelay(pdMS_TO_TICKS(80));
            if (eg91_expect_ok_locked("AT", 3000)) {
                at_ok = true;
                break;
            }
        }
        if (!at_ok) {
            ESP_LOGE(TAG, "EG91 no response to AT, STATUS GPIO%d=%d", EG91_STATUS_GPIO, gpio_get_level(EG91_STATUS_GPIO));
            break;
        }
        (void)eg91_expect_ok_locked("ATE0", EG91_AT_RESPONSE_TIMEOUT_MS);

        if (!eg91_expect_ok_resp_locked("AT+CPIN?", 4000, resp, sizeof(resp)) || strstr(resp, "+CPIN: READY") == NULL) {
            ESP_LOGE(TAG, "EG91 SIM not ready: %s", resp);
            break;
        }
        if (!eg91_expect_ok_resp_locked("AT+CREG?", 4000, resp, sizeof(resp)) ||
            (strstr(resp, "+CREG: 0,1") == NULL && strstr(resp, "+CREG: 0,5") == NULL)) {
            ESP_LOGE(TAG, "EG91 not registered: %s", resp);
            break;
        }
        if (!eg91_expect_ok_resp_locked("AT+CGATT?", 4000, resp, sizeof(resp)) || strstr(resp, "+CGATT: 1") == NULL) {
            ESP_LOGE(TAG, "EG91 not attached: %s", resp);
            break;
        }

        if (!eg91_activate_pdp_locked(resp, sizeof(resp))) {
            ESP_LOGE(TAG, "EG91 QIACT failed after retries");
            break;
        }

        (void)eg91_expect_ok_locked("AT+QSSLCFG=\"sslversion\",1,4", 4000);
        (void)eg91_expect_ok_locked("AT+QSSLCFG=\"seclevel\",1,0", 4000);

        if (!eg91_expect_ok_locked("AT+QHTTPCFG=\"contextid\",1", 4000)) {
            ESP_LOGE(TAG, "EG91 QHTTPCFG contextid failed");
            break;
        }
        if (!eg91_expect_ok_locked("AT+QHTTPCFG=\"requestheader\",1", 4000)) {
            ESP_LOGE(TAG, "EG91 QHTTPCFG requestheader failed");
            break;
        }
        if (!eg91_expect_ok_locked("AT+QHTTPCFG=\"sslctxid\",1", 4000)) {
            ESP_LOGE(TAG, "EG91 QHTTPCFG sslctxid failed");
            break;
        }

        size_t url_len = strlen(url);
        snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,120", (unsigned)url_len);
        if (!eg91_send_at_locked(cmd, 5000, resp, sizeof(resp)) || strstr(resp, "CONNECT") == NULL) {
            ESP_LOGE(TAG, "EG91 QHTTPURL no CONNECT: %s", resp);
            break;
        }
        uart_write_bytes(EG91_UART_PORT, url, url_len);
        if (!eg91_wait_for_locked("OK", 4000, resp, sizeof(resp))) {
            ESP_LOGE(TAG, "EG91 QHTTPURL did not finish OK: %s", resp);
            break;
        }

        char req_hdr[512];
        int hdr_len = snprintf(req_hdr,
                               sizeof(req_hdr),
                               "POST %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Content-Type: image/jpeg\r\n"
                               "X-OCR-JSON: %s\r\n"
                               "Content-Length: %u\r\n"
                               "Connection: close\r\n\r\n",
                               parts.path,
                               parts.host,
                               (ocr_json && ocr_json[0]) ? ocr_json : "{}",
                               (unsigned)jpeg_len);
        if (hdr_len <= 0 || (size_t)hdr_len >= sizeof(req_hdr)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        size_t total_len = (size_t)hdr_len + jpeg_len;
        snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%u,180,180", (unsigned)total_len);
        if (!eg91_send_at_locked(cmd, 5000, resp, sizeof(resp)) || strstr(resp, "CONNECT") == NULL) {
            ESP_LOGE(TAG, "EG91 QHTTPPOST no CONNECT: %s", resp);
            break;
        }

        uart_write_bytes(EG91_UART_PORT, req_hdr, hdr_len);
        uart_write_bytes(EG91_UART_PORT, (const char *)jpeg_data, jpeg_len);

        // QHTTPPOST response can take long for large payloads at 115200 bps.
        // Derive a practical wait window from payload size and clamp it.
        int post_wait_ms = (int)((jpeg_len * 1000ULL * 12ULL) / EG91_UART_BAUDRATE) + 90000;
        if (post_wait_ms < 90000) {
            post_wait_ms = 90000;
        }
        if (post_wait_ms > 300000) {
            post_wait_ms = 300000;
        }

        if (!eg91_wait_for_locked("+QHTTPPOST:", post_wait_ms, resp, sizeof(resp))) {
            ESP_LOGE(TAG, "EG91 QHTTPPOST timeout/no result: %s", resp);
            break;
        }

        int qerr = -1;
        int status = -1;
        int body_len = -1;
        char *p = strstr(resp, "+QHTTPPOST:");
        if (!p || sscanf(p, "+QHTTPPOST: %d,%d,%d", &qerr, &status, &body_len) < 2) {
            ESP_LOGE(TAG, "EG91 QHTTPPOST parse fail: %s", resp);
            break;
        }

        if (http_status) {
            *http_status = status;
        }

        ESP_LOGI(TAG, "EG91 HTTP result qerr=%d status=%d body_len=%d", qerr, status, body_len);
        if (qerr == 0 && status >= 200 && status < 300) {
            ret = ESP_OK;
        }
    } while (0);

    xSemaphoreGive(s_eg91_uart_mutex);
    return ret;
}

static bool eg91_send_at(const char *cmd, int wait_ms, char *response, size_t response_size)
{
    s_eg91_last_uart_busy = false;

    if (!s_eg91_uart_ready) {
        ESP_LOGW(TAG, "EG91 UART not ready yet");
        return false;
    }

    if (s_eg91_uart_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_eg91_uart_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        s_eg91_last_uart_busy = true;
        ESP_LOGW(TAG, "EG91 UART busy");
        return false;
    }

    bool ok = eg91_send_at_locked(cmd, wait_ms, response, response_size);
    xSemaphoreGive(s_eg91_uart_mutex);
    return ok;
}

static bool eg91_validate_response(const char *cmd, const char *resp)
{
    if (resp == NULL || resp[0] == '\0') {
        return false;
    }

    if (strstr(resp, "ERROR") != NULL) {
        return false;
    }

    if (strcmp(cmd, "ATI") == 0) {
        return strstr(resp, "EG91") != NULL && strstr(resp, "OK") != NULL;
    }

    if (strcmp(cmd, "AT+CPIN?") == 0) {
        return strstr(resp, "+CPIN: READY") != NULL && strstr(resp, "OK") != NULL;
    }

    if (strcmp(cmd, "AT+CSQ") == 0) {
        return strstr(resp, "+CSQ:") != NULL && strstr(resp, "OK") != NULL;
    }

    if (strcmp(cmd, "AT+CREG?") == 0) {
        return (strstr(resp, "+CREG: 0,1") != NULL || strstr(resp, "+CREG: 0,5") != NULL) && strstr(resp, "OK") != NULL;
    }

    return strstr(resp, "OK") != NULL;
}

static void eg91_diagnose_and_reboot(const char *failed_cmd)
{
    char diag_resp[320];

    ESP_LOGE(TAG, "EG91 health check failed on command: %s", failed_cmd);
    ESP_LOGE(TAG, "EG91 STATUS GPIO%d=%d", EG91_STATUS_GPIO, gpio_get_level(EG91_STATUS_GPIO));

    if (s_eg91_uart_mutex != NULL && xSemaphoreTake(s_eg91_uart_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        (void)eg91_send_at_locked("AT", EG91_AT_RESPONSE_TIMEOUT_MS, diag_resp, sizeof(diag_resp));
        (void)eg91_send_at_locked("AT+CPIN?", EG91_AT_RESPONSE_TIMEOUT_MS, diag_resp, sizeof(diag_resp));
        (void)eg91_send_at_locked("AT+CSQ", EG91_AT_RESPONSE_TIMEOUT_MS, diag_resp, sizeof(diag_resp));
        (void)eg91_send_at_locked("AT+CREG?", EG91_AT_RESPONSE_TIMEOUT_MS, diag_resp, sizeof(diag_resp));
        xSemaphoreGive(s_eg91_uart_mutex);
    }

    ESP_LOGE(TAG, "Rebooting due to EG91 modem health check failure");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static bool eg91_run_health_check_once(void)
{
    static const char *cmds[] = {
        "AT",
        "ATE0",
        "ATI",
        "AT+CPIN?",
        "AT+CSQ",
        "AT+CREG?",
    };
    char resp[512];
    bool results[sizeof(cmds) / sizeof(cmds[0])] = {false};
    bool all_ok = true;

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        bool got_rx = eg91_send_at(cmds[i], EG91_AT_RESPONSE_TIMEOUT_MS, resp, sizeof(resp));
        if (!got_rx) {
            if (s_eg91_last_uart_busy) {
                ESP_LOGW(TAG, "EG91 health check deferred: UART busy");
                return true;
            }
            results[i] = false;
            all_ok = false;
            eg91_log_test_box("AT TESTS", cmds, results, sizeof(cmds) / sizeof(cmds[0]));
            ESP_LOGE(TAG, "EG91 health check no response on %s", cmds[i]);
            return false;
        }
        bool ok = eg91_validate_response(cmds[i], resp);
        results[i] = ok;
        if (!ok) {
            all_ok = false;
            eg91_log_test_box("AT TESTS", cmds, results, sizeof(cmds) / sizeof(cmds[0]));
            ESP_LOGE(TAG, "EG91 health check invalid response on %s", cmds[i]);
            return false;
        }
    }

    eg91_log_test_box("AT TESTS", cmds, results, sizeof(cmds) / sizeof(cmds[0]));

    if (all_ok) {
        ESP_LOGI(TAG, "EG91 health check OK");
    }
    return all_ok;
}

static void eg91_modem_monitor_task(void *arg)
{
    (void)arg;

    gpio_config_t status_cfg = {
        .pin_bit_mask = 1ULL << EG91_STATUS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&status_cfg));

    s_eg91_uart_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_eg91_uart_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    uart_config_t uart_cfg = {
        .baud_rate = EG91_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(EG91_UART_PORT, EG91_UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(EG91_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(EG91_UART_PORT, EG91_UART_TXD, EG91_UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_flush(EG91_UART_PORT));
    s_eg91_uart_ready = true;

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "EG91 monitor started (interval: %d minutes)", EG91_MONITOR_INTERVAL_MS / 60000);

    int fail_count = 0;

    while (true) {
        if (eg91_run_health_check_once()) {
            fail_count = 0;
        } else {
            fail_count++;
            ESP_LOGE(TAG, "EG91 health check failed (%d/%d)", fail_count, EG91_HEALTH_FAIL_REBOOT_THRESHOLD);
            if (fail_count >= EG91_HEALTH_FAIL_REBOOT_THRESHOLD) {
                eg91_diagnose_and_reboot("consecutive health-check failures");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(EG91_MONITOR_INTERVAL_MS));
    }
}

static void eg91_console_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t idx = 0;

    ESP_LOGI(TAG, "EG91 console ready. Type AT commands in monitor and press Enter.");

    while (true) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (idx == 0) {
                continue;
            }

            line[idx] = '\0';
            idx = 0;

            if (!s_eg91_uart_ready) {
                ESP_LOGW(TAG, "EG91 UART not ready yet");
                continue;
            }

            if (strcasecmp(line, "help") == 0) {
                ESP_LOGI(TAG, "Type AT commands directly, e.g. AT, ATI, AT+CSQ, AT+CREG?");
                continue;
            }

            if (line[0] == '\0') {
                continue;
            }

            char resp[512];
            (void)eg91_send_at(line, EG91_AT_RESPONSE_TIMEOUT_MS, resp, sizeof(resp));
            continue;
        }

        if (idx < (sizeof(line) - 1)) {
            line[idx++] = (char)ch;
        }
    }
}

void app_main(void)
{
    configure_runtime_log_levels();

    ESP_LOGI(TAG, "Initialize NVS");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initialize the I2C");
    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();

    ESP_LOGI(TAG, "Start EG91 modem tasks");
    xTaskCreate(eg91_modem_monitor_task, "eg91_monitor", 4096, NULL, 9, NULL);
    xTaskCreate(eg91_console_task, "eg91_console", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Initialize Pushlog uploader");
    ESP_ERROR_CHECK(app_pushlog_init());

    ESP_LOGI(TAG, "Initialize camera capture pipeline");
    ret = app_pushlog_camera_init(i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera pipeline init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Pushlog upload paused until camera init is fixed");
        return;
    }

    ESP_LOGI(TAG, "Start Pushlog web UI");
    ret = app_pushlog_web_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Web UI start failed: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Pushlog camera uploader started");
}