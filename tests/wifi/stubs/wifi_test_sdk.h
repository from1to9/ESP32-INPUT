// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <net/if.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#ifndef SOC_WIFI_SUPPORTED
#define SOC_WIFI_SUPPORTED 1
#endif
#define CONFIG_ESP_WIFI_ENABLED 1
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(void (*task)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(unsigned);
void vTaskDelete(void *);
int64_t esp_timer_get_time(void);
void esp_fill_random(void *, size_t);
typedef const char *esp_event_base_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define WIFI_EVENT_STA_DISCONNECTED 4
#define WIFI_REASON_ASSOC_LEAVE 8
typedef struct { uint16_t reason; } wifi_event_sta_disconnected_t;
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *);
esp_err_t esp_event_handler_unregister(esp_event_base_t, int32_t,
    void (*handler)(void *, esp_event_base_t, int32_t, void *));
typedef struct { int id; } esp_netif_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip; } esp_netif_ip_info_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (unsigned)((p)->addr & 255U), (unsigned)(((p)->addr >> 8) & 255U), \
    (unsigned)(((p)->addr >> 16) & 255U), (unsigned)(((p)->addr >> 24) & 255U)
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *, char *);
void esp_netif_destroy_default_wifi(void *);
typedef struct { bool capable; } pmf_t;
typedef struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold; pmf_t pmf_cfg; } wifi_sta_config_t;
typedef struct { uint8_t ssid[32], password[64], ssid_len, max_connection, channel; int authmode; } wifi_ap_config_t;
typedef union { wifi_sta_config_t sta; wifi_ap_config_t ap; } wifi_config_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 3
#define WIFI_STORAGE_RAM 0
typedef struct { uint8_t ssid[33]; int8_t rssi; } wifi_ap_record_t;
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_config(int, wifi_config_t *);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef void *httpd_handle_t;
typedef struct {
    size_t content_len;
    const char *host, *origin, *content_type, *body;
    size_t offset, fragment;
    bool timeout;
} httpd_req_t;
typedef struct {
    unsigned server_port, ctrl_port, max_open_sockets, max_uri_handlers, stack_size;
    unsigned recv_wait_timeout, send_wait_timeout;
    bool lru_purge_enable;
    struct ifreq *if_name;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTP_GET 0
#define HTTP_POST 1
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
