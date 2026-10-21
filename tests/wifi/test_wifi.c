// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_wifi.c"

static uint64_t clock_ms = 100;
static esp_netif_t sta_netif = {1}, ap_netif = {2};
static uint8_t durable[STORE_LENGTH];
static bool exists, linked, apply_failed_set, apply_failed_erase;
static esp_err_t store_error, commit_error, connect_error, http_error, mode_error;
static esp_err_t set_blob_error, erase_error, http_stop_error;
static unsigned commits, connects, stops, mode_calls, random_generation, destroyed_netifs, driver_deinits;
static int mode;
static wifi_config_t station_config, ap_config;
static char response[2048], response_status[64];
static void (*created_task)(void *);

int64_t esp_timer_get_time(void) { return (int64_t)(clock_ms * 1000U); }
void esp_fill_random(void *data, size_t length) {
    ++random_generation;
    for (size_t i = 0; i < length; ++i) ((uint8_t *)data)[i] = (uint8_t)(i + random_generation);
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    (void)name; (void)arg; (void)handle;
    assert(stack == 8192 && priority == 2); created_task = task; return pdPASS;
}
void vTaskDelay(unsigned ticks) { (void)ticks; abort(); }
void vTaskDelete(void *task) { (void)task; }
esp_err_t esp_event_loop_create_default(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)arg; assert(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && handler == wifi_event);
    return ESP_OK;
}
esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *)) {
    assert(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && handler == wifi_event);
    return ESP_OK;
}
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &sta_netif; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { return &ap_netif; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info) {
    info->ip.addr = netif == &ap_netif ? 0x0104a8c0U : linked ? 0x1401a8c0U : 0;
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
    if (interface == WIFI_IF_STA) station_config = *config;
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
    return ESP_OK;
}
esp_err_t nvs_open(const char *name, int access, nvs_handle_t *handle) {
    assert(strcmp(name, "app_wifi") == 0);
    if (store_error) return store_error;
    if (access == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
    assert(handle == 1 && strcmp(key, "network") == 0);
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (value) { assert(*length >= sizeof(durable)); memcpy(value, durable, sizeof(durable)); }
    *length = sizeof(durable); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    assert(handle == 1 && strcmp(key, "network") == 0 && length == sizeof(durable));
    if (set_blob_error == ESP_OK || apply_failed_set) {
        memcpy(durable, value, length); exists = true;
    }
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
    memset(&s_snapshot, 0, sizeof(s_snapshot)); memset(&s_setup, 0, sizeof(s_setup));
    memset(&s_pending, 0, sizeof(s_pending)); memset(s_token, 0, sizeof(s_token));
    s_commands = 0; s_started = s_accept_form = s_disconnected = s_close_on_connect = false;
    s_ap_cleanup_needed = false;
    s_server = NULL; s_setup_until = s_retry_at = s_connect_until = s_close_at = 0;
    s_cleanup_retry_at = 0;
    s_retry_delay = 1000; s_generation = s_station_generation = s_event_generation = 0;
    linked = false; store_error = commit_error = connect_error = http_error = mode_error = ESP_OK;
    set_blob_error = erase_error = http_stop_error = ESP_OK;
    apply_failed_set = apply_failed_erase = false;
    commits = connects = stops = mode_calls = 0; clock_ms = 100;
    if (!preserve) { exists = false; memset(durable, 0, sizeof(durable)); }
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
    wifi_event_sta_disconnected_t event = {.reason = 202};
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event); service_step();
    assert(snapshot().state == APP_WIFI_BACKOFF && snapshot().disconnect_reason == 202 && snapshot().retry_in_ms == 1000);
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
    assert(!exists && !snapshot().configured && snapshot().state == APP_WIFI_IDLE && !linked);
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
    assert(exists && !snapshot().configured && snapshot().last_error == ESP_FAIL);
    assert(snapshot().storage_uncertain && !snapshot().stored_data_known);
    assert(app_wifi_stop_setup() == ESP_OK); service_step(); start(); commit_error = ESP_OK;
    submit("Good"); service_step(); linked = true; service_step(); assert(snapshot().configured);
    commit_error = ESP_FAIL; assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(!exists && snapshot().configured && snapshot().last_error == ESP_FAIL);
    assert(app_wifi_start_setup() == ESP_OK); http_error = ESP_ERR_NO_MEM; service_step();
    assert(!snapshot().setup_active && mode == WIFI_MODE_STA && snapshot().last_error == ESP_ERR_NO_MEM);
    reset(false); start(); submit("Seed"); service_step();
    credentials_t credentials = {0}; bool known, present;
    durable[0] = 0; assert(read_credentials(&credentials, &known, &present) == ESP_ERR_INVALID_STATE);
    assert(known && present); reset(true);
    assert(snapshot().initialized && !snapshot().configured && snapshot().last_error == ESP_ERR_INVALID_STATE);
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
        assert(exists == !applied);
        linked = true; service_step(); assert(snapshot().storage_uncertain);
        erase_error = ESP_OK;
        assert(app_wifi_verify_storage() == ESP_OK); service_step();
        assert(snapshot().stored_data_known && snapshot().stored_data_present == !applied);
        assert(!snapshot().storage_uncertain && snapshot().configured);
        assert(app_wifi_forget_network() == ESP_OK); service_step();
        assert(snapshot().stored_data_known && !snapshot().stored_data_present && !snapshot().configured);
        assert(!snapshot().storage_uncertain && snapshot().storage_error == ESP_OK);
    }
    seed_network(); start(); commit_error = ESP_FAIL; submit("WrittenBeforeError"); service_step();
    assert(snapshot().storage_uncertain && strcmp(snapshot().ssid, "Original") == 0);
    reset(true); assert(strcmp(snapshot().ssid, "WrittenBeforeError") == 0 && !snapshot().storage_uncertain);
    commit_error = ESP_FAIL; assert(app_wifi_forget_network() == ESP_OK); service_step();
    assert(snapshot().storage_uncertain && snapshot().configured && !exists);
    reset(true); assert(!snapshot().configured && snapshot().stored_data_known && !snapshot().stored_data_present);
}

static void test_unreadable_recovery(void) {
    seed_network(); durable[0] = 0; reset(true);
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
int main(void) {
    test_setup_and_reconnect(); test_security_and_limits(); test_failures();
    test_cleanup_recovery(); test_storage_uncertainty(); test_unreadable_recovery();
    puts("PASS: production Wi-Fi setup, persistence, reconnect, security and cancellation boundaries");
    return 0;
}
