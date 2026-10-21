// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
typedef struct {int unused;} esp_bt_controller_config_t;
typedef enum {ESP_BT_CONTROLLER_STATUS_IDLE} esp_bt_controller_status_t;
typedef enum {ESP_BT_MODE_BLE=1} esp_bt_mode_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() {0}
esp_bt_controller_status_t esp_bt_controller_get_status(void);
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *configuration);
esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode);
