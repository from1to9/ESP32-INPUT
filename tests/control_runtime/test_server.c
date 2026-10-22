// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "server_test_sdk.h"
#include "control_json.h"
#include "../../firmware/main/app_control_server.h"
#include "../../firmware/main/app_remote_media_runtime_port.h"
#include "../../firmware/main/app_network_task.h"
#include "weak_heap.h"
typedef struct server {
    cp_context_t *protocol;
    SemaphoreHandle_t mutex;
    httpd_handle_t listener;
    cp_identity_t channel_closures[CP_MAX_SESSIONS];
    unsigned channel_closure_count;
    uint64_t next_channel_generation;
    bool stop_requested, download_active;
    void *media_pending[2];
    unsigned media_pending_count;
    struct {
        bool used;
        int fd;
        uint64_t generation;
        cp_identity_t identity;
    } channels[CP_MAX_SESSIONS];
    char body[CP_MAX_FRAME_BYTES + 1];
    cp_http_response_t response;
} server_t;
static server_t *s_server;
static portMUX_TYPE s_lock;
static app_control_media_port_t s_media_port;
static uint64_t clock_ms = 100;
static unsigned critical, allocations, random_seed, safety_calls, dispatch_calls;
static bool sem_available = true;
static cp_command_t dispatched;
static cp_device_state_t device;
static cp_session_view_t s_sessions[CP_MAX_SESSIONS];
static size_t s_session_count;
static uint64_t now_ms(void) { return clock_ms; }
static void clear(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}
static void safety(void *u, cp_safety_reason_t reason, const cp_identity_t *who)
{
    (void)u;
    ++safety_calls;
    if (reason == CP_SAFETY_SESSION_LOST || reason == CP_SAFETY_REVOKED) {
        for (size_t i = 0; i < s_session_count; ++i) {
            if (!who || !memcmp(who->session_id, s_sessions[i].identity.session_id, 16)) {
                s_sessions[i].authenticated = false;
            }
        }
    }
}
static void cache_sessions(server_t *s)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_session_count = cp_sessions_snapshot(s->protocol, s_sessions);
}
bool app_control_server_session_alive(const uint8_t session[16], const uint8_t principal[16])
{
    for (size_t i = 0; i < s_session_count; ++i) {
        if (s_sessions[i].authenticated && clock_ms >= s_sessions[i].last_authenticated_ms &&
            clock_ms - s_sessions[i].last_authenticated_ms < 5000 &&
            !memcmp(session, s_sessions[i].identity.session_id, 16) &&
            !memcmp(principal, s_sessions[i].identity.principal_id, 16)) {
            return true;
        }
    }
    return false;
}
static bool header(httpd_req_t *r, const char *name, char *out, size_t n, bool required)
{
    out[0] = 0;
    for (unsigned i = 0; i < r->header_count; ++i) {
        if (!strcmp(name, r->headers[i].name)) {
            size_t size = strlen(r->headers[i].value);
            if (size >= n) {
                return false;
            }
            memcpy(out, r->headers[i].value, size + 1);
            return true;
        }
    }
    return !required;
}
static esp_err_t error_response(httpd_req_t *r, const char *status, const char *body)
{
    r->status = atoi(status);
    snprintf(r->response, sizeof(r->response), "%s", body);
    return ESP_FAIL;
}
#include "../../firmware/main/app_control_server_media.inc"

static server_t server;
static httpd_req_t ws_request, *active_download;
static void (*download_task)(void *);
static void *download_argument;
static struct {
    void (*fn)(void *);
    void *arg;
} queued[8];
static unsigned queued_count;
static bool fail_task_create;
static unsigned legacy_creates, caps_creates, legacy_deletes, caps_deletes;
static bool http_owner, capture_permission = true, fail_send, reject_queue, deny_read_after_copy,
                        expire_after_copy, revoke_after_copy;
static unsigned channel_losses;
static uint64_t lost_channel;
static unsigned low_level_sends, blocking_helper_calls, capture_failures, reads, cancellations,
    delete_calls, upgrade_101;
static uint8_t binary_received[2048], file_bytes[4140];
static size_t binary_length;
static ae_record_metadata_t record;
static cp_identity_t identity;
static char authorization[80], cookie[256], csrf[33];
void test_enter(void) { ++critical; }
void test_leave(void)
{
    assert(critical);
    --critical;
}
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
void *heap_caps_calloc(size_t count, size_t n, unsigned caps)
{
    void *p = test_heap_calloc(4, count, n, caps);
    if (p) {
        ++allocations;
    }
    return p;
}
void *heap_caps_aligned_calloc(size_t alignment, size_t count, size_t n, unsigned caps)
{
    void *p = test_heap_calloc(alignment, count, n, caps);
    if (p) {
        ++allocations;
    }
    return p;
}
void heap_caps_free(void *p)
{
    assert(p && allocations);
    --allocations;
    test_heap_free(p);
}
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)semaphore;
    (void)ticks;
    return sem_available ? pdTRUE : 0;
}
void xSemaphoreGive(SemaphoreHandle_t semaphore) { (void)semaphore; }
void vTaskDelay(TickType_t ticks)
{
    assert(!critical);
    clock_ms += ticks;
}
void vTaskDelete(void *task)
{
    assert(!APP_NETWORK_EXTERNAL_STACK && !task && !critical);
    ++delete_calls;
    ++legacy_deletes;
}
void vTaskDeleteWithCaps(void *task)
{
    assert(APP_NETWORK_EXTERNAL_STACK && !task && !critical);
    ++delete_calls;
    ++caps_deletes;
}
static int create_download(void (*task)(void *), const char *name, unsigned stack, void *argument,
                           unsigned priority, void *out)
{
    assert(!critical && !strcmp(name, "record_download") && stack == 8192 && priority == 1 && !out);
    if (fail_task_create) {
        return 0;
    }
    download_task = task;
    download_argument = argument;
    return pdPASS;
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                unsigned priority, void *out)
{
    assert(!APP_NETWORK_EXTERNAL_STACK);
    ++legacy_creates;
    return create_download(task, name, stack, argument, priority, out);
}
int xTaskCreateWithCaps(void (*task)(void *), const char *name, unsigned stack, void *argument,
                        unsigned priority, void *out, unsigned caps)
{
    assert(APP_NETWORK_EXTERNAL_STACK && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++caps_creates;
    return create_download(task, name, stack, argument, priority, out);
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    r->status = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    (void)r;
    assert(!strcmp(type, "audio/wav"));
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value)
{
    if (!strcmp(name, "Content-Range")) {
        snprintf(r->content_range, sizeof(r->content_range), "%s", value);
    }
    if (!strcmp(name, "ETag")) {
        snprintf(r->etag, sizeof(r->etag), "%s", value);
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *bytes, ssize_t n)
{
    assert(!critical);
    if (fail_send) {
        return ESP_FAIL;
    }
    if (!bytes) {
        assert(n == 0);
        r->terminated = true;
        return ESP_OK;
    }
    assert(n > 0 && r->received + (size_t)n <= sizeof(r->response));
    memcpy(r->response + r->received, bytes, (size_t)n);
    r->received += (size_t)n;
    return ESP_OK;
}
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out)
{
    assert(!critical);
    *out = r;
    active_download = r;
    return ESP_OK;
}
esp_err_t httpd_req_async_handler_complete(httpd_req_t *r)
{
    assert(!critical);
    r->completed = true;
    return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *r) { return r->fd; }
esp_err_t httpd_sess_trigger_close(httpd_handle_t h, int fd)
{
    assert(h == server.listener && active_download && fd == active_download->fd);
    active_download->closed = true;
    return ESP_OK;
}
void *httpd_sess_get_ctx(httpd_handle_t h, int fd)
{
    assert(h == server.listener);
    return fd == ws_request.fd ? ws_request.sess_ctx : NULL;
}
esp_err_t httpd_ws_recv_frame(httpd_req_t *r, httpd_ws_frame_t *frame, size_t n)
{
    (void)r;
    (void)frame;
    (void)n;
    return ESP_FAIL;
}
esp_err_t httpd_ws_send_frame(httpd_req_t *r, httpd_ws_frame_t *frame)
{
    (void)r;
    (void)frame;
    return ESP_OK;
}
esp_err_t httpd_ws_send_data(httpd_handle_t h, int fd, httpd_ws_frame_t *frame)
{
    (void)h;
    (void)fd;
    (void)frame;
    ++blocking_helper_calls;
    assert(!"HTTP-owner send_data would self-queue and wait forever");
    return ESP_FAIL;
}
esp_err_t httpd_ws_send_frame_async(httpd_handle_t h, int fd, httpd_ws_frame_t *frame)
{
    assert(http_owner && !critical && h == server.listener && fd == ws_request.fd &&
           frame->type == HTTPD_WS_TYPE_BINARY && frame->len <= sizeof(binary_received));
    ++low_level_sends;
    if (fail_send) {
        return ESP_FAIL;
    }
    memcpy(binary_received, frame->payload, frame->len);
    binary_length = frame->len;
    return ESP_OK;
}
esp_err_t httpd_queue_work(httpd_handle_t h, void (*fn)(void *), void *arg)
{
    assert(!critical && h == server.listener && queued_count < 8);
    if (reject_queue) {
        return ESP_FAIL;
    }
    queued[queued_count].fn = fn;
    queued[queued_count++].arg = arg;
    return ESP_OK;
}
static bool capture_allowed(void *u, const cp_identity_t *who, const uint8_t job[16],
                            uint64_t generation, uint64_t channel_generation)
{
    assert(channel_generation);
    (void)u;
    assert(!critical && !memcmp(who->principal_id, identity.principal_id, 16) && job[0] == 3 &&
           generation == 8);
    return capture_permission;
}
static void capture_failed(void *u, const cp_identity_t *who, const uint8_t job[16],
                           uint64_t generation, uint64_t channel_generation, cp_error_t error)
{
    assert(channel_generation);
    (void)u;
    (void)who;
    assert(job[0] == 3 && generation == 8 && error != CP_OK);
    ++capture_failures;
}
static void channel_lost(void *u, const cp_identity_t *who, uint64_t generation)
{
    (void)u;
    assert(!critical && generation && !memcmp(who->session_id, identity.session_id, 16));
    ++channel_losses;
    lost_channel = generation;
}
static cp_error_t playback(void *u, const cp_identity_t *who, uint64_t channel_generation,
                           const uint8_t *bytes, size_t length)
{
    assert(channel_generation);
    (void)u;
    (void)who;
    (void)bytes;
    (void)length;
    return CP_BUSY;
}
ae_result_t app_media_runtime_record_metadata(const cp_identity_t *who, const ae_id_t *object,
                                              ae_record_metadata_t *out)
{
    assert(!critical && !memcmp(who->principal_id, identity.principal_id, 16) &&
           object->bytes[0] == 0x33);
    *out = record;
    return AE_OK;
}
ae_result_t app_media_runtime_content_read(const cp_identity_t *who, const ae_id_t *object,
                                           uint64_t revision, uint64_t offset, void *out, size_t n,
                                           size_t *actual)
{
    assert(!critical && !memcmp(who->principal_id, identity.principal_id, 16) &&
           object->bytes[0] == 0x33 && revision == record.revision &&
           offset + n <= sizeof(file_bytes));
    ++reads;
    memcpy(out, file_bytes + offset, n);
    *actual = n;
    if (revoke_after_copy) {
        assert(cp_revoke_principal(server.protocol, who->principal_id, clock_ms) == CP_OK);
        cache_sessions(&server);
    }
    if (expire_after_copy) {
        clock_ms += 90001;
    }
    return deny_read_after_copy ? AE_DENIED : AE_OK;
}
void app_media_runtime_content_cancel(const cp_identity_t *who, const ae_id_t *object)
{
    assert(!critical && who && object);
    ++cancellations;
}
static bool random_bytes(void *u, uint8_t *out, size_t n)
{
    (void)u;
    for (size_t i = 0; i < n; ++i) {
        out[i] = (uint8_t)(++random_seed);
    }
    return true;
}
static void field(const cp_http_response_t *response, const char *key, char *out, size_t capacity)
{
    cj_document_t *d = malloc(sizeof(*d));
    assert(cj_parse(d, response->body, response->length));
    const char *value = cj_string(d, cj_get(d, 0, key));
    assert(value && strlen(value) < capacity);
    strcpy(out, value);
    free(d);
}
static bool operation_available(void *u, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{
    (void)u;
    (void)op;
    reason[0] = 0;
    return true;
}
static cp_error_t dispatch_command(void *u, const cp_command_t *command, cp_result_t *result)
{
    (void)u;
    dispatched = *command;
    ++dispatch_calls;
    *result = (cp_result_t){.state = CP_RESULT_ACCEPTED, .uncertain_index = -1};
    return CP_OK;
}
static void setup_server(bool browser)
{
    if (ws_request.sess_ctx) {
        websocket_free(ws_request.sess_ctx);
        ws_request.sess_ctx = NULL;
    }
    if (server.protocol) {
        free(server.protocol);
    }
    assert(!allocations && !queued_count);
    memset(&server, 0, sizeof(server));
    memset(&ws_request, 0, sizeof(ws_request));
    memset(&identity, 0, sizeof(identity));
    server.listener = (void *)1;
    server.mutex = (void *)1;
    s_server = &server;
    clock_ms = 100;
    random_seed = 0;
    reads = cancellations = low_level_sends = blocking_helper_calls = capture_failures =
        upgrade_101 = 0;
    channel_losses = 0;
    lost_channel = 0;
    reject_queue = false;
    fail_task_create = false;
    download_task = NULL;
    download_argument = NULL;
    legacy_creates = caps_creates = legacy_deletes = caps_deletes = delete_calls = 0;
    sem_available = true;
    safety_calls = dispatch_calls = 0;
    capture_permission = true;
    fail_send = deny_read_after_copy = expire_after_copy = revoke_after_copy = false;
    void *memory = calloc(1, cp_context_size());
    cp_callbacks_t callbacks = {.random = random_bytes,
                                .available = operation_available,
                                .dispatch = dispatch_command,
                                .safety = safety};
    assert(cp_initialize(memory, cp_context_size(), &callbacks, &server.protocol) == CP_OK &&
           cp_http_configure(server.protocol, "device.test", "https://device.test") == CP_OK);
    device = (cp_device_state_t){.management_generation = 1,
                                 .input_epoch = 1,
                                 .transport_generation = 17,
                                 .ui_revision = 1,
                                 .state_sequence = 1,
                                 .allowance_generation = 1,
                                 .input_ready = true,
                                 .remote_allowed = true};
    strcpy(device.target_id, "test");
    strcpy(device.mode, "mouse");
    cp_set_device_state(server.protocol, &device, clock_ms);
    uint8_t secret[32];
    memset(secret, 0x22, 32);
    cp_principal_t principal = {.scopes = CP_SCOPE_ALL, .enabled = true, .native_only = !browser};
    memset(principal.id, 0x11, 16);
    cp_sha256(secret, 32, principal.credential_sha256);
    assert(cp_load_principal(server.protocol, &principal) == CP_OK);
    const char *login = "{\"client_id\":\"11111111111111111111111111111111\",\"credential\":"
                        "\"2222222222222222222222222222222222222222222222222222222222222222\"}";
    cp_http_security_t security = {.tls = true, .host = "device.test"};
    cp_http_response_t response;
    if (browser) {
        security.origin = "https://device.test";
        assert(cp_http(server.protocol, "GET", "/api/v1/login-challenge", &security, NULL, 0,
                       clock_ms, &response) == CP_OK);
        field(&response, "csrf", csrf, sizeof(csrf));
        strcpy(cookie, response.set_cookie);
        security.cookie = cookie;
        security.csrf = csrf;
    }
    assert(cp_http(server.protocol, "POST", "/api/v1/sessions", &security, login, strlen(login),
                   clock_ms, &response) == CP_OK);
    if (browser) {
        strcpy(cookie, response.set_cookie);
        field(&response, "csrf", csrf, sizeof(csrf));
        security.cookie = cookie;
    } else {
        char token[65];
        field(&response, "session_token", token, sizeof(token));
        snprintf(authorization, sizeof(authorization), "Bearer %s", token);
        security.authorization = authorization;
    }
    assert(cp_transport_authenticate(server.protocol, &security, true, clock_ms, &identity) ==
           CP_OK);
    cp_set_transport_capabilities(server.protocol, true, true);
    cache_sessions(&server);
    for (unsigned i = 0; i < sizeof(file_bytes); ++i) {
        file_bytes[i] = (uint8_t)i;
    }
    record = (ae_record_metadata_t){.revision = 7,
                                    .state = AE_RECORD_COMPLETED,
                                    .export_policy = AE_EXPORT_OWNER_DOWNLOAD,
                                    .pcm_bytes = sizeof(file_bytes) - AE_WAV_HEADER_BYTES,
                                    .integrity_verified = true,
                                    .hash_available = true,
                                    .sync_acknowledged = true};
    memcpy(record.principal_id.bytes, identity.principal_id, 16);
    cp_sha256(file_bytes, sizeof(file_bytes), record.sha256);
    ws_request.user_ctx = &server;
    ws_request.fd = 3;
    ws_request.headers[ws_request.header_count++] = (test_header_t){"Host", "device.test"};
    if (browser) {
        ws_request.headers[ws_request.header_count++] =
            (test_header_t){"Origin", "https://device.test"};
        ws_request.headers[ws_request.header_count++] = (test_header_t){"Cookie", cookie};
    } else {
        ws_request.headers[ws_request.header_count++] =
            (test_header_t){"Authorization", authorization};
    }
    app_control_media_port_t port = {.capture_allowed = capture_allowed,
                                     .capture_failed = capture_failed,
                                     .playback = playback,
                                     .channel_lost = channel_lost};
    app_control_server_set_media_port(&port);
}
static void handshake(void)
{
    esp_err_t result = websocket_authenticate(&ws_request);
    if (result == ESP_OK) {
        ++upgrade_101;
    }
}
static void drain(void)
{
    assert(queued_count);
    void (*fn)(void *) = queued[0].fn;
    void *arg = queued[0].arg;
    memmove(queued, queued + 1, (queued_count - 1) * sizeof(queued[0]));
    --queued_count;
    http_owner = true;
    fn(arg);
    http_owner = false;
}
static cp_http_security_t native_security(void)
{
    return (cp_http_security_t){.tls = true, .host = "device.test", .authorization = authorization};
}
static void make_etag(char out[100])
{
    char hash[65];
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(hash + 2 * i, 3, "%02x", record.sha256[i]);
    }
    snprintf(out, 100, "\"7-%s\"", hash);
}
static void download(httpd_req_t *request, const char *range, const char *etag)
{
    memset(request, 0, sizeof(*request));
    request->fd = 4;
    request->uri = "/api/v1/recordings/33333333333333333333333333333333/content";
    request->user_ctx = &server;
    if (range) {
        request->headers[request->header_count++] = (test_header_t){"Range", range};
    }
    if (etag) {
        request->headers[request->header_count++] = (test_header_t){"If-Match", etag};
    }
    cp_http_security_t security = native_security();
    assert(begin_recording_download(request, &server, &security) == ESP_OK &&
           server.download_active);
    assert(download_task);
    download_task(download_argument);
    assert(!server.download_active && request->completed);
}
static void test_download_stack_allocation_failure(void)
{
    setup_server(false);
    httpd_req_t request = {.fd = 4,
                           .uri = "/api/v1/recordings/33333333333333333333333333333333/content",
                           .user_ctx = &server};
    cp_http_security_t security = native_security();
    test_heap_fail_next = 1;
    assert(begin_recording_download(&request, &server, &security) == ESP_FAIL);
    assert(request.status == 503 && !server.download_active && !download_task &&
           !allocations && !test_heap_live);
    fail_task_create = true;
    assert(begin_recording_download(&request, &server, &security) == ESP_FAIL);
    assert(request.completed && request.status == 503 && !server.download_active &&
           !download_task && !allocations);
    assert((APP_NETWORK_EXTERNAL_STACK ? caps_creates : legacy_creates) == 1 &&
           (APP_NETWORK_EXTERNAL_STACK ? legacy_creates : caps_creates) == 0 && !delete_calls);
    fail_task_create = false;
    download(&request, NULL, NULL);
    assert(request.terminated && delete_calls == 1 && !server.download_active && !allocations);
    assert((APP_NETWORK_EXTERNAL_STACK ? caps_deletes : legacy_deletes) == 1 &&
           (APP_NETWORK_EXTERNAL_STACK ? legacy_deletes : caps_deletes) == 0);
}
static void test_auth_before_upgrade(void)
{
    setup_server(false);
    ws_request.headers[1].value = "Bearer invalid";
    handshake();
    assert(upgrade_101 == 0 && ws_request.status == 401 && !ws_request.sess_ctx);
    ws_request.headers[1].value = authorization;
    handshake();
    assert(upgrade_101 == 1 && ws_request.sess_ctx);
    setup_server(true);
    handshake();
    assert(!upgrade_101 && !ws_request.sess_ctx);
    cp_http_security_t security = {
        .tls = true, .host = "device.test", .origin = "https://device.test", .cookie = cookie};
    cp_http_response_t response;
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0, clock_ms,
                   &response) == CP_UNAUTHENTICATED);
    security.csrf = csrf;
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0, clock_ms,
                   &response) == CP_OK);
    char ticket[65], protocol[100];
    field(&response, "ticket", ticket, sizeof(ticket));
    snprintf(protocol, sizeof(protocol), "esp32-control-v1, ticket.%s", ticket);
    ws_request.headers[ws_request.header_count++] =
        (test_header_t){"Sec-WebSocket-Protocol", protocol};
    clock_ms += 10000;
    handshake();
    assert(!upgrade_101 && !ws_request.sess_ctx); /* Exact expiry, before 101. */
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0, clock_ms,
                   &response) == CP_OK);
    field(&response, "ticket", ticket, sizeof(ticket));
    snprintf(protocol, sizeof(protocol), "esp32-control-v1, ticket.%s", ticket);
    handshake();
    assert(upgrade_101 == 1 && ws_request.sess_ctx);
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    handshake();
    assert(upgrade_101 == 1 && !ws_request.sess_ctx); /* Consumed ticket cannot replay. */
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0, clock_ms,
                   &response) == CP_OK);
    field(&response, "ticket", ticket, sizeof(ticket));
    snprintf(protocol, sizeof(protocol), "esp32-control-v1, ticket.%s", ticket);
    assert(cp_revoke_principal(server.protocol, identity.principal_id, clock_ms) == CP_OK);
    handshake();
    assert(upgrade_101 == 1 && !ws_request.sess_ctx);
}
typedef struct {
    cp_identity_t identity;
    char auth[80], prefix[17], sid[33], boot[33];
    unsigned sequence;
} test_client_t;
static test_client_t login_client(void)
{
    const char *login = "{\"client_id\":\"11111111111111111111111111111111\",\"credential\":"
                        "\"2222222222222222222222222222222222222222222222222222222222222222\"}";
    cp_http_security_t security = {.tls = true, .host = "device.test"};
    cp_http_response_t response;
    test_client_t client = {0};
    assert(cp_http(server.protocol, "POST", "/api/v1/sessions", &security, login, strlen(login),
                   clock_ms, &response) == CP_OK);
    char token[65];
    field(&response, "session_token", token, sizeof(token));
    snprintf(client.auth, sizeof(client.auth), "Bearer %s", token);
    field(&response, "request_prefix", client.prefix, sizeof(client.prefix));
    field(&response, "session_id", client.sid, sizeof(client.sid));
    field(&response, "boot_id", client.boot, sizeof(client.boot));
    security.authorization = client.auth;
    assert(cp_transport_authenticate(server.protocol, &security, true, clock_ms,
                                     &client.identity) == CP_OK);
    cache_sessions(&server);
    return client;
}
static cp_error_t client_command(test_client_t *client, const char *op, const char *args,
                                 const char *lease)
{
    cp_http_security_t security = {
        .tls = true, .host = "device.test", .authorization = client->auth};
    cp_http_response_t response;
    assert(cp_http(server.protocol, "GET", "/api/v1/state", &security, NULL, 0, clock_ms,
                   &response) == CP_OK);
    char body[1500];
    ++client->sequence;
    char context_fields[256];
    const cp_operation_info_t *operation = cp_operation_find(op);
    if (operation && operation->input) {
        snprintf(context_fields, sizeof(context_fields),
                 "\"epoch\":\"%llu\",\"transport_generation\":\"17\",\"target_id\":\"test\",\"ui_"
                 "revision\":\"1\",",
                 (unsigned long long)device.input_epoch);
    } else {
        strcpy(context_fields, "\"management_generation\":\"1\",\"resource_revision\":\"0\",");
    }
    int n = snprintf(
        body, sizeof(body),
        "{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016llx\",\"seq\":%u,"
        "\"deadline_ms\":\"%llu\",%s\"op\":\"%s\",\"args\":%s%s%s%s}",
        client->boot, client->sid, client->prefix, (unsigned long long)client->sequence,
        client->sequence, (unsigned long long)(clock_ms + 100), context_fields, op, args,
        lease ? ",\"lease_id\":\"" : "", lease ? lease : "", lease ? "\"" : "");
    assert(n > 0 && (size_t)n < sizeof(body));
    return cp_http(server.protocol, "POST", "/api/v1/commands", &security, body, (size_t)n,
                   clock_ms, &response);
}
static void apply_acquire(test_client_t *owner, char lease[33])
{
    assert(client_command(owner, "control.acquire", "{\"state_sequence\":\"1\"}", NULL) == CP_OK);
    for (unsigned i = 0; i < 16; ++i) {
        snprintf(lease + 2 * i, 3, "%02x", dispatched.lease_id[i]);
    }
    device.remote_owner = true;
    memcpy(device.owner_session, owner->identity.session_id, 16);
    ++device.input_epoch;
    cp_set_device_state(server.protocol, &device, clock_ms);
    cp_result_t applied = {.state = CP_RESULT_APPLIED, .uncertain_index = -1};
    assert(cp_complete(server.protocol, dispatched.command_key, &applied, clock_ms) == CP_OK);
}
static void test_channel_loss_revokes_exact_logical_owner(void)
{
    setup_server(false);
    cp_http_security_t setup_security = native_security();
    assert(cp_http(server.protocol, "DELETE", "/api/v1/sessions/current", &setup_security, NULL, 0,
                   clock_ms, &server.response) == CP_OK);
    test_client_t owner = login_client(), independent = login_client();
    strcpy(authorization, owner.auth);
    identity = owner.identity;
    handshake();
    assert(upgrade_101 == 1);
    char lease[33];
    apply_acquire(&owner, lease);
    assert(client_command(&owner, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease) == CP_OK);
    uint64_t input_key = dispatched.command_key;
    assert(client_command(&owner, "wifi.scan.start", "{}", NULL) == CP_OK);
    uint64_t management_key = dispatched.command_key;
    assert(client_command(&owner, "audio.capture.start",
                          "{\"source\":\"local_microphone\",\"sample_profile\":\"pcm16_mono_16k\","
                          "\"consent_id\":\"11111111111111111111111111111111\",\"duration_"
                          "seconds\":10,\"byte_limit\":10000}",
                          NULL) == CP_OK);
    uint64_t capture_key = dispatched.command_key;
    channel_t original = *(channel_t *)ws_request.sess_ctx;
    httpd_ws_frame_t close_frame = {.type = HTTPD_WS_TYPE_CLOSE};
    assert(websocket_control(&ws_request, &close_frame) == ESP_OK);
    assert(safety_calls &&
           cp_transport_heartbeat(server.protocol, &owner.identity, clock_ms) == CP_OK);
    cp_result_t done = {.state = CP_RESULT_COMPLETED, .uncertain_index = -1};
    assert(cp_complete(server.protocol, input_key, &done, clock_ms) == CP_STALE);
    assert(cp_complete(server.protocol, management_key, &done, clock_ms) == CP_OK);
    assert(cp_complete(server.protocol, capture_key, &done, clock_ms) == CP_OK);
    assert(cp_transport_heartbeat(server.protocol, &independent.identity, clock_ms) == CP_OK);
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    handshake();
    assert(upgrade_101 == 2); /* Same session, fd and slot. */
    unsigned before = dispatch_calls;
    assert(client_command(&owner, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease) ==
               CP_FORBIDDEN &&
           dispatch_calls == before);
    apply_acquire(&owner, lease);
    channel_t *late = heap_caps_aligned_calloc(_Alignof(channel_t), 1, sizeof(*late), MALLOC_CAP_8BIT);
    *late = original;
    websocket_free(late);
    assert(server.channels[0].used &&
           client_command(&owner, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease) == CP_OK);
    /* Pressure queues only scoped loss; HTTP authentication remains alive.
     * Reconnect drains the previous generation before a new lease admission. */
    sem_available = false;
    late = heap_caps_aligned_calloc(_Alignof(channel_t), 1, sizeof(*late), MALLOC_CAP_8BIT);
    *late = original;
    websocket_free(late);
    assert(!server.channel_closure_count);
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    assert(
        server.channel_closure_count == 1 &&
        app_control_server_session_alive(owner.identity.session_id, owner.identity.principal_id));
    sem_available = true;
    handshake();
    assert(upgrade_101 == 3 && !server.channel_closure_count);
    assert(client_command(&owner, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease) ==
           CP_FORBIDDEN);
    apply_acquire(&owner, lease);
    assert(client_command(&owner, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease) == CP_OK);
    cp_http_security_t security = native_security();
    assert(cp_http(server.protocol, "DELETE", "/api/v1/sessions/current", &security, NULL, 0,
                   clock_ms, &server.response) == CP_OK);
    assert(cp_transport_heartbeat(server.protocol, &owner.identity, clock_ms) ==
           CP_UNAUTHENTICATED);
    assert(cp_transport_heartbeat(server.protocol, &independent.identity, clock_ms) == CP_OK);
}
static void test_capture_queue_and_owner_thread_send(void)
{
    setup_server(false);
    test_heap_fail_next = 1;
    handshake();
    assert(!upgrade_101 && !ws_request.sess_ctx && !allocations && !test_heap_live);
    handshake();
    assert(upgrade_101 == 1);
    assert(websocket_data(&ws_request) == ESP_FAIL);
    cp_media_header_t h = {.kind = CP_MEDIA_CAPTURE,
                           .audio_generation = 2,
                           .consumer_generation = 8,
                           .sequence = 1,
                           .sample_rate_hz = 16000,
                           .channels = 1,
                           .frame_ms = 20,
                           .bytes = 640};
    memset(h.job_id, 3, 16);
    uint8_t pcm[640], packet[CP_MEDIA_FRAME_BYTES];
    memset(pcm, 0x5a, sizeof(pcm));
    size_t length;
    assert(cp_media_encode(&h, pcm, packet, sizeof(packet), &length));
    test_heap_fail_next = 1;
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) == ESP_ERR_NO_MEM &&
           !server.media_pending_count && !queued_count && allocations == 1 && test_heap_live == 1);
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) == ESP_OK);
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) == ESP_OK);
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) != ESP_OK &&
           server.media_pending_count == 2 && queued_count == 2 && !low_level_sends);
    memset(packet, 0, sizeof(packet));
    drain();
    assert(low_level_sends == 1 && !blocking_helper_calls && binary_length == 704 &&
           binary_received[64] == 0x5a && server.media_pending_count == 1);
    capture_permission = false;
    drain();
    assert(low_level_sends == 1 && capture_failures == 1 && !server.media_pending_count);
    capture_permission = true;
    assert(cp_media_encode(&h, pcm, packet, sizeof(packet), &length));
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) == ESP_OK);
    clock_ms += 101;
    drain();
    assert(capture_failures == 2 && low_level_sends == 1);
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) == ESP_OK);
    app_control_server_capture_cancel(&identity, h.job_id, 8,
                                      app_control_server_channel_generation(&identity));
    drain();
    assert(capture_failures == 3 && low_level_sends == 1);
    assert(app_control_server_capture_send(&identity, h.job_id, 8,
                                           app_control_server_channel_generation(&identity), packet,
                                           length) == ESP_OK);
    assert(cp_revoke_principal(server.protocol, identity.principal_id, clock_ms) == CP_OK);
    cache_sessions(&server);
    drain();
    assert(capture_failures == 4 && low_level_sends == 1 && !blocking_helper_calls);
}
static void test_channel_media_binding_and_send_failures(void)
{
    setup_server(false);
    handshake();
    uint64_t first = app_control_server_channel_generation(&identity);
    assert(first);
    cp_media_header_t h = {.kind = CP_MEDIA_CAPTURE,
                           .audio_generation = 2,
                           .consumer_generation = 8,
                           .sequence = 1,
                           .sample_rate_hz = 16000,
                           .channels = 1,
                           .frame_ms = 20,
                           .bytes = 640};
    memset(h.job_id, 3, 16);
    uint8_t pcm[640] = {0}, packet[CP_MEDIA_FRAME_BYTES];
    size_t n;
    assert(cp_media_encode(&h, pcm, packet, sizeof(packet), &n));
    assert(app_control_server_capture_send(&identity, h.job_id, 8, first, packet, n) == ESP_OK);
    channel_t *old = heap_caps_aligned_calloc(_Alignof(channel_t), 1, sizeof(*old), MALLOC_CAP_8BIT);
    *old = *(channel_t *)ws_request.sess_ctx;
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    assert(channel_losses == 1 && lost_channel == first &&
           !app_control_server_channel_generation(&identity));
    handshake();
    uint64_t second = app_control_server_channel_generation(&identity);
    assert(second > first);
    drain();
    assert(!low_level_sends && capture_failures == 1); /* Never reaches replacement. */
    websocket_free(old);
    assert(channel_losses == 1 && app_control_server_channel_generation(&identity) == second);
    assert(app_control_server_capture_send(&identity, h.job_id, 8, first, packet, n) ==
           ESP_ERR_INVALID_STATE);
    reject_queue = true;
    assert(app_control_server_capture_send(&identity, h.job_id, 8, second, packet, n) ==
               ESP_ERR_NO_MEM &&
           !server.media_pending_count);
    reject_queue = false;
    assert(app_control_server_capture_send(&identity, h.job_id, 8, second, packet, n) == ESP_OK);
    fail_send = true;
    drain();
    fail_send = false;
    assert(capture_failures == 2 && !server.media_pending_count);
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    assert(channel_losses == 2 && lost_channel == second); /* No queued PCM still reports loss. */
    httpd_req_t request;
    fail_send = true;
    download(&request, NULL, NULL);
    fail_send = false;
    assert(!request.received && request.closed && request.completed && !server.download_active);
}
static void test_ranges_and_async_revalidation(void)
{
    setup_server(false);
    char etag[100];
    make_etag(etag);
    httpd_req_t request;
    download(&request, "bytes=10-19", etag);
    assert(request.status == 206 && request.received == 10 &&
           !memcmp(request.response, file_bytes + 10, 10) && request.terminated &&
           !strcmp(request.content_range, "bytes 10-19/4140"));
    download(&request, "bytes=-8", etag);
    assert(request.received == 8 &&
           !memcmp(request.response, file_bytes + sizeof(file_bytes) - 8, 8));
    download(&request, "bytes=4139-", etag);
    assert(request.received == 1 && request.terminated);
    unsigned before = reads;
    download(&request, "bytes=1-2,5-6", etag);
    assert(request.status == 416 && reads == before && !request.received);
    download(&request, "bytes=18446744073709551616-", etag);
    assert(request.status == 416 && !request.received);
    download(&request, "bytes=1-2", NULL);
    assert(request.status == 412 && !request.received);
    download(&request, "bytes=1-2", "\"wrong\"");
    assert(request.status == 412 && !request.received);
    download(&request, NULL, NULL);
    assert(request.status == 200 && request.received == sizeof(file_bytes) && request.terminated &&
           !memcmp(request.response, file_bytes, sizeof(file_bytes)));
    deny_read_after_copy = true;
    download(&request, NULL, NULL);
    assert(!request.received && !request.terminated && request.closed);
    deny_read_after_copy = false;
    expire_after_copy = true;
    download(&request, NULL, NULL);
    assert(!request.received && !request.terminated && request.closed);
    expire_after_copy = false;
    revoke_after_copy = true;
    download(&request, NULL, NULL);
    assert(!request.received && !request.terminated && request.closed);
    assert(cancellations >= 10);
}
int main(void)
{
    test_download_stack_allocation_failure();
    test_auth_before_upgrade();
    test_channel_loss_revokes_exact_logical_owner();
    test_capture_queue_and_owner_thread_send();
    test_channel_media_binding_and_send_failures();
    test_ranges_and_async_revalidation();
    setup_server(false);
    if (ws_request.sess_ctx) {
        websocket_free(ws_request.sess_ctx);
    }
    free(server.protocol);
    server.protocol = NULL;
    assert(!allocations && !test_heap_live && !critical && !queued_count);
    puts("PASS: production WSS pre-upgrade auth/CSRF, HTTP-owner direct send, bounded media "
         "queue/lifetime/cancel, async binary Range/If-Match and post-read revocation/deadline");
    return 0;
}
