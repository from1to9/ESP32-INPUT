// SPDX-License-Identifier: Apache-2.0
/* Included by the station HTTPS owner. Audio/store calls below only copy into
 * the existing media worker's bounded mailbox; this task never reads flash. */
#include "app_remote_media_runtime_port.h"
#include "control_media.h"
#include <inttypes.h>

static void drain_channel_closures(server_t *s)
{
    cp_identity_t identities[CP_MAX_SESSIONS];
    portENTER_CRITICAL(&s_lock);
    unsigned count = s->channel_closure_count;
    memcpy(identities, s->channel_closures, sizeof(identities));
    s->channel_closure_count = 0;
    clear(s->channel_closures, sizeof(s->channel_closures));
    portEXIT_CRITICAL(&s_lock);
    for (unsigned i = 0; i < count; ++i) {
        (void)cp_transport_close(s->protocol, &identities[i], now_ms());
    }
}
#if CONFIG_HTTPD_WS_SUPPORT && CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
static void channel_closed(server_t *s, const cp_identity_t *who)
{
    /* No HTTP keep-alive or input-only takeover calls this path. On mutex
     * pressure retain exact ownership in a bounded mailbox, and gate actual
     * input work immediately through its scoped nonblocking safety port. */
    if (xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        (void)cp_transport_close(s->protocol, who, now_ms());
        cache_sessions(s);
        xSemaphoreGive(s->mutex);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool present = false;
    for (unsigned i = 0; i < s->channel_closure_count; ++i) {
        if (!memcmp(s->channel_closures[i].session_id, who->session_id, 16)) {
            present = true;
        }
    }
    bool current = false;
    for (size_t i = 0; i < s_session_count; ++i) {
        if (s_sessions[i].authenticated &&
            !memcmp(s_sessions[i].identity.session_id, who->session_id, 16) &&
            !memcmp(s_sessions[i].identity.principal_id, who->principal_id, 16)) {
            current = true;
        }
    }
    if (!current) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (!present && s->channel_closure_count < CP_MAX_SESSIONS) {
        s->channel_closures[s->channel_closure_count++] = *who;
    }
    portEXIT_CRITICAL(&s_lock);
    safety(s, CP_SAFETY_STATE_CHANGED, who);
}
#endif
typedef struct {
    server_t *server;
    httpd_req_t *request;
    cp_identity_t identity;
    ae_id_t object;
    char host[129], origin[193], authorization[80], cookie[512], csrf[33], range[128], etag[100];
    cp_http_security_t security;
    uint8_t bytes[APP_REMOTE_MEDIA_READ_BYTES];
} download_t;
static bool hex_id(const char *text, uint8_t out[16])
{
    for (unsigned i = 0; i < 16; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = text[i * 2 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        if (out) {
            out[i] = (uint8_t)v;
        }
    }
    return true;
}
static bool recording_content_path(const char *path, ae_id_t *id)
{
    static const char prefix[] = "/api/v1/recordings/";
    size_t n = sizeof(prefix) - 1;
    return path && strlen(path) == n + 32 + 8 && !strncmp(path, prefix, n) &&
           !strcmp(path + n + 32, "/content") && hex_id(path + n, id ? id->bytes : NULL);
}
static bool decimal_span(const char *start, const char *end, uint64_t *out)
{
    if (start == end) {
        return false;
    }
    uint64_t value = 0;
    for (const char *p = start; p < end; ++p) {
        if (*p < '0' || *p > '9' || value > (UINT64_MAX - (unsigned)(*p - '0')) / 10) {
            return false;
        }
        value = value * 10 + (unsigned)(*p - '0');
    }
    *out = value;
    return true;
}
static bool download_range(const char *range, uint64_t total, uint64_t *start, uint64_t *end)
{
    if (!total) {
        return false;
    }
    *start = 0;
    *end = total - 1;
    if (!range[0]) {
        return true;
    }
    if (strncmp(range, "bytes=", 6) || strchr(range, ',')) {
        return false;
    }
    const char *first = range + 6, *dash = strchr(first, '-'), *last = range + strlen(range);
    if (!dash || strchr(dash + 1, '-')) {
        return false;
    }
    if (first == dash) {
        uint64_t suffix;
        if (!decimal_span(dash + 1, last, &suffix) || !suffix) {
            return false;
        }
        *start = suffix >= total ? 0 : total - suffix;
        return true;
    }
    if (!decimal_span(first, dash, start) || *start >= total) {
        return false;
    }
    if (dash + 1 != last && !decimal_span(dash + 1, last, end)) {
        return false;
    }
    if (*end >= total) {
        *end = total - 1;
    }
    return *start <= *end;
}
static cp_error_t download_auth(download_t *d)
{
    server_t *s = d->server;
    portENTER_CRITICAL(&s_lock);
    bool stop = s->stop_requested;
    portEXIT_CRITICAL(&s_lock);
    if (stop) {
        return CP_UNAVAILABLE;
    }
    if (xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return CP_BUSY;
    }
    drain_channel_closures(s);
    cp_identity_t actual;
    cp_error_t error =
        cp_transport_authenticate(s->protocol, &d->security, false, now_ms(), &actual);
    cache_sessions(s);
    xSemaphoreGive(s->mutex);
    if (error != CP_OK) {
        return error;
    }
    if ((actual.scopes & (CP_SCOPE_RECORDING_READ | CP_SCOPE_RECORDING_EXPORT)) !=
        (CP_SCOPE_RECORDING_READ | CP_SCOPE_RECORDING_EXPORT)) {
        return CP_FORBIDDEN;
    }
    if (memcmp(actual.session_id, d->identity.session_id, 16) ||
        memcmp(actual.principal_id, d->identity.principal_id, 16)) {
        return CP_UNAUTHENTICATED;
    }
    return CP_OK;
}
static void download_worker(void *argument)
{
    download_t *d = argument;
    ae_record_metadata_t metadata = {0};
    ae_result_t result = AE_AGAIN;
    uint64_t until = now_ms() + 90000, metadata_until = now_ms() + 3000;
    while (result == AE_AGAIN && now_ms() < metadata_until && download_auth(d) == CP_OK) {
        result = app_media_runtime_record_metadata(&d->identity, &d->object, &metadata);
        if (result == AE_AGAIN) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (result != AE_OK || download_auth(d) != CP_OK) {
        (void)error_response(d->request, "503 Service Unavailable",
                             "{\"error\":\"recording_unavailable\"}");
        goto done;
    }
    if (metadata.state != AE_RECORD_COMPLETED || !metadata.integrity_verified ||
        !metadata.hash_available || !metadata.sync_acknowledged || metadata.access_revoked ||
        metadata.export_policy != AE_EXPORT_OWNER_DOWNLOAD ||
        memcmp(metadata.principal_id.bytes, d->identity.principal_id, 16) ||
        metadata.pcm_bytes > UINT64_MAX - AE_WAV_HEADER_BYTES) {
        (void)error_response(d->request, "403 Forbidden", "{\"error\":\"export_forbidden\"}");
        goto done;
    }
    char digest[65];
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(digest + i * 2, 3, "%02x", metadata.sha256[i]);
    }
    char etag[100];
    snprintf(etag, sizeof(etag), "\"%" PRIu64 "-%s\"", metadata.revision, digest);
    if ((d->range[0] && !d->etag[0]) || (d->etag[0] && strcmp(d->etag, etag))) {
        (void)error_response(d->request, "412 Precondition Failed",
                             "{\"error\":\"recording_revision_changed\"}");
        goto done;
    }
    uint64_t total = metadata.pcm_bytes + AE_WAV_HEADER_BYTES, start, end;
    if (!download_range(d->range, total, &start, &end)) {
        char range[80];
        snprintf(range, sizeof(range), "bytes */%" PRIu64, total);
        httpd_resp_set_hdr(d->request, "Content-Range", range);
        (void)error_response(d->request, "416 Range Not Satisfiable",
                             "{\"error\":\"invalid_range\"}");
        goto done;
    }
    httpd_resp_set_status(d->request, d->range[0] ? "206 Partial Content" : "200 OK");
    httpd_resp_set_type(d->request, "audio/wav");
    httpd_resp_set_hdr(d->request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(d->request, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(d->request, "ETag", etag);
    httpd_resp_set_hdr(d->request, "Content-Disposition", "attachment; filename=recording.wav");
    httpd_resp_set_hdr(d->request, "X-Content-Type-Options", "nosniff");
    if (d->range[0]) {
        char range[100];
        snprintf(range, sizeof(range), "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64, start, end, total);
        httpd_resp_set_hdr(d->request, "Content-Range", range);
    }
    bool complete = true;
    for (uint64_t offset = start; offset <= end;) {
        size_t wanted = (size_t)((end - offset + 1) > sizeof(d->bytes) ? sizeof(d->bytes)
                                                                       : end - offset + 1),
               actual = 0;
        uint64_t chunk_until = now_ms() + 2000;
        result = AE_AGAIN;
        while (result == AE_AGAIN && now_ms() < chunk_until && now_ms() < until &&
               download_auth(d) == CP_OK) {
            result = app_media_runtime_content_read(&d->identity, &d->object, metadata.revision,
                                                    offset, d->bytes, wanted, &actual);
            if (result == AE_AGAIN) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (result != AE_OK || actual != wanted || download_auth(d) != CP_OK || now_ms() >= until) {
            complete = false;
            break;
        }
        esp_err_t sent = httpd_resp_send_chunk(d->request, (const char *)d->bytes, (ssize_t)actual);
        clear(d->bytes, sizeof(d->bytes));
        if (sent != ESP_OK) {
            complete = false;
            break;
        }
        offset += actual;
    }
    if (complete) {
        (void)httpd_resp_send_chunk(d->request, NULL, 0);
    } else {
        (void)httpd_sess_trigger_close(d->server->listener, httpd_req_to_sockfd(d->request));
    }
done:
    app_media_runtime_content_cancel(&d->identity, &d->object);
    (void)httpd_req_async_handler_complete(d->request);
    server_t *s = d->server;
    clear(d, sizeof(*d));
    heap_caps_free(d);
    portENTER_CRITICAL(&s_lock);
    s->download_active = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}
static esp_err_t begin_recording_download(httpd_req_t *request, server_t *s,
                                          const cp_http_security_t *security)
{
    if (xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return error_response(request, "503 Service Unavailable", "{\"error\":\"busy\"}");
    }
    drain_channel_closures(s);
    cp_identity_t identity;
    cp_error_t error = cp_transport_authenticate(s->protocol, security, false, now_ms(), &identity);
    cache_sessions(s);
    xSemaphoreGive(s->mutex);
    if (error != CP_OK) {
        return error_response(request, "401 Unauthorized", "{\"error\":\"unauthenticated\"}");
    }
    if ((identity.scopes & (CP_SCOPE_RECORDING_READ | CP_SCOPE_RECORDING_EXPORT)) !=
        (CP_SCOPE_RECORDING_READ | CP_SCOPE_RECORDING_EXPORT)) {
        return error_response(request, "403 Forbidden", "{\"error\":\"export_forbidden\"}");
    }
    download_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) {
        return error_response(request, "503 Service Unavailable",
                              "{\"error\":\"memory_unavailable\"}");
    }
    d->server = s;
    d->identity = identity;
    (void)recording_content_path(request->uri, &d->object);
    bool valid = header(request, "Range", d->range, sizeof(d->range), false) &&
                 header(request, "If-Match", d->etag, sizeof(d->etag), false);
    if (valid) {
        strcpy(d->host, security->host);
        if (security->origin) {
            strcpy(d->origin, security->origin);
        }
        if (security->authorization) {
            strcpy(d->authorization, security->authorization);
        }
        if (security->cookie) {
            strcpy(d->cookie, security->cookie);
        }
        if (security->csrf) {
            strcpy(d->csrf, security->csrf);
        }
        d->security =
            (cp_http_security_t){.tls = true,
                                 .host = d->host,
                                 .origin = d->origin[0] ? d->origin : NULL,
                                 .authorization = d->authorization[0] ? d->authorization : NULL,
                                 .cookie = d->cookie[0] ? d->cookie : NULL,
                                 .csrf = d->csrf[0] ? d->csrf : NULL};
    }
    portENTER_CRITICAL(&s_lock);
    bool admitted = valid && !s->download_active && !s->stop_requested;
    if (admitted) {
        s->download_active = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!admitted) {
        clear(d, sizeof(*d));
        heap_caps_free(d);
        return error_response(request, "503 Service Unavailable",
                              "{\"error\":\"download_busy_or_invalid_range\"}");
    }
    esp_err_t e = httpd_req_async_handler_begin(request, &d->request);
    if (e == ESP_OK &&
        xTaskCreate(download_worker, "record_download", 8192, d, 1, NULL) == pdPASS) {
        return ESP_OK;
    }
    if (d->request) {
        (void)httpd_req_async_handler_complete(d->request);
    }
    clear(d, sizeof(*d));
    heap_caps_free(d);
    portENTER_CRITICAL(&s_lock);
    s->download_active = false;
    portEXIT_CRITICAL(&s_lock);
    return error_response(request, "503 Service Unavailable",
                          "{\"error\":\"download_unavailable\"}");
}

void app_control_server_set_media_port(const app_control_media_port_t *port)
{
    portENTER_CRITICAL(&s_lock);
    s_media_port = port ? *port : (app_control_media_port_t){0};
    portEXIT_CRITICAL(&s_lock);
}
uint64_t app_control_server_channel_generation(const cp_identity_t *who)
{
    if (!who) {
        return 0;
    }
    uint64_t generation = 0;
    portENTER_CRITICAL(&s_lock);
    server_t *s = s_server;
    if (s && !s->stop_requested) {
        for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
            if (s->channels[i].used &&
                !memcmp(s->channels[i].identity.session_id, who->session_id, 16) &&
                !memcmp(s->channels[i].identity.principal_id, who->principal_id, 16)) {
                generation = s->channels[i].generation;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return generation;
}
#if CONFIG_HTTPD_WS_SUPPORT && CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
typedef struct {
    server_t *server;
    cp_identity_t identity;
    int fd;
    unsigned slot;
    uint64_t generation;
    bool closed;
} channel_t;
static void websocket_closed(channel_t *c)
{
    portENTER_CRITICAL(&s_lock);
    bool current = !c->closed && c->slot < CP_MAX_SESSIONS && c->server->channels[c->slot].used &&
                   c->server->channels[c->slot].generation == c->generation;
    c->closed = true;
    if (current) {
        c->server->channels[c->slot].used = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (current) {
        channel_closed(c->server, &c->identity);
        app_control_media_port_t port;
        portENTER_CRITICAL(&s_lock);
        port = s_media_port;
        portEXIT_CRITICAL(&s_lock);
        if (port.channel_lost) {
            port.channel_lost(port.user, &c->identity, c->generation);
        }
    }
}
static void websocket_free(void *pointer)
{
    channel_t *c = pointer;
    websocket_closed(c);
    clear(c, sizeof(*c));
    heap_caps_free(c);
}
static esp_err_t websocket_authenticate(httpd_req_t *request)
{
    server_t *s = request->user_ctx;
    char host[129], origin[193], auth[80], cookie[512], ticket[65] = {0}, protocol[128];
    bool valid = header(request, "Host", host, sizeof(host), true) &&
                 header(request, "Origin", origin, sizeof(origin), false) &&
                 header(request, "Authorization", auth, sizeof(auth), false) &&
                 header(request, "Cookie", cookie, sizeof(cookie), false) &&
                 header(request, "Sec-WebSocket-Protocol", protocol, sizeof(protocol), false);
    if (valid && origin[0]) {
        static const char browser_protocol[] = "esp32-control-v1, ticket.";
        const char *value = protocol + sizeof(browser_protocol) - 1;
        if (strncmp(protocol, browser_protocol, sizeof(browser_protocol) - 1) ||
            strlen(protocol) != sizeof(browser_protocol) - 1 + 64) {
            valid = false;
        } else {
            uint8_t decoded[16];
            valid = hex_id(value, decoded) && hex_id(value + 32, decoded);
            if (valid) {
                memcpy(ticket, value, 65);
            }
            clear(decoded, sizeof(decoded));
        }
    }
    cp_identity_t identity;
    cp_error_t error = CP_INVALID;
    if (valid && xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        drain_channel_closures(s);
        cp_http_security_t security = {.tls = true,
                                       .host = host,
                                       .origin = origin[0] ? origin : NULL,
                                       .authorization = auth[0] ? auth : NULL,
                                       .cookie = cookie[0] ? cookie : NULL};
        error = cp_transport_upgrade(s->protocol, &security, ticket[0] ? ticket : NULL, now_ms(),
                                     &identity);
        cache_sessions(s);
        xSemaphoreGive(s->mutex);
    }
    clear(auth, sizeof(auth));
    clear(cookie, sizeof(cookie));
    clear(ticket, sizeof(ticket));
    clear(protocol, sizeof(protocol));
    if (error != CP_OK) {
        return error_response(request, "401 Unauthorized",
                              "{\"error\":\"channel_authentication_required\"}");
    }
    channel_t *channel = heap_caps_calloc(1, sizeof(*channel), MALLOC_CAP_8BIT);
    if (!channel) {
        return ESP_FAIL;
    }
    unsigned slot = CP_MAX_SESSIONS;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
        if (s->channels[i].used &&
            !memcmp(s->channels[i].identity.session_id, identity.session_id, 16)) {
            slot = CP_MAX_SESSIONS;
            break;
        }
        if (!s->channels[i].used && slot == CP_MAX_SESSIONS) {
            slot = i;
        }
    }
    if (s->stop_requested || s->next_channel_generation == UINT64_MAX) {
        slot = CP_MAX_SESSIONS;
    }
    for (unsigned i = 0; i < s->channel_closure_count; ++i) {
        if (!memcmp(s->channel_closures[i].session_id, identity.session_id, 16)) {
            slot = CP_MAX_SESSIONS;
        }
    }
    if (slot < CP_MAX_SESSIONS) {
        channel->server = s;
        channel->identity = identity;
        channel->fd = httpd_req_to_sockfd(request);
        channel->slot = slot;
        channel->generation = ++s->next_channel_generation;
        s->channels[slot].generation = channel->generation;
        s->channels[slot].used = true;
        s->channels[slot].identity = identity;
        s->channels[slot].fd = channel->fd;
    }
    portEXIT_CRITICAL(&s_lock);
    if (slot == CP_MAX_SESSIONS) {
        heap_caps_free(channel);
        return error_response(request, "409 Conflict",
                              "{\"error\":\"channel_already_open_or_full\"}");
    }
    request->sess_ctx = channel;
    request->free_ctx = websocket_free;
    return ESP_OK;
}
static esp_err_t websocket_control(httpd_req_t *request, httpd_ws_frame_t *frame)
{
    channel_t *channel = request->sess_ctx;
    if (!channel) {
        return ESP_FAIL;
    }
    server_t *s = channel->server;
    if (xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_FAIL;
    }
    cp_error_t error = cp_transport_heartbeat(s->protocol, &channel->identity, now_ms());
    cache_sessions(s);
    xSemaphoreGive(s->mutex);
    if (error != CP_OK) {
        return ESP_FAIL;
    }
    if (frame->type == HTTPD_WS_TYPE_PING) {
        frame->type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(request, frame);
    }
    if (frame->type == HTTPD_WS_TYPE_CLOSE) {
        websocket_closed(channel);
        return httpd_ws_send_frame(request, frame);
    }
    return ESP_OK;
}
static esp_err_t websocket_data(httpd_req_t *request)
{
    channel_t *channel = request->sess_ctx;
    if (!channel) {
        return ESP_FAIL;
    }
    server_t *s = channel->server;
    httpd_ws_frame_t frame = {0};
    esp_err_t e = httpd_ws_recv_frame(request, &frame, 0);
    if (e != ESP_OK || !frame.final || frame.len > CP_MAX_FRAME_BYTES ||
        (frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_BINARY)) {
        return ESP_FAIL;
    }
    frame.payload = (uint8_t *)s->body;
    if (httpd_ws_recv_frame(request, &frame, CP_MAX_FRAME_BYTES) != ESP_OK) {
        return ESP_FAIL;
    }
    s->body[frame.len] = 0;
    if (xSemaphoreTake(s->mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        clear(s->body, sizeof(s->body));
        return ESP_FAIL;
    }
    drain_channel_closures(s);
    cp_error_t error;
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        error = cp_transport_frame(s->protocol, &channel->identity, s->body, frame.len, now_ms(),
                                   &s->response);
    } else {
        error = cp_transport_heartbeat(s->protocol, &channel->identity, now_ms());
        s->response = (cp_http_response_t){0};
    }
    cache_sessions(s);
    xSemaphoreGive(s->mutex);
    if (error == CP_UNAUTHENTICATED) {
        clear(s->body, sizeof(s->body));
        clear(&s->response, sizeof(s->response));
        return ESP_FAIL;
    }
    if (frame.type == HTTPD_WS_TYPE_BINARY) {
        app_control_media_port_t port;
        portENTER_CRITICAL(&s_lock);
        port = s_media_port;
        portEXIT_CRITICAL(&s_lock);
        cp_media_header_t media_header;
        if (error == CP_OK &&
            (!cp_media_decode((const uint8_t *)s->body, frame.len, &media_header) ||
             media_header.kind != CP_MEDIA_PLAYBACK)) {
            error = CP_INVALID;
        }
        if (error == CP_OK) {
            error = port.playback
                        ? port.playback(port.user, &channel->identity, channel->generation,
                                        (const uint8_t *)s->body, frame.len)
                        : CP_UNAVAILABLE;
        }
        s->response.length = (size_t)snprintf(
            s->response.body, sizeof(s->response.body), "{\"media_state\":\"%s\",\"error\":\"%s\"}",
            error == CP_OK ? "accepted" : "rejected", cp_error_name(error));
    }
    clear(s->body, sizeof(s->body));
    httpd_ws_frame_t response = {.final = true,
                                 .type = HTTPD_WS_TYPE_TEXT,
                                 .payload = (uint8_t *)s->response.body,
                                 .len = s->response.length};
    e = httpd_ws_send_frame(request, &response);
    clear(&s->response, sizeof(s->response));
    return e;
}
typedef struct {
    server_t *server;
    cp_identity_t identity;
    uint8_t job[16];
    uint64_t generation, channel_generation, expires;
    size_t bytes;
    int fd;
    bool cancelled;
    unsigned slot;
    uint8_t packet[CP_MAX_FRAME_BYTES];
} capture_packet_t;
static void capture_send_work(void *argument)
{
    capture_packet_t *packet = argument;
    server_t *s = packet->server;
    app_control_media_port_t port;
    portENTER_CRITICAL(&s_lock);
    port = s_media_port;
    bool cancelled = packet->cancelled || s->stop_requested;
    portEXIT_CRITICAL(&s_lock);
    cp_error_t error = CP_UNAVAILABLE;
    channel_t *channel = httpd_sess_get_ctx(s->listener, packet->fd);
    if (!cancelled && now_ms() < packet->expires && channel && !channel->closed &&
        channel->generation == packet->channel_generation &&
        !memcmp(channel->identity.session_id, packet->identity.session_id, 16) &&
        app_control_server_session_alive(packet->identity.session_id,
                                         packet->identity.principal_id) &&
        port.capture_allowed &&
        port.capture_allowed(port.user, &packet->identity, packet->job, packet->generation,
                             packet->channel_generation)) {
        /* Already on the HTTP owner. send_data would queue to itself and
         * wait forever. The pinned low-level call below borrows this packet
         * only during the socket send, bounded by send_wait_timeout. */
        httpd_ws_frame_t frame = {.final = true,
                                  .type = HTTPD_WS_TYPE_BINARY,
                                  .payload = packet->packet,
                                  .len = packet->bytes};
        error = httpd_ws_send_frame_async(s->listener, packet->fd, &frame) == ESP_OK
                    ? CP_OK
                    : CP_UNAVAILABLE;
    }
    if (error != CP_OK && port.capture_failed) {
        port.capture_failed(port.user, &packet->identity, packet->job, packet->generation,
                            packet->channel_generation, error);
    }
    portENTER_CRITICAL(&s_lock);
    s->media_pending[packet->slot] = NULL;
    --s->media_pending_count;
    portEXIT_CRITICAL(&s_lock);
    clear(packet, sizeof(*packet));
    heap_caps_free(packet);
}
esp_err_t app_control_server_capture_send(const cp_identity_t *identity, const uint8_t job[16],
                                          uint64_t generation, uint64_t channel_generation,
                                          const uint8_t *bytes, size_t length)
{
    if (!identity || !job || !generation || !channel_generation || !bytes || !length ||
        length > CP_MAX_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    cp_media_header_t header;
    if (!cp_media_decode(bytes, length, &header) || header.kind != CP_MEDIA_CAPTURE ||
        memcmp(header.job_id, job, 16) || header.consumer_generation != generation) {
        return ESP_ERR_INVALID_ARG;
    }
    capture_packet_t *packet =
        heap_caps_calloc(1, sizeof(*packet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    server_t *s = s_server;
    if (s && !s->stop_requested && s->listener && s_media_port.capture_allowed &&
        s_media_port.capture_failed && s->media_pending_count < 2) {
        for (unsigned i = 0; i < CP_MAX_SESSIONS; ++i) {
            if (s->channels[i].used && s->channels[i].generation == channel_generation &&
                !memcmp(identity->session_id, s->channels[i].identity.session_id, 16) &&
                !memcmp(identity->principal_id, s->channels[i].identity.principal_id, 16)) {
                unsigned slot = s->media_pending[0] ? 1 : 0;
                packet->server = s;
                packet->identity = *identity;
                memcpy(packet->job, job, 16);
                packet->generation = generation;
                packet->expires = now_ms() + 100;
                packet->fd = s->channels[i].fd;
                packet->channel_generation = s->channels[i].generation;
                packet->slot = slot;
                packet->bytes = length;
                memcpy(packet->packet, bytes, length);
                s->media_pending[slot] = packet;
                ++s->media_pending_count;
                error = ESP_OK;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (error == ESP_OK && httpd_queue_work(s->listener, capture_send_work, packet) != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s->media_pending[packet->slot] = NULL;
        --s->media_pending_count;
        portEXIT_CRITICAL(&s_lock);
        error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        clear(packet, sizeof(*packet));
        heap_caps_free(packet);
    }
    return error;
}
void app_control_server_capture_cancel(const cp_identity_t *identity, const uint8_t job[16],
                                       uint64_t generation, uint64_t channel_generation)
{
    if (!identity || !job) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    server_t *s = s_server;
    if (s) {
        for (unsigned i = 0; i < 2; ++i) {
            capture_packet_t *p = s->media_pending[i];
            if (p && p->generation == generation && p->channel_generation == channel_generation &&
                !memcmp(p->job, job, 16) &&
                !memcmp(p->identity.session_id, identity->session_id, 16) &&
                !memcmp(p->identity.principal_id, identity->principal_id, 16)) {
                p->cancelled = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
#else
esp_err_t app_control_server_capture_send(const cp_identity_t *identity, const uint8_t job[16],
                                          uint64_t generation, uint64_t channel_generation,
                                          const uint8_t *bytes, size_t length)
{
    (void)identity;
    (void)job;
    (void)generation;
    (void)channel_generation;
    (void)bytes;
    (void)length;
    return ESP_ERR_NOT_SUPPORTED;
}
void app_control_server_capture_cancel(const cp_identity_t *identity, const uint8_t job[16],
                                       uint64_t generation, uint64_t channel_generation)
{
    (void)identity;
    (void)job;
    (void)generation;
    (void)channel_generation;
}
#endif
