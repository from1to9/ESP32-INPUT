#include "passport_names_owner.h"

#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/md.h"
#include "nvs.h"

#define SCHEMA_VERSION 1
#define NAMESPACE "passport_names"
#define WIFI_NAMES (PASSPORT_NAME_CAPACITY_PROFILES + PASSPORT_AP_NAMES)

typedef struct {
    uint32_t version, revision, count;
    passport_name_entry_t entries[];
} name_blob_t;

typedef struct {
    const char *nvs_key;
    passport_name_entry_t *entries;
    unsigned capacity;
    unsigned count;
    uint32_t revision;
    bool known, uncertain;
    bool taken;
    uint32_t ticket, completed;
    passport_name_result_t result;
    passport_name_change_t queued;
} catalog_t;

static passport_name_entry_t ble_entries[PASSPORT_NAME_CAPACITY_BLE], wifi_entries[WIFI_NAMES];
static catalog_t catalogs[] = {
    [PN_BLE] = {.nvs_key = "hid", .entries = ble_entries, .capacity = PASSPORT_NAME_CAPACITY_BLE},
    [PN_WIFI] = {.nvs_key = "wifi", .entries = wifi_entries, .capacity = WIFI_NAMES},
};
static portMUX_TYPE names_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t bond_secret[32];

static passport_name_domain_t key_domain(const passport_name_key_t *key)
{
    return key->kind == PN_HID_BOND ? PN_BLE : PN_WIFI;
}

static bool valid_key(const passport_name_key_t *key)
{
    static const uint8_t zero[PASSPORT_BOND_ID_BYTES];
    switch (key->kind) {
    case PN_HID_BOND: return memcmp(key->bond, zero, sizeof(zero)) != 0;
    case PN_WIFI_PROFILE: return key->profile != 0;
    case PN_SCAN_AP:
        return key->ap.length != 0 && key->ap.length <= sizeof(key->ap.ssid) &&
               memcmp(key->ap.bssid, zero, sizeof(key->ap.bssid)) != 0;
    default: return false;
    }
}

bool passport_names_equal(const passport_name_key_t *a, const passport_name_key_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case PN_HID_BOND: return !memcmp(a->bond, b->bond, sizeof(a->bond));
    case PN_WIFI_PROFILE: return a->profile == b->profile;
    case PN_SCAN_AP:
        return a->ap.length <= sizeof(a->ap.ssid) && a->ap.length == b->ap.length &&
               a->ap.auth == b->ap.auth && !memcmp(a->ap.bssid, b->ap.bssid, sizeof(a->ap.bssid)) &&
               !memcmp(a->ap.ssid, b->ap.ssid, a->ap.length);
    default: return false;
    }
}

static unsigned limit(passport_name_kind_t kind)
{
    switch (kind) {
    case PN_HID_BOND: return PASSPORT_NAME_CAPACITY_BLE;
    case PN_WIFI_PROFILE: return PASSPORT_NAME_CAPACITY_PROFILES;
    case PN_SCAN_AP: return PASSPORT_AP_NAMES;
    default: return 0;
    }
}

static size_t blob_size(unsigned count) { return sizeof(name_blob_t) + count * sizeof(passport_name_entry_t); }

static esp_err_t write_blob(const char *key, const void *data, size_t length)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, data, length);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

static esp_err_t read_blob(const char *key, void *data, size_t *length)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, key, data, length);
        nvs_close(handle);
    }
    return err;
}

static bool valid_blob(const name_blob_t *blob, size_t length, passport_name_domain_t domain)
{
    if (length < sizeof(*blob) || blob->version != SCHEMA_VERSION || !blob->revision ||
        blob->count > catalogs[domain].capacity || length != blob_size(blob->count)) return false;
    unsigned counts[3] = {0};
    for (unsigned i = 0; i < blob->count; ++i) {
        const passport_name_entry_t *entry = &blob->entries[i];
        if (!valid_key(&entry->key) || key_domain(&entry->key) != domain ||
            ++counts[entry->key.kind] > limit(entry->key.kind) ||
            !passport_names_text_valid(entry->label, strnlen(entry->label, sizeof(entry->label)))) return false;
        for (unsigned j = 0; j < i; ++j) {
            if (passport_names_equal(&entry->key, &blob->entries[j].key)) return false;
        }
    }
    return true;
}

static esp_err_t load_bond_secret(bool new_catalog)
{
    size_t length = sizeof(bond_secret);
    esp_err_t err = read_blob("bond_secret", bond_secret, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND && new_catalog) {
        // Called after the BLE controller starts. The secret never leaves this module.
        esp_fill_random(bond_secret, sizeof(bond_secret));
        return write_blob("bond_secret", bond_secret, sizeof(bond_secret));
    }
    return err == ESP_OK && length != sizeof(bond_secret) ? ESP_ERR_INVALID_SIZE : err;
}

bool passport_names_bond_id(const uint8_t *material, size_t length, uint8_t id[PASSPORT_BOND_ID_BYTES])
{
    if (!catalogs[PN_BLE].known) return false;
    uint8_t digest[32];
    int err = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                              bond_secret, sizeof(bond_secret), material, length, digest);
    if (!err) memcpy(id, digest, PASSPORT_BOND_ID_BYTES);
    explicit_bzero(digest, sizeof(digest));
    return err == 0;
}

void passport_names_load(passport_name_domain_t domain)
{
    catalog_t *catalog = &catalogs[domain];
    size_t length = blob_size(catalog->capacity);
    name_blob_t *blob = malloc(length);
    if (!blob) return;
    esp_err_t err = read_blob(catalog->nvs_key, blob, &length);
    bool fresh = err == ESP_ERR_NVS_NOT_FOUND;
    bool known = fresh || (err == ESP_OK && valid_blob(blob, length, domain));
    if (known && domain == PN_BLE) known = load_bond_secret(fresh) == ESP_OK;
    // The catalog isn't visible until the owner publishes the loaded entries.
    if (known) {
        if (!fresh) memcpy(catalog->entries, blob->entries, blob->count * sizeof(blob->entries[0]));
        portENTER_CRITICAL(&names_lock);
        catalog->count = fresh ? 0 : blob->count;
        catalog->revision = fresh ? 1 : blob->revision;
        catalog->uncertain = false;
        catalog->known = true;
        portEXIT_CRITICAL(&names_lock);
    }
    free(blob);
}

passport_name_catalog_t passport_names_catalog(passport_name_domain_t domain)
{
    passport_name_catalog_t info = {0};
    if (domain != PN_BLE && domain != PN_WIFI) return info;
    portENTER_CRITICAL(&names_lock);
    const catalog_t *catalog = &catalogs[domain];
    info.known = catalog->known;
    info.revision = catalog->known ? catalog->revision : 0;
    info.count = catalog->known ? catalog->count : 0;
    portEXIT_CRITICAL(&names_lock);
    info.saved_capacity = domain == PN_BLE ? PASSPORT_NAME_CAPACITY_BLE : PASSPORT_NAME_CAPACITY_PROFILES;
    info.ap_capacity = domain == PN_WIFI ? PASSPORT_AP_NAMES : 0;
    return info;
}

static passport_name_result_t unavailable(const catalog_t *catalog)
{
    return catalog->uncertain ? PN_UNCERTAIN : PN_UNAVAILABLE;
}

passport_name_result_t passport_names_entry(passport_name_domain_t domain, uint32_t revision,
                                            unsigned index, passport_name_entry_t *out)
{
    if ((domain != PN_BLE && domain != PN_WIFI) || !out) return PN_INVALID;
    portENTER_CRITICAL(&names_lock);
    const catalog_t *catalog = &catalogs[domain];
    passport_name_result_t result = !catalog->known ? unavailable(catalog) :
        revision != catalog->revision ? PN_STALE : index >= catalog->count ? PN_NOT_FOUND : PN_OK;
    if (result == PN_OK) *out = catalog->entries[index];
    portEXIT_CRITICAL(&names_lock);
    return result;
}

static int index_of(const catalog_t *catalog, const passport_name_key_t *key)
{
    for (unsigned i = 0; i < catalog->count; ++i) {
        if (passport_names_equal(&catalog->entries[i].key, key)) return (int)i;
    }
    return -1;
}

passport_name_result_t passport_names_lookup(const passport_name_key_t *key, char *label)
{
    label[0] = 0;
    if (!valid_key(key)) return PN_INVALID;
    portENTER_CRITICAL(&names_lock);
    const catalog_t *catalog = &catalogs[key_domain(key)];
    int index = catalog->known ? index_of(catalog, key) : -1;
    if (index >= 0) memcpy(label, catalog->entries[index].label, PASSPORT_NAME_BYTES + 1);
    passport_name_result_t result = !catalog->known ? unavailable(catalog) : index < 0 ? PN_NOT_FOUND : PN_OK;
    portEXIT_CRITICAL(&names_lock);
    return result;
}

static bool valid_change(const passport_name_change_t *change)
{
    return valid_key(&change->key) && (change->clear ||
        passport_names_text_valid(change->label, strnlen(change->label, sizeof(change->label))));
}

passport_name_result_t passport_names_submit(const passport_name_change_t *change, uint32_t *ticket)
{
    if (!change || !ticket || !valid_change(change)) return PN_INVALID;
    portENTER_CRITICAL(&names_lock);
    catalog_t *catalog = &catalogs[key_domain(&change->key)];
    passport_name_result_t result = !catalog->known ? unavailable(catalog) :
        change->revision != catalog->revision ? PN_STALE :
        catalog->ticket == UINT32_MAX ? PN_UNAVAILABLE :
        catalog->ticket != catalog->completed ? PN_BUSY : PN_OK;
    if (result == PN_OK) {
        catalog->queued = *change;
        *ticket = ++catalog->ticket;
    }
    portEXIT_CRITICAL(&names_lock);
    return result;
}

passport_name_result_t passport_names_result(passport_name_domain_t domain, uint32_t ticket)
{
    if ((domain != PN_BLE && domain != PN_WIFI) || !ticket) return PN_INVALID;
    portENTER_CRITICAL(&names_lock);
    const catalog_t *catalog = &catalogs[domain];
    passport_name_result_t result = ticket == catalog->completed ? catalog->result :
        ticket == catalog->ticket ? PN_PENDING : PN_RESULT_UNKNOWN;
    portEXIT_CRITICAL(&names_lock);
    return result;
}

bool passport_names_take(passport_name_domain_t domain, passport_name_change_t *change)
{
    catalog_t *catalog = &catalogs[domain];
    portENTER_CRITICAL(&names_lock);
    bool ready = catalog->ticket != catalog->completed && !catalog->taken;
    if (ready) {
        *change = catalog->queued;
        catalog->taken = true;
    }
    portEXIT_CRITICAL(&names_lock);
    return ready;
}

void passport_names_finish(passport_name_domain_t domain, passport_name_result_t result)
{
    catalog_t *catalog = &catalogs[domain];
    portENTER_CRITICAL(&names_lock);
    catalog->completed = catalog->ticket;
    catalog->result = result;
    catalog->taken = false;
    portEXIT_CRITICAL(&names_lock);
}

static name_blob_t *stage(const catalog_t *catalog)
{
    if (catalog->revision == UINT32_MAX) return NULL;
    name_blob_t *blob = malloc(blob_size(catalog->capacity));
    if (!blob) return NULL;
    blob->version = SCHEMA_VERSION;
    blob->revision = catalog->revision + 1;
    blob->count = catalog->count;
    memcpy(blob->entries, catalog->entries, blob->count * sizeof(blob->entries[0]));
    return blob;
}

static passport_name_result_t save(catalog_t *catalog, name_blob_t *blob)
{
    esp_err_t err = write_blob(catalog->nvs_key, blob, blob_size(blob->count));
    portENTER_CRITICAL(&names_lock);
    if (err == ESP_OK) {
        memcpy(catalog->entries, blob->entries, blob->count * sizeof(blob->entries[0]));
        catalog->count = blob->count;
        catalog->revision = blob->revision;
    } else {
        catalog->known = false;
        catalog->uncertain = true;
    }
    portEXIT_CRITICAL(&names_lock);
    free(blob);
    return err == ESP_OK ? PN_OK : PN_UNCERTAIN;
}

passport_name_result_t passport_names_apply(const passport_name_change_t *change)
{
    catalog_t *catalog = &catalogs[key_domain(&change->key)];
    if (!catalog->known) return unavailable(catalog);
    if (change->revision != catalog->revision) return PN_STALE;
    int index = index_of(catalog, &change->key);
    if (change->clear) return passport_names_remove(&change->key);
    if (index >= 0 && !strcmp(catalog->entries[index].label, change->label)) return PN_OK;
    if (index < 0) {
        unsigned count = 0;
        for (unsigned i = 0; i < catalog->count; ++i) count += catalog->entries[i].key.kind == change->key.kind;
        if (count == limit(change->key.kind)) return PN_FULL;
        index = catalog->count;
    }
    name_blob_t *blob = stage(catalog);
    if (!blob) return PN_UNAVAILABLE;
    if ((unsigned)index == blob->count) ++blob->count;
    passport_name_entry_t *entry = &blob->entries[index];
    memset(entry, 0, sizeof(*entry));
    entry->key.kind = change->key.kind;
    switch (entry->key.kind) {
    case PN_HID_BOND: memcpy(entry->key.bond, change->key.bond, sizeof(entry->key.bond)); break;
    case PN_WIFI_PROFILE: entry->key.profile = change->key.profile; break;
    case PN_SCAN_AP:
        entry->key.ap = change->key.ap;
        memset(entry->key.ap.ssid + entry->key.ap.length, 0, sizeof(entry->key.ap.ssid) - entry->key.ap.length);
        break;
    }
    memcpy(entry->label, change->label, strlen(change->label));
    return save(catalog, blob);
}

passport_name_result_t passport_names_remove(const passport_name_key_t *key)
{
    catalog_t *catalog = &catalogs[key_domain(key)];
    if (!catalog->known) return unavailable(catalog);
    int index = index_of(catalog, key);
    if (index < 0) return PN_OK;
    name_blob_t *blob = stage(catalog);
    if (!blob) return PN_UNAVAILABLE;
    --blob->count;
    memmove(&blob->entries[index], &blob->entries[index + 1], (blob->count - index) * sizeof(blob->entries[0]));
    return save(catalog, blob);
}

passport_name_result_t passport_names_prune(passport_name_domain_t domain,
                                            bool (*retained)(const passport_name_key_t *))
{
    catalog_t *catalog = &catalogs[domain];
    if (!catalog->known) return unavailable(catalog);
    name_blob_t *blob = NULL;
    for (unsigned i = 0, kept = 0; i < catalog->count; ++i) {
        const passport_name_entry_t *entry = &catalog->entries[i];
        if (retained(&entry->key)) {
            if (blob) blob->entries[kept] = *entry;
            ++kept;
        } else {
            if (!blob) blob = stage(catalog);
            if (!blob) return PN_UNAVAILABLE;
            --blob->count;
        }
    }
    return blob ? save(catalog, blob) : PN_OK;
}
