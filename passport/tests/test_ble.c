#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ble_stubs.h"
#include "names_owner_fixture.h"
#include "../main/passport_ble.c"

struct ble_config ble_hs_cfg;
static uint64_t clock_ms;
static bool store_ok, send_fail;
static int stop_error, start_error, whitelist_error, terminate_error;
static unsigned adv_calls, terminate_calls, forget_calls, report_count;
static int filter_policy;
static ble_addr_t saved[3], allowed;
static unsigned saved_count;
static uint8_t fixture_ltk;
static uint32_t fixture_security_revision;
static struct ble_gap_conn_desc peer_desc;
static struct { unsigned handle, length; uint8_t bytes[8]; } reports[128];
static passport_command_t queue[8];
static unsigned queue_count;
static struct os_mbuf mbuf;
void passport_control_http_service(uint64_t now) { (void)now; }
void passport_wifi_snapshot(passport_wifi_state_t *out) { *out=(passport_wifi_state_t){0}; }
void esp_fill_random(void *out,size_t size) { static uint8_t entropy; uint8_t *p=out; while(size--) *p++=++entropy; }

void explicit_bzero(void *p, size_t size) { volatile uint8_t *b = p; while(size--) *b++=0; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
int ble_addr_cmp(const ble_addr_t *a,const ble_addr_t *b) { return memcmp(a,b,sizeof(*a)); }
int passport_store_init(void) { return 0; }
bool passport_store_known(void) { return store_ok; }
uint32_t passport_store_security_revision(void) { return fixture_security_revision; }
int passport_store_write(int type,const union ble_store_value *value) { (void)type;(void)value;return store_ok?0:BLE_HS_ESTORE_FAIL; }
int passport_store_forget(const ble_addr_t *peer) {
    ++forget_calls;
    for(unsigned i=0;i<saved_count;i++) if(!ble_addr_cmp(peer,&saved[i])) {
        memmove(saved+i,saved+i+1,(--saved_count-i)*sizeof(*saved)); return 0;
    }
    return BLE_HS_ENOENT;
}
int ble_store_read_our_sec(const struct ble_store_key_sec *key,struct ble_store_value_sec *value) {
    for(unsigned i=0;i<saved_count;i++) if(!ble_addr_cmp(&key->peer_addr,&saved[i])) {
        *value=(struct ble_store_value_sec){.peer_addr=saved[i],.ltk_present=1,.ltk={fixture_ltk}};return 0;
    }
    return BLE_HS_ENOENT;
}
int ble_store_util_bonded_peers(ble_addr_t *peers,int *count,int max) {
    assert((unsigned)max>=saved_count);*count=saved_count;memcpy(peers,saved,saved_count*sizeof(*saved));return 0;
}
QueueHandle_t xQueueCreate(unsigned length,unsigned size) { assert(length==8&&size==sizeof(*queue));return queue; }
void vQueueDelete(QueueHandle_t q) { assert(q==queue); }
int xQueueReset(QueueHandle_t q) { assert(q==queue);queue_count=0;return pdTRUE; }
int xQueueSend(QueueHandle_t q,const void *value,TickType_t t) {
    assert(q==queue&&t==0);if(queue_count==8)return 0;queue[queue_count++]=*(const passport_command_t *)value;return pdTRUE;
}
int xQueueReceive(QueueHandle_t q,void *value,TickType_t t) {
    assert(q==queue&&t==0);if(!queue_count)return 0;*(passport_command_t *)value=queue[0];
    memmove(queue,queue+1,--queue_count*sizeof(*queue));return pdTRUE;
}
int os_mbuf_append(struct os_mbuf *b,const void *data,unsigned len) { assert(b->len+len<=sizeof(b->data));memcpy(b->data+b->len,data,len);b->len+=len;return 0; }
int os_mbuf_copydata(struct os_mbuf *b,unsigned off,unsigned len,void *out) { if(off+len>b->len)return 1;memcpy(out,b->data+off,len);return 0; }
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data,unsigned len) { mbuf.len=0;os_mbuf_append(&mbuf,data,len);return &mbuf; }
int ble_gatts_notify_custom(uint16_t conn,uint16_t handle,struct os_mbuf *b) {
    assert(conn==connection&&report_count<128&&b->len<=8);
    if(send_fail)return 1;
    reports[report_count].handle=handle;reports[report_count].length=b->len;
    memcpy(reports[report_count++].bytes,b->data,b->len);return 0;
}
int ble_gap_adv_stop(void) { return stop_error; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) { assert(f->name_len==14);return 0; }
int ble_gap_wl_set(const ble_addr_t *peer,int n) { assert(n==1);allowed=*peer;return whitelist_error; }
int ble_gap_adv_start(int type,const ble_addr_t *peer,int duration,const struct ble_gap_adv_params *p,int (*cb)(struct ble_gap_event *,void *),void *arg) {
    (void)peer;(void)arg;assert(type==BLE_OWN_ADDR_PUBLIC&&duration==60000&&cb==gap_event);++adv_calls;filter_policy=p->filter_policy;return start_error;
}
int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *desc) { assert(conn==connection);*desc=peer_desc;return 0; }
int ble_gap_security_initiate(uint16_t conn) { assert(conn==connection);return 0; }
int ble_gap_terminate(uint16_t conn,int reason) { assert(conn==connection&&reason==BLE_ERR_REM_USER_CONN_TERM);++terminate_calls;return terminate_error; }
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s) { assert(s==services);return 0; }
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s) { assert(s==services);keyboard_handle=10;mouse_handle=20;return 0; }
void ble_svc_gap_init(void) {} void ble_svc_gatt_init(void) {}
int ble_svc_gap_device_name_set(const char *name) { assert(!strcmp(name,"Passport Input"));return 0; }
void ble_svc_gap_device_appearance_set(int appearance) { assert(appearance==0x03c0); }
void ble_npl_callout_init(struct ble_npl_callout *c,void *q,void (*cb)(struct ble_npl_event *),void *arg) { (void)c;(void)q;(void)arg;assert(cb==service); }
void ble_npl_callout_reset(struct ble_npl_callout *c,unsigned ticks) { (void)c;assert(ticks==20); }
void ble_npl_callout_stop(struct ble_npl_callout *c) { (void)c; }
unsigned ble_npl_time_ms_to_ticks32(unsigned ms) { return ms; }
void *nimble_port_get_dflt_eventq(void) { return NULL; }
esp_err_t nimble_port_init(void) { return ESP_OK; } esp_err_t nimble_port_deinit(void) { return ESP_OK; }
void nimble_port_run(void) {} void vTaskDelete(void *t) { (void)t; }
int xTaskCreate(void (*run)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle) {
    (void)run;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;return pdPASS;
}

static void setup(void) {
    passport_control_deinit();
    state=(passport_ble_state_t){.synced=true,.generation=1,.locked=true};
    connection=BLE_HS_CONN_HANDLE_NONE;hid_engine_init(&engine);hid_engine_set_locked(&engine,true);
    suspended=new_connection=selected_valid=transition=disconnect_requested=closing=link_ready=false;
    pairing_failed=false;
    window_end=release_end=lease_end=0;clock_ms=1000;queue_count=0;store_ok=true;send_fail=false;
    stop_error=start_error=whitelist_error=terminate_error=0;
    adv_calls=terminate_calls=forget_calls=report_count=0;saved_count=0;atomic_store(&emergency_lock,false);
    name_queued = names_reconciled = false;
    name_applied = name_removals = fixture_ltk = fixture_security_revision = 0;
    name_remove_result = PN_OK;
    commands=queue;keyboard_handle=10;mouse_handle=20;refresh_peers();
    assert(passport_control_init(&engine,random_bytes));
}
static passport_command_t action(passport_command_kind_t kind) {
    passport_command_t r={.kind=kind,.generation=state.generation,.bond_revision=state.bond_revision,.peer_type=selected.type,.input_revision=state.input_revision};
    memcpy(r.peer,selected.val,6);return r;
}
static void issue(passport_command_kind_t kind) { passport_command_t r=action(kind);command(&r,clock_ms); }
static void tick_once(void) { clock_ms+=20;service(NULL); }
static void gap(int type) { struct ble_gap_event ev={.type=type,.connect={.conn_handle=1}};gap_event(&ev,NULL); }
static void subscribe(uint16_t handle,bool enabled) {
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_SUBSCRIBE,.subscribe={connection,handle,enabled}};gap_event(&ev,NULL);
}
static void connect_saved(void) {
    saved[0]=(ble_addr_t){.val={1,2,3,4,5,6}};saved_count=1;selected=saved[0];selected_valid=true;
    refresh_peers();advertise(false);peer_desc=(struct ble_gap_conn_desc){.peer_id_addr=saved[0],.sec_state={true,true}};
    gap(BLE_GAP_EVENT_CONNECT);subscribe(keyboard_handle,true);subscribe(mouse_handle,true);issue(PASSPORT_UNLOCK);
    tick_once();assert(!state.ready&&report_count==1&&reports[0].handle==keyboard_handle);
    tick_once();assert(state.ready&&report_count==2&&reports[1].handle==mouse_handle);
    for(unsigned i=0;i<2;i++) for(unsigned j=0;j<reports[i].length;j++) assert(reports[i].bytes[j]==0);
}
static void test_security(void) {
    setup();union ble_store_value v={.sec={.peer_addr={.val={1}},.ltk_present=1}};
    assert(secure_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&v)==BLE_HS_EREJECT);
    issue(PASSPORT_PAIR);tick_once();assert(state.pairing&&state.advertising&&filter_policy==0);
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.type=BLE_ADDR_RANDOM,.val={1,0,0,0,0,0x40}}};
    gap(BLE_GAP_EVENT_CONNECT);assert(new_connection&&!strcmp(state.current,"Identity pending"));
    v.sec.peer_addr=peer_desc.peer_id_addr;
    assert(secure_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&v)==BLE_HS_EREJECT); // RPA is not identity.
    v.sec.peer_addr.val[5]=0xc0;assert(!secure_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&v));
    clock_ms=window_end;assert(secure_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&v)==BLE_HS_EREJECT);
    tick_once();assert(!state.pairing&&terminate_calls==1&&!state.ready);
    setup();issue(PASSPORT_PAIR);tick_once();peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.val={9}}};
    gap(BLE_GAP_EVENT_CONNECT);saved[0]=peer_desc.peer_id_addr;saved_count=1;
    peer_desc.sec_state.encrypted=peer_desc.sec_state.bonded=true;
    clock_ms=window_end;tick_once();assert(!selected_valid&&!state.ready&&terminate_calls==1);
    setup();saved[0]=(ble_addr_t){.val={7}};saved_count=1;refresh_peers();
    issue(PASSPORT_PAIR);tick_once();peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.type=BLE_ADDR_RANDOM,.val={0,0,0,0,0,0x40}}};
    gap(BLE_GAP_EVENT_CONNECT);v.sec.peer_addr=saved[0];
    assert(secure_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&v)==BLE_HS_EREJECT&&pairing_failed);
    peer_desc.peer_id_addr=saved[0];peer_desc.sec_state.encrypted=peer_desc.sec_state.bonded=true;
    tick_once();assert(!state.ready&&!selected_valid&&terminate_calls==1);
    setup();issue(PASSPORT_PAIR);issue(PASSPORT_LOCK);tick_once();assert(!adv_calls&&!state.pairing);
    setup();issue(PASSPORT_PAIR);tick_once();issue(PASSPORT_STOP_PAIR);assert(!state.pairing&&!state.advertising);
    setup();issue(PASSPORT_PAIR);tick_once();passport_command_t cancel=action(PASSPORT_STOP_PAIR);
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.val={9}}};gap(BLE_GAP_EVENT_CONNECT);
    command(&cancel,clock_ms);assert(!state.pairing&&terminate_calls==1); // Safety close survives generation change.
    setup();issue(PASSPORT_PAIR);tick_once();passport_ble_emergency_lock();tick_once();assert(!state.pairing&&!state.advertising);
    setup();saved_count=3;for(unsigned i=0;i<3;i++)saved[i]=(ble_addr_t){.val={i+1}};
    advertise(true);assert(!adv_calls&&!state.pairing&&!forget_calls);
    setup();connect_saved();assert(filter_policy==BLE_HCI_ADV_FILT_CONN&&!ble_addr_cmp(&allowed,&saved[0]));
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_REPEAT_PAIRING};assert(gap_event(&ev,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE&&!forget_calls);
    gap(BLE_GAP_EVENT_PASSKEY_ACTION);tick_once();assert(terminate_calls&&!state.ready);
    setup();selected=(ble_addr_t){.val={1}};selected_valid=true;saved[0]=selected;saved_count=1;
    whitelist_error=1;advertise(false);assert(!adv_calls&&!state.advertising);
    whitelist_error=0;advertise(false);peer_desc.peer_id_addr=(ble_addr_t){.val={2}};
    gap(BLE_GAP_EVENT_CONNECT);assert(terminate_calls==1&&!new_connection);
    setup();stop_error=1;advertise(true);assert(!adv_calls&&!state.pairing);
    setup();peer_desc.peer_id_addr=(ble_addr_t){.val={1}};gap(BLE_GAP_EVENT_CONNECT);assert(terminate_calls==1);
}
static void test_input_release(void) {
    setup();connect_saved();unsigned before=report_count;
    passport_command_t r=action(PASSPORT_KEY);r.usage=40;command(&r,clock_ms);tick_once();
    assert(report_count==before+1&&reports[before].bytes[2]==40);
    tick_once();assert(reports[before+1].bytes[2]==0);
    r=action(PASSPORT_MOVE);r.x=12;r.generation--;command(&r,clock_ms);assert(engine.count==0);
    r.generation=state.generation;command(&r,clock_ms);assert(engine.count==1);
    subscribe(mouse_handle,false);subscribe(mouse_handle,true);tick_once();tick_once();
    assert(engine.count==0&&state.ready); // Re-subscribe never replays old movement.
    r=action(PASSPORT_CLICK);command(&r,clock_ms);tick_once();assert(engine.count==1);
    issue(PASSPORT_FORGET);assert(forget_calls==0);
    tick_once();assert(!terminate_calls&&!forget_calls);
    tick_once();assert(terminate_calls==1&&!forget_calls);
    gap(BLE_GAP_EVENT_DISCONNECT);tick_once();assert(forget_calls==1&&saved_count==0);
    setup();connect_saved();r=action(PASSPORT_FORGET);r.bond_revision--;command(&r,clock_ms);assert(!transition&&!forget_calls);
    issue(PASSPORT_FORGET);saved[1]=(ble_addr_t){.val={2}};saved_count=2;tick_once();tick_once();gap(BLE_GAP_EVENT_DISCONNECT);tick_once();assert(!forget_calls);
    setup();connect_saved();r=action(PASSPORT_KEY);r.usage=40;command(&r,clock_ms);
    clock_ms=lease_end;tick_once();assert(engine.count==0&&engine.state.release_pending);
    setup();connect_saved();subscribe(mouse_handle,false);issue(PASSPORT_FORGET);
    clock_ms=release_end;tick_once();assert(state.release_unknown&&terminate_calls==1&&!forget_calls);
    setup();connect_saved();send_fail=true;issue(PASSPORT_LOCK);tick_once();tick_once();tick_once();assert(state.release_unknown&&!state.ready&&terminate_calls);
    setup();connect_saved();r=action(PASSPORT_MOVE);r.x=12;
    for(unsigned i=0;i<8;i++)assert(passport_ble_request(&r));
    assert(!passport_ble_request(&r));tick_once();assert(state.locked&&!queue_count&&!engine.count);
    setup();connect_saved();
    unsigned cancelled=state.cancel_revision;
    r=action(PASSPORT_CANCEL);assert(passport_ble_request(&r));
    tick_once();assert(published.cancel_revision==cancelled+1 && published.release_pending && !lease_end);
    tick_once();assert(!published.release_pending && state.ready);
    reset(1);assert(!state.connected&&!state.synced&&engine.state.locked);
}
static void test_bond_names(void)
{
    setup();
    saved[0] = (ble_addr_t){.val = {1}};
    saved_count = 1;
    fixture_ltk = 5;
    refresh_peers();
    queued_name = (passport_name_change_t){.key = {.kind = PN_HID_BOND}, .revision = 1};
    memcpy(queued_name.key.bond, state.peers[0].bond_id, sizeof(queued_name.key.bond));
    name_queued = true;
    service_names();
    assert(name_result == PN_LOCKED && !name_applied);
    hid_engine_set_locked(&engine, false);
    name_queued = true;
    service_names();
    assert(name_result == PN_OK && name_applied == 1);
    // Same address with new local LTK is a different bond incarnation.
    ++fixture_ltk;
    ++fixture_security_revision;
    refresh_peers();
    name_queued = true;
    service_names();
    assert(name_result == PN_NOT_FOUND && name_applied == 1);
    memcpy(queued_name.key.bond, state.peers[0].bond_id, sizeof(queued_name.key.bond));
    pending = (passport_command_t){.kind = PASSPORT_FORGET, .peer = {1}, .bond_revision = state.bond_revision};
    name_remove_result = PN_UNCERTAIN;
    execute_management();
    assert(!saved_count && name_removals == 1 && strstr(state.result, "cleanup failed"));
    assert(!memcmp(queued_name.key.bond, removed_name.bond, sizeof(removed_name.bond)));
}

int main(void) {
    setup();passport_control_deinit();assert(passport_ble_start()==ESP_OK);assert(ble_hs_cfg.sm_io_cap==BLE_HS_IO_NO_INPUT_OUTPUT&&ble_hs_cfg.sm_bonding);
    test_security();test_input_release();test_bond_names();
    setup();connect_saved();tick_once();tick_once();
    passport_command_t old=action(PASSPORT_MOVE);old.x=12;
    unsigned revision=state.input_revision;issue(PASSPORT_TAKEOVER);
    assert(state.input_revision==revision+1 && !engine.count);
    command(&old,clock_ms);assert(!engine.count);tick_once();tick_once();
    passport_command_t fresh=action(PASSPORT_MOVE);fresh.x=12;command(&fresh,clock_ms);assert(engine.count==1);
    passport_control_deinit();
    puts("PASS: pairing windows, stable identities, whitelist and capacity, two-report release, stale work, lease and failure recovery");
}
