#include "passport_setup.h"
#include "passport_wifi.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

extern const char setup_html[] __asm__("_binary_setup_html_start");
extern const char setup_css[] __asm__("_binary_setup_css_start");
extern const char setup_js[] __asm__("_binary_setup_js_start");

// The single HTTP task serializes handlers. Neither this snapshot nor JSON contains passwords.
static passport_wifi_state_t wifi;
static char body[513], response[6144];

static void headers(httpd_req_t *req, const char *type)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
}
static esp_err_t fail(httpd_req_t *req, const char *status, const char *message)
{
    headers(req, "application/json");
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, message); // Fixed JSON literals only.
}
static bool header(httpd_req_t *req, const char *key, char *out, size_t capacity)
{
    size_t length = httpd_req_get_hdr_value_len(req, key);
    return length && length < capacity && httpd_req_get_hdr_value_str(req, key, out, capacity) == ESP_OK;
}
static bool ap_request(httpd_req_t *req)
{
    passport_wifi_snapshot(&wifi);
    if (!wifi.initialized || !wifi.ap_started || !wifi.dhcp_running || !wifi.ap_ip[0]) return false;
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    struct in_addr expected;
    if (inet_pton(AF_INET, wifi.ap_ip, &expected) != 1 || !expected.s_addr ||
        getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &length) != 0 ||
        length != sizeof(local) || local.sin_family != AF_INET || local.sin_addr.s_addr != expected.s_addr) return false;
    // Provisioning runs only in a physically selected AP session. Also reject an
    // associated STA, including overlapping LAN/AP subnets and ambiguous ingress.
    wifi_ap_record_t station;
    if (esp_wifi_sta_get_ap_info(&station) != ESP_ERR_WIFI_NOT_CONNECT) return false;
    char host[32], with_port[32], origin[48], expected_origin[48];
    snprintf(with_port, sizeof(with_port), "%s:80", wifi.ap_ip);
    if (!header(req, "Host", host, sizeof(host)) || (strcmp(host, wifi.ap_ip) && strcmp(host, with_port))) return false;
    if (httpd_req_get_hdr_value_len(req, "Origin")) {
        snprintf(expected_origin, sizeof(expected_origin), "http://%s", host);
        if (!header(req, "Origin", origin, sizeof(origin)) || strcmp(origin, expected_origin)) return false;
    }
    return true;
}
static esp_err_t asset(httpd_req_t *req, const char *data, const char *type)
{
    if (!ap_request(req)) return fail(req, "403 Forbidden", "{\"error\":\"Connect to the Passport AP to open setup.\"}");
    headers(req, type);
    return httpd_resp_sendstr(req, data);
}
static esp_err_t html_get(httpd_req_t *req) { return asset(req, setup_html, "text/html; charset=utf-8"); }
static esp_err_t css_get(httpd_req_t *req) { return asset(req, setup_css, "text/css; charset=utf-8"); }
static esp_err_t js_get(httpd_req_t *req) { return asset(req, setup_js, "text/javascript; charset=utf-8"); }

static void ssid_hex(const passport_ssid_t *ssid, char text[65])
{
    for (unsigned i = 0; i < ssid->length; ++i) snprintf(text + i * 2, 3, "%02X", ssid->bytes[i]);
    text[ssid->length * 2] = 0;
}
static esp_err_t state_get(httpd_req_t *req)
{
    if (!ap_request(req)) return fail(req, "403 Forbidden", "{\"error\":\"Setup is available on the Passport AP only.\"}");
    cJSON *root = cJSON_CreateObject();
    char token[33] = {0}, hex[65];
    if (!root) goto exhausted;
    #define ADD(kind, name, value) do { if (!cJSON_Add##kind##ToObject(root, name, value)) goto exhausted; } while (0)
    ADD(Bool, "open", wifi.setup_open);
    ADD(Number, "setupId", wifi.setup_id);
    ADD(Number, "seconds", wifi.setup_seconds);
    if (wifi.setup_open && passport_wifi_setup_token(token)) ADD(String, "token", token);
    ADD(Bool, "storageKnown", wifi.storage_known);
    ADD(Number, "profileCount", wifi.profile_count);
    ADD(Bool, "scanning", wifi.scanning);
    ADD(Bool, "scanKnown", wifi.scan_known);
    ADD(Bool, "truncated", wifi.scan_truncated);
    ADD(Number, "scanRevision", wifi.scan_revision);
    ADD(Bool, "pending", wifi.pending);
    ADD(Number, "requestId", wifi.pending_id);
    ADD(Number, "requestState", wifi.request_state);
    ADD(String, "requestResult", wifi.request_result);
    ADD(Number, "lastSubmissionId", wifi.last_submission_id);
    ADD(Bool, "submissionAccepted", wifi.submission_accepted);
    ADD(String, "result", wifi.result);
    ADD(String, "staIp", wifi.sta_ip);
    ADD(String, "apIp", wifi.ap_ip);
    #undef ADD
    cJSON *rows = cJSON_AddArrayToObject(root, "rows");
    if (!rows) goto exhausted;
    for (unsigned i = 0; wifi.scan_known && i < wifi.scan_count; ++i) {
        const passport_wifi_row_t *row = &wifi.rows[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) goto exhausted;
        if (!cJSON_AddItemToArray(rows, item)) { cJSON_Delete(item); goto exhausted; }
        ssid_hex(&row->ssid, hex);
        if (!cJSON_AddStringToObject(item, "ssidHex", hex) || !cJSON_AddNumberToObject(item, "security", row->security) ||
            !cJSON_AddNumberToObject(item, "rssi", row->rssi) || !cJSON_AddNumberToObject(item, "profileId", row->profile_id) ||
            !cJSON_AddBoolToObject(item, "current", row->current)) goto exhausted;
    }
    if (!cJSON_PrintPreallocated(root, response, sizeof(response), false)) goto exhausted;
    cJSON_Delete(root); explicit_bzero(token, sizeof(token));
    headers(req, "application/json");
    esp_err_t rc = httpd_resp_sendstr(req, response);
    explicit_bzero(response, sizeof(response));
    return rc;
exhausted:
    cJSON_Delete(root); explicit_bzero(token, sizeof(token)); explicit_bzero(response, sizeof(response));
    return fail(req, "503 Service Unavailable", "{\"error\":\"Setup state unavailable; try again.\"}");
}

// Only a flat JSON object is accepted. Bound parser recursion before invoking
// cJSON and reject Unicode escapes (including embedded NUL) in ASCII credentials.
static bool flat_json(const char *text)
{
    bool string = false, escape = false;
    unsigned depth = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (string) {
            if (escape) { if (*p == 'u') return false; escape = false; }
            else if (*p == '\\') escape = true;
            else if (*p == '"') string = false;
        } else if (*p == '"') string = true;
        else if (*p == '{') { if (++depth != 1) return false; }
        else if (*p == '}') { if (!depth) return false; --depth; }
        else if (*p == '[' || *p == ']') return false;
    }
    return !string && !depth;
}
static bool integer(cJSON *value, uint32_t *out)
{
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0 && value->valuedouble <= UINT32_MAX)) return false;
    *out = (uint32_t)value->valuedouble;
    return value->valuedouble == (double)*out;
}
static bool fields(cJSON *root, passport_wifi_submission_t *out, bool scan)
{
    if (!cJSON_IsObject(root)) return false;
    unsigned seen = 0;
    for (cJSON *item = root->child; item; item = item->next) {
        unsigned bit;
        if (!strcmp(item->string, "setupId")) { bit = 1; if (!integer(item, &out->setup_id) || !out->setup_id) return false; }
        else if (!scan && !strcmp(item->string, "scanRevision")) { bit = 2; if (!integer(item, &out->scan_revision)) return false; }
        else if (!scan && !strcmp(item->string, "row")) { bit = 4; if (!integer(item, &out->row) || out->row >= PASSPORT_WIFI_SCAN_ROWS) return false; }
        else if (!scan && !strcmp(item->string, "password")) {
            bit = 8;
            if (!cJSON_IsString(item)) return false;
            size_t length = strlen(item->valuestring);
            if (length >= sizeof(out->password)) return false;
            for (size_t i = 0; i < length; ++i) if ((unsigned char)item->valuestring[i] < 32 || (unsigned char)item->valuestring[i] > 126) return false;
            memcpy(out->password, item->valuestring, length);
        } else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    return seen == (scan ? 1u : 15u);
}
static esp_err_t post(httpd_req_t *req, bool scan)
{
    // Close POST sockets even on rejection, so unread/oversized input is discarded.
    httpd_resp_set_hdr(req, "Connection", "close");
    const char *status = "400 Bad Request", *message = "{\"error\":\"Invalid setup request.\"}";
    char expected[33] = {0}, token[33] = {0}, type[32];
    passport_wifi_submission_t submission = {0};
    cJSON *root = NULL;
    if (!ap_request(req)) { status = "403 Forbidden"; message = "{\"error\":\"Connect to the Passport AP.\"}"; goto done; }
    if (!wifi.setup_open || !passport_wifi_setup_token(expected) || !header(req, "X-Passport-Setup", token, sizeof(token))) {
        status = "403 Forbidden"; message = "{\"error\":\"Open setup on Passport, then refresh this page.\"}"; goto done;
    }
    unsigned difference = 0;
    for (unsigned i = 0; i < sizeof(token); ++i) difference |= (unsigned char)token[i] ^ (unsigned char)expected[i];
    if (difference) { status = "403 Forbidden"; goto done; }
    if (!header(req, "Content-Type", type, sizeof(type)) || strcmp(type, "application/json")) { status = "415 Unsupported Media Type"; goto done; }
    if (!req->content_len || req->content_len >= sizeof(body)) { status = "413 Content Too Large"; goto done; }
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 3000000;
    while (received < req->content_len) {
        if (esp_timer_get_time() >= deadline) { status = "408 Request Timeout"; goto done; }
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) { status = "408 Request Timeout"; goto done; }
        received += count;
    }
    body[received] = 0;
    if (strlen(body) != received || !flat_json(body)) goto done;
    root = cJSON_ParseWithLengthOpts(body, received + 1, NULL, true);
    if (!root || !fields(root, &submission, scan)) goto done;
    if (submission.setup_id != wifi.setup_id || wifi.pending || !wifi.storage_known) {
        status = "409 Conflict"; message = "{\"error\":\"Setup changed or another request is pending. Refresh the page.\"}"; goto done;
    }
    bool queued;
    if (scan) queued = passport_wifi_scan(submission.setup_id, token);
    else {
        if (!wifi.scan_known || wifi.scanning || submission.scan_revision != wifi.scan_revision || submission.row >= wifi.scan_count) {
            status = "409 Conflict"; message = "{\"error\":\"Network list changed. Select again.\"}"; goto done;
        }
        const passport_wifi_row_t *row = &wifi.rows[submission.row];
        size_t length = strlen(submission.password);
        if (!row->ssid.length || row->security == PW_UNSUPPORTED || (row->security == PW_OPEN ? length != 0 : length < 8)) goto done;
        if (!row->profile_id && wifi.profile_count >= PASSPORT_WIFI_PROFILES) { status = "409 Conflict"; message = "{\"error\":\"Profiles full. Forget a profile on Passport first.\"}"; goto done; }
        submission.request_id = esp_random();
        if (!submission.request_id || submission.request_id == wifi.pending_id) submission.request_id = wifi.pending_id == UINT32_MAX ? 1 : wifi.pending_id + 1;
        queued = passport_wifi_submit(&submission, token);
    }
    if (!queued) { status = "503 Service Unavailable"; message = "{\"error\":\"Device queue busy. Nothing was queued; try again.\"}"; goto done; }
    status = "202 Accepted";
    snprintf(response, sizeof(response), "{\"queued\":true,\"requestId\":%lu}", (unsigned long)submission.request_id);
    message = response;
done:
    // cJSON owns a decoded copy as well as the bounded receive/command buffers.
    if (root) for (cJSON *item = root->child; item; item = item->next) if (cJSON_IsString(item)) explicit_bzero(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(root);
    explicit_bzero(body, sizeof(body)); explicit_bzero(expected, sizeof(expected)); explicit_bzero(token, sizeof(token)); explicit_bzero(&submission, sizeof(submission));
    fail(req, status, message);
    explicit_bzero(response, sizeof(response));
    return ESP_FAIL; // HTTPD closes the session after sending the response.
}
static esp_err_t scan_post(httpd_req_t *req) { return post(req, true); }
static esp_err_t submit_post(httpd_req_t *req) { return post(req, false); }
esp_err_t passport_setup_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/setup", .method = HTTP_GET, .handler = html_get},
        {.uri = "/setup.css", .method = HTTP_GET, .handler = css_get},
        {.uri = "/setup.js", .method = HTTP_GET, .handler = js_get},
        {.uri = "/api/setup/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/setup/scan", .method = HTTP_POST, .handler = scan_post},
        {.uri = "/api/setup/submit", .method = HTTP_POST, .handler = submit_post},
    };
    passport_wifi_set_provisioning_available(false);
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t rc = httpd_register_uri_handler(server, &routes[i]);
        if (rc != ESP_OK) return rc; // Caller stops the entire server on failure.
    }
    passport_wifi_set_provisioning_available(true);
    return ESP_OK;
}
