// SPDX-License-Identifier: Apache-2.0
#include "app_wifi.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_WIFI_SUPPORTED && CONFIG_ESP_WIFI_ENABLED
#include <net/if.h>
#include "esp_event.h"
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
#define FORM_LIMIT 512U
#define CONNECT_TIMEOUT_MS 15000U
#define RETRY_MAX_MS 30000U
#define STORE_LENGTH 101U
#define CLEANUP_RETRY_MS 1000U

typedef struct { char ssid[33]; char password[64]; } credentials_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_wifi_snapshot_t s_snapshot;
static app_wifi_setup_info_t s_setup;
static char s_token[33];
static credentials_t s_pending;
static uint32_t s_pending_generation, s_generation, s_station_generation;
static uint32_t s_commands;
static bool s_started, s_accept_form, s_close_on_connect;
static bool s_ap_cleanup_needed;
static httpd_handle_t s_server;
static esp_netif_t *s_sta, *s_ap;
static uint64_t s_setup_until, s_retry_at, s_connect_until, s_close_at;
static uint64_t s_cleanup_retry_at;
static uint32_t s_retry_delay = 1000U;
static uint16_t s_reason;
static uint32_t s_event_generation;
static bool s_disconnected;

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

static void storage_result(esp_err_t error, bool known, bool present,
                           const credentials_t *credentials)
{
    bool valid = error == ESP_OK || (error == ESP_ERR_NVS_NOT_FOUND && known && !present);
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

static esp_err_t read_credentials(credentials_t *value, bool *known, bool *present)
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

static esp_err_t write_credentials(const credentials_t *value)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("app_wifi", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    if (!value) {
        error = nvs_erase_key(handle, "network");
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    } else {
        uint8_t blob[STORE_LENGTH] = {0};
        memcpy(blob, "WIF1", 4);
        blob[4] = (uint8_t)strlen(value->ssid);
        blob[5] = (uint8_t)strlen(value->password);
        memcpy(blob + 6, value->ssid, blob[4]);
        memcpy(blob + 38, value->password, blob[5]);
        error = nvs_set_blob(handle, "network", blob, sizeof(blob));
        clear_secret(blob, sizeof(blob));
    }
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

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
        "Unavailable", "Idle", "Connecting", "Connected", "Waiting to retry", "Needs attention"
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
                now_ms() < s_setup_until && !(s_commands & (SAVE_NETWORK | STOP_SETUP | FORGET_NETWORK))) {
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
    /* Treat an attempted AP mode transition as needing explicit cleanup even
     * if a driver error leaves its side effects uncertain. */
    s_ap_cleanup_needed = true;
    esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);
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
    s_snapshot.rssi_valid = false;
    s_snapshot.connection_error = error;
    s_snapshot.disconnect_reason = reason;
    publish();
    portEXIT_CRITICAL(&s_lock);
}

static void service_step(void)
{
    uint32_t commands, pending_generation;
    credentials_t pending = {0};
    bool disconnected;
    uint16_t reason;
    portENTER_CRITICAL(&s_lock);
    commands = s_commands;
    s_commands = 0;
    pending_generation = s_pending_generation;
    if (commands & SAVE_NETWORK) pending = s_pending;
    clear_secret(&s_pending, sizeof(s_pending));
    disconnected = s_disconnected && s_event_generation == s_station_generation;
    reason = s_reason;
    s_disconnected = false;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    uint64_t now = now_ms();
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
    } else if ((commands & SAVE_NETWORK) && !(commands & STOP_SETUP)) {
        portENTER_CRITICAL(&s_lock);
        bool current = pending_generation == s_generation && now < s_setup_until;
        portEXIT_CRITICAL(&s_lock);
        if (!current) error = ESP_ERR_INVALID_STATE;
        else {
            error = write_credentials(&pending);
            storage_result(error, error == ESP_OK, error == ESP_OK, &pending);
            if (error == ESP_OK) {
                (void)esp_wifi_disconnect();
                error = set_station(&pending);
                s_close_on_connect = error == ESP_OK;
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
                s_snapshot.rssi_valid = false;
                s_snapshot.state = error == ESP_OK ? APP_WIFI_CONNECTING : APP_WIFI_ERROR;
                s_snapshot.connection_error = error;
                publish();
                portEXIT_CRITICAL(&s_lock);
            }
        }
    }
    if ((commands & VERIFY_STORAGE) && !(commands & (SAVE_NETWORK | FORGET_NETWORK))) {
        credentials_t stored_credentials = {0};
        bool known, present;
        esp_err_t verified = read_credentials(&stored_credentials, &known, &present);
        storage_result(verified, known, present, &stored_credentials);
        clear_secret(&stored_credentials, sizeof(stored_credentials));
        if (error == ESP_OK && verified != ESP_ERR_NVS_NOT_FOUND) error = verified;
    }
    if ((commands & START_SETUP) && !(commands & (STOP_SETUP | FORGET_NETWORK)))
        error = start_setup();
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
        bool connected = link == ESP_OK && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK &&
            ip.ip.addr != 0 && strcmp((const char *)ap.ssid, s_snapshot.ssid) == 0;
        if (connected) {
            portENTER_CRITICAL(&s_lock);
            s_snapshot.state = APP_WIFI_CONNECTED;
            snprintf(s_snapshot.ip, sizeof(s_snapshot.ip), IPSTR, IP2STR(&ip.ip));
            s_snapshot.rssi_dbm = ap.rssi;
            s_snapshot.rssi_valid = true;
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
            (void)esp_wifi_disconnect();
            backoff(now, disconnected ? ESP_FAIL : ESP_ERR_TIMEOUT, disconnected ? reason : 0);
        }
        if (!s_connect_until && s_retry_at && now >= s_retry_at) {
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
    bool driver_initialized = false, handler_registered = false;
    esp_err_t error = esp_netif_init();
    if (error == ESP_OK) {
        error = esp_event_loop_create_default();
        if (error == ESP_ERR_INVALID_STATE) error = ESP_OK;
    }
    if (error == ESP_OK) {
        s_sta = esp_netif_create_default_wifi_sta();
        s_ap = esp_netif_create_default_wifi_ap();
        if (!s_sta || !s_ap) error = ESP_ERR_NO_MEM;
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
    if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) error = set_station(stored == ESP_OK ? &credentials : NULL);
    if (error == ESP_OK) error = esp_wifi_start();
    portENTER_CRITICAL(&s_lock);
    s_snapshot.initialized = error == ESP_OK;
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
    if (xTaskCreate(worker, "app_wifi", 8192, NULL, 2, NULL) != pdPASS) {
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

static esp_err_t admit(unsigned command)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t result = ESP_OK;
    if (!s_snapshot.initialized) result = ESP_ERR_INVALID_STATE;
    else if ((command == START_SETUP || command == VERIFY_STORAGE) &&
        (s_commands || s_snapshot.operation_pending ||
        (command == START_SETUP && s_snapshot.setup_active))) result = ESP_ERR_INVALID_STATE;
    else {
        if (command & (STOP_SETUP | FORGET_NETWORK)) {
            ++s_generation;
            s_accept_form = false;
            s_snapshot.cleanup_pending = true;
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
esp_err_t app_wifi_forget_network(void) { return admit(FORGET_NETWORK); }
esp_err_t app_wifi_verify_storage(void) { return admit(VERIFY_STORAGE); }
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
esp_err_t app_wifi_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_start_setup(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_stop_setup(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_forget_network(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_wifi_verify_storage(void) { return ESP_ERR_NOT_SUPPORTED; }
void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot)
{
    if (snapshot) *snapshot = (app_wifi_snapshot_t){.state = APP_WIFI_UNAVAILABLE,
        .last_error = ESP_ERR_NOT_SUPPORTED, .storage_error = ESP_ERR_NOT_SUPPORTED};
}
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
