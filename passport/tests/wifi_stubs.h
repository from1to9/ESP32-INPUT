#pragma once
#include "stubs.h"
#include "theme_stubs.h"
#include <string.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef const char *esp_event_base_t;
static const char wifi_event_base[] = "wifi", ip_event_base[] = "ip";
#define WIFI_EVENT wifi_event_base
#define IP_EVENT ip_event_base
#define ESP_EVENT_ANY_ID -1
typedef void *esp_event_handler_instance_t;
typedef int wifi_auth_mode_t;
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA_WPA2_PSK 4
#define WIFI_AUTH_MAX 16
#define WIFI_MODE_STA 1
#define WIFI_SCAN_TYPE_ACTIVE 0
#define ESP_NETIF_DHCP_STARTED 1
typedef int esp_netif_dhcp_status_t;
enum { WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP, WIFI_EVENT_AP_START, WIFI_EVENT_AP_STOP,
       WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_AP_STACONNECTED,
       WIFI_EVENT_AP_STADISCONNECTED, WIFI_EVENT_SCAN_DONE };
enum { IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP, IP_EVENT_AP_STAIPASSIGNED };
typedef struct { uint8_t ssid[32], ssid_len, bssid[6], channel; int authmode; } wifi_event_sta_connected_t;
typedef struct { uint8_t ssid[32], ssid_len, bssid[6], reason; } wifi_event_sta_disconnected_t;
typedef struct { uint32_t status; uint8_t number, scan_id; } wifi_event_sta_scan_done_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { esp_netif_t *esp_netif; esp_ip4_addr_t ip; } ip_event_ap_staipassigned_t;
typedef struct { uint8_t ssid[33], bssid[6], primary; int8_t rssi; int authmode; } wifi_ap_record_t;
typedef struct { bool show_hidden; int scan_type; struct { struct { unsigned min, max; } active; } scan_time; unsigned home_chan_dwell_time; } wifi_scan_config_t;
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, void (*)(void *, esp_event_base_t, int32_t, void *), void *, esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t, esp_event_handler_instance_t);
esp_err_t esp_netif_dhcps_get_status(esp_netif_t *, esp_netif_dhcp_status_t *);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *, bool);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *, wifi_ap_record_t *);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
uint32_t esp_random(void);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
