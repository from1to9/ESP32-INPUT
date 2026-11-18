#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ble_stubs.h"
#include "names_owner_fixture.h"
#include "../main/passport_ble.c"

struct ble_config ble_hs_cfg;
static uint64_t clock_ms;
static bool store_ok, send_fail;
static int stop_error, start_error, whitelist_error, terminate_error, security_error;
static unsigned adv_calls, terminate_calls, forget_calls, report_count, security_calls;
static bool controller_advertising;
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
static unsigned diagnostic_heap_reads;
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; ++diagnostic_heap_reads; return 20000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 12000; }
void passport_management_install(void) {} // Adapter tested separately; this suite exercises the real BLE backend.
uint64_t passport_control_http_service(uint64_t now) { (void)now; return 0; }
void passport_control_http_finish_move(uint64_t key,uint64_t now) { (void)key; (void)now; }
void passport_wifi_link_snapshot(passport_wifi_link_t *out) { *out=(passport_wifi_link_t){0}; }
void esp_fill_random(void *out,size_t size) { static uint8_t entropy; uint8_t *p=out; while(size--) *p++=++entropy; }

void explicit_bzero(void *p, size_t size) { volatile uint8_t *b = p; while(size--) *b++=0; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
int ble_addr_cmp(const ble_addr_t *a,const ble_addr_t *b) { return memcmp(a,b,sizeof(*a)); }
int passport_store_init(void) { return 0; }
static ble_addr_t stored_selection;
static bool selection_present, selection_failure, selection_cleared;
int passport_store_selected(ble_addr_t *peer) {
    if(selection_failure)return BLE_HS_ESTORE_FAIL;
    if(selection_cleared)return PASSPORT_STORE_SELECTION_CLEARED;
    if(!selection_present)return BLE_HS_ENOENT;*peer=stored_selection;return 0;
}
int passport_store_select(const ble_addr_t *peer) {
    if(selection_failure)return BLE_HS_ESTORE_FAIL;
    stored_selection=*peer;selection_present=true;selection_cleared=false;return 0;
}
bool passport_store_known(void) { return store_ok; }
uint32_t passport_store_security_revision(void) { return fixture_security_revision; }
int passport_store_write(int type,const union ble_store_value *value) { (void)type;(void)value;return store_ok?0:BLE_HS_ESTORE_FAIL; }
int passport_store_forget(const ble_addr_t *peer) {
    ++forget_calls;
    if(!selection_present||!ble_addr_cmp(peer,&stored_selection)){selection_present=false;selection_cleared=true;}
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
int ble_gap_adv_stop(void) { if(!stop_error)controller_advertising=false;return stop_error; }
int ble_gap_adv_active(void) { return controller_advertising; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) { assert(f->name_len==14);return 0; }
int ble_gap_wl_set(const ble_addr_t *peer,int n) { assert(n==1);allowed=*peer;return whitelist_error; }
int ble_gap_adv_start(int type,const ble_addr_t *peer,int duration,const struct ble_gap_adv_params *p,int (*cb)(struct ble_gap_event *,void *),void *arg) {
    (void)peer;(void)arg;assert(type==BLE_OWN_ADDR_PUBLIC&&duration>0&&duration<=300000&&cb==gap_event);++adv_calls;filter_policy=p->filter_policy;if(!start_error)controller_advertising=true;return start_error;
}
int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *desc) { assert(conn==connection);*desc=peer_desc;return 0; }
int ble_gap_security_initiate(uint16_t conn) { assert(conn==connection);++security_calls;return security_error; }
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
    memset(&usb_command,0,sizeof(usb_command)); physical_inputs_available=true;
    passport_control_deinit();
    memset(&remote_management,0,sizeof(remote_management));
    state=(passport_ble_state_t){.synced=true,.generation=1,.locked=true};
    connection=BLE_HS_CONN_HANDLE_NONE;hid_engine_init(&engine);hid_engine_set_locked(&engine,true);
    early_subscriptions.connection=BLE_HS_CONN_HANDLE_NONE;
    suspended=new_connection=selected_valid=transition=disconnect_requested=closing=link_ready=false;
    pairing_failed=false;
    startup_restore=selection_present=selection_failure=selection_cleared=false;
    reconnect_end=reconnect_next=0;reconnect_attempts=0;
    window_end=release_end=lease_end=0;clock_ms=1000;queue_count=0;store_ok=true;send_fail=false;
    stop_error=start_error=whitelist_error=terminate_error=security_error=0;
    adv_calls=terminate_calls=forget_calls=report_count=security_calls=0;saved_count=0;atomic_store(&emergency_lock,false);
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
static void subscribe_on(uint16_t conn,uint16_t handle,bool enabled) {
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_SUBSCRIBE,.subscribe={conn,handle,enabled}};gap_event(&ev,NULL);
}
static void subscribe(uint16_t handle,bool enabled) { subscribe_on(connection,handle,enabled); }
static void connect_saved(void) {
    saved[0]=(ble_addr_t){.val={1,2,3,4,5,6}};saved_count=1;selected=saved[0];selected_valid=true;
    refresh_peers();advertise(false,0);peer_desc=(struct ble_gap_conn_desc){.peer_id_addr=saved[0],.sec_state={true,true}};
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
    advertise(true,0);assert(!adv_calls&&!state.pairing&&!forget_calls);
    setup();connect_saved();assert(filter_policy==BLE_HCI_ADV_FILT_CONN&&!ble_addr_cmp(&allowed,&saved[0]));
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_REPEAT_PAIRING};assert(gap_event(&ev,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE&&!forget_calls);
    gap(BLE_GAP_EVENT_PASSKEY_ACTION);tick_once();assert(terminate_calls&&!state.ready);
    setup();selected=(ble_addr_t){.val={1}};selected_valid=true;saved[0]=selected;saved_count=1;
    whitelist_error=1;advertise(false,0);assert(!adv_calls&&!state.advertising);
    whitelist_error=0;advertise(false,0);peer_desc.peer_id_addr=(ble_addr_t){.val={2}};
    gap(BLE_GAP_EVENT_CONNECT);assert(terminate_calls==1&&!new_connection);
    setup();stop_error=1;advertise(true,0);assert(!adv_calls&&!state.pairing);
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
static void test_disconnect_diagnostics(void)
{
    setup(); advertise(true, 0);
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.val={9}}};
    gap(BLE_GAP_EVENT_CONNECT);
    saved[0]=peer_desc.peer_id_addr; saved_count=1;
    peer_desc.sec_state.encrypted=peer_desc.sec_state.bonded=true;
    tick_once();
    assert(!new_connection && !state.pairing && state.bonded && state.peer_count==1);
    assert(!state.ready && !strcmp(state.result,"New host saved"));
    assert(selection_present && !ble_addr_cmp(&stored_selection,&saved[0]));
    setup(); connect_saved();
    struct ble_gap_event encryption = {.type = BLE_GAP_EVENT_ENC_CHANGE, .enc_change = {.status = 7}};
    gap_event(&encryption, NULL);
    assert(closing && !strcmp(state.result, "Encryption failed"));
    gap(BLE_GAP_EVENT_DISCONNECT);
    assert(!state.connected && !strcmp(state.result, "Encryption failed"));
    setup(); connect_saved();
    gap(BLE_GAP_EVENT_DISCONNECT);
    assert(!strcmp(state.result, "Disconnected"));
    diagnostic_until = clock_ms + 20;
    unsigned before = diagnostic_heap_reads;
    diagnostic("probe", 0);
    assert(diagnostic_heap_reads == before + 1);
    clock_ms += 20;
    diagnostic("expired", 0);
    assert(diagnostic_heap_reads == before + 1);
    advertise(true, 2000);
    assert(diagnostic_until == clock_ms + 12000 && diagnostic_heap_reads > before + 1);
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

static cp_command_t remote_request(cp_operation_id_t op)
{
    cp_command_t c={.operation=op,.command_key=42,.resource_revision=state.bond_revision};
    char id[33];for(unsigned i=0;i<16;++i)snprintf(id+i*2,3,"%02x",state.peers[0].bond_id[i]);
    if(op==CP_OP_BLUETOOTH_TARGET_REQUEST) snprintf(c.args_json,sizeof(c.args_json),"{\"generation\":\"%u\",\"peer_id\":\"%s\",\"window_seconds\":2}",state.generation,id);
    else if(op==CP_OP_BLUETOOTH_DISCONNECT) snprintf(c.args_json,sizeof(c.args_json),"{\"generation\":\"%u\",\"peer_id\":\"%s\"}",state.generation,id);
    else if(op==CP_OP_BLUETOOTH_PAIRING_OPEN) snprintf(c.args_json,sizeof(c.args_json),"{\"generation\":\"%u\",\"window_seconds\":2}",state.generation);
    else snprintf(c.args_json,sizeof(c.args_json),"{\"generation\":\"%u\"}",state.generation);
    return c;
}
static void test_remote_management(void)
{
    cp_result_t r;setup();connect_saved();
    cp_command_t c=remote_request(CP_OP_BLUETOOTH_DISCONNECT);
    assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK&&r.state==CP_RESULT_ACCEPTED);
    assert(!passport_ble_remote_poll(clock_ms,&r));
    tick_once();assert(!terminate_calls);tick_once();assert(terminate_calls==1);
    gap(BLE_GAP_EVENT_DISCONNECT);tick_once();assert(passport_ble_remote_poll(clock_ms,&r)&&r.error==CP_OK&&r.state==CP_RESULT_APPLIED);
    assert(saved_count==1); // Disconnect does not delete or relabel the bond.
    c=remote_request(CP_OP_BLUETOOTH_TARGET_REQUEST);
    assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK);tick_once();
    assert(state.advertising&&!passport_ble_remote_poll(clock_ms,&r));
    clock_ms+=2100;tick_once();assert(passport_ble_remote_poll(clock_ms,&r)&&r.error==CP_EXPIRED);
    c=remote_request(CP_OP_BLUETOOTH_TARGET_REQUEST);
    assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK);tick_once();
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.val={9}},.sec_state={true,true}};gap(BLE_GAP_EVENT_CONNECT);
    assert(passport_ble_remote_poll(clock_ms,&r)&&r.error==CP_STALE&&!strcmp(r.reason,"unexpected_host_rejected"));
    setup();connect_saved();c=remote_request(CP_OP_BLUETOOTH_TARGET_REQUEST);
    assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK);issue(PASSPORT_LOCK);
    assert(passport_ble_remote_poll(clock_ms,&r)&&r.error!=CP_OK&&!transition);
    setup();issue(PASSPORT_UNLOCK);c=remote_request(CP_OP_BLUETOOTH_PAIRING_OPEN);
    cp_command_t unsupported=c;strcpy(unsupported.args_json,"{\"generation\":\"1\",\"peer_id\":\"arbitrary\"}");
    assert(passport_ble_remote_begin(&unsupported,clock_ms,&r)==CP_INVALID);
    assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK);tick_once();
    assert(passport_ble_remote_poll(clock_ms,&r)&&r.error==CP_OK&&state.pairing&&window_end==clock_ms+2000);
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr={.val={9}}};gap(BLE_GAP_EVENT_CONNECT);
    uint64_t original=window_end;c=remote_request(CP_OP_BLUETOOTH_PAIRING_CANCEL_HANDSHAKE);
    assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK&&!passport_ble_remote_poll(clock_ms,&r));
    gap(BLE_GAP_EVENT_DISCONNECT);clock_ms+=20;
    assert(passport_ble_remote_poll(clock_ms,&r)&&r.error==CP_OK&&state.pairing&&window_end==original);
    c=remote_request(CP_OP_BLUETOOTH_PAIRING_CLOSE);assert(passport_ble_remote_begin(&c,clock_ms,&r)==CP_OK);
    assert(passport_ble_remote_poll(clock_ms,&r)&&r.error==CP_OK&&!state.advertising&&!state.pairing);
}

static void early_saved_host(void)
{
    setup();hid_engine_set_locked(&engine,false);
    saved[0]=(ble_addr_t){.val={1}};saved_count=1;
    startup_restore=true;synced();tick_once();
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr=saved[0],.sec_state={true,true}};
}

static void test_early_security_and_subscriptions(void)
{
    // The pinned peripheral host waits for remote version/features before
    // CONNECT. Encryption and restored CCCDs can already have completed.
    early_saved_host();
    struct ble_gap_event encrypted={.type=BLE_GAP_EVENT_ENC_CHANGE};
    gap_event(&encrypted,NULL);
    subscribe_on(1,keyboard_handle,true);subscribe_on(1,mouse_handle,true);
    assert(!state.connected && !state.ready && !report_count);
    gap(BLE_GAP_EVENT_CONNECT);
    assert(state.keyboard_subscribed && state.mouse_subscribed && security_calls==0);
    tick_once();assert(!state.ready && report_count==1);
    tick_once();assert(state.ready && report_count==2 && !terminate_calls);
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<reports[i].length;++j)assert(reports[i].bytes[j]==0);

    // Preserve the latest actual subscription value, without guessing from a bond.
    early_saved_host();subscribe_on(1,keyboard_handle,true);subscribe_on(1,mouse_handle,true);
    subscribe_on(1,mouse_handle,false);gap(BLE_GAP_EVENT_CONNECT);tick_once();
    assert(!state.ready && state.keyboard_subscribed && !state.mouse_subscribed && !report_count);
    subscribe(mouse_handle,true);tick_once();tick_once();assert(state.ready);

    early_saved_host();subscribe_on(2,keyboard_handle,true);subscribe_on(2,mouse_handle,true);
    gap(BLE_GAP_EVENT_CONNECT);tick_once();assert(!state.ready && !report_count);
    subscribe(keyboard_handle,true);subscribe(mouse_handle,true);tick_once();tick_once();assert(state.ready);
    subscribe_on(2,keyboard_handle,false);tick_once();assert(state.ready);

    // A stopped admission window cannot donate subscriptions to another attempt,
    // even when the controller reuses the connection handle.
    early_saved_host();subscribe_on(1,keyboard_handle,true);subscribe_on(1,mouse_handle,true);
    stop_advertising();subscribe_on(1,keyboard_handle,true);advertise(false,0);
    gap(BLE_GAP_EVENT_CONNECT);tick_once();assert(!state.ready && !report_count);
    early_saved_host();subscribe_on(1,keyboard_handle,true);subscribe_on(1,mouse_handle,true);
    clock_ms=window_end;gap(BLE_GAP_EVENT_CONNECT);tick_once();assert(terminate_calls==1 && !state.ready && !report_count);
    early_saved_host();subscribe_on(1,keyboard_handle,true);subscribe_on(1,mouse_handle,true);
    peer_desc.peer_id_addr.val[0]=2;gap(BLE_GAP_EVENT_CONNECT);tick_once();
    assert(terminate_calls==1 && !state.ready && !report_count);

    // An ongoing host security procedure is not a failure; real errors remain so.
    early_saved_host();peer_desc.sec_state.encrypted=false;security_error=BLE_HS_EALREADY;
    gap(BLE_GAP_EVENT_CONNECT);tick_once();assert(security_calls==1 && !terminate_calls && !state.ready);
    peer_desc.sec_state.encrypted=true;subscribe(keyboard_handle,true);subscribe(mouse_handle,true);
    tick_once();tick_once();assert(state.ready);
    early_saved_host();peer_desc.sec_state.encrypted=false;security_error=BLE_HS_EREJECT;
    gap(BLE_GAP_EVENT_CONNECT);assert(security_calls==1 && terminate_calls==1 && !state.ready);
    puts("PASS: encryption/CCCD restoration before CONNECT, admission isolation and existing security procedure");
}

static void test_startup_reconnect(void)
{
    setup();hid_engine_set_locked(&engine,false);
    saved[0]=(ble_addr_t){.val={1}};saved_count=1;
    startup_restore=true;synced();tick_once();
    assert(selection_present && selected_valid && !ble_addr_cmp(&selected,&saved[0]));
    assert(adv_calls==1 && state.advertising && !state.pairing && reconnect_end);
    assert(filter_policy==BLE_HCI_ADV_FILT_CONN && !ble_addr_cmp(&allowed,&saved[0]));
    const uint64_t startup_deadline=window_end;
    peer_desc=(struct ble_gap_conn_desc){.peer_id_addr=saved[0]};gap(BLE_GAP_EVENT_CONNECT);
    peer_desc.sec_state.encrypted=peer_desc.sec_state.bonded=true;
    subscribe(keyboard_handle,true);subscribe(mouse_handle,true);tick_once();tick_once();
    assert(state.ready && !reconnect_end && !engine.count && !state.window_seconds);
    // A completed restore stays usable past the old admission deadline. Local
    // screen inactivity is an independent UI policy, not an expired BLE link.
    clock_ms=startup_deadline;tick_once();tick_once();
    assert(state.ready && !state.locked && !terminate_calls && adv_calls==1);
    assert(strcmp(state.result,"Window expired") && report_count==2);
    issue(PASSPORT_DISCONNECT);tick_once();tick_once();gap(BLE_GAP_EVENT_DISCONNECT);tick_once();
    unsigned attempts=adv_calls;clock_ms+=1000;tick_once();assert(adv_calls==attempts);

    setup();hid_engine_set_locked(&engine,false);
    saved[0]=(ble_addr_t){.val={1}};saved[1]=(ble_addr_t){.val={2}};saved_count=2;
    stored_selection=saved[1];selection_present=startup_restore=true;synced();tick_once();
    assert(!ble_addr_cmp(&selected,&saved[1]) && adv_calls==1);
    issue(PASSPORT_FORGET);tick_once();assert(!selection_present && !reconnect_end && !selected_valid);
    clock_ms+=1000;tick_once();assert(adv_calls==1);
    assert(saved_count==1 && selection_cleared);
    startup_restore=true;synced();tick_once();assert(adv_calls==1 && !selected_valid);

    setup();hid_engine_set_locked(&engine,false);saved_count=2;
    saved[0]=(ble_addr_t){.val={1}};saved[1]=(ble_addr_t){.val={2}};
    startup_restore=true;synced();tick_once();assert(!selected_valid && !adv_calls && !selection_present);
    stored_selection=(ble_addr_t){.val={3}};selection_present=startup_restore=true;
    synced();tick_once();assert(!selected_valid && !adv_calls);
    selection_failure=startup_restore=true;synced();tick_once();assert(!selected_valid && !adv_calls);

    setup();hid_engine_set_locked(&engine,false);saved[0]=(ble_addr_t){.val={1}};saved_count=1;
    start_error=1;startup_restore=true;synced();tick_once();
    for(unsigned i=0;i<5;++i){clock_ms+=1000;tick_once();}
    assert(reconnect_attempts==3 && !reconnect_end && !state.advertising);

    setup();hid_engine_set_locked(&engine,false);saved[0]=(ble_addr_t){.val={1}};saved_count=1;
    startup_restore=true;synced();tick_once();clock_ms=reconnect_end;tick_once();
    assert(!reconnect_end && !state.advertising && !state.pairing);
    setup();hid_engine_set_locked(&engine,false);saved[0]=(ble_addr_t){.val={1}};saved_count=1;
    startup_restore=true;synced();tick_once();
    struct ble_gap_event failed={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=1}};
    gap_event(&failed,NULL);tick_once();assert(adv_calls==1);
    clock_ms+=1000;tick_once();assert(adv_calls==2 && state.advertising);
    saved[1]=(ble_addr_t){.val={2}};saved_count=2;refresh_peers();
    passport_command_t change=action(PASSPORT_ADVERTISE);memcpy(change.peer,saved[1].val,6);
    command(&change,clock_ms);tick_once();
    assert(!reconnect_end && !ble_addr_cmp(&stored_selection,&saved[1]));
    setup();saved[0]=(ble_addr_t){.val={1}};saved_count=1;
    startup_restore=true;synced();tick_once();assert(!adv_calls && !reconnect_end); // Locked startup.

    setup();connect_saved();selection_failure=true;
    issue(PASSPORT_ADVERTISE);tick_once();tick_once();gap(BLE_GAP_EVENT_DISCONNECT);tick_once();
    assert(!state.advertising && !strcmp(state.result,"Host selection not saved"));

    for(unsigned remote=0;remote<2;++remote) {
        setup();hid_engine_set_locked(&engine,false);saved[0]=(ble_addr_t){.val={1}};saved_count=1;
        startup_restore=true;synced();tick_once();clock_ms+=1200;
        if(remote) {
            cp_command_t close=remote_request(CP_OP_BLUETOOTH_PAIRING_CLOSE);cp_result_t result;
            assert(passport_ble_remote_begin(&close,clock_ms,&result)==CP_OK);
            assert(passport_ble_remote_poll(clock_ms,&result) && result.error==CP_OK);
        } else issue(PASSPORT_STOP_PAIR);
        tick_once();assert(!state.advertising && !reconnect_end && adv_calls==1);
    }
}

static void test_usb_receipts(void)
{
    setup();connect_saved();
    passport_command_t c=action(PASSPORT_TAKEOVER);c.command_id=100;
    command(&c,clock_ms);assert(state.command.result==PR_PENDING);
    tick_once();tick_once();
    c=action(PASSPORT_KEY);c.command_id=101;c.usage=0x28;
    command(&c,clock_ms);assert(state.command.result==PR_PENDING);
    unsigned before=report_count;tick_once();
    assert(report_count==before+1&&reports[before].bytes[2]==0x28&&state.command.result==PR_PENDING);
    tick_once();assert(state.command.result==PR_APPLIED&&report_count==before+2);
    for(unsigned j=0;j<reports[before+1].length;++j)assert(!reports[before+1].bytes[j]);
    c=action(PASSPORT_KEY);c.command_id=102;c.usage=0x28;command(&c,clock_ms);send_fail=true;tick_once();
    assert(state.command.result==PR_UNKNOWN);send_fail=false;
    setup();connect_saved();c=action(PASSPORT_LOCK);c.command_id=103;command(&c,clock_ms);
    assert(state.command.result==PR_PENDING);tick_once();assert(state.command.result==PR_PENDING);tick_once();
    assert(state.command.result==PR_APPLIED&&state.locked);
    c=action(PASSPORT_UNLOCK);c.command_id=104;physical_inputs_available=false;command(&c,clock_ms);
    assert(state.command.result==PR_LOCKED&&engine.state.locked);
    physical_inputs_available=true;c.generation--;command(&c,clock_ms);assert(state.command.result==PR_STALE);
    setup();issue(PASSPORT_UNLOCK);c=action(PASSPORT_PAIR);c.command_id=105;c.window_ms=1000;command(&c,clock_ms);
    tick_once();assert(state.command.result==PR_APPLIED&&state.pairing&&state.advertising);
    stop_error=9;c=action(PASSPORT_STOP_PAIR);c.command_id=106;command(&c,clock_ms);assert(state.command.result==PR_UNAVAILABLE);
    for(unsigned neutral=0;neutral<2;++neutral) {
        setup();connect_saved();c=action(PASSPORT_FORGET);c.command_id=701+neutral;
        unsigned before=report_count;command(&c,clock_ms);assert(transition&&engine.state.release_pending);
        if(neutral){tick_once();tick_once();assert(!engine.state.release_pending&&disconnect_requested);}
        gap(BLE_GAP_EVENT_DISCONNECT);tick_once();
        assert(forget_calls==1&&!saved_count); // The bond deletion really happened in either case.
        assert(report_count==before+(neutral?2:0));
        assert(state.command.result==(neutral?PR_APPLIED:PR_UNKNOWN));
        if(!neutral)assert(state.release_unknown);
    }
    setup();connect_saved();c=action(PASSPORT_DISCONNECT);c.command_id=703;
    command(&c,clock_ms);assert(transition&&state.command.result==PR_PENDING);
    reset(77);tick_once();
    assert(!state.synced&&state.release_unknown&&state.command.id==703&&state.command.result==PR_UNKNOWN&&state.command.detail==77);
}

int main(void) {
    for (unsigned available=0;available<2;++available) {
        setup();passport_control_deinit();assert(passport_ble_start(available)==ESP_OK);
        assert(engine.state.locked==!available && state.locked==!available);
        assert(!state.ready && !report_count); // Navigable Home does not send HID.
        assert(ble_hs_cfg.sm_io_cap==BLE_HS_IO_NO_INPUT_OUTPUT&&ble_hs_cfg.sm_bonding);
    }
    test_security();test_input_release();test_disconnect_diagnostics();test_bond_names();test_remote_management();test_startup_reconnect();test_early_security_and_subscriptions();test_usb_receipts();
    setup();connect_saved();tick_once();tick_once();
    passport_command_t old=action(PASSPORT_MOVE);old.x=12;
    unsigned revision=state.input_revision;issue(PASSPORT_TAKEOVER);
    assert(state.input_revision==revision+1 && !engine.count);
    command(&old,clock_ms);assert(!engine.count);tick_once();tick_once();
    passport_command_t fresh=action(PASSPORT_MOVE);fresh.x=12;command(&fresh,clock_ms);assert(engine.count==1);
    passport_control_deinit();
    puts("PASS: pairing windows, stable identities, whitelist and capacity, two-report release, stale work, lease and failure recovery");
}
