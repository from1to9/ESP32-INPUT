// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_wifi.c"

static unsigned manager_lock_depth;
static void (*manager_unlock_hook)(void);
void wifi_test_enter(void) { assert(!manager_lock_depth); ++manager_lock_depth; }
void wifi_test_exit(void) {
    assert(manager_lock_depth==1); --manager_lock_depth;
    if (manager_unlock_hook) { void (*hook)(void)=manager_unlock_hook; manager_unlock_hook=NULL; hook(); }
}
static uint64_t clock_ms = 100;
static esp_netif_t sta_netif = {1}, ap_netif = {2};
static uint8_t durable[STORE_LENGTH];
static bool exists;
static uint8_t profile_banks[2][PROFILE_BANK_BYTES];
static bool profile_exists[2];
static int bank_index(const char *key) { return !strcmp(key,"profiles_a")?0:!strcmp(key,"profiles_b")?1:-1; }
static bool durable_profiles_present(void) {
    int slot=-1;
    for(unsigned i=0;i<2;++i) if(profile_exists[i] && (slot<0 || profile_u64(profile_banks[i]+4)>profile_u64(profile_banks[slot]+4))) slot=(int)i;
    return slot<0?exists:profile_banks[slot][12]!=0;
}
static void corrupt_active_store(void) {
    int slot=-1;
    for(unsigned i=0;i<2;++i) if(profile_exists[i] && (slot<0 || profile_u64(profile_banks[i]+4)>profile_u64(profile_banks[slot]+4))) slot=(int)i;
    if(slot<0) durable[0]=0; else profile_banks[slot][0]=0;
}
static bool linked, apply_failed_set, apply_failed_erase;
static esp_err_t store_error, commit_error, connect_error, http_error, mode_error;
static esp_err_t set_blob_error, erase_error, http_stop_error, station_config_error;
static unsigned commits, connects, stops, mode_calls, random_generation, destroyed_netifs, driver_deinits;
static int mode;
static wifi_config_t station_config, ap_config;
static char response[2048], response_status[64];
static void (*created_task)(void *);
static wifi_ap_record_t scan_aps[40];
static uint16_t scan_ap_count, scan_ap_index;
static unsigned scans, scan_stops, scan_clears, scan_reads;
static unsigned scan_read_fail_at;
static esp_err_t scan_start_error, scan_stop_error, scan_clear_error, scan_read_error, scan_count_error;
static size_t free_heap = 131072, largest_block = 65536;
static size_t free_dma = 12911, largest_dma = 11776;
static bool ip_assigned = true;
static bool fail_ap_netif;
static unsigned ap_creates;
static unsigned dns_clears, tcpip_calls, clock_ready_calls;
static bool tcpip_owner, clock_ready;
static esp_err_t tcpip_error;
static void (*tcpip_hook)(void);
static void (*ip_read_hook)(void);
static esp_err_t ip_read_error;

void app_clock_station_ready(bool ready)
{
    assert(manager_lock_depth == 1);
    clock_ready = ready;
    ++clock_ready_calls;
}
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn fn, void *context)
{
    assert(!manager_lock_depth && !tcpip_owner);
    ++tcpip_calls;
    if (tcpip_error != ESP_OK) return tcpip_error;
    tcpip_owner = true;
    if (tcpip_hook) { void (*hook)(void) = tcpip_hook; tcpip_hook = NULL; hook(); }
    esp_err_t result = fn(context);
    tcpip_owner = false;
    return result;
}
void dns_clear_cache(void)
{
    assert(tcpip_owner && !manager_lock_depth);
    ++dns_clears;
}

size_t heap_caps_get_free_size(unsigned caps) {
    assert(caps == 3U || caps == 5U); return caps == 3U ? free_heap : free_dma;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
    assert(caps == 3U || caps == 5U); return caps == 3U ? largest_block : largest_dma;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) {
    assert(!block && config->max_scan_ap_num == APP_WIFI_SCAN_CAPACITY && config->show_hidden);
    assert(config->scan_time.active.max == 60 && config->home_chan_dwell_time == 30);
    assert(config->coex_background_scan && !s_connect_until);
    ++scans; return scan_start_error;
}
esp_err_t esp_wifi_scan_stop(void) { ++scan_stops; return scan_stop_error; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) { *count = scan_ap_count; return scan_count_error; }
esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *record) {
    ++scan_reads;
    if (scan_read_error && (!scan_read_fail_at || scan_reads == scan_read_fail_at)) return scan_read_error;
    assert(scan_ap_index < scan_ap_count);
    *record = scan_aps[scan_ap_index++]; return ESP_OK;
}
esp_err_t esp_wifi_clear_ap_list(void) {
    ++scan_clears;
    if (!scan_clear_error) scan_ap_count = scan_ap_index = 0;
    return scan_clear_error;
}

int64_t esp_timer_get_time(void) { return (int64_t)(clock_ms * 1000U); }
void esp_fill_random(void *data, size_t length) {
    ++random_generation;
    for (size_t i = 0; i < length; ++i) ((uint8_t *)data)[i] = (uint8_t)(i + random_generation);
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    (void)name; (void)arg; (void)handle;
    assert(stack == 16384 && priority == 2); created_task = task; return pdPASS;
}
void vTaskDelay(unsigned ticks) { (void)ticks; abort(); }
void vTaskDelete(void *task) { (void)task; }
esp_err_t esp_event_loop_create_default(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)arg; assert((base == WIFI_EVENT && ((id == WIFI_EVENT_STA_DISCONNECTED && handler == wifi_event) ||
        (id == WIFI_EVENT_SCAN_DONE && handler == scan_event))) ||
        (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP && handler == wifi_event));
    return ESP_OK;
}
esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *)) {
    assert((base == WIFI_EVENT && ((id == WIFI_EVENT_STA_DISCONNECTED && handler == wifi_event) ||
        (id == WIFI_EVENT_SCAN_DONE && handler == scan_event))) ||
        (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP && handler == wifi_event));
    return ESP_OK;
}
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &sta_netif; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) {
    ++ap_creates; return fail_ap_netif ? NULL : &ap_netif;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info) {
    if (netif == &sta_netif && ip_read_error != ESP_OK) return ip_read_error;
    info->ip.addr = netif == &ap_netif ? 0x0104a8c0U : linked && ip_assigned ? 0x1401a8c0U : 0;
    if (netif == &sta_netif && ip_read_hook) {
        void (*hook)(void) = ip_read_hook; ip_read_hook = NULL; hook();
    }
    return ESP_OK;
}
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name) {
    assert(netif == &ap_netif); strcpy(name, "ap0"); return ESP_OK;
}
void esp_netif_destroy_default_wifi(void *netif) {
    assert(netif == &sta_netif || netif == &ap_netif); ++destroyed_netifs;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { assert(storage == WIFI_STORAGE_RAM); return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config) {
    if (interface == WIFI_IF_STA) {if(station_config_error)return station_config_error;station_config = *config;}
    else { assert(interface == WIFI_IF_AP); ap_config = *config; }
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int value) {
    ++mode_calls; if (mode_error) return mode_error; mode = value; return ESP_OK;
}
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_stop(void) { linked = false; return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { ++driver_deinits; return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { linked = false; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { ++connects; return connect_error; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info) {
    if (!linked) return ESP_ERR_WIFI_NOT_CONNECT;
    memcpy(info->ssid, station_config.sta.ssid, 32); info->ssid[32] = 0; info->rssi = -51;
    info->bssid[5] = 1;
    return ESP_OK;
}
esp_err_t nvs_open(const char *name, int access, nvs_handle_t *handle) {
    assert(strcmp(name, "app_wifi") == 0);
    if (store_error) return store_error;
    if (access == NVS_READONLY && !exists && !profile_exists[0] && !profile_exists[1]) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
    assert(handle==1); int i=bank_index(key);
    const void *source=durable; size_t size=sizeof(durable); bool present=exists;
    if(i>=0) {source=profile_banks[i];size=PROFILE_BANK_BYTES;present=profile_exists[i];}
    else assert(!strcmp(key,"network"));
    if(!present) return ESP_ERR_NVS_NOT_FOUND;
    if(value) {assert(*length>=size);memcpy(value,source,size);} *length=size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    assert(handle==1); int i=bank_index(key);assert(i>=0&&length==PROFILE_BANK_BYTES);
    if(set_blob_error==ESP_OK||apply_failed_set) {memcpy(profile_banks[i],value,length);profile_exists[i]=true;}
    return set_blob_error;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    assert(handle == 1 && strcmp(key, "network") == 0);
    if (erase_error == ESP_OK || apply_failed_erase) {
        exists = false; memset(durable, 0, sizeof(durable));
    }
    return erase_error;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1); if (commit_error) return commit_error;
    /* Like the pinned SDK, commit does not apply a staged transaction. A
     * faulted acknowledgement cannot undo an earlier set_blob/erase. */
    ++commits;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config) {
    assert(config->if_name && strcmp(config->if_name->ifr_name, "ap0") == 0);
    assert(config->server_port == 80 && config->max_open_sockets == 2 && config->recv_wait_timeout == 2);
    if (http_error) return http_error; *server = (void *)1; return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t server) { assert(server == (void *)1); ++stops; return http_stop_error; }
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri) {
    assert(server == (void *)1 && (strcmp(uri->uri, "/") == 0 || strcmp(uri->uri, "/configure") == 0));
    return ESP_OK;
}
static const char *header(httpd_req_t *request, const char *name) {
    if (strcmp(name, "Host") == 0) return request->host;
    if (strcmp(name, "Origin") == 0) return request->origin;
    if (strcmp(name, "Content-Type") == 0) return request->content_type;
    return NULL;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *name) {
    const char *value = header(request, name); return value ? strlen(value) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t length) {
    const char *source = header(request, name); assert(source && strlen(source) < length);
    strcpy(value, source); return ESP_OK;
}
int httpd_req_recv(httpd_req_t *request, char *output, size_t length) {
    if (request->timeout) { clock_ms += 2000U; return -3; }
    if (request->fragment && length > request->fragment) length = request->fragment;
    memcpy(output, request->body + request->offset, length); request->offset += length; return (int)length;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *value) {
    (void)r; assert(strlen(value) < sizeof(response_status)); strcpy(response_status, value); return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *value) { (void)r; (void)value; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value) {
    (void)r; (void)name; (void)value; return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *body) {
    (void)r; assert(strlen(body) < sizeof(response)); strcpy(response, body); return ESP_OK;
}

static app_wifi_snapshot_t snapshot(void) {
    app_wifi_snapshot_t value; app_wifi_get_snapshot(&value); return value;
}
static void reset(bool preserve) {
    memset(&s_network, 0, sizeof(s_network));
    dns_clears = tcpip_calls = clock_ready_calls = 0;
    tcpip_owner = clock_ready = false; tcpip_error = ip_read_error = ESP_OK;
    tcpip_hook = ip_read_hook = NULL;
    memset(&s_snapshot, 0, sizeof(s_snapshot)); memset(&s_setup, 0, sizeof(s_setup));
    memset(&s_pending, 0, sizeof(s_pending)); memset(s_token, 0, sizeof(s_token));
    s_profile_request_counter=s_profile_active_request=s_profile_completed_request=0;s_profile_operation_error=0;memset(s_profile_results,0,sizeof(s_profile_results));s_profile_result_cursor=0;
    s_maintenance=false;
    s_commands = 0; s_started = s_accept_form = s_disconnected = s_close_on_connect = false;
    s_ap_cleanup_needed = false;
    s_sta = s_ap = NULL; fail_ap_netif = false; ap_creates = 0;
    s_server = NULL; s_setup_until = s_retry_at = s_connect_until = s_close_at = 0;
    s_cleanup_retry_at = 0;
    s_retry_delay = 1000; s_generation = s_station_generation = s_event_generation = 0;
    memset(&s_scan, 0, sizeof(s_scan)); memset(s_scan_records, 0, sizeof(s_scan_records));
    memset(&s_scan_diagnostics, 0, sizeof(s_scan_diagnostics));
    s_scan_wait_event = s_scan_done = s_scan_stop_sent = s_scan_cancelled = false;
    s_scan_status = 0;
    s_scan_deadline = s_scan_completed_at = s_scan_next_at = s_scan_cleanup_at = s_scan_cancel_until = 0;
    s_pending_remember = false; s_pending_scan_generation = 0; s_pending_record_index = 0;
    s_selection_counter = s_selection_id = s_selection_scan_generation = 0;
    s_selection_index = s_selection_authmode = 0;
    s_selection_until = s_pending_selection_until = 0;
    memset(s_selection_ssid, 0, sizeof(s_selection_ssid));
    memset(scan_aps, 0, sizeof(scan_aps)); scan_ap_count = scan_ap_index = 0;
    scans = scan_stops = scan_clears = scan_reads = 0;
    scan_read_fail_at = 0;
    scan_start_error = scan_stop_error = scan_clear_error = scan_read_error = scan_count_error = ESP_OK;
    free_heap = 131072; largest_block = 65536; ip_assigned = true;
    free_dma = 12911; largest_dma = 11776;
    linked = false; store_error = commit_error = connect_error = http_error = mode_error = ESP_OK;
    set_blob_error = erase_error = http_stop_error = station_config_error = ESP_OK;
    apply_failed_set = apply_failed_erase = false;
    commits = connects = stops = mode_calls = 0; clock_ms = 100;
    memset(s_quarantine,0,sizeof(s_quarantine));
    memset(&s_profiles,0,sizeof(s_profiles));memset(&s_profile_request,0,sizeof(s_profile_request));
    if (!preserve) { exists = false; memset(durable, 0, sizeof(durable));memset(profile_banks,0,sizeof(profile_banks));memset(profile_exists,0,sizeof(profile_exists)); }
    assert(app_wifi_init() == ESP_OK && created_task == worker);
    assert(snapshot().operation_pending && !snapshot().initialized);
    assert(initialize_worker() == ESP_OK);
}
static void start(void) {
    assert(app_wifi_start_setup() == ESP_OK); service_step();
    assert(snapshot().setup_active && mode == WIFI_MODE_APSTA);
    assert(ap_config.ap.authmode == WIFI_AUTH_WPA2_PSK && ap_config.ap.max_connection == 1);
}
static httpd_req_t request(const char *body) {
    return (httpd_req_t){.host = "192.168.4.1", .origin = "http://192.168.4.1",
        .content_type = "application/x-www-form-urlencoded", .body = body, .content_len = strlen(body), .fragment = 7};
}
static void submit(const char *ssid) {
    char body[256]; snprintf(body, sizeof(body), "token=%s&ssid=%s&password=test-passphrase", s_token, ssid);
    httpd_req_t r = request(body); assert(form_post(&r) == ESP_OK);
    assert(strcmp(response_status, "202 Accepted") == 0);
    assert(strstr(response, "test-passphrase") == NULL && strstr(response, s_token) == NULL);
}
static void test_setup_and_reconnect(void) {
    reset(false); assert(!snapshot().configured && !snapshot().setup_active && connects == 0);
    assert(app_wifi_init() == ESP_ERR_INVALID_STATE); start();
    app_wifi_setup_info_t info; assert(app_wifi_get_setup_info(&info) == ESP_OK);
    assert(strlen(info.password) == 24 && info.remaining_ms <= APP_WIFI_SETUP_TIMEOUT_MS);
    submit("Test%20Network"); assert(!snapshot().configured && snapshot().operation_pending);
    service_step(); assert(snapshot().configured && commits == 1 && connects == 1);
    assert(strcmp(snapshot().ssid, "Test Network") == 0 && !snapshot().rssi_valid);
    linked = true; service_step(); assert(snapshot().state == APP_WIFI_CONNECTED);
    assert(snapshot().rssi_valid && snapshot().rssi_dbm == -51 && strcmp(snapshot().ip, "192.168.1.20") == 0);
    clock_ms += 5001; service_step(); assert(!snapshot().setup_active && stops == 1 && mode == WIFI_MODE_STA);
    assert(app_wifi_get_setup_info(&info) == ESP_ERR_INVALID_STATE && info.password[0] == 0);
    reset(true); assert(snapshot().configured && !snapshot().setup_active);
    clock_ms += 1; service_step(); assert(connects == 1);
    wifi_event_sta_disconnected_t event = {.reason = 201};
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event); service_step();
    assert(snapshot().state == APP_WIFI_BACKOFF && snapshot().disconnect_reason == 201 && snapshot().retry_in_ms == 1000);
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event); service_step();
    assert(snapshot().retry_in_ms == 1000); /* Duplicate events do not postpone the retry. */
    clock_ms += 999; service_step(); assert(connects == 1);
    clock_ms += 1; service_step(); assert(connects == 2);
    for (unsigned i = 0; i < 10; ++i) {
        clock_ms += CONNECT_TIMEOUT_MS; service_step(); assert(snapshot().retry_in_ms <= RETRY_MAX_MS);
        clock_ms += snapshot().retry_in_ms; service_step();
    }
    assert(!snapshot().setup_active);
    linked = true; service_step(); assert(snapshot().state == APP_WIFI_CONNECTED && snapshot().connection_error == ESP_OK);
    assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(!durable_profiles_present() && !snapshot().configured && snapshot().state == APP_WIFI_IDLE && !linked);
    unsigned before = connects; clock_ms += 60000; service_step(); assert(connects == before);
}
static void test_security_and_limits(void) {
    reset(false); start();
    char body[256], old_token[33]; strcpy(old_token, s_token);
    snprintf(body, sizeof(body), "token=%s&ssid=Example&password=test-passphrase", s_token);
    httpd_req_t r = request(body); r.origin = "http://hostile.example"; form_post(&r);
    assert(strcmp(response_status, "403 Forbidden") == 0 && s_commands == 0);
    r = request(body); r.host = "192.168.4.1.attacker"; form_post(&r); assert(s_commands == 0);
    r = request(body); r.origin = NULL; form_post(&r); assert(s_commands == 0);
    r = request(body); r.content_len = FORM_LIMIT + 1; form_post(&r); assert(s_commands == 0);
    r = request(body); r.timeout = true; form_post(&r); assert(s_commands == 0);
    r = request(body); form_get(&r); assert(strstr(response, s_token) && !strstr(response, "test-passphrase"));
    assert(app_wifi_stop_setup() == ESP_OK); r = request(body); form_post(&r); assert(!(s_commands & SAVE_NETWORK));
    service_step(); start(); assert(strcmp(old_token, s_token) != 0);
    r = request(body); form_post(&r); assert(!(s_commands & SAVE_NETWORK));
    submit("%3Cscript%3E%26"); service_step();
    r = request(""); form_get(&r); assert(strstr(response, "&lt;script&gt;&amp;") && !strstr(response, "<script>"));
    clock_ms += APP_WIFI_SETUP_TIMEOUT_MS; service_step(); assert(!snapshot().setup_active);
    start(); submit("Stale"); assert(app_wifi_stop_setup() == ESP_OK); service_step();
    assert(strcmp(snapshot().ssid, "<script>&") == 0);
    credentials_t value; char token[33];
    const char *bad[] = {"ssid=a&ssid=b&password=12345678&token=00000000000000000000000000000000",
        "ssid=%00&password=12345678&token=00000000000000000000000000000000",
        "ssid=%ff&password=12345678&token=00000000000000000000000000000000",
        "ssid=one&password=short&token=00000000000000000000000000000000",
        "ssid=one&password=12345678&token=00000000000000000000000000000000&extra=x",
        "ssid=%x0&password=12345678&token=00000000000000000000000000000000"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&value, 0, sizeof(value)); assert(!parse_form(bad[i], strlen(bad[i]), &value, token));
    }
}
static void test_failures(void) {
    reset(false); start(); commit_error = ESP_FAIL; submit("Failed"); service_step();
    assert(durable_profiles_present() && !snapshot().configured && snapshot().last_error == ESP_FAIL);
    assert(snapshot().storage_uncertain && !snapshot().stored_data_known);
    assert(app_wifi_stop_setup() == ESP_OK); service_step(); start(); commit_error = ESP_OK;
    assert(app_wifi_verify_storage()==ESP_OK);service_step();submit("Good"); service_step(); linked = true; service_step(); assert(snapshot().configured);
    assert(app_wifi_stop_setup()==ESP_OK);service_step(); commit_error = ESP_FAIL; assert(app_wifi_profiles_forget_all(s_profiles.revision,true) == ESP_OK); service_step();
    assert(!durable_profiles_present() && snapshot().configured && snapshot().last_error == ESP_FAIL);
    assert(app_wifi_start_setup() == ESP_OK); http_error = ESP_ERR_NO_MEM; service_step();
    assert(!snapshot().setup_active && mode == WIFI_MODE_STA && snapshot().last_error == ESP_ERR_NO_MEM);
    reset(false); start(); submit("Seed"); service_step();
    credentials_t credentials = {0}; bool known, present;
    corrupt_active_store(); assert(read_credentials(&credentials, &known, &present) == ESP_ERR_INVALID_STATE);
    assert(known && present); reset(true);
    assert(snapshot().initialized && !snapshot().configured && snapshot().last_error == ESP_ERR_INVALID_STATE);
    assert(app_wifi_forget_network() == ESP_OK); service_step();
    start(); submit("Recovered"); service_step(); assert(snapshot().configured && snapshot().last_error == ESP_OK);
    assert(app_wifi_stop_setup() == ESP_OK); service_step();
    start(); submit("Expired"); clock_ms += APP_WIFI_SETUP_TIMEOUT_MS; service_step();
    assert(strcmp(snapshot().ssid, "Recovered") == 0 && !snapshot().setup_active);
    mode_error = ESP_FAIL;
    assert(initialize_worker() == ESP_FAIL);
    assert(!snapshot().initialized && destroyed_netifs == 2 && driver_deinits == 1);
}

static void test_cleanup_recovery(void) {
    for (unsigned failure = 0; failure < 3; ++failure) {
        reset(false); start();
        char body[256]; snprintf(body, sizeof(body), "token=%s&ssid=NoReplay&password=test-passphrase", s_token);
        mode_error = failure != 1 ? ESP_FAIL : ESP_OK;
        http_stop_error = failure != 0 ? ESP_FAIL : ESP_OK;
        if (failure == 1) clock_ms += APP_WIFI_SETUP_TIMEOUT_MS;
        else assert(app_wifi_stop_setup() == ESP_OK);
        service_step();
        assert(snapshot().setup_active && snapshot().cleanup_pending && snapshot().operation_pending);
        assert(snapshot().cleanup_error == ESP_FAIL && snapshot().setup_remaining_ms == 0);
        assert(snapshot().cleanup_retry_in_ms == CLEANUP_RETRY_MS);
        app_wifi_setup_info_t info;
        assert(app_wifi_get_setup_info(&info) == ESP_ERR_INVALID_STATE && info.password[0] == 0);
        httpd_req_t r = request(body); form_post(&r);
        assert(strcmp(response_status, "403 Forbidden") == 0 && !(s_commands & SAVE_NETWORK));
        r = request(""); form_get(&r); assert(strcmp(response_status, "403 Forbidden") == 0);
        assert(app_wifi_start_setup() == ESP_ERR_INVALID_STATE);
        unsigned before_stops = stops, before_modes = mode_calls;
        for (unsigned i = 0; i < 50; ++i) {
            clock_ms += 10;
            assert(app_wifi_stop_setup() == ESP_OK);
            service_step();
        }
        assert(stops == before_stops && mode_calls == before_modes);
        clock_ms += 499; service_step(); assert(stops == before_stops && mode_calls == before_modes);
        mode_error = http_stop_error = ESP_OK;
        clock_ms += 1; service_step();
        assert(!snapshot().cleanup_pending && !snapshot().setup_active && !snapshot().operation_pending);
        assert(snapshot().cleanup_error == ESP_OK && snapshot().cleanup_retry_in_ms == 0);
        assert(mode == WIFI_MODE_STA && s_server == NULL);
        before_stops = stops; before_modes = mode_calls;
        for (unsigned i = 0; i < 20; ++i) { clock_ms += 1000; service_step(); }
        assert(stops == before_stops && mode_calls == before_modes);
        start(); r = request(body); form_post(&r); assert(strcmp(response_status, "403 Forbidden") == 0);
    }
    reset(false); start(); mode_error = http_stop_error = ESP_FAIL;
    assert(app_wifi_stop_setup() == ESP_OK); service_step();
    unsigned before_modes = mode_calls, before_stops = stops;
    for (unsigned i = 0; i < 25; ++i) { clock_ms += 200; service_step(); }
    assert(mode_calls == before_modes + 5 && stops == before_stops + 5);
    assert(snapshot().cleanup_pending && !s_accept_form);
    mode_error = http_stop_error = ESP_OK; clock_ms += 1000; service_step();
    assert(!snapshot().setup_active && !snapshot().cleanup_pending);
}

static void seed_network(void) {
    reset(false); start(); submit("Original"); service_step();
    assert(app_wifi_stop_setup() == ESP_OK); service_step();
    linked = true; service_step();
    assert(snapshot().configured && snapshot().stored_data_known && snapshot().stored_data_present);
    assert(!snapshot().storage_uncertain && strcmp(snapshot().stored_ssid, "Original") == 0);
}
static void test_storage_uncertainty(void) {
    for (unsigned applied = 0; applied < 2; ++applied) {
        seed_network(); start();
        set_blob_error = ESP_FAIL; apply_failed_set = applied != 0;
        submit("Replacement"); service_step();
        assert(snapshot().storage_uncertain && !snapshot().stored_data_known);
        assert(snapshot().storage_error == ESP_FAIL && snapshot().stored_ssid[0] == 0);
        assert(snapshot().configured && strcmp(snapshot().ssid, "Original") == 0);
        httpd_req_t get = request(""); form_get(&get);
        assert(strstr(response, "Runtime network: Original") &&
            strstr(response, "Storage contents are uncertain") && !strstr(response, "Saved network:"));
        linked = true; service_step(); assert(snapshot().state == APP_WIFI_CONNECTED);
        assert(snapshot().storage_uncertain && snapshot().storage_error == ESP_FAIL);
        assert(app_wifi_stop_setup() == ESP_OK); service_step(); start();
        assert(snapshot().storage_uncertain && snapshot().storage_error == ESP_FAIL);
        set_blob_error = ESP_OK;
        assert(app_wifi_verify_storage() == ESP_OK); service_step();
        assert(!snapshot().storage_uncertain && snapshot().storage_error == ESP_OK);
        assert(snapshot().stored_data_known && snapshot().stored_data_present);
        assert(strcmp(snapshot().stored_ssid, applied ? "Replacement" : "Original") == 0);
        assert(strcmp(snapshot().ssid, "Original") == 0); /* Verification never changes runtime. */

        seed_network(); erase_error = ESP_FAIL; apply_failed_erase = applied != 0;
        assert(app_wifi_forget_network() == ESP_OK); service_step();
        assert(snapshot().configured && strcmp(snapshot().ssid, "Original") == 0);
        assert(snapshot().storage_uncertain && !snapshot().stored_data_known && snapshot().storage_error == ESP_FAIL);
        assert(!durable_profiles_present());
        linked = true; service_step(); assert(snapshot().storage_uncertain);
        erase_error = ESP_OK;
        assert(app_wifi_verify_storage() == ESP_OK); service_step();
        assert(snapshot().stored_data_known && !snapshot().stored_data_present);
        assert(!snapshot().storage_uncertain && snapshot().configured);
        assert(app_wifi_forget_network() == ESP_OK); service_step();
        assert(snapshot().stored_data_known && !snapshot().stored_data_present && !snapshot().configured);
        assert(!snapshot().storage_uncertain && snapshot().storage_error == ESP_OK);
    }
    seed_network(); start(); commit_error = ESP_FAIL; submit("WrittenBeforeError"); service_step();
    assert(snapshot().storage_uncertain && strcmp(snapshot().ssid, "Original") == 0);
    reset(true); assert(strcmp(snapshot().ssid, "WrittenBeforeError") == 0 && !snapshot().storage_uncertain);
    assert(app_wifi_stop_setup()==ESP_OK);service_step(); commit_error = ESP_FAIL; assert(app_wifi_profiles_forget_all(s_profiles.revision,true) == ESP_OK); service_step();
    assert(snapshot().storage_uncertain && snapshot().configured && !durable_profiles_present());
    reset(true); assert(!snapshot().configured && snapshot().stored_data_known && !snapshot().stored_data_present);
}

static void test_unreadable_recovery(void) {
    seed_network(); corrupt_active_store(); reset(true);
    assert(!snapshot().configured && snapshot().ssid[0] == 0 && snapshot().stored_ssid[0] == 0);
    assert(snapshot().stored_data_known && snapshot().stored_data_present && snapshot().storage_uncertain);
    assert(snapshot().storage_error == ESP_ERR_INVALID_STATE);
    start(); assert(app_wifi_stop_setup() == ESP_OK); service_step();
    assert(snapshot().storage_error == ESP_ERR_INVALID_STATE && snapshot().storage_uncertain);
    assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(snapshot().stored_data_known && !snapshot().stored_data_present && !snapshot().storage_uncertain);
    store_error = ESP_FAIL;
    assert(app_wifi_verify_storage() == ESP_OK); service_step();
    assert(!snapshot().stored_data_known && snapshot().storage_uncertain && !snapshot().configured);
    store_error = ESP_OK;
    assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(snapshot().stored_data_known && !snapshot().stored_data_present && !snapshot().storage_uncertain);
}

static app_wifi_scan_snapshot_t scan_snapshot(void) {
    app_wifi_scan_snapshot_t value; app_wifi_get_scan_snapshot(&value); return value;
}
static void scan_done(unsigned status) {
    wifi_event_sta_scan_done_t event = {.status = status};
    scan_event(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &event);
}
static void scan_row(unsigned index, const char *ssid, int authmode) {
    assert(index < 40 && strlen(ssid) <= 32);
    scan_aps[index] = (wifi_ap_record_t){.primary = 6, .rssi = -42, .authmode = authmode};
    memcpy(scan_aps[index].ssid, ssid, strlen(ssid));
    scan_aps[index].bssid[5] = (uint8_t)(index + 1);
    if (scan_ap_count <= index) scan_ap_count = (uint16_t)(index + 1);
}
static app_wifi_join_request_t join_request(const char *ssid, bool remember) {
    app_wifi_join_request_t value = {.remember = remember};
    assert(strlen(ssid) <= 32); strcpy(value.ssid, ssid);
    strcpy(value.password, "synthetic-passphrase"); return value;
}
static void begin_scan(void) {
    assert(app_wifi_scan_start() == ESP_OK);
    assert(scan_snapshot().state == APP_WIFI_SCAN_QUEUED);
    service_step(); assert(scan_snapshot().state == APP_WIFI_SCAN_RUNNING);
}

static void test_device_station_provisioning(void) {
    reset(false);
    assert(mode == WIFI_MODE_STA && !s_server && !snapshot().setup_active && !snapshot().lan_ready);
    assert(!s_ap && ap_creates == 0);
    assert(!snapshot().backend_available && snapshot().backend_error == ESP_ERR_NOT_SUPPORTED);
    begin_scan();
    assert(app_wifi_scan_start() == ESP_ERR_INVALID_STATE && app_wifi_start_setup() == ESP_ERR_INVALID_STATE);
    app_wifi_join_request_t join = join_request("Existing LAN", true);
    assert(app_wifi_join(&join) == ESP_ERR_INVALID_STATE);
    scan_row(0, "Existing LAN", WIFI_AUTH_WPA2_PSK);
    scan_row(1, "Open network", WIFI_AUTH_OPEN);
    scan_done(0); service_step();
    uint32_t generation = scan_snapshot().generation;
    assert(scan_snapshot().state == APP_WIFI_SCAN_READY && scan_snapshot().count == 2);
    assert(scan_snapshot().remaining_ms == APP_WIFI_SCAN_RESULT_TTL_MS && scan_clears == 1);
    app_wifi_scan_record_t row;
    assert(app_wifi_get_scan_record(generation, 0, &row) == ESP_OK);
    assert(row.availability == APP_WIFI_JOIN_AVAILABLE && row.security == APP_WIFI_SECURITY_WPA2);
    assert(strcmp(row.label, "Existing LAN") == 0 && row.rssi_dbm == -42 && row.channel == 6);
    assert(app_wifi_get_scan_record(generation + 1, 0, &row) == ESP_ERR_INVALID_STATE && !row.label[0]);
    join.scan_generation = generation; join.record_index = 1;
    strcpy(join.ssid, "Open network"); assert(app_wifi_join(&join) == ESP_ERR_INVALID_STATE);
    join.record_index = 0; assert(app_wifi_join(&join) == ESP_ERR_INVALID_STATE);
    strcpy(join.ssid, "Existing LAN"); assert(app_wifi_join(&join) == ESP_OK);
    assert(!snapshot().configured && !durable_profiles_present()); /* Admission has no radio/storage effect. */
    service_step();
    assert(snapshot().configured && durable_profiles_present() && commits == 1 && connects == 1 && !s_pending.password[0]);
    assert(!snapshot().setup_active && !s_server && mode == WIFI_MODE_STA);
    linked = true; ip_assigned = false; service_step();
    assert(snapshot().state == APP_WIFI_OBTAINING_IP && !snapshot().lan_ready && !snapshot().ip[0]);
    assert(snapshot().rssi_valid && app_wifi_scan_start() == ESP_ERR_INVALID_STATE);
    ip_assigned = true; service_step();
    assert(snapshot().state == APP_WIFI_CONNECTED && snapshot().lan_ready);
    assert(strcmp(snapshot().ip, "192.168.1.20") == 0 && !snapshot().backend_available);
    reset(true); clock_ms++; service_step();
    assert(snapshot().configured && connects == 1 && !snapshot().setup_active && !s_server);
    linked = true; service_step();
    join = join_request("One time LAN", false);
    assert(app_wifi_join(&join) == ESP_OK); service_step();
    assert(commits == 0 && strcmp(snapshot().ssid, "One time LAN") == 0);
    assert(strcmp(snapshot().stored_ssid, "Existing LAN") == 0 && !snapshot().lan_ready);
    reset(true); assert(strcmp(snapshot().ssid, "Existing LAN") == 0);
    join = join_request("Invalid", false); strcpy(join.password, "short");
    assert(app_wifi_join(&join) == ESP_ERR_INVALID_ARG);
    memset(join.ssid, 'x', sizeof(join.ssid)); strcpy(join.password, "synthetic-passphrase");
    assert(app_wifi_join(&join) == ESP_ERR_INVALID_ARG);
    assert(app_wifi_join(NULL) == ESP_ERR_INVALID_ARG);
    reset(false); fail_ap_netif = true;
    assert(app_wifi_start_setup() == ESP_OK); service_step();
    assert(snapshot().last_error == ESP_ERR_NO_MEM && !snapshot().setup_active && mode == WIFI_MODE_STA);
    join = join_request("Station independent", false);
    assert(app_wifi_join(&join) == ESP_OK); service_step();
    assert(snapshot().configured && ap_creates == 1 && !s_server);
}

static void test_scan_bounds_and_identity(void) {
    reset(false); begin_scan();
    for (unsigned i = 0; i < 40; ++i) scan_row(i, "Duplicate", WIFI_AUTH_WPA2_PSK);
    scan_aps[1].ssid[0] = 0xff;
    scan_row(2, "", WIFI_AUTH_WPA2_PSK);
    scan_row(3, "Enterprise", WIFI_AUTH_ENTERPRISE);
    scan_row(4, "WPA3", WIFI_AUTH_WPA3_PSK);
    scan_row(5, "OWE", WIFI_AUTH_OWE);
    scan_row(6, "WPA1", WIFI_AUTH_WPA_PSK);
    scan_row(7, "Unknown", 500);
    scan_row(8, "WPA2 transition", WIFI_AUTH_WPA_WPA2_PSK);
    scan_row(9, "12345678901234567890123456789012", WIFI_AUTH_WPA2_PSK);
    scan_row(10, "Before", WIFI_AUTH_WPA2_PSK); scan_aps[10].ssid[2] = 0;
    scan_done(0); service_step();
    assert(scan_snapshot().count == APP_WIFI_SCAN_CAPACITY && scan_snapshot().capacity_reached);
    assert(scan_reads == APP_WIFI_SCAN_CAPACITY && scan_clears == 1 && scan_ap_count == 0);
    uint32_t generation = scan_snapshot().generation;
    app_wifi_scan_record_t row;
    assert(app_wifi_get_scan_record(generation, 1, &row) == ESP_OK);
    assert(row.availability == APP_WIFI_JOIN_UNSUPPORTED_SSID && strstr(row.label, "\\xFF"));
    assert(app_wifi_get_scan_record(generation, 2, &row) == ESP_OK);
    assert(row.availability == APP_WIFI_JOIN_HIDDEN_SSID && strcmp(row.label, "Hidden network") == 0);
    for (unsigned i = 3; i < 8; ++i) {
        assert(app_wifi_get_scan_record(generation, (uint16_t)i, &row) == ESP_OK);
        assert(row.availability == APP_WIFI_JOIN_UNSUPPORTED_SECURITY);
    }
    assert(app_wifi_get_scan_record(generation, 8, &row) == ESP_OK && row.availability == APP_WIFI_JOIN_AVAILABLE);
    assert(app_wifi_get_scan_record(generation, 9, &row) == ESP_OK && row.ssid_length == 32 && strlen(row.label) == 32);
    assert(app_wifi_get_scan_record(generation, 10, &row) == ESP_OK &&
        row.availability == APP_WIFI_JOIN_UNSUPPORTED_SSID && strstr(row.label, "\\x00"));
    assert(app_wifi_get_scan_record(generation, 32, &row) == ESP_ERR_INVALID_STATE);
    assert(app_wifi_get_scan_record(generation, 0, NULL) == ESP_ERR_INVALID_ARG);
    assert(app_wifi_scan_start() == ESP_ERR_INVALID_STATE); /* Cooldown. */
    app_wifi_join_request_t join = join_request("Duplicate", false); join.scan_generation = generation;
    assert(app_wifi_join(&join) == ESP_OK);
    clock_ms += APP_WIFI_SCAN_RESULT_TTL_MS; service_step();
    assert(!snapshot().configured && snapshot().last_error == ESP_ERR_INVALID_STATE);
    assert(scan_snapshot().state == APP_WIFI_SCAN_EXPIRED);
    assert(app_wifi_get_scan_record(generation, 0, &row) == ESP_ERR_INVALID_STATE && !row.label[0]);
    assert(app_wifi_join(&join) == ESP_ERR_INVALID_STATE);
    begin_scan(); scan_done(0); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_READY && scan_snapshot().count == 0);
}

static void test_scan_cancel_cleanup_and_pressure(void) {
    reset(false);
    assert(app_wifi_scan_start() == ESP_OK && app_wifi_scan_cancel() == ESP_OK);
    service_step(); assert(scans == 0 && scan_snapshot().state == APP_WIFI_SCAN_CANCELLED);
    clock_ms += APP_WIFI_SCAN_COOLDOWN_MS; begin_scan();
    uint32_t old_generation = scan_snapshot().generation;
    assert(app_wifi_scan_cancel() == ESP_OK); service_step();
    assert(scan_stops == 1 && scan_snapshot().cleanup_pending);
    for (unsigned i = 0; i < 20; ++i) {
        clock_ms += 10; assert(app_wifi_scan_cancel() == ESP_OK); service_step();
    }
    assert(scan_stops == 1 && app_wifi_scan_start() == ESP_ERR_INVALID_STATE);
    clock_ms += 2000; service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && scan_snapshot().error == ESP_ERR_TIMEOUT);
    assert(scan_snapshot().cleanup_pending && s_scan_wait_event);
    app_wifi_join_request_t join = join_request("Blocked", false);
    assert(app_wifi_join(&join) == ESP_ERR_INVALID_STATE);
    scan_row(0, "Cancelled result", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && !scan_snapshot().cleanup_pending);
    clock_ms += APP_WIFI_SCAN_COOLDOWN_MS; begin_scan();
    scan_row(0, "Fresh result", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    app_wifi_scan_record_t row;
    assert(app_wifi_get_scan_record(old_generation, 0, &row) == ESP_ERR_INVALID_STATE);
    assert(app_wifi_get_scan_record(scan_snapshot().generation, 0, &row) == ESP_OK);
    assert(strcmp(row.label, "Fresh result") == 0);

    reset(false); begin_scan(); scan_stop_error = ESP_FAIL;
    assert(app_wifi_scan_cancel() == ESP_OK); service_step();
    for (unsigned i = 0; i < 50; ++i) { clock_ms += 10; service_step(); }
    assert(scan_stops == 1 && scan_snapshot().cleanup_error == ESP_FAIL);
    clock_ms += 500; service_step(); assert(scan_stops == 2);
    scan_done(1); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_CANCELLED && !scan_snapshot().cleanup_pending);

    reset(false); begin_scan(); clock_ms += APP_WIFI_SCAN_TIMEOUT_MS; service_step();
    assert(scan_stops == 1 && scan_snapshot().error == ESP_ERR_TIMEOUT);
    scan_done(0); service_step(); assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR);

    reset(false); begin_scan(); scan_row(0, "Unreadable", WIFI_AUTH_WPA2_PSK);
    scan_read_error = ESP_FAIL; scan_clear_error = ESP_FAIL; scan_done(0); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && scan_snapshot().cleanup_pending);
    unsigned before = scan_clears; clock_ms += 999; service_step(); assert(scan_clears == before);
    scan_clear_error = ESP_OK; clock_ms++; service_step();
    assert(!scan_snapshot().cleanup_pending && scan_snapshot().count == 0 && scan_ap_count == 0);
    reset(false); free_heap = 32767;
    assert(app_wifi_scan_start() == ESP_OK); service_step();
    assert(scans == 0 && scan_snapshot().error == ESP_ERR_NO_MEM);
    reset(false); largest_block = 16383;
    assert(app_wifi_scan_start() == ESP_OK); service_step(); assert(scans == 0);
    reset(false); scan_start_error = ESP_ERR_NO_MEM;
    assert(app_wifi_scan_start() == ESP_OK); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && scan_snapshot().error == ESP_ERR_NO_MEM);
    reset(false); begin_scan(); scan_done(1); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && scan_snapshot().error == ESP_FAIL);
    reset(false); begin_scan(); scan_count_error = ESP_FAIL; scan_done(0); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && scan_reads == 0 && scan_clears == 1);
}

static app_wifi_scan_diagnostics_t scan_diagnostics(void) {
    app_wifi_scan_diagnostics_t value;
    app_wifi_get_scan_diagnostics(&value);
    return value;
}
static void queue_scan_at_completion(void) {
    if (s_scan.state != APP_WIFI_SCAN_READY) {
        manager_unlock_hook = queue_scan_at_completion;
        return;
    }
    assert(app_wifi_scan_start() == ESP_OK);
}
static void cancel_at_heap_observation(void) {
    if (!s_scan_diagnostics.current.heap_known) {
        manager_unlock_hook = cancel_at_heap_observation;
        return;
    }
    assert(app_wifi_scan_cancel() == ESP_OK);
}
static void test_scan_diagnostics_preserve_failed_generation(void) {
    reset(false);
    assert(!scan_diagnostics().current.known && !scan_diagnostics().last_failure.known);
    app_wifi_get_scan_diagnostics(NULL);
    seed_network();
    assert(snapshot().lan_ready && snapshot().state == APP_WIFI_CONNECTED);
    free_heap = 38731; largest_block = 25600;
    scan_start_error = ESP_ERR_WIFI_STATE; scan_clear_error = ESP_FAIL;
    assert(app_wifi_scan_start() == ESP_OK);
    assert(scan_diagnostics().current.phase == APP_WIFI_SCAN_PHASE_QUEUED &&
           !scan_diagnostics().current.heap_known);
    service_step();
    app_wifi_scan_diagnostics_t failed = scan_diagnostics();
    assert(scan_snapshot().state == APP_WIFI_SCAN_ERROR && scan_snapshot().cleanup_pending);
    assert(failed.current.phase == APP_WIFI_SCAN_PHASE_CLEAR && failed.current.error == ESP_FAIL &&
           failed.last_failure.known);
    assert(failed.last_failure.phase == APP_WIFI_SCAN_PHASE_SDK_START &&
           failed.last_failure.error == ESP_ERR_WIFI_STATE && !failed.last_failure.driver_admitted);
    assert(failed.last_failure.station_state == APP_WIFI_CONNECTED && failed.last_failure.lan_ready);
    assert(failed.last_failure.heap_known && failed.last_failure.free_internal == 38731 &&
           failed.last_failure.largest_internal == 25600 && failed.last_failure.free_dma == 12911 &&
           failed.last_failure.largest_dma == 11776 && !failed.last_failure.event_status_known &&
           !failed.last_failure.count_known);
    assert(app_wifi_scan_start() == ESP_ERR_INVALID_STATE);
    scan_clear_error = scan_start_error = ESP_OK;
    clock_ms += CLEANUP_RETRY_MS; service_step();
    assert(!scan_snapshot().cleanup_pending);
    assert(!memcmp(&failed.last_failure, &s_scan_diagnostics.last_failure, sizeof(failed.last_failure)));
    clock_ms += APP_WIFI_SCAN_COOLDOWN_MS;
    begin_scan(); scan_row(0, "Fresh after failure", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    app_wifi_scan_diagnostics_t success = scan_diagnostics();
    assert(scan_snapshot().state == APP_WIFI_SCAN_READY && scan_snapshot().count == 1);
    assert(success.current.driver_admitted && success.current.event_status_known &&
           success.current.event_status == 0 && success.current.count_known &&
           success.current.driver_count == 1 && success.current.read_count == 1);
    assert(success.current.generation > failed.last_failure.generation &&
           !memcmp(&success.last_failure, &failed.last_failure, sizeof(success.last_failure)));
    scan_done(99); /* An unowned late event cannot overwrite either observation. */
    assert(!memcmp(&success, &s_scan_diagnostics, sizeof(success)));

    clock_ms += APP_WIFI_SCAN_COOLDOWN_MS; begin_scan();
    scan_row(0, "First", WIFI_AUTH_WPA2_PSK); scan_row(1, "Second", WIFI_AUTH_WPA2_PSK);
    scan_read_error = ESP_ERR_NO_MEM; scan_read_fail_at = scan_reads + 2; scan_clear_error = ESP_FAIL;
    scan_done(0); service_step();
    app_wifi_scan_diagnostics_t read_failure = scan_diagnostics();
    assert(read_failure.last_failure.generation == read_failure.current.generation &&
           read_failure.last_failure.generation > failed.last_failure.generation);
    assert(read_failure.last_failure.phase == APP_WIFI_SCAN_PHASE_RECORD &&
           read_failure.last_failure.error == ESP_ERR_NO_MEM && read_failure.last_failure.driver_count == 2 &&
           read_failure.last_failure.read_count == 1 && read_failure.last_failure.event_status_known);
    assert(scan_snapshot().count == 0 && scan_snapshot().cleanup_error == ESP_FAIL);

    reset(false); begin_scan(); scan_count_error = ESP_FAIL; scan_done(0); service_step();
    assert(scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_COUNT &&
           !scan_diagnostics().last_failure.count_known && scan_reads == 0);
    reset(false); begin_scan(); scan_row(0, "Readable", WIFI_AUTH_WPA2_PSK);
    scan_clear_error = ESP_ERR_INVALID_STATE; scan_done(0); service_step();
    assert(scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_CLEAR &&
           scan_diagnostics().last_failure.error == ESP_ERR_INVALID_STATE &&
           scan_diagnostics().last_failure.read_count == 1 && scan_snapshot().cleanup_pending);
    reset(false); begin_scan(); scan_stop_error = ESP_FAIL;
    assert(app_wifi_scan_cancel() == ESP_OK); service_step();
    assert(scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_STOP &&
           scan_diagnostics().last_failure.error == ESP_FAIL && s_scan_wait_event);

    reset(false); free_heap = 32767;
    assert(app_wifi_scan_start() == ESP_OK); service_step();
    assert(!scans && scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_HEAP &&
           scan_diagnostics().last_failure.error == ESP_ERR_NO_MEM);
    reset(false); free_heap = 32767;
    assert(app_wifi_scan_start() == ESP_OK);
    manager_unlock_hook = cancel_at_heap_observation;
    service_step();
    assert(!manager_unlock_hook && !scans && scan_snapshot().state == APP_WIFI_SCAN_CANCELLED &&
           !scan_diagnostics().last_failure.known);
    reset(false); free_heap = 32768; largest_block = 16384;
    begin_scan(); scan_done(7); service_step();
    assert(scans == 1 && scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_EVENT &&
           scan_diagnostics().last_failure.event_status_known &&
           scan_diagnostics().last_failure.event_status == 7);
    reset(false); begin_scan();
    scan_event(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL); service_step();
    assert(scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_EVENT &&
           !scan_diagnostics().last_failure.event_status_known);
    reset(false); begin_scan(); scan_stop_error = ESP_FAIL;
    clock_ms += APP_WIFI_SCAN_TIMEOUT_MS; service_step();
    assert(scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_WAIT_EVENT &&
           scan_diagnostics().last_failure.error == ESP_ERR_TIMEOUT);
    scan_done(0); service_step();
    assert(scan_diagnostics().last_failure.phase == APP_WIFI_SCAN_PHASE_WAIT_EVENT);
    reset(false); begin_scan(); assert(app_wifi_scan_cancel() == ESP_OK); service_step();
    scan_done(1); service_step();
    assert(scan_snapshot().state == APP_WIFI_SCAN_CANCELLED && !scan_diagnostics().last_failure.known);

    reset(false); begin_scan(); clock_ms += APP_WIFI_SCAN_COOLDOWN_MS; scan_done(0);
    manager_unlock_hook = queue_scan_at_completion;
    service_step();
    assert(!manager_unlock_hook && scan_snapshot().state == APP_WIFI_SCAN_QUEUED &&
           scan_diagnostics().current.phase == APP_WIFI_SCAN_PHASE_QUEUED &&
           !scan_diagnostics().current.heap_known && !scan_diagnostics().last_failure.known);
}

static void test_station_storage_and_scan_reconnect(void) {
    seed_network();
    app_wifi_join_request_t join = join_request("Replacement", true);
    set_blob_error = ESP_FAIL; apply_failed_set = true;
    assert(app_wifi_join(&join) == ESP_OK); service_step();
    assert(snapshot().storage_uncertain && strcmp(snapshot().ssid, "Original") == 0);
    join = join_request("RAM only", false);
    assert(app_wifi_join(&join) == ESP_OK); service_step();
    assert(snapshot().storage_uncertain && strcmp(snapshot().ssid, "RAM only") == 0);
    reset(true); assert(strcmp(snapshot().ssid, "Replacement") == 0);
    clock_ms++; service_step(); linked = true; service_step(); begin_scan();
    scan_row(0, "Replacement", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    app_wifi_scan_record_t row;
    assert(app_wifi_get_scan_record(scan_snapshot().generation, 0, &row) == ESP_OK);
    assert(row.current && row.remembered && snapshot().lan_ready && linked);
    clock_ms += APP_WIFI_SCAN_COOLDOWN_MS; linked = false; service_step();
    assert(snapshot().state == APP_WIFI_BACKOFF && !snapshot().lan_ready && !snapshot().ip[0]);
    begin_scan(); unsigned before = connects;
    clock_ms += RETRY_MAX_MS; service_step(); assert(connects == before);
    scan_done(0); service_step(); assert(connects == before + 1);
    reset(false); begin_scan(); scan_row(0, "Discard", WIFI_AUTH_WPA2_PSK);
    assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(!snapshot().stored_data_known || snapshot().operation_pending);
    assert(s_commands & FORGET_NETWORK);
    scan_done(0); service_step();
    assert(!snapshot().configured && !snapshot().stored_data_present && !scan_snapshot().cleanup_pending);
    assert(scan_snapshot().count == 0 && !s_commands);
}

static app_wifi_join_request_t selected_join(const app_wifi_selection_t *selection) {
    char ssid[APP_WIFI_SSID_CAPACITY] = {0};
    memcpy(ssid, selection->record.ssid, selection->record.ssid_length);
    app_wifi_join_request_t request = join_request(ssid, false);
    request.selection_id = selection->id;
    request.scan_generation = selection->scan_generation;
    request.record_index = selection->record_index;
    return request;
}
static void test_selected_editor_reservations(void) {
    reset(false); begin_scan();
    scan_row(0, "Original selection", WIFI_AUTH_WPA2_PSK);
    scan_row(1, "Other selection", WIFI_AUTH_WPA2_PSK);
    scan_row(2, "Unsupported", WIFI_AUTH_OPEN);
    scan_done(0); service_step();
    uint32_t generation = scan_snapshot().generation;
    app_wifi_selection_t selection, newer;
    assert(app_wifi_select_scan_record(generation, 0, NULL) == ESP_ERR_INVALID_ARG);
    assert(app_wifi_select_scan_record(generation, 2, &newer) == ESP_ERR_INVALID_STATE && !newer.id);
    assert(app_wifi_select_scan_record(generation, 0, &selection) == ESP_OK);
    assert(selection.id && selection.remaining_ms == APP_WIFI_SELECTION_TIMEOUT_MS);
    app_wifi_join_request_t request = selected_join(&selection);
    strcpy(request.ssid, "Other selection"); assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE);
    request = selected_join(&selection); request.record_index = 1;
    assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE);
    clock_ms += APP_WIFI_SCAN_RESULT_TTL_MS + 1;
    assert(app_wifi_select_scan_record(generation, 0, &newer) == ESP_ERR_INVALID_STATE);
    request = selected_join(&selection); request.selection_id = 0;
    assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE); /* Ordinary stale row still fails. */
    begin_scan(); scan_row(0, "Replacement list", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    request = selected_join(&selection);
    assert(app_wifi_join(&request) == ESP_OK);
    assert(app_wifi_release_selection(selection.id) == ESP_ERR_INVALID_STATE); /* Consumed, not cancelled. */
    service_step(); assert(strcmp(snapshot().ssid, "Original selection") == 0 && connects == 1);
    assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE && connects == 1);

    reset(false); begin_scan();
    scan_row(0, "First editor", WIFI_AUTH_WPA2_PSK); scan_row(1, "Second editor", WIFI_AUTH_WPA2_PSK);
    scan_done(0); service_step(); generation = scan_snapshot().generation;
    assert(app_wifi_select_scan_record(generation, 0, &selection) == ESP_OK);
    assert(app_wifi_select_scan_record(generation, 1, &newer) == ESP_OK && newer.id != selection.id);
    assert(app_wifi_release_selection(selection.id) == ESP_ERR_INVALID_STATE);
    request = selected_join(&selection); assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE);
    assert(app_wifi_release_selection(newer.id) == ESP_OK);
    request = selected_join(&newer); assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE);
    assert(app_wifi_select_scan_record(generation, 0, &selection) == ESP_OK);
    request = selected_join(&selection); clock_ms += APP_WIFI_SELECTION_TIMEOUT_MS;
    assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE && !snapshot().configured);

    reset(false); begin_scan(); scan_row(0, "Queued ticket", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    assert(app_wifi_select_scan_record(scan_snapshot().generation, 0, &selection) == ESP_OK);
    request = selected_join(&selection);
    assert(app_wifi_join(&request) == ESP_OK); clock_ms += APP_WIFI_SELECTION_TIMEOUT_MS; service_step();
    assert(!snapshot().configured && snapshot().last_error == ESP_ERR_INVALID_STATE && !s_pending.password[0]);

    reset(false); begin_scan(); scan_row(0, "Forget ticket", WIFI_AUTH_WPA2_PSK); scan_done(0); service_step();
    assert(app_wifi_select_scan_record(scan_snapshot().generation, 0, &selection) == ESP_OK);
    request = selected_join(&selection);
    assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(app_wifi_join(&request) == ESP_ERR_INVALID_STATE && !snapshot().configured);
}

static void test_selection_owner_domains(void) {
    reset(false); begin_scan(); scan_row(0,"Private editor",WIFI_AUTH_WPA2_PSK);
    scan_done(0); service_step(); uint32_t generation=scan_snapshot().generation;
    app_wifi_selection_t local,remote,other;
    app_wifi_selection_owner_t a={.principal_id={1},.session_id={2}},b={.principal_id={1},.session_id={3}},invalid={0};
    assert(app_wifi_select_scan_record(generation,0,&local)==ESP_OK);
    app_wifi_join_request_t request=selected_join(&local);
    assert(app_wifi_select_scan_record_owned(generation,0,&a,&remote)==ESP_ERR_INVALID_STATE && !remote.id);
    assert(app_wifi_release_selection_owned(local.id,&a)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_join_owned(&request,&a)==ESP_ERR_INVALID_STATE && !snapshot().operation_pending);
    request=join_request("Manual remote",false);
    assert(app_wifi_join_owned(&request,&a)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_release_selection(local.id)==ESP_OK);
    assert(app_wifi_select_scan_record_owned(generation,0,&a,&remote)==ESP_OK);
    assert(app_wifi_select_scan_record_owned(generation,0,&b,&other)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_select_scan_record(generation,0,&other)==ESP_ERR_INVALID_STATE);
    request=selected_join(&remote);
    assert(app_wifi_join(&request)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_join_owned(&request,&b)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_join_owned(&request,&invalid)==ESP_ERR_INVALID_ARG);
    assert(app_wifi_release_selection(remote.id)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_release_selection_owned(remote.id,&b)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_join_owned(&request,&a)==ESP_OK); service_step();
    assert(connects==1 && !s_pending.password[0]);
}

static void test_multiple_profiles(void) {
    reset(false);
    app_wifi_profiles_snapshot_t view;app_wifi_profiles_snapshot(&view);
    assert(view.known&&!view.uncertain&&view.count==0&&view.capacity==8);
    char ids[8][33]={{0}};
    for(unsigned i=0;i<8;++i){
        app_wifi_profile_save_t save={.expected_revision=view.revision,.enabled=true};
        snprintf(save.ssid,sizeof(save.ssid),"Profile %u",i);strcpy(save.password,"test-passphrase");
        assert(app_wifi_profile_save(&save)==ESP_OK);assert(!s_profile_request.save.id[0]);
        service_step();app_wifi_profiles_snapshot(&view);assert(view.count==i+1&&!view.uncertain&&view.error==ESP_OK);
        assert(!s_profile_request.save.password[0]);strcpy(ids[i],view.profiles[i].id);
    }
    app_wifi_profile_save_t overflow={.expected_revision=view.revision,.enabled=true};strcpy(overflow.ssid,"Overflow");strcpy(overflow.password,"test-passphrase");
    assert(app_wifi_profile_save(&overflow)==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);assert(view.count==8&&!view.uncertain&&snapshot().last_error==ESP_ERR_NO_MEM);
    app_wifi_join_request_t remembered=join_request("Overflow",true);assert(app_wifi_join(&remembered)==ESP_ERR_NO_MEM);
    assert(app_wifi_forget_network()==ESP_ERR_INVALID_STATE); /* No unnamed eight-profile deletion. */
    assert(app_wifi_profile_join(ids[0],view.revision-1)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_profile_join(ids[3],view.revision)==ESP_OK);service_step();assert(!strcmp(snapshot().ssid,"Profile 3"));
    linked=true;service_step();unsigned before=connects;uint64_t previous=view.revision;
    char reversed[8][33];for(unsigned i=0;i<8;++i)strcpy(reversed[i],ids[7-i]);
    assert(app_wifi_profiles_reorder((const char (*)[33])reversed,8,previous)==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);
    assert(!strcmp(view.profiles[0].id,ids[7])&&snapshot().lan_ready&&connects==before);
    strcpy(reversed[1],reversed[0]);assert(app_wifi_profiles_reorder((const char (*)[33])reversed,8,view.revision)==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);
    assert(view.count==8&&view.revision==previous+1&&!view.uncertain&&snapshot().last_error==ESP_ERR_INVALID_ARG);
    assert(app_wifi_profile_forget(ids[5],view.revision)==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);assert(view.count==7&&snapshot().lan_ready);
    reset(true);app_wifi_profiles_snapshot(&view);assert(view.count==7&&!strcmp(snapshot().ssid,"Profile 7"));
    clock_ms++;service_step();wifi_event_sta_disconnected_t auth={.reason=WIFI_REASON_AUTH_FAIL};wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&auth);service_step();
    assert(strcmp(snapshot().ssid,"Profile 7")!=0&&snapshot().retry_in_ms<=RETRY_MAX_MS);
    app_wifi_profile_save_t edit={.expected_revision=view.revision,.keep_password=true,.enabled=true};strcpy(edit.id,view.profiles[0].id);strcpy(edit.ssid,"Renamed");
    set_blob_error=ESP_FAIL;apply_failed_set=true;assert(app_wifi_profile_save(&edit)==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);assert(view.uncertain&&!view.known);
    set_blob_error=ESP_OK;assert(app_wifi_verify_storage()==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);assert(view.known&&!view.uncertain&&!strcmp(view.profiles[0].ssid,"Renamed"));
    corrupt_active_store();assert(app_wifi_verify_storage()==ESP_OK);service_step();app_wifi_profiles_snapshot(&view);assert(!view.known&&view.uncertain);
    assert(app_wifi_profiles_forget_all(view.revision,false)==ESP_OK);service_step();assert(snapshot().last_error==ESP_ERR_INVALID_STATE);
    uint64_t request_id=0;assert(app_wifi_profiles_forget_all_request(view.revision,true,&request_id)==ESP_OK&&request_id);app_wifi_profiles_snapshot(&view);assert(view.operation_pending&&view.request_id==request_id&&view.completed_request_id!=request_id);service_step();app_wifi_profiles_snapshot(&view);assert(view.known&&!view.uncertain&&view.count==0&&!view.operation_pending&&view.completed_request_id==request_id&&view.operation_error==ESP_OK);app_wifi_profile_result_t exact;assert(app_wifi_profile_result(request_id,&exact)==ESP_OK&&exact.request_id==request_id&&exact.storage_known&&!exact.storage_uncertain&&exact.error==ESP_OK);assert(app_wifi_profile_result(request_id+100,&exact)==ESP_ERR_NOT_FOUND);
    /* Existing WIF1 remains byte-for-byte intact during catalog migration. */
    reset(false);memcpy(durable,"WIF1",4);durable[4]=6;durable[5]=15;memcpy(durable+6,"Legacy",6);memcpy(durable+38,"test-passphrase",15);exists=true;
    uint8_t original[STORE_LENGTH];memcpy(original,durable,sizeof(original));reset(true);app_wifi_profiles_snapshot(&view);assert(view.count==1&&!strcmp(snapshot().ssid,"Legacy"));
    app_wifi_profile_save_t add={.expected_revision=view.revision,.enabled=true};strcpy(add.ssid,"Second");strcpy(add.password,"test-passphrase");assert(app_wifi_profile_save(&add)==ESP_OK);service_step();
    assert(exists&&!memcmp(original,durable,sizeof(original)));app_wifi_profiles_snapshot(&view);assert(view.count==2);
    assert(app_wifi_profile_forget("00000000000000000000000000000001",view.revision)==ESP_OK);service_step();assert(!exists);
}

static char interleaved_profile_id[33];
static uint64_t interleaved_profile_revision;
static void forget_at_profile_join_unlock(void) {
    assert(!manager_lock_depth);
    /* The worker mailbox must already be claimed at the first unlocked point. */
    assert(s_snapshot.operation_pending && (s_commands & JOIN_NETWORK));
    assert(app_wifi_profile_forget(interleaved_profile_id,interleaved_profile_revision)==ESP_ERR_INVALID_STATE);
}
static void test_profile_join_atomic_admission(void) {
    reset(false);
    app_wifi_profile_save_t save={.expected_revision=s_profiles.revision,.enabled=true};
    strcpy(save.ssid,"Atomic saved profile");strcpy(save.password,"test-passphrase");
    assert(app_wifi_profile_save(&save)==ESP_OK);service_step();assert(s_profiles.count==1);
    strcpy(interleaved_profile_id,s_profiles.items[0].id);interleaved_profile_revision=s_profiles.revision;
    manager_unlock_hook=forget_at_profile_join_unlock;
    assert(app_wifi_profile_join(interleaved_profile_id,interleaved_profile_revision)==ESP_OK);
    assert(!manager_unlock_hook && s_profiles.count==1);service_step();assert(connects==1);
    linked=true;service_step();
    assert(app_wifi_profile_forget(interleaved_profile_id,interleaved_profile_revision)==ESP_OK);service_step();
    assert(!s_profiles.count && !snapshot().operation_pending);
    assert(app_wifi_profile_join(interleaved_profile_id,interleaved_profile_revision)==ESP_ERR_INVALID_STATE);
    assert(!snapshot().operation_pending && !s_pending.password[0]);
}

static void test_profile_radio_failure_preserves_durable_evidence(void) {
    seed_network();app_wifi_profiles_snapshot_t view;app_wifi_profiles_snapshot(&view);
    uint64_t request_id=0;station_config_error=ESP_FAIL;
    assert(app_wifi_profiles_forget_all_request(view.revision,true,&request_id)==ESP_OK);service_step();
    app_wifi_profile_result_t result;assert(app_wifi_profile_result(request_id,&result)==ESP_OK&&result.error==ESP_FAIL&&result.storage_known&&!result.storage_uncertain);
    app_wifi_profiles_snapshot(&view);assert(view.known&&!view.uncertain&&view.count==0);
    assert(snapshot().storage_error==ESP_OK&&!snapshot().storage_uncertain&&snapshot().connection_error==ESP_FAIL);
    station_config_error=ESP_OK;
}
static void lost_ip(void) { wifi_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL); }
static void entry_is_closed(void) { assert(!snapshot().lan_ready && !clock_ready); }
static void lose_after_entry_commit(void) { manager_unlock_hook = lost_ip; }
static void disconnect_during_entry(void)
{
    linked = false;
    wifi_event_sta_disconnected_t event = {.reason = WIFI_REASON_ASSOC_LEAVE};
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
static void test_network_entry_dns_barrier(void)
{
    seed_network();
    assert(snapshot().lan_ready && clock_ready && !dns_clears && !tcpip_calls);
    unsigned clock_publications = clock_ready_calls, before_connects = connects;
    for (unsigned i = 0; i < 20; ++i) { clock_ms += 100; service_step(); }
    assert(app_wifi_verify_storage() == ESP_OK); service_step();
    app_wifi_profile_save_t save = {.expected_revision = s_profiles.revision, .enabled = true};
    strcpy(save.ssid, "Other profile"); strcpy(save.password, "test-passphrase");
    assert(app_wifi_profile_save(&save) == ESP_OK); service_step();
    assert(!dns_clears && !tcpip_calls && clock_ready_calls == clock_publications);
    uint32_t network_generation = s_network.generation;
    ip_read_error = ESP_FAIL; service_step();
    assert(!clock_ready && !s_network.pending && s_network.generation == network_generation);
    ip_read_error = ESP_OK; linked = true; service_step();
    assert(snapshot().lan_ready && clock_ready && !dns_clears && !tcpip_calls);

    /* A real lost-IP event is visible even if IP returns between worker polls,
     * with the same SSID and address. Only the owner can reopen admission. */
    lost_ip(); entry_is_closed(); tcpip_hook = entry_is_closed; service_step();
    assert(snapshot().lan_ready && clock_ready && dns_clears == 1 && tcpip_calls == 1);
    for (unsigned i = 0; i < 20; ++i) { clock_ms += 100; service_step(); }
    assert(dns_clears == 1 && connects == before_connects);
    wifi_event_sta_disconnected_t event = {.reason = WIFI_REASON_ASSOC_LEAVE};
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
    entry_is_closed(); service_step();
    assert(snapshot().lan_ready && clock_ready && dns_clears == 2);

    /* Failure stays closed without repeated core submissions on every poll. */
    lost_ip(); tcpip_error = ESP_ERR_NO_MEM; service_step();
    unsigned calls = tcpip_calls;
    assert(!snapshot().lan_ready && !clock_ready && snapshot().connection_error == ESP_ERR_NO_MEM);
    clock_ms += NETWORK_ENTRY_RETRY_MS - 1; service_step(); assert(tcpip_calls == calls);
    tcpip_error = ESP_OK; ++clock_ms; service_step();
    assert(snapshot().lan_ready && clock_ready && tcpip_calls == calls + 1 && dns_clears == 3);

    /* A loss between SDK observations and admission invalidates that observation. */
    ip_read_hook = lost_ip; service_step();
    assert(!snapshot().lan_ready && !clock_ready && dns_clears == 3 && !snapshot().ip[0]);
    service_step(); assert(snapshot().lan_ready && clock_ready && dns_clears == 4);

    /* A second real loss during the owner callback cannot be published ready. */
    lost_ip(); tcpip_hook = lost_ip; service_step();
    assert(!snapshot().lan_ready && !clock_ready && dns_clears == 5);
    service_step(); assert(snapshot().lan_ready && clock_ready && dns_clears == 6);

    /* Nor can the short window after clock admission but before Wi-Fi publish. */
    lost_ip(); tcpip_hook = lose_after_entry_commit; service_step();
    assert(!snapshot().lan_ready && !clock_ready && dns_clears == 7);
    service_step(); assert(snapshot().lan_ready && clock_ready && dns_clears == 8);
    assert(connects == before_connects);

    /* A late entry return must also preserve the ordinary radio retry path. */
    lost_ip(); tcpip_hook = disconnect_during_entry; service_step();
    assert(!snapshot().lan_ready && !clock_ready && !snapshot().ip[0]);
    service_step(); assert(snapshot().state == APP_WIFI_BACKOFF && snapshot().retry_in_ms > 0);
    linked = true; service_step(); assert(snapshot().lan_ready && clock_ready && dns_clears == 10);
    ip_read_error = ESP_FAIL; service_step();
    assert(!clock_ready && !s_network.pending && dns_clears == 10);
    ip_read_error = ESP_OK; linked = true; ip_assigned = false; service_step();
    assert(s_network.pending && !clock_ready && dns_clears == 10);
    linked = ip_assigned = true; service_step();
    assert(snapshot().lan_ready && clock_ready && dns_clears == 11);
}
int main(void) {
    test_setup_and_reconnect(); test_security_and_limits(); test_failures();
    test_cleanup_recovery(); test_storage_uncertainty(); test_unreadable_recovery();
    test_profile_join_atomic_admission();
    test_selection_owner_domains();
    test_device_station_provisioning(); test_scan_bounds_and_identity();
    test_scan_cancel_cleanup_and_pressure(); test_station_storage_and_scan_reconnect();
    test_scan_diagnostics_preserve_failed_generation();
    test_network_entry_dns_barrier();
    test_selected_editor_reservations();test_multiple_profiles();test_profile_radio_failure_preserves_durable_evidence();
    reset(false);start();app_wifi_set_maintenance(true);assert(!s_accept_form);service_step();assert(!snapshot().setup_active);
    assert(app_wifi_start_setup()==ESP_ERR_INVALID_STATE&&app_wifi_forget_network()==ESP_ERR_INVALID_STATE&&app_wifi_scan_start()==ESP_ERR_INVALID_STATE);
    app_wifi_join_request_t blocked=join_request("Blocked",false);assert(app_wifi_join(&blocked)==ESP_ERR_INVALID_STATE);
    app_wifi_profile_save_t save={.expected_revision=s_profiles.revision,.enabled=true};strcpy(save.ssid,"Blocked");strcpy(save.password,"test-passphrase");assert(app_wifi_profile_save(&save)==ESP_ERR_INVALID_STATE);
    assert(app_wifi_verify_storage()==ESP_OK);service_step();app_wifi_set_maintenance(false);assert(app_wifi_scan_start()==ESP_OK);
    puts("PASS: production station scan/join, fallback setup, storage, reconnect and cancellation boundaries");
    return 0;
}
