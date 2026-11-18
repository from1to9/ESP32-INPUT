// Native LVGL draws the production widgets. SDK/BSP boundaries are fixtures.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "../names_view_fixture.h"
#include "../../main/passport_board.c"
#include "../../main/passport_ui.c"
static lv_display_t *display;
static uint16_t pixels[240 * 320];
static uint16_t partial_pixels[240 * 16];
static bool partial_render;
static bool display_locked;
static unsigned updates;
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *map)
{
    if (partial_render) {
        const lv_draw_buf_t *buffer = lv_display_get_buf_active(d);
        const unsigned width = lv_area_get_width(area);
        const unsigned height = lv_area_get_height(area);
        assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 240 && area->y2 < 320);
        assert(buffer->header.stride >= width * sizeof(uint16_t));
        assert(buffer->header.stride * height <= sizeof(partial_pixels));
        for (unsigned y = 0; y < height; ++y) {
            memcpy(pixels + (area->y1 + y) * 240 + area->x1,
                   map + y * buffer->header.stride, width * sizeof(uint16_t));
        }
    }
    ++updates;
    lv_display_flush_ready(d);
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
static passport_sound_state_t sound_state;
void passport_sound_load(void) {}
passport_sound_state_t passport_sound_state(void) { return sound_state; }
esp_err_t passport_sound_set(bool enabled) { (void)enabled; return ESP_OK; }
bool passport_sound_cue(passport_cue_t cue) { (void)cue; return false; }

// Screens render from snapshots. Entering the live USB service loop is a fixture error.
void passport_usb_init(void) { assert(false); }
void passport_usb_service(const passport_ble_state_t *b, const passport_wifi_state_t *w,
                          passport_theme_t t, bool known, bool busy)
{
    (void)b; (void)w; (void)t; (void)known; (void)busy;
    assert(false);
}

void passport_wifi_snapshot(passport_wifi_state_t *out) { (void)out; }
bool passport_wifi_request(const passport_wifi_command_t *c) { (void)c; return true; }
void passport_wifi_ap_credentials(char ssid[32], char password[17]) { snprintf(ssid,32,"Passport-ABCD"); snprintf(password,17,"1234567890ABCDEF"); }
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
static lv_obj_t *find_visible_label(lv_obj_t *object, const char *expected)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), expected)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_visible_label(lv_obj_get_child(object, i), expected);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *assert_label_fits(const char *expected)
{
    lv_obj_t *label = find_visible_label(lv_display_get_screen_active(display), expected);
    assert(label);
    lv_point_t size;
    lv_area_t area, parent;
    lv_obj_get_content_coords(label, &area);
    lv_obj_get_content_coords(lv_obj_get_parent(label), &parent);
    lv_text_get_size(&size, expected, lv_obj_get_style_text_font(label, 0),
                     lv_obj_get_style_text_letter_space(label, 0), lv_obj_get_style_text_line_space(label, 0),
                     lv_area_get_width(&area), LV_TEXT_FLAG_NONE);
    assert(size.x <= lv_area_get_width(&area) && size.y <= lv_area_get_height(&area));
    assert(area.x1 >= parent.x1 && area.x2 <= parent.x2 && area.y1 >= parent.y1 && area.y2 <= parent.y2);
    return label;
}

static void assert_scan_row(const char *title, const char *ssid, const char *summary)
{
    lv_obj_t *first = assert_label_fits(title);
    lv_obj_t *second = assert_label_fits(ssid);
    lv_obj_t *third = assert_label_fits(summary);
    assert(lv_obj_get_parent(first) == lv_obj_get_parent(second));
    assert(lv_obj_get_parent(second) == lv_obj_get_parent(third));
    assert(lv_obj_get_y2(first) <= lv_obj_get_y(second));
    assert(lv_obj_get_y2(second) <= lv_obj_get_y(third));
}

static void capture(const char *name, unsigned count)
{
    render_ui("Listener created");
    lv_obj_update_layout(lv_display_get_screen_active(display));
    lv_refr_now(display);
    assert(objects(lv_display_get_screen_active(display)) == count);
    bounds(lv_display_get_screen_active(display));
    unsigned before = updates;
    render_ui("Listener created");
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
int main(int argc, char **argv)
{
    bool setup_only = false, control_only = false, home_only = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--setup-only")) setup_only = true;
        else if (!strcmp(argv[i], "--control-only")) control_only = true;
        else if (!strcmp(argv[i], "--home-only")) home_only = true;
        else if (!strcmp(argv[i], "--partial")) partial_render = true;
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); return 2; }
    }
    lv_init(); display = lv_display_create(240, 320);
    assert(passport_names_text_valid(LONG_DISPLAY_LABEL, 16));
    assert(strlen(MAX_BYTES_LABEL) == PASSPORT_NAME_BYTES &&
           passport_names_text_valid(MAX_BYTES_LABEL, PASSPORT_NAME_BYTES));
    assert(passport_names_text_valid("45°", sizeof("45°") - 1));
    assert(!passport_names_text_valid("\xe5\xad\x97", 3));
    assert(!passport_names_text_valid("L\xe2\x80\xae", 4));
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    if (partial_render) {
        lv_display_set_buffers(display, partial_pixels, NULL, sizeof(partial_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else {
        lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    }
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
        for (unsigned i = 0; i < 3; ++i) {
            snprintf(ble.peers[i].label, sizeof(ble.peers[i].label), "C%u:22:33:44:55:66/1", i);
            ble.peers[i].bond_id[0] = i + 1;
        }
        wifi = (passport_wifi_state_t){.initialized=true,.storage_known=true,.scan_known=true,.scan_count=4,.profile_count=2,.phase=PW_ONLINE,.active_profile=1,.ap_started=true,.dhcp_running=true,.ap_client=true,.ap_lease=true,.setup_open=false,.setup_seconds=0};
        snprintf(wifi.sta_ip,sizeof(wifi.sta_ip),"192.168.1.40"); snprintf(wifi.ap_ip,sizeof(wifi.ap_ip),"192.168.4.1");
        snprintf(wifi.ap_client_ip,sizeof(wifi.ap_client_ip),"192.168.4.2");
        snprintf(wifi.result,sizeof(wifi.result),"Scan complete");
        for (unsigned n=0;n<4;++n) {
            const char *names[]={"Studio LAN","A long network name with 32 byte","Guest network","Enterprise Wi-Fi"};
            wifi.rows[n]=(passport_wifi_row_t){.security=n==2?PW_OPEN:n==3?PW_UNSUPPORTED:PW_WPA2,.rssi=-42-12*n,.profile_id=n<2?n+1:0,.current=n==0};
            wifi.rows[n].bssid[5] = n;
            wifi.rows[n].auth = n == 2 ? 0 : 3;
            wifi.rows[n].ssid.length=strlen(names[n]); memcpy(wifi.rows[n].ssid.bytes,names[n],wifi.rows[n].ssid.length);
            if(n<2) wifi.profiles[n]=(passport_wifi_profile_t){.id=n+1,.ssid=wifi.rows[n].ssid,.security=PW_WPA2};
        }
        wifi_target=wifi.rows[1]; wifi_pending_id=42; wifi.pending_ssid=wifi.rows[1].ssid; wifi.pending_security=PW_WPA2;
        wifi_confirmation=PW_FORGET;
        target = ble.peers[1];
        wifi.provisioning_available = true;
        if (home_only) {
            page=HOME;focus=0;capture("home-ready",count);
            ble.connected=ble.encrypted=ble.bonded=ble.ready=false;
            strcpy(ble.result,"Select Bluetooth to pair");capture("home-offline",count);
            assert(focus==2);
            assert(lv_obj_has_state(lv_obj_get_parent(assert_label_fits("Mouse")),LV_STATE_DISABLED));
            assert(lv_obj_has_state(lv_obj_get_parent(assert_label_fits("Keys")),LV_STATE_DISABLED));
            assert(!lv_obj_has_state(lv_obj_get_parent(assert_label_fits("Connections")),LV_STATE_DISABLED));
            ble.connected=ble.encrypted=true;capture("home-waiting",count);
            ble.bonded=ble.ready=true;focus=0;
            strcpy(ble.result,"Connected / input ready");capture("home-restored",count);
            assert(!lv_obj_has_state(lv_obj_get_parent(assert_label_fits("Mouse")),LV_STATE_DISABLED));
            assert(!lv_obj_has_state(lv_obj_get_parent(assert_label_fits("Keys")),LV_STATE_DISABLED));
            continue;
        }
        if (control_only) {
            ble.network_available=true;page=STATUS;focus=0;capture("control-idle",count);
            ble.network_active=true;strcpy(ble.controller,"13ab45cd");capture("control-active",count);
            page=MOUSE;capture("control-mouse",count);
            ble.network_active=false;ble.locked=true;ble.ready=false;strcpy(ble.result,"Input locked");page=LOCK_SCREEN;capture("control-lock",count);
            continue;
        }
        if (setup_only) {
            wifi.phase=PW_OFFLINE; wifi.setup_open=true; wifi.setup_seconds=120;
            wifi_pending_id=UINT32_MAX;
            page=WIFI_AP; focus=0; capture("setup-open",count);
            page=WIFI_DETAILS; capture("setup-details",count);
            page=WIFI_URL_QR; capture("setup-page-qr",count);
            page=WIFI_PENDING; capture("setup-confirm",count);
            continue;
        }
        const struct { const char *name; page_t page; unsigned focus; } cases[] = {
            {"home",HOME,0}, {"mouse",MOUSE,3}, {"keys",KEYS,3},
            {"connections",CONNECTIONS,2}, {"bluetooth",BLUETOOTH,1},
            {"bluetooth-scroll",BLUETOOTH,3}, {"host",HOST,2},
            {"forget",CONFIRM_FORGET,0}, {"wifi",WIFI,0}, {"status",STATUS,0}, {"about",STATUS,1},
            {"appearance",APPEARANCE,palette},
            {"wifi-scan",WIFI_LIST,2}, {"wifi-saved",WIFI_SAVED,0}, {"wifi-actions",WIFI_ACTION,2},
            {"wifi-confirm",WIFI_CONFIRM,0}, {"wifi-pending",WIFI_PENDING,0}, {"wifi-ap",WIFI_AP,2},
            {"wifi-details",WIFI_DETAILS,0}, {"wifi-join-qr",WIFI_JOIN_QR,0}, {"wifi-page-qr",WIFI_URL_QR,0},
        };
        theme_note[0] = 0; theme_failed = false;
        for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
            page = cases[i].page; focus = cases[i].focus;
            wifi.phase = page >= WIFI_AP ? PW_OFFLINE : PW_ONLINE; capture(cases[i].name, count);
            if (page == STATUS && focus == 1) {
                char revision[24];
                snprintf(revision, sizeof(revision), "Git commit\n%.8s", APP_BUILD_GIT_KNOWN ? APP_BUILD_GIT_COMMIT : PASSPORT_ARCHIVE_COMMIT);
                assert(strstr(view.body, revision));
                assert_label_fits(view.title);
                assert_label_fits(view.body);
            }
            if (page == WIFI_LIST) {
                assert_scan_row("Studio profile", "Studio LAN", "WPA2 / -42 dBm / saved");
                assert_scan_row("Exact AP", "Guest network", "Open / -66 dBm");
                assert_label_fits("WPA2 / -54 dBm / saved");
                assert(view.items[0].checked && !view.items[1].checked && !view.items[2].checked);
            }
        }
        page = SOUND; focus = 0;
        sound_state = (passport_sound_state_t){.preference_known = true};
        capture("sound-off", count);
        focus = 2;
        sound_state = (passport_sound_state_t){.output = PASSPORT_SOUND_READY, .enabled = true, .preference_known = true};
        capture("sound-on", count);
        sound_state.output = PASSPORT_SOUND_UNAVAILABLE;
        capture("sound-unavailable", count);
        focus = 3;
        capture("sound-back", count);
        page = HOST_IDENTITY; focus = 0; capture("host-identity", count);
        assert(strstr(view.body, LONG_DISPLAY_LABEL) && strstr(view.body, target.label));
        page = WIFI_IDENTITY; wifi_return = WIFI_LIST; wifi_target = wifi.rows[2];
        capture("ap-identity", count);
        assert(strstr(view.body, "Exact AP") && strstr(view.body, "00:00:00:00:00:02"));
        assert_label_fits(view.body);
        wifi_target.ssid.length = sizeof(wifi_target.ssid.bytes);
        memset(wifi_target.ssid.bytes, 0xFF, sizeof(wifi_target.ssid.bytes));
        memset(wifi_target.bssid, 0xAA, sizeof(wifi_target.bssid) - 1);
        wifi_target.auth = 3;
        wifi_target.security = PW_WPA2;
        capture("max-label-raw-ssid", count);
        static const char raw_ssid[] = "hex:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";
        assert(strstr(view.body, LONG_DISPLAY_LABEL) && strstr(view.body, raw_ssid));
        assert(strstr(view.body, "WPA2\nAA:AA:AA:AA:AA:02 / auth 3"));
        assert_label_fits(view.body);
        memset(wifi_target.ssid.bytes, 0xDD, sizeof(wifi_target.ssid.bytes));
        capture("max-label-wide-ssid", count);
        static const char wide_ssid[] = "hex:DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD";
        assert(strstr(view.body, LONG_DISPLAY_LABEL) && strstr(view.body, wide_ssid));
        assert(strstr(view.body, "WPA2\nAA:AA:AA:AA:AA:02 / auth 3"));
        assert_label_fits(view.body);
        wifi_target.auth = 26;
        wifi_target.security = PW_UNSUPPORTED;
        capture("max-byte-label-raw-ssid", count);
        assert(strstr(view.body, MAX_BYTES_LABEL) && strstr(view.body, wide_ssid));
        assert(strstr(view.body, "Unsupported\nAA:AA:AA:AA:AA:02 / auth 26"));
        assert_label_fits(view.body);
        activate();
        assert(page == WIFI_ACTION);
        page = WIFI_IDENTITY;
        wifi_return = WIFI_SAVED; wifi_target = wifi.rows[0];
        capture("profile-identity", count);
        assert(strstr(view.body, "Studio profile") && strstr(view.body, "Studio LAN"));
        page = WIFI_SAVED; label_storage_known = false; capture("labels-unavailable", count);
        label_storage_known = true;
        page = APPEARANCE; theme_failed = true;
        snprintf(theme_note, sizeof(theme_note), "Theme save unconfirmed"); capture("appearance-error", count);
        theme_failed = false; theme_note[0] = 0;
        page = PAIR_WINDOW; ble.connected = ble.ready = false; ble.pairing = ble.advertising = true;
        snprintf(ble.result, sizeof(ble.result), "Pairing window open");
        capture("pair", count);
        ble.window_seconds = 1; capture("pair-last-second", count);
        ble.connected = true; ble.advertising = false; ble.encrypted = ble.bonded = false; ble.peer_count = 0;
        capture("pair-securing", count);
        ble.encrypted = ble.bonded = true; ble.pairing = false; ble.window_seconds = 0; ble.peer_count = 1;
        snprintf(ble.result, sizeof(ble.result), "New host saved");
        capture("pair-saved", count);
        ble.connected = ble.encrypted = ble.bonded = false; ble.peer_count = 0;
        snprintf(ble.result, sizeof(ble.result), "Bond admission failed");
        capture("pair-ended", count);
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
