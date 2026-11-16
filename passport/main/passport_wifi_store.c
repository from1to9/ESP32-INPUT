#include "passport_wifi_store.h"
#include <string.h>
#include "nvs.h"

static bool valid(const passport_wifi_store_t *store)
{
    if (store->version != 1 || store->count > PASSPORT_WIFI_PROFILES || !store->next_id) return false;
    bool preferred = store->count == 0 && store->preferred == 0;
    for (unsigned i = 0; i < store->count; ++i) {
        const passport_wifi_record_t *r = &store->records[i];
        if (!r->id || r->id >= store->next_id || !r->ssid_length || r->ssid_length > 32 ||
            memchr(r->ssid, 0, r->ssid_length) || r->security > PW_WPA2 ||
            r->password_length > 63 || r->password[r->password_length] ||
            (r->security == PW_OPEN ? r->password_length != 0 : r->password_length < 8)) return false;
        for (unsigned n = 0; n < r->password_length; ++n) {
            if ((unsigned char)r->password[n] < 32 || (unsigned char)r->password[n] > 126) return false;
        }
        for (unsigned j = 0; j < i; ++j) if (r->id == store->records[j].id) return false;
        preferred |= r->id == store->preferred;
    }
    return preferred;
}

esp_err_t passport_wifi_store_load(passport_wifi_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->version = store->next_id = 1;
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("passport_wifi", NVS_READONLY, &handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc != ESP_OK) return rc;
    size_t size = sizeof(*store);
    rc = nvs_get_blob(handle, "profiles", store, &size);
    nvs_close(handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc != ESP_OK) return rc;
    return size == sizeof(*store) && valid(store) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t passport_wifi_store_save(const passport_wifi_store_t *store)
{
    if (!valid(store)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("passport_wifi", NVS_READWRITE, &handle);
    if (rc != ESP_OK) return rc;
    rc = nvs_set_blob(handle, "profiles", store, sizeof(*store));
    if (rc == ESP_OK) rc = nvs_commit(handle);
    nvs_close(handle);
    return rc;
}
