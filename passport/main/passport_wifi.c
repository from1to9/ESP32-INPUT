#include "passport_wifi.h"
#include "passport_wifi_store.h"
#include "passport_ble.h"
#include "passport_names_owner.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "bootloader_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define SETUP_MS 120000
#define JOIN_MS 20000
#define FALLBACK_MS 30000
#define SCAN_MS 6000
_Static_assert(WIFI_AUTH_MAX <= UINT8_MAX, "Scan auth identifier must fit in one byte");

typedef struct {
    bool submission, web_scan;
    passport_wifi_command_t command;
    passport_wifi_submission_t credentials;
    char token[33];
} queued_command_t;
typedef struct {
    int32_t id;
    bool ip_event;
    union {
        wifi_event_sta_connected_t connected;
        wifi_event_sta_disconnected_t disconnected;
        wifi_event_sta_scan_done_t scan;
        ip_event_got_ip_t got_ip;
        ip_event_ap_staipassigned_t lease;
    } data;
} queued_event_t;
static QueueHandle_t commands, events;
static portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool event_loss, provisioning;
static passport_wifi_state_t state, published;
static passport_wifi_store_t store, staged_store;
static passport_wifi_record_t joining, proposed, target_record;
static wifi_config_t ap_config, station_config;
static wifi_ap_record_t scan_records[PASSPORT_WIFI_SCAN_ROWS];
static esp_netif_t *station_netif, *ap_netif;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static char setup_token[33], published_token[33];
static uint8_t selected_bssid[6];
static bool pin_bssid, new_profile, desired_station, desired_ap, radio_started, radio_ap;
static bool switching, waiting_release, waiting_stop, associating, setup_requested, scan_draining;
static bool next_station, next_ap, target_new, target_pin;
static unsigned stop_mask, attempts;
static uint32_t cancel_revision, cancel_generation;
static uint64_t release_deadline, stop_deadline, join_deadline, retry_at, fallback_at, scan_deadline, setup_deadline;
static queued_command_t incoming; // Owner-only; erased after consumption, including credentials.

static uint64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void result(const char *message) { snprintf(state.result, sizeof(state.result), "%s", message); }
static void error(const char *operation, esp_err_t rc)
{
    // Error names only: SSIDs and credentials never enter logs.
    snprintf(state.result, sizeof(state.result), "%s: %s", operation, esp_err_to_name(rc));
}
static passport_wifi_security_t security(wifi_auth_mode_t auth)
{
    if (auth == WIFI_AUTH_OPEN) return PW_OPEN;
    if (auth == WIFI_AUTH_WPA2_PSK || auth == WIFI_AUTH_WPA_WPA2_PSK) return PW_WPA2;
    return PW_UNSUPPORTED;
}
static passport_ssid_t record_ssid(const passport_wifi_record_t *record)
{
    passport_ssid_t ssid = {.length = record->ssid_length};
    memcpy(ssid.bytes, record->ssid, ssid.length);
    return ssid;
}
static int profile_index(uint32_t id)
{
    for (unsigned i = 0; i < store.count; ++i) if (store.records[i].id == id) return (int)i;
    return -1;
}
static bool same_ssid(const passport_ssid_t *a, const passport_ssid_t *b)
{
    return a->length == b->length && !memcmp(a->bytes, b->bytes, a->length);
}
static void request_result(passport_wifi_request_state_t outcome, const char *message)
{
    state.request_state = outcome;
    snprintf(state.request_result, sizeof(state.request_result), "%s", message);
}
static void close_setup(void)
{
    if (state.pending) request_result(PW_REQUEST_CANCELLED, "Setup closed; no profile saved");
    state.setup_open = state.pending = false;
    setup_requested = false;
    setup_deadline = 0;
    explicit_bzero(&proposed, sizeof(proposed));
    explicit_bzero(setup_token, sizeof(setup_token));
}
static void refresh_profiles(void)
{
    state.profile_count = state.storage_known ? store.count : 0;
    for (unsigned i = 0; i < state.profile_count; ++i) {
        state.profiles[i] = (passport_wifi_profile_t){store.records[i].id, record_ssid(&store.records[i]), store.records[i].security};
    }
}

static void publish(void)
{
    state.provisioning_available = atomic_load(&provisioning);
    refresh_profiles();
    state.setup_seconds = state.setup_open && setup_deadline > now_ms() ? (setup_deadline - now_ms() + 999) / 1000 : 0;
    portENTER_CRITICAL(&snapshot_lock);
    published = state;
    memcpy(published_token, setup_token, sizeof(published_token));
    portEXIT_CRITICAL(&snapshot_lock);
}
void passport_wifi_snapshot(passport_wifi_state_t *out)
{
    portENTER_CRITICAL(&snapshot_lock); *out = published; portEXIT_CRITICAL(&snapshot_lock);
}
void passport_wifi_ap_credentials(char ssid[32], char password[17])
{
    memcpy(ssid, ap_config.ap.ssid, 32);
    memcpy(password, ap_config.ap.password, 17);
}
void passport_wifi_set_provisioning_available(bool available) { atomic_store(&provisioning, available); }
bool passport_wifi_setup_token(char token[33])
{
    portENTER_CRITICAL(&snapshot_lock);
    bool available = published.setup_open && published.ap_started && published.dhcp_running;
    if (available) memcpy(token, published_token, 33);
    portEXIT_CRITICAL(&snapshot_lock);
    return available;
}
bool passport_wifi_request(const passport_wifi_command_t *command)
{
    if (!commands) return false;
    queued_command_t message = {.command = *command};
    return xQueueSend(commands, &message, 0) == pdTRUE;
}
bool passport_wifi_submit(const passport_wifi_submission_t *submission, const char token[33])
{
    if (!commands) return false;
    queued_command_t message = {.submission = true, .credentials = *submission};
    memcpy(message.token, token, sizeof(message.token));
    bool accepted = xQueueSend(commands, &message, 0) == pdTRUE;
    explicit_bzero(&message, sizeof(message));
    return accepted;
}
bool passport_wifi_scan(uint32_t setup_id, const char token[33])
{
    if (!commands) return false;
    queued_command_t message = {.web_scan = true, .credentials.setup_id = setup_id};
    memcpy(message.token, token, sizeof(message.token));
    bool accepted = xQueueSend(commands, &message, 0) == pdTRUE;
    explicit_bzero(&message, sizeof(message));
    return accepted;
}
void passport_wifi_prepare(void)
{
    uint8_t random[10];
    static const char hex[] = "0123456789ABCDEF";
    bootloader_random_enable(); esp_fill_random(random, sizeof(random)); bootloader_random_disable();
    ap_config.ap.ssid_len = snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "Passport-%02X%02X", random[0], random[1]);
    for (unsigned i = 0; i < 8; ++i) {
        ap_config.ap.password[i * 2] = hex[random[i + 2] >> 4];
        ap_config.ap.password[i * 2 + 1] = hex[random[i + 2] & 15];
    }
    explicit_bzero(random, sizeof(random));
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 1;
}
static void event_received(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    queued_event_t event = {.id = id, .ip_event = base == IP_EVENT};
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_CONNECTED) event.data.connected = *(wifi_event_sta_connected_t *)data;
        if (id == WIFI_EVENT_STA_DISCONNECTED) event.data.disconnected = *(wifi_event_sta_disconnected_t *)data;
        if (id == WIFI_EVENT_SCAN_DONE) event.data.scan = *(wifi_event_sta_scan_done_t *)data;
    } else {
        if (id == IP_EVENT_STA_GOT_IP) event.data.got_ip = *(ip_event_got_ip_t *)data;
        if (id == IP_EVENT_AP_STAIPASSIGNED) event.data.lease = *(ip_event_ap_staipassigned_t *)data;
    }
    if (xQueueSend(events, &event, 0) != pdTRUE) atomic_store(&event_loss, true);
}
static bool cancel_scan(void)
{
    if (state.scanning) {
        esp_err_t rc = esp_wifi_scan_stop();
        if (rc != ESP_OK) { error("Stop scan", rc); return false; }
        scan_draining = true;
        state.scanning = false;
        ++state.scan_revision;
    }
    return true;
}
static void start_radio(uint64_t now)
{
    ++state.transport_generation;
    desired_station = next_station; desired_ap = next_ap;
    joining = target_record; explicit_bzero(&target_record, sizeof(target_record));
    new_profile = target_new; pin_bssid = target_pin;
    // Adopt the new attempt's timers only after the old radio has stopped.
    attempts = 0; retry_at = 0;
    fallback_at = desired_station && !new_profile ? now + FALLBACK_MS : 0;
    esp_err_t rc = esp_wifi_set_mode(desired_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (rc == ESP_OK && desired_ap) rc = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (rc == ESP_OK && desired_station) {
        memset(&station_config, 0, sizeof(station_config));
        memcpy(station_config.sta.ssid, joining.ssid, joining.ssid_length);
        memcpy(station_config.sta.password, joining.password, joining.password_length);
        station_config.sta.threshold.authmode = joining.security == PW_OPEN ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        station_config.sta.pmf_cfg.capable = true;
        station_config.sta.bssid_set = pin_bssid;
        memcpy(station_config.sta.bssid, selected_bssid, 6);
        rc = esp_wifi_set_config(WIFI_IF_STA, &station_config);
        explicit_bzero(&station_config, sizeof(station_config));
    }
    if (rc == ESP_OK) rc = esp_wifi_start();
    switching = waiting_stop = false;
    if (rc != ESP_OK) {
        error("Start Wi-Fi", rc); state.phase = PW_OFFLINE;
        if (new_profile) request_result(PW_REQUEST_FAILED, "Wi-Fi start failed; no profile saved");
        return;
    }
    radio_started = true;
    radio_ap = desired_ap;
    scan_draining = false;
    state.phase = desired_station ? PW_CONNECTING : PW_OFFLINE;
    join_deadline = desired_station ? now + JOIN_MS : 0;
    associating = false; // Connection begins on the corresponding STA_START event.
}
static void abort_switch(void)
{
    if (target_new) request_result(PW_REQUEST_FAILED, "Network switch rejected; no profile saved");
    switching = waiting_release = waiting_stop = setup_requested = false;
    explicit_bzero(&target_record, sizeof(target_record));
}
static void begin_switch(bool station, bool ap, uint64_t now)
{
    close_setup(); cancel_scan();
    next_station = station; next_ap = ap;
    switching = waiting_release = true;
    waiting_stop = false;
    passport_ble_state_t ble;
    passport_ble_snapshot(&ble);
    cancel_revision = ble.cancel_revision; cancel_generation = ble.generation;
    release_deadline = now + 500;
    if (!ble.connected) waiting_release = false;
    else {
        passport_command_t cancel = {.kind = PASSPORT_CANCEL, .generation = ble.generation, .network_change = true};
        if (!passport_ble_request(&cancel)) {
            abort_switch();
            result("Input queue busy; network unchanged");
        }
    }
}
static void fallback(const char *message, uint64_t now)
{
    if (new_profile) request_result(PW_REQUEST_FAILED, message);
    result(message);
    target_new = target_pin = false;
    explicit_bzero(&target_record, sizeof(target_record));
    begin_switch(false, true, now);
}
static void open_setup(uint64_t now)
{
    close_setup();
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); ++i) snprintf(setup_token + 2 * i, 3, "%02x", bytes[i]);
    explicit_bzero(bytes, sizeof(bytes));
    state.setup_id = esp_random();
    if (!state.setup_id) state.setup_id = 1;
    state.setup_open = true; setup_requested = false; setup_deadline = now + SETUP_MS;
    result("Setup open; confirm each request here");
}
static void read_scan(const queued_event_t *event)
{
    if (scan_draining || !state.scanning) { esp_wifi_clear_ap_list(); scan_draining = false; return; }
    state.scanning = false;
    state.scan_known = false;
    ++state.scan_revision;
    if (event->data.scan.status) { esp_wifi_clear_ap_list(); result("Scan failed; previous rows unavailable"); return; }
    uint16_t count = PASSPORT_WIFI_SCAN_ROWS;
    esp_err_t rc = esp_wifi_scan_get_ap_records(&count, scan_records);
    if (rc != ESP_OK) { esp_wifi_clear_ap_list(); error("Read scan", rc); return; }
    state.scan_count = count;
    state.scan_truncated = event->data.scan.number > count;
    for (unsigned i = 0; i < count; ++i) {
        const wifi_ap_record_t *record = &scan_records[i];
        passport_wifi_row_t *row = &state.rows[i];
        memset(row, 0, sizeof(*row));
        row->ssid.length = strnlen((const char *)record->ssid, 32);
        memcpy(row->ssid.bytes, record->ssid, row->ssid.length);
        memcpy(row->bssid, record->bssid, 6);
        row->channel = record->primary; row->rssi = record->rssi;
        row->auth = record->authmode;
        row->security = security(record->authmode);
        unsigned matches = 0;
        for (unsigned p = 0; state.storage_known && p < store.count; ++p) {
            passport_ssid_t ssid = record_ssid(&store.records[p]);
            if (same_ssid(&ssid, &row->ssid) && store.records[p].security == row->security) { row->profile_id = store.records[p].id; ++matches; }
        }
        if (matches != 1) row->profile_id = 0;
        row->current = state.phase == PW_ONLINE && same_ssid(&row->ssid, &state.active_ssid) && !memcmp(selected_bssid, row->bssid, 6);
    }
    state.scan_known = true;
    result(state.scan_truncated ? "Scan complete; strongest 16 shown" : count ? "Scan complete" : "No visible networks found");
}
static void save_working_profile(uint64_t now)
{
    if (!state.storage_known) { fallback("Profile storage unavailable", now); return; }
    staged_store = store;
    int index = profile_index(joining.id);
    if (index < 0) {
        if (store.count == PASSPORT_WIFI_PROFILES || store.next_id == UINT32_MAX) { fallback("Profile storage full", now); return; }
        index = staged_store.count++;
        joining.id = staged_store.next_id++;
    }
    staged_store.records[index] = joining;
    staged_store.preferred = joining.id;
    esp_err_t rc = memcmp(&store, &staged_store, sizeof(store)) ? passport_wifi_store_save(&staged_store) : ESP_OK;
    if (rc != ESP_OK) {
        state.storage_known = false;
        explicit_bzero(&staged_store, sizeof(staged_store));
        fallback("Profile save uncertain; restart required", now);
        return;
    }
    store = staged_store;
    explicit_bzero(&staged_store, sizeof(staged_store));
    if (rc == ESP_OK && new_profile) ++state.revision;
    state.active_profile = joining.id;
    if (new_profile) request_result(PW_REQUEST_SAVED, "LAN connected; profile saved");
    new_profile = false; fallback_at = retry_at = join_deadline = 0;
    // Remove only the AP interface; preserve the working station association.
    if (desired_ap) {
        esp_err_t mode = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode != ESP_OK) { error("LAN ready; AP stop", mode); return; }
        desired_ap = false;
        radio_ap = false;
    }
    result("LAN connected; profile saved");
}
static void handle_event(const queued_event_t *event, uint64_t now)
{
    if (!event->ip_event) {
        if (event->id == WIFI_EVENT_STA_STOP) stop_mask &= ~1u;
        if (event->id == WIFI_EVENT_AP_STOP) { stop_mask &= ~2u; if (state.ap_started) ++state.transport_generation; state.ap_started = state.dhcp_running = state.ap_client = state.ap_lease = false; state.ap_client_ip[0] = 0; }
        if (event->id == WIFI_EVENT_SCAN_DONE) { read_scan(event); return; }
        // Until release succeeds, the old radio is still authoritative. Observe
        // its association/IP changes so a rejected switch cannot restore stale facts.
        if (switching && !waiting_release) return;
        if (event->id == WIFI_EVENT_STA_START && desired_station) {
            esp_err_t rc = esp_wifi_connect(); ++attempts;
            associating = rc == ESP_OK;
            if (rc != ESP_OK) { error("Connect", rc); retry_at = now + 5000; }
        }
        if (event->id == WIFI_EVENT_AP_START && desired_ap) {
            state.ap_started = true;
            esp_netif_dhcp_status_t dhcp;
            esp_netif_ip_info_t ip;
            state.dhcp_running = esp_netif_dhcps_get_status(ap_netif, &dhcp) == ESP_OK && dhcp == ESP_NETIF_DHCP_STARTED;
            if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) snprintf(state.ap_ip, sizeof(state.ap_ip), IPSTR, IP2STR(&ip.ip));
            if (!state.dhcp_running) result("AP started; DHCP unavailable");
            else if (setup_requested) open_setup(now);
        }
        if (event->id == WIFI_EVENT_AP_STACONNECTED) { state.ap_client = true; state.ap_lease = false; }
        if (event->id == WIFI_EVENT_AP_STADISCONNECTED) { if (state.ap_client) ++state.transport_generation; state.ap_client = state.ap_lease = false; state.ap_client_ip[0] = 0; }
        if (event->id == WIFI_EVENT_STA_CONNECTED && desired_station && associating) {
            const wifi_event_sta_connected_t *c = &event->data.connected;
            if (c->ssid_len != joining.ssid_length || memcmp(c->ssid, joining.ssid, c->ssid_len) ||
                security(c->authmode) != joining.security || (pin_bssid && memcmp(c->bssid, selected_bssid, 6))) {
                fallback("Connected identity rejected", now); return;
            }
            state.phase = PW_ASSOCIATED;
            state.active_ssid = record_ssid(&joining);
            memcpy(selected_bssid, c->bssid, 6);
            result("Associated; waiting for DHCP address");
        }
        if (event->id == WIFI_EVENT_STA_DISCONNECTED && desired_station) {
            ++state.transport_generation;
            state.disconnect_reason = event->data.disconnected.reason;
            state.phase = PW_OFFLINE; state.sta_ip[0] = 0; state.active_profile = 0;
            associating = false;
            if (new_profile || !state.storage_known) {
                char failure[64];
                snprintf(failure, sizeof(failure), "Join failed (reason %u); profiles retained", state.disconnect_reason);
                fallback(failure, now); return;
            }
            if (!fallback_at) { fallback_at = now + FALLBACK_MS; attempts = 0; }
            retry_at = now + 5000;
            result("LAN lost; bounded reconnect pending");
        }
    } else {
        if (switching && !waiting_release) return;
        if (event->id == IP_EVENT_STA_GOT_IP && (state.phase == PW_ASSOCIATED || state.phase == PW_ONLINE) &&
            event->data.got_ip.esp_netif == station_netif && event->data.got_ip.ip_info.ip.addr) {
            esp_netif_ip_info_t current;
            if (esp_netif_get_ip_info(station_netif, &current) != ESP_OK || current.ip.addr != event->data.got_ip.ip_info.ip.addr) return;
            state.phase = PW_ONLINE;
            snprintf(state.sta_ip, sizeof(state.sta_ip), IPSTR, IP2STR(&event->data.got_ip.ip_info.ip));
            save_working_profile(now);
        }
        if (event->id == IP_EVENT_STA_LOST_IP && state.phase == PW_ONLINE) {
            esp_netif_ip_info_t current;
            if (esp_netif_get_ip_info(station_netif, &current) == ESP_OK && current.ip.addr) return;
            ++state.transport_generation;
            state.phase = PW_ASSOCIATED; state.sta_ip[0] = 0; state.active_profile = 0;
            fallback_at = now + FALLBACK_MS;
            result("LAN address lost; waiting for DHCP");
        }
        if (event->id == IP_EVENT_AP_STAIPASSIGNED && state.ap_started && state.ap_client && event->data.lease.esp_netif == ap_netif) {
            state.ap_lease = true;
            snprintf(state.ap_client_ip, sizeof(state.ap_client_ip), IPSTR, IP2STR(&event->data.lease.ip));
        }
    }
}
static void handle_submission(const queued_command_t *message)
{
    const passport_wifi_submission_t *s = &message->credentials;
    state.last_submission_id = s->request_id; state.submission_accepted = false;
    if (!s->request_id || !state.setup_open || state.pending || s->setup_id != state.setup_id || memcmp(message->token, setup_token, sizeof(setup_token)) ||
        !state.storage_known || !state.scan_known || state.scanning || s->scan_revision != state.scan_revision || s->row >= state.scan_count) {
        result("Setup request rejected; refresh the page"); return;
    }
    const passport_wifi_row_t *row = &state.rows[s->row];
    size_t length = strnlen(s->password, sizeof(s->password));
    if (!row->ssid.length || row->security == PW_UNSUPPORTED || length > 63 ||
        (row->security == PW_OPEN ? length != 0 : length < 8)) { result("Unsupported security or invalid password"); return; }
    for (unsigned i = 0; i < length; ++i) if ((unsigned char)s->password[i] < 32 || (unsigned char)s->password[i] > 126) { result("Password must be printable ASCII"); return; }
    if (!row->profile_id && store.count == PASSPORT_WIFI_PROFILES) { result("Profiles full; forget one locally first"); return; }
    proposed = (passport_wifi_record_t){.id = row->profile_id, .ssid_length = row->ssid.length, .security = row->security, .password_length = length};
    memcpy(proposed.ssid, row->ssid.bytes, row->ssid.length);
    memcpy(proposed.password, s->password, length);
    memcpy(selected_bssid, row->bssid, 6);
    state.pending_id = s->request_id; state.submission_accepted = true;
    request_result(PW_REQUEST_WAITING, "Confirm this request on Passport");
    state.pending_ssid = row->ssid; state.pending_security = row->security;
    state.pending = true;
    result("Confirm this exact request on Passport");
}
static void handle_command(const passport_wifi_command_t *command, uint64_t now)
{
    if (command->action == PW_SETUP_CLOSE || command->action == PW_REJECT) {
        if (command->action == PW_SETUP_CLOSE || (state.pending && command->id == state.pending_id)) { close_setup(); result("Setup request cancelled"); }
        return;
    }
    passport_ble_state_t ble;
    passport_ble_snapshot(&ble);
    if (ble.locked) { result("Unlock locally to change Wi-Fi"); return; }
    if (switching) { result("Network switch in progress"); return; }
    if (command->action == PW_SCAN_CANCEL) { if (cancel_scan()) result("Scan cancelled"); return; }
    if (command->action == PW_SCAN) {
        if (state.scanning || scan_draining || (associating && state.phase != PW_ONLINE && state.phase != PW_OFFLINE)) { result("Wait for connection before scanning"); return; }
        wifi_scan_config_t scan = {.show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active = {.min = 30, .max = 120}, .home_chan_dwell_time = 30};
        esp_err_t rc = esp_wifi_scan_start(&scan, false);
        if (rc != ESP_OK) { error("Scan", rc); return; }
        state.scanning = true; state.scan_known = false; state.scan_count = 0;
        ++state.scan_revision; scan_deadline = now + SCAN_MS;
        result("Scanning 2.4 GHz networks"); return;
    }
    if (command->action == PW_DISCONNECT && (!command->id || command->id != state.active_profile || command->revision != state.revision)) { result("Selected profile is not active"); return; }
    if (command->action == PW_USE_AP || command->action == PW_DISCONNECT || command->action == PW_SETUP_OPEN) {
        if (command->action == PW_SETUP_OPEN && (!atomic_load(&provisioning) || !state.storage_known)) { result("Provisioning unavailable"); return; }
        bool setup = command->action == PW_SETUP_OPEN;
        if (state.ap_started && !desired_station) {
            if (setup && state.dhcp_running) open_setup(now);
            else if (setup) result("AP DHCP unavailable; setup closed");
            return;
        }
        fallback(setup ? "Opening setup AP" : "Local AP selected", now);
        setup_requested = setup && switching;
        return;
    }
    if (command->action == PW_CONFIRM) {
        if (!state.pending || command->id != state.pending_id || command->revision != state.setup_id) { result("Setup request changed; select again"); return; }
        target_record = proposed; target_new = target_pin = true;
        state.pending = false;
        request_result(PW_REQUEST_JOINING, "Joining; save is not confirmed yet");
        begin_switch(true, true, now);
        if (switching) result("Joining confirmed network");
        return;
    }
    if (!state.storage_known || command->revision != state.revision) { result("Profile list changed or unavailable"); return; }
    int index = profile_index(command->id);
    if (index < 0) { result("Selected profile no longer exists"); return; }
    if (command->action == PW_CONNECT) {
        target_record = store.records[index]; target_pin = target_new = false;
        begin_switch(true, state.ap_started, now);
        if (switching) result("Connecting saved profile");
    } else if (command->action == PW_FORGET) {
        staged_store = store;
        memmove(&staged_store.records[index], &staged_store.records[index + 1], (store.count - index - 1) * sizeof(store.records[0]));
        memset(&staged_store.records[--staged_store.count], 0, sizeof(store.records[0]));
        if (staged_store.preferred == command->id) staged_store.preferred = staged_store.count ? staged_store.records[0].id : 0;
        esp_err_t rc = passport_wifi_store_save(&staged_store);
        if (rc != ESP_OK) { state.storage_known = false; result("Forget uncertain; restart required"); }
        else {
            store = staged_store; ++state.revision; result("Profile forgotten");
            if (joining.id == command->id) fallback("Profile forgotten; local AP selected", now);
            const passport_name_key_t key = {.kind = PN_WIFI_PROFILE, .profile = command->id};
            if (passport_names_remove(&key) != PN_OK) result("Profile forgotten; label cleanup failed");
        }
        explicit_bzero(&staged_store, sizeof(staged_store));
    }
}

static bool wifi_name_retained(const passport_name_key_t *key)
{
    return key->kind == PN_SCAN_AP || profile_index(key->profile) >= 0;
}

static passport_name_result_t wifi_name_change(const passport_name_change_t *change,
                                               const passport_ble_state_t *ble)
{
    if (!state.initialized || !state.storage_known) return PN_UNAVAILABLE;
    if (!ble->synced || ble->locked) return PN_LOCKED;
    if (change->key.kind == PN_WIFI_PROFILE && !wifi_name_retained(&change->key)) return PN_NOT_FOUND;
    if (change->key.kind != PN_SCAN_AP || change->clear) return passport_names_apply(change);
    if (!state.scan_known || state.scanning || change->scan_revision != state.scan_revision) return PN_STALE;
    refresh_profiles();
    for (unsigned i = 0; i < state.scan_count; ++i) {
        const passport_wifi_row_t *row = &state.rows[i];
        const passport_name_key_t key = passport_wifi_ap_name_key(row);
        if (!passport_names_equal(&change->key, &key)) continue;
        uint32_t profile;
        if (passport_wifi_profile_matches(&state, row, &profile)) return PN_INVALID;
        return passport_names_apply(change);
    }
    return PN_NOT_FOUND;
}

static void service_wifi_names(passport_ble_state_t *ble)
{
    passport_name_change_t change;
    if (passport_names_take(PN_WIFI, &change)) {
        passport_ble_snapshot(ble);
        passport_names_finish(PN_WIFI, wifi_name_change(&change, ble));
    }
}

static void step(uint64_t now)
{
    if (!state.initialized) {
        passport_name_change_t change;
        if (passport_names_take(PN_WIFI, &change)) passport_names_finish(PN_WIFI, PN_UNAVAILABLE);
        return;
    }
    passport_ble_state_t ble;
    passport_ble_snapshot(&ble);
    if (state.setup_open && (ble.locked || now >= setup_deadline || !atomic_load(&provisioning))) { close_setup(); result("Setup closed; reopen locally"); }
    if (atomic_exchange(&event_loss, false)) {
        if (state.request_state == PW_REQUEST_JOINING) request_result(PW_REQUEST_FAILED, "Wi-Fi event loss; restart required");
        esp_wifi_stop(); close_setup(); state.initialized = false;
        state.ap_started = state.dhcp_running = state.ap_client = state.ap_lease = false;
        state.phase = PW_OFFLINE; state.sta_ip[0] = 0;
        result("Wi-Fi event loss; restart required"); publish(); return;
    }
    queued_event_t event;
    for (unsigned i = 0; i < 16 && xQueueReceive(events, &event, 0) == pdTRUE; ++i) handle_event(&event, now);
    if (xQueueReceive(commands, &incoming, 0) == pdTRUE) {
        if (incoming.submission) handle_submission(&incoming);
        else if (incoming.web_scan) {
            if (state.setup_open && !state.pending && incoming.credentials.setup_id == state.setup_id &&
                !memcmp(incoming.token, setup_token, sizeof(setup_token))) {
                const passport_wifi_command_t scan = {.action = PW_SCAN};
                handle_command(&scan, now);
            }
        } else handle_command(&incoming.command, now);
        explicit_bzero(&incoming, sizeof(incoming));
    }
    if (switching) {
        if (waiting_release) {
            if (!ble.connected || ble.generation != cancel_generation || (ble.cancel_revision != cancel_revision && !ble.release_pending)) waiting_release = false;
            else if (now >= release_deadline) { abort_switch(); result("Input release unconfirmed; retry network action"); }
        }
        if (switching && !waiting_release && !waiting_stop) {
            if (!radio_started) start_radio(now);
            else {
                stop_mask = 1u | (radio_ap ? 2u : 0u);
                esp_err_t rc = esp_wifi_stop();
                if (rc != ESP_OK) { abort_switch(); error("Stop Wi-Fi", rc); }
                else {
                    waiting_stop = true; stop_deadline = now + 2000;
                    state.phase = PW_SWITCHING;
                    state.sta_ip[0] = 0; state.active_profile = 0; associating = false;
                }
            }
        }
        if (waiting_stop && !stop_mask) { radio_started = false; start_radio(now); }
        else if (waiting_stop && now >= stop_deadline) {
            if (target_new) request_result(PW_REQUEST_FAILED, "Wi-Fi stop unconfirmed; restart required");
            state.initialized = false; close_setup(); result("Wi-Fi stop unconfirmed; restart required");
        }
    }
    if (state.scanning && now >= scan_deadline) { cancel_scan(); state.scan_known = false; result("Scan timed out"); }
    if (!switching && desired_station) {
        if (fallback_at && now >= fallback_at) fallback("LAN unavailable for 30 seconds; AP fallback", now);
        else if (join_deadline && now >= join_deadline && state.phase != PW_ONLINE) {
            if (new_profile) fallback("Join/IP timeout; previous profiles retained", now);
            else { esp_wifi_disconnect(); associating = false; join_deadline = 0; retry_at = now + 1000; }
        } else if (retry_at && now >= retry_at && attempts < 3 && !state.scanning) {
            retry_at = 0; ++attempts;
            esp_err_t rc = esp_wifi_connect(); associating = rc == ESP_OK;
            if (rc != ESP_OK) error("Reconnect", rc);
            else { state.phase = PW_CONNECTING; join_deadline = now + JOIN_MS; }
        }
    }
    if (ble.locked && state.setup_open) { close_setup(); result("Setup closed by Lock"); }
    service_wifi_names(&ble);
    publish();
}
static void owner_task(void *arg)
{
    (void)arg;
    state.storage_known = passport_wifi_store_load(&store) == ESP_OK;
    passport_names_load(PN_WIFI);
    state.revision = 1;
    int preferred = state.storage_known ? profile_index(store.preferred) : -1;
    if (preferred >= 0) {
        target_record = store.records[preferred]; next_station = true;
    } else next_ap = true;
    if (!state.storage_known) result("Profile storage unknown; restart required");
    else result(next_station ? "Connecting last working LAN" : "No saved LAN; starting local AP");
    start_radio(now_ms());
    if (state.storage_known && passport_names_prune(PN_WIFI, wifi_name_retained) != PN_OK) {
        result("Network label storage unavailable");
    }
    for (;;) { step(now_ms()); vTaskDelay(pdMS_TO_TICKS(50)); }
}
static esp_err_t create_netif(wifi_interface_t interface, esp_netif_t **out)
{
    const esp_netif_config_t sta = ESP_NETIF_DEFAULT_WIFI_STA(), ap = ESP_NETIF_DEFAULT_WIFI_AP();
    *out = esp_netif_new(interface == WIFI_IF_STA ? &sta : &ap);
    if (!*out) return ESP_ERR_NO_MEM;
    esp_err_t rc = interface == WIFI_IF_STA ? esp_netif_attach_wifi_station(*out) : esp_netif_attach_wifi_ap(*out);
    if (rc == ESP_OK) rc = interface == WIFI_IF_STA ? esp_wifi_set_default_wifi_sta_handlers() : esp_wifi_set_default_wifi_ap_handlers();
    return rc;
}
esp_err_t passport_wifi_start(void)
{
    esp_err_t rc = esp_netif_init();
    if (rc != ESP_OK) return rc;
    rc = esp_event_loop_create_default();
    if (rc != ESP_OK) return rc;
    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    bool driver = false, wifi_registered = false, ip_registered = false;
    rc = esp_wifi_init(&config);
    if (rc != ESP_OK) goto fail;
    driver = true;
    rc = create_netif(WIFI_IF_STA, &station_netif); if (rc != ESP_OK) goto fail;
    rc = create_netif(WIFI_IF_AP, &ap_netif); if (rc != ESP_OK) goto fail;
    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM); if (rc != ESP_OK) goto fail;
    commands = xQueueCreate(4, sizeof(queued_command_t)); events = xQueueCreate(16, sizeof(queued_event_t));
    if (!commands || !events) { rc = ESP_ERR_NO_MEM; goto fail; }
    rc = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_received, NULL, &wifi_handler);
    if (rc != ESP_OK) goto fail;
    wifi_registered = true;
    rc = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event_received, NULL, &ip_handler);
    if (rc != ESP_OK) goto fail;
    ip_registered = true;
    state.initialized = true;
    if (xTaskCreate(owner_task, "passport_wifi", 4096, NULL, 3, NULL) == pdPASS) return ESP_OK;
    rc = ESP_ERR_NO_MEM;
fail:
    state.initialized = false;
    if (ip_registered) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
    if (wifi_registered) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    if (events) { vQueueDelete(events); events = NULL; }
    if (commands) { vQueueDelete(commands); commands = NULL; }
    if (ap_netif) { esp_netif_destroy_default_wifi(ap_netif); ap_netif = NULL; }
    if (station_netif) { esp_netif_destroy_default_wifi(station_netif); station_netif = NULL; }
    if (driver) esp_wifi_deinit();
    esp_event_loop_delete_default();
    error("Initialize Wi-Fi", rc); publish();
    return rc;
}
