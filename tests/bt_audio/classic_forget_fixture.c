// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bt_classic_forget.h"

typedef uint8_t BD_ADDR[6], LINK_KEY[16];
typedef char bdstr_t[18];
typedef struct { BD_ADDR address; } bt_bdaddr_t;
typedef struct { size_t serialized_size; } config_t;
typedef unsigned nvs_handle_t;
enum { NVS_READONLY = 1, CONFIG_FILE_MAX_SIZE = 1536 };
enum { KEY, TYPE, PIN, SC, FIELDS };
#define CONFIG_FILE_PATH "bt_config.conf"
#define CONFIG_KEY "bt_cfg_key"
#define BTC_STORAGE_LINK_KEY_STR KEY
#define BTC_STORAGE_LINK_KEY_TYPE_STR TYPE
#define BTC_STORAGE_PIN_LENGTH_STR PIN
#define BTC_STORAGE_SC_SUPPORT SC

static const uint8_t selected[6] = {1, 2, 3, 4, 5, 6};
static config_t live_config, *config = &live_config;
static size_t persisted_size;
static bool config_locked, save_fails;
static unsigned writes, nvs_closes;
static esp_err_t read_error;
typedef struct {
    uint8_t le[32];
    LINK_KEY classic;
    unsigned type, pin;
    uint8_t sc;
    bool present[FIELDS];
} saved_peer_t;
static saved_peer_t cache[2], flash[2];

static bool get_config_size(const config_t *current, size_t *size)
{
    assert(config_locked && current == config);
    *size = current->serialized_size;
    return true;
}
static esp_err_t nvs_open(const char *name, unsigned mode, nvs_handle_t *handle)
{
    assert(config_locked && !strcmp(name, CONFIG_FILE_PATH) && mode == NVS_READONLY);
    *handle = 1;
    return ESP_OK;
}
static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    assert(handle == 1 && !strcmp(key, CONFIG_KEY "0") && !out && !*length);
    *length = persisted_size;
    return read_error;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 1); ++nvs_closes; }
static bool config_save(const config_t *current, const char *filename)
{
    assert(config_locked && current == config && !strcmp(filename, CONFIG_FILE_PATH));
    assert(current->serialized_size <= CONFIG_FILE_MAX_SIZE);
    ++writes;
    if (save_fails) return false;
    memcpy(flash, cache, sizeof(flash));
    persisted_size = current->serialized_size - 1;
    return true;
}
static void btc_config_lock(void) { assert(!config_locked); config_locked = true; }
static void btc_config_unlock(void) { assert(config_locked); config_locked = false; }
static void bdaddr_to_string(const bt_bdaddr_t *peer, char *name, size_t size)
{
    assert(!memcmp(peer->address, selected, 6) && size == sizeof(bdstr_t));
    snprintf(name, size, "01:02:03:04:05:06");
}
static saved_peer_t *entry(const char *address)
{
    assert(config_locked && !strcmp(address, "01:02:03:04:05:06"));
    return &cache[0];
}
static bool btc_config_get_bin(const char *address, unsigned field, uint8_t *out, size_t *length)
{
    const saved_peer_t *peer = entry(address);
    assert(field == KEY || field == SC);
    if (!peer->present[field]) return false;
    assert(*length == (field == KEY ? sizeof(peer->classic) : sizeof(peer->sc)));
    memcpy(out, field == KEY ? peer->classic : &peer->sc, *length);
    return true;
}
static bool btc_config_get_int(const char *address, unsigned field, int *out)
{
    const saved_peer_t *peer = entry(address);
    assert(field == TYPE || field == PIN);
    if (!peer->present[field]) return false;
    *out = field == TYPE ? (int)peer->type : (int)peer->pin;
    return true;
}
static bool btc_config_remove(const char *address, unsigned field)
{
    saved_peer_t *peer = entry(address);
    assert(field < FIELDS && peer->present[field]);
    peer->present[field] = false;
    if (field == KEY) memset(peer->classic, 0, sizeof(peer->classic));
    return true;
}
static bool btc_config_set_bin(const char *address, unsigned field, const uint8_t *data, size_t length)
{
    saved_peer_t *peer = entry(address);
    assert(field == KEY || field == SC);
    memcpy(field == KEY ? peer->classic : &peer->sc, data, length);
    peer->present[field] = true;
    return true;
}
static bool btc_config_set_int(const char *address, unsigned field, int value)
{
    saved_peer_t *peer = entry(address);
    assert(field == TYPE || field == PIN);
    *(field == TYPE ? &peer->type : &peer->pin) = (unsigned)value;
    peer->present[field] = true;
    return true;
}
#define PSA_SUCCESS 0
static int psa_crypto_init(void) { return PSA_SUCCESS; }
static void mbedtls_platform_zeroize(void *data, size_t length) { memset(data, 0, length); }
/* Crypto is a boundary substitute. The inputs are synthetic. */
bool bt_classic_bond_id(const uint8_t key[16], bt_classic_bond_id_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->bytes, key, 16);
    return true;
}

#define BT_TRANSPORT_BR_EDR 1
#define BTM_SEC_INVALID_HANDLE UINT16_MAX
#define BTM_PAIR_STATE_IDLE 0
#define BTM_SEC_STATE_IDLE 0
#define BTM_SM4_CONN_PEND 0x40
#define BTM_SEC_AUTHORIZED 0x01
#define BTM_SEC_AUTHENTICATED 0x02
#define BTM_SEC_ENCRYPTED 0x04
#define BTM_SEC_LINK_KEY_KNOWN 0x10
#define BTM_SEC_LINK_KEY_AUTHED 0x20
#define BTM_SEC_16_DIGIT_PIN_AUTHED 0x80
#define BTM_SEC_LE_MASK 0x3f00
#define BT_DEVICE_TYPE_BLE 2
#define SEC_DEV_BDA 0
#define SMP_INCLUDED 1
#define TRUE 1

typedef struct {
    BD_ADDR address;
    LINK_KEY link_key;
    uint16_t sec_flags, hci_handle, ble_hci_handle;
    uint8_t link_key_type, sec_state, sm4, device_type, bond_type;
    uint8_t le[32];
} tBTM_SEC_DEV_REC;
typedef tBTM_SEC_DEV_REC list_node_t;
typedef struct { unsigned type; union { uint8_t *p_bd_addr; } context; } tSecDevContext;
static struct {
    list_node_t *p_sec_dev_rec_list, *p_collided_dev_rec;
    unsigned pairing_state;
    BD_ADDR pairing_bda;
} btm_cb;
static bool controller_store, classic_acl, classic_lcb;
static uintptr_t odm_lk_store_capacity(void) { return controller_store ? 4 : 0; }
static bool BTM_IsAclConnectionUp(const uint8_t *address, int transport)
{ assert(!memcmp(address, selected, 6) && transport == BT_TRANSPORT_BR_EDR); return classic_acl; }
static void *l2cu_find_lcb_by_bd_addr(const uint8_t *address, int transport)
{ assert(!memcmp(address, selected, 6) && transport == BT_TRANSPORT_BR_EDR); return classic_lcb ? &classic_lcb : NULL; }
static bool btm_find_sec_dev_in_list(void *data, void *arg)
{
    const list_node_t *node = data;
    const tSecDevContext *context = arg;
    assert(context->type == SEC_DEV_BDA);
    return memcmp(node->address, context->context.p_bd_addr, 6) != 0;
}
static list_node_t *list_foreach(list_node_t *node, bool (*match)(void *, void *), void *arg)
{ return match(node, arg) ? NULL : node; }
static void *list_node(list_node_t *node) { return node; }
static void list_remove(list_node_t *list, list_node_t *node)
{ assert(list == node); btm_cb.p_sec_dev_rec_list = NULL; }

#include "classic_forget_sdk.inc"

int main(void)
{
    static tBTM_SEC_DEV_REC runtime = {
        .address = {1, 2, 3, 4, 5, 6}, .hci_handle = BTM_SEC_INVALID_HANDLE,
        .ble_hci_handle = 4, .device_type = BT_DEVICE_TYPE_BLE, .bond_type = 2,
        .sec_flags = BTM_SEC_LINK_KEY_KNOWN | BTM_SEC_LINK_KEY_AUTHED | BTM_SEC_LE_MASK,
        .link_key = {42}, .link_key_type = 5, .le = {11, 22, 33},
    };
    btm_cb.p_sec_dev_rec_list = &runtime;
    cache[0] = (saved_peer_t){.le = {11, 22, 33}, .classic = {42}, .type = 5, .pin = 16,
                            .sc = 1, .present = {true, true, true, true}};
    cache[1] = (saved_peer_t){.le = {44}, .classic = {66}, .present = {true, true, true, true}};
    memcpy(flash, cache, sizeof(flash));
    const saved_peer_t other = flash[1];
    const tBTM_SEC_DEV_REC before = runtime;
    live_config.serialized_size = 1536;
    persisted_size = 1535;
    bt_classic_bond_id_t identity;
    assert(bt_classic_forget_review(selected, &identity) == ESP_OK);

    persisted_size = 1536;
    assert(bt_classic_forget_storage(selected, &identity) == ESP_ERR_INVALID_SIZE);
    persisted_size = 1535;
    live_config.serialized_size = 1537;
    assert(bt_classic_forget_storage(selected, &identity) == ESP_ERR_INVALID_SIZE);
    live_config.serialized_size = 1536;
    read_error = ESP_FAIL;
    assert(bt_classic_forget_storage(selected, &identity) == ESP_FAIL);
    read_error = ESP_OK;
    assert(!writes && !memcmp(cache, flash, sizeof(flash)));

    controller_store = true;
    assert(bt_classic_forget_runtime(selected, &identity, false) == ESP_ERR_NOT_SUPPORTED);
    controller_store = false;
    classic_acl = true;
    assert(bt_classic_forget_runtime(selected, &identity, false) == ESP_ERR_INVALID_STATE);
    classic_acl = false;
    classic_lcb = true;
    assert(bt_classic_forget_runtime(selected, &identity, false) == ESP_ERR_INVALID_STATE);
    assert(bt_classic_forget_runtime(selected, &identity, true) == ESP_ERR_INVALID_STATE);
    assert(!writes && !memcmp(&before, &runtime, sizeof(runtime)));
    classic_lcb = false;
    runtime.link_key[0] = 99;
    assert(bt_classic_forget_runtime(selected, &identity, true) == ESP_ERR_INVALID_VERSION);
    runtime.link_key[0] = 42;
    cache[0].classic[0] = 99;
    assert(bt_classic_forget_storage(selected, &identity) == ESP_ERR_INVALID_VERSION && !writes);
    cache[0].classic[0] = 42;

    assert(bt_classic_forget_runtime(selected, &identity, false) == ESP_OK);
    save_fails = true;
    assert(bt_classic_forget_storage(selected, &identity) == ESP_ERR_NOT_FINISHED && writes == 1);
    assert(!memcmp(cache, flash, sizeof(flash)) && !memcmp(&before, &runtime, sizeof(runtime)));
    save_fails = false;
    assert(bt_classic_forget_storage(selected, &identity) == ESP_OK && writes == 2);
    assert(bt_classic_forget_runtime(selected, &identity, true) == ESP_OK);
    assert(btm_cb.p_sec_dev_rec_list == &runtime && runtime.ble_hci_handle == before.ble_hci_handle);
    assert(runtime.bond_type == before.bond_type && runtime.sec_flags == BTM_SEC_LE_MASK);
    assert(!memcmp(runtime.le, before.le, sizeof(runtime.le)));
    const LINK_KEY absent = {0};
    assert(!memcmp(runtime.link_key, absent, sizeof(absent)));

    memset(cache, 0, sizeof(cache));
    memcpy(cache, flash, sizeof(cache)); /* Reopen the committed image. */
    for (unsigned i = 0; i < FIELDS; ++i) assert(!cache[0].present[i]);
    assert(!memcmp(cache[0].le, before.le, sizeof(before.le)));
    assert(!memcmp(&cache[1], &other, sizeof(other)) && nvs_closes >= 5);
    puts("Classic forget: stale key, active ACL, atomic size limit, NVS failure, live and reopened LE, and other peer passed");
}
