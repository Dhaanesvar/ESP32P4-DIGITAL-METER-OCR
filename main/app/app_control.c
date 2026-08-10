#include <stdio.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "iot_knob.h"

#include "ui_extra.h"
#include "app_video_stream.h"
#include "app_album.h"
#include "app_control.h"

/* Private definitions */
static const char *TAG = "app_control";

/* Button related variables */
static button_handle_t btns[BSP_BUTTON_NUM];
static knob_handle_t s_knob;

/* Knob related variables */
static int knob_step_counter = 0;
static int knob_last_direction = 0;  // 0: no direction, 1: right, -1: left
static int64_t knob_last_time = 0;   // timestamp of last rotation
static const int knob_timeout_ms = 500;  // timeout in milliseconds
static int knob_step_threshold = 3;  // threshold for knob step counter

/* Private function prototypes */
static void btn_handler(void *arg, void *data);
static void handle_knob_rotation(int direction, void (*action_camera)(void), void (*action_main)(void), void (*action_settings)(void));

/* Helper functions for camera actions */
static void camera_decrease_magnification(void)
{
    app_extra_set_magnification_factor(app_extra_get_magnification_factor() - 1);
}

static void camera_increase_magnification(void)
{
    app_extra_set_magnification_factor(app_extra_get_magnification_factor() + 1);
}

/* Button handler implementation */
static void btn_handler(void *arg, void *data)
{
    intptr_t button_id = (intptr_t)data;
    
    bsp_display_lock(0);

    if (ui_extra_handle_usb_disk_page()) {
        bsp_display_unlock();
        return;
    }
    
    switch (button_id) {
        case BSP_BUTTON_1:
            ui_extra_btn_menu();
            break;

        default:
            ESP_LOGW(TAG, "Unknown button ID: %d", (int)button_id);
            break;
    }
    
    bsp_display_unlock();
}

/* Helper function to handle knob rotation */
static void handle_knob_rotation(int direction, void (*action_camera)(void), void (*action_main)(void), void (*action_settings)(void))
{
    if (ui_extra_get_current_page() == UI_PAGE_ALBUM || 
        ui_extra_get_current_page() == UI_PAGE_USB_DISK) {
        return;
    }

    int64_t current_time = esp_timer_get_time() / 1000;  // get current time in milliseconds
    
    // Check for timeout or direction change
    if (current_time - knob_last_time > knob_timeout_ms || knob_last_direction == -direction) {
        // Timeout or direction change, reset counter
        knob_step_counter = 0;
        knob_last_direction = direction;
    }
    
    // Increment step counter
    knob_step_counter++;
    knob_last_time = current_time;
    
    // Trigger action when accumulated steps reach threshold
    if (knob_step_counter >= knob_step_threshold) {
        knob_step_counter = 0;  // Reset counter
        
        ESP_LOGD(TAG, "Continuous rotation detected: %d steps, value %d", knob_step_counter, direction);
        
        bsp_display_lock(0);
        if (ui_extra_get_current_page() == UI_PAGE_CAMERA || 
            ui_extra_get_current_page() == UI_PAGE_INTERVAL_CAM || 
            ui_extra_get_current_page() == UI_PAGE_VIDEO_MODE ||
            ui_extra_get_current_page() == UI_PAGE_AI_DETECT) {
            action_camera();
        } else if (ui_extra_get_current_page() == UI_PAGE_MAIN) {
            action_main();
        } else if (ui_extra_get_current_page() == UI_PAGE_SETTINGS) {
            action_settings();
        }
        bsp_display_unlock();
    }
}

/* Knob callbacks implementation */
static void knob_right_cb(void *arg, void *data)
{
    handle_knob_rotation(
        -1,
        camera_decrease_magnification,
        ui_extra_btn_up,
        ui_extra_btn_right
    );
}

static void knob_left_cb(void *arg, void *data)
{
    handle_knob_rotation(
        1,
        camera_increase_magnification,
        ui_extra_btn_down,
        ui_extra_btn_left
    );
}

/* Public functions implementation */

/**
 * @brief Set encoder step threshold for knob sensitivity
 * 
 * @param threshold Sensitivity threshold value (higher = less sensitive)
 */
void app_control_set_knob_sensitivity(int threshold)
{
    if (threshold > 0) {
        knob_step_threshold = threshold;
        ESP_LOGI(TAG, "Knob sensitivity set to %d steps", knob_step_threshold);
    }
}

/**
 * @brief Initialize application control module
 * 
 * This function initializes buttons and knob controls, and registers corresponding callbacks
 * 
 * @return
 *      - ESP_OK: Success
 *      - Others: Fail
 */
esp_err_t app_control_init(void)
{
    // Initialize the buttons
    ESP_ERROR_CHECK(bsp_iot_button_create(btns, NULL, BSP_BUTTON_NUM));
    ESP_ERROR_CHECK(iot_button_register_cb(btns[BSP_BUTTON_1], BUTTON_PRESS_DOWN, NULL, btn_handler, (void *) BSP_BUTTON_1));

    // Initialize the knob
    knob_config_t knob_cfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_ENCODER_A,
        .gpio_encoder_b = BSP_ENCODER_B,
        .enable_power_save = false,
    };
    s_knob = iot_knob_create(&knob_cfg);
    if (s_knob == NULL) {
        ESP_LOGE(TAG, "Failed to create knob");
        return ESP_FAIL;
    }

    // Register callback functions
    ESP_ERROR_CHECK(iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_cb, NULL));
    ESP_ERROR_CHECK(iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL));

    return ESP_OK;
}