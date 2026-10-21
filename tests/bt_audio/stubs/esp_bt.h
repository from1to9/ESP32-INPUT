// SPDX-License-Identifier: Apache-2.0
#pragma once
typedef enum { ESP_BT_CONTROLLER_STATUS_IDLE, ESP_BT_CONTROLLER_STATUS_INITED,
    ESP_BT_CONTROLLER_STATUS_ENABLED } esp_bt_controller_status_t;
esp_bt_controller_status_t esp_bt_controller_get_status(void);
