// SPDX-License-Identifier: Apache-2.0
static void controller_permission_checks(void)
{
    app_view_t original = s_state;
    s_state.overlay = APP_OVERLAY_API_CONNECTION;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.local_access = APP_LOCAL_NORMAL;
    ++s_state.ui_revision;
    render(); clear_intents(); tap_control("Wi-Fi controllers");
    assert(last_action(APP_MENU)->x == APP_OVERLAY_CONTROLLERS);
    s_state.overlay = APP_OVERLAY_CONTROLLERS;
    ++s_state.ui_revision;
    s_state.controllers = (app_controllers_view_t){.known = true, .count = 4, .clock_known = true};
    app_controller_row_t *rows = s_state.controllers.rows;
    const char *digest = "0123456789abcdef0123456789abcdefabcdef0123456789abcdef0123456789";
    for (unsigned i = 0; i < 4; ++i) {
        rows[i].key.revision = 101;
        rows[i].key.id[0] = (uint8_t)(i + 1);
        strcpy(rows[i].label, i < 2 ? "Office laptop" : i == 2 ? "Desk browser" : "Tablet");
        strcpy(rows[i].fingerprint, digest);
        rows[i].permissions = APP_UI_GRANT_INPUT;
    }
    rows[0].key.pairing_request = 71;
    rows[0].state = APP_CONTROLLER_PAIRING;
    rows[1].state = APP_CONTROLLER_AUTHORIZED; rows[1].controlling = true;
    rows[2].state = APP_CONTROLLER_EXPIRED;
    rows[3].state = APP_CONTROLLER_REVOKED;
    screenshot("controller-list");
    const char *proposal = "Office laptop\nPairing request  /  ID 01000000";
    clear_intents(); tap_control(proposal);
    assert(last_action(APP_CONTROLLER_SELECT)->controller.key.id[0] == 1);
    lv_area_t area;
    lv_obj_get_coords(control(proposal), &area);
    clear_intents(); pointer(area.x1 + 20, area.y1 + 12, true);
    rows[0].key.id[0] = 7; ++rows[0].key.revision; render();
    pointer(area.x1 + 20, area.y1 + 12, false);
    assert(action_count(APP_CONTROLLER_SELECT) == 0);

    s_state.controllers.selection = rows[0].key;
    s_state.overlay = APP_OVERLAY_CONTROLLER_ACCESS; ++s_state.ui_revision;
    screenshot("controller-request");
    s_state.controllers.clock_known = false; render();
    assert(lv_obj_has_state(control("Review permissions"), LV_STATE_DISABLED));
    s_state.controllers.clock_known = true; render();
    tap_control("Keyboard\n& mouse");
    assert(lv_obj_has_state(control("Review permissions"), LV_STATE_DISABLED));
    tap_control("Keyboard\n& mouse");
    clear_intents(); tap_control("Review permissions");
    screenshot("controller-default-review");
    assert(has_visible_label(lv_screen_active(), "Duration: 6 hours"));
    assert(!action_count(APP_CONTROLLER_EDIT));
    tap_control("Approve controller");
    const app_controller_request_t *request = &last_action(APP_CONTROLLER_EDIT)->controller;
    assert(request->operation == APP_UI_GRANT_APPROVE && request->key.pairing_request == 71 &&
        request->permissions == APP_UI_GRANT_INPUT && request->duration == APP_UI_GRANT_6H);

    tap_control("Management"); tap_control("Updates"); tap_control("24 hours"); tap_control("Review permissions");
    tap_control("Approve controller");
    request = &last_action(APP_CONTROLLER_EDIT)->controller;
    assert(request->permissions == 7 && request->duration == APP_UI_GRANT_24H);
    tap_control("Review permissions");
    clear_intents(); lv_obj_get_coords(control("Approve controller"), &area);
    pointer(area.x1 + 50, area.y1 + 20, true); ++rows[0].key.revision; render();
    pointer(area.x1 + 50, area.y1 + 20, false);
    assert(action_count(APP_CONTROLLER_EDIT) == 0);

    s_state.overlay = APP_OVERLAY_CONTROLLERS; ++s_state.ui_revision; render();
    s_state.overlay = APP_OVERLAY_CONTROLLER_ACCESS;
    rows[1].legacy = true; s_state.controllers.selection = rows[1].key;
    ++s_state.ui_revision; render();
    tap_control("Management"); tap_control("Updates"); tap_control("Until revoked"); tap_control("Review permissions");
    screenshot("controller-permanent-review");
    assert(has_visible_label(lv_screen_active(), "Saving does not resume control or jobs."));
    assert(has_visible_label(lv_screen_active(), "Replaces legacy scopes"));
    assert(!lv_obj_has_state(control("Save permissions"), LV_STATE_DISABLED));
    clear_intents(); tap_control("Save permissions");
    request = &last_action(APP_CONTROLLER_EDIT)->controller;
    assert(request->operation == APP_UI_GRANT_EDIT && request->key.id[0] == 2 &&
        request->permissions == 7 && request->duration == APP_UI_GRANT_PERMANENT);

    clear_intents(); tap_control("Revoke..."); assert(!action_count(APP_CONTROLLER_EDIT));
    screenshot("controller-revoke"); tap_control("Revoke access");
    request = &last_action(APP_CONTROLLER_EDIT)->controller;
    assert(request->operation == APP_UI_GRANT_REVOKE && request->key.id[0] == 2 && !request->permissions);
    s_state.controllers.busy = true; strcpy(s_state.controllers.feedback, "Saving. Waiting for the result.");
    render(); clear_intents(); tap_control("Review permissions"); assert(!action_count(APP_CONTROLLER_EDIT));
    assert(lv_obj_has_state(control("Review permissions"), LV_STATE_DISABLED));
    s_state.controllers.busy = false; ++rows[1].key.revision;
    strcpy(s_state.controllers.feedback, "Save outcome uncertain. Recheck the device list.");
    screenshot("controller-save-uncertain");
    s_state.controllers.applied = true;
    strcpy(s_state.controllers.feedback, "Access revoked.");
    screenshot("controller-revoked");
    assert(has_visible_label(lv_screen_active(), "Request complete"));
    s_state = original; render(); clear_intents();
}
