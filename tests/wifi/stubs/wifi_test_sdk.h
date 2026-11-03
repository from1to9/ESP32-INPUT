// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <net/if.h>
typedef int esp_err_t;
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_WIFI_STATE 0x3006
#ifndef SOC_WIFI_SUPPORTED
#define SOC_WIFI_SUPPORTED 1
#endif
#define CONFIG_ESP_WIFI_ENABLED 1
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
void wifi_test_enter(void);
void wifi_test_exit(void);
#define portENTER_CRITICAL(p) ((void)(p), wifi_test_enter())
#define portEXIT_CRITICAL(p) ((void)(p), wifi_test_exit())
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(void (*task)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(unsigned);
void vTaskDelete(void *);
int64_t esp_timer_get_time(void);
void esp_fill_random(void *, size_t);
typedef const char *esp_event_base_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define IP_EVENT ((esp_event_base_t)2)
#define IP_EVENT_STA_LOST_IP 5
#define WIFI_EVENT_STA_DISCONNECTED 4
#define WIFI_EVENT_SCAN_DONE 1
#define WIFI_REASON_ASSOC_LEAVE 8
typedef struct { uint16_t reason; } wifi_event_sta_disconnected_t;
typedef struct { uint32_t status; uint8_t number, scan_id; } wifi_event_sta_scan_done_t;
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
typedef esp_err_t (*esp_netif_callback_fn)(void *);
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn, void *);
void dns_clear_cache(void);
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
enum {
    WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_ENTERPRISE, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WAPI_PSK, WIFI_AUTH_OWE,
    WIFI_AUTH_WPA3_ENT_192, WIFI_AUTH_DUMMY_1, WIFI_AUTH_DUMMY_2, WIFI_AUTH_DPP,
    WIFI_AUTH_WPA3_ENTERPRISE, WIFI_AUTH_WPA2_WPA3_ENTERPRISE, WIFI_AUTH_WPA_ENTERPRISE
};
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 3
#define WIFI_STORAGE_RAM 0
typedef struct { uint8_t ssid[33], bssid[6], primary; int8_t rssi; int authmode; } wifi_ap_record_t;
typedef struct {
    bool show_hidden;
    int scan_type;
    struct { struct { uint32_t min, max; } active; uint32_t passive; } scan_time;
    uint8_t home_chan_dwell_time;
    bool coex_background_scan;
    uint8_t max_scan_ap_num;
} wifi_scan_config_t;
#define WIFI_SCAN_TYPE_ACTIVE 0
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *, bool);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *);
esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *);
esp_err_t esp_wifi_clear_ap_list(void);
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
