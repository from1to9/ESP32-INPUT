// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "server_test_sdk.h"
#include "control_json.h"
#include "control_motion_window.h"
#include "../../firmware/main/app_control_server.h"
#include "../../firmware/main/app_remote_media_runtime_port.h"
#include "../../firmware/main/app_network_task.h"
#include "weak_heap.h"
typedef struct { uint64_t key; cp_result_t result; } completion_t;
typedef struct {
    cp_identity_t identity;
    char auth[80], prefix[17], sid[33], boot[33];
    unsigned sequence;
} test_client_t;
static test_client_t browser_client;
typedef struct server {
    unsigned slot;
    cp_session_view_t sessions[CP_MAX_SESSIONS];
    size_t session_count;
    char host[129], hotspot_ip_host[32];
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
        bool queued;
        unsigned slot;
        uint64_t generation, key;
        int fd;
    } motion_work;
    struct {
        bool used;
        int fd;
        uint64_t generation;
        cp_identity_t identity;
        control_motion_window_t motion;
    } channels[CP_MAX_SESSIONS];
    char body[CP_MAX_FRAME_BYTES + 1];
    cp_http_response_t response;
} server_t;
static server_t *s_server;
static portMUX_TYPE s_lock;
static app_control_media_port_t s_media_port;
static uint64_t clock_ms = 100;
static bool offer_binary_frame;
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
                s_server->sessions[i].authenticated = false;
            }
        }
    }
}
static void cache_sessions(server_t *s)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_session_count = cp_sessions_snapshot(s->protocol, s_sessions);
    s->session_count = s_session_count; memcpy(s->sessions, s_sessions, sizeof(s_sessions));
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
#include "server_host_fixture.inc"
static int mock_shutdown(int fd, int how);
static int mock_setsockopt(int fd, int level, int name, const void *value, socklen_t length);
#define shutdown mock_shutdown
#define setsockopt mock_setsockopt
#define ESP_LOGW(tag, ...) ((void)(tag))
#include "../../firmware/main/app_control_server_media.inc"
#undef ESP_LOGW
#undef setsockopt
#undef shutdown

static server_t server;
static httpd_req_t ws_request, *active_download;
static unsigned ws_shutdowns, ws_text_sends;
static char ws_text[CP_MAX_RESPONSE_BYTES + 1];
static const char *ws_offer_text;
static bool sync_move;
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
static int mock_shutdown(int fd, int how)
{
    assert(http_owner && fd == ws_request.fd && how == SHUT_RDWR);
    ++ws_shutdowns;
    return 0;
}
static int mock_setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
    assert(fd == ws_request.fd && level == IPPROTO_TCP && name == TCP_NODELAY);
    assert(length == sizeof(int) && *(const int *)value == 1);
    return 0;
}
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
    if (offer_binary_frame) {
        assert(n == 0); /* Enrolled HTTP must reject before reading binary payload. */
        *frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_BINARY, .final = true, .len = 2};
        return ESP_OK;
    }
    if (ws_offer_text) {
        if (!n) {
            *frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_TEXT, .final = true, .len = strlen(ws_offer_text)};
        } else {
            assert(frame->len <= n);
            memcpy(frame->payload, ws_offer_text, frame->len);
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t httpd_ws_send_frame(httpd_req_t *r, httpd_ws_frame_t *frame)
{
    (void)r;
    if (frame->type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    assert(frame->len < sizeof(ws_text));
    memcpy(ws_text, frame->payload, frame->len);
    ws_text[frame->len] = 0;
    ++ws_text_sends;
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
    assert(http_owner && !critical && h == server.listener && fd == ws_request.fd);
    if (frame->type == HTTPD_WS_TYPE_TEXT)
        return fail_send ? ESP_FAIL : httpd_ws_send_frame(&ws_request, frame);
    assert(frame->type == HTTPD_WS_TYPE_BINARY && frame->len <= sizeof(binary_received));
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
    *result = (cp_result_t){.state = sync_move && command->operation == CP_OP_MOUSE_MOVE_RELATIVE ?
        CP_RESULT_SENT : CP_RESULT_ACCEPTED, .uncertain_index = -1};
    return CP_OK;
}
static void setup_server_transport(bool browser, bool plain, cp_scope_mask_t scopes)
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
    browser_client = (test_client_t){0};
    ws_shutdowns = ws_text_sends = 0;
    ws_offer_text = NULL;
    sync_move = false;
    server.listener = (void *)1;
    server.mutex = (void *)1;
    server.slot = plain ? 1U : 0U;
    s_server = &server; strcpy(server.host, "device.test");
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
    const char *origin = plain ? "http://device.test" : "https://device.test";
    assert(cp_initialize_policy(memory, cp_context_size(), &callbacks,
               plain ? CP_POLICY_ENROLLED_HTTP : CP_POLICY_ENROLLED_TLS, &server.protocol) == CP_OK &&
           cp_http_configure(server.protocol, "device.test", origin) == CP_OK);
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
    cp_principal_t principal = {.scopes = scopes, .enabled = true, .native_only = !browser};
    memset(principal.id, 0x11, 16);
    cp_sha256(secret, 32, principal.credential_sha256);
    assert(cp_load_principal(server.protocol, &principal) == CP_OK);
    const char *login = "{\"client_id\":\"11111111111111111111111111111111\",\"credential\":"
                        "\"2222222222222222222222222222222222222222222222222222222222222222\"}";
    cp_http_security_t security = {.tls = !plain, .host = "device.test"};
    cp_http_response_t response;
    if (browser) {
        security.origin = origin;
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
        field(&response, "request_prefix", browser_client.prefix, sizeof(browser_client.prefix));
        field(&response, "session_id", browser_client.sid, sizeof(browser_client.sid));
        field(&response, "boot_id", browser_client.boot, sizeof(browser_client.boot));
        security.cookie = cookie;
    } else {
        char token[65];
        field(&response, "session_token", token, sizeof(token));
        snprintf(authorization, sizeof(authorization), "Bearer %s", token);
        security.authorization = authorization;
    }
    assert(cp_transport_authenticate(server.protocol, &security, true, clock_ms, &identity) ==
           CP_OK);
    if (browser) browser_client.identity = identity;
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
            (test_header_t){"Origin", origin};
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
static void setup_server(bool browser)
{
    setup_server_transport(browser, false, CP_SCOPE_ALL);
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
static void test_enrolled_http_upgrade(void)
{
    setup_server_transport(true, true, CP_SCOPE_VIEW);
    cp_http_security_t security = {.host = "device.test", .origin = "http://device.test",
                                    .cookie = cookie, .csrf = csrf};
    cp_http_response_t response;
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0,
                   clock_ms, &response) == CP_FORBIDDEN);

    setup_server_transport(true, true, CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL);
    assert(!strncmp(cookie, "control-http=", 13) && !strstr(cookie, "Secure"));
    handshake();
    assert(upgrade_101 == 0);
    security.csrf = NULL;
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0,
                   clock_ms, &response) == CP_UNAUTHENTICATED);
    security.csrf = csrf;
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0,
                   clock_ms, &response) == CP_OK);
    char ticket[65], protocol[100];
    field(&response, "ticket", ticket, sizeof(ticket));
    snprintf(protocol, sizeof(protocol), "esp32-control-v1, ticket.%s", ticket);
    ws_request.headers[ws_request.header_count++] = (test_header_t){"Sec-WebSocket-Protocol", protocol};
    ws_request.headers[1].value = "null";
    handshake();
    ws_request.headers[1].value = "http://other.test";
    handshake();
    ws_request.headers[1].value = "http://device.test";
    ws_request.headers[0].value = "other.test";
    handshake();
    assert(upgrade_101 == 0);
    ws_request.headers[0].value = "device.test";
    handshake();
    assert(upgrade_101 == 1 && ws_request.sess_ctx);
    offer_binary_frame = true;
    assert(websocket_data(&ws_request) == ESP_FAIL);
    offer_binary_frame = false;
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    assert(!channel_losses && cp_transport_heartbeat(server.protocol, &identity, clock_ms) == CP_OK);
    handshake();
    assert(upgrade_101 == 1 && !ws_request.sess_ctx);
}
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
static size_t format_client_command(test_client_t *client, const char *op, const char *args,
                                    const char *lease, char *body, size_t capacity)
{
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
        body, capacity,
        "{\"v\":1,\"boot_id\":\"%s\",\"session_id\":\"%s\",\"request_id\":\"%s%016llx\",\"seq\":%u,"
        "\"deadline_ms\":\"%llu\",%s\"op\":\"%s\",\"args\":%s%s%s%s}",
        client->boot, client->sid, client->prefix, (unsigned long long)client->sequence,
        client->sequence, (unsigned long long)(clock_ms + 100), context_fields, op, args,
        lease ? ",\"lease_id\":\"" : "", lease ? lease : "", lease ? "\"" : "");
    assert(n > 0 && (size_t)n < capacity);
    return (size_t)n;
}
static cp_error_t client_command(test_client_t *client, const char *op, const char *args,
                                 const char *lease)
{
    cp_http_security_t security = client->identity.browser ?
        (cp_http_security_t){.host = "device.test", .origin = "http://device.test", .cookie = cookie, .csrf = csrf} :
        (cp_http_security_t){.tls = true, .host = "device.test", .authorization = client->auth};
    cp_http_response_t response;
    assert(cp_http(server.protocol, "GET", "/api/v1/state", &security, NULL, 0, clock_ms,
                   &response) == CP_OK);
    char body[1500];
    size_t n = format_client_command(client, op, args, lease, body, sizeof(body));
    return cp_http(server.protocol, "POST", "/api/v1/commands", &security, body, n,
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
static esp_err_t receive_text(const char *text)
{
    ws_offer_text = text;
    esp_err_t result = websocket_data(&ws_request);
    ws_offer_text = NULL;
    return result;
}
static void open_browser_channel(void)
{
    cp_http_security_t security = {.host = "device.test", .origin = "http://device.test", .cookie = cookie, .csrf = csrf};
    cp_http_response_t response;
    assert(cp_http(server.protocol, "POST", "/api/v1/ws-ticket", &security, NULL, 0, clock_ms, &response) == CP_OK);
    char ticket[65], protocol[100];
    field(&response, "ticket", ticket, sizeof(ticket));
    snprintf(protocol, sizeof(protocol), "esp32-control-v1, ticket.%s", ticket);
    ws_request.headers[ws_request.header_count++] = (test_header_t){"Sec-WebSocket-Protocol", protocol};
    handshake();
    --ws_request.header_count;
    assert(ws_request.sess_ctx);
}
static void test_motion_terminal_delivery(void)
{
    setup_server_transport(true, true, CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL);
    cp_set_motion_terminal_available(server.protocol, true);
    cp_set_motion_window_max(server.protocol, 1);
    cp_http_security_t security = {.host = "device.test", .origin = "http://device.test", .cookie = cookie, .csrf = csrf};
    cp_http_response_t reply;
    assert(cp_http(server.protocol, "GET", "/api/v1/capabilities", &security, NULL, 0, clock_ms, &reply) == CP_OK);
    assert(strstr(reply.body, "\"websocket_motion_window\":false") &&
           !strstr(reply.body, "websocket_motion_window_max"));
    open_browser_channel();
    assert(receive_text("{\"v\":1,\"type\":\"motion_window\"}") == ESP_OK &&
           strstr(ws_text, "\"error\":\"unavailable\""));
    assert(receive_text("{\"v\":1,\"type\":\"renew_terminal\"}") == ESP_OK &&
           !strcmp(ws_text, "{\"motion_terminal\":true,\"renew_terminal\":true}"));

    setup_server_transport(true, true, CP_SCOPE_INPUT_CONTROL);
    cp_set_motion_terminal_available(server.protocol, true);
    open_browser_channel();
    assert(receive_text("{\"v\":1,\"type\":\"motion_terminal\"}") == ESP_OK);
    assert(((channel_t *)ws_request.sess_ctx)->terminal_mode == CP_TERMINAL_DISABLED &&
           !strstr(ws_text, "\"motion_terminal\":true"));
    enum { LEGACY, SYNCHRONOUS, NORMAL, TAKEOVER, REPLACED, STOPPING, REVOKED, SEND_LOST };
    for (unsigned scenario = LEGACY; scenario <= SEND_LOST; ++scenario) {
        setup_server_transport(true, true, CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL);
        cp_set_motion_terminal_available(server.protocol, true);
        open_browser_channel();
        const unsigned slot = ((channel_t *)ws_request.sess_ctx)->slot;
        if (scenario != LEGACY) {
            assert(receive_text("{\"v\":1,\"type\":\"motion_terminal\"}") == ESP_OK);
            assert(!strcmp(ws_text, "{\"motion_terminal\":true}"));
        }
        char lease[33], body[1500];
        apply_acquire(&browser_client, lease);
        format_client_command(&browser_client, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease, body, sizeof(body));
        sync_move = scenario == SYNCHRONOUS;
        assert(receive_text(body) == ESP_OK);
        uint64_t key = dispatched.command_key;
        assert(strstr(ws_text, sync_move ? "\"state\":\"sent\"" : "\"state\":\"accepted\""));
        assert(!strstr(ws_text, "motion_terminal") && !queued_count);
        if (scenario == SYNCHRONOUS) {
            assert(!control_motion_find(&server.channels[slot].motion, key));
            continue;
        }
        if (scenario == TAKEOVER) cp_physical_override(server.protocol, false, clock_ms);
        completion_t completion = {.key = key, .result = {.state = CP_RESULT_SENT, .uncertain_index = -1}};
        cp_error_t recorded = cp_complete(server.protocol, key, &completion.result, clock_ms);
        assert(recorded == (scenario == TAKEOVER ? CP_STALE : CP_OK));
        motion_terminal_recorded(&server, &completion, recorded);
        if (scenario == LEGACY) {
            queue_motion_terminal(&server);
            assert(!queued_count && !control_motion_find(&server.channels[slot].motion, key));
            continue;
        }
        assert(control_motion_find(&server.channels[slot].motion, key));
        unsigned sent = ws_text_sends;
        assert(receive_text(body) == ESP_FAIL && ws_text_sends == sent);
        reject_queue = true;
        queue_motion_terminal(&server);
        assert(!server.motion_work.queued && !queued_count);
        reject_queue = false;
        queue_motion_terminal(&server);
        assert(server.motion_work.queued && queued_count == 1);
        if (scenario == NORMAL) {
            sem_available = false;
            drain();
            assert(ws_text_sends == sent && control_motion_find(&server.channels[slot].motion, key));
            sem_available = true;
            queue_motion_terminal(&server);
        } else if (scenario == REPLACED) {
            websocket_free(ws_request.sess_ctx);
            ws_request.sess_ctx = NULL;
            open_browser_channel(); // Same fd and slot, new channel generation.
        } else if (scenario == STOPPING) {
            server.stop_requested = true;
        } else if (scenario == REVOKED) {
            assert(cp_revoke_principal(server.protocol, identity.principal_id, clock_ms) == CP_OK);
        } else if (scenario == SEND_LOST) {
            fail_send = true;
        }
        drain();
        assert(!server.motion_work.queued);
        if (scenario == NORMAL || scenario == TAKEOVER) {
            char expected[80];
            snprintf(expected, sizeof(expected), "\"command_key\":\"%llu\"", (unsigned long long)key);
            assert(ws_text_sends == sent + 1 && strstr(ws_text, expected) &&
                   strstr(ws_text, "\"motion_terminal\":true"));
            assert(strstr(ws_text, scenario == NORMAL ? "\"state\":\"sent\"" : "\"state\":\"cancelled\""));
            motion_terminal_recorded(&server, &completion, cp_complete(server.protocol, key, &completion.result, clock_ms));
            queue_motion_terminal(&server);
            assert(!queued_count);
        } else {
            assert(ws_text_sends == sent && ws_shutdowns == (scenario == REVOKED || scenario == SEND_LOST));
        }
    }
}
static esp_err_t request_motion_width(unsigned width)
{
    char request[64];
    snprintf(request, sizeof(request), "{\"v\":1,\"type\":\"motion_window\",\"motion_window\":%u}", width);
    return receive_text(request);
}

static void test_motion_window_delivers_each_record(unsigned width)
{
    setup_server_transport(true, true, CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL);
    cp_set_motion_terminal_available(server.protocol, true);
    cp_set_motion_window_max(server.protocol, CP_MOTION_WINDOW_CONFIG_MAX);
    cp_http_security_t security = {.host = "device.test", .origin = "http://device.test", .cookie = cookie, .csrf = csrf};
    cp_http_response_t reply;
    assert(cp_http(server.protocol, "GET", "/api/v1/capabilities", &security, NULL, 0, clock_ms, &reply) == CP_OK);
    char advertised_max[48];
    snprintf(advertised_max, sizeof(advertised_max), "\"websocket_motion_window_max\":%u",
             (unsigned)CP_MOTION_WINDOW_CONFIG_MAX);
    assert(strstr(reply.body, "\"websocket_motion_window\":true") &&
           strstr(reply.body, advertised_max) &&
           !strstr(reply.body, "websocket_motion_ordered"));

    open_browser_channel();
    const channel_t *channel = ws_request.sess_ctx;
    const unsigned slot = channel->slot;
    assert((width == 2 ? receive_text("{\"v\":1,\"type\":\"motion_window\"}") :
            request_motion_width(width)) == ESP_OK);
    char selection[32];
    snprintf(selection, sizeof(selection), "\"motion_window\":%u", width);
    assert(strstr(ws_text, selection));
    const char *const invalid[] = {
        "{\"v\":1,\"type\":\"motion_window\",\"motion_window\":0}",
        "{\"v\":1,\"type\":\"motion_window\",\"motion_window\":\"3\"}",
        "{\"v\":1,\"type\":\"motion_window\",\"motion_window\":2.5}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(receive_text(invalid[i]) == ESP_OK && strstr(ws_text, "\"error\":\"invalid\""));
    }
    if (CP_MOTION_WINDOW_CONFIG_MAX < CP_MOTION_WINDOW_WIRE_MAX) {
        assert(request_motion_width(CP_MOTION_WINDOW_WIRE_MAX) == ESP_OK &&
               strstr(ws_text, "\"error\":\"unavailable\""));
    }
    assert(request_motion_width(CP_MOTION_WINDOW_WIRE_MAX + 1) == ESP_OK &&
           strstr(ws_text, "\"error\":\"invalid\""));
    assert(receive_text("{\"v\":1,\"type\":\"motion_window\",\"motion_ordered\":true}") == ESP_OK &&
           strstr(ws_text, "\"error\":\"unavailable\""));
    assert(channel->terminal_mode == CP_TERMINAL_MOVE_WINDOW && channel->motion_width == width);

    char lease[33], body[1500];
    apply_acquire(&browser_client, lease);
    completion_t motions[CP_MOTION_WINDOW_CONFIG_MAX] = {0};
    unsigned sequences[CP_MOTION_WINDOW_CONFIG_MAX];
    for (unsigned i = 0; i < width; ++i) {
        format_client_command(&browser_client, "mouse.move_relative", "{\"dx\":1,\"dy\":0}",
                              lease, body, sizeof(body));
        sequences[i] = browser_client.sequence;
        assert(receive_text(body) == ESP_OK && strstr(ws_text, "\"state\":\"accepted\""));
        assert(dispatched.motion_window);
        motions[i] = (completion_t){.key = dispatched.command_key,
            .result = {.state = CP_RESULT_SENT, .uncertain_index = -1}};
        assert(control_motion_find(&server.channels[slot].motion, motions[i].key));
        if (i == 0) {
            unsigned before = dispatch_calls;
            assert(receive_text("{\"v\":1,\"type\":\"motion_window\"}") == ESP_OK &&
                   strstr(ws_text, "\"error\":\"busy\""));
            test_client_t contender = browser_client;
            char renew[1500];
            format_client_command(&contender, "control.renew", "{}", lease, renew, sizeof(renew));
            assert(receive_text(renew) == ESP_OK && strstr(ws_text, "\"error\":\"busy\""));
            assert(dispatch_calls == before && channel->motion_width == width);
        }
    }
    unsigned sent = ws_text_sends, dispatched_before = dispatch_calls;
    for (unsigned i = 0; i < width; ++i) {
        const control_motion_slot_t *movement = control_motion_find(&server.channels[slot].motion, motions[i].key);
        assert(movement && movement->relative && !movement->ready);
        if (i) assert(motions[i - 1].key < motions[i].key);
    }
    assert(cp_http(server.protocol, "POST", "/api/v1/commands", &security, body, strlen(body), clock_ms, &reply) == CP_OK);
    assert(reply.status == 202 && dispatch_calls == dispatched_before);

    for (unsigned i = width; i-- > 0;) {
        const completion_t *completion = &motions[i];
        cp_error_t recorded = cp_complete(server.protocol, completion->key, &completion->result, clock_ms);
        assert(recorded == CP_OK);
        motion_terminal_recorded(&server, completion, recorded);
        queue_motion_terminal(&server);
        assert(queued_count == 1);
    }
    for (unsigned send = 0; send < width; ++send) {
        const unsigned i = send == 0 ? width - 1 : send - 1;
        char sequence[40], key[48];
        snprintf(sequence, sizeof(sequence), "\"command_seq\":%u", sequences[i]);
        snprintf(key, sizeof(key), "\"command_key\":\"%llu\"", (unsigned long long)motions[i].key);
        assert(cp_transport_motion_inline_result(server.protocol, &channel->identity, motions[i].key,
                                                clock_ms, &reply) == CP_OK);
        assert(strstr(reply.body, sequence) && strstr(reply.body, key) && !strstr(reply.body, "motion_terminal"));
        drain();
        assert(ws_text_sends == ++sent && strstr(ws_text, sequence) && strstr(ws_text, key) &&
               strstr(ws_text, "\"motion_terminal\":true"));
        assert(!control_motion_find(&server.channels[slot].motion, motions[i].key));
        queue_motion_terminal(&server);
    }
    assert(!queued_count && !ws_shutdowns);
    clock_ms += 100; /* Refill the session's rate budget before the second burst. */
    for (unsigned i = 0; i < width; ++i) {
        format_client_command(&browser_client, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease, body, sizeof(body));
        assert(receive_text(body) == ESP_OK && strstr(ws_text, "\"state\":\"accepted\""));
    }
    unsigned full = ws_text_sends;
    format_client_command(&browser_client, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease, body, sizeof(body));
    assert(receive_text(body) == ESP_FAIL && ws_text_sends == full && dispatch_calls == dispatched_before + width);
    websocket_free(ws_request.sess_ctx);
    ws_request.sess_ctx = NULL;
    queue_motion_terminal(&server);
    assert(!queued_count && !server.channels[slot].used);
}

static void test_motion_window_renew_is_exclusive(void)
{
    setup_server_transport(true, true, CP_SCOPE_VIEW | CP_SCOPE_INPUT_CONTROL);
    cp_set_motion_terminal_available(server.protocol, true);
    cp_set_motion_window_max(server.protocol, CP_MOTION_WINDOW_CONFIG_MAX);
    open_browser_channel();
    const channel_t *channel = ws_request.sess_ctx;
    assert(request_motion_width(CP_MOTION_WINDOW_CONFIG_MAX) == ESP_OK);

    char lease[33], body[1500];
    apply_acquire(&browser_client, lease);
    cp_http_security_t security = {.host = "device.test", .origin = "http://device.test", .cookie = cookie, .csrf = csrf};
    cp_http_response_t reply;
    assert(cp_http(server.protocol, "GET", "/api/v1/state", &security, NULL, 0, clock_ms, &reply) == CP_OK);
    format_client_command(&browser_client, "control.renew", "{\"state_sequence\":\"1\"}", lease, body, sizeof(body));
    assert(receive_text(body) == ESP_OK && strstr(ws_text, "\"state\":\"accepted\""));
    const control_motion_slot_t *renewal = control_motion_find(&server.channels[channel->slot].motion, dispatched.command_key);
    assert(renewal && !renewal->relative);
    unsigned sent = ws_text_sends, dispatched_before = dispatch_calls;
    format_client_command(&browser_client, "mouse.move_relative", "{\"dx\":1,\"dy\":0}", lease, body, sizeof(body));
    assert(receive_text(body) == ESP_FAIL && ws_text_sends == sent && dispatch_calls == dispatched_before);
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
    test_enrolled_http_upgrade();
    test_motion_terminal_delivery();
    test_motion_window_delivers_each_record(2);
    test_motion_window_delivers_each_record(CP_MOTION_WINDOW_CONFIG_MAX);
    if (CP_MOTION_WINDOW_CONFIG_MAX == 5) {
        test_motion_window_renew_is_exclusive();
    }
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
