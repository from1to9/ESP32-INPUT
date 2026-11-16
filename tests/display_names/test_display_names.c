// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_display_names.c"

static catalog_t durable, pending;
static size_t durable_size, pending_size;
static bool fail_commit, corrupt_read;
const lv_font_t lv_font_montserrat_16 = {0};
void test_port_enter_critical(portMUX_TYPE *lock) { assert(!*lock); *lock = 1; }
void test_port_exit_critical(portMUX_TYPE *lock) { assert(*lock); *lock = 0; }

bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph, uint32_t value, uint32_t next)
{
    assert(font == &lv_font_montserrat_16 && !next);
    glyph->is_placeholder = value == 0x4e2d;
    return (value >= 0x20 && value <= 0x7e) || value == 0xe9 || glyph->is_placeholder;
}
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "control") && !strcmp(name, APP_DISPLAY_NAME_NAMESPACE));
    *handle = mode;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *size)
{
    (void)handle;
    assert(!strcmp(key, "catalog"));
    if (!durable_size) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= durable_size);
    *size = durable_size;
    memcpy(bytes, &durable, durable_size);
    if (corrupt_read) ((catalog_t *)bytes)->version = 0;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t size)
{
    assert(handle == NVS_READWRITE && !strcmp(key, "catalog") && size <= sizeof(pending));
    memcpy(&pending, bytes, size);
    pending_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == NVS_READWRITE);
    if (fail_commit) return ESP_FAIL;
    durable = pending;
    durable_size = pending_size;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

static bool confirmed_removed(void *context, const app_display_name_entry_t *entry)
{
    return entry->kind == APP_DISPLAY_NAME_HID_BOND && entry->id[0] == *(const uint8_t *)context;
}
int main(void)
{
    app_display_names_load();
    assert(app_display_names_status().known && !app_display_names_status().count);
    app_display_name_entry_t hid = {.kind = APP_DISPLAY_NAME_HID_BOND, .id = {1}, .label = "Studio café"};
    app_display_name_entry_t classic = hid;
    classic.kind = APP_DISPLAY_NAME_CLASSIC_BOND;
    uint64_t revision = 0;
    assert(app_display_names_change(&hid, 0, false, &revision) == CP_OK && revision == 1);
    assert(app_display_names_change(&classic, 0, false, &revision) == CP_STALE);
    assert(app_display_names_change(&classic, 1, false, &revision) == CP_OK && revision == 2);
    uint8_t removed = 1;
    app_display_names_cleanup(confirmed_removed, &removed);
    char label[APP_DISPLAY_NAME_CAPACITY];
    assert(!app_display_names_get(hid.kind, hid.id, label));
    assert(app_display_names_get(classic.kind, classic.id, label) && !strcmp(label, classic.label));
    revision = app_display_names_status().revision;
    const char *invalid[] = {"abcdefghijklmnopq", "\xc0\x80", "\xed\xa0\x80", "\xe2\x80\xae", "\xe4\xb8\xad", "lab\troom"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(hid.label, invalid[i]);
        assert(app_display_names_change(&hid, revision, false, &revision) == CP_INVALID);
    }
    app_display_name_entry_t ap = {.kind = APP_DISPLAY_NAME_WIFI_AP, .ssid_length = 4,
        .ssid = {'W', 'i', 'F', 'i'}, .label = "\"Guest\""};
    uint8_t bssid[6] = {2}, other_auth[APP_DISPLAY_NAME_ID_BYTES], first_id[APP_DISPLAY_NAME_ID_BYTES];
    assert(app_display_name_ap_identity(ap.ssid, ap.ssid_length, bssid, 3, first_id));
    assert(app_display_name_ap_identity(ap.ssid, ap.ssid_length, bssid, 4, other_auth));
    assert(memcmp(first_id, other_auth, sizeof(first_id)));
    uint8_t embedded_zero[] = {1, 0, 2};
    assert(!app_display_name_ap_identity(embedded_zero, sizeof(embedded_zero), bssid, 3, ap.id));
    for (unsigned i = 0; i < APP_DISPLAY_NAME_AP_LIMIT; ++i) {
        bssid[5] = (uint8_t)i;
        assert(app_display_name_ap_identity(ap.ssid, ap.ssid_length, bssid, 3, ap.id));
        assert(app_display_names_change(&ap, revision, false, &revision) == CP_OK);
    }
    memcpy(ap.id, other_auth, sizeof(ap.id));
    assert(app_display_names_change(&ap, revision, false, &revision) == CP_BUSY);
    assert(app_display_names_status().ap_count == APP_DISPLAY_NAME_AP_LIMIT);
    app_display_names_load();
    assert(app_display_names_status().revision == revision && app_display_names_get(ap.kind, first_id, label));
    char json[4096]; size_t bytes;
    assert(app_display_names_read("/api/v1/display-names?cursor=8", json, sizeof(json), &bytes) == CP_OK);
    assert(strstr(json, "\\\"Guest\\\"") && strstr(json, "\"original_ssid\":\"WiFi\"") && strstr(json, "\"next_cursor\":16"));
    memcpy(ap.id, first_id, sizeof(ap.id));
    fail_commit = true;
    assert(app_display_names_change(&ap, revision, true, &revision) == CP_STORAGE_UNCERTAIN);
    assert(app_display_names_read("/api/v1/display-names", json, sizeof(json), &bytes) == CP_OK);
    assert(strstr(json, "\"known\":false") && strstr(json, "\"labels\":null"));
    fail_commit = false;
    app_display_names_load();
    assert(app_display_names_get(ap.kind, first_id, label));
    assert(app_display_names_change(&ap, revision, true, &revision) == CP_OK);
    assert(app_display_names_change(&ap, revision, true, &revision) == CP_OK);
    assert(!app_display_names_get(ap.kind, first_id, label));
    corrupt_read = true;
    app_display_names_load();
    assert(!app_display_names_status().known);
    puts("PASS: persistent label identity, pruning, stale revision, wire escaping, AP capacity, font and read/write failures");
}
