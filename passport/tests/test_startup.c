#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "names_view_fixture.h"

#include "../main/main.c"
#include "../main/passport_board.c"
#include "../main/passport_ui.c"

static bool locked;
void passport_usb_init(void) {}
void passport_usb_service(const passport_ble_state_t *b, const passport_wifi_state_t *w,
                          passport_theme_t t, bool known, bool busy)
{ (void)b; (void)w; (void)t; (void)known; (void)busy; }
static unsigned lock_calls, fail_lock_call;
static lv_obj_t label;
static passport_view_t applied_view;
static esp_err_t theme_save_result;
static passport_sound_state_t mock_sound = {.preference_known = true};
static esp_err_t sound_save_result;
static unsigned sound_requests, cue_requests;
static passport_cue_t last_cue;
static bool request_failure;
static passport_wifi_state_t mock_wifi;
static passport_wifi_command_t wifi_requests[16];
static unsigned wifi_request_count;
static lv_display_t display;
static bsp_btn_cb_t button_callback;
static void *button_user;
static passport_button_event_t queued_events[16];
static unsigned queued_count;
static passport_ble_state_t mock_ble = { .synced = true, .locked = false };
static passport_command_t requests[64];
static unsigned request_count, emergency_count;
static uint8_t backlight_percent;
static uint64_t time_ms = 1000;
static bool run_main_loop;
static unsigned polls;
static unsigned adc_reads;
static jmp_buf main_done;
static esp_err_t http_start_result;
static unsigned http_start_calls, http_registrations;

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 10000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 1000; }
esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config)
{
    ++http_start_calls;
    assert(config->stack_size == 4096);
    assert(config->task_caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_RTCRAM));
    assert(config->max_uri_handlers == 14 && config->recv_wait_timeout == 2 && config->uri_match_fn==httpd_uri_match_wildcard);
    assert(config->max_open_sockets == 3 && config->lru_purge_enable);
    if (http_start_result != ESP_OK) return http_start_result;
    *server = &label;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    ++http_registrations;
    assert(server && strcmp(uri->uri, "/health") == 0 && uri->method == HTTP_GET);
    return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t server) { (void)server; return ESP_OK; }
esp_err_t passport_setup_register(httpd_handle_t server) { assert(server); return ESP_OK; }
esp_err_t passport_control_http_register(httpd_handle_t server) { assert(server); return ESP_OK; }
bool httpd_uri_match_wildcard(const char *a,const char *b,size_t length) { (void)a;(void)b;(void)length;return true; }
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
esp_err_t passport_ble_start(bool buttons_available) { assert(buttons_available); return ESP_OK; }
void passport_ble_snapshot(passport_ble_state_t *out) { *out = mock_ble; }
bool passport_ble_request(const passport_command_t *command)
{
    assert(request_count < 64);
    requests[request_count++] = *command;
    return !request_failure;
}
void passport_ble_emergency_lock(void) { ++emergency_count; }
int64_t esp_timer_get_time(void) { return time_ms * 1000; }
static void (*replay_callback)(void *);
static unsigned replay_deleted;
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{ replay_callback=args->callback;*timer=&replay_callback;return ESP_OK; }
int esp_timer_start_once(esp_timer_handle_t timer, uint64_t us)
{ assert(timer==&replay_callback && us==30000000);return ESP_OK; }
int esp_timer_delete(esp_timer_handle_t timer)
{ assert(timer==&replay_callback);++replay_deleted;return ESP_OK; }
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
            assert(applied_view.layout == VIEW_HOME);
            assert(!strcmp(applied_view.title, "Your controls"));
            assert(!strcmp(applied_view.items[0].title, "Mouse"));
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
void bsp_display_backlight(uint8_t percent) { assert(percent == 60 || percent == 0); backlight_percent = percent; }
esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user)
{
    // Models BSP success even if the upstream polling timer never starts.
    button_callback = callback;
    button_user = user;
    return ESP_OK;
}
int bsp_button_read_mv(void) { ++adc_reads; return 3300; }
uint32_t bsp_button_glitch_count(void) { return 0; }
lv_obj_t *lv_display_get_screen_active(lv_display_t *screen) { assert(screen); return &label; }
bool passport_widgets_init(lv_obj_t *screen) { assert(screen && locked); return true; }
void passport_widgets_update(const passport_view_t *view) { assert(locked); applied_view = *view; }
esp_err_t passport_theme_load(passport_theme_t *out) { *out = PASSPORT_DARK; return ESP_OK; }
esp_err_t passport_theme_save(passport_theme_t value)
{
    assert(!locked && value <= PASSPORT_LIGHT);
    return theme_save_result;
}
void passport_sound_load(void) {}
passport_sound_state_t passport_sound_state(void) { return mock_sound; }
esp_err_t passport_sound_set(bool enabled)
{
    assert(!locked);
    ++sound_requests;
    if (sound_save_result == ESP_OK) mock_sound.enabled = enabled;
    mock_sound.preference_known = sound_save_result == ESP_OK;
    if (!enabled) mock_sound.output = PASSPORT_SOUND_OFF;
    return sound_save_result;
}
bool passport_sound_cue(passport_cue_t cue)
{
    if (mock_sound.output != PASSPORT_SOUND_READY) return false;
    ++cue_requests;
    last_cue = cue;
    return true;
}

void passport_wifi_prepare(void) {}
esp_err_t passport_wifi_start(void) { return ESP_OK; }
void passport_wifi_snapshot(passport_wifi_state_t *out) { *out = mock_wifi; }
void passport_wifi_link_snapshot(passport_wifi_link_t *out)
{
    *out = (passport_wifi_link_t){.phase = mock_wifi.phase, .ap_started = mock_wifi.ap_started,
        .dhcp_running = mock_wifi.dhcp_running, .ap_lease = mock_wifi.ap_lease,
        .transport_generation = mock_wifi.transport_generation};
    memcpy(out->sta_ip, mock_wifi.sta_ip, sizeof(out->sta_ip));
    memcpy(out->ap_ip, mock_wifi.ap_ip, sizeof(out->ap_ip));
}
bool passport_wifi_request(const passport_wifi_command_t *command) { assert(wifi_request_count < 16); wifi_requests[wifi_request_count++] = *command; return true; }
void passport_wifi_ap_credentials(char ssid[32], char password[17]) { snprintf(ssid, 32, "Passport-ABCD"); snprintf(password, 17, "0123456789ABCDEF"); }

static void test_main_render_retry(void)
{
    lock_calls = 0;
    fail_lock_call = 2; // First status update times out after display creation.
    assert(page == HOME); // Exercise the production cold-boot page.
    run_main_loop = true;
    if (setjmp(main_done) == 0) app_main();
    run_main_loop = false;
    assert(polls == 2 && lock_calls == 3);
    assert(atomic_load(&boot_stage_count)==4);
    for(unsigned i=0;i<4;++i) assert(boot_stages[i].status==ESP_OK);
    replay_callback(NULL);assert(replay_deleted==1 && !boot_replay);
    passport_view_t old_view = applied_view;
    passport_view_t changed = {.title = "Changed"};
    fail_lock_call = lock_calls + 1;
    assert(!passport_render(&changed));
    assert(memcmp(&old_view, &applied_view, sizeof(old_view)) == 0);
    assert(passport_render(&changed));
    assert(!strcmp(applied_view.title, "Changed"));
}

static const char *check_health(const char *expected)
{
    httpd_req_t request = { .status = "200 OK" };
    assert(health_get(&request) == ESP_OK);
    assert(strcmp(request.status, expected) == 0);
    assert(strstr(request.body, "AP client lease"));
    assert(strstr(request.body, "</html>"));
    assert(!strstr(request.body, "ABABABABABABABAB"));
    assert(!strstr(request.body, "Password"));
    return request.body;
}

static void test_http_stack_failure(void)
{
    const unsigned calls = http_start_calls, registrations = http_registrations;
    http_start_result = ESP_ERR_NO_MEM;
    assert(start_health_server() == ESP_ERR_NO_MEM);
    assert(http_start_calls == calls + 1 && http_registrations == registrations);
    http_start_result = ESP_OK;
}

static void test_button_observation(void)
{
    assert(passport_buttons_observed() == 0);
    assert(strstr(check_health("200 OK"), "UP pending, DOWN pending, OK pending"));
    button_callback(BSP_BTN_UP, BSP_BTN_RELEASE, button_user);
    assert(passport_buttons_observed() == 0);
    button_callback(BSP_BTN_UP, BSP_BTN_PRESS, button_user);
    button_callback(BSP_BTN_DOWN, BSP_BTN_PRESS, button_user);
    assert(strstr(check_health("200 OK"), "UP yes, DOWN yes, OK pending"));
    button_callback(BSP_BTN_OK, BSP_BTN_PRESS, button_user);
    // Full event queue preserves order independently of cumulative observations.
    assert(passport_buttons_observed() == PASSPORT_ALL_BUTTONS);
    check_health("200 OK");
    display_status = ESP_FAIL;
    check_health("503 Service Unavailable");
    display_status = ESP_OK;
    button_status = ESP_FAIL;
    check_health("503 Service Unavailable");
    button_status = ESP_OK;
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

static void test_cold_home_navigation(void)
{
    assert(page == HOME && !ble.locked && focus == 2);
    request_count = 0;
    time_ms += 1000;
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    assert(page == HOME && focus == 3 && !request_count);
    event(PASSPORT_OK, PASSPORT_PRESS);
    event(PASSPORT_OK, PASSPORT_RELEASE);
    assert(page == STATUS && request_count == 1 && requests[0].kind == PASSPORT_CANCEL);
}

static void test_home_readiness(void)
{
    mock_ble=(passport_ble_state_t){0};observe_owner();navigate(HOME);
    build_view("");assert(focus==2 && view.items[0].disabled && view.items[1].disabled);
    assert(!view.items[2].disabled && !view.items[3].disabled);
    request_count=0;focus=0;activate();assert(page==HOME && !request_count);
    mock_ble.connected=mock_ble.encrypted=true;observe_owner();build_view("");
    assert(focus==2 && !strcmp(view.items[0].subtitle,"Not ready") && view.items[0].disabled);
    atomic_store(&text_applied,true);time_ms+=1000;
    event(PASSPORT_UP,PASSPORT_PRESS);event(PASSPORT_UP,PASSPORT_RELEASE);assert(focus==3);
    event(PASSPORT_DOWN,PASSPORT_PRESS);event(PASSPORT_DOWN,PASSPORT_RELEASE);assert(focus==2);
    mock_ble.bonded=mock_ble.ready=true;observe_owner();build_view("");
    assert(focus==2 && !view.items[0].disabled && !view.items[1].disabled);
    event(PASSPORT_UP,PASSPORT_PRESS);event(PASSPORT_UP,PASSPORT_RELEASE);assert(focus==1);
    event(PASSPORT_UP,PASSPORT_PRESS);event(PASSPORT_UP,PASSPORT_RELEASE);assert(focus==0);
    time_ms+=1000;event(PASSPORT_OK,PASSPORT_PRESS);
    mock_ble.ready=false;observe_owner();build_view("");assert(focus==2);
    event(PASSPORT_OK,PASSPORT_LONG);assert(page==HOME);
    request_count=0;event(PASSPORT_OK,PASSPORT_RELEASE);assert(page==HOME && !request_count);
    mock_ble.ready=true;observe_owner();build_view("");
    event(PASSPORT_DOWN,PASSPORT_PRESS);event(PASSPORT_DOWN,PASSPORT_RELEASE);assert(focus==3);
    event(PASSPORT_DOWN,PASSPORT_PRESS);event(PASSPORT_DOWN,PASSPORT_RELEASE);assert(focus==0);
    time_ms+=1000;event(PASSPORT_OK,PASSPORT_PRESS);event(PASSPORT_OK,PASSPORT_RELEASE);
    assert(page==MOUSE);
}

static void test_gestures(void)
{
    time_ms += 1000;
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
    render_ui("");
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
    event(PASSPORT_OK, PASSPORT_DOUBLE);
    event(PASSPORT_OK, PASSPORT_CLICKED);
    assert(page == HOME);
    atomic_store(&text_applied, false);
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
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

static void test_idle_screen(void)
{
    for (unsigned remote = 0; remote < 2; ++remote) {
        ble = (passport_ble_state_t){.ready = true, .network_active = remote};
        page = KEYS; focus = 1; awake = true; wifi.setup_open = false;
        wake_gesture = takeover_gesture = false;
        request_count = 0; request_failure = false;
        const unsigned emergencies = emergency_count;
        last_activity = time_ms;
        passport_backlight(true);
        service_idle(time_ms + 59999);
        assert(awake && backlight_percent == 60);
        wifi.setup_open = true;
        service_idle(time_ms + 60000);
        assert(awake && backlight_percent == 60);
        wifi.setup_open = false;
        time_ms += 60000;
        service_idle(time_ms);
        assert(!awake && !backlight_percent && page == KEYS && focus == 1);
        assert(ble.ready && !ble.locked && ble.network_active == remote);
        assert(!request_count && emergency_count == emergencies);
        atomic_store(&text_applied, true);
        event(PASSPORT_OK, PASSPORT_PRESS);
        event(PASSPORT_OK, PASSPORT_LONG);
        event(PASSPORT_OK, PASSPORT_RELEASE);
        assert(awake && backlight_percent == 60 && page == KEYS && focus == 1);
        assert(!request_count && emergency_count == emergencies);
        // The next deliberate safety hold still locks, including remote input.
        event(PASSPORT_DOWN, PASSPORT_PRESS);
        event(PASSPORT_DOWN, PASSPORT_LONG);
        event(PASSPORT_DOWN, PASSPORT_RELEASE);
        assert(page == LOCK_SCREEN && request_count == 1 + remote);
        assert(requests[request_count - 1].kind == PASSPORT_LOCK);
    }
    ble.network_active = false;
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
        render_ui("");
        event(PASSPORT_OK, PASSPORT_LONG);
        assert(request_count == 0);
        event(PASSPORT_OK, PASSPORT_RELEASE);
        time_ms += 1000;
        event(PASSPORT_OK, PASSPORT_PRESS);
        event(PASSPORT_OK, PASSPORT_LONG);
        assert(request_count == 1 && requests[0].kind == PASSPORT_UNLOCK);
        mock_ble.locked = false;
        observe_owner();
        assert(page == HOME);
    }
}

static void test_release_latency(void)
{
    time_ms += 1000;
    ble = mock_ble;
    ble.locked = ble.network_active = false;
    ble.ready = true;
    page = CONNECTIONS; focus = 0; request_count = 0;
    awake = true; wake_gesture = takeover_gesture = false;
    atomic_store(&text_applied, true);
    // Captured rapid DOWN cadence: two releases with no SINGLE_CLICK between.
    const uint64_t start = time_ms;
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    time_ms = start + 140; event(PASSPORT_DOWN, PASSPORT_RELEASE);
    assert(focus == 1); // Already applied, before any delayed click callback.
    time_ms = start + 300; event(PASSPORT_DOWN, PASSPORT_PRESS);
    time_ms = start + 480; event(PASSPORT_DOWN, PASSPORT_RELEASE);
    assert(focus == 2);
    event(PASSPORT_DOWN, PASSPORT_DOUBLE);
    event(PASSPORT_DOWN, PASSPORT_CLICKED);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    assert(focus == 2); // No duplicate action from callbacks or an orphan release.
    time_ms += 50; event(PASSPORT_UP, PASSPORT_PRESS);
    time_ms += 140; event(PASSPORT_UP, PASSPORT_RELEASE);
    assert(focus == 1);

    // A quick second OK cannot pass through Home into a Mouse command.
    page = HOME; focus = 0; request_count = 0;
    time_ms += 1000; event(PASSPORT_OK, PASSPORT_PRESS);
    time_ms += 150; event(PASSPORT_OK, PASSPORT_RELEASE);
    assert(page == MOUSE && request_count == 1 && requests[0].kind == PASSPORT_CANCEL);
    render_ui("");
    time_ms += 50; event(PASSPORT_OK, PASSPORT_PRESS);
    time_ms += 150; event(PASSPORT_OK, PASSPORT_RELEASE);
    event(PASSPORT_OK, PASSPORT_DOUBLE);
    event(PASSPORT_OK, PASSPORT_CLICKED);
    assert(request_count == 1);
    time_ms += passport_button_click_guard_ms() + 1;
    event(PASSPORT_OK, PASSPORT_PRESS);
    time_ms += 150; event(PASSPORT_OK, PASSPORT_RELEASE);
    assert(request_count == 2 && requests[1].kind == PASSPORT_MOVE);
    time_ms += 185; event(PASSPORT_OK, PASSPORT_CLICKED);
    assert(request_count == 2);

    // Long DOWN stays a safety lock and does not add a short release action.
    page = HOME; focus = 1; request_count = 0;
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    time_ms += 500; event(PASSPORT_DOWN, PASSPORT_LONG);
    assert(page == LOCK_SCREEN && request_count == 1 && requests[0].kind == PASSPORT_LOCK);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    event(PASSPORT_DOWN, PASSPORT_CLICKED);
    assert(page == LOCK_SCREEN && focus == 0 && request_count == 1);

    // A stale edge invalidates an earlier armed short gesture immediately,
    // even before the asynchronous BLE owner publishes its Lock snapshot.
    page = HOME; focus = 0; atomic_store(&text_applied, true);
    event(PASSPORT_DOWN, PASSPORT_PRESS);
    passport_button_event_t stale = {PASSPORT_DOWN, PASSPORT_RELEASE, time_ms - 251};
    unsigned emergencies = emergency_count;
    dispatch_button(&stale);
    event(PASSPORT_DOWN, PASSPORT_RELEASE);
    assert(focus == 0 && emergency_count == emergencies + 1);
}

static void start_repeat(void)
{
    time_ms += 1000;
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
    mock_sound.output = PASSPORT_SOUND_READY;
    unsigned initial_cues = cue_requests;
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
    for (unsigned cause = 0; cause < 7; ++cause) {
        start_repeat();
        switch (cause) {
        case 0: event(PASSPORT_OK, PASSPORT_RELEASE); break;
        case 1: ble.ready = false; break;
        case 2: ++ble.generation; break;
        case 3: navigate(HOME); break;
        case 4: enter_lock_screen(); break;
        case 5: request_failure = true; break;
        case 6: ++ble.input_revision; break;
        }
        unsigned before = emergency_count;
        time_ms += 100;
        service_repeat(time_ms);
        assert(!repeat_active);
        if (cause == 5) assert(emergency_count == before + 1);
        request_failure = false;
    }
    assert(cue_requests == initial_cues);
    mock_sound.output = PASSPORT_SOUND_OFF;
}

static void test_network_takeover(void)
{
    time_ms += 1000;
    page=MOUSE;focus=2;awake=true;wake_gesture=false;request_count=0;
    ble.locked=false;ble.ready=true;ble.network_active=true;atomic_store(&text_applied,true);
    event(PASSPORT_OK,PASSPORT_PRESS);
    assert(request_count==1 && requests[0].kind==PASSPORT_TAKEOVER);
    ble.network_active=false;++ble.input_revision;
    event(PASSPORT_OK,PASSPORT_LONG);assert(!repeat_active);
    event(PASSPORT_OK,PASSPORT_RELEASE);event(PASSPORT_OK,PASSPORT_CLICKED);
    assert(request_count==1);
    event(PASSPORT_OK,PASSPORT_PRESS);event(PASSPORT_OK,PASSPORT_RELEASE);event(PASSPORT_OK,PASSPORT_CLICKED);
    assert(request_count==2 && requests[1].kind==PASSPORT_CLICK && requests[1].input_revision==ble.input_revision);

    time_ms += 1000;
    page=MOUSE;focus=2;request_count=0;ble.network_active=true;
    event(PASSPORT_DOWN,PASSPORT_PRESS);
    assert(request_count==1 && requests[0].kind==PASSPORT_TAKEOVER);
    // A held OK that began before Lock is visible cannot unlock it.
    event(PASSPORT_OK,PASSPORT_PRESS);
    ble.network_active=false;
    unsigned before=request_count;
    event(PASSPORT_DOWN,PASSPORT_LONG);
    assert(request_count==before+1 && requests[before].kind==PASSPORT_LOCK && page==LOCK_SCREEN);
    ble.locked=true;ble.ready=false;
    event(PASSPORT_DOWN,PASSPORT_RELEASE);event(PASSPORT_DOWN,PASSPORT_CLICKED);
    assert(request_count==before+1 && page==LOCK_SCREEN && focus==0);
    render_ui("");++time_ms;
    event(PASSPORT_OK,PASSPORT_LONG);
    assert(request_count==before+1 && !unlock_requested);
    event(PASSPORT_OK,PASSPORT_RELEASE);event(PASSPORT_OK,PASSPORT_CLICKED);
    event(PASSPORT_OK,PASSPORT_PRESS);event(PASSPORT_OK,PASSPORT_LONG);
    assert(request_count==before+2 && requests[before+1].kind==PASSPORT_UNLOCK && unlock_requested);
    event(PASSPORT_OK,PASSPORT_RELEASE);event(PASSPORT_OK,PASSPORT_CLICKED);
    assert(request_count==before+2 && page==LOCK_SCREEN);
}

static void test_appearance(void)
{
    page = APPEARANCE; focus = 1; theme = PASSPORT_DARK;
    theme_save_result = ESP_FAIL;
    activate();
    atomic_store(&text_applied, false);
    apply_theme();
    assert(theme_pending && theme == PASSPORT_DARK);
    render_ui("");
    apply_theme();
    build_view("");
    assert(theme == PASSPORT_DARK && view.items[0].checked && !view.items[1].checked);
    assert(theme_failed && view.tone == TONE_ERROR && strstr(view.detail, "unconfirmed"));
    theme_save_result = ESP_OK;
    activate(); render_ui(""); apply_theme(); build_view("");
    assert(theme == PASSPORT_LIGHT && view.items[1].checked && !theme_failed);
}

static void test_visible_state(void)
{
    page=LOCK_SCREEN;unlock_requested=false;owner_was_locked=true;mock_ble.locked=false;mock_wifi.pending=false;
    observe_owner();assert(page==HOME); // A physical USB unlock follows actual owner state.
    page = HOME; focus = 2; ble.synced = false;
    ble.release_unknown = true;
    snprintf(ble.result, sizeof(ble.result), "Command queue full");
    build_view("");
    assert(strstr(view.items[2].subtitle, "WiFi offline"));
    assert(!strcmp(view.detail, "Command queue full"));
    page = STATUS; build_view("Unavailable");
    assert(strstr(view.body, "Previous release unknown"));
    assert(strstr(view.body, "STA IP: none") && strstr(view.body, "AP IP: none"));
    assert(strstr(view.body, "Network control: unavailable"));
    page = HOST; focus = 4; build_view("");
    assert(!strcmp(view.ok, "Back"));
    snprintf(target.label, sizeof(target.label), "C1:22:33:44:55:66/1");
    page = CONFIRM_FORGET; focus = 0; build_view("");
    assert(!strcmp(view.ok, "Cancel") && strstr(view.body, target.label));
}

static void test_label_navigation(void)
{
    ble = (passport_ble_state_t){.storage_known = true, .peer_count = 2, .bond_revision = 37,
        .peers = {{.address = {1}, .bond_id = {1}, .label = "00:00:00:00:00:01/0"},
                  {.address = {2}, .bond_id = {2}, .label = "00:00:00:00:00:02/0"}}};
    page = BLUETOOTH; focus = 2;
    build_view("");
    assert(!strcmp(view.items[1].title, view.items[2].title));
    assert(strcmp(view.items[1].subtitle, view.items[2].subtitle));
    activate();
    assert(page == HOST && target.address[0] == 2 && target_revision == 37);
    focus = 3; activate(); build_view("");
    assert(page == HOST_IDENTITY && strstr(view.body, LONG_DISPLAY_LABEL) && strstr(view.body, target.label));
    activate();
    assert(page == HOST);
}

static void test_wifi_ui(void)
{
    time_ms += 1000;
    wifi=(passport_wifi_state_t){.initialized=true,.storage_known=true,.scan_known=true,.scan_count=16};
    page=WIFI_LIST; focus=0; wifi_request_count=0; ble.locked=false;
    atomic_store(&text_applied,true); awake=true; wake_gesture=false;
    event(PASSPORT_UP,PASSPORT_PRESS); event(PASSPORT_UP,PASSPORT_RELEASE); event(PASSPORT_UP,PASSPORT_CLICKED);
    assert(focus==0 && !wifi_request_count);
    focus=17; event(PASSPORT_DOWN,PASSPORT_PRESS); event(PASSPORT_DOWN,PASSPORT_RELEASE); event(PASSPORT_DOWN,PASSPORT_CLICKED);
    assert(focus==17 && !wifi_request_count);
    build_view(""); assert(view.count==3 && view.focus==2 && !strcmp(view.items[2].title,"Back"));
    mock_ble.locked=false; mock_wifi=wifi; mock_wifi.pending=true; mock_wifi.pending_id=7; mock_wifi.setup_id=42;
    mock_wifi.pending_ssid=(passport_ssid_t){.bytes="Test LAN",.length=8};
    wifi_pending_id=0; request_count=0;
    observe_owner(); assert(page==WIFI_PENDING && focus==0);
    event(PASSPORT_OK,PASSPORT_PRESS); render_ui(""); focus=1;
    event(PASSPORT_OK,PASSPORT_RELEASE);
    event(PASSPORT_OK,PASSPORT_CLICKED); assert(!wifi_request_count && page==WIFI_PENDING);
    time_ms += 1000; event(PASSPORT_OK,PASSPORT_PRESS); event(PASSPORT_OK,PASSPORT_RELEASE); event(PASSPORT_OK,PASSPORT_CLICKED);
    assert(wifi_request_count==1 && wifi_requests[0].action==PW_CONFIRM && wifi_requests[0].id==7 && wifi_requests[0].revision==42);
    mock_wifi.pending=false; observe_owner(); assert(page==WIFI_AP);
}

static void test_pair_feedback(void)
{
    page = PAIR_WINDOW;
    ble = (passport_ble_state_t){.synced=true, .storage_known=true, .pairing=true, .window_seconds=30};
    build_view("");
    assert(!strcmp(view.heading, "Ready to pair") && !strcmp(view.hero, "30 s"));
    ble.connected = true;
    build_view("");
    assert(!strcmp(view.heading, "Securing link") && strstr(view.body, "unencrypted"));
    ble.encrypted = ble.bonded = true; ble.pairing = false; ble.window_seconds = 0; ble.peer_count = 1;
    build_view("");
    assert(!strcmp(view.heading, "Host saved") && strstr(view.body, "Saved hosts: 1") && !strcmp(view.ok, "Back"));
    assert(!ble.ready); // Bond persistence is not HID readiness.
    ble.connected = ble.encrypted = ble.bonded = false; ble.peer_count = 0;
    snprintf(ble.result, sizeof(ble.result), "Bond admission failed");
    build_view("");
    assert(!strcmp(view.heading, "Pairing ended") && strstr(view.body, "Bond admission failed"));
    assert(strstr(view.body, "Saved hosts: 0") && !view.hero[0]);
}

static void test_sound_ui(void)
{
    time_ms += 1000;
    page = CONNECTIONS;
    focus = 2;
    mock_sound = (passport_sound_state_t){.preference_known = true};
    render_ui("");
    event(PASSPORT_OK, PASSPORT_PRESS);
    event(PASSPORT_OK, PASSPORT_RELEASE);
    event(PASSPORT_OK, PASSPORT_CLICKED);
    build_view("");
    assert(page == SOUND && view.count == 4 && view.items[0].checked);
    assert(!strcmp(view.message, "Sound off"));
    focus = 1;
    activate();
    atomic_store(&text_applied, false);
    apply_sound();
    assert(sound_pending && sound_requests == 0);
    render_ui("");
    apply_sound();
    assert(sound_requests == 1 && mock_sound.enabled);
    mock_sound.output = PASSPORT_SOUND_READY;
    focus = 2;
    activate();
    assert(cue_requests == 1 && last_cue == PASSPORT_CUE_CONFIRM);
    ble.ready = false;
    mock_ble.ready = true;
    observe_owner();
    assert(cue_requests == 2 && last_cue == PASSPORT_CUE_SUCCESS);
    mock_wifi.phase = wifi.phase = PW_CONNECTING;
    mock_wifi.phase = PW_OFFLINE;
    mock_wifi.disconnect_reason = 201;
    observe_owner();
    assert(cue_requests == 3 && last_cue == PASSPORT_CUE_ERROR);
    observe_owner();
    assert(cue_requests == 3);
    mock_sound.output = PASSPORT_SOUND_UNAVAILABLE;
    activate();
    build_view("");
    assert(cue_requests == 3 && !strcmp(view.message, "Speaker unavailable"));
    assert(view.items[1].checked && view.tone == TONE_ERROR);
    sound_save_result = ESP_FAIL;
    focus = 0;
    activate();
    render_ui("");
    apply_sound();
    build_view("");
    assert(!view.items[1].checked && strstr(view.detail, "unconfirmed"));
    time_ms += 1000;
    event(PASSPORT_OK, PASSPORT_PRESS);
    event(PASSPORT_OK, PASSPORT_LONG);
    assert(page == CONNECTIONS);
}

#include "sdk_button.inc"

typedef struct {
    button_driver_t driver;
    button_dev_t button;
    bsp_btn_t key;
    uint8_t level;
    unsigned longs;
    uint64_t pressed_at, released_at, long_at;
} driver_fixture_t;

static uint8_t driver_level(button_driver_t *driver)
{
    return ((driver_fixture_t *)driver)->level;
}

static void driver_callback(void *handle, void *user)
{
    driver_fixture_t *fixture = user;
    const button_dev_t *button = handle;
    bsp_btn_ev_t event;
    switch (button->event) {
    case BUTTON_PRESS_DOWN: event = BSP_BTN_PRESS; fixture->pressed_at = time_ms; break;
    case BUTTON_PRESS_UP: event = BSP_BTN_RELEASE; fixture->released_at = time_ms; break;
    case BUTTON_LONG_PRESS_START: event = BSP_BTN_LONG; ++fixture->longs; fixture->long_at = time_ms; break;
    case BUTTON_SINGLE_CLICK: event = BSP_BTN_CLICK; break;
    case BUTTON_DOUBLE_CLICK: event = BSP_BTN_DOUBLE; break;
    default: assert(false); return;
    }
    button_callback(fixture->key, event, button_user);
}

static void driver_reset(driver_fixture_t *fixture, bsp_btn_t key)
{
    for (unsigned i = 0; i < BUTTON_EVENT_MAX; ++i) free(fixture->button.cb_info[i]);
    *fixture = (driver_fixture_t){.driver.get_key_level = driver_level, .key = key};
    fixture->button = (button_dev_t){.driver = &fixture->driver, .short_press_ticks = 36,
                                    .long_press_ticks = 100, .count = {0, -1}};
    const button_event_t events[] = {BUTTON_PRESS_DOWN, BUTTON_PRESS_UP,
        BUTTON_LONG_PRESS_START, BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK};
    for (unsigned i = 0; i < 5; ++i) {
        // Match iot_button_register_cb: each event owns its exact allocation.
        button_cb_info_t *callback = calloc(1, sizeof(*callback));
        assert(callback);
        *callback = (button_cb_info_t){.cb = driver_callback, .usr_data = fixture,
                                      .event_args.long_press.press_time = 500};
        fixture->button.cb_info[events[i]] = callback;
        fixture->button.size[events[i]] = 1;
    }
    time_ms += 1000;
    xQueueReset(button_queue);
    enter_lock_screen();
    ble = mock_ble; ble.locked = ble.network_active = false; ble.ready = true;
    page = HOME; focus = 0; awake = true; request_count = 0; ok_click_blocked_until = 0;
    atomic_store(&text_applied, true);
}

static void driver_ticks(driver_fixture_t *fixture, uint8_t level, unsigned ticks, bool dispatch)
{
    fixture->level = level;
    for (unsigned i = 0; i < ticks; ++i) {
        time_ms += CONFIG_BUTTON_PERIOD_TIME_MS;
        button_handler(&fixture->button);
        passport_button_event_t next;
        if (dispatch) while (passport_next_button(&next)) dispatch_button(&next);
    }
}

static void driver_short(driver_fixture_t *fixture)
{
    driver_ticks(fixture, BUTTON_ACTIVE, 28, true);
    driver_ticks(fixture, BUTTON_INACTIVE, 8, true);
}

static void test_pinned_button_holds(void)
{
    driver_fixture_t fixture = {0};
    for (unsigned taps = 0; taps <= 2; ++taps) {
        driver_reset(&fixture, BSP_BTN_DOWN);
        for (unsigned i = 0; i < taps; ++i) driver_short(&fixture);
        assert(focus == taps && request_count == 0);
        driver_ticks(&fixture, BUTTON_ACTIVE, 2, true); // Confirmed PRESS.
        driver_ticks(&fixture, BUTTON_ACTIVE, 99, true);
        assert(fixture.longs == 0 && request_count == 0); // 495 ms, still held.
        driver_ticks(&fixture, BUTTON_ACTIVE, 1, true);
        assert(fixture.longs == 1 && fixture.long_at - fixture.pressed_at == 500);
        assert(page == LOCK_SCREEN && request_count == 1 && requests[0].kind == PASSPORT_LOCK);
        driver_ticks(&fixture, BUTTON_ACTIVE, 40, true);
        driver_ticks(&fixture, BUTTON_INACTIVE, 40, true);
        assert(fixture.longs == 1 && page == LOCK_SCREEN && focus == 0 && request_count == 1);
    }
    // A repeated press confirmed released before the threshold is still short.
    driver_reset(&fixture, BSP_BTN_DOWN);
    driver_short(&fixture);
    driver_ticks(&fixture, BUTTON_ACTIVE, 99, true);
    driver_ticks(&fixture, BUTTON_INACTIVE, 40, true);
    assert(fixture.released_at - fixture.pressed_at == 495);
    assert(fixture.longs == 0 && page == HOME && focus == 2 && request_count == 0);

    // Repeated UP holds do not move focus on release.
    driver_reset(&fixture, BSP_BTN_UP);
    driver_short(&fixture);
    assert(focus == 3);
    driver_ticks(&fixture, BUTTON_ACTIVE, 142, true);
    driver_ticks(&fixture, BUTTON_INACTIVE, 40, true);
    assert(fixture.longs == 1 && focus == 3 && request_count == 0);

    // The second OK gesture is consumed even when the driver now emits LONG.
    driver_reset(&fixture, BSP_BTN_OK);
    driver_short(&fixture);
    assert(page == MOUSE && request_count == 1 && requests[0].kind == PASSPORT_CANCEL);
    render_ui("");
    driver_ticks(&fixture, BUTTON_ACTIVE, 142, true);
    driver_ticks(&fixture, BUTTON_INACTIVE, 40, true);
    assert(fixture.longs == 1 && page == MOUSE && request_count == 1 && !repeat_active);

    // A complete waking hold only wakes; an awake takeover DOWN hold still locks.
    for (unsigned takeover = 0; takeover < 2; ++takeover) {
        driver_reset(&fixture, BSP_BTN_DOWN);
        driver_short(&fixture);
        awake = takeover != 0;
        ble.network_active = takeover != 0;
        driver_ticks(&fixture, BUTTON_ACTIVE, 142, true);
        driver_ticks(&fixture, BUTTON_INACTIVE, 40, true);
        assert(fixture.longs == 1 && awake);
        if (takeover) {
            assert(page == LOCK_SCREEN && request_count == 2);
            assert(requests[0].kind == PASSPORT_TAKEOVER && requests[1].kind == PASSPORT_LOCK);
        } else assert(page == HOME && focus == 1 && request_count == 0);
    }
    // Queue backlog cannot manufacture LONG from a previously released short tap.
    driver_reset(&fixture, BSP_BTN_DOWN);
    driver_ticks(&fixture, BUTTON_ACTIVE, 28, false);
    driver_ticks(&fixture, BUTTON_INACTIVE, 140, false);
    const unsigned emergencies = emergency_count;
    passport_button_event_t next;
    while (passport_next_button(&next)) dispatch_button(&next);
    assert(fixture.longs == 0 && request_count == 0 && focus == 0 && emergency_count > emergencies);
    for (unsigned i = 0; i < BUTTON_EVENT_MAX; ++i) free(fixture.button.cb_info[i]);
    puts("PASS: pinned button driver -> board queue -> UI; first/repeated holds at 500 ms, short releases, wake/takeover and OK guard");
}

int main(void)
{
    test_main_render_retry();
    test_http_stack_failure();
    test_button_observation();
    test_cold_home_navigation();
    test_home_readiness();
    test_gestures();
    test_idle_screen();
    test_async_lock_recovery();
    test_release_latency();
    test_pinned_button_holds();
    test_repeat();
    test_network_takeover();
    test_appearance();
    test_visible_state();
    test_wifi_ui();
    test_pair_feedback();
    test_sound_ui();
    test_label_navigation();
    time_ms += 250;
    const unsigned before_adc = adc_reads;
    assert(passport_board_diagnostics() && adc_reads == before_adc + 1);
    assert(passport_board_diagnostics() && adc_reads == before_adc + 1);
    time_ms += 180000;
    assert(!passport_board_diagnostics() && adc_reads == before_adc + 1);
    puts("PASS: display retry and buffer lifetime, observed-button health, gestures, bounded queue and three asynchronous lock paths, bounded repeats, appearance, sound and labels");
    return 0;
}
