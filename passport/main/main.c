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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "passport_ble.h"
#include "passport_ui.h"
#include "passport_wifi.h"
#include "passport_setup.h"
#include "passport_control_http.h"
#include "nvs_flash.h"
#include "passport_board.h"

static const char *TAG = "passport";
static esp_err_t display_status;
static esp_err_t button_status;
static esp_err_t nimble_status;

#if CONFIG_PASSPORT_DIAGNOSTICS
static struct { const char *stage; esp_err_t status; size_t free, largest; } boot_stages[4];
static atomic_uint boot_stage_count;
static esp_timer_handle_t boot_replay;

static void replay_boot(void *arg)
{
    (void)arg;
    // A separate timer task can report retained stages even if the UI asserts.
    unsigned count = atomic_load_explicit(&boot_stage_count, memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "Boot replay %s: status=%s free=%zu largest=%zu", boot_stages[i].stage,
                 esp_err_to_name(boot_stages[i].status), boot_stages[i].free, boot_stages[i].largest);
    }
    passport_ble_state_t ble;
    passport_ble_snapshot(&ble);
    ESP_LOGI(TAG, "Boot replay owner: synced=%u control=%u storage=%u peers=%u ready=%u",
             ble.synced, ble.network_available, ble.storage_known, ble.peer_count, ble.ready);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_delete(boot_replay));
    boot_replay = NULL;
}
#endif

static void startup_heap(const char *stage, esp_err_t status)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t free = heap_caps_get_free_size(caps), largest = heap_caps_get_largest_free_block(caps);
    unsigned count = atomic_load_explicit(&boot_stage_count, memory_order_relaxed);
    if (count < sizeof(boot_stages) / sizeof(boot_stages[0])) {
        boot_stages[count].stage = stage; boot_stages[count].status = status;
        boot_stages[count].free = free; boot_stages[count].largest = largest;
        atomic_store_explicit(&boot_stage_count, count + 1, memory_order_release);
    }
    ESP_LOGI(TAG, "Heap after %s: free=%zu largest=%zu", stage,
             free, largest);
#else
    (void)stage; (void)status;
#endif
}

static esp_err_t health_get(httpd_req_t *request)
{
    passport_ble_state_t ble;
    passport_ble_snapshot(&ble);
    const bool synced = ble.synced;
    const unsigned observed = passport_buttons_observed();
    const bool applied = passport_ui_text_applied();
    if (display_status != ESP_OK || !applied || button_status != ESP_OK || !synced) {
        httpd_resp_set_status(request, "503 Service Unavailable");
    }
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    // HTTP server serializes handlers on its task. This response contains no credentials.
    static char body[1536];
    static passport_wifi_link_t wifi;
    passport_wifi_link_snapshot(&wifi);
    snprintf(body, sizeof(body),
             "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
             "<title>Passport C3 health</title><h1>Passport C3 health</h1>"
             "<ul><li>Display initialization (240 &times; 320) &mdash; %s</li>"
             "<li>Status text &mdash; %s; physical display unverified</li>"
             "<li>Button setup call &mdash; %s</li>"
             "<li>Presses observed since boot &mdash; UP %s, DOWN %s, OK %s</li>"
             "<li>NimBLE &mdash; %s</li>"
             "<li>Wi-Fi STA &mdash; %s; STA IP %s; AP %s; DHCP server %s; AP client lease %s</li>"
             "<li>Internal heap &mdash; %zu bytes free, %zu bytes largest block</li></ul></html>",
             esp_err_to_name(display_status), applied ? "Applied to LVGL" : "Update pending",
             esp_err_to_name(button_status),
             observed & PASSPORT_BUTTON_UP ? "yes" : "pending",
             observed & PASSPORT_BUTTON_DOWN ? "yes" : "pending",
             observed & PASSPORT_BUTTON_OK ? "yes" : "pending", synced ? "Host synchronized" :
             (nimble_status == ESP_OK ? "Synchronization pending" : esp_err_to_name(nimble_status)),
             passport_wifi_phase_name(wifi.phase), wifi.sta_ip[0] ? wifi.sta_ip : "none",
             wifi.ap_started ? "started" : "not started", wifi.dhcp_running ? "running" : "unavailable",
             wifi.ap_lease ? "assigned" : "not observed",
             heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps));

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t start_health_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // HTTP needs byte-addressable internal RAM, not Wi-Fi's DMA-capable DRAM.
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_RTCRAM;
    config.max_open_sockets = 3; // HTML, CSS and JS may be fetched concurrently.
    config.lru_purge_enable = true;
    config.max_uri_handlers = 15; // Health + setup (6) + API/WS (4) + controller assets (4).
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server start");
    const httpd_uri_t health = { .uri = "/health", .method = HTTP_GET, .handler = health_get };
    esp_err_t err = httpd_register_uri_handler(server, &health);
    if (err == ESP_OK) err = passport_setup_register(server);
    if (err == ESP_OK) err = passport_control_http_register(server);
    if (err != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_stop(server));
    }
    return err;
}

void app_main(void)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    const esp_timer_create_args_t replay = { .callback = replay_boot, .name = "boot_replay" };
    esp_err_t replay_status = esp_timer_create(&replay, &boot_replay);
    if (replay_status == ESP_OK) {
        replay_status = esp_timer_start_once(boot_replay, 30000000);
        if (replay_status != ESP_OK) { esp_timer_delete(boot_replay); boot_replay = NULL; }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(replay_status);
#endif
    passport_wifi_prepare();

    display_status = passport_display_init();
    ESP_ERROR_CHECK_WITHOUT_ABORT(display_status);
    button_status = passport_buttons_init();
    ESP_ERROR_CHECK_WITHOUT_ABORT(button_status);
    startup_heap("display/buttons", display_status != ESP_OK ? display_status : button_status);

    esp_err_t wifi_status = nvs_flash_init();
    nimble_status = wifi_status;
    if (wifi_status == ESP_OK) {
        nimble_status = passport_ble_start(button_status == ESP_OK);
        startup_heap("BLE/control", nimble_status);
        wifi_status = passport_wifi_start();
        startup_heap("Wi-Fi init", wifi_status);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nimble_status);
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_status);

    const char *http_status = "Unavailable";
    esp_err_t http_result = wifi_status;
    if (wifi_status == ESP_OK) {
        http_result = start_health_server();
        ESP_ERROR_CHECK_WITHOUT_ABORT(http_result);
        http_status = http_result == ESP_OK ? "Listener created" : esp_err_to_name(http_result);
    }
    startup_heap(wifi_status == ESP_OK ? "HTTP init" : "HTTP skipped", http_result);

    passport_ui_run(http_status, button_status == ESP_OK);
}
