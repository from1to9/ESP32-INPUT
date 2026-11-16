#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "names_owner_fixture.h"
#include "../main/passport_wifi.c"
#include "../main/passport_wifi_text.c"
#include "../main/passport_wifi_store.c"

static uint64_t clock_ms = 1000;
static passport_ble_state_t mock_ble;
static unsigned connects, scans, stops, starts, commits, clears;
static int init_step, fail_step;
static bool event_loop, driver_live, entropy, nvs_opened, reject_cancel;
static unsigned registrations;
static esp_netif_t interfaces[2];
static wifi_ap_record_t fixture_rows[16];
static unsigned fixture_count;
static unsigned observed_station_ip;
static esp_err_t write_error, commit_error, read_error, stop_error;
static passport_wifi_store_t durable;
static bool persisted;
static uint32_t random_counter = 100;
typedef struct { unsigned count, length, size; uint8_t data[16][256]; } queue_t;
static esp_err_t next_step(void) { return ++init_step == fail_step ? ESP_FAIL : ESP_OK; }
const char *esp_err_to_name(esp_err_t rc) { return rc ? "ESP_FAIL" : "ESP_OK"; }
void explicit_bzero(void *data, size_t size) { volatile uint8_t *p = data; while (size--) *p++ = 0; }
void bootloader_random_enable(void) { entropy = true; }
void bootloader_random_disable(void) { entropy = false; }
void esp_fill_random(void *data, size_t size) { memset(data, 0xAB, size); }
uint32_t esp_random(void) { return ++random_counter; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
esp_err_t esp_netif_init(void) { return next_step(); }
esp_err_t esp_event_loop_create_default(void) { esp_err_t rc = next_step(); event_loop = !rc; return rc; }
esp_err_t esp_event_loop_delete_default(void) { event_loop = false; return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; esp_err_t rc = next_step(); driver_live = !rc; return rc; }
esp_err_t esp_wifi_deinit(void) { driver_live = false; return ESP_OK; }
esp_netif_t *esp_netif_new(const esp_netif_config_t *c) { if (next_step()) return NULL; interfaces[c->interface].live = true; return &interfaces[c->interface]; }
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *n) { assert(n); return next_step(); }
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *n) { assert(n); return next_step(); }
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) { return next_step(); }
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void) { return next_step(); }
void esp_netif_destroy_default_wifi(void *n) { assert(n); ((esp_netif_t *)n)->live = false; }
esp_err_t esp_wifi_set_storage(int mode) { assert(mode == WIFI_STORAGE_RAM); return next_step(); }
esp_err_t esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA); return ESP_OK; }
esp_err_t esp_wifi_set_config(int mode, wifi_config_t *c) { if (mode == WIFI_IF_AP) assert(c->ap.authmode == WIFI_AUTH_WPA2_PSK); return ESP_OK; }
esp_err_t esp_wifi_start(void) { ++starts; return ESP_OK; }
esp_err_t esp_wifi_stop(void) { ++stops; return stop_error; }
esp_err_t esp_wifi_connect(void) { ++connects; return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id, void (*cb)(void *,esp_event_base_t,int32_t,void *), void *arg, esp_event_handler_instance_t *handle)
{ (void)base; (void)id; (void)cb; (void)arg; esp_err_t rc = next_step(); if (!rc) { ++registrations; *handle = (void *)1; } return rc; }
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, esp_event_handler_instance_t handle)
{ (void)base; (void)id; assert(handle && registrations); --registrations; return ESP_OK; }
esp_err_t esp_netif_dhcps_get_status(esp_netif_t *netif, esp_netif_dhcp_status_t *status) { assert(netif); *status = ESP_NETIF_DHCP_STARTED; return ESP_OK; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip) { assert(netif); ip->ip.addr = netif == station_netif ? observed_station_ip : 42; return ESP_OK; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) { assert(!block && config->scan_time.active.max <= 120); ++scans; return ESP_OK; }
esp_err_t esp_wifi_scan_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { ++clears; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records) { *count = fixture_count; memcpy(records, fixture_rows, fixture_count * sizeof(*records)); ++clears; return ESP_OK; }
void passport_ble_snapshot(passport_ble_state_t *out) { *out = mock_ble; }
bool passport_ble_request(const passport_command_t *command) { assert(command->kind == PASSPORT_CANCEL); return !reject_cancel; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return next_step() ? 0 : pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
QueueHandle_t xQueueCreate(unsigned length, unsigned size) { if (next_step()) return NULL; assert(length <= 16 && size <= 256); queue_t *q = calloc(1, sizeof(*q)); q->length = length; q->size = size; return q; }
void vQueueDelete(QueueHandle_t q) { free(q); }
int xQueueSend(QueueHandle_t handle, const void *data, TickType_t ticks) { (void)ticks; queue_t *q = handle; if (q->count == q->length) return 0; memcpy(q->data[q->count++], data, q->size); return pdTRUE; }
int xQueueReceive(QueueHandle_t handle, void *data, TickType_t ticks) { (void)ticks; queue_t *q = handle; if (!q->count) return 0; memcpy(data, q->data[0], q->size); memmove(q->data, q->data + 1, --q->count * sizeof(q->data[0])); return pdTRUE; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) { (void)mode; assert(!strcmp(name, "passport_wifi")); nvs_opened = true; *handle = 1; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle && nvs_opened); nvs_opened = false; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) { assert(handle && !strcmp(key,"profiles")); if (read_error) return read_error; if (!persisted) return ESP_ERR_NVS_NOT_FOUND; assert(*size == sizeof(durable)); memcpy(out, &durable, sizeof(durable)); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) { assert(handle && !strcmp(key,"profiles") && size == sizeof(durable)); if (write_error) return write_error; durable = *(const passport_wifi_store_t *)data; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle); ++commits; if (commit_error) return commit_error; persisted = true; return ESP_OK; }
static void event(bool ip, int id) { queued_event_t e = {.ip_event = ip, .id = id}; handle_event(&e, clock_ms); }
static void command_local(passport_wifi_action_t action, uint32_t id, uint32_t revision) { passport_wifi_command_t c = {.action=action,.id=id,.revision=revision}; handle_command(&c,clock_ms); }
static void reset_runtime(void)
{
    state = (passport_wifi_state_t){.initialized = true, .storage_known = true, .revision = 1};
    store = (passport_wifi_store_t){.version = 1, .next_id = 1};
    mock_ble = (passport_ble_state_t){0};
    reject_cancel = false;
    switching = waiting_stop = waiting_release = associating = radio_started = radio_ap = desired_ap = desired_station = next_ap = next_station = false;
    new_profile = target_new = pin_bssid = target_pin = setup_requested = scan_draining = false;
    fallback_at = retry_at = join_deadline = setup_deadline = 0;
    starts = stops = connects = scans = clears = commits = attempts = 0;
    clock_ms = 1000; commit_error = write_error = read_error = stop_error = 0;
    atomic_store(&provisioning,true); atomic_store(&event_loss,false);
    memset(&joining,0,sizeof(joining)); memset(&target_record,0,sizeof(target_record));
}
static void test_init(void)
{
    passport_wifi_prepare();
    assert(!entropy && strlen((char *)ap_config.ap.password) == 16);
    for (fail_step = 1; fail_step <= 15; ++fail_step) {
        init_step = 0;
        assert(passport_wifi_start() != ESP_OK);
        assert(!event_loop && !driver_live && !interfaces[0].live && !interfaces[1].live && !registrations && !commands && !events);
    }
    fail_step = 0; init_step = 0;
    assert(passport_wifi_start() == ESP_OK && init_step == 15);
}
static void test_scan_and_ap(void)
{
    reset_runtime(); next_ap = true; start_radio(clock_ms);
    assert(!state.ap_started && !state.dhcp_running);
    event(false,WIFI_EVENT_AP_START);
    assert(state.ap_started && state.dhcp_running && !state.ap_lease);
    event(false,WIFI_EVENT_AP_STACONNECTED);
    queued_event_t lease = {.ip_event=true,.id=IP_EVENT_AP_STAIPASSIGNED,.data.lease={.esp_netif=ap_netif,.ip={43}}};
    handle_event(&lease,clock_ms); assert(state.ap_lease && state.ap_client);
    uint64_t network_generation=state.transport_generation;
    event(false,WIFI_EVENT_AP_STADISCONNECTED); assert(!state.ap_lease && state.transport_generation==network_generation+1);
    command_local(PW_SCAN,0,0); assert(state.scanning && scans == 1 && stops == 0);
    command_local(PW_SCAN_CANCEL,0,0); assert(scan_draining && !state.scanning);
    command_local(PW_SCAN,0,0); assert(scans == 1);
    event(false,WIFI_EVENT_SCAN_DONE); assert(!scan_draining && clears == 1);
    command_local(PW_SCAN,0,0); assert(scans == 2);
    fixture_count = 1; fixture_rows[0] = (wifi_ap_record_t){.ssid="Test LAN",.authmode=WIFI_AUTH_WPA2_PSK,.primary=6,.rssi=-55,.bssid={1}};
    event(false,WIFI_EVENT_SCAN_DONE); assert(state.scan_known && state.scan_count == 1 && stops == 0);
}
static void stage_request(void)
{
    command_local(PW_SETUP_OPEN,0,0); assert(state.setup_open);
    queued_command_t message = {.submission=true,.credentials={.setup_id=state.setup_id,.request_id=1001,.scan_revision=state.scan_revision,.row=0,.password="fixture123"}};
    memcpy(message.token,setup_token,33);
    handle_submission(&message); assert(state.pending && state.submission_accepted && state.last_submission_id == 1001 && state.request_state == PW_REQUEST_WAITING);
    command_local(PW_CONFIRM,state.pending_id + 1,state.setup_id); assert(state.pending && !switching);
    command_local(PW_CONFIRM,state.pending_id,state.setup_id); assert(switching && !state.pending && state.request_state == PW_REQUEST_JOINING);
    step(clock_ms); assert(waiting_stop && stop_mask == 3);
    event(false,WIFI_EVENT_STA_STOP); event(false,WIFI_EVENT_AP_STOP); step(clock_ms);
    assert(!switching && new_profile && !state.setup_open);
    event(false,WIFI_EVENT_STA_START); assert(connects == 1);
}
static void got_connection(bool ip)
{
    queued_event_t associated = {.id=WIFI_EVENT_STA_CONNECTED,.data.connected={.ssid="Test LAN",.ssid_len=8,.authmode=WIFI_AUTH_WPA2_PSK,.bssid={1}}};
    handle_event(&associated,clock_ms);
    assert(state.phase == PW_ASSOCIATED && !commits && !store.count);
    if (ip) {
        queued_event_t address = {.ip_event=true,.id=IP_EVENT_STA_GOT_IP,.data.got_ip={.esp_netif=station_netif,.ip_info={.ip={123}}}};
        observed_station_ip = 0;
        handle_event(&address,clock_ms);
        assert(state.phase == PW_ASSOCIATED && !commits);
        observed_station_ip = 123;
        handle_event(&address,clock_ms);
    }
}
static void test_join_and_persistence(void)
{
    stage_request(); got_connection(true);
    assert(state.phase == PW_ONLINE && commits == 1 && store.count == 1 && state.active_profile == 1);
    assert(state.pending_id == 1001 && state.request_state == PW_REQUEST_SAVED);
    assert(!desired_ap && !state.setup_open);
    passport_wifi_store_t restored;
    assert(passport_wifi_store_load(&restored) == ESP_OK && restored.records[0].id == 1);
    event(false,WIFI_EVENT_STA_DISCONNECTED); assert(!switching && fallback_at == clock_ms + 30000);
    step(clock_ms + 4999); assert(connects == 1);
    step(clock_ms + 5000); assert(connects == 2 && !switching);
    step(clock_ms + 29999); assert(!switching);
    step(clock_ms + 30000); assert(switching);
}
static void test_save_failure(void)
{
    test_scan_and_ap(); stage_request(); commit_error = ESP_FAIL; got_connection(true);
    assert(!state.storage_known && store.count == 0 && switching && strstr(state.result,"uncertain"));
    assert(state.request_state == PW_REQUEST_FAILED && strstr(state.request_result,"uncertain"));
    assert(!state.pending && !state.setup_open);
}
static void test_release_and_lock(void)
{
    reset_runtime(); next_ap = true; start_radio(clock_ms); event(false,WIFI_EVENT_AP_START);
    mock_ble.connected = true; mock_ble.generation = 9; mock_ble.cancel_revision = 2; mock_ble.release_pending = true;
    begin_switch(false,true,clock_ms); step(clock_ms); assert(waiting_release && !stops);
    ++mock_ble.cancel_revision; step(clock_ms); assert(waiting_release && !stops);
    mock_ble.release_pending = false; step(clock_ms); assert(waiting_stop && stops == 1);
    reset_runtime(); state.ap_started = state.dhcp_running = true;
    open_setup(clock_ms); mock_ble.locked = true; step(clock_ms); assert(!state.setup_open);
    command_local(PW_SCAN,0,0); assert(!state.scanning);
}
static void test_rejection_and_recovery(void)
{
    test_scan_and_ap();
    store = (passport_wifi_store_t){.version=1,.count=1,.next_id=2,.preferred=1,
        .records={{.id=1,.ssid_length=3,.ssid="Old",.security=PW_WPA2,.password_length=8,.password="old12345"}}};
    passport_wifi_store_t old = store;
    stage_request();
    event(false,WIFI_EVENT_STA_DISCONNECTED);
    assert(switching && !memcmp(&store,&old,sizeof(old)) && !commits);
    assert(state.request_state == PW_REQUEST_FAILED);
    test_scan_and_ap(); stage_request();
    step(clock_ms + JOIN_MS);
    assert(switching && !store.count && !commits && strstr(state.result,"timeout"));
    test_scan_and_ap(); open_setup(clock_ms);
    queued_command_t request = {.submission=true,.credentials={.setup_id=state.setup_id,.request_id=1001,.scan_revision=state.scan_revision-1,.row=0,.password="fixture123"}};
    memcpy(request.token,setup_token,33);
    handle_submission(&request); assert(!state.pending);
    request.credentials.scan_revision=state.scan_revision;
    memset(request.token,'x',33); handle_submission(&request); assert(!state.pending);
    memcpy(request.token,setup_token,33); handle_submission(&request); assert(state.pending);
    uint32_t approved_id=state.pending_id, old_setup=state.setup_id;
    char approved_password[64]; memcpy(approved_password,proposed.password,64);
    snprintf(request.credentials.password,64,"different"); handle_submission(&request);
    assert(state.pending_id==approved_id && !memcmp(proposed.password,approved_password,64));
    open_setup(clock_ms); assert(!state.pending && !proposed.password[0] && state.setup_id!=old_setup && state.request_state == PW_REQUEST_CANCELLED);
    command_local(PW_CONFIRM,approved_id,old_setup); assert(!switching && !state.pending);
    step(clock_ms+SETUP_MS); assert(!state.setup_open);
    test_scan_and_ap();
    mock_ble.connected=true; mock_ble.release_pending=true;
    begin_switch(false,true,clock_ms); step(clock_ms+501);
    assert(!switching && !stops && strstr(state.result,"unconfirmed"));
    atomic_store(&event_loss,true); step(clock_ms+502);
    assert(!state.initialized && !state.setup_open && strstr(state.result,"event loss"));
}
static void test_profile_store(void)
{
    passport_wifi_store_t snapshot={.version=1,.count=1,.next_id=2,.preferred=1,
        .records={{.id=1,.ssid_length=32,.security=PW_WPA2,.password_length=8,.password="fixture1"}}};
    memset(snapshot.records[0].ssid,'X',32);
    snapshot.records[0].ssid[0]=0xE4; snapshot.records[0].ssid[1]=0xB8; snapshot.records[0].ssid[2]=0xAD;
    assert(passport_wifi_store_save(&snapshot)==ESP_OK && !nvs_opened);
    passport_wifi_store_t restored;
    assert(passport_wifi_store_load(&restored)==ESP_OK && !memcmp(&restored,&snapshot,sizeof(restored)));
    passport_ssid_t ssid=record_ssid(&restored.records[0]); char name[72];
    passport_wifi_ssid_text(&ssid,name,sizeof(name)); assert(strlen(name)==68 && !strncmp(name,"hex:E4B8AD",10));
    write_error=ESP_FAIL; assert(passport_wifi_store_save(&snapshot)==ESP_FAIL && !nvs_opened); write_error=0;
    read_error=ESP_FAIL; assert(passport_wifi_store_load(&restored)==ESP_FAIL && !nvs_opened); read_error=0;
    durable.version=2; assert(passport_wifi_store_load(&restored)==ESP_ERR_INVALID_STATE);
    snapshot.records[0].ssid_length=33; assert(passport_wifi_store_save(&snapshot)==ESP_ERR_INVALID_ARG);
}

static void healthy_lan(void)
{
    reset_runtime();
    store = (passport_wifi_store_t){.version=1,.count=1,.next_id=2,.preferred=1,
        .records={{.id=1,.ssid_length=3,.ssid="Old",.security=PW_WPA2,.password_length=8,.password="old12345"}}};
    joining = store.records[0];
    state.phase = PW_ONLINE; state.active_profile = 1; state.active_ssid = record_ssid(&joining);
    snprintf(state.sta_ip,sizeof(state.sta_ip),"192.168.1.42");
    observed_station_ip = 42; desired_station = radio_started = true;
    mock_ble.connected = mock_ble.release_pending = true;
    mock_ble.generation = 9;
}
static void test_rejected_switch_preserves_live_network(void)
{
    for (unsigned immediate = 0; immediate < 2; ++immediate) {
        healthy_lan(); reject_cancel = immediate;
        command_local(PW_CONNECT,1,state.revision);
        step(clock_ms+501);
        assert(!switching && state.phase == PW_ONLINE && state.active_profile == 1);
        assert(!fallback_at && !retry_at && !join_deadline && !stops);
        step(clock_ms+FALLBACK_MS+1000);
        assert(!switching && !stops && !connects && state.phase == PW_ONLINE);
    }
    healthy_lan();
    command_local(PW_CONNECT,1,state.revision);
    clock_ms += 100; event(false,WIFI_EVENT_STA_DISCONNECTED);
    assert(state.phase == PW_OFFLINE && !state.sta_ip[0] && !state.active_profile);
    uint64_t lost_deadline = fallback_at;
    assert(lost_deadline == clock_ms+FALLBACK_MS);
    step(clock_ms+501);
    assert(!switching && state.phase == PW_OFFLINE && !stops && fallback_at == lost_deadline);
    step(clock_ms+5000);
    assert(connects == 1 && state.phase == PW_CONNECTING);
    step(lost_deadline);
    assert(switching); // Actual LAN loss still owns the normal recovery deadline.

    healthy_lan();
    command_local(PW_USE_AP,0,0);
    observed_station_ip = 0; event(true,IP_EVENT_STA_LOST_IP);
    step(clock_ms+501);
    assert(!switching && state.phase == PW_ASSOCIATED && !state.sta_ip[0] && !state.active_profile);
    assert(fallback_at == clock_ms+FALLBACK_MS && !stops);

    healthy_lan(); stop_error = ESP_FAIL; mock_ble.connected = false;
    command_local(PW_CONNECT,1,state.revision); step(clock_ms);
    assert(!switching && stops == 1 && !fallback_at && state.phase == PW_ONLINE);
    assert(state.sta_ip[0] && state.active_profile == 1);
    step(clock_ms+FALLBACK_MS+1000);
    assert(!switching && stops == 1 && state.phase == PW_ONLINE);
}
static void test_web_scan_window(void)
{
    test_scan_and_ap(); open_setup(clock_ms); publish();
    char token[33]; assert(passport_wifi_setup_token(token));
    uint32_t old_id=state.setup_id;
    assert(passport_wifi_scan(old_id,token));
    close_setup(); step(clock_ms); assert(scans==2 && !state.scanning);
    open_setup(clock_ms); publish();
    assert(passport_wifi_scan(old_id,token)); step(clock_ms); assert(scans==2);
    assert(passport_wifi_setup_token(token));
    assert(passport_wifi_scan(state.setup_id,token)); step(clock_ms);
    assert(scans==3 && state.scanning);
}
static void full_profiles(void)
{
    test_scan_and_ap();
    store = (passport_wifi_store_t){.version=1,.count=4,.next_id=5,.preferred=1,
        .records={{.id=1,.ssid_length=8,.ssid="Test LAN",.security=PW_WPA2,.password_length=8,.password="old12345"},
                  {.id=2,.ssid_length=3,.ssid="Two",.security=PW_OPEN},
                  {.id=3,.ssid_length=5,.ssid="Three",.security=PW_OPEN},
                  {.id=4,.ssid_length=4,.ssid="Four",.security=PW_OPEN}}};
    durable=store; persisted=true;
    command_local(PW_SCAN,0,0); event(false,WIFI_EVENT_SCAN_DONE);
    assert(state.rows[0].profile_id==1); // Production scan-to-profile identity match.
}
static void test_full_profile_update(void)
{
    full_profiles(); passport_wifi_store_t old=store;
    stage_request();
    assert(joining.id==1 && !memcmp(&store,&old,sizeof(old)) && !commits);
    event(false,WIFI_EVENT_STA_DISCONNECTED);
    assert(state.request_state==PW_REQUEST_FAILED && !commits);
    assert(!memcmp(&store,&old,sizeof(old)) && !memcmp(&durable,&old,sizeof(old)));

    full_profiles(); old=store; stage_request();
    queued_event_t associated={.id=WIFI_EVENT_STA_CONNECTED,.data.connected={.ssid="Test LAN",.ssid_len=8,.authmode=WIFI_AUTH_WPA2_PSK,.bssid={1}}};
    handle_event(&associated,clock_ms);
    assert(state.phase==PW_ASSOCIATED && !commits && !memcmp(&store,&old,sizeof(old)));
    queued_event_t address={.ip_event=true,.id=IP_EVENT_STA_GOT_IP,.data.got_ip={.esp_netif=station_netif,.ip_info={.ip={123}}}};
    observed_station_ip=123; handle_event(&address,clock_ms);
    assert(state.request_state==PW_REQUEST_SAVED && commits==1 && store.count==4 && store.next_id==5);
    assert(store.records[0].id==1 && !strcmp(store.records[0].password,"fixture123"));
    assert(!memcmp(&store.records[1],&old.records[1],3*sizeof(store.records[0])));

    full_profiles(); old=store;
    memcpy(fixture_rows[0].ssid,"Fifth LAN",10);
    command_local(PW_SCAN,0,0); event(false,WIFI_EVENT_SCAN_DONE);
    assert(!state.rows[0].profile_id);
    open_setup(clock_ms);
    queued_command_t message={.submission=true,.credentials={.setup_id=state.setup_id,.request_id=2001,.scan_revision=state.scan_revision,.row=0,.password="fixture123"}};
    memcpy(message.token,setup_token,33); handle_submission(&message);
    assert(!state.pending && !state.submission_accepted && strstr(state.result,"Profiles full"));
    assert(!commits && !memcmp(&store,&old,sizeof(old)));
}

static void test_scan_names(void)
{
    reset_runtime();
    mock_ble.synced = true;
    state.scan_known = true;
    state.scan_revision = 42;
    state.scan_count = 2;
    state.rows[0] = (passport_wifi_row_t){.ssid = {.bytes = "AP", .length = 2},
        .bssid = {0, 0, 0, 0, 0, 11}, .auth = WIFI_AUTH_WPA2_PSK, .security = PW_WPA2};
    state.rows[1] = state.rows[0];
    state.rows[1].bssid[5] = 21;
    passport_name_change_t change = {.key = passport_wifi_ap_name_key(&state.rows[0]),
                                     .revision = 1, .scan_revision = 41};
    assert(wifi_name_change(&change, &mock_ble) == PN_STALE);
    change.scan_revision = 42;
    mock_ble.locked = true;
    assert(wifi_name_change(&change, &mock_ble) == PN_LOCKED);
    mock_ble.locked = false;
    change.key.ap.auth ^= 1;
    assert(wifi_name_change(&change, &mock_ble) == PN_NOT_FOUND);
    change.key = passport_wifi_ap_name_key(&state.rows[0]);
    queued_name = change;
    name_queued = true;
    step(clock_ms);
    assert(name_result == PN_OK && name_applied == 1);
    state.scan_known = false;
    change.clear = true;
    assert(wifi_name_change(&change, &mock_ble) == PN_OK);
    state.scan_known = true;
    change.clear = false;
    store = (passport_wifi_store_t){.version = 1, .next_id = 3, .count = 1, .preferred = 1,
        .records = {{.id = 1, .ssid = "AP", .ssid_length = 2, .security = PW_WPA2,
                     .password = "test1234", .password_length = 8}}};
    assert(wifi_name_change(&change, &mock_ble) == PN_INVALID);
    char label[PASSPORT_NAME_BYTES + 1];
    assert(passport_wifi_scan_label(&state, &state.rows[0], label) == PN_OK && !strcmp(label, "Exact AP"));
    assert(passport_wifi_scan_label(&state, &state.rows[1], label) == PN_OK && !strcmp(label, "Profile label"));
    store.records[1] = store.records[0]; store.records[1].id = 2; ++store.count;
    refresh_profiles();
    assert(passport_wifi_scan_label(&state, &state.rows[1], label) == PN_NOT_FOUND && !label[0]);
    name_remove_result = PN_UNCERTAIN;
    command_local(PW_FORGET, 1, state.revision);
    assert(name_removals == 1 && removed_name.kind == PN_WIFI_PROFILE && removed_name.profile == 1);
    assert(store.count == 1 && store.records[0].id == 2 && strstr(state.result, "cleanup failed"));
}

static void test_remote_scan_receipts(void)
{
    state.scan_revision=11;state.scanning=false;scan_draining=false;switching=false;associating=false;
    mock_ble.locked=true;
    passport_wifi_command_t c={.action=PW_SCAN,.revision=11,.remote_scan_id=91};
    unsigned before=scans;handle_command(&c,clock_ms);assert(state.scan_request_id==91&&state.scan_request_result==PW_SCAN_LOCKED&&scans==before);
    mock_ble.locked=false;c.revision=10;handle_command(&c,clock_ms);assert(state.scan_request_result==PW_SCAN_STALE&&scans==before);
    c.revision=11;handle_command(&c,clock_ms);assert(state.scan_request_result==PW_SCAN_ACCEPTED&&scans==before+1&&state.scanning);
    c.remote_scan_id=92;c.revision=state.scan_revision;handle_command(&c,clock_ms);assert(state.scan_request_result==PW_SCAN_BUSY&&scans==before+1);
    c.action=PW_SCAN_CANCEL;handle_command(&c,clock_ms);assert(state.scan_request_result==PW_SCAN_ACCEPTED&&!state.scanning&&scan_draining);
}

int main(void)
{
    test_init(); test_scan_and_ap(); test_join_and_persistence(); test_save_failure(); test_release_and_lock(); test_rejection_and_recovery(); test_profile_store(); test_rejected_switch_preserves_live_network(); test_web_scan_window(); test_full_profile_update();
    test_scan_names();
    vQueueDelete(commands); vQueueDelete(events);
    test_remote_scan_receipts();
    puts("PASS: Wi-Fi initialization rollback, AP/DHCP observations, scan cancellation, exact setup confirmation, IP-before-save, reconnect deadline, NVS uncertainty and BLE release boundary");
}
