// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "setup_stubs.h"
#define HTTP_DELETE 2
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
TickType_t xTaskGetTickCount(void);
typedef enum { HTTPD_WS_TYPE_CONTINUE, HTTPD_WS_TYPE_TEXT, HTTPD_WS_TYPE_BINARY,
               HTTPD_WS_TYPE_CLOSE=8, HTTPD_WS_TYPE_PING, HTTPD_WS_TYPE_PONG } httpd_ws_type_t;
typedef struct { bool final,fragmented; httpd_ws_type_t type; uint8_t *payload; size_t len; } httpd_ws_frame_t;
esp_err_t httpd_ws_recv_frame(httpd_req_t *,httpd_ws_frame_t *,size_t);
esp_err_t httpd_ws_send_frame(httpd_req_t *,httpd_ws_frame_t *);
void *httpd_sess_get_ctx(httpd_handle_t, int);
esp_err_t httpd_queue_work(httpd_handle_t, void (*)(void *), void *);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int, httpd_ws_frame_t *);
