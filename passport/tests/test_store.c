#include <assert.h>
#include <stdio.h>
#include "ble_stubs.h"
#include "../main/passport_store.c"
struct ble_config ble_hs_cfg;
static struct { int type; union ble_store_value value; } ram[40];
static unsigned count, commits;
static uint8_t disk[sizeof(snapshot)];
static bool exists, fail_read, fail_set, fail_commit, fail_delete;
void explicit_bzero(void *p,size_t n) { volatile uint8_t *b=p;while(n--)*b++=0; }
void ble_store_config_init(void) { count=0; }
int ble_store_config_read(int type,const union ble_store_key *key,union ble_store_value *value) {
    unsigned idx=0;
    for(unsigned i=0;i<count;i++)if(ram[i].type==type&&idx++==key->sec.idx) { *value=ram[i].value;return 0; }
    return BLE_HS_ENOENT;
}
int ble_store_config_write(int type,const union ble_store_value *value) {
    assert(count<40);ram[count].type=type;ram[count++].value=*value;return 0;
}
int ble_store_config_delete(int type,const union ble_store_key *key) {
    if(fail_delete)return BLE_HS_ESTORE_FAIL;
    for(unsigned i=0;i<count;i++)if(ram[i].type==type&&!memcmp(&key->sec.peer_addr,&ram[i].value.sec.peer_addr,sizeof(ble_addr_t))) {
        memmove(ram+i,ram+i+1,(--count-i)*sizeof(*ram));return 0;
    }
    return BLE_HS_ENOENT;
}
int ble_store_iterate(int type,int (*cb)(int,union ble_store_value *,void *),void *arg) {
    for(unsigned i=0;i<count;i++)if(ram[i].type==type&&cb(type,&ram[i].value,arg))return 0; // Actual NimBLE early-stop semantics.
    return 0;
}
int ble_gap_unpair(const ble_addr_t *peer) {
    union ble_store_key key={.sec={.peer_addr=*peer}};
    int rc=ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&key);
    if(!rc)rc=ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_PEER_SEC,&key);
    return rc;
}
int nvs_open(const char *name,int mode,nvs_handle_t *handle) { assert(!strcmp(name,"passport_bonds")&&mode==NVS_READWRITE);*handle=1;return ESP_OK; }
int nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size) {
    assert(handle==1&&!strcmp(key,"records")&&*size==sizeof(disk));
    if(fail_read)return ESP_FAIL;if(!exists)return ESP_ERR_NVS_NOT_FOUND;memcpy(out,disk,*size);return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle,const char *key,const void *value,size_t size) {
    assert(handle==1&&!strcmp(key,"records")&&size==sizeof(disk));
    if(fail_set)return ESP_FAIL;memcpy(disk,value,size);exists=true;return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) { assert(handle==1);++commits;return fail_commit?ESP_FAIL:ESP_OK; }
static void setup(void) {
    exists=fail_read=fail_set=fail_commit=fail_delete=false;commits=0;known=false;transaction=false;
    assert(!passport_store_init()&&passport_store_known());
}
int main(void) {
    setup();union ble_store_value value={.sec={.peer_addr={.val={7}},.ltk_present=1}};
    assert(!passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value));
    assert(!passport_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value));
    assert(commits==2);assert(!passport_store_init()&&count==2);
    assert(!passport_store_forget(&value.sec.peer_addr)&&commits==3&&count==0);
    assert(!passport_store_init()&&count==0);
    fail_read=true;assert(passport_store_init()&&!passport_store_known());
    setup();fail_set=true;assert(passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)&&!known);
    union ble_store_key key={0};union ble_store_value out;
    assert(read_record(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==BLE_HS_ESTORE_FAIL);
    setup();fail_commit=true;assert(passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)&&!known);
    setup();assert(!passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value));
    fail_delete=true;assert(passport_store_forget(&value.sec.peer_addr)&&!known);
    setup();assert(!passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value));
    uint32_t bad_version=999;memcpy(disk,&bad_version,sizeof(bad_version));assert(passport_store_init()&&!known); // ABI/schema mismatch remains unavailable.
    setup();for(unsigned i=0;i<33;i++)assert(!ble_store_config_write(BLE_STORE_OBJ_TYPE_CCCD,&value));
    assert(persist()==BLE_HS_ESTORE_CAP&&!known&&!commits); // No truncated successful snapshot.
    puts("PASS: bond restore, atomic individual forget, read/write/commit/delete failures and capacity overflow");
}
