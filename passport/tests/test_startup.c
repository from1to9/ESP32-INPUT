#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../main/main.c"
#include "../main/passport_board.c"
#include "../main/passport_ui.c"

static int fail_step, step;
static bool loop_live, wifi_live;
static esp_netif_t netifs[2];
static void *random_buffer;
static size_t random_size;
static bool entropy_enabled;
static bool locked;
static unsigned lock_calls, fail_lock_call;
static lv_obj_t label;
static passport_view_t applied_view;
static esp_err_t theme_save_result;
static bool request_failure;
static lv_display_t display;
static bsp_btn_cb_t button_callback;
static void *button_user;
static passport_button_event_t queued_events[16];
static unsigned queued_count;
static passport_ble_state_t mock_ble = { .synced = true, .locked = false };
static passport_command_t requests[64];
static unsigned request_count, emergency_count;
static uint64_t time_ms = 1000;
static bool run_main_loop;
static unsigned polls;
static jmp_buf main_done;

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

    return pdPASS;
}
void vTaskDelete(void *task) { (void)task; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
esp_err_t passport_ble_start(void) { return ESP_OK; }
void passport_ble_snapshot(passport_ble_state_t *out) { *out = mock_ble; }
bool passport_ble_request(const passport_command_t *command)
{
    assert(request_count < 64);
    requests[request_count++] = *command;
    return !request_failure;
}
void passport_ble_emergency_lock(void) { ++emergency_count; }
int64_t esp_timer_get_time(void) { return time_ms * 1000; }
QueueHandle_t xQueueCreate(unsigned length, unsigned size)
{
    assert(length == 16 && size == sizeof(passport_button_event_t));
    return queued_events;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue == queued_events); }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    assert(queue == queued_events && ticks == 0);
    if (queued_count == 16) return 0;
    queued_events[queued_count++] = *(const passport_button_event_t *)item;
    return pdTRUE;
}
int xQueueReset(QueueHandle_t queue)
{
    assert(queue == queued_events); queued_count = 0; return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    assert(queue == queued_events && ticks == pdMS_TO_TICKS(20));
    if (run_main_loop) {
        ++polls;
        if (polls == 1) {
            assert(!passport_ui_text_applied());
            assert(!strcmp(applied_view.title, "Starting"));
        } else {
            assert(polls == 2 && passport_ui_text_applied());
            assert(strstr(applied_view.body, "Password: ABABABABABABABAB"));
            assert(strstr(applied_view.body, "DHCP unverified"));
            longjmp(main_done, 1);
        }
    }
    if (!queued_count) return 0;
    *(passport_button_event_t *)item = queued_events[0];
    memmove(queued_events, queued_events + 1, --queued_count * sizeof(queued_events[0]));
    return pdTRUE;
}
esp_err_t bsp_display_init(void) { return ESP_OK; }
lv_display_t *bsp_lvgl_init(void) { return &display; }
bool bsp_lvgl_lock(int timeout_ms)
{
    assert((timeout_ms == 1000 || timeout_ms == 50) && !locked);
    if (++lock_calls == fail_lock_call) return false;
    locked = true;
    return true;
}
void bsp_lvgl_unlock(void) { assert(locked); locked = false; }
void bsp_display_backlight(uint8_t percent) { assert(percent == 60 || percent == 0); }
esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user)
{
    // Models BSP success even if the upstream polling timer never starts.
    button_callback = callback;
    button_user = user;
    return ESP_OK;
}
lv_obj_t *lv_display_get_screen_active(lv_display_t *screen) { assert(screen); return &label; }
bool passport_widgets_init(lv_obj_t *screen) { assert(screen && locked); return true; }
void passport_widgets_update(const passport_view_t *view) { assert(locked); applied_view = *view; }
esp_err_t passport_theme_load(passport_theme_t *out) { *out = PASSPORT_DARK; return ESP_OK; }
esp_err_t passport_theme_save(passport_theme_t value)
{
    assert(!locked && value <= PASSPORT_LIGHT);
    return theme_save_result;
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
    page = STATUS;
    run_main_loop = true;
    if (setjmp(main_done) == 0) app_main();
    run_main_loop = false;
    assert(polls == 2 && lock_calls == 3);
    for (size_t i = 0; i < sizeof(ap_config); ++i) assert(((uint8_t *)&ap_config)[i] == 0);
    for (size_t i = 0; i < random_size; ++i) assert(((uint8_t *)random_buffer)[i] == 0);
    assert(!entropy_enabled);
    passport_view_t old_view = applied_view;
    passport_view_t changed = {.title = "Changed"};
    fail_lock_call = lock_calls + 1;
    assert(!passport_render(&changed));
    assert(memcmp(&old_view, &applied_view, sizeof(old_view)) == 0);
    assert(passport_render(&changed));
    assert(!strcmp(applied_view.title, "Changed"));
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
    // Full event queue preserves order independently of cumulative observations.
    assert(passport_buttons_observed() == PASSPORT_ALL_BUTTONS);
    check_health("200 OK");
    atomic_store(&text_applied, false);
    check_health("503 Service Unavailable");
    atomic_store(&text_applied, true);
    mock_ble.synced = false;
    check_health("503 Service Unavailable");
}

static void event(passport_button_t button, passport_button_kind_t kind)
{
    passport_button_event_t ev = { .button = button, .kind = kind, .time_ms = time_ms };
    input(&ev);
}

static void test_gestures(void)
{
    ble = mock_ble;
    ble.locked = true;
    page = HOME; focus = 0; request_count = 0;
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_LONG);
    assert(request_count == 1 && requests[0].kind == PASSPORT_LOCK);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    event(PASSPORT_UP, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_CLICKED);
    assert(focus == 0); // Another key cannot re-enable a consumed long click.
    event(PASSPORT_OK, PASSPORT_LONG);
    assert(request_count == 1); // No fresh press, no unlock.
    render_ui("", "");
    ++time_ms;
    event(PASSPORT_OK, PASSPORT_PRESS);
    event(PASSPORT_OK, PASSPORT_LONG);
    assert(request_count == 2 && requests[1].kind == PASSPORT_UNLOCK);
    event(PASSPORT_OK, PASSPORT_RELEASE);
    event(PASSPORT_OK, PASSPORT_CLICKED);
    assert(page == LOCK_SCREEN);
    page = HOME;
    awake = false;
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_LONG);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    event(PASSPORT_DOWN, PASSPORT_CLICKED);
    assert(awake && !down_held && focus == 0 && request_count == 2);
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    event(PASSPORT_DOWN, PASSPORT_CLICKED);
    assert(focus == 1);
    event(PASSPORT_OK, PASSPORT_PRESS);
    event(PASSPORT_OK, PASSPORT_DOUBLE);
    event(PASSPORT_OK, PASSPORT_CLICKED);
    assert(page == HOME);
    atomic_store(&text_applied, false);
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_CLICKED);
    assert(focus == 1); // Do not act on a view that failed to render.
    atomic_store(&text_applied, true);
    for (int i = 0; i < 17; ++i) button_callback(BSP_BTN_UP, BSP_BTN_PRESS, button_user);
    assert(passport_button_overflow() && !passport_button_overflow());
    passport_button_event_t ev;
    assert(!passport_next_button(&ev));
    button_callback(BSP_BTN_OK, BSP_BTN_PRESS, button_user);
    button_callback(BSP_BTN_OK, BSP_BTN_RELEASE, button_user);
    assert(passport_next_button(&ev) && ev.kind == PASSPORT_PRESS && ev.time_ms == time_ms);
    assert(passport_next_button(&ev) && ev.kind == PASSPORT_RELEASE);
}

static void test_async_lock_recovery(void)
{
    for (unsigned reason = 0; reason < 3; ++reason) {
        mock_ble.locked = false;
        ++mock_ble.generation;
        observe_owner();
        page = reason == 1 ? KEYS : MOUSE;
        request_count = 0;
        unlock_armed = unlock_requested = down_held = wake_gesture = true;
        if (reason == 0) ++mock_ble.generation; // BLE reset snapshot.
        if (reason == 1) passport_ble_emergency_lock(); // Owner command-queue pressure.
        if (reason == 2) {
            passport_button_event_t old = {PASSPORT_OK, PASSPORT_PRESS, time_ms - 251};
            unsigned before = emergency_count;
            dispatch_button(&old);
            assert(emergency_count == before + 1);
        }
        mock_ble.locked = true; // The real owner paths are exercised in test_ble.
        observe_owner();
        assert(page == LOCK_SCREEN && !unlock_armed && !unlock_requested && !down_held && !wake_gesture);
        event(PASSPORT_OK, PASSPORT_PRESS); // Before Lock is rendered: cannot arm.
        render_ui("", "");
        event(PASSPORT_OK, PASSPORT_LONG);
        assert(request_count == 0);
        event(PASSPORT_OK, PASSPORT_RELEASE);
        ++time_ms;
        event(PASSPORT_OK, PASSPORT_PRESS);
        event(PASSPORT_OK, PASSPORT_LONG);
        assert(request_count == 1 && requests[0].kind == PASSPORT_UNLOCK);
        mock_ble.locked = false;
        observe_owner();
        assert(page == HOME);
    }
}

static void start_repeat(void)
{
    page = MOUSE; focus = 0; ble.locked = false; ble.ready = true;
    event(PASSPORT_OK, PASSPORT_RELEASE);
    awake = true; wake_gesture = false; request_count = 0;
    atomic_store(&text_applied, true);
    event(PASSPORT_OK, PASSPORT_PRESS);
    time_ms += 500;
    event(PASSPORT_OK, PASSPORT_LONG);
    assert(repeat_active);
    service_repeat(time_ms);
    assert(request_count == 1 && requests[0].kind == PASSPORT_MOVE && requests[0].y == -12);
}

static void test_repeat(void)
{
    start_repeat();
    service_repeat(time_ms + 99);
    assert(request_count == 1);
    service_repeat(time_ms + 450); // A delayed tick produces only one step.
    assert(request_count == 2);
    service_repeat(ok_pressed_at + 2000);
    assert(!repeat_active && requests[2].kind == PASSPORT_CANCEL);
    time_ms = ok_pressed_at + 2100;
    event(PASSPORT_OK, PASSPORT_LONG);
    assert(!repeat_active); // A missing release cannot restart the cap.
    event(PASSPORT_OK, PASSPORT_RELEASE);
    event(PASSPORT_OK, PASSPORT_CLICKED);
    assert(request_count == 3); // Long hold does not add a click on release.
    for (unsigned cause = 0; cause < 6; ++cause) {
        start_repeat();
        switch (cause) {
        case 0: event(PASSPORT_OK, PASSPORT_RELEASE); break;
        case 1: ble.ready = false; break;
        case 2: ++ble.generation; break;
        case 3: navigate(HOME); break;
        case 4: enter_lock_screen(); break;
        case 5: request_failure = true; break;
        }
        unsigned before = emergency_count;
        time_ms += 100;
        service_repeat(time_ms);
        assert(!repeat_active);
        if (cause == 5) assert(emergency_count == before + 1);
        request_failure = false;
    }
}

static void test_appearance(void)
{
    page = APPEARANCE; focus = 1; theme = PASSPORT_DARK;
    theme_save_result = ESP_FAIL;
    activate();
    atomic_store(&text_applied, false);
    apply_theme();
    assert(theme_pending && theme == PASSPORT_DARK);
    render_ui("", "");
    apply_theme();
    build_view("", "");
    assert(theme == PASSPORT_DARK && view.items[0].checked && !view.items[1].checked);
    assert(theme_failed && view.tone == TONE_ERROR && strstr(view.detail, "unconfirmed"));
    theme_save_result = ESP_OK;
    activate(); render_ui("", ""); apply_theme(); build_view("", "");
    assert(theme == PASSPORT_LIGHT && view.items[1].checked && !theme_failed);
}

static void test_visible_state(void)
{
    page = HOME; focus = 2; ble.synced = false;
    ble.release_unknown = true;
    snprintf(ble.result, sizeof(ble.result), "Command queue full");
    build_view("", "");
    assert(strstr(view.items[2].subtitle, "BT ?"));
    assert(!strcmp(view.detail, "Command queue full"));
    page = STATUS; build_view("AP configured / STA idle", "Unavailable");
    assert(strstr(view.body, "Previous release unknown"));
    page = HOST; focus = 3; build_view("", "");
    assert(!strcmp(view.ok, "Back"));
    snprintf(target.label, sizeof(target.label), "C1:22:33:44:55:66/1");
    page = CONFIRM_FORGET; focus = 0; build_view("", "");
    assert(!strcmp(view.ok, "Cancel") && strstr(view.body, target.label));
}

int main(void)
{
    test_wifi_failures();
    test_main_retry_and_secrets();
    test_button_observation();
    test_gestures();
    test_async_lock_recovery();
    test_repeat();
    test_appearance();
    test_visible_state();
    puts("PASS: 14 Wi-Fi failures, display retry and buffer lifetime, secret clearing, observed-button health, gestures, bounded queue and three asynchronous lock paths, bounded repeats and appearance failures");
    return 0;
}
