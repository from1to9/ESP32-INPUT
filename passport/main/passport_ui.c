#include "passport_ui.h"
#include "passport_board.h"
#include "passport_ble.h"
#include "passport_sound.h"
#include "passport_wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

typedef enum { HOME, BLUETOOTH, HOST, CONFIRM_FORGET, PAIR_WINDOW, MOUSE, KEYS, STATUS, LOCK_SCREEN, CONNECTIONS, WIFI, APPEARANCE, WIFI_LIST, WIFI_SAVED, WIFI_ACTION, WIFI_CONFIRM, WIFI_AP, WIFI_DETAILS, WIFI_JOIN_QR, WIFI_URL_QR, WIFI_PENDING, SOUND, HOST_IDENTITY, WIFI_IDENTITY } page_t;
static page_t page = LOCK_SCREEN;
static unsigned focus;
static passport_peer_t target;
static uint32_t target_revision;
static passport_ble_state_t ble;
static passport_wifi_state_t wifi;
static passport_wifi_row_t wifi_target;
static uint32_t wifi_revision, wifi_pending_id;
static page_t wifi_return, status_return = HOME;
static passport_wifi_action_t wifi_confirmation;
static bool confirmation_armed;
static uint64_t confirmation_visible_after = UINT64_MAX;
static char ap_ssid[32], ap_password[17], wifi_note[64];
static uint64_t wifi_note_until;
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
static bool sound_pending, pending_sound, sound_save_failed;
static bool repeat_active, ok_held;
static uint64_t ok_pressed_at, repeat_until, repeat_next;
static uint32_t repeat_generation;
static unsigned repeat_direction;
static const int8_t move_x[] = {0, -12, 0, 12, 0};
static const int8_t move_y[] = {-12, 0, 0, 0, 12};
static const char *const home[] = {"Mouse", "Keys", "Connections", "Status"};
static const char *const key_names[] = {"Esc", "Enter", "Tab", "Backspace", "Up", "Left", "Right", "Down", "Back"};
static const uint8_t key_usages[] = {41, 40, 43, 42, 82, 80, 79, 81};

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
    confirmation_armed = false;
    confirmation_visible_after = UINT64_MAX;
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
    const bool was_ready = ble.ready, release_was_unknown = ble.release_unknown;
    const passport_wifi_phase_t previous_phase = wifi.phase;
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
    passport_wifi_snapshot(&wifi);
    if (awake && page != LOCK_SCREEN && !ble.locked) {
        if ((!was_ready && ble.ready) || (previous_phase != PW_ONLINE && wifi.phase == PW_ONLINE)) {
            passport_sound_cue(PASSPORT_CUE_SUCCESS);
        } else if ((!release_was_unknown && ble.release_unknown) ||
                   ((previous_phase == PW_CONNECTING || previous_phase == PW_ASSOCIATED) &&
                    wifi.phase == PW_OFFLINE && wifi.disconnect_reason)) {
            passport_sound_cue(PASSPORT_CUE_ERROR);
        }
    }
    if (wifi.pending && wifi.pending_id != wifi_pending_id && page != LOCK_SCREEN) {
        wifi_pending_id = wifi.pending_id;
        navigate(WIFI_PENDING);
        memset(suppress_click, true, sizeof(suppress_click));
    }
    if (page == WIFI_PENDING && (!wifi.pending || wifi.pending_id != wifi_pending_id)) navigate(WIFI_AP);
}

static unsigned entries(void)
{
    switch (page) {
    case HOME: return 4;
    case CONNECTIONS: return 5;
    case APPEARANCE: return 3;
    case SOUND: return 4;
    case BLUETOOTH: return ble.peer_count + 2;
    case HOST: return 5;
    case CONFIRM_FORGET: return 2;
    case WIFI: return 5;
    case WIFI_LIST: return (wifi.scan_known ? wifi.scan_count : 0) + 2;
    case WIFI_SAVED: return wifi.profile_count + 1;
    case WIFI_ACTION: return wifi_target.profile_id ? 5 : 3;
    case WIFI_CONFIRM: case WIFI_PENDING: return 2;
    case WIFI_AP: return 5;
    case MOUSE: return 6;
    case KEYS: return 9;
    default: return 1;
    }
}

static void wifi_request(passport_wifi_action_t action, uint32_t id, uint32_t revision)
{
    passport_wifi_command_t command = {action, id, revision};
    if (!wifi.initialized || !passport_wifi_request(&command)) {
        passport_sound_cue(PASSPORT_CUE_ERROR);
        snprintf(wifi_note, sizeof(wifi_note), "%s", wifi.initialized ? "Wi-Fi queue full; try again" : "Wi-Fi unavailable");
        wifi_note_until = esp_timer_get_time() / 1000 + 3000;
    } else wifi_note[0] = 0;
}

static void wifi_select(unsigned index, bool saved)
{
    if (saved) {
        passport_wifi_profile_t *profile = &wifi.profiles[index];
        wifi_target = (passport_wifi_row_t){.ssid = profile->ssid, .profile_id = profile->id, .security = profile->security};
    } else wifi_target = wifi.rows[index];
    wifi_revision = wifi.revision;
    wifi_return = saved ? WIFI_SAVED : WIFI_LIST;
    navigate(WIFI_ACTION);
}

static void activate(void)
{
    switch (page) {
    case HOME:
        status_return = HOME;
        navigate((page_t[]){MOUSE, KEYS, CONNECTIONS, STATUS}[focus]);
        break;
    case CONNECTIONS:
        navigate((page_t[]){BLUETOOTH, WIFI, SOUND, APPEARANCE, HOME}[focus]);
        break;
    case SOUND:
        if (focus == 3) navigate(CONNECTIONS);
        else if (focus == 2) passport_sound_cue(PASSPORT_CUE_CONFIRM);
        else if (focus == 0 || passport_sound_state().output != PASSPORT_SOUND_STOPPING) {
            pending_sound = focus == 1;
            sound_pending = true;
        }
        break;
    case APPEARANCE:
        if (focus == 2) navigate(CONNECTIONS);
        else {
            pending_theme = focus == 0 ? PASSPORT_DARK : PASSPORT_LIGHT;
            theme_pending = true;
            snprintf(theme_note, sizeof(theme_note), "Saving appearance...");
        }
        break;
    case WIFI:
        if (focus == 0) { wifi_request(PW_SCAN, 0, 0); navigate(WIFI_LIST); }
        else if (focus == 1) navigate(WIFI_SAVED);
        else if (focus == 2) { wifi_request(PW_USE_AP, 0, 0); navigate(WIFI_AP); }
        else if (focus == 3) { status_return = WIFI; navigate(STATUS); }
        else navigate(CONNECTIONS);
        break;
    case WIFI_LIST: {
        unsigned count = wifi.scan_known ? wifi.scan_count : 0;
        if (focus < count) wifi_select(focus, false);
        else if (focus == count) wifi_request(PW_SCAN, 0, 0);
        else { wifi_request(PW_SCAN_CANCEL, 0, 0); navigate(WIFI); }
        break;
    }
    case WIFI_SAVED:
        if (focus < wifi.profile_count) wifi_select(focus, true);
        else navigate(WIFI);
        break;
    case WIFI_ACTION:
        if (!wifi_target.profile_id) {
            if (!focus && wifi_target.security != PW_UNSUPPORTED && wifi_target.ssid.length) { wifi_request(PW_SETUP_OPEN, 0, 0); navigate(WIFI_AP); }
            else if (focus == 1) navigate(WIFI_IDENTITY);
            else if (focus == 2) navigate(wifi_return);
        } else if (focus == 4) navigate(wifi_return);
        else if (focus == 3) navigate(WIFI_IDENTITY);
        else if (focus == 1) { wifi_request(PW_DISCONNECT, wifi_target.profile_id, wifi_revision); navigate(WIFI); }
        else { wifi_confirmation = focus == 0 ? PW_CONNECT : PW_FORGET; navigate(WIFI_CONFIRM); }
        break;
    case WIFI_IDENTITY: navigate(WIFI_ACTION); break;
    case HOST_IDENTITY: navigate(HOST); break;
    case WIFI_CONFIRM:
        if (confirmation_armed && focus == 1) wifi_request(wifi_confirmation, wifi_target.profile_id, wifi_revision);
        navigate(wifi_return);
        break;
    case WIFI_PENDING:
        if (confirmation_armed) wifi_request(focus ? PW_CONFIRM : PW_REJECT, wifi_pending_id, wifi.setup_id);
        navigate(WIFI_AP);
        break;
    case WIFI_AP:
        if (focus == 0) navigate(WIFI_JOIN_QR);
        else if (focus == 1) navigate(WIFI_URL_QR);
        else if (focus == 2) wifi_request(wifi.setup_open ? PW_SETUP_CLOSE : PW_SETUP_OPEN, 0, 0);
        else if (focus == 3) navigate(WIFI_DETAILS);
        else navigate(WIFI);
        break;
    case WIFI_DETAILS: case WIFI_JOIN_QR: case WIFI_URL_QR: navigate(WIFI_AP); break;
    case STATUS: navigate(status_return); break;
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
        else if (focus == 3) navigate(HOST_IDENTITY);
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
        if (event->button != PASSPORT_OK) confirmation_armed = false;
        if (event->button == PASSPORT_OK && (page == WIFI_CONFIRM || page == WIFI_PENDING)) {
            confirmation_armed = !wake_gesture && atomic_load(&text_applied) && event->time_ms > confirmation_visible_after;
        }
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
                else if (page == HOST_IDENTITY) navigate(HOST);
                else if (page == WIFI_IDENTITY) navigate(WIFI_ACTION);
                else if (page == STATUS) navigate(status_return);
                else if (page == BLUETOOTH || page == WIFI || page == APPEARANCE || page == SOUND) navigate(CONNECTIONS);
                else if (page == CONFIRM_FORGET) navigate(HOST);
                else if (page == WIFI_ACTION || page == WIFI_CONFIRM) navigate(wifi_return);
                else if (page == WIFI_LIST || page == WIFI_SAVED || page == WIFI_AP) navigate(WIFI);
                else if (page == WIFI_PENDING) { wifi_request(PW_REJECT, wifi_pending_id, wifi.setup_id); navigate(WIFI_AP); }
                else if (page == WIFI_DETAILS || page == WIFI_JOIN_QR || page == WIFI_URL_QR) navigate(WIFI_AP);
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
    bool bounded_list = page == WIFI_LIST || page == WIFI_SAVED;
    if (event->button == PASSPORT_UP) focus = bounded_list ? (focus ? focus - 1 : 0) : (focus + entries() - 1) % entries();
    else if (event->button == PASSPORT_DOWN) focus = bounded_list ? (focus + 1 < entries() ? focus + 1 : focus) : (focus + 1) % entries();
    else {
        if ((page == WIFI_CONFIRM || page == WIFI_PENDING) && !confirmation_armed) return;
        page_t previous = page;
        activate();
        if (page != previous && (previous == HOME || previous == CONNECTIONS ||
                                 previous == BLUETOOTH || previous == SOUND)) {
            passport_sound_cue(PASSPORT_CUE_CONFIRM);
        }
    }
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

static void saved_wifi_label(uint32_t profile, char *label)
{
    const passport_name_key_t key = {.kind = PN_WIFI_PROFILE, .profile = profile};
    passport_names_lookup(&key, label);
}

static void host_label(const passport_peer_t *peer, char *label)
{
    passport_name_key_t key = {.kind = PN_HID_BOND};
    memcpy(key.bond, peer->bond_id, sizeof(key.bond));
    passport_names_lookup(&key, label);
}

static void identity_page(const char *title)
{
    view.layout = VIEW_IDENTITY;
    view.count = 0;
    snprintf(view.title, sizeof(view.title), "%s", title);
    view.target[0] = 0;
    snprintf(view.ok, sizeof(view.ok), "Back");
}

static void build_view(const char *http)
{
    memset(&view, 0, sizeof(view));
    view.theme = theme;
    snprintf(view.wifi, sizeof(view.wifi), "%s", !wifi.initialized ? "WiFi ?" : wifi.phase == PW_ONLINE ? "WiFi IP" : wifi.ap_started ? "WiFi AP" : "WiFi --");
    view.wifi_tone = wifi.phase == PW_ONLINE ? TONE_GOOD : wifi.ap_started ? TONE_WAIT : TONE_MUTED;
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
        item(2, home[2], wifi.phase == PW_ONLINE ? "BT / LAN" : wifi.ap_started ? "BT / AP" : "BT / WiFi offline", ICON_CONNECTIONS);
        item(3, home[3], "Device details", ICON_STATUS);
        break;
    case CONNECTIONS:
        snprintf(view.title, sizeof(view.title), "Connections");
        item(0, "Bluetooth", ble.connected ? "Host connected" : ble.pairing ? "Pairing window" : "Saved hosts + pairing", ICON_BLUETOOTH);
        item(1, "Wi-Fi", wifi.ap_started && wifi.phase == PW_OFFLINE ? "Passport AP active" : passport_wifi_phase_name(wifi.phase), ICON_WIFI);
        item(2, "Sound", "Local speaker cues", ICON_SOUND);
        item(3, "Appearance", theme == PASSPORT_DARK ? "Dark theme" : "Light theme", ICON_APPEARANCE);
        item(4, "Back", "Your controls", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 4 ? "Back" : "Open");
        break;
    case SOUND: {
        passport_sound_state_t sound = passport_sound_state();
        const bool ready = sound.output == PASSPORT_SOUND_READY;
        static const char *const messages[] = {
            "Sound off", "Starting speaker...", "Sound on", "Stopping speaker...", "Speaker unavailable",
        };
        snprintf(view.title, sizeof(view.title), "Sound");
        snprintf(view.target, sizeof(view.target), "Local speaker");
        item(0, "Off", "Mute interaction cues", ICON_SOUND);
        item(1, "On", "Enable interaction cues", ICON_SOUND);
        item(2, "Test cue", ready ? "Play a short tone" :
             sound.output == PASSPORT_SOUND_OFF ? "Enable sound to play" : "Speaker must be ready", ICON_SOUND);
        item(3, "Back", "Connections", ICON_BACK);
        if (sound.preference_known) view.items[sound.enabled ? 1 : 0].checked = true;
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 3 ? "Back" : focus == 2 ? ready ? "Play" : "Blocked" :
                 focus == 1 && sound.output == PASSPORT_SOUND_STOPPING ? "Wait" : "Apply");
        snprintf(view.message, sizeof(view.message), "%s", sound_pending ? "Saving sound preference..." : messages[sound.output]);
        snprintf(view.detail, sizeof(view.detail), "%s", sound_save_failed ? "Sound save unconfirmed" :
                 !sound.preference_known ? "Sound preference unavailable" : "");
        view.tone = !sound.preference_known || sound.output == PASSPORT_SOUND_UNAVAILABLE ? TONE_ERROR :
                    sound_pending ? TONE_WAIT : ready ? TONE_GOOD : sound.output == PASSPORT_SOUND_OFF ? TONE_MUTED : TONE_WAIT;
        break;
    }
    case BLUETOOTH:
        snprintf(view.title, sizeof(view.title), "Bluetooth");
        item(0, "Pair new host", "Open a 60-second window", ICON_ADD);
        for (unsigned i = 0; i < ble.peer_count; ++i) {
            bool connected = ble.connected && !strcmp(ble.current, ble.peers[i].label);
            char label[PASSPORT_NAME_BYTES + 1];
            host_label(&ble.peers[i], label);
            item(i + 1, label[0] ? label : ble.peers[i].label,
                 label[0] ? ble.peers[i].label : connected ? "Connected host" : "Saved host", ICON_BLUETOOTH);
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
        item(0, "Advertise", "Only this host can reconnect", ICON_BLUETOOTH);
        item(1, "Disconnect", "Keep the saved bond", ICON_CONNECTIONS);
        item(2, "Forget", "Review before removal", ICON_STATUS);
        item(3, "Host details", "Label and original identity", ICON_STATUS);
        item(4, "Back", "Bluetooth hosts", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "%s", (const char *const[]){"Start", "End link", "Forget", "Open", "Back"}[focus]);
        break;
    case HOST_IDENTITY: {
        char label[PASSPORT_NAME_BYTES + 1];
        host_label(&target, label);
        identity_page("Host details");
        if (label[0]) snprintf(view.body, sizeof(view.body), "%s\n\nBond identity\n%s", label, target.label);
        else snprintf(view.body, sizeof(view.body), "Bond identity\n%s", target.label);
        break;
    }
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
        snprintf(view.title, sizeof(view.title), "Wi-Fi");
        item(0, "Scan networks", "2.4 GHz / bounded scan", ICON_WIFI);
        item(1, "Saved networks", wifi.storage_known ? "Connect or forget a profile" : "Storage unavailable", ICON_STATUS);
        item(2, "Use Passport Wi-Fi", "Direct AP connection", ICON_CONNECTIONS);
        item(3, "Network status", "Observed link and IP state", ICON_STATUS);
        item(4, "Back", "Connections", ICON_BACK);
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 4 ? "Back" : "Open");
        break;
    case WIFI_LIST:
    case WIFI_SAVED: {
        bool saved = page == WIFI_SAVED;
        unsigned count = saved ? wifi.profile_count : wifi.scan_known ? wifi.scan_count : 0;
        unsigned first = focus >= PASSPORT_LIST_ROWS ? focus - (PASSPORT_LIST_ROWS - 1) : 0;
        unsigned total = count + (saved ? 1 : 2);
        view.count = total - first < PASSPORT_LIST_ROWS ? total - first : PASSPORT_LIST_ROWS;
        view.focus = focus - first;
        snprintf(view.title, sizeof(view.title), "%s", saved ? "Saved networks" : "Nearby networks");
        for (unsigned slot = 0; slot < view.count; ++slot) {
            unsigned index = first + slot;
            if (index < count) {
                char name[72], description[sizeof(view.scan_summary[0])], label[PASSPORT_NAME_BYTES + 1];
                const passport_ssid_t *ssid = saved ? &wifi.profiles[index].ssid : &wifi.rows[index].ssid;
                passport_wifi_ssid_text(ssid, name, sizeof(name));
                if (saved) snprintf(description, sizeof(description), "%s / profile %lu", passport_wifi_security_name(wifi.profiles[index].security), (unsigned long)wifi.profiles[index].id);
                else snprintf(description, sizeof(description), "%s / %d dBm%s", passport_wifi_security_name(wifi.rows[index].security), wifi.rows[index].rssi, wifi.rows[index].profile_id ? " / saved" : "");
                if (saved) saved_wifi_label(wifi.profiles[index].id, label);
                else passport_wifi_scan_label(&wifi, &wifi.rows[index], label);
                item(slot, label[0] ? label : name, label[0] ? name : description, ICON_WIFI);
                if (!saved && label[0]) {
                    snprintf(view.scan_summary[slot], sizeof(view.scan_summary[slot]), "%s", description);
                }
                view.items[slot].checked = saved ? wifi.active_profile == wifi.profiles[index].id : wifi.rows[index].current;
            } else if (!saved && index == count) item(slot, wifi.scanning ? "Scanning..." : "Refresh", wifi.scanning ? "Please wait" : "Scan again", ICON_WIFI);
            else item(slot, "Back", "Wi-Fi", ICON_BACK);
        }
        snprintf(view.ok, sizeof(view.ok), "%s", focus < count ? "Open" : (!saved && focus == count) ? (wifi.scanning ? "Wait" : "Scan") : "Back");
        break;
    }
    case WIFI_ACTION: {
        char name[72]; passport_wifi_ssid_text(&wifi_target.ssid, name, sizeof(name));
        snprintf(view.title, sizeof(view.title), "Network actions");
        snprintf(view.target, sizeof(view.target), "%.31s", name);
        if (wifi_target.profile_id) {
            item(0, "Connect", "Confirm the saved network", ICON_WIFI);
            item(1, "Disconnect", "Switch to Passport AP", ICON_CONNECTIONS);
            item(2, "Forget", "Confirm profile removal", ICON_STATUS);
            item(3, "Network details", "Label and original identity", ICON_STATUS);
            item(4, "Back", "Network list", ICON_BACK);
            snprintf(view.ok, sizeof(view.ok), "%s", (const char *const[]){"Review", "End link", "Review", "Open", "Back"}[focus]);
        } else {
            bool can_setup = wifi_target.security != PW_UNSUPPORTED && wifi_target.ssid.length;
            item(0, "Configure via AP", wifi_target.security == PW_UNSUPPORTED ? "Unsupported security" : !wifi_target.ssid.length ? "Hidden SSID unsupported" : "Use the local web form", ICON_WIFI);
            item(1, "Network details", "Label and original identity", ICON_STATUS);
            item(2, "Back", "Network list", ICON_BACK);
            snprintf(view.ok, sizeof(view.ok), "%s", focus == 2 ? "Back" : focus == 1 ? "Open" : can_setup ? "Setup" : "Blocked");
        }
        break;
    }
    case WIFI_IDENTITY: {
        char name[72], label[PASSPORT_NAME_BYTES + 1], identity[48];
        passport_wifi_ssid_text(&wifi_target.ssid, name, sizeof(name));
        if (wifi_return == WIFI_SAVED) {
            saved_wifi_label(wifi_target.profile_id, label);
            snprintf(identity, sizeof(identity), "Profile %lu", (unsigned long)wifi_target.profile_id);
        } else {
            passport_wifi_scan_label(&wifi, &wifi_target, label);
            const uint8_t *b = wifi_target.bssid;
            snprintf(identity, sizeof(identity), "%02X:%02X:%02X:%02X:%02X:%02X / auth %u", b[0], b[1], b[2], b[3], b[4], b[5], wifi_target.auth);
        }
        identity_page("Network details");
        snprintf(view.body, sizeof(view.body), "%s%sOriginal SSID\n%s\n%s\n%s",
                 label, label[0] ? "\n" : "", name, passport_wifi_security_name(wifi_target.security), identity);
        break;
    }
    case WIFI_CONFIRM:
    case WIFI_PENDING: {
        char name[72];
        passport_wifi_ssid_text(page == WIFI_PENDING ? &wifi.pending_ssid : &wifi_target.ssid, name, sizeof(name));
        view.layout = VIEW_NETWORK_CONFIRM;
        snprintf(view.title, sizeof(view.title), "%s", page == WIFI_PENDING ? "Confirm request" : wifi_confirmation == PW_FORGET ? "Forget network" : "Connect network");
        snprintf(view.heading, sizeof(view.heading), "Exact network");
        snprintf(view.body, sizeof(view.body), "%s\n%s\n%s %lu", name,
                 passport_wifi_security_name(page == WIFI_PENDING ? wifi.pending_security : wifi_target.security),
                 page == WIFI_PENDING ? "Request" : "Profile", (unsigned long)(page == WIFI_PENDING ? wifi_pending_id : wifi_target.profile_id));
        item(0, "Cancel", "", ICON_BACK); item(1, "Confirm", "", ICON_CHECK);
        snprintf(view.ok, sizeof(view.ok), "%s", focus ? "Confirm" : "Cancel");
        break;
    }
    case WIFI_AP:
        snprintf(view.title, sizeof(view.title), "Passport Wi-Fi");
        item(0, "Join Wi-Fi QR", "Scan with your phone", ICON_WIFI);
        item(1, "Page address QR", "Open after joining Wi-Fi", ICON_STATUS);
        item(2, wifi.setup_open ? "Close setup" : "Open setup", wifi.provisioning_available ? "Two-minute local window" : "Web setup unavailable", ICON_ADD);
        item(3, "Manual details", "SSID, password and address", ICON_STATUS);
        item(4, "Back", "Wi-Fi", ICON_BACK);
        if (wifi.setup_open) snprintf(view.items[2].subtitle, sizeof(view.items[2].subtitle), "%u s / local confirmation", wifi.setup_seconds);
        snprintf(view.ok, sizeof(view.ok), "%s", focus == 4 ? "Back" : focus == 2 ? wifi.setup_open ? "Close" : "Setup" : "Open");
        break;
    case WIFI_JOIN_QR:
    case WIFI_URL_QR:
        view.layout = VIEW_QR; view.count = 0;
        passport_wifi_ap_credentials(ap_ssid, ap_password);
        snprintf(view.title, sizeof(view.title), "%s", page == WIFI_JOIN_QR ? "Join Passport Wi-Fi" : "Open local page");
        if (wifi.ap_started && wifi.dhcp_running) {
            if (page == WIFI_JOIN_QR) snprintf(view.qr, sizeof(view.qr), "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_password);
            else snprintf(view.qr, sizeof(view.qr), "http://%s/%s", wifi.ap_ip, wifi.provisioning_available ? "setup" : "health");
        } else {
            view.layout = VIEW_MESSAGE; view.hero_icon = ICON_WIFI;
            snprintf(view.heading, sizeof(view.heading), "AP address unavailable");
            snprintf(view.body, sizeof(view.body), "Wait for AP and DHCP,\nor return to Manual details.");
        }
        snprintf(view.ok, sizeof(view.ok), "Back");
        break;
    case WIFI_DETAILS:
    case STATUS:
        view.layout = VIEW_DETAILS; view.count = 0;
        snprintf(view.title, sizeof(view.title), "%s", page == STATUS ? "Device status" : "AP details");
        if (page == WIFI_DETAILS) {
            passport_wifi_ap_credentials(ap_ssid, ap_password);
            snprintf(view.body, sizeof(view.body), "SSID: %s\nPassword: %s\nAP IP: %s\nDHCP server: %s\nClient lease: %s\nhttp://%s/%s", ap_ssid, ap_password, wifi.ap_started ? wifi.ap_ip : "unavailable", wifi.dhcp_running ? "running" : "unavailable", wifi.ap_lease ? wifi.ap_client_ip : "not observed", wifi.ap_ip, wifi.provisioning_available ? "setup" : "health");
        } else snprintf(view.body, sizeof(view.body), "%s\nSTA IP: %s\nAP: %s\nDHCP: %s\nClient lease: %s\nHTTP: %s%s", passport_wifi_phase_name(wifi.phase), wifi.sta_ip[0] ? wifi.sta_ip : "none", wifi.ap_started ? wifi.ap_ip : "not started", wifi.dhcp_running ? "running" : "not running", wifi.ap_lease ? "assigned" : "not observed", http, ble.release_unknown ? "\nPrevious release unknown" : "");
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
    if (page == WIFI || page == WIFI_IDENTITY || (page >= WIFI_LIST && page <= WIFI_PENDING)) {
        snprintf(view.message, sizeof(view.message), "%s", !wifi.initialized ? "Wi-Fi unavailable" : wifi.ap_started && wifi.phase == PW_OFFLINE ? "Passport AP active" : passport_wifi_phase_name(wifi.phase));
        snprintf(view.detail, sizeof(view.detail), "%s", wifi_note[0] && (uint64_t)(esp_timer_get_time() / 1000) < wifi_note_until ? wifi_note : wifi.result);
        if (page != WIFI_ACTION && page != WIFI_IDENTITY) snprintf(view.target, sizeof(view.target), "%s", (page >= WIFI_AP && wifi.ap_started) ? wifi.ap_ip : wifi.phase == PW_ONLINE ? wifi.sta_ip : wifi.ap_started ? "Passport AP active" : "No active Wi-Fi address");
        view.tone = !wifi.initialized || !wifi.storage_known ? TONE_ERROR : wifi.phase == PW_ONLINE ? TONE_GOOD : TONE_WAIT;
    }
    if ((page == BLUETOOTH || page == HOST_IDENTITY) && ble.storage_known &&
        !passport_names_catalog(PN_BLE).known) {
        snprintf(view.detail, sizeof(view.detail), "Label storage unavailable");
        view.tone = TONE_ERROR;
    }
    if ((page == WIFI_LIST || page == WIFI_SAVED || page == WIFI_IDENTITY) && wifi.storage_known &&
        !passport_names_catalog(PN_WIFI).known) {
        snprintf(view.detail, sizeof(view.detail), "Label storage unavailable");
        view.tone = TONE_ERROR;
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
        passport_sound_cue(PASSPORT_CUE_ERROR);
        snprintf(theme_note, sizeof(theme_note), "Theme save unconfirmed");
    }
}

static void apply_sound(void)
{
    if (!sound_pending || !atomic_load(&text_applied)) return;
    sound_pending = false;
    sound_save_failed = passport_sound_set(pending_sound) != ESP_OK;
    if (sound_save_failed && pending_sound) passport_sound_cue(PASSPORT_CUE_ERROR);
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

static void render_ui(const char *http)
{
    build_view(http);
    if (memcmp(&view, &rendered_view, sizeof(view)) || !atomic_load(&text_applied)) {
        bool applied = passport_render(&view);
        atomic_store(&text_applied, applied);
        if (applied) rendered_view = view;
    }
    if ((page == WIFI_CONFIRM || page == WIFI_PENDING) && atomic_load(&text_applied) && confirmation_visible_after == UINT64_MAX) confirmation_visible_after = esp_timer_get_time() / 1000;
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

void passport_ui_run(const char *http, bool buttons_available)
{
    last_activity = esp_timer_get_time() / 1000;
    if (passport_theme_load(&theme) != ESP_OK) {
        theme_failed = true;
        snprintf(theme_note, sizeof(theme_note), "Theme load failed; using dark");
    }
    passport_sound_load();
    bool pairing_sent = false;
    for (;;) {
        observe_owner();
        if (!buttons_available || passport_button_overflow()) {
            passport_ble_emergency_lock();
            enter_lock_screen();
        }
        render_ui(http);
        apply_theme();
        apply_sound();
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
        if (awake && !wifi.setup_open && (uint64_t)(esp_timer_get_time() / 1000) - last_activity >= 60000) {
            awake = false;
            enter_lock_screen();
            passport_backlight(false);
            passport_ble_emergency_lock();
        }
    }
}

bool passport_ui_text_applied(void) { return atomic_load(&text_applied); }
