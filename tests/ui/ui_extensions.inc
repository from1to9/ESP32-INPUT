// SPDX-License-Identifier: Apache-2.0
/* Included by ui_visual.c: shared real-pointer, screenshot and intent helpers. */
static app_local_edit_request_t s_private_request;
static app_local_edit_result_t s_private_result;
static size_t s_private_count;
static uint64_t s_test_editor_session = 100;
static bool s_test_editor_lowercase, s_test_editor_symbols;

static app_local_edit_result_t capture_private(const app_local_edit_request_t *request)
{
    assert(request);
    s_private_request = *request; /* Test-only observation, never logged. */
    ++s_private_count;
    return s_private_result;
}

static void assert_local_events_only(void)
{
    for (size_t i = 0; i < s_intent_count; ++i) {
        app_action_t action = s_intents[i].action;
        assert(action == APP_TOUCH_BEGIN || action == APP_TOUCH_END || action == APP_TOUCH_CANCEL);
        assert(s_intents[i].text[0] == '\0');
    }
}

static void assert_secret_not_public(const char *secret)
{
    assert(!contains_text(lv_screen_active(), secret));
    assert(!ui_memory_contains(secret));
    assert(!ui_private_public_state_contains(secret));
}

static void editor_cell(unsigned cell)
{
    assert(cell < 9);
    tap(68 + (int)(cell % 3) * 120, 178 + (int)(cell / 3) * 84);
}

static void editor_type(char character)
{
    clear_intents();
    if (character == ' ') tap_control("Space");
    else {
        bool letter = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        if (s_test_editor_symbols == letter) {
            tap(420, 346);
            s_test_editor_symbols = !letter;
        }
        unsigned group = 0, index = 0;
        if (letter) {
            bool lower = character >= 'a' && character <= 'z';
            if (lower != s_test_editor_lowercase) {
                tap(420, 262);
                s_test_editor_lowercase = lower;
            }
            unsigned offset = (unsigned)(character - (lower ? 'a' : 'A'));
            group = offset / 3; index = offset % 3;
        } else {
            const char *symbols[] = {"12345", "67890", ".,!?", "@#$%", "^&*()", "-_=+", "[]{}", ";:'\"", "/\\|<>`~"};
            bool found = false;
            for (unsigned i = 0; i < 9; ++i) {
                const char *match = strchr(symbols[i], character);
                if (match) { group = i; index = (unsigned)(match - symbols[i]); found = true; break; }
            }
            assert(found);
        }
        editor_cell(group);
        editor_cell(index);
    }
    assert_local_events_only();
    clear_intents();
}

static void editor_type_text(const char *value)
{
    for (size_t i = 0; value[i]; ++i) editor_type(value[i]);
}

static void open_editor(app_local_edit_purpose_t purpose, const char *initial)
{
    /* Lifecycle cases may advance the coordinator-owned session themselves.
     * Never reopen that consumed identity in a later independent fixture. */
    if (s_test_editor_session < s_state.local_editor.session)
        s_test_editor_session = s_state.local_editor.session;
    s_state.mode = INPUT_KEYBOARD;
    s_state.locked = s_state.screen_off = false;
    s_state.overlay = APP_OVERLAY_LOCAL_EDITOR;
    s_state.local_editor = (app_local_editor_view_t){.purpose = purpose,
        .session = ++s_test_editor_session, .seconds_left = 300, .submit_available = true,
        .wifi_scan_generation = 47, .wifi_record_index = 17, .wifi_remember = true};
    if (initial) snprintf(s_state.local_editor.bluetooth_initial,
        sizeof(s_state.local_editor.bluetooth_initial), "%s", initial);
    ++s_state.ui_revision;
    s_private_count = 0;
    memset(&s_private_request, 0, sizeof(s_private_request));
    s_private_result = APP_LOCAL_EDIT_ACCEPTED;
    s_test_editor_lowercase = s_test_editor_symbols = false;
    render();
    clear_intents();
}

static void finish_editor(void)
{
    if (s_test_editor_session < s_state.local_editor.session)
        s_test_editor_session = s_state.local_editor.session;
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.locked = s_state.screen_off = false;
    s_state.local_editor = (app_local_editor_view_t){0};
    ++s_state.ui_revision;
    render();
    assert(ui_private_buffers_cleared());
    memset(&s_private_request, 0, sizeof(s_private_request));
    s_private_count = 0;
    clear_intents();
}

static void assert_terminal(const app_intent_t *begin)
{
    assert(action_count(APP_TOUCH_END) + action_count(APP_TOUCH_CANCEL) == 1);
    const app_intent_t *end = last_action(action_count(APP_TOUCH_END) ? APP_TOUCH_END : APP_TOUCH_CANCEL);
    assert(end->epoch == begin->epoch && end->ui_revision == begin->ui_revision && end->gesture == begin->gesture);
}

static void native_wifi_scan_checks(void)
{
    app_view_t original = s_state;
    ui_fixture_native_networks(&s_state);
    s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_WIFI;
    ++s_state.ui_revision;
    render(); clear_intents();
    tap_control("Find networks");
    assert(action_count(APP_WIFI_SCAN) == 1 && last_action(APP_WIFI_SCAN)->x == 1);
    assert(action_count(APP_WIFI_SETUP) == 0 && s_state.overlay == APP_OVERLAY_SETTINGS);
    s_state.overlay = APP_OVERLAY_WIFI_NETWORKS;
    ++s_state.ui_revision;
    screenshot("native-wifi-list-four-long-rows");
    for (unsigned i = 0; i < 4; ++i) {
        clear_intents(); tap(240, 134 + (int)i * 60);
        assert(action_count(APP_WIFI_NETWORK_SELECT) == 1);
        const app_intent_t *selected = last_action(APP_WIFI_NETWORK_SELECT);
        assert(selected->wifi_scan_generation == 47);
        assert(selected->wifi_record_index == s_state.wifi_scan.rows[i].record_index);
        assert(selected->ui_revision == s_state.ui_revision);
    }
    assert(has_visible_label(lv_screen_active(), "capped"));
    assert(lv_obj_has_state(control("Previous"), LV_STATE_DISABLED));
    clear_intents(); tap_control("Next");
    assert(action_count(APP_WIFI_SCAN_PAGE) == 1 && last_action(APP_WIFI_SCAN_PAGE)->x == 1);
    assert(last_action(APP_WIFI_SCAN_PAGE)->wifi_scan_generation == 47 && s_state.wifi_scan.page == 0);
    s_state.wifi_scan.page = 7;
    ++s_state.ui_revision;
    screenshot("native-wifi-list-last-page");
    assert(lv_obj_has_state(control("Next"), LV_STATE_DISABLED));
    clear_intents(); tap_control("Previous");
    assert(last_action(APP_WIFI_SCAN_PAGE)->x == 6);

    s_state.wifi_scan.page = 0; ++s_state.ui_revision; render();
    clear_intents(); pointer(240, 134, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    s_state.wifi_scan.generation = 48;
    s_state.wifi_scan.rows[0].record_index = 8;
    ++s_state.ui_revision;
    render(); pointer(240, 134, false);
    assert_terminal(&begin);
    assert(action_count(APP_WIFI_NETWORK_SELECT) <= 1);
    if (action_count(APP_WIFI_NETWORK_SELECT)) {
        const app_intent_t *old = last_action(APP_WIFI_NETWORK_SELECT);
        assert(old->wifi_scan_generation == 47 && old->wifi_record_index == 0);
        assert(old->ui_revision == begin.ui_revision && old->gesture == begin.gesture);
    }
    render(); clear_intents(); tap(240, 134);
    assert(last_action(APP_WIFI_NETWORK_SELECT)->wifi_scan_generation == 48);
    assert(last_action(APP_WIFI_NETWORK_SELECT)->wifi_record_index == 8);

    clear_intents(); pointer(240, 134, true);
    begin = *last_action(APP_TOUCH_BEGIN);
    s_state.wifi_scan.cleanup_pending = true;
    strcpy(s_state.wifi_scan.message, "Waiting for scan cleanup.");
    ++s_state.ui_revision;
    render(); pointer(240, 134, false); assert_terminal(&begin);
    /* A hidden row may finish its old press. Its immutable identity and old
     * revision must survive so coordinator rejection cannot retarget it. */
    assert(action_count(APP_WIFI_NETWORK_SELECT) <= 1);
    if (action_count(APP_WIFI_NETWORK_SELECT)) {
        const app_intent_t *old = last_action(APP_WIFI_NETWORK_SELECT);
        assert(old->wifi_scan_generation == 48 && old->wifi_record_index == 8);
        assert(old->ui_revision == begin.ui_revision && old->gesture == begin.gesture);
    }
    screenshot("native-wifi-scan-cleanup-pending");
    s_state.wifi_scan.cleanup_pending = false; s_state.wifi_scan.message[0] = '\0';

    static const struct { app_ui_scan_state_t state; const char *name; const char *text; } phases[] = {
        {APP_UI_SCAN_UNAVAILABLE, "unavailable", "Scan unavailable"},
        {APP_UI_SCAN_IDLE, "idle", "Ready to scan"},
        {APP_UI_SCAN_QUEUED, "queued", "Scan queued"},
        {APP_UI_SCAN_RUNNING, "running", "Scanning nearby"},
        {APP_UI_SCAN_CANCELLING, "cancelling", "Cancelling scan"},
        {APP_UI_SCAN_CANCELLED, "cancelled", "Scan cancelled"},
        {APP_UI_SCAN_ERROR, "error", "Scan failed"},
        {APP_UI_SCAN_EXPIRED, "expired", "Scan expired"},
    };
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        s_state.wifi_scan.state = phases[i].state;
        s_state.wifi_scan.can_cancel = phases[i].state == APP_UI_SCAN_RUNNING;
        s_state.wifi_scan.can_start = phases[i].state == APP_UI_SCAN_IDLE || phases[i].state == APP_UI_SCAN_ERROR;
        char name[80]; snprintf(name, sizeof(name), "native-wifi-scan-%s", phases[i].name);
        screenshot(name);
        assert(has_visible_label(lv_screen_active(), phases[i].text));
        clear_intents(); tap(240, 134);
        assert(action_count(APP_WIFI_NETWORK_SELECT) == 0);
        if (s_state.wifi_scan.can_cancel) {
            clear_intents(); tap_control("Cancel scan");
            assert(action_count(APP_WIFI_SCAN) == 1 && last_action(APP_WIFI_SCAN)->x == 0);
            assert(last_action(APP_WIFI_SCAN)->wifi_scan_generation == 48);
        }
    }
    s_state.wifi_scan = (app_wifi_scan_view_t){.state = APP_UI_SCAN_READY, .generation = 49, .can_start = true};
    screenshot("native-wifi-scan-empty");
    assert(has_visible_label(lv_screen_active(), "No networks found"));
    s_state.wifi_scan.retry_seconds = 5; s_state.wifi_scan.can_start = false;
    screenshot("native-wifi-scan-cooldown");
    assert(lv_obj_has_state(control("Scan cooling down"), LV_STATE_DISABLED));
    s_state = original; render(); clear_intents();
    puts("PASS: native Wi-Fi scan phases, immutable row identity, paging, held generation replacement and truthful empty/unavailable states");
}

static void native_wifi_detail_and_status_checks(void)
{
    app_view_t original = s_state;
    ui_fixture_native_networks(&s_state);
    s_state.overlay = APP_OVERLAY_WIFI_NETWORK;
    ++s_state.ui_revision;
    screenshot("native-wifi-detail-supported");
    const char *join[] = {"Join once", "Remember + join"};
    for (unsigned i = 0; i < 2; ++i) {
        clear_intents(); tap_control(join[i]);
        assert(action_count(APP_WIFI_CREDENTIAL_OPEN) == 1);
        const app_intent_t *intent = last_action(APP_WIFI_CREDENTIAL_OPEN);
        assert(intent->x == (int)i && intent->wifi_scan_generation == 47 && intent->wifi_record_index == 17);
        assert(action_count(APP_WIFI_SETUP) == 0 && s_private_count == 0);
    }
    clear_intents(); pointer(124, 388, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    s_state.wifi_selection.valid = false; ++s_state.ui_revision;
    render(); pointer(124, 388, false); assert_terminal(&begin);
    assert(action_count(APP_WIFI_CREDENTIAL_OPEN) == 0);
    screenshot("native-wifi-detail-expired");
    assert(lv_obj_has_state(control("Join once"), LV_STATE_DISABLED));
    ui_fixture_native_networks(&s_state);
    s_state.wifi_selection.network = s_state.wifi_scan.rows[3];
    s_state.wifi_selection.network.availability = APP_UI_JOIN_UNSUPPORTED_SECURITY;
    screenshot("native-wifi-detail-unsupported-security");
    assert(has_visible_label(lv_screen_active(), "unsupported"));
    for (unsigned i = 0; i < 2; ++i) {
        assert(lv_obj_has_state(control(join[i]), LV_STATE_DISABLED));
        clear_intents(); tap_control(join[i]); assert(action_count(APP_WIFI_CREDENTIAL_OPEN) == 0);
    }
    s_state.wifi_selection.network.availability = APP_UI_JOIN_HIDDEN_SSID;
    strcpy(s_state.wifi_selection.network.label, "Hidden network");
    screenshot("native-wifi-detail-hidden");
    assert(has_visible_label(lv_screen_active(), "manual entry unavailable"));
    s_state.overlay = APP_OVERLAY_SETTINGS; s_state.settings_page = APP_SETTINGS_WIFI;
    ++s_state.ui_revision;
    s_state.wifi_state = APP_UI_WIFI_CONNECTED;
    strcpy(s_state.wifi_ssid, "Current synthetic network");
    strcpy(s_state.wifi_ipv4, "192.0.2.42");
    strcpy(s_state.wifi_lan_service_url, "http://192.0.2.42:8080/");
    screenshot("native-wifi-lan-without-service");
    assert(has_visible_label(lv_screen_active(), "LAN connected"));
    assert(has_visible_label(lv_screen_active(), "Web/API unavailable"));
    assert(!has_visible_label(lv_screen_active(), "http://192.0.2.42:8080/"));
    assert(has_visible_label(lv_screen_active(), "Different saved network"));
    s_state.wifi_lan_service = APP_UI_LAN_SERVICE_READY;
    screenshot("native-wifi-service-ready-fixture");
    assert(has_visible_label(lv_screen_active(), "Web/API ready"));
    assert(has_visible_label(lv_screen_active(), "http://192.0.2.42:8080/"));
    s_state.wifi_state = APP_UI_WIFI_OBTAINING_IP;
    s_state.wifi_lan_ready = false; s_state.wifi_lan_service = APP_UI_LAN_SERVICE_UNAVAILABLE;
    s_state.wifi_ipv4[0] = '\0';
    screenshot("native-wifi-obtaining-address");
    assert(has_visible_label(lv_screen_active(), "address"));
    s_state.wifi_state = APP_UI_WIFI_ERROR; s_state.wifi_storage_uncertain = true;
    strcpy(s_state.wifi_message, "Authentication rejected; saved network unchanged.");
    screenshot("native-wifi-join-error-storage-uncertain");
    assert(has_visible_label(lv_screen_active(), "uncertain"));
    assert(has_visible_label(lv_screen_active(), "Authentication rejected"));
    s_state = original; render(); clear_intents();
    puts("PASS: Wi-Fi detail admission, held expiry, unsupported/hidden networks and distinct LAN/runtime/saved/service truth");
}

static void private_editor_submission_checks(void)
{
    app_view_t original = s_state;
    ui_fixture_native_networks(&s_state);
    strcpy(s_state.preview, "HID draft stays");
    app_ui_set_local_edit_submit(capture_private);
    const char *secret = "Qa7!mZ2@";
    open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
    screenshot("native-password-editor-empty");
    assert(lv_obj_has_state(control("Join"), LV_STATE_DISABLED));
    for (size_t i = 0; i < 7; ++i) editor_type(secret[i]);
    assert(lv_obj_has_state(control("Join"), LV_STATE_DISABLED));
    editor_type(secret[7]);
    assert(ui_private_password_contains(secret)); /* Positive control: actual static bytes. */
    assert_secret_not_public(secret);
    assert(strcmp(s_state.preview, "HID draft stays") == 0);
    assert(!lv_obj_has_state(control("Join"), LV_STATE_DISABLED));
    screenshot("native-password-editor-masked");
    assert(has_visible_label(lv_screen_active(), "8 / 63 bytes"));
    s_private_result = APP_LOCAL_EDIT_BUSY;
    clear_intents(); tap_control("Join");
    assert_local_events_only();
    assert(s_private_count == 1 && s_private_request.purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE);
    assert(s_private_request.value.wifi.length == 8 && strcmp(s_private_request.value.wifi.password, secret) == 0);
    assert(s_private_request.epoch == s_state.epoch && s_private_request.ui_revision == s_state.ui_revision);
    assert(s_private_request.editor_session == s_state.local_editor.session);
    assert(s_private_request.gesture == last_action(APP_TOUCH_BEGIN)->gesture);
    assert(s_private_request.value.wifi.scan_generation == 47 && s_private_request.value.wifi.record_index == 17);
    assert(s_private_request.value.wifi.remember && ui_private_password_contains(secret));
    screenshot("native-password-editor-busy");
    assert_secret_not_public(secret);
    s_private_result = APP_LOCAL_EDIT_INVALID;
    clear_intents(); tap_control("Join");
    assert(s_private_count == 2 && ui_private_password_contains(secret));
    s_private_result = APP_LOCAL_EDIT_UNAVAILABLE;
    clear_intents(); tap_control("Join");
    assert(s_private_count == 3 && ui_private_password_contains(secret));
    screenshot("native-password-editor-unavailable");
    s_private_result = APP_LOCAL_EDIT_ACCEPTED;
    clear_intents(); tap_control("Join");
    assert(s_private_count == 4 && ui_private_buffers_cleared());
    assert_secret_not_public(secret);
    screenshot("native-password-editor-submitted");
    render(); clear_intents(); tap_control("Join");
    assert(s_private_count == 4 && ui_private_buffers_cleared());
    finish_editor();

    open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
    editor_type_text(secret);
    app_ui_set_local_edit_submit(NULL);
    render(); clear_intents(); tap_control("Join");
    assert(s_private_count == 0 && ui_private_password_contains(secret));
    assert(lv_obj_has_state(control("Join"), LV_STATE_DISABLED));
    app_ui_set_local_edit_submit(capture_private);
    render(); s_private_result = APP_LOCAL_EDIT_STALE;
    clear_intents(); tap_control("Join");
    assert(s_private_count == 1 && ui_private_buffers_cleared());
    screenshot("native-password-editor-stale-submit");
    finish_editor();

    /* Every supported printable ASCII byte travels through real local keys.
     * Two bounded submissions exercise case, symbols, spaces and all 63 bytes. */
    for (unsigned chunk = 0; chunk < 2; ++chunk) {
        char expected[64] = {0};
        unsigned first = chunk ? 95 : 32, last = chunk ? 126 : 94;
        size_t length = (size_t)(last - first + 1);
        open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
        s_state.local_editor.wifi_remember = chunk == 0;
        render();
        for (unsigned value = first; value <= last; ++value) {
            expected[value - first] = (char)value;
            editor_type((char)value);
        }
        if (!chunk) {
            editor_type('Q'); /* Full draft must reject, not truncate or replace. */
            screenshot("native-password-editor-full");
            assert(has_visible_label(lv_screen_active(), "63 / 63 bytes"));
        }
        assert_secret_not_public(expected);
        clear_intents(); tap_control("Join"); assert_local_events_only();
        assert(s_private_count == 1 && s_private_request.value.wifi.length == length);
        assert(memcmp(s_private_request.value.wifi.password, expected, length + 1) == 0);
        assert(s_private_request.value.wifi.remember == (chunk == 0));
        assert(ui_private_buffers_cleared());
        finish_editor();
    }
    s_state = original; render(); clear_intents();
    puts("PASS: private editor uses actual pointer keys for all printable ASCII, preserves HID draft, keeps passwords out of public/allocator storage and handles every admission result");
}

static void private_editor_cleanup_checks(void)
{
    app_view_t original = s_state;
    ui_fixture_native_networks(&s_state);
    const char *secret = "Qa7!mZ2@";
    for (unsigned exit = 0; exit < 6; ++exit) {
        open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
        editor_type_text(secret);
        assert(ui_private_password_contains(secret));
        clear_intents(); pointer(396, 448, true);
        app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
        if (exit == 0) s_state.overlay = APP_OVERLAY_MODES; /* AI double/navigation acknowledgement. */
        if (exit == 1) s_state.locked = true;
        if (exit == 2) s_state.screen_off = true;
        if (exit == 3) s_state.local_editor.seconds_left = 0;
        if (exit == 4) {
            ++s_state.local_editor.session;
            ++s_state.local_editor.wifi_scan_generation;
            ++s_state.wifi_selection.generation;
        }
        if (exit == 5) {
            ++s_state.local_editor.session;
            s_state.local_editor.purpose = APP_LOCAL_EDIT_BLUETOOTH_NAME;
            strcpy(s_state.local_editor.bluetooth_initial, "Fresh name");
        }
        ++s_state.ui_revision;
        render();
        assert(!ui_private_password_contains(secret));
        assert_secret_not_public(secret);
        pointer(396, 448, false);
        assert_terminal(&begin);
        assert(s_private_count == 0);
        if (exit < 5) assert(ui_private_buffers_cleared());
        if (exit == 3) {
            /* A late snapshot of the same expired session cannot revive it. */
            s_state.local_editor.seconds_left = 300;
            render(); clear_intents(); editor_cell(0); editor_cell(0);
            tap(396, 448);
            assert(s_private_count == 0 && ui_private_buffers_cleared());
            screenshot("native-password-editor-expired");
        }
        finish_editor();
    }
    open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
    editor_type_text(secret);
    clear_intents(); tap_control("Close");
    assert(action_count(APP_CLOSE) == 1 && s_private_count == 0);
    assert(ui_private_buffers_cleared()); /* Clear on accepted close, before acknowledgement. */
    assert_secret_not_public(secret);
    render(); assert(ui_private_buffers_cleared());
    finish_editor();

    open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
    editor_type_text(secret);
    clear_intents(); tap_control("Delete");
    assert(!ui_private_password_contains(secret));
    assert(ui_private_password_tail_cleared());
    editor_type('@');
    assert(ui_private_password_contains(secret));
    clear_intents(); pointer(396, 448, true);
    app_intent_t held = *last_action(APP_TOUCH_BEGIN);
    s_state.local_editor.submit_available = false; ++s_state.ui_revision;
    render(); pointer(396, 448, false); assert_terminal(&held);
    assert(s_private_count == 0 && ui_private_password_contains(secret));
    s_state.screen_off = true; ++s_state.ui_revision; render();
    clear_intents(); pointer(396, 448, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    s_state.screen_off = false; ++s_state.local_editor.session; ++s_state.ui_revision;
    render(); pointer(396, 448, false); assert_terminal(&begin);
    assert(s_private_count == 0 && ui_private_buffers_cleared());
    finish_editor();
    s_state = original; render(); clear_intents();
    puts("PASS: actual private draft/tail wiping across held Join, close/navigation, lock, sleep, expiry, session/purpose replacement and whole-gesture wake");
}

static void bluetooth_name_editor_checks(void)
{
    app_view_t original = s_state;
    ui_fixture_native_networks(&s_state);
    s_state.overlay = APP_OVERLAY_SETTINGS; s_state.settings_page = APP_SETTINGS_BLUETOOTH;
    ++s_state.ui_revision; render(); clear_intents(); tap_control("Device name");
    assert(action_count(APP_BLUETOOTH_NAME_EDIT) == 1 && s_state.overlay == APP_OVERLAY_SETTINGS);
    s_state.bluetooth_name.available = false; render(); clear_intents(); tap_control("Device name");
    assert(action_count(APP_BLUETOOTH_NAME_EDIT) == 0);
    s_state.bluetooth_name.available = true;
    s_state.overlay = APP_OVERLAY_CONFIRM; s_state.confirmation = APP_CONFIRM_PAIR_OPEN;
    screenshot("native-bluetooth-effective-pairing-name");
    assert(has_visible_label(lv_screen_active(), "Effective name"));
    assert(!has_visible_label(lv_screen_active(), "Configured name"));
    open_editor(APP_LOCAL_EDIT_BLUETOOTH_NAME, "Cafe\xC3\xA9");
    screenshot("native-bluetooth-name-utf8-prefill");
    clear_intents(); tap_control("Delete"); assert_local_events_only();
    clear_intents(); tap_control("Apply"); assert_local_events_only();
    assert(s_private_count == 1 && s_private_request.purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME);
    assert(s_private_request.value.bluetooth.length == 4);
    assert(strcmp(s_private_request.value.bluetooth.name, "Cafe") == 0);
    assert(ui_private_buffers_cleared()); finish_editor();
    open_editor(APP_LOCAL_EDIT_BLUETOOTH_NAME, "");
    assert(lv_obj_has_state(control("Apply"), LV_STATE_DISABLED));
    editor_type_text(" Device");
    assert(lv_obj_has_state(control("Apply"), LV_STATE_DISABLED));
    finish_editor();
    open_editor(APP_LOCAL_EDIT_BLUETOOTH_NAME, "Device ");
    assert(lv_obj_has_state(control("Apply"), LV_STATE_DISABLED));
    clear_intents(); tap_control("Delete");
    assert(!lv_obj_has_state(control("Apply"), LV_STATE_DISABLED));
    finish_editor();
    open_editor(APP_LOCAL_EDIT_BLUETOOTH_NAME, NULL);
    editor_type_text("ABCDEFGHIJKLMNOPQRS"); /* 19th byte must not replace the 18th. */
    screenshot("native-bluetooth-name-full");
    clear_intents(); tap_control("Apply");
    assert(s_private_count == 1 && s_private_request.value.bluetooth.length == 18);
    assert(strcmp(s_private_request.value.bluetooth.name, "ABCDEFGHIJKLMNOPQR") == 0);
    finish_editor();
    s_state = original; render(); clear_intents();
    puts("PASS: local Bluetooth name editing preserves UTF-8 identity/deletion, enforces byte/space limits and submits through its separate purpose");
}

static void common_keys_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_KEYBOARD;
    s_state.connected = s_state.ready = s_state.bluetooth_available = true;
    s_state.transport_switch_pending = s_state.transport_error = s_state.release_pending = false;
    s_state.locked = s_state.screen_off = s_state.sending = false;
    s_state.input_transport = APP_INPUT_BLUETOOTH;
    s_state.host_os = APP_HOST_CTRL;
    strcpy(s_state.preview, "Unsent draft");
    const char *entry_names[] = {"Motion", "Touch", "Spoken", "Hotkeys"};
    const char *entry_scenes[] = {"keys-entry-motion", "keys-entry-touch", "keys-entry-spoken", "keys-entry-shortcuts"};
    for (unsigned submode = 0; submode < 4; ++submode) {
        s_state.overlay = APP_OVERLAY_NONE;
        s_state.keyboard_submode = (app_keyboard_submode_t)submode;
        ++s_state.ui_revision; screenshot(entry_scenes[submode]);
        assert_rect(control("Keys"), 224, 8, 48, 48);
        assert_rect(control(entry_names[submode]), 140, 8, 76, 48);
        const char *header_names[] = {entry_names[submode], "Keys", "Menu"};
        lv_area_t first_caption = {0};
        for (unsigned i = 0; i < 3; ++i) {
            lv_obj_t *caption = lv_obj_get_child(control(header_names[i]), 0);
            lv_area_t area;
            lv_obj_get_coords(caption, &area);
            if (i == 0) first_caption = area;
            else assert(area.y1 == first_caption.y1 && area.y2 == first_caption.y2);
            assert(lv_obj_get_style_text_font(caption, 0) == &lv_font_montserrat_16);
            lv_point_t size;
            lv_text_get_size(&size, lv_label_get_text(caption), lv_obj_get_style_text_font(caption, 0),
                lv_obj_get_style_text_letter_space(caption, 0), lv_obj_get_style_text_line_space(caption, 0),
                LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            assert(size.x <= lv_obj_get_content_width(caption));
            if (i == 0) printf("PASS: header %s uses Montserrat 16, text width %d within %d pixels and shared caption baseline\n",
                entry_names[submode], (int)size.x, (int)lv_obj_get_content_width(caption));
        }
        clear_intents(); tap_control("Keys");
        assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_COMMON_KEYS);
        assert(action_count(APP_KEY) == 0 && action_count(APP_SHORTCUT) == 0 && action_count(APP_SEND) == 0);
        assert(s_state.overlay == APP_OVERLAY_NONE && !strcmp(s_state.preview, "Unsent draft"));
    }
    s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    s_state.listening = true;
    screenshot("keys-entry-capture");
    assert_rect(control("Keys"), 224, 8, 48, 48);
    s_state.listening = false;
    s_state.connected = s_state.ready = false;
    screenshot("keys-entry-disconnected");
    clear_intents(); tap_control("Keys");
    assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_COMMON_KEYS);
    s_state.connected = s_state.ready = true;
    s_state.overlay = APP_OVERLAY_KEYBOARD; ++s_state.ui_revision;
    render(); clear_intents(); tap_control("Common keys");
    assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_COMMON_KEYS);
    assert(action_count(APP_KEY) == 0 && s_state.overlay == APP_OVERLAY_KEYBOARD);
    s_state.overlay = APP_OVERLAY_COMMON_KEYS; ++s_state.ui_revision;
    screenshot("common-keys-ready");
    const char *rows[][3] = {{"Copy", "Paste", "Undo"}, {"Backspace", "Delete fwd", "Enter"},
        {"Esc", "Tab", "Select all"}, {"Home", "Up", "End"}, {"Left", "Down", "Right"}};
    for (unsigned row = 0; row < 5; ++row) row_geometry(rows[row], 3);
    assert_rect(control("Back to keyboard"), 12, 424, 456, 48);
    assert_rect(control("Close"), 404, 8, 64, 48);
    s_state.target_name = "Office workstation - shared desk";
    screenshot("common-keys-long-host");
    assert(has_visible_label(lv_screen_active(), "Bluetooth / Office workstation - shared desk"));
    s_state.target_name = original.target_name;
    render();
    const char *names[] = {"Backspace", "Delete fwd", "Enter", "Esc", "Tab", "Home", "Up", "End", "Left", "Down", "Right"};
    const unsigned usage[] = {0x2a, 0x4c, 0x28, 0x29, 0x2b, 0x4a, 0x52, 0x4d, 0x50, 0x51, 0x4f};
    for (unsigned i = 0; i < 11; ++i) {
        unsigned cell = i < 5 ? i + 3 : i + 4;
        assert_rect(control(names[i]), 12 + (int)(cell % 3) * 156,
                    112 + (int)(cell / 3) * 60, 144, 52);
        clear_intents(); tap_control(names[i]);
        assert(action_count(APP_KEY) == 1 && (unsigned)last_action(APP_KEY)->x == usage[i]);
        assert(last_action(APP_KEY)->y == 0 && last_action(APP_KEY)->ui_revision == s_state.ui_revision);
        assert(action_count(APP_DELETE) == 0 && action_count(APP_SEND) == 0 && action_count(APP_SHORTCUT) == 0);
        assert(strcmp(s_state.preview, "Unsent draft") == 0);
    }
    const char *shortcuts[] = {"Copy", "Paste", "Undo", "Select all"};
    const app_shortcut_t actions[] = {APP_SHORTCUT_COPY, APP_SHORTCUT_PASTE, APP_SHORTCUT_UNDO, APP_SHORTCUT_SELECT_ALL};
    for (unsigned host = 0; host < 2; ++host) {
        s_state.host_os = host ? APP_HOST_COMMAND : APP_HOST_CTRL;
        ++s_state.ui_revision; screenshot(host ? "common-keys-command" : "common-keys-control");
        const char *modifier_hint = host ? "Apple shortcuts / Cmd+C, Cmd+V, Cmd+Z, Cmd+A" :
            "Windows shortcuts / Ctrl+C, Ctrl+V, Ctrl+Z, Ctrl+A";
        lv_obj_t *hint = find_visible_label(lv_screen_active(), modifier_hint);
        assert(hint);
        lv_point_t hint_size;
        lv_text_get_size(&hint_size, modifier_hint, lv_obj_get_style_text_font(hint, 0),
            lv_obj_get_style_text_letter_space(hint, 0), lv_obj_get_style_text_line_space(hint, 0),
            LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        assert(hint_size.x <= lv_obj_get_content_width(hint));
        for (unsigned submode = 0; submode < 4; ++submode) {
            s_state.keyboard_submode = (app_keyboard_submode_t)submode;
            ++s_state.ui_revision; render();
            for (unsigned i = 0; i < 4; ++i) {
                unsigned cell = i < 3 ? i : 8;
                assert_rect(control(shortcuts[i]), 12 + (int)(cell % 3) * 156,
                            112 + (int)(cell / 3) * 60, 144, 52);
                clear_intents(); tap_control(shortcuts[i]);
                assert(action_count(APP_SHORTCUT) == 1 && last_action(APP_SHORTCUT)->x == (int)actions[i]);
                assert(last_action(APP_SHORTCUT)->ui_revision == s_state.ui_revision);
                assert(action_count(APP_KEY) == 0 && action_count(APP_DELETE) == 0 && action_count(APP_SEND) == 0);
                assert(strcmp(s_state.preview, "Unsent draft") == 0);
            }
        }
    }
    /* A held shortcut retains the old revision; it must never adopt a new host setting. */
    for (unsigned i = 0; i < 2; ++i) {
        int x = i ? 396 : 84, y = i ? 258 : 138;
        clear_intents(); pointer(x, y, true);
        app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
        uint64_t pressed_revision = s_state.ui_revision;
        s_state.host_os = s_state.host_os == APP_HOST_CTRL ? APP_HOST_COMMAND : APP_HOST_CTRL;
        ++s_state.ui_revision;
        render(); pointer(x, y, false); assert_terminal(&begin);
        assert(action_count(APP_SHORTCUT) == 1 && last_action(APP_SHORTCUT)->ui_revision == pressed_revision);
    }
    clear_intents(); pointer(396, 198, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    s_state.ready = false; ++s_state.ui_revision;
    render(); pointer(396, 198, false); assert_terminal(&begin);
    assert(action_count(APP_KEY) == 0);
    screenshot("common-keys-unready");
    for (unsigned i = 0; i < 11; ++i) assert(lv_obj_has_state(control(names[i]), LV_STATE_DISABLED));
    for (unsigned i = 0; i < 4; ++i) assert(lv_obj_has_state(control(shortcuts[i]), LV_STATE_DISABLED));
    s_state.ready = true;
    for (unsigned gate = 0; gate < 3; ++gate) {
        s_state.sending = gate == 0;
        s_state.connected = gate != 1;
        s_state.release_pending = gate == 2;
        ++s_state.ui_revision; render();
        for (unsigned i = 0; i < 4; ++i) {
            clear_intents(); tap_control(shortcuts[i]);
            assert(action_count(APP_SHORTCUT) == 0 && action_count(APP_SEND) == 0);
        }
        clear_intents(); tap_control("Enter"); assert(action_count(APP_KEY) == 0);
        if (gate == 1) screenshot("common-keys-disconnected");
    }
    s_state.connected = true; s_state.release_pending = false;
    for (unsigned gate = 0; gate < 2; ++gate) {
        for (unsigned i = 0; i < 2; ++i) {
            int x = i ? 396 : 84, y = i ? 258 : 138;
            s_state.locked = gate == 0; s_state.screen_off = gate == 1;
            ++s_state.ui_revision; render(); clear_intents();
            if (!gate && !i) screenshot("common-keys-locked");
            pointer(x, y, true);
            begin = *last_action(APP_TOUCH_BEGIN);
            s_state.locked = s_state.screen_off = false;
            ++s_state.ui_revision; render(); pointer(x, y, false);
            assert_terminal(&begin);
            assert(action_count(APP_KEY) == 0 && action_count(APP_SHORTCUT) == 0);
        }
    }
    s_state.overlay = APP_OVERLAY_NONE; s_state.keyboard_submode = APP_KEYBOARD_TOUCH;
    grid_groups(); ++s_state.ui_revision; render();
    clear_intents(); tap_control("Delete draft");
    assert(action_count(APP_DELETE) == 1 && action_count(APP_KEY) == 0 && action_count(APP_SHORTCUT) == 0);
    clear_intents(); tap_control("Send");
    assert(action_count(APP_SEND) == 1 && action_count(APP_KEY) == 0 && action_count(APP_SHORTCUT) == 0);
    s_state = original; render(); clear_intents();
    puts("PASS: one-tap Keys in every Keyboard submode; symmetric 5 x 3 panel with eleven host usages and four host-setting shortcuts preserves draft, revision and readiness/wake/lock gates");
}

static void motion_reason_and_tap_setting_checks(void)
{
    app_view_t original = s_state;
    const struct { app_motion_reason_t reason; const char *name; const char *label; } cases[] = {
        {APP_MOTION_UNKNOWN, "unknown", "Motion status unavailable"},
        {APP_MOTION_READY, "ready", "Touch pauses motion"},
        {APP_MOTION_SENSOR_UNAVAILABLE, "sensor-unavailable", "Motion sensor unavailable"},
        {APP_MOTION_CALIBRATION_REQUIRED, "calibration-required", "Calibrate motion"},
        {APP_MOTION_CALIBRATING, "calibrating", "Keep the device still"},
        {APP_MOTION_PROVIDER_UNAVAILABLE, "provider-unavailable", "Selected input provider unavailable"},
        {APP_MOTION_DISCONNECTED, "usb-disconnected", "Connect a USB data host"},
        {APP_MOTION_INPUT_NOT_READY, "input-unready", "Host connected; input is not ready"},
        {APP_MOTION_RELEASING, "releasing", "Waiting for held input to release"},
        {APP_MOTION_TRANSPORT_SWITCHING, "switching", "Switching input connection"},
        {APP_MOTION_TRANSPORT_ERROR, "error", "Input connection error"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        s_state = original;
        s_state.mode = INPUT_MOUSE; s_state.overlay = APP_OVERLAY_NONE;
        s_state.locked = s_state.screen_off = s_state.motion_enabled = false;
        s_state.input_transport = APP_INPUT_USB; s_state.usb_available = true;
        s_state.connected = s_state.ready = s_state.motion_available = s_state.calibration_available = true;
        s_state.transport_switch_pending = s_state.transport_error = s_state.release_pending = false;
        s_state.calibrating = false; s_state.motion_reason = cases[i].reason;
        s_state.tap_to_click_available = s_state.tap_to_click_enabled = true;
        switch (cases[i].reason) {
        case APP_MOTION_UNKNOWN: case APP_MOTION_SENSOR_UNAVAILABLE:
            s_state.calibration_available = false; s_state.motion_available = false; break;
        case APP_MOTION_CALIBRATION_REQUIRED: s_state.motion_available = false; break;
        case APP_MOTION_CALIBRATING: s_state.motion_available = false; s_state.calibrating = true; break;
        case APP_MOTION_PROVIDER_UNAVAILABLE: s_state.usb_available = false; break;
        case APP_MOTION_DISCONNECTED: s_state.connected = s_state.ready = false; break;
        case APP_MOTION_INPUT_NOT_READY: s_state.ready = false; break;
        case APP_MOTION_RELEASING: s_state.release_pending = true; break;
        case APP_MOTION_TRANSPORT_SWITCHING: s_state.transport_switch_pending = true; break;
        case APP_MOTION_TRANSPORT_ERROR: s_state.transport_error = true; break;
        default: break;
        }
        char name[80]; snprintf(name, sizeof(name), "motion-reason-%s", cases[i].name);
        screenshot(name);
        assert(has_visible_label(lv_screen_active(), cases[i].label));
        /* Calibration changes the caption; exercise the same contracted hit target. */
        clear_intents(); pointer(124, 448, true); pointer(124, 448, false);
        assert(action_count(APP_MOTION) == (cases[i].reason == APP_MOTION_READY ? 1U : 0U));
        assert(!s_state.motion_enabled);
        clear_intents(); tap_control("Motion help");
        assert(action_count(APP_MENU) == 1 && last_action(APP_MENU)->x == APP_OVERLAY_TARGET);
        assert(action_count(APP_TOUCH_MOVE) == 0 && action_count(APP_MOTION) == 0);
    }
    s_state = original; ui_fixture_native_networks(&s_state);
    s_state.overlay = APP_OVERLAY_SETTINGS; s_state.settings_page = APP_SETTINGS_INPUT;
    ++s_state.ui_revision;
    screenshot("tap-to-click-on");
    clear_intents(); tap_control("Tap to click / on");
    assert(action_count(APP_TAP_TO_CLICK) == 1 && last_action(APP_TAP_TO_CLICK)->x == 0);
    assert(s_state.tap_to_click_enabled);
    s_state.tap_to_click_enabled = false; s_state.settings_save_state = APP_SAVE_PENDING;
    screenshot("tap-to-click-off-saving");
    clear_intents(); tap_control("Tap to click / off");
    assert(last_action(APP_TAP_TO_CLICK)->x == 1 && !s_state.tap_to_click_enabled);
    s_state.settings_save_state = APP_SAVE_ERROR;
    screenshot("tap-to-click-save-error");
    assert(has_visible_label(lv_screen_active(), "Could not save"));
    s_state.tap_to_click_available = false;
    screenshot("tap-to-click-unavailable");
    clear_intents(); tap_control("Tap to click / unavailable");
    assert(action_count(APP_TAP_TO_CLICK) == 0);
    s_state.overlay = APP_OVERLAY_NONE; s_state.mode = INPUT_MOUSE;
    s_state.tap_to_click_available = true; s_state.motion_reason = APP_MOTION_READY;
    ++s_state.ui_revision;
    screenshot("mouse-tap-off-help");
    assert(has_visible_label(lv_screen_active(), "Tap to click off"));
    clear_intents(); pointer(80, 200, true); pointer(85, 204, true); pointer(85, 204, false);
    assert(action_count(APP_TOUCH_MOVE) == 1 && action_count(APP_CLICK) == 0);
    clear_intents(); pointer(80, 200, true);
    app_intent_t begin = *last_action(APP_TOUCH_BEGIN);
    pointer(80, 200, true); pointer(80, 200, true);
    assert(action_count(APP_TOUCH_MOVE) == 0); /* 40 ms: no early heartbeat. */
    pointer(80, 200, true); /* 60 ms of genuine stationary contact. */
    assert(action_count(APP_TOUCH_MOVE) == 1);
    const app_intent_t *heartbeat = last_action(APP_TOUCH_MOVE);
    assert(heartbeat->x == 0 && heartbeat->y == 0 && heartbeat->gesture == begin.gesture);
    assert(heartbeat->surface == APP_SURFACE_TOUCHPAD && heartbeat->event_ms == 0);
    s_state.overlay = APP_OVERLAY_MODES; ++s_state.ui_revision; render();
    for (unsigned i = 0; i < 4; ++i) pointer(80, 200, true);
    pointer(80, 200, false); assert_terminal(&begin);
    lv_tick_inc(100); lv_timer_handler();
    assert(action_count(APP_TOUCH_MOVE) == 1 && action_count(APP_CLICK) == 0);
    s_state = original; render(); clear_intents();
    puts("PASS: coordinator-derived Motion reasons and help routing, truthful tap preference/save state and no duplicate touchpad recognition");
}

static void extension_checks(void)
{
    native_wifi_scan_checks();
    native_wifi_detail_and_status_checks();
    private_editor_submission_checks();
    private_editor_cleanup_checks();
    bluetooth_name_editor_checks();
    common_keys_checks();
    motion_reason_and_tap_setting_checks();
    app_ui_set_local_edit_submit(NULL);
}
