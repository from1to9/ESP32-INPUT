#include "passport_ui.h"
#include "passport_board.h"
#include "passport_ble.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

typedef enum { HOME, BLUETOOTH, HOST, CONFIRM_FORGET, PAIR_WINDOW, MOUSE, KEYS, STATUS, LOCK_SCREEN, CONNECTIONS, WIFI, APPEARANCE } page_t;
static page_t page = LOCK_SCREEN;
static unsigned focus;
static passport_peer_t target;
static uint32_t target_revision;
static passport_ble_state_t ble;
static bool suppress_click[3], awake = true, wake_gesture, down_held, unlock_armed;
static bool unlock_requested;
static uint64_t last_activity;
static uint64_t lock_visible_after = UINT64_MAX;
static bool owner_was_locked;
static uint32_t owner_generation;
static atomic_bool text_applied;
static passport_view_t view, rendered_view;
static passport_theme_t theme = PASSPORT_DARK, pending_theme;
static bool theme_pending, theme_failed;
static char theme_note[48];
static bool repeat_active, ok_held;
static uint64_t ok_pressed_at, repeat_until, repeat_next;
static uint32_t repeat_generation;
static unsigned repeat_direction;
static const int8_t move_x[] = {0, -12, 0, 12, 0};
static const int8_t move_y[] = {-12, 0, 0, 0, 12};
static const char *const home[] = {"Mouse", "Keys", "Connections", "Status"};
static const char *const key_names[] = {"Esc", "Enter", "Tab", "Backspace", "Up", "Left", "Right", "Down", "Back"};
static const uint8_t key_usages[] = {41, 40, 43, 42, 82, 80, 79, 81};
static const char *const actions[] = {"Advertise", "Disconnect", "Forget", "Back"};

static void request(passport_command_kind_t kind, int x, int y, unsigned usage)
{
    passport_command_t command = {
        .kind = kind, .generation = ble.generation, .peer_type = target.type,
        .bond_revision = target_revision,
        .x = x, .y = y, .usage = usage,
    };
    memcpy(command.peer, target.address, 6);
    if (!passport_ble_request(&command)) {
        repeat_active = false;
        passport_ble_emergency_lock();
    }
}

static void navigate(page_t next)
{
    if (page == PAIR_WINDOW && next != PAIR_WINDOW) request(PASSPORT_STOP_PAIR, 0, 0, 0);
    repeat_active = false;
    request(PASSPORT_CANCEL, 0, 0, 0);
    page = next;
    focus = 0;
}

static void enter_lock_screen(void)
{
    page = LOCK_SCREEN;
    focus = 0;
    unlock_armed = unlock_requested = down_held = wake_gesture = false;
    repeat_active = ok_held = false;
    memset(suppress_click, true, sizeof(suppress_click));
    lock_visible_after = UINT64_MAX;
    atomic_store(&text_applied, false);
}

static void observe_owner(void)
{
    passport_ble_snapshot(&ble);
    if (ble.locked && (!owner_was_locked || owner_generation != ble.generation || page != LOCK_SCREEN)) {
        enter_lock_screen();
    } else if (page == LOCK_SCREEN && unlock_requested && !ble.locked) {
        page = HOME;
        focus = 0;
        unlock_requested = false;
    }
    owner_was_locked = ble.locked;
    owner_generation = ble.generation;
}

static unsigned entries(void)
{
    switch (page) {
    case HOME: return 4;
    case CONNECTIONS: return 4;
    case APPEARANCE: return 3;
    case BLUETOOTH: return ble.peer_count + 2;
    case HOST: return 4;
    case CONFIRM_FORGET: return 2;
    case MOUSE: return 6;
    case KEYS: return 9;
    default: return 1;
    }
}

static void activate(void)
{
    switch (page) {
    case HOME:
        navigate((page_t[]){MOUSE, KEYS, CONNECTIONS, STATUS}[focus]);
        break;
    case CONNECTIONS:
        navigate((page_t[]){BLUETOOTH, WIFI, APPEARANCE, HOME}[focus]);
        break;
    case APPEARANCE:
        if (focus == 2) navigate(CONNECTIONS);
        else {
            pending_theme = focus == 0 ? PASSPORT_DARK : PASSPORT_LIGHT;
            theme_pending = true;
            snprintf(theme_note, sizeof(theme_note), "Saving appearance...");
        }
        break;
    case WIFI: navigate(CONNECTIONS); break;
    case BLUETOOTH:
        if (focus == 0) { navigate(PAIR_WINDOW); }
        else if (focus <= ble.peer_count) {
            target = ble.peers[focus - 1];
            target_revision = ble.bond_revision;
            navigate(HOST);
        }
        else navigate(CONNECTIONS);
        break;
    case HOST:
        if (focus == 0) request(PASSPORT_ADVERTISE, 0, 0, 0);
        else if (focus == 1) request(PASSPORT_DISCONNECT, 0, 0, 0);
        else if (focus == 2) navigate(CONFIRM_FORGET);
        else navigate(BLUETOOTH);
        break;
    case CONFIRM_FORGET:
        if (focus == 1) request(PASSPORT_FORGET, 0, 0, 0);
        navigate(BLUETOOTH);
        break;
    case MOUSE:
        if (focus == 5) navigate(HOME);
        else if (focus == 2) request(PASSPORT_CLICK, 0, 0, 0);
        else {
            request(PASSPORT_MOVE, move_x[focus], move_y[focus], 0);
        }
        break;
    case KEYS:
        if (focus == 8) navigate(HOME);
        else request(PASSPORT_KEY, 0, 0, key_usages[focus]);
        break;
    case PAIR_WINDOW:
        navigate(BLUETOOTH);
        break;
    default: navigate(HOME); break;
    }
}

static void input(const passport_button_event_t *event)
{
    last_activity = event->time_ms;
    if (!awake) {
        awake = true;
        wake_gesture = true;
        passport_backlight(true);
    }
    if (event->kind == PASSPORT_PRESS) {
        if (event->button == PASSPORT_OK && !ok_held) {
            ok_held = true;
            ok_pressed_at = event->time_ms;
        } else if (repeat_active) {
            repeat_active = false;
            request(PASSPORT_CANCEL, 0, 0, 0);
        }
        if (!wake_gesture) suppress_click[event->button] = false;
        if (event->button == PASSPORT_DOWN) down_held = true;
        if (event->button == PASSPORT_OK && page == LOCK_SCREEN && ble.locked &&
            !down_held && !wake_gesture && atomic_load(&text_applied) &&
            event->time_ms > lock_visible_after) unlock_armed = true;
    }
    if (event->kind == PASSPORT_RELEASE && event->button == PASSPORT_DOWN) down_held = false;
    if (event->kind == PASSPORT_RELEASE && event->button == PASSPORT_OK) {
        unlock_armed = ok_held = false;
        if (repeat_active) {
            repeat_active = false;
            request(PASSPORT_CANCEL, 0, 0, 0);
        }
    }
    if (wake_gesture) {
        if (event->kind == PASSPORT_CLICKED || event->kind == PASSPORT_DOUBLE) wake_gesture = false;
        // A long waking hold is consumed through release. The next full press is required.
        if (event->kind == PASSPORT_RELEASE) { wake_gesture = false; suppress_click[event->button] = true; }
        return;
    }
    if (event->kind == PASSPORT_LONG) {
        suppress_click[event->button] = true;
        if (event->button == PASSPORT_DOWN) {
            request(PASSPORT_LOCK, 0, 0, 0);
            enter_lock_screen();
        } else if (event->button == PASSPORT_OK) {
            if (ble.locked && unlock_armed) {
                request(PASSPORT_UNLOCK, 0, 0, 0);
                unlock_requested = true;
            }
            else if (!ble.locked && page == MOUSE) {
                if (focus < 5 && focus != 2 && ok_held && ble.ready && atomic_load(&text_applied) &&
                    event->time_ms < ok_pressed_at + 2000) {
                    repeat_active = true;
                    repeat_direction = focus;
                    repeat_generation = ble.generation;
                    repeat_until = ok_pressed_at + 2000;
                    repeat_next = event->time_ms;
                }
            } else if (!ble.locked) {
                if (page == HOME) navigate(CONNECTIONS);
                else if (page == HOST) navigate(BLUETOOTH);
                else if (page == BLUETOOTH || page == WIFI || page == APPEARANCE) navigate(CONNECTIONS);
                else if (page == CONFIRM_FORGET) navigate(HOST);
                else navigate(HOME);
            }
            unlock_armed = false;
        }
        return;
    }
    if (event->kind == PASSPORT_DOUBLE) { suppress_click[event->button] = true; return; }
    if (event->kind != PASSPORT_CLICKED || suppress_click[event->button]) return;
    if (!atomic_load(&text_applied)) return;
    if (page == LOCK_SCREEN) return;
    if (event->button == PASSPORT_UP) focus = (focus + entries() - 1) % entries();
    else if (event->button == PASSPORT_DOWN) focus = (focus + 1) % entries();
    else activate();
}

static const char *readiness(void)
{
    if (!ble.synced) return "Bluetooth unavailable";
    if (!ble.storage_known) return "Bond storage unknown";
    if (ble.locked) return "Input locked";
    if (!ble.connected) return "Disconnected";
    if (!ble.encrypted) return "Connected / unencrypted";
    if (!ble.bonded) return "Encrypted / bond pending";
    if (!ble.keyboard_subscribed || !ble.mouse_subscribed) return "Waiting for subscriptions";
    if (!ble.ready) return "Waiting for release";
    return "Input ready";
}

static void item(unsigned index, const char *name, const char *subtitle, passport_icon_t icon)
{
    passport_item_t *entry = &view.items[index];
    snprintf(entry->title, sizeof(entry->title), "%s", name);
    snprintf(entry->subtitle, sizeof(entry->subtitle), "%s", subtitle);
    entry->icon = icon;
}

static void build_view(const char *network, const char *http)
{
    memset(&view, 0, sizeof(view));
    view.theme = theme;
    view.layout = VIEW_LIST;
    view.count = entries();
    if (focus >= view.count) focus = 0;
    view.focus = focus;
    view.tone = !ble.synced || !ble.storage_known ? TONE_ERROR : ble.ready ? TONE_GOOD : TONE_WAIT;
    view.bluetooth_tone = !ble.synced ? TONE_ERROR : ble.ready ? TONE_GOOD : ble.connected || ble.advertising ? TONE_WAIT : TONE_MUTED;
    snprintf(view.bluetooth, sizeof(view.bluetooth), "%s", !ble.synced ? "BT ?" : ble.ready ? "BT ready" : ble.pairing ? "BT pair" : ble.connected ? "BT link" : ble.advertising ? "BT adv" : "BT idle");
    snprintf(view.message, sizeof(view.message), "%s", readiness());
    snprintf(view.detail, sizeof(view.detail), "%s", ble.result);
    snprintf(view.ok, sizeof(view.ok), "Open");
    if (ble.connected) snprintf(view.target, sizeof(view.target), "Host  %s", ble.current);
    else snprintf(view.target, sizeof(view.target), "No active host");
    switch (page) {
    case HOME:
        view.layout = VIEW_HOME;
        snprintf(view.title, sizeof(view.title), "Your controls");
        item(0, home[0], "Point + click", ICON_MOUSE);
        item(1, home[1], "Quick actions", ICON_KEYS);
        item(2, home[2], !ble.synced ? "BT ? / WiFi ?" : ble.ready ? "Ready / WiFi ?" : ble.connected ? "Link / WiFi ?" : ble.pairing ? "Pair / WiFi ?" : ble.advertising ? "Adv / WiFi ?" : "Idle / WiFi ?", ICON_CONNECTIONS);
        item(3, home[3], "Device details", ICON_STATUS);
        break;
    case CONNECTIONS:
        snprintf(view.title, sizeof(view.title), "Connections");
        item(0, "Bluetooth", ble.connected ? "Host connected" : ble.pairing ? "Pairing window" : "Saved hosts + pairing", ICON_BLUETOOTH);
        item(1, "Wi-Fi", "Live state unknown", ICON_WIFI);
        item(2, "Appearance", theme == PASSPORT_DARK ? "Dark theme" : "Light theme", ICON_APPEARANCE);
        item(3, "Back", "Your controls", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 3 ? "Back" : "Open");
        break;
    case BLUETOOTH:
        snprintf(view.title, sizeof(view.title), "Bluetooth");
        item(0, "Pair new host", "Open a 60-second window", ICON_ADD);
        for (unsigned i = 0; i < ble.peer_count; ++i) {
            bool connected = ble.connected && !strcmp(ble.current, ble.peers[i].label);
            item(i + 1, connected ? "Connected host" : "Saved host", ble.peers[i].label, ICON_BLUETOOTH);
            view.items[i + 1].checked = connected;
        }
        item(ble.peer_count + 1, "Back", "Connections", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "%s", !focus ? "Pair" : focus > ble.peer_count ? "Back" : "Open");
        if (!ble.storage_known) snprintf(view.detail, sizeof(view.detail), "Saved hosts unavailable");
        else if (!ble.peer_count) snprintf(view.detail, sizeof(view.detail), "No saved hosts. Choose Pair new.");
        break;
    case HOST:
        snprintf(view.title, sizeof(view.title), "Saved host");
        snprintf(view.target, sizeof(view.target), "%s", target.label);
        item(0, actions[0], "Only this host can reconnect", ICON_BLUETOOTH);
        item(1, actions[1], "Keep the saved bond", ICON_CONNECTIONS);
        item(2, actions[2], "Review before removal", ICON_STATUS);
        item(3, actions[3], "Bluetooth hosts", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "%s", (const char *const[]){"Start", "End link", "Forget", "Back"}[focus]);
        break;
    case CONFIRM_FORGET:
        view.layout = VIEW_CONFIRM;
        snprintf(view.title, sizeof(view.title), "Forget host");
        snprintf(view.target, sizeof(view.target), "This removes one saved bond");
        snprintf(view.heading, sizeof(view.heading), "Remove this host?");
        snprintf(view.body, sizeof(view.body), "%s\nPair again to reconnect.", target.label);
        view.hero_icon = ICON_BLUETOOTH;
        item(0, "Cancel", "", ICON_BACK); item(1, "Forget", "", ICON_NONE);
        snprintf(view.ok, sizeof(view.ok), "%s", focus ? "Forget" : "Cancel");
        break;
    case PAIR_WINDOW:
        view.layout = VIEW_MESSAGE;
        snprintf(view.title, sizeof(view.title), "Pair a host");
        snprintf(view.target, sizeof(view.target), "Bluetooth / Just Works");
        snprintf(view.heading, sizeof(view.heading), "%s", ble.pairing ? "Ready to pair" : ble.connected ? "Check your host" : "Pairing window");
        if (ble.window_seconds) snprintf(view.hero, sizeof(view.hero), "%u s", ble.window_seconds);
        else view.hero_icon = ICON_BLUETOOTH;
        snprintf(view.body, sizeof(view.body), "Choose Passport Input\nwhile the window is open.");
        item(0, "Cancel / Back", "", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "Cancel");
        break;
    case MOUSE:
        view.layout = VIEW_MOUSE;
        snprintf(view.title, sizeof(view.title), "Mouse");
        for (unsigned i = 0; i < 6; ++i) item(i, (const char *const[]){"Up", "Left", "Click", "Right", "Down", "Back"}[i], "", (passport_icon_t[]){ICON_UP, ICON_LEFT, ICON_CLICK, ICON_RIGHT, ICON_DOWN, ICON_BACK}[i]);
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 2 ? "Click" : focus == 5 ? "Back" : "Move");
        break;
    case KEYS:
        view.layout = VIEW_KEYS;
        snprintf(view.title, sizeof(view.title), "Quick keys");
        for (unsigned i = 0; i < 9; ++i) item(i, key_names[i], "", ICON_NONE);
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 8 ? "Back" : "Send");
        break;
    case APPEARANCE:
        snprintf(view.title, sizeof(view.title), "Appearance");
        snprintf(view.target, sizeof(view.target), "Saved on this Passport");
        item(0, "Dark", "Graphite + mint", ICON_APPEARANCE);
        item(1, "Light", "Warm white + teal", ICON_APPEARANCE);
        item(2, "Back", "Connections", ICON_BACK);
        view.items[theme == PASSPORT_DARK ? 0 : 1].checked = true;
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 2 ? "Back" : "Apply");
        if (theme_note[0]) snprintf(view.detail, sizeof(view.detail), "%s", theme_note);
        if (theme_failed) {
            view.tone = TONE_ERROR;
            snprintf(view.message, sizeof(view.message), "Appearance error");
        } else if (theme_pending) view.tone = TONE_WAIT;
        break;
    case WIFI:
    case STATUS:
        view.layout = VIEW_DETAILS;
        view.count = 0;
        snprintf(view.title, sizeof(view.title), "%s", page == WIFI ? "Wi-Fi" : "Device status");
        snprintf(view.target, sizeof(view.target), "Live Wi-Fi state unknown");
        snprintf(view.body, sizeof(view.body), "%s\nHTTP: %s%s", network, http,
                 ble.release_unknown ? "\nPrevious release unknown" : "");
        snprintf(view.ok, sizeof(view.ok), "Back");
        break;
    case LOCK_SCREEN:
        view.layout = VIEW_MESSAGE;
        view.count = 0;
        snprintf(view.title, sizeof(view.title), "Locked");
        snprintf(view.heading, sizeof(view.heading), "Input paused");
        snprintf(view.body, sizeof(view.body), "Release all buttons.\nThen hold OK to unlock.");
        view.hero_icon = ICON_LOCK;
        snprintf(view.ok, sizeof(view.ok), "Hold");
        break;
    }
}

static void apply_theme(void)
{
    if (!theme_pending || !atomic_load(&text_applied)) return;
    theme_pending = false;
    if (passport_theme_save(pending_theme) == ESP_OK) {
        theme_failed = false;
        theme = pending_theme;
        snprintf(theme_note, sizeof(theme_note), "Theme saved");
    } else {
        theme_failed = true;
        snprintf(theme_note, sizeof(theme_note), "Theme save unconfirmed");
    }
}

static void service_repeat(uint64_t now)
{
    if (!repeat_active) return;
    if (!ok_held || page != MOUSE || !ble.ready || ble.generation != repeat_generation || now >= repeat_until) {
        repeat_active = false;
        request(PASSPORT_CANCEL, 0, 0, 0);
    } else if (now >= repeat_next) {
        request(PASSPORT_MOVE, move_x[repeat_direction], move_y[repeat_direction], 0);
        repeat_next = now + 100; // No catch-up burst after a delayed UI tick.
    }
}

static void render_ui(const char *network, const char *http)
{
    build_view(network, http);
    if (memcmp(&view, &rendered_view, sizeof(view)) || !atomic_load(&text_applied)) {
        bool applied = passport_render(&view);
        atomic_store(&text_applied, applied);
        if (applied) rendered_view = view;
    }
    if (page == LOCK_SCREEN && atomic_load(&text_applied) && lock_visible_after == UINT64_MAX) {
        lock_visible_after = esp_timer_get_time() / 1000;
    }
}

static void dispatch_button(const passport_button_event_t *event)
{
    uint64_t now = esp_timer_get_time() / 1000;
    if (now >= event->time_ms && now - event->time_ms <= 250) input(event);
    else { repeat_active = false; passport_ble_emergency_lock(); }
}

void passport_ui_run(const char *network, const char *http, bool buttons_available)
{
    last_activity = esp_timer_get_time() / 1000;
    if (passport_theme_load(&theme) != ESP_OK) {
        theme_failed = true;
        snprintf(theme_note, sizeof(theme_note), "Theme load failed; using dark");
    }
    bool pairing_sent = false;
    for (;;) {
        observe_owner();
        if (!buttons_available || passport_button_overflow()) {
            passport_ble_emergency_lock();
            enter_lock_screen();
        }
        render_ui(network, http);
        apply_theme();
        // The pairing explanation must be applied to the screen before opening the window.
        if (page == PAIR_WINDOW && !pairing_sent && atomic_load(&text_applied)) {
            request(PASSPORT_PAIR, 0, 0, 0);
            pairing_sent = true;
        }
        if (page != PAIR_WINDOW) pairing_sent = false;
        passport_button_event_t event;
        if (passport_next_button(&event)) {
            dispatch_button(&event);
        }
        service_repeat(esp_timer_get_time() / 1000);
        if (awake && (uint64_t)(esp_timer_get_time() / 1000) - last_activity >= 60000) {
            awake = false;
            enter_lock_screen();
            passport_backlight(false);
            passport_ble_emergency_lock();
        }
    }
}

bool passport_ui_text_applied(void) { return atomic_load(&text_applied); }
