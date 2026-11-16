#include "passport_store.h"

#include <strings.h>
#include "nvs.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

// One atomic NVS blob preserves the fixed SDK's store records. An SDK/ABI
// change requires a deliberate migration; unknown formats remain unavailable.
#define STORE_VERSION 1
#define STORE_RECORDS 32
static struct {
    uint32_t version, value_size, count;
    struct { int type; union ble_store_value value; } records[STORE_RECORDS];
} snapshot;
static const int types[] = {
    BLE_STORE_OBJ_TYPE_OUR_SEC, BLE_STORE_OBJ_TYPE_PEER_SEC, BLE_STORE_OBJ_TYPE_CCCD,
    BLE_STORE_OBJ_TYPE_CSFC, BLE_STORE_OBJ_TYPE_PEER_ADDR, BLE_STORE_OBJ_TYPE_LOCAL_IRK
};
static nvs_handle_t storage;
static bool known, transaction, snapshot_full;

static int collect(int type, union ble_store_value *value, void *arg)
{
    (void)arg;
    if (snapshot.count == STORE_RECORDS) { snapshot_full = true; return BLE_HS_ESTORE_CAP; }
    snapshot.records[snapshot.count].type = type;
    snapshot.records[snapshot.count++].value = *value;
    return 0;
}

static int persist(void)
{
    snapshot.version = STORE_VERSION;
    snapshot.value_size = sizeof(union ble_store_value);
    snapshot.count = 0;
    snapshot_full = false;
    int rc = 0;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]) && !rc; ++i) {
        rc = ble_store_iterate(types[i], collect, NULL);
        // NimBLE treats a nonzero iterator callback as successful early stop.
        if (snapshot_full) rc = BLE_HS_ESTORE_CAP;
    }
    if (!rc && nvs_set_blob(storage, "records", &snapshot, sizeof(snapshot)) != ESP_OK) rc = BLE_HS_ESTORE_FAIL;
    if (!rc && nvs_commit(storage) != ESP_OK) rc = BLE_HS_ESTORE_FAIL;
    explicit_bzero(&snapshot, sizeof(snapshot));
    if (rc) known = false;
    return rc;
}

static int read_record(int type, const union ble_store_key *key, union ble_store_value *value)
{
    if (!known) return BLE_HS_ESTORE_FAIL;
    return ble_store_config_read(type, key, value);
}

int passport_store_write(int type, const union ble_store_value *value)
{
    if (!known) return BLE_HS_ESTORE_FAIL;
    int rc = ble_store_config_write(type, value);
    if (!rc && !transaction) rc = persist();
    // The SDK may ignore a key-write error after earlier records were written.
    if (rc) known = false;
    return rc;
}

static int delete_record(int type, const union ble_store_key *key)
{
    if (!known) return BLE_HS_ESTORE_FAIL;
    int rc = ble_store_config_delete(type, key);
    if (!rc && !transaction) rc = persist();
    return rc;
}

int passport_store_init(void)
{
    ble_store_config_init();
    esp_err_t err = nvs_open("passport_bonds", NVS_READWRITE, &storage);
    if (err != ESP_OK) return BLE_HS_ESTORE_FAIL;
    size_t length = sizeof(snapshot);
    err = nvs_get_blob(storage, "records", &snapshot, &length);
    int rc = 0;
    if (err == ESP_OK) {
        if (length != sizeof(snapshot) || snapshot.version != STORE_VERSION ||
            snapshot.value_size != sizeof(union ble_store_value) || snapshot.count > STORE_RECORDS) {
            rc = BLE_HS_ESTORE_FAIL;
        }
        for (size_t i = 0; i < snapshot.count && !rc; ++i) {
            bool supported = false;
            for (size_t j = 0; j < sizeof(types) / sizeof(types[0]); ++j) {
                supported |= snapshot.records[i].type == types[j];
            }
            rc = supported ? ble_store_config_write(snapshot.records[i].type, &snapshot.records[i].value)
                           : BLE_HS_ESTORE_FAIL;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        rc = BLE_HS_ESTORE_FAIL;
    }
    explicit_bzero(&snapshot, sizeof(snapshot));
    known = rc == 0;
    ble_hs_cfg.store_read_cb = read_record;
    ble_hs_cfg.store_write_cb = passport_store_write;
    ble_hs_cfg.store_delete_cb = delete_record;
    return rc;
}

bool passport_store_known(void) { return known; }

int passport_store_forget(const ble_addr_t *peer)
{
    if (!known) return BLE_HS_ESTORE_FAIL;
    transaction = true;
    int rc = ble_gap_unpair(peer);
    transaction = false;
    if (!rc) rc = persist();
    // A partial in-memory deletion or uncertain NVS commit cannot become ready.
    if (rc) known = false;
    return rc;
}
