// SPDX-License-Identifier: Apache-2.0
/* Host-only: the real HTTP adapter/protocol/coordinator/engine, with SDK socket,
 * local approval, trusted clock, NVS and HID-send boundaries substituted. */
#define WEB_CONTROLLER_NATIVE 1
#define main coordinator_suite_main
#include "../coordinator/test_coordinator.c"
#undef main
#include "esp_http_server.h"
#include "app_control_web.h"
#include "app_control_access.h"
#include "control_json.h"
#include "nvs.h"

typedef void *SemaphoreHandle_t;
typedef struct {
    cp_context_t *protocol;
    SemaphoreHandle_t mutex;
    char body[CP_MAX_FRAME_BYTES + 1U];
    cp_http_response_t response;
} server_t;
static server_t web_server;
static char web_host[129], web_origin[193];
static unsigned web_random, web_completed;
static size_t web_cancel_after;
static uint8_t web_banks[2][1024];
static size_t web_bank_sizes[2];

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name)
{
    for (unsigned i = 0; i < r->header_count; ++i) if (!strcmp(name, r->headers[i].name)) return strlen(r->headers[i].value);
    return 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t size)
{
    for (unsigned i = 0; i < r->header_count; ++i) if (!strcmp(name, r->headers[i].name)) {
        if (strlen(r->headers[i].value) >= size) return ESP_ERR_NO_MEM;
        strcpy(out, r->headers[i].value); return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { r->status = atoi(status); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { snprintf(r->type, sizeof(r->type), "%s", type); return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value)
{
    assert(r->response_header_count < 12);
    unsigned i = r->response_header_count++;
    snprintf(r->response_headers[i].name, sizeof(r->response_headers[i].name), "%s", name);
    snprintf(r->response_headers[i].value, sizeof(r->response_headers[i].value), "%s", value);
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *body, ssize_t size)
{
    assert(size >= 0 && (size_t)size < sizeof(r->response));
    memcpy(r->response, body, (size_t)size); r->response[(size_t)size] = 0;
    r->response_length = (size_t)size; return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *body) { return httpd_resp_send(r, body, (ssize_t)strlen(body)); }
int httpd_req_recv(httpd_req_t *r, char *out, size_t size)
{
    size_t length = r->content_len - r->received;
    if (length > size) length = size;
    memcpy(out, r->body + r->received, length); r->received += length; return (int)length;
}
static uint64_t now_ms(void) { return s_now_ms; }
static void clear(void *memory, size_t size) { volatile unsigned char *p = memory; while (size--) *p++ = 0; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { (void)m; (void)t; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t m) { (void)m; }
static void drain_channel_closures(server_t *s) { (void)s; }
static void cache_sessions(server_t *s) { (void)s; }
static bool recording_content_path(const char *path, void *id) { (void)path; (void)id; return false; }
static esp_err_t begin_recording_download(httpd_req_t *r, server_t *s, const cp_http_security_t *security)
{ (void)r; (void)s; (void)security; abort(); }
/* Extracted verbatim by run.py; production header parsing and body/CSRF route. */
#include "web_http_adapter.inc"

esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *out)
{ assert(!strcmp(partition, "control") && !strcmp(ns, "ctrl_access")); (void)mode; *out = 1; return ESP_OK; }
static unsigned web_bank(const char *key) { assert(!strcmp(key, "access_a") || !strcmp(key, "access_b")); return !strcmp(key, "access_b") ? 1U : 0U; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 1); unsigned i = web_bank(key);
    if (!web_bank_sizes[i]) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= web_bank_sizes[i]); *size = web_bank_sizes[i]; memcpy(out, web_banks[i], *size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{ assert(handle == 1 && size <= 1024); unsigned i = web_bank(key); memcpy(web_banks[i], value, size); web_bank_sizes[i] = size; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
bool app_clock_wall_time(uint64_t *out) { *out = 1793808000U + s_now_ms / 1000U; return true; }
void app_control_trust_snapshot(app_control_trust_snapshot_t *out)
{ *out = (app_control_trust_snapshot_t){.initialized = true, .known = true, .present = true}; memset(out->active_digest, 'a', 64); }
size_t app_control_trust_principals(cp_principal_t out[CP_MAX_PRINCIPALS], char digest[65])
{ (void)out; memset(digest, 'a', 64); digest[64] = 0; return 0; }
bool app_control_trust_update_policy(const uint8_t id[16], app_update_policy_t *out)
{ (void)id; (void)out; return false; }
void app_control_server_snapshot(app_control_server_snapshot_t *out)
{ *out = (app_control_server_snapshot_t){.state = APP_CONTROL_READY, .tls = true}; snprintf(out->address, sizeof(out->address), "%s", web_origin); }
bool app_control_server_client_connected(const uint8_t principal[16])
{
    cp_session_view_t sessions[CP_MAX_SESSIONS];
    size_t count = cp_sessions_snapshot(web_server.protocol, sessions);
    for (size_t i = 0; i < count; ++i) if (sessions[i].authenticated && !memcmp(principal, sessions[i].identity.principal_id, 16)) return true;
    return false;
}

static bool web_random_source(void *user, uint8_t *out, size_t length)
{ (void)user; for (size_t i = 0; i < length; ++i) out[i] = (uint8_t)(++web_random * 17U + 1U); return true; }
static bool web_principal(void *user, const uint8_t id[16], cp_principal_t *out)
{ (void)user; return app_control_access_principal(id, out); }
static bool web_input_authorized(void *user, const cp_identity_t *who)
{ (void)user; return app_control_access_input(who); }
static cp_error_t web_pairing(void *user, bool status, const char *client, const char *proof,
                              const char *label, char *json, size_t capacity, size_t *length)
{
    (void)user;
    if (status) return app_control_access_pair_status(client, proof, json, capacity, length);
    uint64_t request;
    cp_error_t error = app_control_access_pair(client, proof, label, &request);
    if (error != CP_OK) return error;
    int written = snprintf(json, capacity, "{\"state\":\"pending\",\"request_id\":\"%llu\",\"fingerprint\":\"%s\"}", (unsigned long long)request, proof);
    assert(written > 0 && (size_t)written < capacity); *length = (size_t)written; return CP_OK;
}
static void web_service(void)
{
    s_now_ms += 10;
    if (web_cancel_after && trace_count >= web_cancel_after) {
        web_cancel_after = 0; remote_local_override(false); cp_physical_override(web_server.protocol, false, s_now_ms);
    }
    cp_tick(web_server.protocol, s_now_ms);
    remote_control_service();
    (void)hid_engine_step(&s_engine, send_report, &test_link.generation);
    sync_interaction(); remote_control_service();
    /* Match the production server worker: publish current owner/releases
     * before delivering its completion mailbox. */
    cp_set_device_state(web_server.protocol, &test_control_state, s_now_ms);
    if (test_control_completions != web_completed) {
        web_completed = test_control_completions;
        (void)cp_complete(web_server.protocol, test_control_result.command_key, &test_control_result, s_now_ms);
    }
}
static void web_fixture(httpd_req_t *r)
{
    if (!strcmp(r->uri, "/_fixture/approve")) {
        app_control_access_snapshot_t view; app_control_access_snapshot(&view); assert(view.pending);
        app_control_access_request_t request = {.action = APP_CONTROL_ACCESS_APPROVE,
            .expected_revision = view.revision, .pairing_request = view.pairing_request,
            .permissions = APP_CONTROL_ACCESS_INPUT, .duration = APP_CONTROL_ACCESS_SIX_HOURS};
        memcpy(request.client_id, view.proposal.id, 16); uint64_t receipt;
        assert(app_control_access_submit(&request, &receipt) == CP_OK);
        app_control_access_service(true); app_control_access_snapshot(&view);
        assert(view.receipt_request == receipt && view.receipt_state == CP_RESULT_APPLIED);
    } else if (!strcmp(r->uri, "/_fixture/takeover")) {
        remote_local_override(false); cp_physical_override(web_server.protocol, false, s_now_ms);
    } else if (!strcmp(r->uri, "/_fixture/allow")) {
        assert(set_remote_allowance(true));
    } else if (!strcmp(r->uri, "/_fixture/partial")) {
        web_cancel_after = trace_count + 3U;
    } else if (!strcmp(r->uri, "/_fixture/target")) {
        input_link_t previous = input_link_snapshot(&test_link);
        ++test_link.generation;
        input_link_t next = input_link_snapshot(&test_link);
        observe_input_link(&next, &previous);
    } else if (!strcmp(r->uri, "/_fixture/reset-trace")) trace_count = 0;
    else assert(!strcmp(r->uri, "/_fixture/reports"));
    size_t used = (size_t)snprintf(r->response, sizeof(r->response), "{\"reports\":[");
    for (size_t i = 0; i < trace_count; ++i) {
        used += (size_t)snprintf(r->response + used, sizeof(r->response) - used, "%s{\"id\":%u,\"bytes\":[", i ? "," : "", (unsigned)trace[i].id);
        for (size_t j = 0; j < trace[i].length; ++j) used += (size_t)snprintf(r->response + used, sizeof(r->response) - used, "%s%u", j ? "," : "", (unsigned)trace[i].bytes[j]);
        used += (size_t)snprintf(r->response + used, sizeof(r->response) - used, "]}");
    }
    used += (size_t)snprintf(r->response + used, sizeof(r->response) - used, "]}");
    r->response_length = used; strcpy(r->type, "application/json");
}
static void web_reply(httpd_req_t *r)
{
    char *escaped = malloc(sizeof(r->response) * 6U + 3U); assert(escaped);
    assert(cj_escape(r->response, escaped, sizeof(r->response) * 6U + 3U));
    printf("{\"status\":%d,\"body\":%s,\"headers\":{\"Content-Type\":\"%s\"", r->status, escaped, r->type[0] ? r->type : "application/json");
    for (unsigned i = 0; i < r->response_header_count; ++i) {
        assert(cj_escape(r->response_headers[i].value, escaped, sizeof(r->response) * 6U + 3U));
        printf(",\"%s\":%s", r->response_headers[i].name, escaped);
    }
    puts("}}"); fflush(stdout); free(escaped);
}
int main(int argc, char **argv)
{
    assert(argc == 3); snprintf(web_host, sizeof(web_host), "%s", argv[1]); snprintf(web_origin, sizeof(web_origin), "%s", argv[2]);
    setup(INPUT_MOUSE); remote_control_init(false); test_control_session_alive = true; assert(set_remote_allowance(true));
    assert(app_control_access_init() == ESP_OK);
    void *storage = calloc(1, cp_context_size()); assert(storage);
    cp_callbacks_t callbacks = {.user = s_command_bridge, .random = web_random_source,
        .available = app_command_bridge_available, .dispatch = app_command_bridge_submit,
        .safety = app_command_bridge_safety, .read = app_command_bridge_read,
        .principal = web_principal, .input_authorized = web_input_authorized, .pairing = web_pairing};
    assert(cp_initialize(storage, cp_context_size(), &callbacks, &web_server.protocol) == CP_OK);
    assert(cp_http_configure(web_server.protocol, web_host, web_origin) == CP_OK);
    web_service();
    static char input[8192]; static cj_document_t document; static httpd_req_t request;
    while (fgets(input, sizeof(input), stdin)) {
        assert(cj_parse(&document, input, strlen(input)));
        uint64_t time; assert(cj_u64(&document, cj_get(&document, 0, "now_ms"), &time));
        while (s_now_ms + 10U <= time) web_service();
        memset(&request, 0, sizeof(request)); request.status = 200; request.user_ctx = &web_server;
        request.uri = cj_string(&document, cj_get(&document, 0, "path"));
        request.body = cj_string(&document, cj_get(&document, 0, "body")); assert(request.uri && request.body);
        request.content_len = strlen(request.body);
        const char *method = cj_string(&document, cj_get(&document, 0, "method"));
        request.method = !strcmp(method, "POST") ? HTTP_POST : !strcmp(method, "DELETE") ? HTTP_DELETE : HTTP_GET;
        const char *names[] = {"Host", "Origin", "Authorization", "Cookie", "X-CSRF-Token", "Content-Type", "Content-Encoding", "Transfer-Encoding"};
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            const char *value = cj_string(&document, cj_get(&document, 0, names[i]));
            if (value) request.headers[request.header_count++] = (web_header_t){names[i], value};
        }
        if (!strncmp(request.uri, "/_fixture/", 10)) web_fixture(&request);
        else if (!strncmp(request.uri, "/api/", 5)) (void)handle_request(&request);
        else (void)app_control_web_serve(&request, web_host, web_origin);
        web_service(); web_service(); web_reply(&request);
    }
    free(storage); return 0;
}
