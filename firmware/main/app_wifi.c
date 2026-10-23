// SPDX-License-Identifier: Apache-2.0
#include "app_wifi.h"
#include "app_wifi_profiles.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_WIFI_SUPPORTED && CONFIG_ESP_WIFI_ENABLED
#include <net/if.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define START_SETUP 1U
#define STOP_SETUP 2U
#define FORGET_NETWORK 4U
#define SAVE_NETWORK 8U
#define VERIFY_STORAGE 16U
#define SCAN_START 32U
#define SCAN_CANCEL 64U
#define JOIN_NETWORK 128U
#define PROFILE_MUTATION 256U
#define FORM_LIMIT 512U
#define CONNECT_TIMEOUT_MS 15000U
#define RETRY_MAX_MS 30000U
#define STORE_LENGTH 101U
#define CLEANUP_RETRY_MS 1000U

typedef struct { char ssid[33]; char password[64]; } credentials_t;
typedef struct {
    char ssid[33];
    uint8_t bssid[6], channel;
    int8_t rssi;
    uint16_t authmode;
    bool current, remembered, malformed_ssid;
} scan_record_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_wifi_snapshot_t s_snapshot;
static app_wifi_setup_info_t s_setup;
static char s_token[33];
static credentials_t s_pending;
static uint32_t s_pending_generation, s_generation, s_station_generation;
static uint32_t s_commands;
static bool s_started, s_accept_form, s_close_on_connect;
static bool s_maintenance;
static bool s_ap_cleanup_needed;
static httpd_handle_t s_server;
static esp_netif_t *s_sta, *s_ap;
static uint64_t s_setup_until, s_retry_at, s_connect_until, s_close_at;
static uint64_t s_cleanup_retry_at;
static uint32_t s_retry_delay = 1000U;
static uint16_t s_reason;
static uint32_t s_event_generation;
static bool s_disconnected;
static app_wifi_scan_snapshot_t s_scan;
static app_wifi_scan_diagnostics_t s_scan_diagnostics;
static scan_record_t s_scan_records[APP_WIFI_SCAN_CAPACITY];
static bool s_scan_wait_event, s_scan_done, s_scan_stop_sent, s_scan_cancelled;
static uint32_t s_scan_status;
static uint64_t s_scan_deadline, s_scan_completed_at, s_scan_next_at, s_scan_cleanup_at;
static uint64_t s_scan_cancel_until;
static bool s_pending_remember;
static uint32_t s_pending_scan_generation;
static uint16_t s_pending_record_index;
static uint32_t s_selection_counter, s_selection_id, s_selection_scan_generation;
static app_wifi_selection_owner_t s_selection_owner;
static uint16_t s_selection_index;
static char s_selection_ssid[APP_WIFI_SSID_CAPACITY];
static uint16_t s_selection_authmode;
static uint64_t s_selection_until, s_pending_selection_until;

static bool profile_remembered_locked(const char *ssid);
static bool profile_capacity_locked(const char *ssid);
static void profile_reconnect(uint64_t now, uint16_t reason);
static esp_err_t join_locked(const app_wifi_join_request_t *,const app_wifi_selection_owner_t *);

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void clear_secret(void *memory, size_t length)
{
    volatile unsigned char *p = memory;
    while (length--) *p++ = 0;
}
static void publish(void)
{
    /* The worker owns s_snapshot; readers copy under this same lock. */
    ++s_snapshot.revision;
}
static void set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.last_error = error;
    publish();
    portEXIT_CRITICAL(&s_lock);
}

static bool valid_text(const char *text, size_t maximum, bool ascii)
{
    size_t length = strnlen(text, maximum + 1U);
    if (!length || length > maximum) return false;
    for (size_t i = 0; i < length;) {
        unsigned char c = (unsigned char)text[i++];
        if (c < 32U || c == 127U) return false;
        if (c < 128U) continue;
        if (ascii) return false;
        unsigned remaining;
        uint32_t value, minimum;
        if (c >= 0xc2U && c <= 0xdfU) { remaining = 1; value = c & 31U; minimum = 0x80U; }
        else if (c >= 0xe0U && c <= 0xefU) { remaining = 2; value = c & 15U; minimum = 0x800U; }
        else if (c >= 0xf0U && c <= 0xf4U) { remaining = 3; value = c & 7U; minimum = 0x10000U; }
        else return false;
        if (i + remaining > length) return false;
        while (remaining--) {
            c = (unsigned char)text[i++];
            if ((c & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (c & 63U);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
            return false;
    }
    return true;
}

static app_wifi_security_t scan_security(uint16_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return APP_WIFI_SECURITY_OPEN;
    case WIFI_AUTH_WEP: return APP_WIFI_SECURITY_WEP;
    case WIFI_AUTH_WPA_PSK: return APP_WIFI_SECURITY_WPA;
    case WIFI_AUTH_WPA2_PSK: return APP_WIFI_SECURITY_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK: return APP_WIFI_SECURITY_WPA_WPA2;
    case WIFI_AUTH_WPA3_PSK: return APP_WIFI_SECURITY_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK: return APP_WIFI_SECURITY_WPA2_WPA3;
    case WIFI_AUTH_OWE: return APP_WIFI_SECURITY_OWE;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE: return APP_WIFI_SECURITY_ENTERPRISE;
    default: return APP_WIFI_SECURITY_OTHER;
    }
}

static app_wifi_join_availability_t scan_availability(const scan_record_t *record)
{
    if (record->malformed_ssid) return APP_WIFI_JOIN_UNSUPPORTED_SSID;
    if (!record->ssid[0]) return APP_WIFI_JOIN_HIDDEN_SSID;
    if (!valid_text(record->ssid, 32, false)) return APP_WIFI_JOIN_UNSUPPORTED_SSID;
    if (record->authmode != WIFI_AUTH_WPA2_PSK && record->authmode != WIFI_AUTH_WPA_WPA2_PSK)
        return APP_WIFI_JOIN_UNSUPPORTED_SECURITY;
    return APP_WIFI_JOIN_AVAILABLE;
}

/* Caller holds s_lock. No driver reads or side effects in validation. */
static bool fresh_scan_row(uint32_t generation, uint16_t index, const char *ssid)
{
    return s_scan.state == APP_WIFI_SCAN_READY && !s_scan.cleanup_pending &&
        generation == s_scan.generation && index < s_scan.count &&
        now_ms() - s_scan_completed_at < APP_WIFI_SCAN_RESULT_TTL_MS &&
        scan_availability(&s_scan_records[index]) == APP_WIFI_JOIN_AVAILABLE &&
        strcmp(ssid, s_scan_records[index].ssid) == 0;
}

static void scan_event(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    if (base != WIFI_EVENT || id != WIFI_EVENT_SCAN_DONE) return;
    const wifi_event_sta_scan_done_t *event = data;
    portENTER_CRITICAL(&s_lock);
    if (s_scan_wait_event && !s_scan_done) {
        s_scan_done = true;
        s_scan_status = event ? event->status : 1U;
        s_scan_diagnostics.current.event_status_known = event != NULL;
        s_scan_diagnostics.current.event_status = event ? event->status : 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool scan_busy(void)
{
    portENTER_CRITICAL(&s_lock);
    bool busy = s_scan.state == APP_WIFI_SCAN_QUEUED ||
        s_scan.state == APP_WIFI_SCAN_RUNNING || s_scan.state == APP_WIFI_SCAN_CANCELLING ||
        s_scan.cleanup_pending;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

/* Only the Wi-Fi owner advances phases. The event callback copies the raw
 * completion status under the same lock; readers receive a bounded copy. */
static void observe_scan_locked(app_wifi_scan_phase_t phase, esp_err_t error)
{
    app_wifi_scan_observation_t *current = &s_scan_diagnostics.current;
    if (current->known) {
        current->phase = phase;
        current->error = error;
        if (error != ESP_OK && (!s_scan_diagnostics.last_failure.known ||
            s_scan_diagnostics.last_failure.generation != current->generation))
            s_scan_diagnostics.last_failure = *current;
    }
}
static void observe_scan(app_wifi_scan_phase_t phase, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    observe_scan_locked(phase, error);
    portEXIT_CRITICAL(&s_lock);
}

static void finish_scan(app_wifi_scan_state_t state, esp_err_t error, esp_err_t cleanup)
{
    portENTER_CRITICAL(&s_lock);
    if (state == APP_WIFI_SCAN_READY && (s_commands & (SCAN_CANCEL | FORGET_NETWORK)))
        state = APP_WIFI_SCAN_CANCELLED;
    s_scan.state = cleanup != ESP_OK && state == APP_WIFI_SCAN_CANCELLED ?
        APP_WIFI_SCAN_CANCELLING : state;
    s_scan.error = error == ESP_OK && state == APP_WIFI_SCAN_ERROR ? cleanup : error;
    s_scan.cleanup_error = cleanup;
    s_scan.cleanup_pending = cleanup != ESP_OK;
    s_scan_wait_event = false;
    s_scan_done = false;
    if (state != APP_WIFI_SCAN_READY) {
        s_scan.count = 0;
        memset(s_scan_records, 0, sizeof(s_scan_records));
    }
    s_scan_completed_at = now_ms();
    observe_scan_locked(cleanup == ESP_OK ? APP_WIFI_SCAN_PHASE_COMPLETE : APP_WIFI_SCAN_PHASE_CLEAR,
        cleanup == ESP_OK ? s_scan.error : cleanup);
    publish();
    portEXIT_CRITICAL(&s_lock);
    s_scan_cleanup_at = cleanup != ESP_OK ? now_ms() + CLEANUP_RETRY_MS : 0;
}

static void start_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    bool cancelled = s_commands & (SCAN_CANCEL | FORGET_NETWORK);
    portEXIT_CRITICAL(&s_lock);
    if (cancelled) {
        finish_scan(APP_WIFI_SCAN_CANCELLED, ESP_OK, ESP_OK);
        return;
    }
    /* Provisional reserves are admission limits, not heap/radio qualification. */
    unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t free_internal = heap_caps_get_free_size(caps);
    size_t largest_internal = heap_caps_get_largest_free_block(caps);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    portENTER_CRITICAL(&s_lock);
    s_scan_diagnostics.current.heap_known = true;
    s_scan_diagnostics.current.station_state = s_snapshot.state;
    s_scan_diagnostics.current.lan_ready = s_snapshot.lan_ready;
    s_scan_diagnostics.current.free_internal = free_internal;
    s_scan_diagnostics.current.largest_internal = largest_internal;
    s_scan_diagnostics.current.free_dma = free_dma;
    s_scan_diagnostics.current.largest_dma = largest_dma;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (free_internal < 32768U || largest_internal < 16384U) error = ESP_ERR_NO_MEM;
    wifi_scan_config_t config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 30, .max = 60}, .passive = 120},
        .home_chan_dwell_time = 30,
        .coex_background_scan = true,
        .max_scan_ap_num = APP_WIFI_SCAN_CAPACITY
    };
    portENTER_CRITICAL(&s_lock);
    cancelled = s_commands & (SCAN_CANCEL | FORGET_NETWORK);
    if (cancelled) {
        portEXIT_CRITICAL(&s_lock);
        finish_scan(APP_WIFI_SCAN_CANCELLED, ESP_OK, ESP_OK);
        return;
    }
    s_scan_wait_event = error == ESP_OK;
    s_scan_done = s_scan_stop_sent = s_scan_cancelled = false;
    s_scan_status = 0;
    s_scan.state = APP_WIFI_SCAN_RUNNING;
    publish();
    portEXIT_CRITICAL(&s_lock);
    observe_scan(APP_WIFI_SCAN_PHASE_HEAP, error);
    if (error == ESP_OK) {
        error = esp_wifi_scan_start(&config, false);
        portENTER_CRITICAL(&s_lock);
        s_scan_diagnostics.current.driver_admitted = error == ESP_OK;
        portEXIT_CRITICAL(&s_lock);
        observe_scan(APP_WIFI_SCAN_PHASE_SDK_START, error);
    }
    if (error != ESP_OK) {
        /* Failed admission must not manufacture a successful empty scan. */
        esp_err_t cleared = esp_wifi_clear_ap_list();
        observe_scan(APP_WIFI_SCAN_PHASE_CLEAR, cleared);
        finish_scan(APP_WIFI_SCAN_ERROR, error, cleared);
    } else s_scan_deadline = now_ms() + APP_WIFI_SCAN_TIMEOUT_MS;
}

static void service_scan(bool cancel)
{
    portENTER_CRITICAL(&s_lock);
    bool awaiting = s_scan_wait_event, done = s_scan_done;
    bool cleanup = s_scan.cleanup_pending;
    uint32_t status = s_scan_status;
    portEXIT_CRITICAL(&s_lock);
    uint64_t now = now_ms();
    if (awaiting && (cancel || s_scan_cancelled || now >= s_scan_deadline)) {
        if (!s_scan_cancelled) s_scan_cancel_until = now + 2000U;
        s_scan_cancelled = true;
        portENTER_CRITICAL(&s_lock);
        s_scan.state = now >= s_scan_cancel_until ? APP_WIFI_SCAN_ERROR : APP_WIFI_SCAN_CANCELLING;
        s_scan.count = 0;
        if (now >= s_scan_deadline && s_scan.error == ESP_OK) s_scan.error = ESP_ERR_TIMEOUT;
        if (now >= s_scan_cancel_until && s_scan.error == ESP_OK) s_scan.error = ESP_ERR_TIMEOUT;
        publish();
        portEXIT_CRITICAL(&s_lock);
        if (s_scan.error != ESP_OK) observe_scan(APP_WIFI_SCAN_PHASE_WAIT_EVENT, s_scan.error);
        if (!done && !s_scan_stop_sent && now >= s_scan_cleanup_at) {
            esp_err_t stopped = esp_wifi_scan_stop();
            observe_scan(APP_WIFI_SCAN_PHASE_STOP, stopped);
            s_scan_stop_sent = stopped == ESP_OK;
            s_scan_cleanup_at = now + CLEANUP_RETRY_MS;
            portENTER_CRITICAL(&s_lock);
            s_scan.cleanup_pending = true;
            s_scan.cleanup_error = stopped;
            publish();
            portEXIT_CRITICAL(&s_lock);
        }
        /* Even a successful stop awaits its documented completion event.
         * Without that evidence new scan/join/reconnect remains blocked. */
    }
    if (awaiting && done) {
        observe_scan(APP_WIFI_SCAN_PHASE_EVENT, !s_scan_cancelled && status ? ESP_FAIL : ESP_OK);
        if (s_scan_cancelled || status != 0) {
            esp_err_t error = s_scan.error;
            if (!s_scan_cancelled && status && error == ESP_OK) error = ESP_FAIL;
            esp_err_t cleared = esp_wifi_clear_ap_list();
            observe_scan(APP_WIFI_SCAN_PHASE_CLEAR, cleared);
            finish_scan(s_scan_cancelled && error == ESP_OK ? APP_WIFI_SCAN_CANCELLED :
                APP_WIFI_SCAN_ERROR, error, cleared);
        } else {
            uint16_t count = 0;
            esp_err_t error = esp_wifi_scan_get_ap_num(&count);
            portENTER_CRITICAL(&s_lock);
            s_scan_diagnostics.current.count_known = error == ESP_OK;
            s_scan_diagnostics.current.driver_count = error == ESP_OK ? count : 0;
            portEXIT_CRITICAL(&s_lock);
            observe_scan(APP_WIFI_SCAN_PHASE_COUNT, error);
            wifi_ap_record_t current = {0};
            bool current_known = esp_wifi_sta_get_ap_info(&current) == ESP_OK;
            uint16_t kept = count < APP_WIFI_SCAN_CAPACITY ? count : APP_WIFI_SCAN_CAPACITY;
            for (uint16_t i = 0; error == ESP_OK && i < kept; ++i) {
                wifi_ap_record_t ap = {0};
                error = esp_wifi_scan_get_ap_record(&ap);
                observe_scan(APP_WIFI_SCAN_PHASE_RECORD, error);
                if (error != ESP_OK) break;
                scan_record_t record = {.channel = ap.primary, .rssi = ap.rssi,
                    .authmode = (uint16_t)ap.authmode};
                memcpy(record.ssid, ap.ssid, 32);
                size_t name_length = strnlen(record.ssid, 32);
                for (size_t k = name_length; k < 32; ++k)
                    if (record.ssid[k]) record.malformed_ssid = true;
                memcpy(record.bssid, ap.bssid, sizeof(record.bssid));
                record.current = current_known && memcmp(current.bssid, ap.bssid, 6) == 0;
                portENTER_CRITICAL(&s_lock);
                record.remembered = s_snapshot.stored_data_known && s_snapshot.stored_data_present &&
                    !s_snapshot.storage_uncertain && scan_availability(&record) == APP_WIFI_JOIN_AVAILABLE &&
                    profile_remembered_locked(record.ssid);
                s_scan_records[i] = record;
                s_scan_diagnostics.current.read_count = i + 1U;
                portEXIT_CRITICAL(&s_lock);
            }
            esp_err_t cleared = esp_wifi_clear_ap_list();
            observe_scan(APP_WIFI_SCAN_PHASE_CLEAR, cleared);
            portENTER_CRITICAL(&s_lock);
            s_scan.count = error == ESP_OK ? kept : 0;
            s_scan.capacity_reached = count >= APP_WIFI_SCAN_CAPACITY;
            bool cancelled = s_commands & (SCAN_CANCEL | FORGET_NETWORK);
            portEXIT_CRITICAL(&s_lock);
            finish_scan(error == ESP_OK && !cancelled && cleared == ESP_OK ? APP_WIFI_SCAN_READY :
                cancelled ? APP_WIFI_SCAN_CANCELLED : APP_WIFI_SCAN_ERROR, error, cleared);
        }
    } else if (!awaiting && cleanup && now >= s_scan_cleanup_at) {
        app_wifi_scan_state_t terminal = s_scan.state == APP_WIFI_SCAN_CANCELLING ?
            (s_scan.error == ESP_OK ? APP_WIFI_SCAN_CANCELLED : APP_WIFI_SCAN_ERROR) : s_scan.state;
        esp_err_t cleared = esp_wifi_clear_ap_list();
        observe_scan(APP_WIFI_SCAN_PHASE_CLEAR, cleared);
        finish_scan(terminal, s_scan.error, cleared);
    } else if (!awaiting && cancel && !cleanup) {
        finish_scan(APP_WIFI_SCAN_CANCELLED, ESP_OK, ESP_OK);
    }
}

static void storage_result(esp_err_t error, bool known, bool present,
                           const credentials_t *credentials)
{
    bool valid = error == ESP_OK || (error == ESP_ERR_NVS_NOT_FOUND && known);
    portENTER_CRITICAL(&s_lock);
    s_snapshot.stored_data_known = known;
    s_snapshot.stored_data_present = known && present;
    s_snapshot.storage_uncertain = !valid;
    s_snapshot.storage_error = valid ? ESP_OK : error;
    s_snapshot.stored_ssid[0] = '\0';
    if (error == ESP_OK && known && present && credentials)
        memcpy(s_snapshot.stored_ssid, credentials->ssid, sizeof(credentials->ssid));
    publish();
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t read_legacy_credentials(credentials_t *value, bool *known, bool *present)
{
    nvs_handle_t handle;
    uint8_t blob[STORE_LENGTH] = {0};
    size_t length = 0;
    *known = *present = false;
    esp_err_t error = nvs_open("app_wifi", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        *known = error == ESP_ERR_NVS_NOT_FOUND;
        return error;
    }
    error = nvs_get_blob(handle, "network", NULL, &length);
    if (error == ESP_OK) {
        *known = *present = true;
        if (length == sizeof(blob)) error = nvs_get_blob(handle, "network", blob, &length);
        else error = ESP_ERR_INVALID_STATE;
    } else if (error == ESP_ERR_NVS_NOT_FOUND) *known = true;
    nvs_close(handle);
    if (error == ESP_OK) {
        if (length != sizeof(blob) || memcmp(blob, "WIF1", 4) != 0 ||
            blob[4] == 0 || blob[4] > 32 || blob[5] < 8 || blob[5] > 63) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            memcpy(value->ssid, blob + 6, blob[4]);
            memcpy(value->password, blob + 38, blob[5]);
            if (strlen(value->ssid) != blob[4] || strlen(value->password) != blob[5] ||
                !valid_text(value->ssid, 32, false) || !valid_text(value->password, 63, true))
                error = ESP_ERR_INVALID_STATE;
        }
    }
    clear_secret(blob, sizeof(blob));
    return error;
}

#include "app_wifi_profiles.inc"

static esp_err_t set_station(const credentials_t *value)
{
    wifi_config_t config = {0};
    if (value) {
        memcpy(config.sta.ssid, value->ssid, strlen(value->ssid));
        memcpy(config.sta.password, value->password, strlen(value->password));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        config.sta.pmf_cfg.capable = true;
    }
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    clear_secret(&config, sizeof(config));
    return error;
}

static void wifi_event(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    if (base != WIFI_EVENT || id != WIFI_EVENT_STA_DISCONNECTED) return;
    const wifi_event_sta_disconnected_t *event = data;
    portENTER_CRITICAL(&s_lock);
    s_disconnected = true;
    s_event_generation = s_station_generation;
    s_reason = event ? event->reason : 0;
    portEXIT_CRITICAL(&s_lock);
}

static bool header_matches(httpd_req_t *request, const char *name, const char *expected,
                           bool optional)
{
    char value[80];
    size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0) return optional;
    return length < sizeof(value) &&
        httpd_req_get_hdr_value_str(request, name, value, sizeof(value)) == ESP_OK &&
        strcmp(value, expected) == 0;
}

static bool permitted_request(httpd_req_t *request, bool mutation)
{
    char ip[APP_WIFI_IP_CAPACITY], origin[40];
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_snapshot.setup_active && !s_snapshot.cleanup_pending && now_ms() < s_setup_until;
    memcpy(ip, s_snapshot.setup_ip, sizeof(ip));
    portEXIT_CRITICAL(&s_lock);
    snprintf(origin, sizeof(origin), "http://%s", ip);
    return active && header_matches(request, "Host", ip, false) &&
        header_matches(request, "Origin", origin, !mutation);
}

static esp_err_t send_page(httpd_req_t *request, const char *status, const char *body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    return httpd_resp_sendstr(request, body);
}

static bool escape_html(const char *input, char *output, size_t capacity)
{
    size_t used = 0;
    for (; *input; ++input) {
        const char *replacement = NULL;
        switch (*input) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        size_t length = replacement ? strlen(replacement) : 1U;
        if (used + length >= capacity) return false;
        memcpy(output + used, replacement ? replacement : input, length);
        used += length;
    }
    output[used] = '\0';
    return true;
}

static esp_err_t form_get(httpd_req_t *request)
{
    if (!permitted_request(request, false))
        return send_page(request, "403 Forbidden", "Setup is unavailable.");
    app_wifi_snapshot_t snapshot;
    app_wifi_get_snapshot(&snapshot);
    char ssid[193], stored_ssid[193], token[33], page[2000];
    bool accepts;
    portENTER_CRITICAL(&s_lock);
    memcpy(token, s_token, sizeof(token));
    accepts = s_accept_form;
    portEXIT_CRITICAL(&s_lock);
    if (!escape_html(snapshot.ssid, ssid, sizeof(ssid)) ||
        !escape_html(snapshot.stored_ssid, stored_ssid, sizeof(stored_ssid)))
        return send_page(request, "500 Internal Server Error", "Cannot render setup.");
    static const char *const states[] = {
        "Unavailable", "Idle", "Connecting", "Connected", "Waiting to retry", "Needs attention",
        "Obtaining IP address"
    };
    const char *state = (unsigned)snapshot.state < sizeof(states) / sizeof(states[0]) ?
        states[snapshot.state] : "Unknown";
    const char *stored = snapshot.storage_uncertain ? "Storage contents are uncertain; check the device." :
        !snapshot.stored_data_known ? "Unknown" : !snapshot.stored_data_present ? "None" :
        stored_ssid[0] ? stored_ssid : "Record present; name unavailable";
    int length = snprintf(page, sizeof(page),
        "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-INPUT Wi-Fi setup</title><style>body{font:18px sans-serif;"
        "max-width:32em;margin:2em auto;padding:1em}input,button{font:inherit;"
        "display:block;margin:1em 0;padding:.5em;max-width:90%%}</style>"
        "<h1>Wi-Fi setup</h1><p>Runtime network: %s</p><p>Remembered network: %s</p>"
        "<p>Connection: %s. Setup expires in %lu seconds.</p>"
        "%s<form method=\"post\" action=\"/configure\" autocomplete=\"off\">"
        "<input type=\"hidden\" name=\"token\" value=\"%s\">"
        "<label>Network name<input name=\"ssid\" required maxlength=\"32\"></label>"
        "<label>WPA2 passphrase<input name=\"password\" type=\"password\" required "
        "minlength=\"8\" maxlength=\"63\" autocomplete=\"new-password\"></label>"
        "<button %s>Save network</button></form>"
        "<p>Only Wi-Fi setup is available here. Check the device for the final result."
        " Stop setup on the device when finished.</p></html>",
        snapshot.configured ? ssid : "None", stored, state,
        (unsigned long)(snapshot.setup_remaining_ms / 1000U),
        accepts ? "" : "<p>A submission was accepted. Check the device for completion.</p>",
        accepts ? token : "", accepts ? "" : "disabled");
    clear_secret(token, sizeof(token));
    if (length < 0 || (size_t)length >= sizeof(page))
        return send_page(request, "500 Internal Server Error", "Cannot render setup.");
    esp_err_t result = send_page(request, "200 OK", page);
    clear_secret(page, sizeof(page));
    return result;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool decode_field(const char *input, size_t length, char *output, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (i + 2 >= length) return false;
            int high = hex_value(input[++i]);
            int low = hex_value(input[++i]);
            if (high < 0 || low < 0) return false;
            c = (unsigned char)((high << 4) | low);
        } else if (c == '+') c = ' ';
        if (!c || used + 1 >= capacity) return false;
        output[used++] = (char)c;
    }
    output[used] = '\0';
    return true;
}

static bool parse_form(const char *body, size_t length, credentials_t *credentials, char token[33])
{
    unsigned fields = 0;
    size_t position = 0;
    while (position < length) {
        size_t end = position;
        while (end < length && body[end] != '&') ++end;
        size_t equals = position;
        while (equals < end && body[equals] != '=') ++equals;
        if (equals == end) return false;
        size_t key_length = equals - position;
        char *output;
        size_t capacity;
        unsigned bit;
        if (key_length == 5 && memcmp(body + position, "token", 5) == 0) {
            output = token; capacity = 33; bit = 1;
        } else if (key_length == 4 && memcmp(body + position, "ssid", 4) == 0) {
            output = credentials->ssid; capacity = sizeof(credentials->ssid); bit = 2;
        } else if (key_length == 8 && memcmp(body + position, "password", 8) == 0) {
            output = credentials->password; capacity = sizeof(credentials->password); bit = 4;
        } else return false;
        if ((fields & bit) || !decode_field(body + equals + 1, end - equals - 1, output, capacity))
            return false;
        fields |= bit;
        position = end + 1;
        if (end < length && position == length) return false;
    }
    if (fields != 7 || strlen(token) != 32 || strlen(credentials->password) < 8 ||
        !valid_text(credentials->ssid, 32, false) || !valid_text(credentials->password, 63, true))
        return false;
    return true;
}

static esp_err_t form_post(httpd_req_t *request)
{
    char body[FORM_LIMIT + 1] = {0}, token[33] = {0};
    credentials_t credentials = {0};
    const char *status = "400 Bad Request", *message = "Invalid setup request.";
    if (!permitted_request(request, true)) {
        status = "403 Forbidden"; message = "Setup is unavailable.";
    } else if (request->content_len == 0 || request->content_len > FORM_LIMIT ||
        !header_matches(request, "Content-Type", "application/x-www-form-urlencoded", false)) {
        status = "413 Content Too Large"; message = "Unsupported or oversized form.";
    } else {
        size_t received = 0;
        uint64_t until = now_ms() + 4000U;
        while (received < request->content_len && now_ms() < until) {
            int count = httpd_req_recv(request, body + received, request->content_len - received);
            if (count <= 0) break;
            received += (size_t)count;
        }
        if (received == request->content_len && parse_form(body, received, &credentials, token)) {
            portENTER_CRITICAL(&s_lock);
            unsigned difference = 0;
            for (unsigned i = 0; i < 32; ++i)
                difference |= (unsigned char)token[i] ^ (unsigned char)s_token[i];
            if (difference == 0 && s_accept_form && s_snapshot.setup_active &&
                now_ms() < s_setup_until && !(s_commands & (SAVE_NETWORK | JOIN_NETWORK | STOP_SETUP | FORGET_NETWORK))) {
                s_pending = credentials;
                s_pending_generation = s_generation;
                s_accept_form = false;
                s_commands |= SAVE_NETWORK;
                s_snapshot.operation_pending = true;
                publish();
                status = "202 Accepted";
                message = "Submission accepted, not yet saved or connected. Check the device for completion.";
            } else {
                status = "403 Forbidden"; message = "Setup session expired, changed or already submitted.";
            }
            portEXIT_CRITICAL(&s_lock);
        }
    }
    clear_secret(body, sizeof(body));
    clear_secret(token, sizeof(token));
    clear_secret(&credentials, sizeof(credentials));
    return send_page(request, status, message);
}

static esp_err_t stop_setup(void)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_accept_form = false;
    s_snapshot.cleanup_pending = true;
    ++s_generation;
    clear_secret(s_token, sizeof(s_token));
    clear_secret(&s_setup, sizeof(s_setup));
    clear_secret(&s_pending, sizeof(s_pending));
    s_setup_until = s_close_at = 0;
    s_close_on_connect = false;
    s_snapshot.setup_remaining_ms = 0;
    s_snapshot.setup_ssid[0] = '\0';
    s_snapshot.setup_ip[0] = '\0';
    esp_err_t previous_error = s_snapshot.cleanup_error;
    bool retry_wait = s_cleanup_retry_at && now < s_cleanup_retry_at;
    portEXIT_CRITICAL(&s_lock);
    if (retry_wait) return previous_error;
    esp_err_t error = ESP_OK;
    if (s_server) {
        error = httpd_stop(s_server);
        if (error == ESP_OK) s_server = NULL;
    }
    if (s_ap_cleanup_needed) {
        esp_err_t mode_error = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode_error == ESP_OK) s_ap_cleanup_needed = false;
        if (error == ESP_OK) error = mode_error;
    }
    bool incomplete = s_server != NULL || s_ap_cleanup_needed;
    portENTER_CRITICAL(&s_lock);
    s_snapshot.setup_active = incomplete;
    s_snapshot.cleanup_pending = incomplete;
    s_snapshot.cleanup_error = error;
    s_snapshot.operation_pending = incomplete || s_commands != 0;
    s_cleanup_retry_at = incomplete ? now_ms() + CLEANUP_RETRY_MS : 0;
    s_snapshot.cleanup_retry_in_ms = incomplete ? CLEANUP_RETRY_MS : 0;
    publish();
    portEXIT_CRITICAL(&s_lock);
    return error;
}

static esp_err_t start_setup(void)
{
    uint8_t random[32];
    static const char hex[] = "0123456789abcdef";
    app_wifi_setup_info_t setup = {0};
    char token[33];
    esp_fill_random(random, sizeof(random));
    snprintf(setup.ssid, sizeof(setup.ssid), "ESP32-INPUT-Setup-%02x%02x%02x",
        random[0], random[1], random[2]);
    for (unsigned i = 0; i < 12; ++i) {
        setup.password[i * 2] = hex[random[i + 3] >> 4];
        setup.password[i * 2 + 1] = hex[random[i + 3] & 15U];
    }
    for (unsigned i = 0; i < 16; ++i) {
        token[i * 2] = hex[random[i + 15] >> 4];
        token[i * 2 + 1] = hex[random[i + 15] & 15U];
    }
    token[32] = '\0';
    clear_secret(random, sizeof(random));
    wifi_config_t config = {0};
    memcpy(config.ap.ssid, setup.ssid, strlen(setup.ssid));
    config.ap.ssid_len = (uint8_t)strlen(setup.ssid);
    memcpy(config.ap.password, setup.password, 24);
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 1;
    config.ap.channel = 1;
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    esp_err_t error = s_ap ? ESP_OK : ESP_ERR_NO_MEM;
    /* Treat an attempted AP mode transition as needing explicit cleanup even
     * if a driver error leaves its side effects uncertain. */
    if (error == ESP_OK) {
        s_ap_cleanup_needed = true;
        error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_AP, &config);
    clear_secret(&config, sizeof(config));
    esp_netif_ip_info_t ip;
    struct ifreq interface = {0};
    if (error == ESP_OK) error = esp_netif_get_ip_info(s_ap, &ip);
    if (error == ESP_OK) {
        snprintf(setup.ip, sizeof(setup.ip), IPSTR, IP2STR(&ip.ip));
        error = esp_netif_get_netif_impl_name(s_ap, interface.ifr_name);
    }
    if (error == ESP_OK) {
        httpd_config_t http = HTTPD_DEFAULT_CONFIG();
        http.server_port = 80;
        http.ctrl_port = 32769;
        http.max_open_sockets = 2;
        http.max_uri_handlers = 2;
        http.stack_size = 6144;
        http.recv_wait_timeout = 2;
        http.send_wait_timeout = 2;
        http.lru_purge_enable = true;
        http.if_name = &interface;
        error = httpd_start(&s_server, &http);
    }
    if (error == ESP_OK) {
        const httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = form_get};
        const httpd_uri_t post = {.uri = "/configure", .method = HTTP_POST, .handler = form_post};
        error = httpd_register_uri_handler(s_server, &get);
        if (error == ESP_OK) error = httpd_register_uri_handler(s_server, &post);
    }
    portENTER_CRITICAL(&s_lock);
    if (error == ESP_OK && !(s_commands & (STOP_SETUP | FORGET_NETWORK))) {
        setup.generation = ++s_generation;
        setup.remaining_ms = APP_WIFI_SETUP_TIMEOUT_MS;
        s_setup = setup;
        memcpy(s_token, token, sizeof(token));
        s_setup_until = now_ms() + APP_WIFI_SETUP_TIMEOUT_MS;
        s_snapshot.setup_active = true;
        memcpy(s_snapshot.setup_ssid, setup.ssid, sizeof(setup.ssid));
        memcpy(s_snapshot.setup_ip, setup.ip, sizeof(setup.ip));
        s_accept_form = true;
        s_close_on_connect = false;
        publish();
    } else if (error == ESP_OK) error = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_lock);
    clear_secret(&setup, sizeof(setup));
    clear_secret(token, sizeof(token));
    if (error != ESP_OK) {
        esp_err_t cleanup = stop_setup();
        if (cleanup != ESP_OK) error = cleanup;
    }
    return error;
}

static void backoff(uint64_t now, esp_err_t error, uint16_t reason)
{
    s_connect_until = 0;
    s_retry_at = now + s_retry_delay;
    s_retry_delay = s_retry_delay < RETRY_MAX_MS / 2U ? s_retry_delay * 2U : RETRY_MAX_MS;
    portENTER_CRITICAL(&s_lock);
    s_snapshot.state = APP_WIFI_BACKOFF;
    s_snapshot.ip[0] = '\0';
    s_snapshot.lan_ready = false;
    s_snapshot.rssi_valid = false;
    s_snapshot.connection_error = error;
    s_snapshot.disconnect_reason = reason;
    publish();
    portEXIT_CRITICAL(&s_lock);
    profile_reconnect(now, reason);
}

static void service_step(void)
{
    uint32_t commands, pending_generation;
    uint32_t pending_scan_generation;
    uint16_t pending_record_index;
    bool remember;
    uint64_t pending_selection_until;
    credentials_t pending = {0};
    bool disconnected;
    uint16_t reason;
    portENTER_CRITICAL(&s_lock);
    commands = s_commands;
    s_commands = 0;
    pending_generation = s_pending_generation;
    if (commands & (SAVE_NETWORK | JOIN_NETWORK)) pending = s_pending;
    remember = (commands & SAVE_NETWORK) || s_pending_remember;
    pending_scan_generation = s_pending_scan_generation;
    pending_record_index = s_pending_record_index;
    pending_selection_until = s_pending_selection_until;
    clear_secret(&s_pending, sizeof(s_pending));
    disconnected = s_disconnected && s_event_generation == s_station_generation;
    reason = s_reason;
    s_disconnected = false;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    uint64_t now = now_ms();
    service_scan(commands & (SCAN_CANCEL | FORGET_NETWORK));
    if (commands & PROFILE_MUTATION) {
        esp_err_t mutation = service_profile_mutation();
        if (mutation != ESP_OK) error = mutation;
    }
    if ((commands & FORGET_NETWORK) && scan_busy()) {
        /* Keep the destructive intent, but finish old driver/event ownership
         * before clearing configuration or admitting reconnect. */
        portENTER_CRITICAL(&s_lock);
        s_commands |= FORGET_NETWORK;
        s_snapshot.operation_pending = true;
        portEXIT_CRITICAL(&s_lock);
        clear_secret(&pending, sizeof(pending));
        return;
    }
    if (commands & (STOP_SETUP | FORGET_NETWORK)) error = stop_setup();
    if (commands & FORGET_NETWORK) {
        esp_err_t forgotten = write_credentials(NULL);
        storage_result(forgotten, forgotten == ESP_OK, false, NULL);
        if (forgotten == ESP_OK) {
            forgotten = esp_wifi_disconnect();
            if (forgotten == ESP_ERR_WIFI_NOT_CONNECT) forgotten = ESP_OK;
            esp_err_t cleared = set_station(NULL);
            if (forgotten == ESP_OK) forgotten = cleared;
            if (cleared == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                ++s_station_generation;
                s_snapshot.configured = false;
                s_snapshot.ssid[0] = '\0';
                s_snapshot.ip[0] = '\0';
                s_snapshot.lan_ready = false;
                s_snapshot.rssi_valid = false;
                s_snapshot.state = APP_WIFI_IDLE;
                publish();
                portEXIT_CRITICAL(&s_lock);
                s_connect_until = s_retry_at = 0;
            }
            if (forgotten != ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s_snapshot.state = APP_WIFI_ERROR;
                portEXIT_CRITICAL(&s_lock);
            }
        }
        if (error == ESP_OK) error = forgotten;
    } else if ((commands & (SAVE_NETWORK | JOIN_NETWORK)) && !(commands & STOP_SETUP)) {
        portENTER_CRITICAL(&s_lock);
        bool current = pending_generation == s_generation &&
            ((commands & JOIN_NETWORK) ? (pending_selection_until ? now < pending_selection_until :
                (!pending_scan_generation || fresh_scan_row(pending_scan_generation,
                    pending_record_index, pending.ssid))) : now < s_setup_until);
        portEXIT_CRITICAL(&s_lock);
        if (!current || scan_busy()) error = ESP_ERR_INVALID_STATE;
        else {
            if (remember) {
                error = write_credentials(&pending);
                storage_result(error, error == ESP_OK, error == ESP_OK, &pending);
            }
            portENTER_CRITICAL(&s_lock);
            current = pending_generation == s_generation && !(s_commands & FORGET_NETWORK);
            portEXIT_CRITICAL(&s_lock);
            if (error == ESP_OK && !current) error = ESP_ERR_INVALID_STATE;
            if (error == ESP_OK) {
                error = esp_wifi_disconnect();
                if (error == ESP_ERR_WIFI_NOT_CONNECT) error = ESP_OK;
                if (error == ESP_OK) error = set_station(&pending);
                s_close_on_connect = (commands & SAVE_NETWORK) && error == ESP_OK;
                s_connect_until = 0;
                s_retry_delay = 1000;
                s_retry_at = error == ESP_OK ? now : 0;
                portENTER_CRITICAL(&s_lock);
                if (error == ESP_OK) {
                    ++s_station_generation;
                    s_snapshot.configured = true;
                    memcpy(s_snapshot.ssid, pending.ssid, sizeof(pending.ssid));
                }
                s_snapshot.ip[0] = '\0';
                s_snapshot.lan_ready = false;
                s_snapshot.rssi_valid = false;
                s_snapshot.state = error == ESP_OK ? APP_WIFI_CONNECTING : APP_WIFI_ERROR;
                s_snapshot.connection_error = error;
                publish();
                portEXIT_CRITICAL(&s_lock);
            }
        }
    }
    if ((commands & VERIFY_STORAGE) && !(commands & (SAVE_NETWORK | JOIN_NETWORK | FORGET_NETWORK))) {
        credentials_t stored_credentials = {0};
        bool known, present;
        esp_err_t verified = read_credentials(&stored_credentials, &known, &present);
        storage_result(verified, known, present, &stored_credentials);
        clear_secret(&stored_credentials, sizeof(stored_credentials));
        if (error == ESP_OK && verified != ESP_ERR_NVS_NOT_FOUND) error = verified;
    }
    if ((commands & START_SETUP) && !(commands & (STOP_SETUP | FORGET_NETWORK)))
        error = start_setup();
    if ((commands & SCAN_START) && !(commands & (SCAN_CANCEL | FORGET_NETWORK))) start_scan();
    clear_secret(&pending, sizeof(pending));
    if (commands) set_error(error);
    portENTER_CRITICAL(&s_lock);
    bool configured = s_snapshot.configured;
    bool setup_active = s_snapshot.setup_active;
    bool timeout = setup_active && ((s_setup_until && now >= s_setup_until) ||
        (s_close_at && now >= s_close_at));
    bool cleanup_due = s_snapshot.cleanup_pending && now >= s_cleanup_retry_at;
    s_snapshot.operation_pending = s_commands != 0 || s_snapshot.cleanup_pending;
    portEXIT_CRITICAL(&s_lock);
    if (timeout || cleanup_due) set_error(stop_setup());
    if (configured) {
        wifi_ap_record_t ap = {0};
        esp_netif_ip_info_t ip = {0};
        esp_err_t link = esp_wifi_sta_get_ap_info(&ap);
        bool associated = link == ESP_OK && memcmp(ap.ssid, s_snapshot.ssid, 32) == 0;
        bool connected = associated && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK && ip.ip.addr != 0;
        if (connected) {
            portENTER_CRITICAL(&s_lock);
            s_snapshot.state = APP_WIFI_CONNECTED;
            snprintf(s_snapshot.ip, sizeof(s_snapshot.ip), IPSTR, IP2STR(&ip.ip));
            s_snapshot.rssi_dbm = ap.rssi;
            s_snapshot.rssi_valid = true;
            s_snapshot.lan_ready = true;
            s_snapshot.connection_error = ESP_OK;
            s_snapshot.disconnect_reason = 0;
            publish();
            portEXIT_CRITICAL(&s_lock);
            s_connect_until = s_retry_at = 0;
            s_retry_delay = 1000;
            portENTER_CRITICAL(&s_lock);
            if (setup_active && s_close_on_connect && !s_close_at) s_close_at = now + 5000U;
            portEXIT_CRITICAL(&s_lock);
        } else if ((disconnected && s_connect_until && reason != WIFI_REASON_ASSOC_LEAVE) ||
                   (s_connect_until && now >= s_connect_until) ||
                   (s_snapshot.state == APP_WIFI_CONNECTED)) {
            if (!scan_busy()) (void)esp_wifi_disconnect();
            backoff(now, disconnected ? ESP_FAIL : ESP_ERR_TIMEOUT, disconnected ? reason : 0);
        } else if (associated) {
            portENTER_CRITICAL(&s_lock);
            s_snapshot.state = APP_WIFI_OBTAINING_IP;
            s_snapshot.lan_ready = false;
            s_snapshot.ip[0] = '\0';
            s_snapshot.rssi_dbm = ap.rssi;
            s_snapshot.rssi_valid = true;
            publish();
            portEXIT_CRITICAL(&s_lock);
        }
        if (!scan_busy() && !s_connect_until && s_retry_at && now >= s_retry_at) {
            s_retry_at = 0;
            esp_err_t result = esp_wifi_connect();
            if (result == ESP_OK) {
                s_connect_until = now + CONNECT_TIMEOUT_MS;
                portENTER_CRITICAL(&s_lock);
                s_snapshot.state = APP_WIFI_CONNECTING;
                publish();
                portEXIT_CRITICAL(&s_lock);
            } else backoff(now, result, 0);
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_snapshot.retry_in_ms = s_retry_at > now ? (uint32_t)(s_retry_at - now) : 0;
    s_snapshot.setup_remaining_ms = s_setup_until > now ? (uint32_t)(s_setup_until - now) : 0;
    s_snapshot.cleanup_retry_in_ms = s_cleanup_retry_at > now ?
        (uint32_t)(s_cleanup_retry_at - now) : 0;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t initialize_worker(void)
{
    credentials_t credentials = {0};
    bool known, present;
    esp_err_t stored = read_credentials(&credentials, &known, &present);
    storage_result(stored, known, present, &credentials);
    if (stored != ESP_OK) {
        /* Preserve bad/unreadable storage for explicit local recovery. A
         * storage fault must not disable the only provisioning/forget UI. */
        clear_secret(&credentials, sizeof(credentials));
    }
    bool driver_initialized = false, handler_registered = false, scan_registered = false;
    esp_err_t error = esp_netif_init();
    if (error == ESP_OK) {
        error = esp_event_loop_create_default();
        if (error == ESP_ERR_INVALID_STATE) error = ESP_OK;
    }
    if (error == ESP_OK) {
        s_sta = esp_netif_create_default_wifi_sta();
        if (!s_sta) error = ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (error == ESP_OK) {
        error = esp_wifi_init(&init);
        driver_initialized = error == ESP_OK;
    }
    if (error == ESP_OK) error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) {
        error = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL);
        handler_registered = error == ESP_OK;
    }
    if (error == ESP_OK) {
        error = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_event, NULL);
        scan_registered = error == ESP_OK;
    }
    if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) error = set_station(stored == ESP_OK ? &credentials : NULL);
    if (error == ESP_OK) error = esp_wifi_start();
    portENTER_CRITICAL(&s_lock);
    s_snapshot.initialized = error == ESP_OK;
    s_snapshot.backend_available = false;
    s_snapshot.backend_error = ESP_ERR_NOT_SUPPORTED;
    s_snapshot.operation_pending = false;
    s_snapshot.state = error == ESP_OK ? APP_WIFI_IDLE : APP_WIFI_ERROR;
    s_snapshot.last_error = error != ESP_OK ? error :
        stored != ESP_OK && stored != ESP_ERR_NVS_NOT_FOUND ? stored : ESP_OK;
    if (error == ESP_OK && stored == ESP_OK) {
        s_snapshot.configured = true;
        memcpy(s_snapshot.ssid, credentials.ssid, sizeof(credentials.ssid));
        s_retry_at = now_ms() + 1U;
    }
    publish();
    portEXIT_CRITICAL(&s_lock);
    clear_secret(&credentials, sizeof(credentials));
    if (error != ESP_OK) {
        if (scan_registered)
            (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_event);
        if (handler_registered)
            (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event);
        if (driver_initialized) {
            (void)esp_wifi_stop();
            (void)esp_wifi_deinit();
        }
        if (s_ap) esp_netif_destroy_default_wifi(s_ap);
        if (s_sta) esp_netif_destroy_default_wifi(s_sta);
        s_ap = s_sta = NULL;
    }
    return error;
}

static void worker(void *argument)
{
    (void)argument;
    esp_err_t error = initialize_worker();
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_snapshot.state = APP_WIFI_ERROR;
        s_snapshot.last_error = error;
        s_snapshot.operation_pending = false;
        publish();
        portEXIT_CRITICAL(&s_lock);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        service_step();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t app_wifi_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_started) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;
    s_snapshot.operation_pending = true;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(worker, "app_wifi", 16384, NULL, 2, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_started = false;
        s_snapshot.operation_pending = false;
        s_snapshot.state = APP_WIFI_ERROR;
        s_snapshot.last_error = ESP_ERR_NO_MEM;
        publish();
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_wifi_set_backend_state(bool available,esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    if(s_snapshot.backend_available!=available||s_snapshot.backend_error!=error){s_snapshot.backend_available=available;s_snapshot.backend_error=error;publish();}
    portEXIT_CRITICAL(&s_lock);
}
void app_wifi_set_maintenance(bool active)
{
    portENTER_CRITICAL(&s_lock);
    bool changed=s_maintenance!=active;s_maintenance=active;
    if(changed&&active&&s_snapshot.initialized){
        ++s_generation;s_accept_form=false;s_selection_id=0;memset(s_selection_ssid,0,sizeof(s_selection_ssid));
        if(s_snapshot.setup_active||s_snapshot.cleanup_pending)s_commands|=STOP_SETUP;
        if(s_scan_wait_event||s_scan.state==APP_WIFI_SCAN_QUEUED||s_scan.cleanup_pending)s_commands|=SCAN_CANCEL;
        if (s_commands) s_snapshot.operation_pending=true;
        publish();
    }
    portEXIT_CRITICAL(&s_lock);
}
static esp_err_t admit(unsigned command)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t result = ESP_OK;
    if (!s_snapshot.initialized || (s_maintenance && command != STOP_SETUP && command != VERIFY_STORAGE)) result = ESP_ERR_INVALID_STATE;
    else if ((command == START_SETUP || command == VERIFY_STORAGE) &&
        (s_commands || s_snapshot.operation_pending ||
        (command == START_SETUP && (s_snapshot.setup_active || s_scan_wait_event ||
            s_scan.cleanup_pending || s_scan.state == APP_WIFI_SCAN_QUEUED)))) result = ESP_ERR_INVALID_STATE;
    else {
        if (command & (STOP_SETUP | FORGET_NETWORK)) {
            ++s_generation;
            s_accept_form = false;
            s_snapshot.cleanup_pending = true;
        }
        if (command & (START_SETUP | FORGET_NETWORK)) {
            s_selection_id = 0;
            memset(s_selection_ssid, 0, sizeof(s_selection_ssid));
        }
        s_commands |= command;
        s_snapshot.operation_pending = true;
        publish();
    }
    portEXIT_CRITICAL(&s_lock);
    return result;
}
esp_err_t app_wifi_start_setup(void) { return admit(START_SETUP); }
esp_err_t app_wifi_stop_setup(void) { return admit(STOP_SETUP); }
esp_err_t app_wifi_forget_network(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ambiguous = s_profiles.known && s_profiles.count > 1;
    portEXIT_CRITICAL(&s_lock);
    return ambiguous ? ESP_ERR_INVALID_STATE : admit(FORGET_NETWORK);
}
esp_err_t app_wifi_verify_storage(void) { return admit(VERIFY_STORAGE); }

esp_err_t app_wifi_scan_start(void)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (s_maintenance || !s_snapshot.initialized || s_snapshot.operation_pending || s_commands ||
        s_snapshot.setup_active || s_snapshot.cleanup_pending || s_scan_wait_event ||
        s_scan.cleanup_pending || s_scan.state == APP_WIFI_SCAN_QUEUED ||
        s_snapshot.state == APP_WIFI_CONNECTING || s_snapshot.state == APP_WIFI_OBTAINING_IP ||
        now_ms() < s_scan_next_at || s_scan.generation == UINT32_MAX) error = ESP_ERR_INVALID_STATE;
    else {
        uint32_t generation = s_scan.generation + 1U;
        s_scan = (app_wifi_scan_snapshot_t){.state = APP_WIFI_SCAN_QUEUED, .generation = generation};
        s_scan_diagnostics.current = (app_wifi_scan_observation_t){.known = true,
            .generation = generation, .phase = APP_WIFI_SCAN_PHASE_QUEUED,
            .station_state = s_snapshot.state, .lan_ready = s_snapshot.lan_ready};
        memset(s_scan_records, 0, sizeof(s_scan_records));
        s_scan_next_at = now_ms() + APP_WIFI_SCAN_COOLDOWN_MS;
        s_scan_cleanup_at = 0;
        s_commands |= SCAN_START;
        s_snapshot.operation_pending = true;
        publish();
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

esp_err_t app_wifi_scan_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (!s_snapshot.initialized) error = ESP_ERR_INVALID_STATE;
    else {
        s_commands |= SCAN_CANCEL;
        s_scan.state = APP_WIFI_SCAN_CANCELLING;
        s_scan.count = 0;
        s_snapshot.operation_pending = true;
        publish();
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

void app_wifi_get_scan_snapshot(app_wifi_scan_snapshot_t *snapshot)
{
    if (!snapshot) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_scan;
    uint64_t now = now_ms();
    snapshot->retry_in_ms = s_scan_next_at > now ? (uint32_t)(s_scan_next_at - now) : 0;
    if (snapshot->state == APP_WIFI_SCAN_READY) {
        uint64_t age = now - s_scan_completed_at;
        snapshot->age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        snapshot->remaining_ms = age < APP_WIFI_SCAN_RESULT_TTL_MS ?
            APP_WIFI_SCAN_RESULT_TTL_MS - (uint32_t)age : 0;
        if (!snapshot->remaining_ms) snapshot->state = APP_WIFI_SCAN_EXPIRED;
    }
    portEXIT_CRITICAL(&s_lock);
}

void app_wifi_get_scan_diagnostics(app_wifi_scan_diagnostics_t *diagnostics)
{
    if (!diagnostics) return;
    portENTER_CRITICAL(&s_lock);
    *diagnostics = s_scan_diagnostics;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_wifi_get_scan_record(uint32_t generation, uint16_t index,
                                  app_wifi_scan_record_t *record)
{
    if (!record) return ESP_ERR_INVALID_ARG;
    memset(record, 0, sizeof(*record));
    scan_record_t source;
    portENTER_CRITICAL(&s_lock);
    bool valid = s_scan.state == APP_WIFI_SCAN_READY && !s_scan.cleanup_pending &&
        generation == s_scan.generation && index < s_scan.count &&
        now_ms() - s_scan_completed_at < APP_WIFI_SCAN_RESULT_TTL_MS;
    if (valid) source = s_scan_records[index];
    portEXIT_CRITICAL(&s_lock);
    if (!valid) return ESP_ERR_INVALID_STATE;
    record->ssid_length = source.malformed_ssid ? 32U : (uint8_t)strnlen(source.ssid, 32);
    memcpy(record->ssid, source.ssid, record->ssid_length);
    memcpy(record->bssid, source.bssid, sizeof(record->bssid));
    record->channel = source.channel;
    record->rssi_dbm = source.rssi;
    record->sdk_authmode = source.authmode;
    record->security = scan_security(source.authmode);
    record->availability = scan_availability(&source);
    record->current = source.current;
    record->remembered = source.remembered;
    if (!source.malformed_ssid && valid_text(source.ssid, 32, false)) {
        memcpy(record->label, source.ssid, record->ssid_length);
    } else if (!record->ssid_length) {
        snprintf(record->label, sizeof(record->label), "Hidden network");
    } else {
        size_t used = 0;
        for (unsigned i = 0; i < record->ssid_length; ++i) {
            unsigned char c = record->ssid[i];
            if (c >= 32 && c < 127 && c != '\\') record->label[used++] = (char)c;
            else {
                snprintf(record->label + used, sizeof(record->label) - used, "\\x%02X", c);
                used += 4;
            }
        }
    }
    return ESP_OK;
}

static bool valid_selection_owner(const app_wifi_selection_owner_t *owner)
{
    if (!owner) return true;
    unsigned principal=0,session=0;
    for (unsigned i=0;i<16;++i) {principal|=owner->principal_id[i];session|=owner->session_id[i];}
    return principal && session;
}
static bool same_selection_owner(const app_wifi_selection_owner_t *owner)
{
    const app_wifi_selection_owner_t local={0};
    return !memcmp(&s_selection_owner,owner?owner:&local,sizeof(local));
}
esp_err_t app_wifi_select_scan_record_owned(uint32_t generation, uint16_t index,
    const app_wifi_selection_owner_t *owner, app_wifi_selection_t *selection)
{
    if (!selection || !valid_selection_owner(owner)) return ESP_ERR_INVALID_ARG;
    memset(selection, 0, sizeof(*selection));
    app_wifi_scan_record_t record;
    esp_err_t error = app_wifi_get_scan_record(generation, index, &record);
    if (error != ESP_OK) return error;
    portENTER_CRITICAL(&s_lock);
    if ((s_selection_id && now_ms() < s_selection_until && !same_selection_owner(owner)) ||
        s_maintenance || !s_snapshot.initialized || s_snapshot.operation_pending || s_commands ||
        s_snapshot.setup_active || s_snapshot.cleanup_pending ||
        s_selection_counter == UINT32_MAX || index >= s_scan.count ||
        !fresh_scan_row(generation, index, s_scan_records[index].ssid)) error = ESP_ERR_INVALID_STATE;
    else {
        s_selection_id = ++s_selection_counter;
        s_selection_owner = owner ? *owner : (app_wifi_selection_owner_t){0};
        s_selection_scan_generation = generation;
        s_selection_index = index;
        s_selection_until = now_ms() + APP_WIFI_SELECTION_TIMEOUT_MS;
        memcpy(s_selection_ssid, s_scan_records[index].ssid, sizeof(s_selection_ssid));
        s_selection_authmode = s_scan_records[index].authmode;
        *selection = (app_wifi_selection_t){.id = s_selection_id, .scan_generation = generation,
            .record_index = index, .remaining_ms = APP_WIFI_SELECTION_TIMEOUT_MS, .record = record};
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

esp_err_t app_wifi_release_selection_owned(uint32_t selection_id,const app_wifi_selection_owner_t *owner)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (valid_selection_owner(owner) && same_selection_owner(owner) && selection_id && selection_id == s_selection_id) {
        s_selection_id = 0;
        memset(s_selection_ssid, 0, sizeof(s_selection_ssid));
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

/* The caller holds the one manager lock. This only validates cached state and
 * copies into the worker mailbox; no SDK/radio/storage operation runs here. */
static esp_err_t join_locked(const app_wifi_join_request_t *request,const app_wifi_selection_owner_t *owner)
{
    esp_err_t error = ESP_OK;
    if ((s_selection_id && now_ms() < s_selection_until && !same_selection_owner(owner)) ||
        s_maintenance || !s_snapshot.initialized || s_snapshot.operation_pending || s_commands ||
        s_snapshot.setup_active || s_snapshot.cleanup_pending || s_scan_wait_event ||
        s_scan.cleanup_pending || s_scan.state == APP_WIFI_SCAN_QUEUED ||
        (request->selection_id ? (request->selection_id != s_selection_id || !same_selection_owner(owner) || now_ms() >= s_selection_until ||
            request->scan_generation != s_selection_scan_generation || request->record_index != s_selection_index ||
            strcmp(request->ssid, s_selection_ssid) != 0 ||
            (s_selection_authmode != WIFI_AUTH_WPA2_PSK && s_selection_authmode != WIFI_AUTH_WPA_WPA2_PSK)) :
            (request->scan_generation && !fresh_scan_row(request->scan_generation,
                request->record_index, request->ssid)))) error = ESP_ERR_INVALID_STATE;
    else if (request->remember && !profile_capacity_locked(request->ssid)) error = ESP_ERR_NO_MEM;
    else {
        memset(&s_pending, 0, sizeof(s_pending));
        memcpy(s_pending.ssid, request->ssid, strlen(request->ssid));
        memcpy(s_pending.password, request->password, strlen(request->password));
        s_pending_generation = s_generation;
        s_pending_remember = request->remember;
        s_pending_scan_generation = request->scan_generation;
        s_pending_record_index = request->record_index;
        s_pending_selection_until = request->selection_id ? s_selection_until : 0;
        if (request->selection_id) {
            s_selection_id = 0;
            memset(s_selection_ssid, 0, sizeof(s_selection_ssid));
        }
        s_commands |= JOIN_NETWORK;
        s_snapshot.operation_pending = true;
        publish();
    }
    return error;
}
esp_err_t app_wifi_join_owned(const app_wifi_join_request_t *request,const app_wifi_selection_owner_t *owner)
{
    if (!valid_selection_owner(owner) || !request || !valid_text(request->ssid, 32, false) ||
        !valid_text(request->password, 63, true) || strlen(request->password) < 8 ||
        (!request->scan_generation && (request->record_index || request->selection_id))) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error=join_locked(request,owner);
    portEXIT_CRITICAL(&s_lock);
    return error;
}

esp_err_t app_wifi_select_scan_record(uint32_t generation,uint16_t index,app_wifi_selection_t *out)
{return app_wifi_select_scan_record_owned(generation,index,NULL,out);}
esp_err_t app_wifi_release_selection(uint32_t id)
{return app_wifi_release_selection_owned(id,NULL);}
esp_err_t app_wifi_join(const app_wifi_join_request_t *request)
{return app_wifi_join_owned(request,NULL);}

void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot)
{
    if (!snapshot) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    uint64_t now = now_ms();
    if (s_snapshot.setup_active && !s_snapshot.cleanup_pending &&
        s_setup_until > now && s_setup.password[0] &&
        !(s_commands & (STOP_SETUP | FORGET_NETWORK))) {
        *info = s_setup;
        info->remaining_ms = (uint32_t)(s_setup_until - now);
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

#else
void app_wifi_get_scan_diagnostics(app_wifi_scan_diagnostics_t *out)
{ if (out) memset(out, 0, sizeof(*out)); }
esp_err_t app_wifi_profile_result(uint64_t id,app_wifi_profile_result_t *out){(void)id;if(out)memset(out,0,sizeof(*out));return ESP_ERR_NOT_SUPPORTED;}
void app_wifi_set_backend_state(bool available,esp_err_t error){(void)available;(void)error;}
esp_err_t app_wifi_profiles_forget_all_request(uint64_t rev,bool ack,uint64_t *id){(void)rev;(void)ack;if(id)*id=0;return ESP_ERR_NOT_SUPPORTED;}
void app_wifi_set_maintenance(bool active){(void)active;}
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *out) { if(out)*out=(app_wifi_profiles_snapshot_t){.capacity=APP_WIFI_PROFILE_CAPACITY,.error=ESP_ERR_NOT_SUPPORTED}; }
esp_err_t app_wifi_profile_save(const app_wifi_profile_save_t *p) { (void)p;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_profile_join(const char *id,uint64_t rev) { (void)id;(void)rev;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_profile_forget(const char *id,uint64_t rev) { (void)id;(void)rev;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_profiles_reorder(const char ids[APP_WIFI_PROFILE_CAPACITY][APP_WIFI_PROFILE_ID_CAPACITY],uint8_t count,uint64_t rev) {(void)ids;(void)count;(void)rev;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t app_wifi_profiles_forget_all(uint64_t rev,bool ack) {(void)rev;(void)ack;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t app_wifi_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_start_setup(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_stop_setup(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_forget_network(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_verify_storage(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_scan_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_scan_cancel(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_join(const app_wifi_join_request_t *request)
{
    (void)request;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_wifi_select_scan_record_owned(uint32_t generation,uint16_t index,
    const app_wifi_selection_owner_t *owner,app_wifi_selection_t *out)
{(void)owner;(void)generation;(void)index;if(out)memset(out,0,sizeof(*out));return ESP_ERR_NOT_SUPPORTED;}
esp_err_t app_wifi_release_selection_owned(uint32_t id,const app_wifi_selection_owner_t *owner)
{(void)id;(void)owner;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t app_wifi_join_owned(const app_wifi_join_request_t *request,const app_wifi_selection_owner_t *owner)
{(void)request;(void)owner;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t app_wifi_select_scan_record(uint32_t generation, uint16_t index, app_wifi_selection_t *selection)
{
    (void)generation;
    (void)index;
    if (selection) memset(selection, 0, sizeof(*selection));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_wifi_release_selection(uint32_t selection_id)
{
    (void)selection_id;
    return ESP_ERR_NOT_SUPPORTED;
}
void app_wifi_get_scan_snapshot(app_wifi_scan_snapshot_t *snapshot)
{
    if (snapshot) *snapshot = (app_wifi_scan_snapshot_t){.state = APP_WIFI_SCAN_ERROR,
        .error = ESP_ERR_NOT_SUPPORTED};
}
esp_err_t app_wifi_get_scan_record(uint32_t generation, uint16_t index, app_wifi_scan_record_t *record)
{
    (void)generation;
    (void)index;
    if (record) memset(record, 0, sizeof(*record));
    return ESP_ERR_NOT_SUPPORTED;
}
void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot)
{
    if (snapshot) *snapshot = (app_wifi_snapshot_t){.state = APP_WIFI_UNAVAILABLE,
        .last_error = ESP_ERR_NOT_SUPPORTED, .storage_error = ESP_ERR_NOT_SUPPORTED,
        .backend_error = ESP_ERR_NOT_SUPPORTED};
}
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
