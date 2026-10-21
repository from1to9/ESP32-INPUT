// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_WIFI_SSID_CAPACITY 33U
#define APP_WIFI_IP_CAPACITY 16U
#define APP_WIFI_SETUP_PASSWORD_CAPACITY 25U
#define APP_WIFI_SETUP_TIMEOUT_MS 600000U
#define APP_WIFI_PASSWORD_CAPACITY 64U
#define APP_WIFI_SCAN_CAPACITY 32U
#define APP_WIFI_SCAN_LABEL_CAPACITY 129U
#define APP_WIFI_SCAN_TIMEOUT_MS 15000U
#define APP_WIFI_SCAN_RESULT_TTL_MS 30000U
#define APP_WIFI_SCAN_COOLDOWN_MS 10000U
#define APP_WIFI_SELECTION_TIMEOUT_MS 300000U

typedef enum {
    APP_WIFI_UNAVAILABLE,
    APP_WIFI_IDLE,
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,
    APP_WIFI_BACKOFF,
    APP_WIFI_ERROR,
    APP_WIFI_OBTAINING_IP
} app_wifi_state_t;

typedef enum {
    APP_WIFI_SCAN_IDLE,
    APP_WIFI_SCAN_QUEUED,
    APP_WIFI_SCAN_RUNNING,
    APP_WIFI_SCAN_CANCELLING,
    APP_WIFI_SCAN_READY,
    APP_WIFI_SCAN_CANCELLED,
    APP_WIFI_SCAN_ERROR,
    APP_WIFI_SCAN_EXPIRED
} app_wifi_scan_state_t;

typedef enum {
    APP_WIFI_SECURITY_OPEN,
    APP_WIFI_SECURITY_WEP,
    APP_WIFI_SECURITY_WPA,
    APP_WIFI_SECURITY_WPA2,
    APP_WIFI_SECURITY_WPA_WPA2,
    APP_WIFI_SECURITY_WPA3,
    APP_WIFI_SECURITY_WPA2_WPA3,
    APP_WIFI_SECURITY_ENTERPRISE,
    APP_WIFI_SECURITY_OWE,
    APP_WIFI_SECURITY_OTHER
} app_wifi_security_t;

typedef enum {
    APP_WIFI_JOIN_AVAILABLE,
    APP_WIFI_JOIN_HIDDEN_SSID,
    APP_WIFI_JOIN_UNSUPPORTED_SSID,
    APP_WIFI_JOIN_UNSUPPORTED_SECURITY
} app_wifi_join_availability_t;

typedef struct {
    app_wifi_scan_state_t state;
    uint32_t generation;
    uint32_t age_ms;
    uint32_t remaining_ms;
    uint32_t retry_in_ms;
    uint16_t count;
    /* Driver retention reached its cap; the true number nearby is unknown. */
    bool capacity_reached;
    bool cleanup_pending;
    esp_err_t error;
    esp_err_t cleanup_error;
} app_wifi_scan_snapshot_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_length;
    /* Valid UTF-8 or an escaped byte label. Treat as text, never HTML/markup. */
    char label[APP_WIFI_SCAN_LABEL_CAPACITY];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi_dbm;
    uint16_t sdk_authmode;
    app_wifi_security_t security;
    app_wifi_join_availability_t availability;
    bool current;
    bool remembered;
} app_wifi_scan_record_t;

typedef struct {
    uint32_t id;
    uint32_t scan_generation;
    uint16_t record_index;
    uint32_t remaining_ms;
    app_wifi_scan_record_t record;
} app_wifi_selection_t;

/* The native editor has a distinct local owner (NULL). Remote reservations
 * carry the authenticated principal/session copied by their management owner. */
typedef struct {
    uint8_t principal_id[16], session_id[16];
} app_wifi_selection_owner_t;

typedef struct {
    char ssid[APP_WIFI_SSID_CAPACITY];
    char password[APP_WIFI_PASSWORD_CAPACITY];
    bool remember;
    /* Without a selection ticket, zero means explicit manual/hidden WPA2 entry;
     * nonzero requires a fresh row. With a ticket retain its original identity. */
    uint32_t scan_generation;
    uint16_t record_index;
    /* Explicit local editor reservation. Nonzero pins the original fresh row
     * for five minutes, independently of later list expiry/replacement. Keep
     * its original scan_generation/index and exact SSID. Never derive from label. */
    uint32_t selection_id;
} app_wifi_join_request_t;

typedef struct {
    bool initialized;
    /* A selected runtime station configuration, not proof of durable storage. */
    bool configured;
    bool setup_active;
    /* Admission is revoked; HTTP/AP shutdown is still incomplete. */
    bool cleanup_pending;
    bool operation_pending;
    bool rssi_valid;
    /* Valid station IPv4, not Internet reachability or a running backend. */
    bool lan_ready;
    /* Actual station HTTPS owner observation, independent from DHCP. */
    bool backend_available;
    esp_err_t backend_error;
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
 * default STA netif; the fallback AP netif is created only on explicit setup.
 * Does not erase NVS, initialize Bluetooth or start AP.
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
/* Coordinator-owned OTA reservation. Copy-only; closes fallback admission,
 * cancels scans and drains already admitted work before flash may be reserved.
 * Read/status and release/stop remain usable. The active STA link stays up. */
void app_wifi_set_maintenance(bool active);
/* Read-model observation from the actual control-runtime owner, not a client
 * declaration. It never changes radio or credential behavior. */
void app_wifi_set_backend_state(bool available, esp_err_t error);

/* Local device provisioning, independent of the optional fallback SoftAP.
 * All admission is nonblocking; only the worker invokes radio/storage APIs.
 * Scan and join are serialized with each other and setup/cleanup. Scan may run
 * on a healthy station link or during backoff, never during association/DHCP.
 * Scan cancellation invalidates access immediately; cleanup waits for the old
 * SDK completion event before permitting new radio work. */
esp_err_t app_wifi_scan_start(void);
esp_err_t app_wifi_scan_cancel(void);
void app_wifi_get_scan_snapshot(app_wifi_scan_snapshot_t *snapshot);
/* One row per copy avoids a multi-KiB UI stack snapshot. Expired/stale or
 * unavailable results fail closed and clear the output. */
esp_err_t app_wifi_get_scan_record(uint32_t generation, uint16_t index,
                                  app_wifi_scan_record_t *record);
/* Reserves one currently fresh/joinable row for one local editor; no SDK work.
 * A new explicit selection replaces only the same owner's unused ticket. No renewal
 * from stale rows. Close/cancel releases it; a stale release cannot revoke a
 * newer selection. Join consumes the ticket into its bounded worker request. */
esp_err_t app_wifi_select_scan_record(uint32_t generation, uint16_t index,
                                     app_wifi_selection_t *selection);
esp_err_t app_wifi_release_selection(uint32_t selection_id);
/* Foreign unexpired reservations cannot be replaced, released or consumed.
 * These copied owner values never establish authentication by themselves. */
esp_err_t app_wifi_select_scan_record_owned(uint32_t generation,uint16_t index,
    const app_wifi_selection_owner_t *,app_wifi_selection_t *);
esp_err_t app_wifi_release_selection_owned(uint32_t selection_id,const app_wifi_selection_owner_t *);
esp_err_t app_wifi_join_owned(const app_wifi_join_request_t *,const app_wifi_selection_owner_t *);
/* Copies the secret into bounded worker-owned memory; caller clears its copy.
 * Join once leaves durable metadata/uncertainty untouched. Remember writes
 * before switching runtime; a failed write never promises rollback. */
esp_err_t app_wifi_join(const app_wifi_join_request_t *request);

void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot);
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info);
