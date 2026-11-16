#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../main/main.c"
#include "../main/passport_board.c"

static int fail_step, step;
static bool loop_live, wifi_live;
static esp_netif_t netifs[2];
static void *random_buffer;
static size_t random_size;
static bool entropy_enabled;
static bool locked;
static unsigned lock_calls, fail_lock_call;
static lv_obj_t label;
static lv_display_t display;
static bsp_btn_cb_t button_callback;
static void *button_user;
static bsp_btn_t queued_button;
static bool queue_pending;
static bool run_main_loop;
static unsigned polls;
static jmp_buf main_done;
struct ble_config ble_hs_cfg;

static esp_err_t next_step(void)
{
    return ++step == fail_step ? ESP_FAIL : ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

// macOS lacks the SDK libc's explicit_bzero. Preserve its observable semantics.
void explicit_bzero(void *buffer, size_t size)
{
    volatile uint8_t *bytes = buffer;
    while (size--) *bytes++ = 0;
}

void bootloader_random_enable(void) { entropy_enabled = true; }
void bootloader_random_disable(void) { entropy_enabled = false; }
void esp_fill_random(void *buffer, size_t size)
{
    assert(entropy_enabled);
    random_buffer = buffer;
    random_size = size;
    memset(buffer, 0xAB, size);
}

esp_err_t esp_netif_init(void) { return next_step(); }
esp_err_t esp_event_loop_create_default(void)
{
    assert(!loop_live);
    esp_err_t err = next_step();
    loop_live = err == ESP_OK;
    return err;
}
esp_err_t esp_event_loop_delete_default(void)
{
    assert(loop_live);
    loop_live = false;
    return ESP_OK;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    assert(config && !wifi_live);
    esp_err_t err = next_step();
    wifi_live = err == ESP_OK;
    return err;
}
esp_err_t esp_wifi_deinit(void)
{
    assert(wifi_live);
    wifi_live = false;
    return ESP_OK;
}
esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    if (next_step() != ESP_OK) return NULL;
    esp_netif_t *netif = &netifs[config->interface];
    assert(!netif->live);
    netif->live = true;
    netif->interface = config->interface;
    return netif;
}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif)
{
    assert(netif->live && netif->interface == WIFI_IF_STA);
    return next_step();
}
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *netif)
{
    assert(netif->live && netif->interface == WIFI_IF_AP);
    return next_step();
}
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) { return next_step(); }
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void) { return next_step(); }
void esp_netif_destroy_default_wifi(void *handle)
{
    esp_netif_t *netif = handle;
    if (netif) {
        assert(netif->live);
        netif->live = false;
    }
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip)
{
    assert(netif->live);
    ip->ip = 42;
    return next_step();
}
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return next_step(); }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return next_step(); }
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config)
{
    assert(interface == WIFI_IF_AP && config->ap.authmode == WIFI_AUTH_WPA2_PSK);
    assert(strlen((char *)config->ap.password) == 16);
    return next_step();
}
esp_err_t esp_wifi_start(void) { return next_step(); }
esp_err_t esp_wifi_stop(void) { return ESP_OK; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 10000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 1000; }
esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config)
{
    assert(config->max_uri_handlers == 1);
    *server = &label;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    assert(server && strcmp(uri->uri, "/health") == 0 && uri->method == HTTP_GET);
    return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t server) { (void)server; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    request->status = status;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request; (void)type;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value)
{
    (void)request;
    assert(strcmp(name, "Cache-Control") == 0 && strcmp(value, "no-store") == 0);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *body)
{
    request->body = body;
    return ESP_OK;
}
esp_err_t nimble_port_init(void) { return ESP_OK; }
esp_err_t nimble_port_deinit(void) { return ESP_OK; }
void nimble_port_run(void) {}
int xTaskCreate(void (*run)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle)
{
    (void)run; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    ble_hs_cfg.sync_cb();
    return pdPASS;
}
void vTaskDelete(void *task) { (void)task; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
QueueHandle_t xQueueCreate(unsigned length, unsigned size)
{
    assert(length == 1 && size == sizeof(bsp_btn_t));
    return &queued_button;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue == &queued_button); }
int xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    assert(queue == &queued_button);
    queued_button = *(const bsp_btn_t *)item;
    queue_pending = true;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    assert(queue == &queued_button && ticks == pdMS_TO_TICKS(1000));
    if (run_main_loop) {
        ++polls;
        if (polls == 1) {
            assert(!atomic_load(&display_text_applied));
            assert(strstr(label.text, "Starting..."));
        } else {
            assert(polls == 2 && atomic_load(&display_text_applied));
            assert(strstr(label.text, "Password: ABABABABABABABAB"));
            assert(strstr(label.text, "DHCP unverified"));
            longjmp(main_done, 1);
        }
    }
    if (!queue_pending) return 0;
    *(bsp_btn_t *)item = queued_button;
    queue_pending = false;
    return pdTRUE;
}
esp_err_t bsp_display_init(void) { return ESP_OK; }
lv_display_t *bsp_lvgl_init(void) { return &display; }
bool bsp_lvgl_lock(int timeout_ms)
{
    assert(timeout_ms == 1000 && !locked);
    if (++lock_calls == fail_lock_call) return false;
    locked = true;
    return true;
}
void bsp_lvgl_unlock(void) { assert(locked); locked = false; }
void bsp_display_backlight(uint8_t percent) { assert(percent == 60); }
esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user)
{
    // Models BSP success even if the upstream polling timer never starts.
    button_callback = callback;
    button_user = user;
    return ESP_OK;
}
lv_obj_t *lv_display_get_screen_active(lv_display_t *screen) { assert(screen); return &label; }
lv_obj_t *lv_label_create(lv_obj_t *parent) { assert(parent); return &label; }
void lv_obj_set_width(lv_obj_t *object, int width) { assert(object && width == 200); }
void lv_obj_align(lv_obj_t *object, int align, int x, int y)
{
    assert(object && align == LV_ALIGN_TOP_MID && x == 0 && y == 32);
}
void lv_label_set_text_static(lv_obj_t *object, const char *text)
{
    assert(locked && object == &label);
    label.text = text;
}

static void test_wifi_failures(void)
{
    prepare_ap(&ap_config);
    esp_netif_ip_info_t ip;
    for (int failure = 1; failure <= 14; ++failure) {
        step = 0;
        fail_step = failure;
        assert(start_wifi(&ap_config, &ip) != ESP_OK);
        assert(step == failure);
        assert(!loop_live && !wifi_live && !netifs[0].live && !netifs[1].live);
    }
    fail_step = 0;
    step = 0;
    assert(start_wifi(&ap_config, &ip) == ESP_OK);
    assert(step == 14 && loop_live && wifi_live && netifs[0].live && netifs[1].live);
    esp_netif_destroy_default_wifi(&netifs[0]);
    esp_netif_destroy_default_wifi(&netifs[1]);
    esp_wifi_deinit();
    esp_event_loop_delete_default();
}

static void test_main_retry_and_secrets(void)
{
    step = 0;
    lock_calls = 0;
    fail_lock_call = 2; // First status update times out after display creation.
    run_main_loop = true;
    if (setjmp(main_done) == 0) app_main();
    run_main_loop = false;
    assert(polls == 2 && lock_calls == 3);
    for (size_t i = 0; i < sizeof(ap_config); ++i) assert(((uint8_t *)&ap_config)[i] == 0);
    for (size_t i = 0; i < random_size; ++i) assert(((uint8_t *)random_buffer)[i] == 0);
    assert(!entropy_enabled);
    assert(label.text == status_text);

    char old_text[sizeof(status_text)];
    memcpy(old_text, status_text, sizeof(old_text));
    fail_lock_call = lock_calls + 1;
    assert(!passport_show_status("Changed", "Changed", "Changed", ESP_OK));
    assert(memcmp(old_text, status_text, sizeof(old_text)) == 0);
    assert(passport_show_status("Changed", "Created", "Synced", ESP_OK));
    assert(label.text == status_text && strstr(label.text, "Changed"));
}

static void check_health(const char *expected)
{
    httpd_req_t request = { .status = "200 OK" };
    assert(health_get(&request) == ESP_OK);
    assert(strcmp(request.status, expected) == 0);
    assert(strstr(request.body, "DHCP and association unverified"));
    assert(strstr(request.body, "</html>"));
    assert(!strstr(request.body, "ABABABABABABABAB"));
    assert(!strstr(request.body, "Password"));
}

static void test_button_observation(void)
{
    assert(passport_buttons_observed() == 0);
    check_health("503 Service Unavailable");
    button_callback(BSP_BTN_UP, BSP_BTN_RELEASE, button_user);
    assert(passport_buttons_observed() == 0);
    button_callback(BSP_BTN_UP, BSP_BTN_PRESS, button_user);
    button_callback(BSP_BTN_DOWN, BSP_BTN_PRESS, button_user);
    check_health("503 Service Unavailable");
    button_callback(BSP_BTN_OK, BSP_BTN_PRESS, button_user);
    // Overwriting the one-slot queue must retain observation of every key.
    assert(passport_buttons_observed() == PASSPORT_ALL_BUTTONS);
    check_health("200 OK");
    atomic_store(&display_text_applied, false);
    check_health("503 Service Unavailable");
    atomic_store(&display_text_applied, true);
    on_nimble_reset(1);
    check_health("503 Service Unavailable");
}

int main(void)
{
    test_wifi_failures();
    test_main_retry_and_secrets();
    test_button_observation();
    puts("PASS: 14 Wi-Fi failures, display retry and buffer lifetime, secret clearing, observed-button health");
    return 0;
}
