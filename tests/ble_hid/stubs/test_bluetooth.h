// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Minimal hardware API model, not a radio, ATT implementation or SDK ABI test.
 * Event fields and constants model the adapter's pinned SDK boundary. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_BD_ADDR_LEN 6
#define ESP_UUID_LEN_16 2
#define ESP_GATT_MAX_ATTR_LEN 517
#define ESP_GATT_IF_NONE 0xff
#define ESP_GATT_UUID_HID_SVC 0x1812
#define ESP_GATT_UUID_PRI_SERVICE 0x2800
#define ESP_GATT_UUID_CHAR_DECLARE 0x2803
#define ESP_GATT_UUID_CHAR_CLIENT_CONFIG 0x2902
#define ESP_GATT_UUID_RPT_REF_DESCR 0x2908
#define ESP_GATT_UUID_HID_INFORMATION 0x2a4a
#define ESP_GATT_UUID_HID_REPORT_MAP 0x2a4b
#define ESP_GATT_UUID_HID_CONTROL_POINT 0x2a4c
#define ESP_GATT_UUID_HID_REPORT 0x2a4d
#define ESP_GATT_CHAR_PROP_BIT_READ (1U << 1)
#define ESP_GATT_CHAR_PROP_BIT_WRITE_NR (1U << 2)
#define ESP_GATT_CHAR_PROP_BIT_WRITE (1U << 3)
#define ESP_GATT_CHAR_PROP_BIT_NOTIFY (1U << 4)
#define ESP_GATT_PERM_READ (1U << 0)
#define ESP_GATT_PERM_READ_ENCRYPTED (1U << 1)
#define ESP_GATT_PERM_WRITE_ENCRYPTED (1U << 5)
#define ESP_GATT_AUTO_RSP 1
#define ESP_GATT_RSP_BY_APP 0
#define ESP_GATT_PREP_WRITE_CANCEL 0
#define ESP_GATT_PREP_WRITE_EXEC 1
#define ESP_LE_AUTH_REQ_SC_BOND 9
#define ESP_LE_AUTH_BOND 1
#define ESP_LE_AUTH_REQ_SC_ONLY 8
#define ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE 1
#define ESP_IO_CAP_NONE 3
#define ESP_BLE_ENC_KEY_MASK 1
#define ESP_BLE_ID_KEY_MASK 2
#define ESP_BLE_ADV_FLAG_GEN_DISC 2
#define ESP_BLE_ADV_FLAG_BREDR_NOT_SPT 4
#define ADV_TYPE_IND 0
#define BLE_ADDR_TYPE_PUBLIC 0
#define BLE_ADDR_TYPE_RANDOM 1
#define ADV_CHNL_ALL 7
#define ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY 0
#define ESP_BT_STATUS_SUCCESS 0

typedef uint8_t esp_bd_addr_t[ESP_BD_ADDR_LEN];
typedef uint8_t esp_gatt_if_t;
typedef enum {
    ESP_GATT_OK = 0,
    ESP_GATT_READ_NOT_PERMIT = 2,
    ESP_GATT_WRITE_NOT_PERMIT = 3,
    ESP_GATT_REQ_NOT_SUPPORTED = 6,
    ESP_GATT_INVALID_OFFSET = 7,
    ESP_GATT_INSUF_AUTHENTICATION = 5,
    ESP_GATT_INSUF_RESOURCE = 0x11,
    ESP_GATT_INVALID_ATTR_LEN = 0x0d,
    ESP_GATT_ERROR = 0x85,
    ESP_GATT_CCC_CFG_ERR = 0xfd,
    ESP_GATT_OUT_OF_RANGE = 0xff
} esp_gatt_status_t;

typedef struct {
    struct { uint8_t auto_rsp; } attr_control;
    struct {
        uint16_t uuid_length;
        uint8_t *uuid_p;
        uint16_t perm;
        uint16_t max_length;
        uint16_t length;
        uint8_t *value;
    } att_desc;
} esp_gatts_attr_db_t;
typedef struct {
    struct {
        uint8_t value[ESP_GATT_MAX_ATTR_LEN];
        uint16_t handle;
        uint16_t offset;
        uint16_t len;
    } attr_value;
} esp_gatt_rsp_t;

typedef enum {
    ESP_GATTS_REG_EVT = 0,
    ESP_GATTS_READ_EVT = 1,
    ESP_GATTS_WRITE_EVT = 2,
    ESP_GATTS_EXEC_WRITE_EVT = 3,
    ESP_GATTS_CONF_EVT = 5,
    ESP_GATTS_UNREG_EVT = 6,
    ESP_GATTS_START_EVT = 12,
    ESP_GATTS_STOP_EVT = 13,
    ESP_GATTS_CONNECT_EVT = 14,
    ESP_GATTS_DISCONNECT_EVT = 15,
    ESP_GATTS_CONGEST_EVT = 20,
    ESP_GATTS_CREAT_ATTR_TAB_EVT = 22
} esp_gatts_cb_event_t;
typedef union {
    struct { esp_gatt_status_t status; uint16_t app_id; } reg;
    struct {
        uint16_t conn_id;
        uint32_t trans_id;
        esp_bd_addr_t bda;
        uint16_t handle, offset;
        bool is_long, need_rsp;
    } read;
    struct {
        uint16_t conn_id;
        uint32_t trans_id;
        esp_bd_addr_t bda;
        uint16_t handle, offset;
        bool need_rsp, is_prep;
        uint16_t len;
        uint8_t *value;
    } write;
    struct {
        uint16_t conn_id;
        uint32_t trans_id;
        esp_bd_addr_t bda;
        uint8_t exec_write_flag;
    } exec_write;
    struct { esp_gatt_status_t status; uint16_t conn_id, handle; } conf;
    struct { esp_gatt_status_t status; uint16_t service_handle; } start;
    struct { uint16_t conn_id; esp_bd_addr_t remote_bda; uint8_t ble_addr_type; } connect;
    struct { uint16_t conn_id; esp_bd_addr_t remote_bda; } disconnect;
    struct { uint16_t conn_id; bool congested; } congest;
    struct {
        esp_gatt_status_t status;
        uint8_t svc_inst_id;
        uint16_t num_handle;
        uint16_t *handles;
    } add_attr_tab;
} esp_ble_gatts_cb_param_t;
typedef void (*esp_gatts_cb_t)(esp_gatts_cb_event_t, esp_gatt_if_t, esp_ble_gatts_cb_param_t *);

typedef enum {
    ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT = 0,
    ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT = 1,
    ESP_GAP_BLE_ADV_START_COMPLETE_EVT = 6,
    ESP_GAP_BLE_AUTH_CMPL_EVT = 8,
    ESP_GAP_BLE_SEC_REQ_EVT = 10,
    ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT = 17,
    ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT = 23,
    ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT = 27,
    ESP_GAP_BLE_GET_DEV_NAME_COMPLETE_EVT = 60
} esp_gap_ble_cb_event_t;
typedef union {
    struct { int status; char *name; } get_dev_name_cmpl;
    struct { int status; } adv_data_cmpl, scan_rsp_data_cmpl, adv_start_cmpl, adv_stop_cmpl;
    struct { int status; int8_t rssi; esp_bd_addr_t remote_addr; } read_rssi_cmpl;
    struct { int status; esp_bd_addr_t bd_addr; } remove_bond_dev_cmpl;
    union {
        struct { esp_bd_addr_t bd_addr; bool success; uint8_t auth_mode; uint8_t fail_reason; } auth_cmpl;
        struct { esp_bd_addr_t bd_addr; } ble_req;
    } ble_security;
} esp_ble_gap_cb_param_t;
typedef void (*esp_gap_ble_cb_t)(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t *);
typedef enum { ESP_BLE_SEC_ENCRYPT = 1 } esp_ble_sec_act_t;
typedef enum {
    ESP_BLE_SM_AUTHEN_REQ_MODE,
    ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
    ESP_BLE_SM_IOCAP_MODE,
    ESP_BLE_SM_MAX_KEY_SIZE,
    ESP_BLE_SM_SET_INIT_KEY,
    ESP_BLE_SM_SET_RSP_KEY
} esp_ble_sm_param_t;
typedef struct {
    bool set_scan_rsp, include_name;
    uint16_t appearance, service_uuid_len;
    uint8_t *p_service_uuid;
    uint8_t flag;
} esp_ble_adv_data_t;
typedef struct {
    uint16_t adv_int_min, adv_int_max;
    uint8_t adv_type, own_addr_type, channel_map, adv_filter_policy;
} esp_ble_adv_params_t;

esp_err_t esp_ble_gatts_register_callback(esp_gatts_cb_t callback);
esp_err_t esp_ble_gap_get_device_name(void);
esp_err_t esp_ble_gatts_app_register(uint16_t app_id);
esp_err_t esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *, esp_gatt_if_t, uint16_t, uint8_t);
esp_err_t esp_ble_gatts_start_service(uint16_t handle);
esp_err_t esp_ble_gatts_close(esp_gatt_if_t interface, uint16_t conn_id);
esp_err_t esp_ble_gatts_send_response(esp_gatt_if_t, uint16_t, uint32_t, esp_gatt_status_t, esp_gatt_rsp_t *);
esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t, uint16_t, uint16_t, uint16_t, uint8_t *, bool);
esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t callback);
esp_err_t esp_ble_gap_set_security_param(esp_ble_sm_param_t, void *, uint8_t);
esp_err_t esp_ble_gap_set_device_name(const char *name);
esp_err_t esp_ble_gap_config_adv_data(esp_ble_adv_data_t *data);
esp_err_t esp_ble_gap_start_advertising(esp_ble_adv_params_t *parameters);
esp_err_t esp_ble_gap_stop_advertising(void);
esp_err_t esp_ble_set_encryption(esp_bd_addr_t, esp_ble_sec_act_t);
esp_err_t esp_ble_gap_security_rsp(esp_bd_addr_t, bool);

/* Exported bond fields used by production identity derivation. */
typedef struct {
    esp_bd_addr_t bd_addr;
    struct {
        uint8_t key_mask;
        struct { uint8_t ltk[16], rand[8]; uint16_t ediv; uint8_t key_size; } penc_key;
        struct { uint8_t irk[16]; uint8_t addr_type; esp_bd_addr_t static_addr; } pid_key;
    } bond_key;
    uint8_t bd_addr_type;
} esp_ble_bond_dev_t;
int esp_ble_get_bond_device_num(void);
esp_err_t esp_ble_get_bond_device_list(int *, esp_ble_bond_dev_t *);

esp_err_t esp_ble_gap_read_rssi(esp_bd_addr_t address);
esp_err_t esp_ble_remove_bond_device(esp_bd_addr_t address);
