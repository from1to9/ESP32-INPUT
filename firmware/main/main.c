// SPDX-License-Identifier: Apache-2.0
#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "input_core.h"
#include "lvgl.h"

/* Bring-up only. No HID or audio transport is advertised by this application. */
void app_main(void)
{
    bsp_board_variant_t board;
    esp_err_t error = bsp_board_variant_get(&board);
    if (error != ESP_OK) {
        ESP_LOGE("input", "Unsupported board revision: %s", esp_err_to_name(error));
        return;
    }
    ESP_ERROR_CHECK(bsp_power_init());
    ESP_ERROR_CHECK(bsp_imu_init());
    bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(bsp_imu_start(&config));
    float x, y, z;
    error = bsp_imu_get_gyro(&x, &y, &z);
    ESP_LOGI("input", "Board %s; gyro read: %s", board == BSP_BOARD_VARIANT_V1_0 ?
             "v1.0" : "v1.2", esp_err_to_name(error));
    input_state_t input;
    input_init(&input);
    lv_display_t *display = bsp_display_start();
    if (!display || !bsp_display_lock(-1)) {
        ESP_LOGE("input", "Display unavailable");
        return;
    }
    lv_obj_t *label = lv_label_create(lv_display_get_screen_active(display));
    lv_label_set_text(label, "ESP32-INPUT\nBSP bring-up\nMouse default / gyro OFF\nHID & audio not connected");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    bsp_display_unlock();
}
