// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_factory_reset.c"

static struct { const char *partition, *name; unsigned keys; } stores[] = {
    {"nvs", "input_prefs", 2}, {"nvs", "app_wifi", 3}, {"nvs", "hid_targets", 2},
    {"nvs", "hid_cccd", 2}, {"nvs", "bt_config.conf", 4}, {"nvs", "nvs.net80211", 3},
    {"nvs", "phy", 3}, {"control", "ctrl_ota", 1}, {"control", RESET_NAMESPACE, 1},
    {"control", "ctrl_v1", 4}, {"control", "ctrl_access", 4}, {"control", "record_audio", 1},
    {"nvs", "misc", 3},
};
static esp_partition_t partitions[] = {
    {0x9000, 0x6000, 1, 2, "nvs"}, {0xf20000, 0x10000, 1, 2, "control"},
    {0xf30000, 0xd0000, 1, 0x40, "recordings"}
};
struct reset_iterator { unsigned index; const char *partition; };
static struct reset_iterator cursor;
static unsigned calls, fail_at, mutations, marker = 1, handles;
static bool applied_failure, recording_dirty, iterator_live;
/* The pinned SDK iterator omits ItemType::BLOB (0x41), but nvs_open and
 * nvs_erase_all still support its namespace. See nvs_storage.cpp:isIterableItem. */
static bool legacy_only[sizeof(stores) / sizeof(stores[0])];
static bool namespace_present[sizeof(stores) / sizeof(stores[0])];
static esp_err_t init_error, read_error;
static bool boundary(void) { return ++calls == fail_at; }
static esp_err_t mutation(unsigned *value, unsigned next)
{
    bool fail = boundary();
    if (!fail || applied_failure) { *value = next; ++mutations; }
    return fail ? ESP_FAIL : ESP_OK;
}
const esp_partition_t *esp_partition_find_first(unsigned type, unsigned subtype, const char *label)
{
    for (size_t i = 0; i < 3; ++i)
        if (partitions[i].type == type && partitions[i].subtype == subtype && !strcmp(partitions[i].label, label)) return &partitions[i];
    return NULL;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t bytes)
{
    assert(p == &partitions[2] && offset == 0 && bytes == 0xd0000);
    bool fail = boundary();
    if (!fail || applied_failure) { recording_dirty = false; ++mutations; }
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_flash_init(void) { return init_error; }
esp_err_t nvs_flash_init_partition(const char *p) { assert(!strcmp(p, "control")); return init_error; }
esp_err_t nvs_open_from_partition(const char *p, const char *name, int mode, nvs_handle_t *handle)
{
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        if (strcmp(p, stores[i].partition) || strcmp(name, stores[i].name)) continue;
        if (!namespace_present[i]) {
            assert(mode == NVS_READONLY); /* No namespace creation during reset. */
            return ESP_ERR_NVS_NOT_FOUND;
        }
        *handle = i; ++handles; return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value)
{
    assert(h == 8 && !strcmp(key, RESET_KEY));
    if (read_error) return read_error;
    if (!marker) return ESP_ERR_NVS_NOT_FOUND;
    *value = (uint8_t)marker; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{ assert(h == 8 && !strcmp(key, RESET_KEY)); return mutation(&marker, value); }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return boundary() ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; assert(handles); --handles; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{ assert(h == 8 && !strcmp(key, RESET_KEY)); return mutation(&marker, 0); }
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    assert(!iterator_live && h != 6 && h != 7 && h != 8);
    return mutation(&stores[h].keys, 0);
}
static esp_err_t seek(nvs_iterator_t *it)
{
    for (; cursor.index < sizeof(stores) / sizeof(stores[0]); ++cursor.index)
        if (stores[cursor.index].keys && !legacy_only[cursor.index] && !strcmp(cursor.partition, stores[cursor.index].partition)) {
            *it = &cursor; iterator_live = true; return ESP_OK;
        }
    *it = NULL; iterator_live = false; return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_find(const char *p, const char *name, int type, nvs_iterator_t *it)
{ assert(!iterator_live && !name && type == NVS_TYPE_ANY); cursor = (struct reset_iterator){0, p}; return seek(it); }
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info)
{ assert(iterator_live && it == &cursor); strcpy(info->namespace_name, stores[it->index].name); return ESP_OK; }
esp_err_t nvs_entry_next(nvs_iterator_t *it) { ++cursor.index; return seek(it); }
void nvs_release_iterator(nvs_iterator_t it) { (void)it; iterator_live = false; }
static void reset(void)
{
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); ++i) stores[i].keys = 3;
    memset(legacy_only, 0, sizeof(legacy_only));
    memset(namespace_present, 1, sizeof(namespace_present));
    partitions[2].address = 0xf30000;
    calls = fail_at = mutations = handles = 0; marker = 1;
    applied_failure = iterator_live = false; recording_dirty = true;
    init_error = read_error = ESP_OK;
}
static void complete(void)
{
    assert(!marker && !recording_dirty && !handles && !iterator_live);
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); ++i)
        assert(stores[i].keys == ((i == 6 || i == 7 || i == 8) ? 3U : 0U));
}
static void legacy_namespace_cases(void)
{
    for (unsigned legacy = 0; legacy <= sizeof(stores) / sizeof(stores[0]); ++legacy) {
        reset();
        if (legacy < sizeof(stores) / sizeof(stores[0])) legacy_only[legacy] = true;
        else memset(legacy_only, 1, sizeof(legacy_only));
        bool performed;
        assert(app_factory_reset_boot(&performed) == ESP_OK && performed);
        complete();
    }
}
static void missing_control_case(void)
{
    reset(); marker = 0; init_error = ESP_ERR_NOT_FOUND;
    bool performed;
    assert(app_factory_reset_boot(&performed) == ESP_OK && !performed && !mutations && recording_dirty);
    /* Only partition-init NOT_FOUND means an absent optional partition.
     * A marker read failure after successful init is not absence. */
    reset(); read_error = ESP_ERR_NOT_FOUND;
    assert(app_factory_reset_boot(&performed) == ESP_ERR_NOT_FOUND && !performed && !mutations);
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcmp(argv[1], "legacy")) legacy_namespace_cases();
        else { assert(!strcmp(argv[1], "missing_control")); missing_control_case(); }
        return 0;
    }
    assert(argc == 1);
    legacy_namespace_cases();
    missing_control_case();
    reset();
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        if (i == 6 || i == 7 || i == 8) continue;
        namespace_present[i] = false; stores[i].keys = 0;
    }
    bool empty_performed;
    assert(app_factory_reset_boot(&empty_performed) == ESP_OK && empty_performed); complete();
    /* Preserve generic cleanup of additional iterator-visible user namespaces. */
    reset(); const char *name = stores[0].name; stores[0].name = "extra_user";
    assert(app_factory_reset_boot(&empty_performed) == ESP_OK && empty_performed); complete();
    stores[0].name = name;
    bool performed;
    reset(); marker = 0;
    assert(app_factory_reset_boot(&performed) == ESP_OK && !performed && !mutations && recording_dirty);
    reset(); marker = 2;
    assert(app_factory_reset_boot(&performed) != ESP_OK && !performed && !mutations);
    reset(); read_error = ESP_FAIL;
    assert(app_factory_reset_boot(&performed) != ESP_OK && !performed && !mutations);
    reset(); init_error = ESP_FAIL;
    assert(app_factory_reset_boot(&performed) != ESP_OK && !performed && !mutations);
    reset(); partitions[2].address = 0xc20000;
    assert(app_factory_reset_boot(&performed) != ESP_OK && marker && !mutations);
    assert(app_factory_reset_arm() != ESP_OK && !mutations);
    reset(); assert(app_factory_reset_boot(&performed) == ESP_OK && performed); complete();
    unsigned boundaries = calls;
    for (unsigned boundary_index = 1; boundary_index <= boundaries; ++boundary_index) {
        for (unsigned applied = 0; applied < 2; ++applied) {
            reset(); fail_at = boundary_index; applied_failure = applied != 0;
            assert(app_factory_reset_boot(&performed) == ESP_FAIL && !performed && !handles && !iterator_live);
            /* Completion may have cleared the marker before a failing commit;
             * only then may the next boot run without retrying. */
            if (!marker) complete();
            fail_at = 0;
            assert(app_factory_reset_boot(&performed) == ESP_OK); complete();
        }
    }
    reset(); marker = 0; assert(app_factory_reset_arm() == ESP_OK && marker == 1 && recording_dirty);
    assert(app_factory_reset_boot(&performed) == ESP_OK && performed); complete();
    for (unsigned i = 1; i <= 2; ++i) {
        reset(); marker = 0; fail_at = i;
        assert(app_factory_reset_arm() == ESP_FAIL && recording_dirty && !handles);
    }
    printf("Reset storage: ordinary boot preservation, owned partitions, retained PHY/OTA, all user stores, %u interruption/failure boundaries and retry passed.\n", boundaries * 2);
}
