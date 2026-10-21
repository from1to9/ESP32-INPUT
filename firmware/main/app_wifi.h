// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_WIFI_SSID_CAPACITY 33U
#define APP_WIFI_IP_CAPACITY 16U
#define APP_WIFI_SETUP_PASSWORD_CAPACITY 25U
#define APP_WIFI_SETUP_TIMEOUT_MS 600000U

typedef enum {
    APP_WIFI_UNAVAILABLE,
    APP_WIFI_IDLE,
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,
    APP_WIFI_BACKOFF,
    APP_WIFI_ERROR
} app_wifi_state_t;

typedef struct {
    bool initialized;
    /* A selected runtime station configuration, not proof of durable storage. */
    bool configured;
    bool setup_active;
    /* Admission is revoked; HTTP/AP shutdown is still incomplete. */
    bool cleanup_pending;
    bool operation_pending;
    bool rssi_valid;
    /* Presence is meaningful only while known. These may differ from configured.
     * A corrupt record can be known-present with no usable SSID. */
    bool stored_data_known;
    bool stored_data_present;
    /* Sticky after storage errors until an explicit successful save/forget or
     * read verification. Radio recovery and setup start/stop cannot clear it. */
    bool storage_uncertain;
    app_wifi_state_t state;
    char ssid[APP_WIFI_SSID_CAPACITY];
    /* Empty for absent, unreadable or uncertain storage; never synthesized. */
    char stored_ssid[APP_WIFI_SSID_CAPACITY];
    char ip[APP_WIFI_IP_CAPACITY];
    char setup_ssid[APP_WIFI_SSID_CAPACITY];
    char setup_ip[APP_WIFI_IP_CAPACITY];
    int8_t rssi_dbm;
    uint16_t disconnect_reason;
    uint32_t retry_in_ms;
    uint32_t setup_remaining_ms;
    uint32_t cleanup_retry_in_ms;
    uint32_t revision;
    esp_err_t last_error;
    esp_err_t connection_error;
    esp_err_t cleanup_error;
    esp_err_t storage_error;
} app_wifi_snapshot_t;

/* Trusted local-display data. Never log, export over a network or retain it.
 * The caller must clear this copy when the setup screen closes/expires. */
typedef struct {
    uint32_t generation;
    char ssid[APP_WIFI_SSID_CAPACITY];
    char password[APP_WIFI_SETUP_PASSWORD_CAPACITY];
    char ip[APP_WIFI_IP_CAPACITY];
    uint32_t remaining_ms;
} app_wifi_setup_info_t;

/* Call once from startup, after shared nvs_flash_init(). Owns Wi-Fi and its
 * default STA/AP netifs; does not erase NVS, initialize Bluetooth or start AP.
 * ESP_OK means the low-priority worker was created; inspect initialized and
 * last_error for actual SDK/storage initialization. Reconnects saved WPA2 credentials.
 * Do not call from a Bluetooth, UI, interrupt or sensor callback. */
esp_err_t app_wifi_init(void);

/* Nonblocking local-intent admission only. ESP_OK does not mean completed.
 * Observe operation_pending/setup_active/cleanup_pending/state and error fields.
 * Start is rejected while another start/save is pending or setup is active.
 * Stop/forget take priority and invalidate old HTTP submissions. */
esp_err_t app_wifi_start_setup(void);
esp_err_t app_wifi_stop_setup(void);
/* Explicit recovery is allowed even when configured is false or stored presence
 * is unknown. The caller must not gate this operation on configured alone. */
esp_err_t app_wifi_forget_network(void);
/* Reconcile storage metadata by a bounded read; never switches runtime network. */
esp_err_t app_wifi_verify_storage(void);

void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot);
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info);
