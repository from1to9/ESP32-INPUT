#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>

#include "bootloader_random.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nvs_flash.h"
#include "passport_board.h"

static const char *TAG = "passport";
static esp_err_t display_status;
static esp_err_t button_status;
static esp_err_t nimble_status;
static atomic_bool nimble_synced;
static atomic_bool display_text_applied;
// Main-task-owned credential storage stays outside the raw task stack dump.
static wifi_config_t ap_config;
static char network_text[224];

static void prepare_ap(wifi_config_t *config)
{
    static uint8_t random[10];
    static const char hex[] = "0123456789ABCDEF";
    // Use the SAR entropy source before the button ADC or either radio starts.
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();

    config->ap.ssid_len = snprintf((char *)config->ap.ssid, sizeof(config->ap.ssid),
                                   "Passport-%02X%02X", random[0], random[1]);
    for (size_t i = 0; i < 8; ++i) {
        config->ap.password[2 * i] = hex[random[i + 2] >> 4];
        config->ap.password[2 * i + 1] = hex[random[i + 2] & 15];
    }
    config->ap.password[16] = '\0';
    explicit_bzero(random, sizeof(random));
    config->ap.channel = 1;
    config->ap.authmode = WIFI_AUTH_WPA2_PSK;
    config->ap.max_connection = 1;
}

static esp_err_t create_wifi_netif(wifi_interface_t interface, esp_netif_t **netif)
{
    const esp_netif_config_t sta = ESP_NETIF_DEFAULT_WIFI_STA();
    const esp_netif_config_t ap = ESP_NETIF_DEFAULT_WIFI_AP();
    *netif = esp_netif_new(interface == WIFI_IF_STA ? &sta : &ap);
    if (!*netif) {
        return ESP_FAIL;
    }
    esp_err_t err = interface == WIFI_IF_STA ? esp_netif_attach_wifi_station(*netif)
                                            : esp_netif_attach_wifi_ap(*netif);
    if (err == ESP_OK) {
        err = interface == WIFI_IF_STA ? esp_wifi_set_default_wifi_sta_handlers()
                                      : esp_wifi_set_default_wifi_ap_handlers();
    }
    if (err != ESP_OK) {
        // The SDK records the interface before attach, even when attach fails.
        esp_netif_destroy_default_wifi(*netif);
        *netif = NULL;
    }
    return err;
}

static esp_err_t start_wifi(wifi_config_t *ap, esp_netif_ip_info_t *ip)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network interface initialization");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Default event loop creation");

    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&config);
    if (ret != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_delete_default());
        return ret;
    }
    esp_netif_t *sta_netif = NULL;
    esp_netif_t *ap_netif = NULL;

    ESP_GOTO_ON_ERROR(create_wifi_netif(WIFI_IF_STA, &sta_netif), fail, TAG, "Create STA interface");
    ESP_GOTO_ON_ERROR(create_wifi_netif(WIFI_IF_AP, &ap_netif), fail, TAG, "Create AP interface");
    ESP_GOTO_ON_ERROR(esp_netif_get_ip_info(ap_netif, ip), fail, TAG, "Read AP address");
    ESP_GOTO_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), fail, TAG, "Set Wi-Fi storage");
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), fail, TAG, "Set APSTA mode");
    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, ap), fail, TAG, "Configure protected AP");
    ESP_GOTO_ON_ERROR(esp_wifi_start(), fail, TAG, "Start Wi-Fi");
    return ESP_OK;

fail:
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    esp_netif_destroy_default_wifi(ap_netif);
    esp_netif_destroy_default_wifi(sta_netif);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_deinit());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_delete_default());
    return ret;
}

static void on_nimble_sync(void)
{
    atomic_store(&nimble_synced, true);
}

static void on_nimble_reset(int reason)
{
    atomic_store(&nimble_synced, false);
    ESP_LOGW(TAG, "NimBLE host reset (%d)", reason);
}

static void run_nimble(void *arg)
{
    (void)arg;
    nimble_port_run();
    atomic_store(&nimble_synced, false);
    vTaskDelete(NULL);
}

static esp_err_t start_nimble(void)
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "NimBLE initialization");
    ble_hs_cfg.sync_cb = on_nimble_sync;
    ble_hs_cfg.reset_cb = on_nimble_reset;
    // Use the SDK host stack and priority, with an observable task creation result.
    if (xTaskCreate(run_nimble, "nimble_host", NIMBLE_HS_STACK_SIZE,
                    NULL, configMAX_PRIORITIES - 4, NULL) != pdPASS) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nimble_port_deinit());
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static const char *nimble_text(bool synced)
{
    if (nimble_status != ESP_OK) {
        return esp_err_to_name(nimble_status);
    }
    return synced ? "Host synchronized" : "Waiting for host sync";
}

static esp_err_t health_get(httpd_req_t *request)
{
    const bool synced = atomic_load(&nimble_synced);
    const unsigned observed = passport_buttons_observed();
    const bool applied = atomic_load(&display_text_applied);
    if (display_status != ESP_OK || !applied || button_status != ESP_OK ||
        observed != PASSPORT_ALL_BUTTONS || !synced) {
        httpd_resp_set_status(request, "503 Service Unavailable");
    }
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    // HTTP server serializes handlers on its task. This response contains no credentials.
    static char body[1024];
    snprintf(body, sizeof(body),
             "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
             "<title>Passport C3 health</title><h1>Passport C3 health</h1>"
             "<ul><li>Display initialization (240 &times; 320) &mdash; %s</li>"
             "<li>Status text &mdash; %s; physical display unverified</li>"
             "<li>Button setup call &mdash; %s</li>"
             "<li>Presses observed since boot &mdash; UP %s, DOWN %s, OK %s</li>"
             "<li>NimBLE &mdash; %s</li>"
             "<li>Wi-Fi &mdash; AP configured, STA idle; DHCP and association unverified</li>"
             "<li>Internal heap &mdash; %zu bytes free, %zu bytes largest block</li></ul></html>",
             esp_err_to_name(display_status), applied ? "Applied to LVGL" : "Update pending",
             esp_err_to_name(button_status),
             observed & PASSPORT_BUTTON_UP ? "yes" : "pending",
             observed & PASSPORT_BUTTON_DOWN ? "yes" : "pending",
             observed & PASSPORT_BUTTON_OK ? "yes" : "pending", nimble_text(synced),
             heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps));

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t start_health_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 2;
    config.max_uri_handlers = 1;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server start");
    const httpd_uri_t health = { .uri = "/health", .method = HTTP_GET, .handler = health_get };
    esp_err_t err = httpd_register_uri_handler(server, &health);
    if (err != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_stop(server));
    }
    return err;
}

void app_main(void)
{
    prepare_ap(&ap_config);

    display_status = passport_display_init();
    ESP_ERROR_CHECK_WITHOUT_ABORT(display_status);
    button_status = passport_buttons_init();
    ESP_ERROR_CHECK_WITHOUT_ABORT(button_status);

    esp_netif_ip_info_t ip;
    esp_err_t wifi_status = nvs_flash_init();
    nimble_status = wifi_status;
    if (wifi_status == ESP_OK) {
        nimble_status = start_nimble();
        wifi_status = start_wifi(&ap_config, &ip);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nimble_status);
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_status);

    const char *http_status = "Unavailable";
    if (wifi_status == ESP_OK) {
        const esp_err_t http = start_health_server();
        ESP_ERROR_CHECK_WITHOUT_ABORT(http);
        http_status = http == ESP_OK ? "Listener created" : esp_err_to_name(http);
        snprintf(network_text, sizeof(network_text),
                 "AP configured / STA idle\nDHCP unverified\nSSID: %s\nPassword: %s\n"
                 "Configured URL\nhttp://" IPSTR "/health",
                 (char *)ap_config.ap.ssid, (char *)ap_config.ap.password, IP2STR(&ip.ip));
    } else {
        snprintf(network_text, sizeof(network_text), "Wi-Fi: %s", esp_err_to_name(wifi_status));
    }
    explicit_bzero(&ap_config, sizeof(ap_config));

    bool synced = atomic_load(&nimble_synced);
    bool update_pending = true;
    for (;;) {
        if (update_pending) {
            const bool applied = passport_show_status(network_text, http_status,
                                                       nimble_text(synced), button_status);
            atomic_store(&display_text_applied, applied);
            update_pending = !applied;
        }
        const bool pressed = passport_wait_for_button();
        const bool now_synced = atomic_load(&nimble_synced);
        if (pressed || now_synced != synced) {
            synced = now_synced;
            update_pending = true;
            atomic_store(&display_text_applied, false);
        }
    }
}
