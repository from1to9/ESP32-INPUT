// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_test_sdk.h"
#include "control_protocol.h"
#include "control_registry.h"
#define CONFIG_HTTPD_WS_SUPPORT 1
#define CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT 1
typedef void *httpd_handle_t;
typedef struct {
    const char *name, *value;
} test_header_t;
typedef struct {
    const char *uri;
    void *user_ctx, *sess_ctx;
    void (*free_ctx)(void *);
    int fd, status;
    test_header_t headers[12];
    unsigned header_count;
    char response[18000];
    size_t received;
    bool terminated, closed, completed;
    char content_range[128], etag[128];
} httpd_req_t;
typedef enum {
    HTTPD_WS_TYPE_TEXT = 1,
    HTTPD_WS_TYPE_BINARY = 2,
    HTTPD_WS_TYPE_CLOSE = 8,
    HTTPD_WS_TYPE_PING = 9,
    HTTPD_WS_TYPE_PONG = 10
} httpd_ws_type_t;
typedef struct {
    bool final, fragmented;
    httpd_ws_type_t type;
    uint8_t *payload;
    size_t len, left_len;
} httpd_ws_frame_t;
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *, httpd_req_t **);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *);
int httpd_req_to_sockfd(httpd_req_t *);
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int);
void *httpd_sess_get_ctx(httpd_handle_t, int);
esp_err_t httpd_ws_recv_frame(httpd_req_t *, httpd_ws_frame_t *, size_t);
esp_err_t httpd_ws_send_frame(httpd_req_t *, httpd_ws_frame_t *);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int, httpd_ws_frame_t *);
esp_err_t httpd_ws_send_data(httpd_handle_t, int, httpd_ws_frame_t *);
esp_err_t httpd_queue_work(httpd_handle_t, void (*)(void *), void *);
void vTaskDelete(void *);
