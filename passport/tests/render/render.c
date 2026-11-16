// Native LVGL draws the production widgets. SDK/BSP boundaries are fixtures.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "../../main/passport_board.c"
#include "../../main/passport_ui.c"
static lv_display_t *display;
static uint16_t pixels[240 * 320];
static bool display_locked;
static unsigned updates;
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *map)
{
    (void)area; (void)map; ++updates; lv_display_flush_ready(d);
}
esp_err_t bsp_display_init(void) { return ESP_OK; }
lv_display_t *bsp_lvgl_init(void) { return display; }
bool bsp_lvgl_lock(int ms) { (void)ms; assert(!display_locked); display_locked = true; return true; }
void bsp_lvgl_unlock(void) { assert(display_locked); display_locked = false; }
void bsp_display_backlight(uint8_t n) { (void)n; }
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *arg) { (void)cb; (void)arg; return ESP_OK; }
QueueHandle_t xQueueCreate(unsigned a, unsigned b) { (void)a; (void)b; return NULL; }
void vQueueDelete(QueueHandle_t q) { (void)q; }
int xQueueSend(QueueHandle_t q, const void *v, unsigned t) { (void)q; (void)v; (void)t; return 0; }
int xQueueReceive(QueueHandle_t q, void *v, unsigned t) { (void)q; (void)v; (void)t; return 0; }
int xQueueReset(QueueHandle_t q) { (void)q; return 1; }
void vTaskDelay(unsigned t) { (void)t; }
int64_t esp_timer_get_time(void) { return 1000000; }
void passport_ble_snapshot(passport_ble_state_t *out) { (void)out; }
bool passport_ble_request(const passport_command_t *r) { (void)r; return true; }
void passport_ble_emergency_lock(void) {}
esp_err_t passport_theme_load(passport_theme_t *out) { *out = PASSPORT_DARK; return ESP_OK; }
esp_err_t passport_theme_save(passport_theme_t value) { (void)value; return ESP_OK; }

static unsigned objects(lv_obj_t *object)
{
    unsigned total = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) total += objects(lv_obj_get_child(object, i));
    return total;
}
static void bounds(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a;
    lv_obj_get_coords(object, &a);
    if (a.x1 < 0 || a.y1 < 0 || a.x2 >= 240 || a.y2 >= 320) {
        fprintf(stderr, "Out of screen: %ld,%ld,%ld,%ld\n", (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2);
        assert(false);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) bounds(lv_obj_get_child(object, i));
}
static void capture(const char *name, unsigned count)
{
    render_ui("AP configured / STA idle\nDHCP unverified\nSSID: Passport-ABCD\nPassword: 1234567890ABCDEF\nConfigured URL\nhttp://192.168.4.1/health", "Listener created");
    lv_obj_update_layout(lv_display_get_screen_active(display));
    lv_refr_now(display);
    assert(objects(lv_display_get_screen_active(display)) == count);
    bounds(lv_display_get_screen_active(display));
    unsigned before = updates;
    render_ui("AP configured / STA idle\nDHCP unverified\nSSID: Passport-ABCD\nPassword: 1234567890ABCDEF\nConfigured URL\nhttp://192.168.4.1/health", "Listener created");
    lv_refr_now(display);
    assert(updates == before); // An unchanged model causes no redraw.
    char filename[96];
    snprintf(filename, sizeof(filename), "%s-%s.ppm", theme == PASSPORT_DARK ? "dark" : "light", name);
    FILE *file = fopen(filename, "wb"); assert(file);
    fprintf(file, "P6\n240 320\n255\n");
    for (unsigned n = 0; n < 240 * 320; ++n) {
        unsigned c = pixels[n];
        uint8_t rgb[] = {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    printf("PASS %s: %u retained objects, bounds and unchanged-frame check\n", filename, count);
}
int main(void)
{
    lv_init(); display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(passport_display_init() == ESP_OK);
    unsigned count = objects(lv_display_get_screen_active(display));
    for (unsigned palette = 0; palette < 2; ++palette) {
        theme = (passport_theme_t)palette;
        ble = (passport_ble_state_t){.synced=true, .storage_known=true, .connected=true,
            .encrypted=true, .bonded=true, .keyboard_subscribed=true, .mouse_subscribed=true,
            .ready=true, .peer_count=3, .window_seconds=60};
        snprintf(ble.current, sizeof(ble.current), "C1:22:33:44:55:66/1");
        snprintf(ble.result, sizeof(ble.result), "Connected / input ready");
        for (unsigned i = 0; i < 3; ++i) snprintf(ble.peers[i].label, sizeof(ble.peers[i].label), "C%u:22:33:44:55:66/1", i);
        target = ble.peers[1];
        const struct { const char *name; page_t page; unsigned focus; } cases[] = {
            {"home",HOME,0}, {"mouse",MOUSE,3}, {"keys",KEYS,3},
            {"connections",CONNECTIONS,1}, {"bluetooth",BLUETOOTH,1},
            {"bluetooth-scroll",BLUETOOTH,3}, {"host",HOST,2},
            {"forget",CONFIRM_FORGET,0}, {"wifi",WIFI,0}, {"status",STATUS,0},
            {"appearance",APPEARANCE,palette},
        };
        theme_note[0] = 0; theme_failed = false;
        for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
            page = cases[i].page; focus = cases[i].focus; capture(cases[i].name, count);
        }
        page = APPEARANCE; theme_failed = true;
        snprintf(theme_note, sizeof(theme_note), "Theme save unconfirmed"); capture("appearance-error", count);
        theme_failed = false; theme_note[0] = 0;
        page = PAIR_WINDOW; ble.connected = ble.ready = false; ble.pairing = ble.advertising = true;
        snprintf(ble.result, sizeof(ble.result), "Pairing window open");
        capture("pair", count);
        ble.window_seconds = 1; capture("pair-last-second", count);
        ble.pairing = ble.advertising = false; ble.peer_count = 0; page = BLUETOOTH; focus = 0;
        capture("bluetooth-empty", count);
        ble.storage_known = false;
        snprintf(ble.result, sizeof(ble.result), "Storage unavailable; input blocked");
        capture("bluetooth-error", count);
        page = LOCK_SCREEN; ble.locked = true; ble.storage_known = true;
        snprintf(ble.result, sizeof(ble.result), "Input locked"); capture("lock", count);
        page = STATUS; ble.release_unknown = true; capture("release-unknown", count);
    }
    lv_deinit();
}
