// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifndef ESP_BT_GAP_JUST_WORKS_CONFIRM_REQUIRED
#define ESP_BT_GAP_JUST_WORKS_CONFIRM_REQUIRED 1
#endif
typedef uint8_t esp_bd_addr_t[6];
typedef uint8_t esp_bt_pin_code_t[16];
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_BT_IO_CAP_NONE 3
#define ESP_BT_SP_IOCAP_MODE 0
typedef enum { ESP_BT_NON_CONNECTABLE, ESP_BT_CONNECTABLE } esp_bt_connection_mode_t;
typedef enum { ESP_BT_NON_DISCOVERABLE, ESP_BT_LIMITED_DISCOVERABLE, ESP_BT_GENERAL_DISCOVERABLE } esp_bt_discovery_mode_t;
typedef enum { ESP_BT_GAP_AUTH_CMPL_EVT, ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT,
    ESP_BT_GAP_CFM_REQ_EVT, ESP_BT_GAP_PIN_REQ_EVT, ESP_BT_GAP_KEY_REQ_EVT } esp_bt_gap_cb_event_t;
typedef union {
    struct { uint8_t bda[6]; unsigned int stat; } auth_cmpl;
    struct { uint8_t bda[6]; } cfm_req;
    struct { uint8_t bda[6]; } pin_req;
    struct { uint8_t bda[6]; } key_req;
} esp_bt_gap_cb_param_t;
typedef void (*esp_bt_gap_cb_t)(esp_bt_gap_cb_event_t, esp_bt_gap_cb_param_t *);
typedef struct { esp_bt_connection_mode_t conn_mode; esp_bt_discovery_mode_t disc_mode; } esp_bt_gap_profile_status_t;
esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t);
esp_err_t esp_bt_gap_set_security_param(unsigned int, void *, uint8_t);
int esp_bt_gap_get_bond_device_num(void);
esp_err_t esp_bt_gap_get_bond_device_list(int *, esp_bd_addr_t *);
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t, esp_bt_discovery_mode_t);
esp_err_t esp_bt_gap_get_profile_status(esp_bt_gap_profile_status_t *);
esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t, bool, uint8_t, esp_bt_pin_code_t);
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t, bool);
esp_err_t esp_bt_gap_ssp_passkey_reply(esp_bd_addr_t, bool, uint32_t);
