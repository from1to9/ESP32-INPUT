// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Read-only label owner boundary for coordinator and presenter fixtures. */
#include "../../firmware/main/app_display_names.h"
#include <string.h>

bool app_display_names_get(app_display_name_kind_t kind, const uint8_t id[APP_DISPLAY_NAME_ID_BYTES],
    char label[APP_DISPLAY_NAME_CAPACITY])
{
    (void)kind;
    (void)id;
    label[0] = 0;
    return false;
}
bool app_display_name_profile_identity(const char *profile, uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    (void)profile;
    (void)id;
    return false;
}
void app_display_name_classic_identity(const uint8_t address[6], const uint8_t key[APP_DISPLAY_NAME_ID_BYTES],
    uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    (void)address;
    memcpy(id, key, APP_DISPLAY_NAME_ID_BYTES);
}
void app_display_names_scan(const app_wifi_scan_record_t *row, const app_wifi_profiles_snapshot_t *snapshot,
    char label[APP_DISPLAY_NAME_CAPACITY])
{
    (void)row;
    (void)snapshot;
    label[0] = 0;
}
void app_display_name_format_id(const app_display_name_entry_t *entry, char hex[APP_DISPLAY_NAME_ID_CAPACITY])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < APP_DISPLAY_NAME_ID_BYTES; ++i) {
        hex[2U * i] = digits[entry->id[i] >> 4];
        hex[2U * i + 1U] = digits[entry->id[i] & 15U];
    }
    hex[2U * APP_DISPLAY_NAME_ID_BYTES] = 0;
}
