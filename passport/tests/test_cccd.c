// Compile the pinned SDK CCCD functions, supplied unchanged by run.py.
#include <assert.h>
#include <stdio.h>
#include "ble_stubs.h"
#define MYNEWT_VAL(name) TEST_##name
#define TEST_BLE_SM_SC 1
#define NIMBLE_BLE_CONNECT 1
#define BLE_HS_LOG(...) ((void)0)
static const ble_addr_t any = {.type = 0xff};
#define BLE_ADDR_ANY (&any)
struct ble_store_key_cccd {
    ble_addr_t peer_addr;
    uint16_t chr_val_handle;
    uint8_t idx;
};
struct ble_store_value_cccd {
    ble_addr_t peer_addr;
    uint16_t chr_val_handle, flags;
    unsigned value_changed:1;
};
static struct ble_store_value_cccd ble_store_config_cccds[TEST_BLE_STORE_MAX_CCCDS];
static int ble_store_config_num_cccds;
int ble_addr_cmp(const ble_addr_t *a, const ble_addr_t *b) { return memcmp(a, b, sizeof(*a)); }
// SDK NVS persistence is disabled; passport_store owns the durable snapshot.
static int ble_store_config_persist_cccds(void) { return 0; }
#include "sdk_cccd.inc"
int main(void)
{
    for (unsigned host = 0; host < 3; ++host) {
        for (unsigned characteristic = 1; characteristic <= 3; ++characteristic) {
            struct ble_store_value_cccd value = {
                .peer_addr = {.val = {host + 1}},
                .chr_val_handle = characteristic,
                .flags = characteristic == 1 ? 2 : 1,
            };
            assert(ble_store_config_write_cccd(&value) == 0);
        }
    }
    assert(ble_store_config_num_cccds == 9);
    struct ble_store_value_cccd extra = {.peer_addr = {.val = {4}}, .chr_val_handle = 1, .flags = 1};
    assert(ble_store_config_write_cccd(&extra) == BLE_HS_ESTORE_CAP);
    for (unsigned host = 0; host < 3; ++host) {
        for (unsigned characteristic = 1; characteristic <= 3; ++characteristic) {
            struct ble_store_key_cccd key = {.peer_addr = {.val = {host + 1}}, .chr_val_handle = characteristic};
            struct ble_store_value_cccd value;
            assert(ble_store_config_read_cccd(&key, &value) == 0);
            assert(value.flags == (characteristic == 1 ? 2 : 1));
            assert(ble_store_config_write_cccd(&value) == 0); // Updating a full store consumes no extra slot.
        }
    }
    assert(ble_store_config_num_cccds == 9);
    puts("PASS: pinned SDK stores all 3 x 3 CCCDs; overflow preserves existing entries");
}
