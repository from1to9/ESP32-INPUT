// SPDX-License-Identifier: Apache-2.0
static void controller_control_tap(uint32_t gesture, app_action_t action_kind, app_controller_request_t request)
{
    app_intent_t intent = make_intent(action_kind, 0, 0, NULL);
    intent.gesture = gesture; intent.surface = APP_SURFACE_CONTROL; intent.controller = request;
    local_control_contact(gesture);
    deliver(intent);
}

static void controller_approval_is_physical_scoped_and_observed(void)
{
    setup(INPUT_MOUSE);
    test_controller_catalog = (app_control_access_snapshot_t){.known = true, .pending = true, .revision = 10,
        .pairing_request = 17, .pairing_remaining_ms = 10000, .count = 1};
    test_controller_catalog.proposal.id[0] = 1;
    strcpy(test_controller_catalog.proposal.label, "Shared name");
    test_controller_catalog.clients[0].id[0] = 2;
    strcpy(test_controller_catalog.clients[0].label, "Shared name");
    test_controller_calls = 0;
    test_controller_ingress = ESP_OK;
    action(APP_MENU, APP_OVERLAY_SETTINGS); action(APP_SETTINGS_PAGE, APP_SETTINGS_WIFI);
    action(APP_MENU, APP_OVERLAY_API_CONNECTION); action(APP_MENU, APP_OVERLAY_CONTROLLERS);
    assert(s_overlay == APP_OVERLAY_CONTROLLERS);
    render_view(&test_link, ESP_OK);
    assert(test_view.controllers.count == 2 && !strcmp(test_view.controllers.rows[1].label, "Shared name"));
    app_controller_request_t request = {.key = test_view.controllers.rows[0].key,
        .operation = APP_UI_GRANT_APPROVE, .permissions = APP_UI_GRANT_INPUT, .duration = APP_UI_GRANT_6H};
    controller_control_tap(601, APP_CONTROLLER_SELECT, request);
    assert(s_overlay == APP_OVERLAY_CONTROLLER_ACCESS && !test_controller_calls);
    app_controller_request_t different = request;
    different.key.id[0] = 2;
    controller_control_tap(602, APP_CONTROLLER_EDIT, different);
    app_intent_t synthetic = make_intent(APP_CONTROLLER_EDIT, 0, 0, NULL);
    synthetic.controller = request; deliver(synthetic);
    assert(!test_controller_calls);
    ++test_controller_catalog.revision;
    controller_control_tap(603, APP_CONTROLLER_EDIT, request);
    assert(!test_controller_calls);
    action(APP_MENU, APP_OVERLAY_CONTROLLERS);
    request.key.revision = test_controller_catalog.revision;
    controller_control_tap(604, APP_CONTROLLER_SELECT, request);
    controller_control_tap(605, APP_CONTROLLER_EDIT, request);
    assert(test_controller_calls == 1 && test_controller_mutation.action == APP_CONTROL_ACCESS_APPROVE &&
        test_controller_mutation.permissions == APP_CONTROL_ACCESS_INPUT && test_controller_mutation.duration == APP_CONTROL_ACCESS_SIX_HOURS &&
        test_controller_mutation.client_id[0] == 1 && test_controller_mutation.expected_revision == request.key.revision &&
        test_controller_mutation.pairing_request == 17);
    test_controller_catalog.receipt_request = 999; test_controller_catalog.receipt_state = CP_RESULT_APPLIED;
    render_view(&test_link, ESP_OK);
    assert(test_view.controllers.busy && !strstr(test_view.controllers.feedback, "saved"));
    test_controller_catalog.receipt_request = 81; test_controller_catalog.receipt_state = CP_RESULT_ACCEPTED;
    render_view(&test_link, ESP_OK); assert(test_view.controllers.busy);
    test_controller_catalog.receipt_state = CP_RESULT_FAILED;
    test_controller_catalog.receipt_error = CP_STORAGE_UNCERTAIN;
    render_view(&test_link, ESP_OK);
    assert(strstr(test_view.controllers.feedback, "uncertain"));
    test_controller_catalog.receipt_state = CP_RESULT_APPLIED; test_controller_catalog.receipt_error = CP_OK;
    render_view(&test_link, ESP_OK);
    assert(!test_view.controllers.busy && strstr(test_view.controllers.feedback, "saved and verified"));
    test_controller_ingress = ESP_ERR_INVALID_STATE;
    controller_control_tap(606, APP_CONTROLLER_EDIT, request);
    render_view(&test_link, ESP_OK); assert(strstr(test_view.controllers.feedback, "not accepted"));
    test_update_busy = true; remote_update_service();
    controller_control_tap(607, APP_CONTROLLER_EDIT, request);
    assert(test_controller_calls == 2);
}

static void controller_status_uses_the_active_lease(void)
{
    setup(INPUT_MOUSE);
    remote_acquire_current_host();
    test_controller_catalog = (app_control_access_snapshot_t){.known = true, .revision = 6, .count = 2};
    for (unsigned i = 0; i < 2; ++i) {
        test_controller_catalog.clients[i].id[0] = (uint8_t)(10 + i);
        test_controller_catalog.clients[i].enabled = true;
        strcpy(test_controller_catalog.clients[i].label, "Same name");
    }
    /* Inspect the view before a physical contact takes local control. */
    s_overlay = APP_OVERLAY_CONTROLLERS;
    render_view(&test_link, ESP_OK);
    assert(!test_view.controllers.rows[0].controlling && test_view.controllers.rows[1].controlling);
    local_control_contact(701);
    render_view(&test_link, ESP_OK);
    assert(!test_view.controllers.rows[0].controlling && !test_view.controllers.rows[1].controlling);
    assert(test_view.controllers.rows[1].state == APP_CONTROLLER_AUTHORIZED);
}
