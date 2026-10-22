// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Internal, single BLE control-worker owner. No function here runs in a BLE
 * callback. NVS stores fingerprints and real subscription bits, never keys. */
#include "nvs.h"
#include "psa/crypto.h"
#include "esp_attr.h"

#define SUBSCRIPTION_CAPACITY BLE_HID_MAX_BONDS
#define SUBSCRIPTION_DIGEST_LENGTH 32U
#define SUBSCRIPTION_RECORD_LENGTH 33U
#define SUBSCRIPTION_BLOB_LENGTH (4U + SUBSCRIPTION_CAPACITY * SUBSCRIPTION_RECORD_LENGTH)

typedef struct {
    uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH];
    uint8_t mask;
} subscription_record_t;

/* Task-owned data, never a DMA buffer or an ISR dependency. Keep the worker
 * stack internal for NVS; raw exported bond keys are still cleared after use. */
static struct {
    bool loaded;
    uint8_t count;
    subscription_record_t records[SUBSCRIPTION_CAPACITY];
    subscription_record_t candidate[SUBSCRIPTION_CAPACITY];
    uint8_t blob[SUBSCRIPTION_BLOB_LENGTH];
    esp_ble_bond_dev_t bonds[SUBSCRIPTION_CAPACITY];
    uint8_t live_fingerprints[SUBSCRIPTION_CAPACITY][SUBSCRIPTION_DIGEST_LENGTH];
    bool live_valid[SUBSCRIPTION_CAPACITY];
    int live_count;
} subscriptions EXT_RAM_BSS_ATTR;

static void subscription_clear_bonds(void)
{
    volatile uint8_t *clear = (volatile uint8_t *)subscriptions.bonds;
    for (size_t i = 0; i < sizeof(subscriptions.bonds); ++i) {
        clear[i] = 0;
    }
}

static esp_err_t subscription_load(void)
{
    if (subscriptions.loaded) {
        return ESP_OK;
    }
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hid_cccd", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        subscriptions.loaded = true;
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    size_t length = sizeof(subscriptions.blob);
    error = nvs_get_blob(handle, "subscriptions", subscriptions.blob, &length);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        subscriptions.loaded = true;
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    uint8_t *blob = subscriptions.blob;
    if (length < 4 || blob[0] != 'H' || blob[1] != 'C' || blob[2] != 1 ||
        blob[3] > SUBSCRIPTION_CAPACITY ||
        length != 4U + (size_t)blob[3] * SUBSCRIPTION_RECORD_LENGTH) {
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned int i = 0; i < blob[3]; ++i) {
        uint8_t *record = blob + 4U + i * SUBSCRIPTION_RECORD_LENGTH;
        if (record[SUBSCRIPTION_DIGEST_LENGTH] > 3) {
            return ESP_ERR_INVALID_STATE;
        }
        for (unsigned int j = 0; j < i; ++j) {
            if (memcmp(record, subscriptions.records[j].fingerprint,
                       SUBSCRIPTION_DIGEST_LENGTH) == 0) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        memcpy(subscriptions.records[i].fingerprint, record, SUBSCRIPTION_DIGEST_LENGTH);
        subscriptions.records[i].mask = record[SUBSCRIPTION_DIGEST_LENGTH];
    }
    subscriptions.count = blob[3];
    subscriptions.loaded = true;
    return ESP_OK;
}

static esp_err_t subscription_fingerprint(const esp_ble_bond_dev_t *bond,
                                         uint8_t output[SUBSCRIPTION_DIGEST_LENGTH])
{
    if (!(bond->bond_key.key_mask & ESP_BLE_ENC_KEY_MASK) ||
        bond->bond_key.penc_key.key_size < 7 || bond->bond_key.penc_key.key_size > 16 ||
        bond->bd_addr_type > 3) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Encode fields explicitly: no compiler padding or transient on-air RPA.
     * The SDK bond index is stable after its RPA-to-pseudo resolution. LTK is
     * mandatory so a re-pair with the same identity/IRK cannot inherit CCCDs. */
    uint8_t material[64] = {0};
    size_t length = 0;
    material[length++] = 1; /* Stored HID schema: report IDs 1 keyboard, 2 mouse. */
    memcpy(material + length, bond->bd_addr, ESP_BD_ADDR_LEN);
    length += ESP_BD_ADDR_LEN;
    material[length++] = (uint8_t)bond->bd_addr_type;
    memcpy(material + length, bond->bond_key.penc_key.ltk, 16);
    length += 16;
    memcpy(material + length, bond->bond_key.penc_key.rand, 8);
    length += 8;
    material[length++] = (uint8_t)bond->bond_key.penc_key.ediv;
    material[length++] = (uint8_t)(bond->bond_key.penc_key.ediv >> 8);
    material[length++] = bond->bond_key.penc_key.key_size;
    bool identity = (bond->bond_key.key_mask & ESP_BLE_ID_KEY_MASK) != 0;
    material[length++] = identity ? 1 : 0;
    if (identity) {
        memcpy(material + length, bond->bond_key.pid_key.irk, 16);
        length += 16;
        memcpy(material + length, bond->bond_key.pid_key.static_addr, ESP_BD_ADDR_LEN);
        length += ESP_BD_ADDR_LEN;
        material[length++] = (uint8_t)bond->bond_key.pid_key.addr_type;
    }
    size_t produced = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, material, length,
        output, SUBSCRIPTION_DIGEST_LENGTH, &produced);
    /* Do not keep exported key material after deriving a non-secret identity. */
    volatile uint8_t *clear = material;
    for (size_t i = 0; i < sizeof(material); ++i) {
        clear[i] = 0;
    }
    return status == PSA_SUCCESS && produced == SUBSCRIPTION_DIGEST_LENGTH ? ESP_OK : ESP_FAIL;
}

static esp_err_t subscription_bond(const esp_bd_addr_t address,
                                   uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH])
{
    int count = esp_ble_get_bond_device_num();
    if (count < 0 || count > (int)SUBSCRIPTION_CAPACITY) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(subscriptions.bonds, 0, sizeof(subscriptions.bonds));
    esp_err_t error = esp_ble_get_bond_device_list(&count, subscriptions.bonds);
    if (error != ESP_OK || count < 0 || count > (int)SUBSCRIPTION_CAPACITY) {
        subscription_clear_bonds();
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    subscriptions.live_count = count;
    memset(subscriptions.live_valid, 0, sizeof(subscriptions.live_valid));
    bool found = false;
    for (int i = 0; i < count; ++i) {
        esp_ble_bond_dev_t *bond = &subscriptions.bonds[i];
        esp_err_t digest_error = subscription_fingerprint(bond, subscriptions.live_fingerprints[i]);
        subscriptions.live_valid[i] = digest_error == ESP_OK;
        if (memcmp(address, bond->bd_addr, ESP_BD_ADDR_LEN) == 0) {
            error = digest_error;
            if (error == ESP_OK) {
                memcpy(fingerprint, subscriptions.live_fingerprints[i], SUBSCRIPTION_DIGEST_LENGTH);
                found = true;
            }
        }
    }
    subscription_clear_bonds();
    return found ? ESP_OK : (error == ESP_OK ? ESP_ERR_NOT_FOUND : error);
}

static uint8_t subscription_mask(const uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH])
{
    for (unsigned int i = 0; i < subscriptions.count; ++i) {
        if (memcmp(fingerprint, subscriptions.records[i].fingerprint,
                   SUBSCRIPTION_DIGEST_LENGTH) == 0) {
            return subscriptions.records[i].mask;
        }
    }
    return 0;
}

static esp_err_t subscription_save(const uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH],
                                   uint8_t mask)
{
    unsigned int index = subscriptions.count;
    for (unsigned int i = 0; i < subscriptions.count; ++i) {
        if (memcmp(fingerprint, subscriptions.records[i].fingerprint,
                   SUBSCRIPTION_DIGEST_LENGTH) == 0) {
            if (subscriptions.records[i].mask == mask) {
                return ESP_OK;
            }
            index = i;
            break;
        }
    }
    if (index == SUBSCRIPTION_CAPACITY) {
        /* Only evict a record proven absent from the successfully retrieved
         * current bond list. Unknown fingerprints are never empty evidence. */
        for (unsigned int i = 0; i < subscriptions.count; ++i) {
            bool live = false;
            for (int j = 0; j < subscriptions.live_count; ++j) {
                if (!subscriptions.live_valid[j]) {
                    return ESP_ERR_NOT_SUPPORTED;
                }
                live |= memcmp(subscriptions.records[i].fingerprint,
                    subscriptions.live_fingerprints[j], SUBSCRIPTION_DIGEST_LENGTH) == 0;
            }
            if (!live) {
                index = i;
                break;
            }
        }
        if (index == SUBSCRIPTION_CAPACITY) {
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(subscriptions.candidate, subscriptions.records, sizeof(subscriptions.records));
    memcpy(subscriptions.candidate[index].fingerprint, fingerprint, SUBSCRIPTION_DIGEST_LENGTH);
    subscriptions.candidate[index].mask = mask;
    uint8_t count = subscriptions.count;
    if (index == count) {
        ++count;
    }
    uint8_t *blob = subscriptions.blob;
    blob[0] = 'H'; blob[1] = 'C'; blob[2] = 1; blob[3] = count;
    for (unsigned int i = 0; i < count; ++i) {
        uint8_t *record = blob + 4U + i * SUBSCRIPTION_RECORD_LENGTH;
        memcpy(record, subscriptions.candidate[i].fingerprint, SUBSCRIPTION_DIGEST_LENGTH);
        record[SUBSCRIPTION_DIGEST_LENGTH] = subscriptions.candidate[i].mask;
    }
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hid_cccd", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "subscriptions", blob,
                             4U + (size_t)count * SUBSCRIPTION_RECORD_LENGTH);
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (error == ESP_OK) {
        memcpy(subscriptions.records, subscriptions.candidate, sizeof(subscriptions.records));
        subscriptions.count = count;
    }
    return error;
}

static esp_err_t subscription_forget_one(const uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH])
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < subscriptions.count; ++i) {
        if (memcmp(subscriptions.records[i].fingerprint, fingerprint, SUBSCRIPTION_DIGEST_LENGTH) != 0)
            subscriptions.candidate[count++] = subscriptions.records[i];
    }
    if (count == subscriptions.count) return ESP_OK;
    uint8_t *blob = subscriptions.blob;
    blob[0] = 'H'; blob[1] = 'C'; blob[2] = 1; blob[3] = (uint8_t)count;
    for (unsigned int i = 0; i < count; ++i) {
        uint8_t *record = blob + 4U + i * SUBSCRIPTION_RECORD_LENGTH;
        memcpy(record, subscriptions.candidate[i].fingerprint, SUBSCRIPTION_DIGEST_LENGTH);
        record[SUBSCRIPTION_DIGEST_LENGTH] = subscriptions.candidate[i].mask;
    }
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hid_cccd", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "subscriptions", blob, 4U + (size_t)count * SUBSCRIPTION_RECORD_LENGTH);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error == ESP_OK) {
        uint8_t verified[SUBSCRIPTION_BLOB_LENGTH];
        size_t length = sizeof(verified), expected = 4U + (size_t)count * SUBSCRIPTION_RECORD_LENGTH;
        error = nvs_open("hid_cccd", NVS_READONLY, &handle);
        if (error == ESP_OK) {
            error = nvs_get_blob(handle, "subscriptions", verified, &length);
            nvs_close(handle);
            if (error == ESP_OK && (length != expected || memcmp(blob, verified, expected) != 0)) error = ESP_ERR_INVALID_STATE;
        }
    }
    if (error == ESP_OK) {
        memset(subscriptions.records, 0, sizeof(subscriptions.records));
        memcpy(subscriptions.records, subscriptions.candidate, count * sizeof(*subscriptions.records));
        subscriptions.count = (uint8_t)count;
    }
    return error;
}

static esp_err_t subscription_forget_all(void)
{
    const uint8_t empty[] = {'H', 'C', 1, 0};
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hid_cccd", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "subscriptions", empty, sizeof(empty));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error == ESP_OK) {
        subscriptions.count = 0;
        subscriptions.loaded = true;
        memset(subscriptions.records, 0, sizeof(subscriptions.records));
        memset(subscriptions.candidate, 0, sizeof(subscriptions.candidate));
    }
    return error;
}
