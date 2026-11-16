#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { \
    (void)(tag); esp_err_t result = (call); if (result != ESP_OK) return result; \
} while (0)
#define ESP_GOTO_ON_ERROR(call, label, tag, ...) do { \
    (void)(tag); ret = (call); if (ret != ESP_OK) goto label; \
} while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(call) ((void)(call))
#define ESP_LOGW(tag, format, ...) ((void)(tag), (void)(format), (void)(__VA_ARGS__))
#define ESP_LOGI(tag, format, ...) ((void)(tag), (void)(format), (void)(__VA_ARGS__))
const char *esp_err_to_name(esp_err_t err);
void explicit_bzero(void *buffer, size_t size);
void bootloader_random_enable(void);
void bootloader_random_disable(void);
void esp_fill_random(void *buffer, size_t size);

typedef struct { int interface; bool live; } esp_netif_t;
typedef struct { int interface; } esp_netif_config_t;
typedef struct { unsigned addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef int wifi_interface_t;
typedef struct { int unused; } wifi_init_config_t;
typedef struct {
    struct {
        uint8_t ssid[32]; uint8_t password[64];
        int ssid_len, channel, authmode, max_connection;
    } ap;
    struct { uint8_t ssid[32], password[64], bssid[6]; bool bssid_set; struct { int authmode; } threshold; struct { bool capable; } pmf_cfg; } sta;
} wifi_config_t;
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_APSTA 3
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define ESP_NETIF_DEFAULT_WIFI_STA() {WIFI_IF_STA}
#define ESP_NETIF_DEFAULT_WIFI_AP() {WIFI_IF_AP}
#define IPSTR "%u"
#define IP2STR(ip) ((ip)->addr)
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_loop_delete_default(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *netif);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void);
void esp_netif_destroy_default_wifi(void *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t nvs_flash_init(void);

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
typedef struct { const char *status; const char *body; size_t content_len; const char *uri; int method; void *sess_ctx; void (*free_ctx)(void *); void *aux; } httpd_req_t;
typedef void *httpd_handle_t;
typedef struct { int max_open_sockets, max_uri_handlers, recv_wait_timeout, send_wait_timeout; bool lru_purge_enable; bool (*uri_match_fn)(const char *,const char *,size_t); } httpd_config_t;
typedef struct {
    const char *uri; int method; esp_err_t (*handler)(httpd_req_t *);
    bool is_websocket, handle_ws_control_frames;
    const char *supported_subprotocol;
    esp_err_t (*ws_pre_handshake_cb)(httpd_req_t *);
} httpd_uri_t;
#define HTTPD_DEFAULT_CONFIG() {.max_open_sockets=7,.max_uri_handlers=8,.recv_wait_timeout=5,.send_wait_timeout=5}
#define HTTP_GET 0
bool httpd_uri_match_wildcard(const char *,const char *,size_t);
esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri);
esp_err_t httpd_stop(httpd_handle_t server);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value);
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *body);

#define NIMBLE_HS_STACK_SIZE 4096
#define configMAX_PRIORITIES 25
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef unsigned TickType_t;
typedef void *QueueHandle_t;
int xTaskCreate(void (*run)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle);
void vTaskDelete(void *task);
void vTaskDelay(TickType_t ticks);
esp_err_t nimble_port_init(void);
esp_err_t nimble_port_deinit(void);
void nimble_port_run(void);
QueueHandle_t xQueueCreate(unsigned length, unsigned size);
void vQueueDelete(QueueHandle_t queue);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
int xQueueReset(QueueHandle_t queue);
int64_t esp_timer_get_time(void);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);

typedef enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum { BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG, BSP_BTN_RELEASE } bsp_btn_ev_t;
typedef void (*bsp_btn_cb_t)(bsp_btn_t, bsp_btn_ev_t, void *);
#define BSP_LCD_W 240
#define LV_ALIGN_TOP_MID 1
typedef struct { const char *text; } lv_obj_t;
typedef struct { int unused; } lv_display_t;
esp_err_t bsp_display_init(void);
lv_display_t *bsp_lvgl_init(void);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
void bsp_display_backlight(uint8_t percent);
esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user);
lv_obj_t *lv_display_get_screen_active(lv_display_t *display);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_set_width(lv_obj_t *object, int width);
void lv_obj_align(lv_obj_t *object, int align, int x, int y);
void lv_label_set_text_static(lv_obj_t *object, const char *text);
