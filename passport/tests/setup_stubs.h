#pragma once
#include "wifi_stubs.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#define HTTP_POST 1
#define ESP_ERR_WIFI_NOT_CONNECT 40
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_to_sockfd(httpd_req_t *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
int setup_getsockname(int, struct sockaddr *, socklen_t *);
#define getsockname setup_getsockname
