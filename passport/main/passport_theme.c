#include "passport_theme.h"
#include "nvs.h"
#include <stdint.h>

esp_err_t passport_theme_load(passport_theme_t *theme)
{
    *theme = PASSPORT_DARK;
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("passport_ui", NVS_READONLY, &handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc != ESP_OK) return rc;
    uint8_t value = PASSPORT_DARK;
    rc = nvs_get_u8(handle, "theme", &value);
    nvs_close(handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc != ESP_OK) return rc;
    if (value > PASSPORT_LIGHT) return ESP_ERR_INVALID_STATE;
    *theme = (passport_theme_t)value;
    return ESP_OK;
}

esp_err_t passport_theme_save(passport_theme_t theme)
{
    if (theme != PASSPORT_DARK && theme != PASSPORT_LIGHT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("passport_ui", NVS_READWRITE, &handle);
    if (rc != ESP_OK) return rc;
    rc = nvs_set_u8(handle, "theme", (uint8_t)theme);
    if (rc == ESP_OK) rc = nvs_commit(handle);
    nvs_close(handle);
    return rc;
}
