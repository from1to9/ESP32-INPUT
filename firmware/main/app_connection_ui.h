// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_SAVED_WIFI_ROWS 8U
#define APP_API_REVIEW_PAGES 64U
/* Copied safe display metadata only. No password, client secret, import token,
 * certificate private key or original USB package enters the UI snapshot. */
typedef enum { APP_API_WINDOW_OPEN, APP_API_WINDOW_CLOSE, APP_API_REVIEW_PAGE,
    APP_API_REVIEW_APPROVE } app_api_operation_t;
typedef struct {
    app_api_operation_t operation;
    uint64_t revision;
    unsigned page;
    char review_digest[65];
} app_api_request_t;
typedef struct {
    bool known, ready, tls, commissioning_open, usb_import_active;
    bool can_open, can_close, remaining_known;
    bool remote_input_allowed; /* Parent-owned physical local admission, independent of TLS. */
    uint64_t remote_input_generation; /* Zero is unknown; rejects a stale held toggle. */
    uint32_t remaining_seconds;
    uint64_t revision;
    char url[256], reason[97];
    bool review_pending, review_complete, can_approve;
    char review_digest[65], review_host[129];
    unsigned review_page, review_page_count;
    char review_pages[APP_API_REVIEW_PAGES][257]; /* Complete owner-formatted permissions. */
} app_api_view_t;
typedef enum { APP_SAVED_WIFI_PAGE, APP_SAVED_WIFI_JOIN, APP_SAVED_WIFI_PREFER,
    APP_SAVED_WIFI_FORGET_REQUEST } app_saved_wifi_operation_t;
typedef struct {
    app_saved_wifi_operation_t operation;
    uint64_t revision;
    unsigned page;
    char id[33];
} app_saved_wifi_request_t;
typedef struct {
    char id[33], label[129];
    bool enabled, current, can_join, can_prefer, can_forget;
    unsigned priority;
} app_saved_wifi_row_t;
typedef struct {
    bool known, uncertain, pending;
    uint64_t revision;
    unsigned page;
    size_t count;
    app_saved_wifi_row_t rows[APP_SAVED_WIFI_ROWS];
    char reason[97];
} app_saved_wifi_view_t;
typedef struct {
    uint8_t address[6], address_type, bond_id[32];
    bool bond_id_known;
} app_bluetooth_target_key_t;
typedef enum { APP_BLUETOOTH_TARGET_DETAILS, APP_BLUETOOTH_TARGET_SELECT,
    APP_BLUETOOTH_TARGET_PREFER, APP_BLUETOOTH_TARGET_FORGET_REQUEST } app_bluetooth_target_operation_t;
typedef struct {
    app_bluetooth_target_operation_t operation;
    uint64_t bond_revision, preferences_revision;
    app_bluetooth_target_key_t target;
} app_bluetooth_target_request_t;
typedef struct {
    bool known, pending, uncertain, selected, can_select, can_prefer, can_forget;
    uint64_t bond_revision, preferences_revision;
    app_bluetooth_target_key_t target;
    char name[49], identity[49], reason[97];
} app_bluetooth_target_view_t;
typedef struct {
    bool known, pending, uncertain, can_add, can_stop, window_open, advertising, connected;
    bool remaining_known;
    uint32_t remaining_seconds;
    char status[49], reason[97], forget_identity[49];
} app_bluetooth_management_view_t;
