#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ble_stubs.h"
#include "../main/passport_store.c"
struct ble_config ble_hs_cfg;
static struct { int type; union ble_store_value value; } ram[40];
static unsigned count, commits;
static uint8_t disk[sizeof(snapshot)];
static bool exists, fail_read, fail_set, fail_commit, fail_delete, fail_open;
static bool selected_exists, fail_clear;
static ble_addr_t selected_disk;
static size_t selected_length;
int ble_addr_cmp(const ble_addr_t *a,const ble_addr_t *b) { return memcmp(a,b,sizeof(*a)); }
static bool fail_allocation;
static unsigned nvs_opens;
static void *fixture_calloc(size_t count, size_t size) { return fail_allocation ? NULL : calloc(count, size); }
void explicit_bzero(void *p,size_t n) { volatile uint8_t *b=p;while(n--)*b++=0; }
// Execute the pinned initializer, including its callback overwrite and record
// reset. Substitute backing arrays and allocator/platform boundaries only.
#define MYNEWT_VAL(name) TEST_##name
#define TEST_BLE_STATIC_TO_DYNAMIC 1
#define TEST_ENC_ADV_DATA 0
#define SYSINIT_ASSERT_ACTIVE() ((void)0)
#define nimble_platform_mem_calloc fixture_calloc
#define nimble_platform_mem_free free
typedef struct { int unused; } ble_store_config_vars_t;
static ble_store_config_vars_t *ble_store_config_vars;
#define ble_store_config_num_our_secs count
static int ble_store_config_num_peer_secs, ble_store_config_num_cccds;
static int ble_store_config_num_csfcs, ble_store_config_num_rpa_recs, ble_store_config_num_local_irks;
static void ble_store_config_conf_init(void) {} // SDK persistence is disabled.
#define ble_store_config_init __real_ble_store_config_init
#include "sdk_store_init.inc"
#undef ble_store_config_init
#include "sdk_store_deinit.inc"

struct ble_store_key_local_irk { ble_addr_t addr; uint8_t idx; };
struct ble_store_value_local_irk { ble_addr_t addr; uint8_t irk[16]; };
static struct { uint8_t irk[16]; } *ble_hs_pvcy_ctx;
#define ble_hs_pvcy_default_irk (ble_hs_pvcy_ctx->irk)
#define BLE_DEV_ADDR_LEN 6
#define BLE_HS_LOG(...) ((void)0)
static unsigned random_calls, admitted_writes;
static int ble_hs_id_addr(uint8_t type, const uint8_t **addr, int *nrpa)
{ static const uint8_t local[6]={1,2,3,4,5,6}; assert(type==BLE_ADDR_PUBLIC&&!nrpa); *addr=local; return 0; }
static int ble_hs_hci_util_rand(void *out, int size)
{ ++random_calls; memset(out,random_calls,(size_t)size); return 0; }
static int ble_store_read_local_irk(const struct ble_store_key_local_irk *key, struct ble_store_value_local_irk *value)
{
    union ble_store_key k={0}; union ble_store_value v;
    memcpy(&k,key,sizeof(*key));
    int rc=ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&k,&v);
    if(!rc)memcpy(value,&v,sizeof(*value));
    return rc;
}
static int ble_store_write_local_irk(const struct ble_store_value_local_irk *value)
{
    union ble_store_value v={0}; memcpy(&v,value,sizeof(*value));
    return ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_LOCAL_IRK,&v);
}
// This macro is the host substitute for the target's --wrap linkage, not for
// startup behavior: the extracted SDK body still makes its real second call.
#define ble_store_config_init __wrap_ble_store_config_init
#include "sdk_default_irk.inc"
#undef ble_store_config_init
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
int nvs_open(const char *name,int mode,nvs_handle_t *handle) { assert(!strcmp(name,"passport_bonds")&&mode==NVS_READWRITE);++nvs_opens;*handle=1;return fail_open?ESP_FAIL:ESP_OK; }
int nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size) {
    if(!strcmp(key,"selected")) {
        assert(handle==1&&*size==sizeof(selected_disk));
        if(fail_read)return ESP_FAIL;if(!selected_exists)return ESP_ERR_NVS_NOT_FOUND;
        memcpy(out,&selected_disk,sizeof(selected_disk));*size=selected_length;return ESP_OK;
    }
    assert(handle==1&&!strcmp(key,"records")&&*size==sizeof(disk));
    if(fail_read)return ESP_FAIL;if(!exists)return ESP_ERR_NVS_NOT_FOUND;memcpy(out,disk,*size);return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle,const char *key,const void *value,size_t size) {
    if(!strcmp(key,"selected")) {
        assert(handle==1&&size==sizeof(selected_disk));if(fail_set)return ESP_FAIL;
        if(fail_clear&&((const ble_addr_t *)value)->type==UINT8_MAX)return ESP_FAIL;
        memcpy(&selected_disk,value,size);selected_length=size;selected_exists=true;return ESP_OK;
    }
    assert(handle==1&&!strcmp(key,"records")&&size==sizeof(disk));
    if(fail_set)return ESP_FAIL;memcpy(disk,value,size);exists=true;return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) { assert(handle==1);++commits;return fail_commit?ESP_FAIL:ESP_OK; }
static void setup(void) {
    exists=fail_read=fail_set=fail_commit=fail_delete=fail_open=false;commits=0;known=false;transaction=false;
    selected_exists=fail_clear=false;
    assert(!passport_store_init()&&passport_store_known());
}
static int admitted_write(int type,const union ble_store_value *value)
{ ++admitted_writes; return passport_store_write(type,value); }
static void startup(void)
{
    ble_hs_cfg.store_write_cb=admitted_write; // Like the BLE owner's secure_write.
    ble_hs_pvcy_set_default_irk();
    assert(ble_hs_cfg.store_write_cb==admitted_write);
    assert(ble_hs_cfg.store_read_cb==read_record && ble_hs_cfg.store_delete_cb==delete_record);
}
static void test_initial_allocation_failure(void)
{
    fail_allocation=true;
    assert(passport_store_init()==BLE_HS_ESTORE_FAIL);
    assert(!known && !config_initialized && !ble_store_config_vars && !nvs_opens);
    __wrap_ble_store_config_init();
    assert(!config_initialized && !ble_store_config_vars);
    fail_allocation=false;
    __wrap_ble_store_config_init();
    assert(config_initialized && ble_store_config_vars && !known);
}
static void test_startup_and_reboot(void)
{
    setup(); random_calls=admitted_writes=0;
    startup(); assert(count==1 && commits==1 && random_calls==1 && admitted_writes==1);
    uint8_t irk[16]; memcpy(irk,ble_hs_pvcy_default_irk,sizeof(irk));
    union ble_store_value peer={.sec={.peer_addr={.val={7}},.ltk_present=1}};
    assert(!ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&peer));
    assert(!ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_PEER_SEC,&peer));
    assert(count==3 && commits==3);
    ble_store_config_deinit();
    fail_allocation=true;
    unsigned opened=nvs_opens;
    assert(passport_store_init()==BLE_HS_ESTORE_FAIL);
    assert(!known && !config_initialized && !ble_store_config_vars && nvs_opens==opened);
    fail_allocation=false;
    assert(!passport_store_init() && count==3); // Restore before host startup.
    free(ble_hs_pvcy_ctx); ble_hs_pvcy_ctx=NULL;
    startup(); assert(count==3 && commits==3 && random_calls==1);
    assert(!memcmp(irk,ble_hs_pvcy_default_irk,sizeof(irk)));
    startup(); assert(count==3 && random_calls==1); // Host resynchronization.
    assert(!passport_store_forget(&peer.sec.peer_addr) && count==1);
    assert(!passport_store_init()); startup(); assert(count==1 && random_calls==1);
    setup(); fail_commit=true; startup(); assert(!known && commits==1);
    // A repeated SDK startup must not escape the failed-closed wrappers.
    startup(); assert(!known && commits==1);
    fail_open=true; assert(passport_store_init() && !known);
}
static void test_selected_host(void)
{
    setup();ble_addr_t a={.val={1}},b={.val={2}},out={.val={99}};
    assert(passport_store_selected(&out)==BLE_HS_ENOENT && out.val[0]==99);
    assert(!passport_store_select(&a) && commits==1);
    assert(!passport_store_select(&a) && commits==1);
    assert(!passport_store_init() && !passport_store_selected(&out) && !ble_addr_cmp(&out,&a));
    assert(!passport_store_select(&b) && !passport_store_selected(&out) && !ble_addr_cmp(&out,&b));
    selected_length--;assert(passport_store_selected(&out)==BLE_HS_ESTORE_FAIL);
    selected_length++;selected_disk.type=BLE_ADDR_RANDOM;selected_disk.val[5]=0;
    assert(passport_store_selected(&out)==BLE_HS_ESTORE_FAIL);
    assert(!passport_store_select(&a)); // Explicit selection replaces a malformed preference.
    fail_set=true;assert(passport_store_select(&b)==BLE_HS_ESTORE_FAIL && known);fail_set=false;
    fail_commit=true;assert(passport_store_select(&b)==BLE_HS_ESTORE_FAIL && known);fail_commit=false;
    assert(passport_store_selected(&out)==BLE_HS_ESTORE_FAIL);
    unsigned before=commits;assert(!passport_store_select(&b) && commits==before+1);
    assert(!passport_store_select(&a));
    for(unsigned i=0;i<2;++i) {
        union ble_store_value value={.sec={.peer_addr=i?b:a,.ltk_present=1}};
        assert(!passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value));
        assert(!passport_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value));
    }
    assert(!passport_store_forget(&b) && !passport_store_selected(&out) && !ble_addr_cmp(&out,&a));
    assert(!passport_store_forget(&a) && passport_store_selected(&out)==PASSPORT_STORE_SELECTION_CLEARED);
    assert(!passport_store_init() && passport_store_selected(&out)==PASSPORT_STORE_SELECTION_CLEARED);
    setup();union ble_store_value value={.sec={.peer_addr=a,.ltk_present=1}};
    assert(!passport_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value));
    assert(!passport_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value));
    assert(!passport_store_select(&a));fail_clear=true;
    assert(passport_store_forget(&a)==BLE_HS_ESTORE_FAIL && !known);
}
int main(void) {
    test_initial_allocation_failure();
    test_startup_and_reboot();
    test_selected_host();
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
    free(ble_hs_pvcy_ctx); ble_store_config_deinit();
    puts("PASS: pinned dynamic IRK startup preserves callbacks, bonds and IRK across resync/reboot; atomic forget, storage failures and capacity overflow");
}
