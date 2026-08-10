/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"

#include "app_pushlog.h"
#include "app_pushlog_camera.h"
#include "app_pushlog_web.h"

static const char *TAG = "main";

void app_main(void)
{
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