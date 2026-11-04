// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
enum { HTTP_GET, HTTP_POST, HTTP_DELETE };
typedef struct { const char *name, *value; } web_header_t;
typedef struct {
    const char *uri, *body;
    void *user_ctx;
    int method, status;
    size_t content_len, received, response_length;
    web_header_t headers[12];
    unsigned header_count, response_header_count;
    char response[65536], type[80];
    struct { char name[64], value[512]; } response_headers[12];
} httpd_req_t;
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
