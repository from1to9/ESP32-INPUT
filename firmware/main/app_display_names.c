// SPDX-License-Identifier: Apache-2.0
#include "app_display_names.h"
#include "control_json.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "nvs.h"
#include "esp_attr.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_NAME_MAGIC UINT32_C(0x444e414d)
#define DISPLAY_NAME_VERSION 1U
#define DISPLAY_NAME_LIMIT (32U + 32U + 8U + APP_DISPLAY_NAME_AP_LIMIT)
#define DISPLAY_NAME_PAGE 8U

typedef struct {
    uint64_t revision;
    uint32_t magic;
    uint16_t version, count;
    app_display_name_entry_t entries[DISPLAY_NAME_LIMIT];
} catalog_t;

static const char *const kinds[] = {"", "hid_bond", "classic_bond", "wifi_profile", "wifi_ap"};
static EXT_RAM_BSS_ATTR catalog_t banks[2];
static EXT_RAM_BSS_ATTR catalog_t readback;
static portMUX_TYPE names_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned active;
static app_display_names_status_t status;
static const char storage_key[] = "catalog";

app_display_name_kind_t app_display_name_parse_kind(const char *kind)
{
    if (kind) for (unsigned i = 1; i < sizeof(kinds) / sizeof(kinds[0]); ++i)
        if (!strcmp(kinds[i], kind)) return (app_display_name_kind_t)i;
    return APP_DISPLAY_NAME_INVALID;
}
static size_t id_size(app_display_name_kind_t kind)
{
    return kind == APP_DISPLAY_NAME_WIFI_PROFILE ? 16U :
        kind > APP_DISPLAY_NAME_INVALID && kind <= APP_DISPLAY_NAME_WIFI_AP ? APP_DISPLAY_NAME_ID_BYTES : 0;
}
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
bool app_display_name_parse_id(app_display_name_kind_t kind, const char *text, uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    size_t size = id_size(kind);
    if (!size || !text || !id || strlen(text) != size * 2U) return false;
    memset(id, 0, APP_DISPLAY_NAME_ID_BYTES);
    for (size_t i = 0; i < size; ++i) {
        int high = hex_digit(text[2U * i]), low = hex_digit(text[2U * i + 1U]);
        if (high < 0 || low < 0) return false;
        id[i] = (uint8_t)(high * 16 + low);
    }
    return true;
}
void app_display_name_format_id(const app_display_name_entry_t *entry, char hex[APP_DISPLAY_NAME_ID_CAPACITY])
{
    static const char digits[] = "0123456789abcdef";
    size_t size = id_size(entry->kind);
    for (size_t i = 0; i < size; ++i) {
        hex[2U * i] = digits[entry->id[i] >> 4];
        hex[2U * i + 1U] = digits[entry->id[i] & 15U];
    }
    hex[size * 2U] = 0;
}
bool app_display_name_ap_identity(const uint8_t *ssid, uint8_t length, const uint8_t bssid[6],
    uint16_t authmode, uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    if (!ssid || !length || length > 32U || memchr(ssid, 0, length) || !bssid || !id) return false;
    uint8_t key[41] = {length, (uint8_t)(authmode >> 8), (uint8_t)authmode};
    memcpy(key + 3, bssid, 6);
    memcpy(key + 9, ssid, length);
    cp_sha256(key, 9U + length, id);
    return true;
}
bool app_display_name_profile_identity(const char *profile, uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    /* WIF1 imports may reuse their synthetic ID. They can acquire a label
     * once a regular profile with a persistent random identity is saved. */
    return profile && strcmp(profile, "00000000000000000000000000000001") &&
        app_display_name_parse_id(APP_DISPLAY_NAME_WIFI_PROFILE, profile, id);
}
void app_display_name_classic_identity(const uint8_t address[6], const uint8_t fingerprint[APP_DISPLAY_NAME_ID_BYTES],
    uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    uint8_t key[6 + APP_DISPLAY_NAME_ID_BYTES];
    memcpy(key, address, 6);
    memcpy(key + 6, fingerprint, APP_DISPLAY_NAME_ID_BYTES);
    cp_sha256(key, sizeof(key), id);
}

unsigned app_display_name_scan_profiles(const app_wifi_profiles_snapshot_t *profiles,
    const app_wifi_scan_record_t *row, uint8_t profile_id[APP_DISPLAY_NAME_ID_BYTES])
{
    /* The saved credential format carries no auth-mode tag. Its Wi-Fi owner
     * currently joins only WPA2 and WPA/WPA2 personal scan rows. */
    if (row->availability != APP_WIFI_JOIN_AVAILABLE || !profiles->known || profiles->uncertain ||
        (row->security != APP_WIFI_SECURITY_WPA2 && row->security != APP_WIFI_SECURITY_WPA_WPA2)) return 0;
    unsigned matches = 0;
    for (unsigned i = 0; i < profiles->count; ++i) {
        const app_wifi_profile_view_t *profile = &profiles->profiles[i];
        if (strnlen(profile->ssid, sizeof(profile->ssid)) != row->ssid_length ||
            memcmp(profile->ssid, row->ssid, row->ssid_length)) continue;
        ++matches;
        if (!app_display_name_profile_identity(profile->id, profile_id)) memset(profile_id, 0, APP_DISPLAY_NAME_ID_BYTES);
    }
    return matches;
}

void app_display_names_scan(const app_wifi_scan_record_t *row, const app_wifi_profiles_snapshot_t *profiles,
    char label[APP_DISPLAY_NAME_CAPACITY])
{
    uint8_t id[APP_DISPLAY_NAME_ID_BYTES] = {0};
    label[0] = 0;
    if (app_display_name_ap_identity((const uint8_t *)row->ssid, row->ssid_length, row->bssid, row->sdk_authmode, id) &&
        app_display_names_get(APP_DISPLAY_NAME_WIFI_AP, id, label)) return;
    if (app_display_name_scan_profiles(profiles, row, id) == 1U)
        (void)app_display_names_get(APP_DISPLAY_NAME_WIFI_PROFILE, id, label);
}

static bool valid_label(const char label[APP_DISPLAY_NAME_CAPACITY])
{
    size_t size = strnlen(label, APP_DISPLAY_NAME_CAPACITY);
    if (!size || size > APP_DISPLAY_NAME_BYTES) return false;
    const uint8_t *text = (const uint8_t *)label;
    unsigned scalars = 0;
    for (size_t i = 0; i < size; ) {
        uint32_t value = text[i++];
        unsigned rest = 0;
        uint32_t minimum = 0;
        if (value >= 0xc2 && value <= 0xdf) { value &= 0x1f; rest = 1; minimum = 0x80; }
        else if (value >= 0xe0 && value <= 0xef) { value &= 0x0f; rest = 2; minimum = 0x800; }
        else if (value >= 0xf0 && value <= 0xf4) { value &= 0x07; rest = 3; minimum = 0x10000; }
        else if (value > 0x7f) return false;
        if (rest > size - i || ++scalars > 16U) return false;
        while (rest--) {
            uint8_t byte = text[i++];
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            value <= 0x1f || (value >= 0x7f && value <= 0x9f) || value == 0x061c ||
            value == 0x200e || value == 0x200f || (value >= 0x202a && value <= 0x202e) ||
            (value >= 0x2066 && value <= 0x2069)) return false;
        lv_font_glyph_dsc_t glyph = {0};
        if (!lv_font_get_glyph_dsc(&lv_font_montserrat_16, &glyph, value, 0) || glyph.is_placeholder) return false;
    }
    return true;
}

static int find(const catalog_t *catalog, app_display_name_kind_t kind, const uint8_t id[APP_DISPLAY_NAME_ID_BYTES])
{
    for (unsigned i = 0; i < catalog->count; ++i)
        if (catalog->entries[i].kind == kind && !memcmp(catalog->entries[i].id, id, APP_DISPLAY_NAME_ID_BYTES))
            return (int)i;
    return -1;
}
static size_t encoded_size(const catalog_t *catalog)
{
    return offsetof(catalog_t, entries) + catalog->count * sizeof(catalog->entries[0]);
}
static unsigned ap_count(const catalog_t *catalog)
{
    unsigned count = 0;
    for (unsigned i = 0; i < catalog->count; ++i) count += catalog->entries[i].kind == APP_DISPLAY_NAME_WIFI_AP;
    return count;
}
static bool valid_catalog(const catalog_t *catalog, size_t size)
{
    if (catalog->magic != DISPLAY_NAME_MAGIC || catalog->version != DISPLAY_NAME_VERSION ||
        catalog->count > DISPLAY_NAME_LIMIT || encoded_size(catalog) != size || ap_count(catalog) > APP_DISPLAY_NAME_AP_LIMIT)
        return false;
    for (unsigned i = 0; i < catalog->count; ++i) {
        const app_display_name_entry_t *entry = &catalog->entries[i];
        if (!id_size(entry->kind) || !valid_label(entry->label) || find(catalog, entry->kind, entry->id) != (int)i) return false;
        if (entry->kind == APP_DISPLAY_NAME_WIFI_AP) {
            if (!entry->ssid_length || entry->ssid_length > sizeof(entry->ssid) ||
                memchr(entry->ssid, 0, entry->ssid_length)) return false;
        } else if (entry->ssid_length) return false;
    }
    return true;
}
static void publish(unsigned bank, bool known, bool cleanup_pending)
{
    const catalog_t *catalog = &banks[bank];
    app_display_names_status_t next = {.known = known, .cleanup_pending = cleanup_pending};
    if (known) {
        next.revision = catalog->revision;
        next.count = catalog->count;
        next.ap_count = ap_count(catalog);
    }
    portENTER_CRITICAL(&names_lock);
    active = bank;
    status = next;
    portEXIT_CRITICAL(&names_lock);
}
void app_display_names_load(void)
{
    unsigned bank = active ^ 1U;
    catalog_t *candidate = &banks[bank];
    memset(candidate, 0, sizeof(*candidate));
    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition("control", APP_DISPLAY_NAME_NAMESPACE, NVS_READONLY, &handle);
    size_t size = sizeof(*candidate);
    if (error == ESP_OK) {
        error = nvs_get_blob(handle, storage_key, candidate, &size);
        nvs_close(handle);
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        candidate->magic = DISPLAY_NAME_MAGIC;
        candidate->version = DISPLAY_NAME_VERSION;
        size = encoded_size(candidate);
        error = ESP_OK;
    }
    publish(bank, error == ESP_OK && valid_catalog(candidate, size), status.cleanup_pending);
}
app_display_names_status_t app_display_names_status(void)
{
    portENTER_CRITICAL(&names_lock);
    app_display_names_status_t copy = status;
    portEXIT_CRITICAL(&names_lock);
    return copy;
}
bool app_display_names_get(app_display_name_kind_t kind, const uint8_t id[APP_DISPLAY_NAME_ID_BYTES],
    char label[APP_DISPLAY_NAME_CAPACITY])
{
    if (!label || !id) return false;
    label[0] = 0;
    portENTER_CRITICAL(&names_lock);
    int index = status.known ? find(&banks[active], kind, id) : -1;
    if (index >= 0) memcpy(label, banks[active].entries[index].label, APP_DISPLAY_NAME_CAPACITY);
    portEXIT_CRITICAL(&names_lock);
    return index >= 0;
}

static cp_error_t save(unsigned bank, bool cleanup)
{
    catalog_t *candidate = &banks[bank];
    size_t size = encoded_size(candidate);
    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition("control", APP_DISPLAY_NAME_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, storage_key, candidate, size);
        if (error == ESP_OK) error = nvs_commit(handle);
        size_t actual = sizeof(readback);
        if (error == ESP_OK) error = nvs_get_blob(handle, storage_key, &readback, &actual);
        if (error == ESP_OK && (actual != size || memcmp(&readback, candidate, size))) error = ESP_FAIL;
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        publish(active, false, cleanup);
        return CP_STORAGE_UNCERTAIN;
    }
    publish(bank, true, false);
    return CP_OK;
}
static void remove_entry(catalog_t *catalog, unsigned index)
{
    --catalog->count;
    memmove(catalog->entries + index, catalog->entries + index + 1U,
        (catalog->count - index) * sizeof(catalog->entries[0]));
}
cp_error_t app_display_names_change(const app_display_name_entry_t *entry, uint64_t expected,
    bool clear, uint64_t *revision)
{
    if (!entry || !revision || !id_size(entry->kind) || (!clear && !valid_label(entry->label))) return CP_INVALID;
    if (!clear && entry->kind == APP_DISPLAY_NAME_WIFI_AP &&
        (!entry->ssid_length || entry->ssid_length > sizeof(entry->ssid) || memchr(entry->ssid, 0, entry->ssid_length)))
        return CP_INVALID;
    if (!status.known || status.revision == UINT64_MAX) return CP_STORAGE_UNCERTAIN;
    if (status.revision != expected) return CP_STALE;
    const catalog_t *current = &banks[active];
    int found = find(current, entry->kind, entry->id);
    if ((clear && found < 0) || (!clear && found >= 0 && !strcmp(entry->label, current->entries[found].label))) {
        *revision = current->revision;
        return CP_OK;
    }
    if (!clear && found < 0 && (current->count == DISPLAY_NAME_LIMIT ||
        (entry->kind == APP_DISPLAY_NAME_WIFI_AP && status.ap_count == APP_DISPLAY_NAME_AP_LIMIT))) return CP_BUSY;
    unsigned bank = active ^ 1U;
    catalog_t *candidate = &banks[bank];
    *candidate = *current;
    if (clear) remove_entry(candidate, (unsigned)found);
    else {
        unsigned index = found >= 0 ? (unsigned)found : candidate->count++;
        candidate->entries[index] = *entry;
    }
    ++candidate->revision;
    cp_error_t result = save(bank, false);
    if (result == CP_OK) *revision = candidate->revision;
    return result;
}
void app_display_names_cleanup(app_display_names_removed_fn removed, void *context)
{
    if (!removed || !status.known || status.revision == UINT64_MAX || !status.count) return;
    unsigned bank = active ^ 1U;
    catalog_t *candidate = &banks[bank];
    *candidate = banks[active];
    for (unsigned i = candidate->count; i > 0; --i)
        if (removed(context, &candidate->entries[i - 1U])) remove_entry(candidate, i - 1U);
    if (candidate->count == status.count) return;
    ++candidate->revision;
    (void)save(bank, true);
}
cp_error_t app_display_names_read(const char *resource, char *json, size_t capacity, size_t *length)
{
    static const char root[] = "/api/v1/display-names";
    if (!resource || !json || !capacity || !length || strncmp(resource, root, sizeof(root) - 1U)) return CP_INVALID;
    const char *page = resource + sizeof(root) - 1U;
    unsigned cursor = 0;
    if (*page) {
        static const char prefix[] = "?cursor=";
        if (strncmp(page, prefix, sizeof(prefix) - 1U)) return CP_INVALID;
        page += sizeof(prefix) - 1U;
        if (*page < '0' || *page > '9') return CP_INVALID;
        while (*page >= '0' && *page <= '9' && cursor <= DISPLAY_NAME_LIMIT) cursor = cursor * 10U + (unsigned)(*page++ - '0');
        if (*page || cursor > DISPLAY_NAME_LIMIT) return CP_INVALID;
    }
    app_display_name_entry_t rows[DISPLAY_NAME_PAGE];
    portENTER_CRITICAL(&names_lock);
    app_display_names_status_t view = status;
    unsigned end = cursor + DISPLAY_NAME_PAGE < view.count ? cursor + DISPLAY_NAME_PAGE : view.count;
    if (cursor < end) memcpy(rows, banks[active].entries + cursor, (end - cursor) * sizeof(rows[0]));
    portEXIT_CRITICAL(&names_lock);
    if (view.known && cursor > view.count) return CP_STALE;
    if (!view.known) {
        int wrote = snprintf(json, capacity,
            "{\"known\":false,\"cleanup_pending\":%s,\"revision\":null,\"ap_capacity\":%u,"
            "\"ap_count\":null,\"labels\":null,\"next_cursor\":0}",
            view.cleanup_pending ? "true" : "false", APP_DISPLAY_NAME_AP_LIMIT);
        if (wrote < 0 || (size_t)wrote >= capacity) return CP_TOO_LARGE;
        *length = (size_t)wrote;
        return CP_OK;
    }
    int wrote = snprintf(json, capacity,
        "{\"known\":%s,\"cleanup_pending\":%s,\"revision\":\"%" PRIu64 "\",\"ap_capacity\":%u,\"ap_count\":%u,\"labels\":[",
        view.known ? "true" : "false", view.cleanup_pending ? "true" : "false", view.revision,
        APP_DISPLAY_NAME_AP_LIMIT, view.ap_count);
    if (wrote < 0 || (size_t)wrote >= capacity) return CP_TOO_LARGE;
    size_t used = (size_t)wrote;
    for (unsigned i = cursor; i < end; ++i) {
        const app_display_name_entry_t *row = &rows[i - cursor];
        char id[APP_DISPLAY_NAME_ID_CAPACITY], value[APP_DISPLAY_NAME_BYTES * 6U + 3U];
        char original[32U * 8U + 3U] = "null";
        if (row->ssid_length) {
            char safe[32U * 4U + 1U] = {0};
            size_t used_text = 0;
            for (unsigned j = 0; j < row->ssid_length; ++j) {
                uint8_t c = row->ssid[j];
                if (c >= 32U && c < 127U && c != '\\') safe[used_text++] = (char)c;
                else {
                    (void)snprintf(safe + used_text, sizeof(safe) - used_text, "\\x%02X", c);
                    used_text += 4U;
                }
            }
            if (!cj_escape(safe, original, sizeof(original))) return CP_INTERNAL;
        }
        app_display_name_format_id(row, id);
        if (!cj_escape(row->label, value, sizeof(value))) return CP_INTERNAL;
        wrote = snprintf(json + used, capacity - used, "%s{\"kind\":\"%s\",\"id\":\"%s\",\"label\":%s,\"original_ssid\":%s}",
            i == cursor ? "" : ",", kinds[row->kind], id, value, original);
        if (wrote < 0 || (size_t)wrote >= capacity - used) return CP_TOO_LARGE;
        used += (size_t)wrote;
    }
    wrote = snprintf(json + used, capacity - used, "],\"next_cursor\":%u}", end < view.count ? end : 0);
    if (wrote < 0 || (size_t)wrote >= capacity - used) return CP_TOO_LARGE;
    *length = used + (size_t)wrote;
    return CP_OK;
}
