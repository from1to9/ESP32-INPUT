// SPDX-License-Identifier: Apache-2.0
#include "app_control_server.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if SOC_WIFI_SUPPORTED && CONFIG_ESP_WIFI_ENABLED
#include <net/if.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "app_control_dns.h"
#include "app_network_task.h"
#include "app_control_web.h"
#include "control_motion_window.h"
#include "app_wifi.h"

#define CERT_BYTES 2048U
#define KEY_BYTES 1024U
#define COMPLETION_CAPACITY CP_MAX_ACTIVE_REQUESTS

typedef struct {
    uint64_t key;
    cp_result_t result;
} completion_t;
typedef struct server server_t;
struct server {
    unsigned slot;
    cp_session_view_t sessions[CP_MAX_SESSIONS];
    size_t session_count;
    cp_context_t *protocol;
    void *protocol_memory;
    SemaphoreHandle_t mutex;
    httpd_handle_t listener;
    struct ifreq interface;
    cp_callbacks_t callbacks;
    cp_principal_t principals[CP_MAX_PRINCIPALS];
    size_t principal_count;
    char ifkey[32], host[129], origin[193];
    char hotspot_name[APP_CONTROL_HOTSPOT_NAME_CAPACITY];
    char hotspot_ip_host[APP_WIFI_IP_CAPACITY + 6];
    int dns_fd;
    uint32_t dns_address;
    uint8_t certificate[CERT_BYTES + 1], private_key[KEY_BYTES + 1];
    size_t certificate_length, key_length;
    uint16_t port;
    cp_device_state_t state;
    bool state_pending, state_known, override_pending, override_locked, stop_requested;
    completion_t completions[COMPLETION_CAPACITY];
    unsigned completion_head, completion_count;
    char body[CP_MAX_FRAME_BYTES + 1];
    cp_http_response_t response;
    bool download_active;
    void *media_pending[2];
    unsigned media_pending_count;
    cp_identity_t channel_closures[CP_MAX_SESSIONS];
    unsigned channel_closure_count;
    uint64_t next_channel_generation;
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
};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Completions can outlive a listener. Never restart the shared key sequence
 * while the firmware still owns work from either listener. */
static uint64_t s_command_key;
static server_t *s_servers[2]; /* TLS, enrolled local HTTP. */
#define s_server s_servers[0] /* Existing streaming-media owner remains TLS. */
static app_control_server_snapshot_t s_snapshots[2];
static app_control_media_port_t s_media_port;
static void cache_sessions(server_t *s)
{
    cp_session_view_t views[CP_MAX_SESSIONS] = {0};
    size_t count = cp_sessions_snapshot(s->protocol, views);
    portENTER_CRITICAL(&s_lock);
    memcpy(s->sessions, views, sizeof(views));
    s->session_count = count;
    portEXIT_CRITICAL(&s_lock);
}
static bool cached_authority(const cp_session_view_t *v, const cp_callbacks_t *callbacks)
{
    if (!callbacks->principal) return true; /* Original standalone/legacy provider. */
    cp_principal_t current;
    return callbacks->principal(callbacks->user, v->identity.principal_id, &current) &&
        current.enabled && current.scopes == v->identity.scopes &&
        !memcmp(current.id, v->identity.principal_id, CP_ID_BYTES) &&
        !memcmp(current.credential_sha256, v->credential_sha256, 32);
}
static bool cached_session(const uint8_t *session, const uint8_t *principal,
                            const cp_identity_t *identity)
{
    cp_session_view_t view = {0}; cp_callbacks_t callbacks = {0};
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    bool alive = false;
    portENTER_CRITICAL(&s_lock);
    for (unsigned k = 0; k < 2 && !alive; ++k) {
        server_t *s = s_servers[k];
        if (!s || s->stop_requested) continue;
        callbacks = s->callbacks;
        for (size_t i = 0; i < s->session_count; ++i) {
            const cp_session_view_t *v = &s->sessions[i];
            if (v->authenticated && now >= v->last_authenticated_ms &&
                now - v->last_authenticated_ms < 5000U &&
                (!session || !memcmp(session, v->identity.session_id, CP_ID_BYTES)) &&
                !memcmp(principal, v->identity.principal_id, CP_ID_BYTES) &&
                (!identity || (identity->scopes == v->identity.scopes && identity->browser == v->identity.browser))) {
                view = *v; alive = true; break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return alive && cached_authority(&view, &callbacks);
}
bool app_control_server_session_alive(const uint8_t session[CP_ID_BYTES], const uint8_t principal[CP_ID_BYTES])
{
    return session && principal && cached_session(session, principal, NULL);
}
bool app_control_server_identity_alive(const cp_identity_t *identity)
{
    return identity && cached_session(identity->session_id, identity->principal_id, identity);
}
bool app_control_server_client_connected(const uint8_t principal[CP_ID_BYTES])
{
    return principal && cached_session(NULL, principal, NULL);
}
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void clear(void *memory, size_t length)
{
    volatile unsigned char *p = memory;
    while (length--) {
        *p++ = 0;
    }
}
static void publish(unsigned slot, app_control_server_state_t state, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshots[slot].state = state;
    s_snapshots[slot].error = error;
    ++s_snapshots[slot].revision;
    if (state != APP_CONTROL_READY) {
        s_snapshots[slot].tls = false;
        s_snapshots[slot].address[0] = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}
static uint64_t next_command_key(void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&s_lock);
    uint64_t key = s_command_key == UINT64_MAX ? 0 : ++s_command_key;
    portEXIT_CRITICAL(&s_lock);
    return key;
}
static bool random_source(void *user, uint8_t *output, size_t length)
{
    server_t *s = user;
    if (s->callbacks.random) {
        return s->callbacks.random(s->callbacks.user, output, length);
    }
    esp_fill_random(output, length);
    return true;
}
static bool principal_source(void *user, const uint8_t id[16], cp_principal_t *out)
{
    server_t *s = user;
    return s->callbacks.principal(s->callbacks.user, id, out);
}
static bool input_authorized(void *user, const cp_identity_t *identity)
{
    server_t *s = user;
    return s->callbacks.input_authorized(s->callbacks.user, identity);
}
static cp_error_t pairing(void *user, bool status, const char *client, const char *proof,
                           const char *label, char *json, size_t capacity, size_t *length)
{
    server_t *s = user;
    return s->callbacks.pairing(s->callbacks.user, status, client, proof, label, json, capacity, length);
}
static cp_error_t pairing_code(void *user, const char *client, const char *credential,
    const char *label, const char *code, const char *duration, char *json, size_t capacity, size_t *length)
{
    server_t *s = user;
    return s->callbacks.pairing_code(s->callbacks.user, client, credential, label, code, duration,
                                     json, capacity, length);
}
static bool available(void *user, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{
    server_t *s = user;
    return s->callbacks.available && s->callbacks.available(s->callbacks.user, op, reason);
}
static cp_error_t dispatch(void *user, const cp_command_t *command, cp_result_t *result)
{
    server_t *s = user;
    return s->callbacks.dispatch ? s->callbacks.dispatch(s->callbacks.user, command, result)
                                 : CP_UNAVAILABLE;
}
static void safety(void *user, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    server_t *s = user;
    if (reason == CP_SAFETY_SESSION_LOST || reason == CP_SAFETY_REVOKED) {
        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < s->session_count; ++i) {
            if (!identity ||
                memcmp(identity->session_id, s->sessions[i].identity.session_id, CP_ID_BYTES) == 0) {
                s->sessions[i].authenticated = false;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
    if (s->callbacks.safety) {
        s->callbacks.safety(s->callbacks.user, reason, identity);
    }
}
static cp_error_t read_state(void *user, const cp_identity_t *identity, const char *resource,
                             char *json, size_t capacity, size_t *length)
{
    server_t *s = user;
    return s->callbacks.read
               ? s->callbacks.read(s->callbacks.user, identity, resource, json, capacity, length)
               : CP_UNAVAILABLE;
}
static bool header(httpd_req_t *request, const char *name, char *value, size_t capacity,
                   bool required)
{
    size_t length = httpd_req_get_hdr_value_len(request, name);
    value[0] = 0;
    if (!length) {
        return !required;
    }
    return length < capacity &&
           httpd_req_get_hdr_value_str(request, name, value, capacity) == ESP_OK;
}
static esp_err_t error_response(httpd_req_t *request, const char *status, const char *error)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    (void)httpd_resp_sendstr(request, error);
    return ESP_FAIL;
}

static const char *accepted_control_host(const server_t *s, const char *host,
    const char *origin, const char *cookie, const char *csrf, bool native_request)
{
    if (!strcmp(host, s->host)) return host;
    /* Browser assets, sessions and tickets live only at the unique DNS
     * origin. The exact IP alias admits native login or Bearer traffic. */
    if (s->hotspot_ip_host[0] && !strcmp(host, s->hotspot_ip_host) &&
        native_request && !origin[0] && !cookie[0] && !csrf[0]) return s->host;
    return NULL;
}

#include "app_control_server_media.inc"

static esp_err_t handle_web(httpd_req_t *request)
{
    server_t *s = request->user_ctx;
    return app_control_web_serve(request, s->host, s->origin);
}

static esp_err_t handle_request(httpd_req_t *request)
{
    server_t *s = request->user_ctx;
    esp_err_t answer = ESP_FAIL;
    char host[129], origin[193], authorization[80], cookie[512], csrf[33], type[64], encoding[32],
        transfer[32];
    if (!header(request, "Host", host, sizeof(host), true) ||
        !header(request, "Origin", origin, sizeof(origin), false) ||
        !header(request, "Authorization", authorization, sizeof(authorization), false) ||
        !header(request, "Cookie", cookie, sizeof(cookie), false) ||
        !header(request, "X-CSRF-Token", csrf, sizeof(csrf), false) ||
        !header(request, "Content-Type", type, sizeof(type), false) ||
        !header(request, "Content-Encoding", encoding, sizeof(encoding), false) ||
        !header(request, "Transfer-Encoding", transfer, sizeof(transfer), false)) {
        answer = error_response(request, "400 Bad Request", "{\"error\":\"invalid_headers\"}");
        goto request_cleanup;
    }
    if (encoding[0] || transfer[0]) {
        answer = error_response(request, "400 Bad Request", "{\"error\":\"unsupported_encoding\"}");
        goto request_cleanup;
    }
    if (request->content_len > CP_MAX_FRAME_BYTES) {
        answer = error_response(request, "413 Content Too Large", "{\"error\":\"too_large\"}");
        goto request_cleanup;
    }
    bool post = request->method == HTTP_POST;
    if ((request->content_len && !post) || (post && strcmp(type, "application/json") != 0)) {
        answer = error_response(request, "400 Bad Request", "{\"error\":\"invalid_content_type\"}");
        goto request_cleanup;
    }
    size_t received = 0;
    uint64_t until = now_ms() + 4000U;
    while (received < request->content_len && now_ms() < until) {
        int count = httpd_req_recv(request, s->body + received, request->content_len - received);
        if (count <= 0) {
            break;
        }
        received += (size_t)count;
    }
    if (received != request->content_len) {
        clear(s->body, sizeof(s->body));
        answer = error_response(request, "408 Request Timeout", "{\"error\":\"expired_body\"}");
        goto request_cleanup;
    }
    s->body[received] = 0;
    cp_http_security_t security = {.tls = s->slot == 0,
                                   .host = host,
                                   .origin = origin[0] ? origin : NULL,
                                   .authorization = authorization[0] ? authorization : NULL,
                                   .cookie = cookie[0] ? cookie : NULL,
                                   .csrf = csrf[0] ? csrf : NULL};
    const char *method = post ? "POST" : request->method == HTTP_DELETE ? "DELETE" : "GET";
    security.host = accepted_control_host(s, host, origin, cookie, csrf,
        authorization[0] || (post && !strcmp(request->uri, "/api/v1/sessions")));
    if (!security.host) {
        answer = error_response(request, "403 Forbidden", "{\"error\":\"forbidden\"}");
        goto request_cleanup;
    }
    if (request->method == HTTP_GET && recording_content_path(request->uri, NULL)) {
        answer = begin_recording_download(request, s, &security);
        goto request_cleanup;
    }
    if (xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        clear(s->body, sizeof(s->body));
        answer = error_response(request, "503 Service Unavailable", "{\"error\":\"busy\"}");
        goto request_cleanup;
    }
    portENTER_CRITICAL(&s_lock);
    bool stopping = s->stop_requested;
    portEXIT_CRITICAL(&s_lock);
    if (stopping) {
        xSemaphoreGive(s->mutex);
        answer = error_response(request, "503 Service Unavailable", "{\"error\":\"stopping\"}");
        goto request_cleanup;
    }
    drain_channel_closures(s);
    (void)cp_http(s->protocol, method, request->uri, &security, s->body, received, now_ms(),
                  &s->response);
    cache_sessions(s);
    xSemaphoreGive(s->mutex);
    clear(s->body, sizeof(s->body));
    clear(authorization, sizeof(authorization));
    clear(cookie, sizeof(cookie));
    clear(csrf, sizeof(csrf));
    char status[48];
    const char *reason = s->response.status == 200   ? "OK"
                         : s->response.status == 202 ? "Accepted"
                         : s->response.status == 400 ? "Bad Request"
                         : s->response.status == 401 ? "Unauthorized"
                         : s->response.status == 403 ? "Forbidden"
                         : s->response.status == 408 ? "Request Timeout"
                         : s->response.status == 409 ? "Conflict"
                         : s->response.status == 413 ? "Content Too Large"
                         : s->response.status == 423 ? "Locked"
                         : s->response.status == 429 ? "Too Many Requests"
                                                     : "Service Unavailable";
    snprintf(status, sizeof(status), "%u %s", s->response.status, reason);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'none'; frame-ancestors 'none'");
    if (s->response.set_cookie[0]) {
        httpd_resp_set_hdr(request, "Set-Cookie", s->response.set_cookie);
    }
    answer = httpd_resp_send(request, s->response.body, (ssize_t)s->response.length);
request_cleanup:
    clear(authorization, sizeof(authorization));
    clear(cookie, sizeof(cookie));
    clear(csrf, sizeof(csrf));
    clear(s->body, sizeof(s->body));
    clear(&s->response, sizeof(s->response));
    return answer;
}
static esp_err_t hotspot_dns_start(server_t *s, uint32_t address)
{
    if (!s->hotspot_name[0]) return ESP_OK;
    s->dns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->dns_fd < 0) return ESP_FAIL;
    const struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(53),
                                     .sin_addr.s_addr = address};
    if (setsockopt(s->dns_fd, SOL_SOCKET, SO_BINDTODEVICE, &s->interface,
                   sizeof(s->interface)) < 0 ||
        bind(s->dns_fd, (const struct sockaddr *)&local, sizeof(local)) < 0) return ESP_FAIL;
    s->dns_address = address;
    return ESP_OK;
}

/* Keep the DNS packet buffer out of the authentication tick's stack frame. */
static void __attribute__((noinline)) hotspot_dns_service(server_t *s)
{
    if (s->dns_fd < 0) return;
    uint8_t packet[512 + 16];
    for (unsigned i = 0; i < 4; ++i) {
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        ssize_t count = recvfrom(s->dns_fd, packet, 512, MSG_DONTWAIT,
                                (struct sockaddr *)&peer, &peer_size);
        if (count < 0) return;
        size_t length = app_control_dns_reply(s->hotspot_name, s->dns_address, packet,
                                              (size_t)count, sizeof(packet));
        if (length)
            (void)sendto(s->dns_fd, packet, length, MSG_DONTWAIT,
                         (const struct sockaddr *)&peer, peer_size);
    }
}

static void hotspot_dns_close(server_t *s)
{
    if (s->dns_fd < 0) return;
    close(s->dns_fd);
    s->dns_fd = -1;
}

static esp_err_t initialize(server_t *s)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(s->ifkey);
    esp_netif_ip_info_t ip = {0};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = esp_netif_get_netif_impl_name(netif, s->interface.ifr_name);
    if (error != ESP_OK) {
        return error;
    }
    s->mutex = xSemaphoreCreateMutex();
    if (!s->mutex) {
        return ESP_ERR_NO_MEM;
    }
    s->protocol_memory =
        heap_caps_aligned_calloc(cp_context_alignment(), 1, cp_context_size(),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->protocol_memory) {
        s->protocol_memory =
            heap_caps_aligned_calloc(cp_context_alignment(), 1, cp_context_size(), MALLOC_CAP_8BIT);
    }
    if (!s->protocol_memory) {
        return ESP_ERR_NO_MEM;
    }
    cp_callbacks_t callbacks = {.user = s,
                                .random = random_source,
                                .available = available,
                                .dispatch = dispatch,
                                .safety = safety,
                                .read = read_state,
                                .principal = s->callbacks.principal ? principal_source : NULL,
                                .input_authorized = s->callbacks.input_authorized ? input_authorized : NULL,
                                .pairing = s->callbacks.pairing ? pairing : NULL,
                                .pairing_code = s->callbacks.pairing_code ? pairing_code : NULL,
                                .next_command_key = next_command_key};
    cp_error_t result =
        cp_initialize_policy(s->protocol_memory, cp_context_size(), &callbacks,
                             s->slot ? CP_POLICY_ENROLLED_HTTP : CP_POLICY_ENROLLED_TLS, &s->protocol);
    if (result != CP_OK || cp_http_configure(s->protocol, s->host, s->origin) != CP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s->principal_count; ++i) {
        if (cp_load_principal(s->protocol, &s->principals[i]) != CP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    error = hotspot_dns_start(s, ip.ip.addr);
    if (error != ESP_OK) return error;
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.transport_mode = s->slot ? HTTPD_SSL_TRANSPORT_INSECURE : HTTPD_SSL_TRANSPORT_SECURE;
    config.port_insecure = s->port;
    config.port_secure = s->port;
    config.servercert = s->certificate;
    config.servercert_len = s->certificate_length;
    config.prvtkey_pem = s->private_key;
    config.prvtkey_len = s->key_length;
    config.httpd.if_name = &s->interface;
    config.httpd.ctrl_port = 32770 + s->slot;
    config.httpd.max_open_sockets = 4;
    config.httpd.max_uri_handlers = 8;
    /* Reserve room for browser UA, Accept, Fetch Metadata and cookie headers.
     * The SDK allocates according to the actual request header length. */
    config.httpd.max_req_hdr_len = 3072;
    config.httpd.backlog_conn = 2;
    config.httpd.stack_size = 12288;
#if APP_NETWORK_EXTERNAL_STACK
    config.httpd.task_caps = APP_NETWORK_STACK_CAPS;
#endif
    config.httpd.task_priority = 1;
    config.httpd.recv_wait_timeout = 2;
    config.httpd.send_wait_timeout = 2;
    config.httpd.lru_purge_enable = false;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    error = httpd_ssl_start(&s->listener, &config);
#if CONFIG_HTTPD_WS_SUPPORT && CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    if (error == ESP_OK) {
        httpd_uri_t channel = {.uri = "/api/v1/channel",
                               .method = HTTP_GET,
                               .handler = websocket_data,
                               .user_ctx = s,
                               .is_websocket = true,
                               .supported_subprotocol = "esp32-control-v1",
                               .handle_ws_control_frames = true,
                               .ws_pre_handshake_cb = websocket_authenticate,
                               .ws_control_handler = websocket_control};
        error = httpd_register_uri_handler(s->listener, &channel);
    }
#endif
    for (unsigned i = 0; error == ESP_OK && i < 3; ++i) {
        httpd_uri_t route = {.uri = "/api/v1/*",
                             .method = i == 0   ? HTTP_GET
                                       : i == 1 ? HTTP_POST
                                                : HTTP_DELETE,
                             .handler = handle_request,
                             .user_ctx = s};
        error = httpd_register_uri_handler(s->listener, &route);
    }
    const char *const web_paths[] = {"/", "/controller.css", "/controller.js", "/motion_window.mjs"};
    for (unsigned i = 0; error == ESP_OK && i < sizeof(web_paths) / sizeof(web_paths[0]); ++i) {
        httpd_uri_t route = {.uri = web_paths[i], .method = HTTP_GET,
                             .handler = handle_web, .user_ctx = s};
        error = httpd_register_uri_handler(s->listener, &route);
    }
    bool websocket = false;
#if CONFIG_HTTPD_WS_SUPPORT && CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    websocket = error == ESP_OK;
#endif
    cp_set_transport_capabilities(s->protocol, websocket, error == ESP_OK);
    cp_set_motion_terminal_available(s->protocol, websocket);
    cp_set_motion_window_max(s->protocol, CP_MOTION_WINDOW_CONFIG_MAX);
    portENTER_CRITICAL(&s_lock);
    s_snapshots[s->slot].websocket_available = websocket;
    portEXIT_CRITICAL(&s_lock);
    return error;
}
static void worker(void *argument)
{
    server_t *s = argument;
    esp_err_t error = initialize(s);
    bool failed = error != ESP_OK;
    if (!failed) {
        portENTER_CRITICAL(&s_lock);
        s_snapshots[s->slot].state = s->stop_requested ? APP_CONTROL_STOPPING : APP_CONTROL_READY;
        s_snapshots[s->slot].tls = !s->slot && !s->stop_requested;
        s_snapshots[s->slot].error = ESP_OK;
        ++s_snapshots[s->slot].revision;
        if (!s->stop_requested) snprintf(s_snapshots[s->slot].address, sizeof(s_snapshots[s->slot].address), "%s://%s", s->slot ? "http" : "https", s->host);
        portEXIT_CRITICAL(&s_lock);
    } else {
        portENTER_CRITICAL(&s_lock);
        s->stop_requested = true;
        portEXIT_CRITICAL(&s_lock);
        publish(s->slot, APP_CONTROL_STOPPING, error);
    }
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        bool stop = s->stop_requested || failed;
        bool state_pending = s->state_pending;
        cp_device_state_t state = s->state;
        s->state_pending = false;
        bool override = s->override_pending, locked = s->override_locked;
        s->override_pending = s->override_locked = false;
        completion_t completion = {0};
        bool has_completion = s->completion_count != 0;
        if (has_completion) {
            completion = s->completions[s->completion_head];
        }
        portEXIT_CRITICAL(&s_lock);
        if (s->protocol && xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            drain_channel_closures(s);
            if (override) {
                cp_physical_override(s->protocol, locked, now_ms());
            }
            if (state_pending) {
                cp_set_device_state(s->protocol, &state, now_ms());
            }
            if (has_completion) {
                cp_error_t recorded = cp_complete(s->protocol, completion.key, &completion.result, now_ms());
                motion_terminal_recorded(s, &completion, recorded);
                portENTER_CRITICAL(&s_lock);
                s->completion_head = (s->completion_head + 1) % COMPLETION_CAPACITY;
                --s->completion_count;
                portEXIT_CRITICAL(&s_lock);
            }
            cp_tick(s->protocol, now_ms());
            if (stop) {
                cp_shutdown(s->protocol, now_ms());
            }
            cache_sessions(s);
            xSemaphoreGive(s->mutex);
        } else if (state_pending || override || has_completion) {
            /* HTTP processing is bounded/nonblocking. Preserve undelivered
             * state and completion instead of reporting mailbox success as ACK. */
            portENTER_CRITICAL(&s_lock);
            if (state_pending && !s->state_pending) {
                s->state = state;
                s->state_pending = true;
            }
            s->override_pending |= override;
            s->override_locked |= locked;
            portEXIT_CRITICAL(&s_lock);
        }
        if (!stop) queue_motion_terminal(s);
        if (stop) {
            hotspot_dns_close(s);
            portENTER_CRITICAL(&s_lock);
            bool transport_pending = s->download_active || s->media_pending_count != 0 || s->motion_work.queued;
            portEXIT_CRITICAL(&s_lock);
            if (transport_pending) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (s->protocol && xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (s->protocol) {
                cp_shutdown(s->protocol, now_ms());
                cache_sessions(s);
                xSemaphoreGive(s->mutex);
            }
            if (s->listener) {
                esp_err_t stopped = httpd_ssl_stop(s->listener);
                if (stopped != ESP_OK) {
                    publish(s->slot, APP_CONTROL_STOPPING, stopped);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                s->listener = NULL;
            }
            portENTER_CRITICAL(&s_lock);
            s_servers[s->slot] = NULL;
            portEXIT_CRITICAL(&s_lock);
            if (s->mutex) {
                vSemaphoreDelete(s->mutex);
            }
            if (s->protocol_memory) {
                clear(s->protocol_memory, cp_context_size());
                heap_caps_free(s->protocol_memory);
            }
            unsigned slot = s->slot;
            clear(s, sizeof(*s));
            heap_caps_free(s);
            publish(slot, failed ? APP_CONTROL_ERROR : APP_CONTROL_STOPPED, failed ? error : ESP_OK);
            app_network_task_delete_self();
            return;
        }
        hotspot_dns_service(s);
        vTaskDelay(pdMS_TO_TICKS(has_completion ? 1 : 20));
    }
}
static esp_err_t start(const app_control_server_config_t *config, unsigned slot)
{
    if (!config || config->abi_version != APP_CONTROL_SERVER_ABI || !config->interface_key ||
        !config->allowed_host || !config->browser_origin ||
        (!slot && (!config->certificate_pem || !config->private_key_pem || !config->certificate_length ||
         config->certificate_length > CERT_BYTES || !config->private_key_length ||
         config->private_key_length > KEY_BYTES || !config->principal_count || !config->principals)) ||
        config->principal_count > CP_MAX_PRINCIPALS ||
        (config->principal_count && !config->principals) ||
        (strcmp(config->interface_key, "WIFI_STA_DEF") != 0 &&
         strcmp(config->interface_key, "WIFI_AP_DEF") != 0) || strlen(config->allowed_host) >= 129 ||
        strlen(config->browser_origin) >= 193) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ap = !strcmp(config->interface_key, "WIFI_AP_DEF");
    if (!slot && (ap ? (!config->hotspot_name || !config->hotspot_name[0] ||
              strlen(config->hotspot_name) >= APP_CONTROL_HOTSPOT_NAME_CAPACITY) :
             config->hotspot_name != NULL)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    bool busy = s_servers[slot] || s_snapshots[slot].state == APP_CONTROL_STARTING ||
                s_snapshots[slot].state == APP_CONTROL_STOPPING;
    if (!busy) {
        s_snapshots[slot].state = APP_CONTROL_STARTING;
        s_snapshots[slot].error = ESP_OK;
        ++s_snapshots[slot].revision;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    server_t *s = heap_caps_aligned_calloc(_Alignof(server_t), 1, sizeof(*s),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) {
        s = heap_caps_aligned_calloc(_Alignof(server_t), 1, sizeof(*s), MALLOC_CAP_8BIT);
    }
    if (!s) {
        publish(slot, APP_CONTROL_ERROR, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s->slot = slot;
    strcpy(s->ifkey, config->interface_key);
    strcpy(s->host, config->allowed_host);
    strcpy(s->origin, config->browser_origin);
    s->port = config->port ? config->port : slot ? 80 : 443;
    s->dns_fd = -1;
    if (ap && !slot) {
        strcpy(s->hotspot_name, config->hotspot_name);
        if (s->port == 443) strcpy(s->hotspot_ip_host, APP_WIFI_CONTROL_AP_IP);
        else snprintf(s->hotspot_ip_host, sizeof(s->hotspot_ip_host),
                      APP_WIFI_CONTROL_AP_IP ":%u", s->port);
    }
    s->callbacks = config->coordinator;
    if (!slot) {
    memcpy(s->certificate, config->certificate_pem, config->certificate_length);
    s->certificate_length =
        config->certificate_length + (config->certificate_pem[config->certificate_length - 1] != 0);
    memcpy(s->private_key, config->private_key_pem, config->private_key_length);
    s->key_length =
        config->private_key_length + (config->private_key_pem[config->private_key_length - 1] != 0);
    }
    if (config->principal_count) memcpy(s->principals, config->principals, config->principal_count * sizeof(cp_principal_t));
    s->principal_count = config->principal_count;
    portENTER_CRITICAL(&s_lock);
    s_servers[slot] = s;
    portEXIT_CRITICAL(&s_lock);
    if (app_network_task_create(worker, "control_server", 4096, s, 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_servers[slot] = NULL;
        portEXIT_CRITICAL(&s_lock);
        clear(s, sizeof(*s));
        heap_caps_free(s);
        publish(slot, APP_CONTROL_ERROR, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t stop(unsigned slot)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (s_servers[slot]) {
        s_servers[slot]->stop_requested = true;
        s_snapshots[slot].state = APP_CONTROL_STOPPING;
        s_snapshots[slot].tls = false;
        s_snapshots[slot].address[0] = 0;
        ++s_snapshots[slot].revision;
    } else if (s_snapshots[slot].state == APP_CONTROL_STARTING) {
        error = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}
static void snapshot(unsigned slot, app_control_server_snapshot_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshots[slot];
    portEXIT_CRITICAL(&s_lock);
}
esp_err_t app_control_server_start(const app_control_server_config_t *c) { return start(c, 0); }
esp_err_t app_control_server_start_http(const app_control_server_config_t *c) { return start(c, 1); }
esp_err_t app_control_server_stop(void) { return stop(0); }
esp_err_t app_control_server_stop_http(void) { return stop(1); }
void app_control_server_snapshot(app_control_server_snapshot_t *out) { snapshot(0, out); }
void app_control_server_http_snapshot(app_control_server_snapshot_t *out) { snapshot(1, out); }
esp_err_t app_control_server_publish_state(const cp_device_state_t *state)
{
    if (!state || strnlen(state->target_id, sizeof(state->target_id)) == sizeof(state->target_id) ||
        strnlen(state->mode, sizeof(state->mode)) == sizeof(state->mode)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    for (unsigned k = 0; k < 2; ++k) {
        server_t *s = s_servers[k];
        if (s && !s->stop_requested) {
            s->state = *state; s->state_pending = s->state_known = true; error = ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}
esp_err_t app_control_server_complete(uint64_t key, const cp_result_t *result)
{
    if (!key || !result) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    /* Preflight all bounded mailboxes before enqueueing, so a retry cannot
     * duplicate delivery. Each protocol ignores keys owned by the other. */
    for (unsigned k = 0; k < 2; ++k) {
        server_t *s = s_servers[k];
        if (s && !s->stop_requested) {
            error = s->completion_count == COMPLETION_CAPACITY ? ESP_ERR_NO_MEM : ESP_OK;
            if (error != ESP_OK) break;
        }
    }
    if (error == ESP_OK) for (unsigned k = 0; k < 2; ++k) {
        server_t *s = s_servers[k];
        if (s && !s->stop_requested) {
            unsigned tail = (s->completion_head + s->completion_count) % COMPLETION_CAPACITY;
            s->completions[tail] = (completion_t){.key = key, .result = *result}; ++s->completion_count;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}
void app_control_server_physical_override(bool locked)
{
    portENTER_CRITICAL(&s_lock);
    for (unsigned k = 0; k < 2; ++k) if (s_servers[k]) {
        s_servers[k]->override_pending = true; s_servers[k]->override_locked |= locked;
    }
    portEXIT_CRITICAL(&s_lock);
}

#else
bool app_control_server_client_connected(const uint8_t id[CP_ID_BYTES]) { (void)id; return false; }
bool app_control_server_identity_alive(const cp_identity_t *who)
{
    (void)who;
    return false;
}
uint64_t app_control_server_channel_generation(const cp_identity_t *who)
{
    (void)who;
    return 0;
}
void app_control_server_set_media_port(const app_control_media_port_t *p) { (void)p; }
esp_err_t app_control_server_capture_send(const cp_identity_t *i, const uint8_t job[16],
                                          uint64_t gen, uint64_t channel_generation,
                                          const uint8_t *p, size_t n)
{
    (void)i;
    (void)job;
    (void)gen;
    (void)channel_generation;
    (void)p;
    (void)n;
    return ESP_ERR_NOT_SUPPORTED;
}
void app_control_server_capture_cancel(const cp_identity_t *i, const uint8_t job[16], uint64_t gen,
                                       uint64_t channel_generation)
{
    (void)i;
    (void)job;
    (void)gen;
    (void)channel_generation;
}
bool app_control_server_session_alive(const uint8_t session[CP_ID_BYTES],
                                      const uint8_t principal[CP_ID_BYTES])
{
    (void)session;
    (void)principal;
    return false;
}
esp_err_t app_control_server_start(const app_control_server_config_t *c)
{
    (void)c;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_control_server_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
void app_control_server_snapshot(app_control_server_snapshot_t *s)
{
    if (s) {
        *s = (app_control_server_snapshot_t){.state = APP_CONTROL_ERROR,
                                             .error = ESP_ERR_NOT_SUPPORTED};
    }
}
esp_err_t app_control_server_publish_state(const cp_device_state_t *s)
{
    (void)s;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_control_server_complete(uint64_t key, const cp_result_t *r)
{
    (void)key;
    (void)r;
    return ESP_ERR_NOT_SUPPORTED;
}
void app_control_server_physical_override(bool locked) { (void)locked; }
esp_err_t app_control_server_start_http(const app_control_server_config_t *c)
{ (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_control_server_stop_http(void) { return ESP_ERR_NOT_SUPPORTED; }
void app_control_server_http_snapshot(app_control_server_snapshot_t *s) { app_control_server_snapshot(s); }
#endif
