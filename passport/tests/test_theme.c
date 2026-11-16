#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../main/passport_theme.c"
static esp_err_t open_error, read_error, write_error, commit_error;
static bool opened;
static int access_mode;
static uint8_t durable, pending;
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!opened && !strcmp(name, "passport_ui"));
    if (open_error) return open_error;
    opened = true; access_mode = mode; *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(opened && handle == 1 && access_mode == NVS_READONLY && !strcmp(key, "theme"));
    if (read_error) return read_error;
    *value = durable;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(opened && handle == 1 && access_mode == NVS_READWRITE && !strcmp(key, "theme"));
    if (write_error) return write_error;
    pending = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(opened && handle == 1);
    if (commit_error) return commit_error;
    durable = pending;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(opened && handle == 1); opened = false; }
int main(void)
{
    passport_theme_t loaded = PASSPORT_LIGHT;
    open_error = ESP_ERR_NVS_NOT_FOUND;
    assert(passport_theme_load(&loaded) == ESP_OK && loaded == PASSPORT_DARK);
    open_error = ESP_FAIL;
    assert(passport_theme_load(&loaded) == ESP_FAIL);
    assert(passport_theme_save(PASSPORT_LIGHT) == ESP_FAIL);
    open_error = 0; read_error = ESP_ERR_NVS_NOT_FOUND;
    assert(passport_theme_load(&loaded) == ESP_OK && !opened);
    read_error = ESP_FAIL;
    assert(passport_theme_load(&loaded) == ESP_FAIL && !opened);
    read_error = 0; durable = 9;
    assert(passport_theme_load(&loaded) == ESP_ERR_INVALID_STATE && loaded == PASSPORT_DARK);
    assert(passport_theme_save((passport_theme_t)9) == ESP_ERR_INVALID_ARG);
    durable = PASSPORT_DARK; write_error = ESP_FAIL;
    assert(passport_theme_save(PASSPORT_LIGHT) == ESP_FAIL && !opened && durable == PASSPORT_DARK);
    write_error = 0; commit_error = ESP_FAIL;
    assert(passport_theme_save(PASSPORT_LIGHT) == ESP_FAIL && !opened);
    commit_error = 0;
    assert(passport_theme_save(PASSPORT_LIGHT) == ESP_OK && !opened);
    assert(passport_theme_load(&loaded) == ESP_OK && loaded == PASSPORT_LIGHT);
    assert(passport_theme_save(PASSPORT_DARK) == ESP_OK);
    assert(passport_theme_load(&loaded) == ESP_OK && loaded == PASSPORT_DARK);
    puts("PASS: theme defaults, restore, invalid data and NVS open/read/write/commit failures");
}
