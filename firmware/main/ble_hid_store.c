// SPDX-License-Identifier: Apache-2.0
#include "ble_hid_targets_store.h"
#include "nvs.h"
#include <string.h>

#define TARGET_HEADER_BYTES 56U
#define TARGET_KEY_BYTES 40U
#define TARGET_CRC_BYTES 4U
#define TARGET_BLOB_MAX (TARGET_HEADER_BYTES + BLE_HID_MAX_BONDS * TARGET_KEY_BYTES + TARGET_CRC_BYTES)

/* One worker serializes load/save. Static bounded buffers keep NVS readback
 * off its small task stack; contents are non-secret fingerprints and keys. */
static uint8_t encoded[TARGET_BLOB_MAX], readback[TARGET_BLOB_MAX];
static ble_hid_preferences_t verified;

static uint32_t crc32(const uint8_t *data, size_t bytes)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
    }
    return ~crc;
}

static bool same_key(const ble_hid_target_t *a, const ble_hid_target_t *b)
{
    return a->address_type == b->address_type && memcmp(a->address, b->address, 6) == 0;
}

static bool valid(const ble_hid_preferences_t *value)
{
    if (!value || !value->revision || value->count > BLE_HID_MAX_BONDS ||
        (value->selected && (!value->target.bond_id_known || value->target.address_type > 3))) return false;
    for (unsigned int i = 0; i < value->count; ++i) {
        if (value->order[i].address_type > 3) return false;
        for (unsigned int j = 0; j < i; ++j)
            if (same_key(&value->order[i], &value->order[j])) return false;
    }
    return true;
}

static void put_key(uint8_t *output, const ble_hid_target_t *key)
{
    memcpy(output, key->address, 6); output[6] = key->address_type;
    output[7] = key->bond_id_known ? 1 : 0;
    if (key->bond_id_known) memcpy(output + 8, key->bond_id, BLE_HID_BOND_ID_BYTES);
}

static bool get_key(const uint8_t *input, ble_hid_target_t *key)
{
    memcpy(key->address, input, 6); key->address_type = input[6];
    if (input[7] > 1) return false;
    key->bond_id_known = input[7] != 0;
    if (key->bond_id_known) memcpy(key->bond_id, input + 8, BLE_HID_BOND_ID_BYTES);
    else for (unsigned int i = 8; i < TARGET_KEY_BYTES; ++i) if (input[i]) return false;
    return true;
}

static size_t encode(const ble_hid_preferences_t *value)
{
    size_t bytes = TARGET_HEADER_BYTES + (size_t)value->count * TARGET_KEY_BYTES;
    memset(encoded, 0, bytes + TARGET_CRC_BYTES);
    encoded[0] = 'H'; encoded[1] = 'T'; encoded[2] = 'P'; encoded[3] = 1;
    encoded[4] = value->selected ? 1 : 0; encoded[5] = value->count;
    for (unsigned int i = 0; i < 8; ++i) encoded[8 + i] = (uint8_t)(value->revision >> (8U * i));
    if (value->selected) put_key(encoded + 16, &value->target);
    for (unsigned int i = 0; i < value->count; ++i)
        put_key(encoded + TARGET_HEADER_BYTES + i * TARGET_KEY_BYTES, &value->order[i]);
    uint32_t crc = crc32(encoded, bytes);
    for (unsigned int i = 0; i < 4; ++i) encoded[bytes + i] = (uint8_t)(crc >> (8U * i));
    return bytes + TARGET_CRC_BYTES;
}

static esp_err_t decode(const uint8_t *blob, size_t length, ble_hid_preferences_t *value)
{
    if (length < TARGET_HEADER_BYTES + TARGET_CRC_BYTES || blob[0] != 'H' || blob[1] != 'T' || blob[2] != 'P')
        return ESP_ERR_INVALID_STATE;
    if (blob[3] != 1) return ESP_ERR_NOT_SUPPORTED;
    if (blob[4] > 1 || blob[5] > BLE_HID_MAX_BONDS || blob[6] || blob[7] ||
        length != TARGET_HEADER_BYTES + (size_t)blob[5] * TARGET_KEY_BYTES + TARGET_CRC_BYTES)
        return ESP_ERR_INVALID_STATE;
    size_t bytes = length - TARGET_CRC_BYTES;
    uint32_t stored = 0;
    for (unsigned int i = 0; i < 4; ++i) stored |= (uint32_t)blob[bytes + i] << (8U * i);
    if (stored != crc32(blob, bytes)) return ESP_ERR_INVALID_STATE;
    *value = (ble_hid_preferences_t){.selected = blob[4] != 0, .count = blob[5]};
    for (unsigned int i = 0; i < 8; ++i) value->revision |= (uint64_t)blob[8 + i] << (8U * i);
    if (value->selected) { if (!get_key(blob + 16, &value->target)) return ESP_ERR_INVALID_STATE; }
    else for (unsigned int i = 16; i < TARGET_HEADER_BYTES; ++i) if (blob[i]) return ESP_ERR_INVALID_STATE;
    for (unsigned int i = 0; i < value->count; ++i)
        if (!get_key(blob + TARGET_HEADER_BYTES + i * TARGET_KEY_BYTES, &value->order[i])) return ESP_ERR_INVALID_STATE;
    return valid(value) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t read_record(ble_hid_preferences_t *value, bool *exists, size_t *actual)
{
    *value = (ble_hid_preferences_t){0}; *exists = false;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hid_targets", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    *actual = sizeof(readback);
    error = nvs_get_blob(handle, "preferences", readback, actual);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    error = decode(readback, *actual, value);
    if (error == ESP_OK) *exists = true;
    return error;
}

esp_err_t ble_hid_preferences_load(ble_hid_preferences_t *value, bool *exists)
{
    if (!value || !exists) return ESP_ERR_INVALID_ARG;
    size_t length = 0;
    return read_record(value, exists, &length);
}

esp_err_t ble_hid_preferences_save(const ble_hid_preferences_t *value, bool *uncertain)
{
    if (!uncertain) return ESP_ERR_INVALID_ARG;
    *uncertain = false;
    if (!valid(value)) return ESP_ERR_INVALID_ARG;
    size_t bytes = encode(value);
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hid_targets", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    *uncertain = true;
    error = nvs_set_blob(handle, "preferences", encoded, bytes);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    bool exists; size_t actual = 0;
    error = read_record(&verified, &exists, &actual);
    if (error == ESP_OK && (!exists || actual != bytes || memcmp(encoded, readback, bytes) != 0))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) *uncertain = false;
    return error;
}
