// SPDX-License-Identifier: Apache-2.0
#include "app_factory_reset.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

#define RESET_NAMESPACE "local_reset"
#define RESET_KEY "pending"

/* Match the owned internal stores in partitions.csv. Never infer ownership
 * from a label alone or fall back to an arbitrary data/flash partition. */
static const esp_partition_t *owned_partition(const char *label, unsigned subtype,
                                             uint32_t address, uint32_t size)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, label);
    return p && p->address == address && p->size == size ? p : NULL;
}
static esp_err_t validate_stores(const esp_partition_t **recordings)
{
    if (!owned_partition("nvs", ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000) ||
        !owned_partition("control", ESP_PARTITION_SUBTYPE_DATA_NVS, 0xf20000, 0x10000))
        return ESP_ERR_INVALID_STATE;
    *recordings = owned_partition("recordings", 0x40, 0xf30000, 0xd0000);
    return *recordings ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static esp_err_t marker_read(bool *pending)
{
    *pending = false;
    nvs_handle_t handle;
    esp_err_t e = nvs_open_from_partition("control", RESET_NAMESPACE, NVS_READONLY, &handle);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    uint8_t value = 0;
    e = nvs_get_u8(handle, RESET_KEY, &value);
    nvs_close(handle);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e == ESP_OK && value != 1) return ESP_ERR_INVALID_STATE;
    *pending = e == ESP_OK;
    return e;
}
esp_err_t app_factory_reset_arm(void)
{
    const esp_partition_t *recordings;
    esp_err_t e = validate_stores(&recordings);
    if (e != ESP_OK) return e;
    nvs_handle_t handle;
    e = nvs_open_from_partition("control", RESET_NAMESPACE, NVS_READWRITE, &handle);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(handle, RESET_KEY, 1);
    if (e == ESP_OK) e = nvs_commit(handle);
    nvs_close(handle);
    bool pending = false;
    if (e == ESP_OK) e = marker_read(&pending);
    return e == ESP_OK && !pending ? ESP_ERR_INVALID_STATE : e;
}
static bool retained_namespace(const char *partition, const char *name)
{
    /* Preserve RF calibration and the OTA anti-rollback security floor.
     * Everything else in these firmware-owned stores is user data. */
    return !strcmp(partition, "nvs") ? !strcmp(name, "phy") :
        !strcmp(name, RESET_NAMESPACE) || !strcmp(name, "ctrl_ota");
}
static esp_err_t clear_namespace(const char *partition, const char *name)
{
    nvs_handle_t handle;
    esp_err_t e = nvs_open_from_partition(partition, name, NVS_READWRITE, &handle);
    if (e != ESP_OK) return e;
    e = nvs_erase_all(handle);
    if (e == ESP_OK) e = nvs_commit(handle);
    nvs_close(handle);
    return e;
}
static esp_err_t clear_owned_namespaces(const char *partition)
{
    /* nvs_entry_find omits legacy BLOB entries even though nvs_get_blob still
     * reads them. Clear every current application/SDK user-store namespace by
     * name first; nvs_erase_all covers both legacy and current entry types.
     * Keep this inventory in step with the firmware's user-store owners. */
    static const struct { const char *partition, *name; } owned[] = {
        {"nvs", "input_prefs"}, {"nvs", "app_wifi"}, {"nvs", "hid_targets"},
        {"nvs", "hid_cccd"}, {"nvs", "bt_config.conf"},
        {"nvs", "nvs.net80211"}, {"nvs", "misc"}, /* Both SDK Wi-Fi stores. */
        {"control", "ctrl_v1"}, {"control", "ctrl_access"}, {"control", "record_audio"},
    };
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i) {
        if (strcmp(partition, owned[i].partition)) continue;
        nvs_handle_t handle;
        /* Probe read-only so reset does not create absent namespaces. */
        esp_err_t e = nvs_open_from_partition(partition, owned[i].name, NVS_READONLY, &handle);
        if (e == ESP_ERR_NVS_NOT_FOUND) continue;
        if (e != ESP_OK) return e;
        nvs_close(handle);
        e = clear_namespace(partition, owned[i].name);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
static esp_err_t clear_user_namespaces(const char *partition)
{
    esp_err_t owned_error = clear_owned_namespaces(partition);
    if (owned_error != ESP_OK) return owned_error;
    for (;;) {
        nvs_iterator_t iterator = NULL;
        nvs_entry_info_t info;
        esp_err_t e = nvs_entry_find(partition, NULL, NVS_TYPE_ANY, &iterator);
        while (e == ESP_OK) {
            e = nvs_entry_info(iterator, &info);
            if (e != ESP_OK || !retained_namespace(partition, info.namespace_name)) break;
            e = nvs_entry_next(&iterator);
        }
        nvs_release_iterator(iterator);
        if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
        if (e != ESP_OK) return e;
        e = clear_namespace(partition, info.namespace_name);
        if (e != ESP_OK) return e;
        /* Restart enumeration after mutation. No application worker exists yet.
         * NVS atomic entries and the retained marker make interruption retryable. */
    }
}
esp_err_t app_factory_reset_boot(bool *performed)
{
    if (!performed) return ESP_ERR_INVALID_ARG;
    *performed = false;
    esp_err_t e = nvs_flash_init_partition("control");
    /* SDK partition lookup returns NOT_FOUND only when the optional control
     * partition is absent. Such an old layout cannot contain our reset marker.
     * Initialization corruption and all later marker-read errors still stop boot. */
    if (e == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    bool pending;
    e = marker_read(&pending);
    if (e != ESP_OK || !pending) return e;
    const esp_partition_t *recordings;
    e = validate_stores(&recordings);
    if (e == ESP_OK) e = nvs_flash_init();
    if (e == ESP_OK) e = clear_user_namespaces("nvs");
    if (e == ESP_OK) e = clear_user_namespaces("control");
    if (e == ESP_OK) e = esp_partition_erase_range(recordings, 0, recordings->size);
    if (e != ESP_OK) return e;
    /* Remove authority only after every user store is cleared. A failed or
     * interrupted completion stays blocked and retries on the next boot. */
    nvs_handle_t handle;
    e = nvs_open_from_partition("control", RESET_NAMESPACE, NVS_READWRITE, &handle);
    if (e != ESP_OK) return e;
    e = nvs_erase_key(handle, RESET_KEY);
    if (e == ESP_OK) e = nvs_commit(handle);
    nvs_close(handle);
    if (e == ESP_OK) *performed = true;
    return e;
}
