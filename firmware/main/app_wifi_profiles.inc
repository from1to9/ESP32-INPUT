// SPDX-License-Identifier: Apache-2.0
/* Included by app_wifi.c: the existing worker remains the sole radio/storage
 * owner. This is a catalog, not a second Wi-Fi task or credential side channel. */
static esp_err_t set_station(const credentials_t *);
#define PROFILE_ENTRY_BYTES 131U
#define PROFILE_HEADER_BYTES 20U
#define PROFILE_BANK_BYTES (PROFILE_HEADER_BYTES + APP_WIFI_PROFILE_CAPACITY * PROFILE_ENTRY_BYTES)
typedef struct {
    char id[33];
    credentials_t credentials;
    bool enabled;
} profile_t;
typedef struct {
    bool known, uncertain;
    uint8_t count;
    uint64_t revision;
    esp_err_t error;
    profile_t items[APP_WIFI_PROFILE_CAPACITY];
} profile_catalog_t;
typedef enum { PROFILE_SAVE, PROFILE_FORGET, PROFILE_REORDER, PROFILE_FORGET_ALL } profile_action_t;
static profile_catalog_t s_profiles;
static uint64_t s_profile_request_counter, s_profile_active_request, s_profile_completed_request;
static esp_err_t s_profile_operation_error;
static app_wifi_profile_result_t s_profile_results[8];
static unsigned s_profile_result_cursor;
static struct {
    profile_action_t action;
    app_wifi_profile_save_t save;
    char id[33], order[APP_WIFI_PROFILE_CAPACITY][APP_WIFI_PROFILE_ID_CAPACITY];
    uint8_t count;
    uint64_t expected_revision;
    bool acknowledge_unknown;
} s_profile_request;
static uint64_t profile_u64(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) {
        n = (n << 8) | p[i];
    }
    return n;
}
static void profile_put64(uint8_t *p, uint64_t n)
{
    for (int i = 7; i >= 0; --i) {
        p[i] = (uint8_t)n;
        n >>= 8;
    }
}
static uint32_t profile_crc(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return ~crc;
}
static bool profile_id_valid(const char *id)
{
    if (!id || strnlen(id, 33) != 32) {
        return false;
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}
static void profile_new_id(char id[33])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        id[2 * i] = hex[bytes[i] >> 4];
        id[2 * i + 1] = hex[bytes[i] & 15];
    }
    id[32] = 0;
}
static void profile_publish(const profile_catalog_t *catalog)
{
    portENTER_CRITICAL(&s_lock);
    s_profiles = *catalog;
    portEXIT_CRITICAL(&s_lock);
}
static esp_err_t profile_bank_read(nvs_handle_t nvs, const char *key, profile_catalog_t *out)
{
    uint8_t blob[PROFILE_BANK_BYTES] = {0};
    size_t length = sizeof(blob);
    esp_err_t e = nvs_get_blob(nvs, key, blob, &length);
    if (e == ESP_OK) {
        uint32_t expected = ((uint32_t)blob[16] << 24) | ((uint32_t)blob[17] << 16) |
                            ((uint32_t)blob[18] << 8) | blob[19];
        memset(blob + 16, 0, 4);
        if (length != sizeof(blob) || memcmp(blob, "WFP1", 4) || !profile_u64(blob + 4) ||
            blob[12] > APP_WIFI_PROFILE_CAPACITY || blob[13] || blob[14] || blob[15] ||
            profile_crc(blob, length) != expected) {
            e = ESP_ERR_INVALID_STATE;
        } else {
            memset(out, 0, sizeof(*out));
            out->known = true;
            out->revision = profile_u64(blob + 4);
            out->count = blob[12];
            for (unsigned i = 0; i < out->count; ++i) {
                const uint8_t *p = blob + PROFILE_HEADER_BYTES + i * PROFILE_ENTRY_BYTES;
                profile_t *v = &out->items[i];
                memcpy(v->id, p, 33);
                v->enabled = p[33] != 0;
                memcpy(v->credentials.ssid, p + 34, 33);
                memcpy(v->credentials.password, p + 67, 64);
                if (!profile_id_valid(v->id) || p[33] > 1 || !memchr(v->credentials.ssid, 0, 33) ||
                    !memchr(v->credentials.password, 0, 64) ||
                    !valid_text(v->credentials.ssid, 32, false) ||
                    !valid_text(v->credentials.password, 63, true) ||
                    strlen(v->credentials.password) < 8) {
                    e = ESP_ERR_INVALID_STATE;
                }
                for (unsigned j = 0; j < i; ++j) {
                    if (!strcmp(v->id, out->items[j].id) ||
                        !strcmp(v->credentials.ssid, out->items[j].credentials.ssid)) {
                        e = ESP_ERR_INVALID_STATE;
                    }
                }
            }
        }
    }
    clear_secret(blob, sizeof(blob));
    return e;
}
static esp_err_t read_credentials(credentials_t *value, bool *known, bool *present)
{
    profile_catalog_t a = {0}, b = {0}, loaded = {0};
    nvs_handle_t nvs = 0;
    esp_err_t opened = nvs_open("app_wifi", NVS_READONLY, &nvs);
    esp_err_t ea = ESP_ERR_NVS_NOT_FOUND, eb = ESP_ERR_NVS_NOT_FOUND;
    if (opened == ESP_OK) {
        ea = profile_bank_read(nvs, "profiles_a", &a);
        eb = profile_bank_read(nvs, "profiles_b", &b);
        nvs_close(nvs);
    } else if (opened != ESP_ERR_NVS_NOT_FOUND) {
        ea = opened;
    }
    esp_err_t error = ESP_OK;
    if (ea == ESP_ERR_NVS_NOT_FOUND && eb == ESP_ERR_NVS_NOT_FOUND) {
        error = read_legacy_credentials(value, known, present);
        loaded.known = *known;
        loaded.uncertain = error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND;
        loaded.error = loaded.uncertain ? error : ESP_OK;
        if (error == ESP_OK) {
            loaded.count = 1;
            loaded.revision = 1;
            loaded.items[0].credentials = *value;
            loaded.items[0].enabled = true;
            strcpy(loaded.items[0].id, "00000000000000000000000000000001");
        }
    } else if ((ea != ESP_OK && ea != ESP_ERR_NVS_NOT_FOUND) ||
               (eb != ESP_OK && eb != ESP_ERR_NVS_NOT_FOUND) ||
               (ea == ESP_OK && eb == ESP_OK && a.revision == b.revision)) {
        error = ea != ESP_OK && ea != ESP_ERR_NVS_NOT_FOUND ? ea
                : eb != ESP_OK                              ? eb
                                                            : ESP_ERR_INVALID_STATE;
        loaded.uncertain = true;
        loaded.error = error;
        *known = *present =
            opened == ESP_OK && (ea == ESP_ERR_INVALID_STATE || eb == ESP_ERR_INVALID_STATE);
    } else {
        loaded = ea == ESP_OK && (eb != ESP_OK || a.revision > b.revision) ? a : b;
        *known = true;
        *present = loaded.count > 0;
        bool selected = false;
        for (unsigned i = 0; i < loaded.count; ++i) {
            if (loaded.items[i].enabled) {
                *value = loaded.items[i].credentials;
                selected = true;
                break;
            }
        }
        if (!selected) {
            error = ESP_ERR_NVS_NOT_FOUND;
        }
    }
    profile_publish(&loaded);
    clear_secret(&a, sizeof(a));
    clear_secret(&b, sizeof(b));
    clear_secret(&loaded, sizeof(loaded));
    return error;
}
static esp_err_t profile_write(profile_catalog_t *catalog, bool erase_legacy)
{
    if (catalog->revision == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    ++catalog->revision;
    uint8_t blob[PROFILE_BANK_BYTES] = {0};
    memcpy(blob, "WFP1", 4);
    profile_put64(blob + 4, catalog->revision);
    blob[12] = catalog->count;
    for (unsigned i = 0; i < catalog->count; ++i) {
        uint8_t *p = blob + PROFILE_HEADER_BYTES + i * PROFILE_ENTRY_BYTES;
        const profile_t *v = &catalog->items[i];
        memcpy(p, v->id, 33);
        p[33] = v->enabled;
        memcpy(p + 34, v->credentials.ssid, 33);
        memcpy(p + 67, v->credentials.password, 64);
    }
    uint32_t crc = profile_crc(blob, sizeof(blob));
    blob[16] = (uint8_t)(crc >> 24);
    blob[17] = (uint8_t)(crc >> 16);
    blob[18] = (uint8_t)(crc >> 8);
    blob[19] = (uint8_t)crc;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open("app_wifi", NVS_READWRITE, &nvs);
    if (e == ESP_OK) {
        e = nvs_set_blob(nvs, (catalog->revision & 1) ? "profiles_a" : "profiles_b", blob,
                         sizeof(blob));
        if (e == ESP_OK && erase_legacy) {
            e = nvs_erase_key(nvs, "network");
            if (e == ESP_ERR_NVS_NOT_FOUND) {
                e = ESP_OK;
            }
        }
        if (e == ESP_OK) {
            e = nvs_commit(nvs);
        }
        if (e == ESP_OK) {
            uint8_t verified[PROFILE_BANK_BYTES] = {0};
            size_t length = sizeof(verified);
            e = nvs_get_blob(nvs, (catalog->revision & 1) ? "profiles_a" : "profiles_b", verified,
                             &length);
            if (e == ESP_OK &&
                (length != sizeof(verified) || memcmp(verified, blob, sizeof(verified)))) {
                e = ESP_ERR_INVALID_STATE;
            }
            clear_secret(verified, sizeof(verified));
            if (e == ESP_OK && erase_legacy) {
                size_t legacy_length = 0;
                esp_err_t absent = nvs_get_blob(nvs, "network", NULL, &legacy_length);
                if (absent != ESP_ERR_NVS_NOT_FOUND) {
                    e = absent == ESP_OK ? ESP_ERR_INVALID_STATE : absent;
                }
            }
        }
        nvs_close(nvs);
    }
    clear_secret(blob, sizeof(blob));
    if (e == ESP_OK) {
        catalog->known = true;
        catalog->uncertain = false;
        catalog->error = ESP_OK;
        profile_publish(catalog);
    } else {
        portENTER_CRITICAL(&s_lock);
        s_profiles.known = false;
        s_profiles.uncertain = true;
        s_profiles.error = e;
        portEXIT_CRITICAL(&s_lock);
    }
    return e;
}
static esp_err_t write_credentials(const credentials_t *value)
{
    profile_catalog_t catalog;
    portENTER_CRITICAL(&s_lock);
    catalog = s_profiles;
    portEXIT_CRITICAL(&s_lock);
    if (value && (!catalog.known || catalog.uncertain)) {
        clear_secret(&catalog, sizeof(catalog));
        return ESP_ERR_INVALID_STATE;
    }
    bool recovering = !catalog.known || catalog.uncertain;
    if (!value) {
        catalog.count = 0;
        clear_secret(catalog.items, sizeof(catalog.items));
    } else {
        unsigned slot = catalog.count;
        for (unsigned i = 0; i < catalog.count; ++i) {
            if (!strcmp(value->ssid, catalog.items[i].credentials.ssid)) {
                slot = i;
                break;
            }
        }
        if (slot == APP_WIFI_PROFILE_CAPACITY) {
            clear_secret(&catalog, sizeof(catalog));
            return ESP_ERR_NO_MEM;
        }
        if (slot == catalog.count) {
            profile_new_id(catalog.items[slot].id);
            ++catalog.count;
        }
        catalog.items[slot].credentials = *value;
        catalog.items[slot].enabled = true;
        profile_t promoted = catalog.items[slot];
        memmove(catalog.items + 1, catalog.items, slot * sizeof(profile_t));
        catalog.items[0] = promoted;
        clear_secret(&promoted, sizeof(promoted));
    }
    esp_err_t e = profile_write(&catalog, !value);
    if (e == ESP_OK && recovering) {
        e = profile_write(&catalog, !value);
    }
    clear_secret(&catalog, sizeof(catalog));
    return e;
}
static esp_err_t service_profile_mutation(void)
{
    profile_catalog_t catalog;
    app_wifi_profile_save_t save;
    profile_action_t action;
    char id[33], order[APP_WIFI_PROFILE_CAPACITY][33];
    uint8_t count;
    uint64_t expected, request_id;
    bool acknowledge;
    portENTER_CRITICAL(&s_lock);
    catalog = s_profiles;
    request_id = s_profile_active_request;
    save = s_profile_request.save;
    action = s_profile_request.action;
    memcpy(id, s_profile_request.id, 33);
    memcpy(order, s_profile_request.order, sizeof(order));
    count = s_profile_request.count;
    expected = s_profile_request.expected_revision;
    acknowledge = s_profile_request.acknowledge_unknown;
    clear_secret(&s_profile_request, sizeof(s_profile_request));
    portEXIT_CRITICAL(&s_lock);
    esp_err_t e = ESP_OK;
    bool join = false, erase_legacy = false, detach = false;
    if ((!catalog.known || catalog.uncertain) && !(action == PROFILE_FORGET_ALL && acknowledge)) {
        e = ESP_ERR_INVALID_STATE;
    }
    if (expected != catalog.revision) {
        e = ESP_ERR_INVALID_STATE;
    }
    if (e == ESP_OK && action == PROFILE_SAVE) {
        unsigned slot = catalog.count;
        if (save.id[0]) {
            for (unsigned i = 0; i < catalog.count; ++i) {
                if (!strcmp(save.id, catalog.items[i].id)) {
                    slot = i;
                    break;
                }
            }
            if (slot == catalog.count) {
                e = ESP_ERR_NOT_FOUND;
            }
        } else {
            for (unsigned i = 0; i < catalog.count; ++i) {
                if (!strcmp(save.ssid, catalog.items[i].credentials.ssid)) {
                    e = ESP_ERR_INVALID_ARG;
                }
            }
        }
        if (slot == APP_WIFI_PROFILE_CAPACITY) {
            e = ESP_ERR_NO_MEM;
        }
        for (unsigned i = 0; i < catalog.count; ++i) {
            if (i != slot && !strcmp(save.ssid, catalog.items[i].credentials.ssid)) {
                e = ESP_ERR_INVALID_ARG;
            }
        }
        if (e == ESP_OK) {
            profile_t *p = &catalog.items[slot];
            if (slot == catalog.count) {
                profile_new_id(p->id);
                ++catalog.count;
            }
            if (!save.keep_password) {
                memcpy(p->credentials.password, save.password, 64);
            } else if (!save.id[0]) {
                e = ESP_ERR_INVALID_ARG;
            }
            memcpy(p->credentials.ssid, save.ssid, 33);
            p->enabled = save.enabled;
            join = save.join;
        }
    } else if (e == ESP_OK && action == PROFILE_FORGET) {
        unsigned slot = catalog.count;
        for (unsigned i = 0; i < catalog.count; ++i) {
            if (!strcmp(id, catalog.items[i].id)) {
                slot = i;
                break;
            }
        }
        if (slot == catalog.count) {
            e = ESP_ERR_NOT_FOUND;
        } else {
            detach = s_snapshot.configured &&
                     !strcmp(s_snapshot.ssid, catalog.items[slot].credentials.ssid);
            credentials_t legacy = {0};
            bool known, present;
            esp_err_t read = read_legacy_credentials(&legacy, &known, &present);
            if (read == ESP_OK && present &&
                !strcmp(legacy.ssid, catalog.items[slot].credentials.ssid)) {
                erase_legacy = true;
            }
            clear_secret(&legacy, sizeof(legacy));
            memmove(catalog.items + slot, catalog.items + slot + 1,
                    (catalog.count - slot - 1) * sizeof(profile_t));
            clear_secret(&catalog.items[--catalog.count], sizeof(profile_t));
        }
    } else if (e == ESP_OK && action == PROFILE_REORDER) {
        profile_t reordered[APP_WIFI_PROFILE_CAPACITY] = {0};
        bool seen[APP_WIFI_PROFILE_CAPACITY] = {0};
        if (count != catalog.count) {
            e = ESP_ERR_INVALID_ARG;
        }
        for (unsigned i = 0; i < count && e == ESP_OK; ++i) {
            unsigned j = 0;
            for (; j < catalog.count; ++j) {
                if (!strcmp(order[i], catalog.items[j].id)) {
                    break;
                }
            }
            if (j == catalog.count || seen[j]) {
                e = ESP_ERR_INVALID_ARG;
            } else {
                seen[j] = true;
                reordered[i] = catalog.items[j];
            }
        }
        if (e == ESP_OK) {
            memcpy(catalog.items, reordered, sizeof(reordered));
        }
        clear_secret(reordered, sizeof(reordered));
    } else if (e == ESP_OK && action == PROFILE_FORGET_ALL) {
        catalog.count = 0;
        clear_secret(catalog.items, sizeof(catalog.items));
        erase_legacy = true;
        detach = true;
    }
    bool attempted_write = e == ESP_OK;
    if (e == ESP_OK) {
        /* Unknown-bank recovery explicitly replaces both keys. No erase of the
         * NVS partition, bonds or unrelated data. A partial ACK remains unknown. */
        bool recovering = !catalog.known || catalog.uncertain;
        e = profile_write(&catalog, erase_legacy);
        if (e == ESP_OK && recovering) {
            e = profile_write(&catalog, erase_legacy);
        }
    }
    esp_err_t persistence_error = e;
    if (e == ESP_OK && detach) {
        e = esp_wifi_disconnect();
        if (e == ESP_ERR_WIFI_NOT_CONNECT) {
            e = ESP_OK;
        }
        esp_err_t cleared = set_station(NULL);
        if (e == ESP_OK) {
            e = cleared;
        }
        s_connect_until = s_retry_at = 0;
        portENTER_CRITICAL(&s_lock);
        if (cleared == ESP_OK) {
            ++s_station_generation;
            s_snapshot.configured = false;
            s_snapshot.ssid[0] = 0;
            s_snapshot.ip[0] = 0;
            s_snapshot.lan_ready = false;
            s_snapshot.rssi_valid = false;
            s_snapshot.state = APP_WIFI_IDLE;
            publish();
        }
        s_snapshot.connection_error = e;
        s_snapshot.lan_ready = false;
        s_snapshot.ip[0] = 0;
        s_snapshot.rssi_valid = false;
        if (e != ESP_OK) {
            s_snapshot.state = APP_WIFI_ERROR;
        }
        publish();
        portEXIT_CRITICAL(&s_lock);
    }
    if (e == ESP_OK && join) {
        unsigned slot = 0;
        for (; slot < catalog.count; ++slot) {
            if (!strcmp(save.ssid, catalog.items[slot].credentials.ssid)) {
                break;
            }
        }
        if (slot < catalog.count) {
            portENTER_CRITICAL(&s_lock);
            s_pending = catalog.items[slot].credentials;
            s_pending_generation = s_generation;
            s_pending_remember = false;
            s_pending_scan_generation = 0;
            s_pending_record_index = 0;
            s_pending_selection_until = 0;
            s_commands |= JOIN_NETWORK;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    credentials_t selected = {0};
    bool known, present;
    esp_err_t verified = persistence_error == ESP_OK ? read_credentials(&selected, &known, &present)
                                                     : persistence_error;
    if (persistence_error == ESP_OK) {
        storage_result(verified, known, present, &selected);
        if (e == ESP_OK && verified != ESP_OK && verified != ESP_ERR_NVS_NOT_FOUND) {
            e = verified;
        }
    } else if (attempted_write) {
        storage_result(persistence_error, false, false, NULL);
    }
    portENTER_CRITICAL(&s_lock);
    s_profile_completed_request = request_id;
    s_profile_operation_error = e;
    s_profile_results[s_profile_result_cursor] =
        (app_wifi_profile_result_t){.request_id = request_id,
                                    .revision = s_profiles.revision,
                                    .error = e,
                                    .storage_known = s_profiles.known,
                                    .storage_uncertain = s_profiles.uncertain};
    s_profile_result_cursor = (s_profile_result_cursor + 1) % 8;
    portEXIT_CRITICAL(&s_lock);
    clear_secret(&selected, sizeof(selected));
    clear_secret(&catalog, sizeof(catalog));
    clear_secret(&save, sizeof(save));
    return e;
}
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->capacity = APP_WIFI_PROFILE_CAPACITY;
    portENTER_CRITICAL(&s_lock);
    out->known = s_profiles.known;
    out->uncertain = s_profiles.uncertain;
    out->revision = s_profiles.revision;
    out->count = s_profiles.known ? s_profiles.count : 0;
    out->error = s_profiles.error;
    out->operation_pending = s_snapshot.operation_pending;
    out->request_id = s_profile_active_request;
    out->completed_request_id = s_profile_completed_request;
    out->operation_error = s_profile_operation_error;
    for (unsigned i = 0; i < out->count; ++i) {
        app_wifi_profile_view_t *v = &out->profiles[i];
        const profile_t *p = &s_profiles.items[i];
        memcpy(v->id, p->id, 33);
        memcpy(v->ssid, p->credentials.ssid, 33);
        v->enabled = p->enabled;
        v->priority = (uint8_t)i;
        v->current = s_snapshot.configured && !strcmp(v->ssid, s_snapshot.ssid);
    }
    portEXIT_CRITICAL(&s_lock);
}
static esp_err_t profile_admit(profile_action_t action, uint64_t expected)
{
    if (s_maintenance || !s_snapshot.initialized || s_snapshot.operation_pending || s_commands ||
        s_snapshot.setup_active || s_snapshot.cleanup_pending || s_scan_wait_event ||
        s_scan.cleanup_pending || s_scan.state == APP_WIFI_SCAN_QUEUED ||
        expected != s_profiles.revision || s_profile_request_counter == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    s_profile_active_request = ++s_profile_request_counter;
    s_profile_request.action = action;
    s_profile_request.expected_revision = expected;
    s_commands |= PROFILE_MUTATION;
    s_snapshot.operation_pending = true;
    publish();
    return ESP_OK;
}
esp_err_t app_wifi_profile_save(const app_wifi_profile_save_t *save)
{
    if (!save || !valid_text(save->ssid, 32, false) ||
        (save->id[0] && !profile_id_valid(save->id)) ||
        (!save->keep_password &&
         (!valid_text(save->password, 63, true) || strlen(save->password) < 8)) ||
        (save->join && !save->enabled)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    esp_err_t e = profile_admit(PROFILE_SAVE, save->expected_revision);
    if (e == ESP_OK) {
        s_profile_request.save = *save;
    }
    portEXIT_CRITICAL(&s_lock);
    return e;
}
esp_err_t app_wifi_profile_join(const char *id, uint64_t expected)
{
    if (!profile_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    app_wifi_join_request_t request = {0};
    portENTER_CRITICAL(&s_lock);
    bool found = false;
    if (s_profiles.known && !s_profiles.uncertain && s_profiles.revision == expected) {
        for (unsigned i = 0; i < s_profiles.count; ++i) {
            if (!strcmp(id, s_profiles.items[i].id) && s_profiles.items[i].enabled) {
                memcpy(request.ssid, s_profiles.items[i].credentials.ssid, 33);
                memcpy(request.password, s_profiles.items[i].credentials.password, 64);
                found = true;
                break;
            }
        }
    }
    /* Exact profile identity/revision and join admission share one atomic
     * manager claim. A completed edit/delete cannot replay copied credentials;
     * a mutation after this claim sees the admitted operation as busy. */
    esp_err_t e = found ? join_locked(&request, NULL) : ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_lock);
    clear_secret(&request, sizeof(request));
    return e;
}
esp_err_t app_wifi_profile_forget(const char *id, uint64_t expected)
{
    if (!profile_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    esp_err_t e = profile_admit(PROFILE_FORGET, expected);
    if (e == ESP_OK) {
        memcpy(s_profile_request.id, id, 33);
    }
    portEXIT_CRITICAL(&s_lock);
    return e;
}
esp_err_t
app_wifi_profiles_reorder(const char ids[APP_WIFI_PROFILE_CAPACITY][APP_WIFI_PROFILE_ID_CAPACITY],
                          uint8_t count, uint64_t expected)
{
    if (!ids || count > APP_WIFI_PROFILE_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!profile_id_valid(ids[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    portENTER_CRITICAL(&s_lock);
    esp_err_t e = profile_admit(PROFILE_REORDER, expected);
    if (e == ESP_OK) {
        s_profile_request.count = count;
        memcpy(s_profile_request.order, ids, sizeof(s_profile_request.order));
    }
    portEXIT_CRITICAL(&s_lock);
    return e;
}
esp_err_t app_wifi_profiles_forget_all_request(uint64_t expected, bool acknowledge,
                                               uint64_t *request_id)
{
    if (request_id) {
        *request_id = 0;
    }
    portENTER_CRITICAL(&s_lock);
    esp_err_t e = profile_admit(PROFILE_FORGET_ALL, expected);
    if (e == ESP_OK) {
        s_profile_request.acknowledge_unknown = acknowledge;
        if (request_id) {
            *request_id = s_profile_active_request;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return e;
}
esp_err_t app_wifi_profiles_forget_all(uint64_t expected, bool acknowledge)
{
    return app_wifi_profiles_forget_all_request(expected, acknowledge, NULL);
}

static bool profile_remembered_locked(const char *ssid)
{
    if (!s_profiles.known || s_profiles.uncertain) {
        return false;
    }
    for (unsigned i = 0; i < s_profiles.count; ++i) {
        if (!strcmp(ssid, s_profiles.items[i].credentials.ssid)) {
            return true;
        }
    }
    return false;
}
static bool profile_capacity_locked(const char *ssid)
{
    return s_profiles.known && !s_profiles.uncertain &&
           (s_profiles.count < APP_WIFI_PROFILE_CAPACITY || profile_remembered_locked(ssid));
}
static struct {
    char id[33];
    uint64_t until;
} s_quarantine[APP_WIFI_PROFILE_CAPACITY];
static void profile_reconnect(uint64_t now, uint16_t reason)
{
    /* One station attempt at a time, saved priority order, five-minute auth
     * quarantine, and the existing capped backoff. Transient join-once networks
     * never silently become saved-catalog reconnect selection. */
    credentials_t next = {0};
    bool found = false;
    uint64_t earliest = UINT64_MAX;
    portENTER_CRITICAL(&s_lock);
    unsigned current = s_profiles.count;
    if (s_profiles.known && !s_profiles.uncertain) {
        for (unsigned i = 0; i < s_profiles.count; ++i) {
            if (!strcmp(s_snapshot.ssid, s_profiles.items[i].credentials.ssid)) {
                current = i;
                break;
            }
        }
    }
    if (current < s_profiles.count) {
        if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            unsigned slot = 0;
            for (; slot < APP_WIFI_PROFILE_CAPACITY; ++slot) {
                if (!s_quarantine[slot].id[0] || s_quarantine[slot].until <= now ||
                    !strcmp(s_quarantine[slot].id, s_profiles.items[current].id)) {
                    break;
                }
            }
            if (slot < APP_WIFI_PROFILE_CAPACITY) {
                memcpy(s_quarantine[slot].id, s_profiles.items[current].id, 33);
                s_quarantine[slot].until = now + 300000;
            }
        }
        for (unsigned step = 1; step <= s_profiles.count; ++step) {
            unsigned i = (current + step) % s_profiles.count;
            if (!s_profiles.items[i].enabled) {
                continue;
            }
            uint64_t until = 0;
            for (unsigned j = 0; j < APP_WIFI_PROFILE_CAPACITY; ++j) {
                if (!strcmp(s_quarantine[j].id, s_profiles.items[i].id)) {
                    until = s_quarantine[j].until;
                }
            }
            if (until > now) {
                if (until < earliest) {
                    earliest = until;
                }
                continue;
            }
            next = s_profiles.items[i].credentials;
            found = i != current;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (found) {
        esp_err_t e = set_station(&next);
        portENTER_CRITICAL(&s_lock);
        if (e == ESP_OK) {
            memcpy(s_snapshot.ssid, next.ssid, sizeof(next.ssid));
            ++s_station_generation;
            publish();
        } else {
            s_snapshot.connection_error = e;
        }
        portEXIT_CRITICAL(&s_lock);
    } else if (earliest != UINT64_MAX && earliest > s_retry_at) {
        s_retry_at = earliest;
    }
    clear_secret(&next, sizeof(next));
}

esp_err_t app_wifi_profile_result(uint64_t id, app_wifi_profile_result_t *out)
{
    if (!id || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    esp_err_t error = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < 8; ++i) {
        if (s_profile_results[i].request_id == id) {
            *out = s_profile_results[i];
            error = ESP_OK;
            break;
        }
    }
    if (error != ESP_OK && id == s_profile_active_request && id != s_profile_completed_request) {
        error = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}
