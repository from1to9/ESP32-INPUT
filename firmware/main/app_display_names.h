// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "control_protocol.h"
#include "app_wifi.h"
#include "app_wifi_profiles.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_DISPLAY_NAME_BYTES 48U
#define APP_DISPLAY_NAME_CAPACITY (APP_DISPLAY_NAME_BYTES + 1U)
#define APP_DISPLAY_NAME_AP_LIMIT 16U
#define APP_DISPLAY_NAME_ID_BYTES 32U
#define APP_DISPLAY_NAME_ID_CAPACITY (APP_DISPLAY_NAME_ID_BYTES * 2U + 1U)
#define APP_DISPLAY_NAME_NAMESPACE "display_names"

typedef enum {
    APP_DISPLAY_NAME_INVALID,
    APP_DISPLAY_NAME_HID_BOND,
    APP_DISPLAY_NAME_CLASSIC_BOND,
    APP_DISPLAY_NAME_WIFI_PROFILE,
    APP_DISPLAY_NAME_WIFI_AP
} app_display_name_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t id[APP_DISPLAY_NAME_ID_BYTES];
    char label[APP_DISPLAY_NAME_CAPACITY];
    /* Retained scan rows have no other owner for their original name. */
    uint8_t ssid_length, ssid[32];
} app_display_name_entry_t;

typedef struct {
    uint64_t revision;
    unsigned count, ap_count;
    bool known, cleanup_pending;
} app_display_names_status_t;

/* The control worker alone loads and mutates the catalog. Readers copy
 * entries under a short lock and never perform storage or radio work. */
void app_display_names_load(void);
app_display_names_status_t app_display_names_status(void);
bool app_display_names_get(app_display_name_kind_t kind, const uint8_t id[APP_DISPLAY_NAME_ID_BYTES],
    char label[APP_DISPLAY_NAME_CAPACITY]);
cp_error_t app_display_names_change(const app_display_name_entry_t *entry, uint64_t expected,
    bool clear, uint64_t *revision);
typedef bool (*app_display_names_removed_fn)(void *context, const app_display_name_entry_t *entry);
void app_display_names_cleanup(app_display_names_removed_fn removed, void *context);
cp_error_t app_display_names_read(const char *resource, char *json, size_t capacity, size_t *length);

app_display_name_kind_t app_display_name_parse_kind(const char *kind);
bool app_display_name_parse_id(app_display_name_kind_t kind, const char *text, uint8_t id[APP_DISPLAY_NAME_ID_BYTES]);
bool app_display_name_ap_identity(const uint8_t *ssid, uint8_t ssid_length, const uint8_t bssid[6],
    uint16_t authmode, uint8_t id[APP_DISPLAY_NAME_ID_BYTES]);
bool app_display_name_profile_identity(const char *profile, uint8_t id[APP_DISPLAY_NAME_ID_BYTES]);
void app_display_name_classic_identity(const uint8_t address[6],
    const uint8_t fingerprint[APP_DISPLAY_NAME_ID_BYTES], uint8_t id[APP_DISPLAY_NAME_ID_BYTES]);
void app_display_name_format_id(const app_display_name_entry_t *entry, char hex[APP_DISPLAY_NAME_ID_CAPACITY]);
unsigned app_display_name_scan_profiles(const app_wifi_profiles_snapshot_t *,
    const app_wifi_scan_record_t *, uint8_t profile_id[APP_DISPLAY_NAME_ID_BYTES]);
void app_display_names_scan(const app_wifi_scan_record_t *, const app_wifi_profiles_snapshot_t *,
    char label[APP_DISPLAY_NAME_CAPACITY]);
