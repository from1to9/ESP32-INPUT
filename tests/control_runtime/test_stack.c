// SPDX-License-Identifier: Apache-2.0
/* Exact server owner functions are inserted by run_stack.py. Only the SDK
 * listener/task/clock ports and unrelated route and channel callbacks are replaced. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include "server_test_sdk.h"
#include "../../firmware/main/app_control_server.h"
#include "../../firmware/main/app_network_task.h"
#include "weak_heap.h"
typedef struct {
    unsigned marker;
} esp_netif_t;
typedef struct {
    struct {
        uint32_t addr;
    } ip;
} esp_netif_ip_info_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
    bool is_websocket, handle_ws_control_frames;
    const char *supported_subprotocol;
    esp_err_t (*ws_pre_handshake_cb)(httpd_req_t *);
    esp_err_t (*ws_control_handler)(httpd_req_t *, httpd_ws_frame_t *);
} httpd_uri_t;
typedef struct {
    unsigned stack_size, task_priority, task_caps, max_open_sockets, max_uri_handlers, backlog_conn;
    unsigned recv_wait_timeout, send_wait_timeout, ctrl_port, max_req_hdr_len;
    bool lru_purge_enable;
    struct ifreq *if_name;
    bool (*uri_match_fn)(const char *, const char *, size_t);
} test_httpd_config_t;
typedef struct {
    test_httpd_config_t httpd;
    unsigned transport_mode, port_secure;
    const uint8_t *servercert, *prvtkey_pem;
    size_t servercert_len, prvtkey_len;
} httpd_ssl_config_t;
#define HTTPD_SSL_CONFIG_DEFAULT()                                                                 \
    {                                                                                              \
        .httpd = {.stack_size = 4096, .task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT }         \
    }
#define HTTPD_SSL_TRANSPORT_SECURE 1U
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTP_DELETE 2
/* TYPES */
/* PORTS */
static portMUX_TYPE s_lock;
static server_t *s_server;
static app_control_server_snapshot_t s_snapshot;
static cp_session_view_t s_sessions[CP_MAX_SESSIONS];
static size_t s_session_count;
static unsigned critical, creates, caps_creates, legacy_creates, caps_deletes, legacy_deletes;
static unsigned live, cleared, http_starts, http_stops, mutex_deletes, delays;
static bool fail_create, fail_listen, fail_stop_once;
static TaskFunction_t task_entry;
static void *task_argument;
static jmp_buf exited;
static struct {
    void *pointer;
    size_t bytes;
} allocations[8];
void test_enter(void) { ++critical; }
void test_leave(void)
{
    assert(critical);
    --critical;
}
static uint64_t now_ms(void) { return 100; }
static void clear(void *pointer, size_t bytes) { memset(pointer, 0, bytes); }
void *heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, unsigned caps)
{
    assert(!critical && (caps & MALLOC_CAP_8BIT));
    void *pointer = test_heap_calloc(alignment, count, size, caps);
    if (!pointer) return NULL;
    unsigned i = 0;
    while (i < 8 && allocations[i].pointer) {
        ++i;
    }
    assert(i < 8);
    allocations[i].pointer = pointer;
    allocations[i].bytes = count * size;
    ++live;
    return pointer;
}
void heap_caps_free(void *pointer)
{
    unsigned i = 0;
    while (i < 8 && allocations[i].pointer != pointer) {
        ++i;
    }
    assert(i < 8 && live);
    const uint8_t *bytes = pointer;
    for (size_t n = 0; n < allocations[i].bytes; ++n) {
        assert(!bytes[n]);
    }
    ++cleared;
    --live;
    allocations[i].pointer = NULL;
    test_heap_free(pointer);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    (void)timeout;
    assert(sem);
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t sem) { assert(sem); }
void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    assert(sem);
    ++mutex_deletes;
}
static int create_owner(TaskFunction_t entry, const char *name, unsigned stack, void *argument,
                        unsigned priority, void *handle)
{
    assert(!critical && !strcmp(name, "control_server") && stack == 4096 && priority == 1 &&
           !handle);
    ++creates;
    if (fail_create) {
        return 0;
    }
    task_entry = entry;
    task_argument = argument;
    return pdPASS;
}
int xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack, void *argument,
                unsigned priority, void *handle)
{
    assert(!APP_NETWORK_EXTERNAL_STACK);
    ++legacy_creates;
    return create_owner(entry, name, stack, argument, priority, handle);
}
int xTaskCreateWithCaps(TaskFunction_t entry, const char *name, unsigned stack, void *argument,
                        unsigned priority, void *handle, unsigned caps)
{
    assert(APP_NETWORK_EXTERNAL_STACK && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++caps_creates;
    return create_owner(entry, name, stack, argument, priority, handle);
}
void vTaskDelete(void *handle)
{
    assert(!APP_NETWORK_EXTERNAL_STACK && !handle && !critical);
    ++legacy_deletes;
    longjmp(exited, 1);
}
void vTaskDeleteWithCaps(void *handle)
{
    assert(APP_NETWORK_EXTERNAL_STACK && !handle && !critical);
    ++caps_deletes;
    longjmp(exited, 1);
}
void vTaskDelay(TickType_t delay)
{
    assert(delay == 1000 && !critical && live == 2 && s_server && s_server->listener);
    ++delays; /* Failed stop retains both resources until the next actual ACK. */
}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    static esp_netif_t station;
    assert(!strcmp(key, "WIFI_STA_DEF"));
    return &station;
}
static esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *station, char *name)
{
    assert(station);
    strcpy(name, "st0");
    return ESP_OK;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t *station, esp_netif_ip_info_t *ip)
{
    assert(station && ip);
    ip->ip.addr = 1;
    return ESP_OK;
}
static bool httpd_uri_match_wildcard(const char *a, const char *b, size_t length)
{
    (void)a;
    (void)b;
    (void)length;
    return false;
}
static esp_err_t httpd_ssl_start(httpd_handle_t *listener, httpd_ssl_config_t *config)
{
    assert(config->httpd.stack_size == 12288 && config->httpd.task_caps == APP_NETWORK_STACK_CAPS);
    assert(config->httpd.max_req_hdr_len == 3072);
    assert(config->httpd.max_open_sockets == 4 &&
           config->transport_mode == HTTPD_SSL_TRANSPORT_SECURE);
    assert(config->servercert_len && config->prvtkey_len);
    ++http_starts;
    if (fail_listen) {
        return ESP_ERR_NO_MEM;
    }
    *listener = (void *)2;
    return ESP_OK;
}
static esp_err_t httpd_ssl_stop(httpd_handle_t listener)
{
    assert(listener == (void *)2 && live == 2);
    ++http_stops;
    if (fail_stop_once) {
        fail_stop_once = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t httpd_register_uri_handler(httpd_handle_t listener, const httpd_uri_t *route)
{
    assert(listener && route->handler && route->user_ctx);
    return ESP_OK;
}
static esp_err_t handle_request(httpd_req_t *request)
{
    (void)request;
    return ESP_FAIL;
}
static esp_err_t handle_web(httpd_req_t *request)
{
    (void)request;
    return ESP_FAIL;
}
static esp_err_t websocket_authenticate(httpd_req_t *request)
{
    (void)request;
    return ESP_FAIL;
}
static esp_err_t websocket_data(httpd_req_t *request)
{
    (void)request;
    return ESP_FAIL;
}
static esp_err_t websocket_control(httpd_req_t *request, httpd_ws_frame_t *frame)
{
    (void)request;
    (void)frame;
    return ESP_FAIL;
}
static void drain_channel_closures(server_t *server) { assert(!server->channel_closure_count); }
static bool random_source(void *context, uint8_t *bytes, size_t count)
{
    (void)context;
    memset(bytes, 1, count);
    return true;
}
static bool available(void *context, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{
    (void)context;
    (void)op;
    (void)reason;
    return false;
}
static cp_error_t dispatch(void *context, const cp_command_t *command, cp_result_t *result)
{
    (void)context;
    (void)command;
    (void)result;
    return CP_UNAVAILABLE;
}
static void safety(void *context, cp_safety_reason_t reason, const cp_identity_t *who)
{
    (void)context;
    (void)reason;
    (void)who;
}
static cp_error_t read_state(void *context, const cp_identity_t *who, const char *resource,
                             char *json, size_t capacity, size_t *length)
{
    (void)context;
    (void)who;
    (void)resource;
    (void)json;
    (void)capacity;
    (void)length;
    return CP_UNAVAILABLE;
}
/* OWNER */
/* TESTS */
static void setup(void)
{
    assert(!live && !s_server && !critical);
    assert(!test_heap_live);
    test_heap_fail_external = false;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    creates = caps_creates = legacy_creates = caps_deletes = legacy_deletes = 0;
    cleared = http_starts = http_stops = mutex_deletes = delays = 0;
    fail_create = fail_listen = fail_stop_once = false;
    task_entry = NULL;
    task_argument = NULL;
}
static esp_err_t start(void)
{
    static const uint8_t cert[] = "CERT", key[] = "KEY";
    cp_principal_t principal = {.enabled = true, .native_only = true, .scopes = CP_SCOPE_VIEW};
    memset(principal.id, 1, sizeof(principal.id));
    memset(principal.credential_sha256, 2, 32);
    app_control_server_config_t config = {.abi_version = APP_CONTROL_SERVER_ABI,
                                          .station_ifkey = "WIFI_STA_DEF",
                                          .allowed_host = "device.test",
                                          .browser_origin = "https://device.test",
                                          .port = 443,
                                          .certificate_pem = cert,
                                          .certificate_length = sizeof(cert),
                                          .private_key_pem = key,
                                          .private_key_length = sizeof(key),
                                          .principals = &principal,
                                          .principal_count = 1};
    return app_control_server_start(&config);
}
static void run_stopped_owner(unsigned expected_cleared)
{
    assert(task_entry && s_server == task_argument);
    s_server->stop_requested = true;
    if (setjmp(exited) == 0) {
        task_entry(task_argument);
    }
    assert(!s_server && !live && !critical && cleared == expected_cleared && mutex_deletes == 1);
    assert((APP_NETWORK_EXTERNAL_STACK ? caps_deletes : legacy_deletes) == 1 &&
           (APP_NETWORK_EXTERNAL_STACK ? legacy_deletes : caps_deletes) == 0);
}
int main(void)
{
    setup();
    test_heap_fail_next = 2;
    assert(start() == ESP_ERR_NO_MEM && !s_server && !live && !test_heap_live && !task_entry);
    setup();
    test_heap_fail_external = true;
    assert(start() == ESP_OK);
    run_stopped_owner(2);
    assert(test_heap_external_failures >= 2);
    setup();
    assert(start() == ESP_OK);
    test_heap_fail_next = 2; /* Both protocol storage capability attempts fail. */
    run_stopped_owner(1);
    assert(!http_starts && s_snapshot.state == APP_CONTROL_ERROR &&
           s_snapshot.error == ESP_ERR_NO_MEM);
    setup();
    fail_create = true;
    assert(start() == ESP_ERR_NO_MEM && !s_server && !live && cleared == 1 && !task_entry);
    assert(creates == 1 && (APP_NETWORK_EXTERNAL_STACK ? caps_creates : legacy_creates) == 1 &&
           (APP_NETWORK_EXTERNAL_STACK ? legacy_creates : caps_creates) == 0);
    setup();
    assert(start() == ESP_OK);
    run_stopped_owner(2);
    assert(http_starts == 1 && http_stops == 1);
    setup();
    fail_stop_once = true;
    assert(start() == ESP_OK);
    run_stopped_owner(2);
    assert(http_stops == 2 && delays == 1 && s_snapshot.state == APP_CONTROL_STOPPED);
    setup();
    fail_listen = true;
    assert(start() == ESP_OK);
    run_stopped_owner(2);
    assert(http_starts == 1 && !http_stops && s_snapshot.state == APP_CONTROL_ERROR &&
           s_snapshot.error == ESP_ERR_NO_MEM);
    puts("PASS: actual server owner/HTTPS stack caps, no allocation fallback, matched deletion, "
         "secret cleanup and retained stop retry");
    return 0;
}
