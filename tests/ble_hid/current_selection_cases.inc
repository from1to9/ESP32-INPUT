// SPDX-License-Identifier: Apache-2.0
static ble_hid_target_t current_selection_setup(bool selected, bool ready)
{
    native_setup(true);
    if (!selected) {
        assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK);
        worker_cycle();
    }
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.ready);
    if (!ready) write_cccd(ATTR_MOUSE_CCCD, false);
    assert(state.public.ready == ready && state.public.connection_admitted);
    return native_target(active_peer);
}
static void current_selection_replace_bond(void)
{
    save_sdk_bond(1, active_peer, 0x66);
    authentication_event(true, true);
}
static void current_selection_disable_mouse(void) { write_cccd(ATTR_MOUSE_CCCD, false); }
static void current_selection_review_cases(void)
{
    for (unsigned selecting = 0; selecting < 2; ++selecting)
    for (unsigned operation = 0; operation < 3; ++operation) {
        ble_hid_target_t target = current_selection_setup(operation == 1, true);
        const ble_hid_bond_list_t before = ble_hid_bond_list();
        const unsigned commits = target_commit_calls;
        native_begin(selecting ? APP_BT_MANAGE_SELECT : APP_BT_MANAGE_ADD, selecting ? &target : NULL);
        native_context.release_pending = true;
        app_bluetooth_management_service(&native_context, ++native_now);
        ble_hid_target_t order[2] = {public_target(&before.bonds[1]), public_target(&before.bonds[0])};
        ble_hid_target_preferences_request_t request = {.expected_bond_revision = before.revision,
            .expected_revision = before.preferences_revision, .count = 2, .selected = true, .target = target};
        memcpy(request.order, order, sizeof(order));
        esp_err_t error = operation == 0 ? ble_hid_target_select(before.revision, &target) :
            operation == 1 ? ble_hid_target_order(before.revision, order, 2) :
            ble_hid_target_preferences_request(&request, NULL);
        assert(error == ESP_ERR_INVALID_STATE);
        worker_cycle();
        assert(target_commit_calls == commits && native_receipt().active && !close_calls);
        assert(ble_hid_target_snapshot().selected == (operation == 1));
        app_bluetooth_management_cancel();
    }
    ble_hid_target_t target = current_selection_setup(false, true);
    const ble_hid_bond_list_t before = ble_hid_bond_list();
    ble_hid_target_preferences_request_t request = {.expected_bond_revision = before.revision,
        .expected_revision = before.preferences_revision, .count = 2, .selected = true, .target = target};
    for (unsigned i = 0; i < 2; ++i) request.order[i] = public_target(&before.bonds[i]);
    const unsigned commits = target_commit_calls;
    native_begin(APP_BT_MANAGE_ADD, NULL);
    const uint64_t old_token = state.management_reservation;
    assert(ble_hid_management_select_current(old_token, state.public.generation, &request, NULL) == ESP_ERR_INVALID_STATE);
    app_bluetooth_management_cancel();
    native_begin(APP_BT_MANAGE_SELECT, &target);
    const uint64_t token = state.management_reservation, request_id = state.target.request_id;
    const uint64_t rejected_tokens[] = {0, old_token, token + 1U};
    for (unsigned i = 0; i < 3; ++i)
        assert(ble_hid_management_select_current(rejected_tokens[i], state.public.generation, &request, NULL) == ESP_ERR_INVALID_STATE);
    assert(ble_hid_management_select_current(token, state.public.generation - 1U, &request, NULL) == ESP_ERR_INVALID_STATE);
    request.target.bond_id[0] ^= 1U;
    assert(ble_hid_management_select_current(token, state.public.generation, &request, NULL) == ESP_ERR_INVALID_STATE);
    request.target = native_target(0xc2);
    assert(ble_hid_management_select_current(token, state.public.generation, &request, NULL) == ESP_ERR_INVALID_STATE);
    worker_cycle();
    assert(state.target.request_id == request_id && target_commit_calls == commits && !close_calls);
    app_bluetooth_management_cancel();
    target = current_selection_setup(false, true);
    native_begin(APP_BT_MANAGE_SELECT, &target);
    const uint64_t once = state.management_reservation;
    native_step(false);
    const unsigned saved_commits = target_commit_calls;
    request.target = target; request.expected_revision = state.target.revision;
    request.expected_bond_revision = state.bond_list.revision;
    assert(ble_hid_management_select_current(once, state.public.generation, &request, NULL) == ESP_ERR_INVALID_STATE);
    native_finish();
    assert(target_commit_calls == saved_commits && state.public.ready && !close_calls);
    for (unsigned already = 0; already < 2; ++already) {
        target = current_selection_setup(already != 0, true);
        native_begin(APP_BT_MANAGE_SELECT, &target);
        if (!already) native_step(false); /* Commit before racing the final receipt. */
        model_before_state_lock_count = 3; model_before_state_lock = current_selection_disable_mouse;
        app_bluetooth_management_service(&native_context, ++native_now);
        assert(!model_before_state_lock_count && !state.public.ready && !close_calls);
        assert(native_receipt().phase == APP_BT_MANAGE_COMPLETE);
        assert(!strstr(native_receipt().message, " ready") && !strstr(native_receipt().message, "connected"));
        assert(ble_hid_target_snapshot().selected && ble_hid_target_snapshot().durable);
    }
    puts("Current selection review: other operations cannot borrow admission; completion reports only the saved preference passed");
}
static void current_selection_cases(void)
{
    for (unsigned ready = 0; ready < 2; ++ready)
    for (unsigned selected = 0; selected < 2; ++selected) {
        ble_hid_target_t target = current_selection_setup(selected != 0, ready != 0);
        const uint64_t generation = state.public.generation;
        const unsigned commits = target_commit_calls;
        native_begin(APP_BT_MANAGE_SELECT, &target);
        native_context.release_pending = true; native_step(false);
        assert(!close_calls && state.public.ready == (ready != 0));
        native_context.release_pending = false;
        for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(false);
        assert(!close_calls && state.public.connected && state.public.ready == (ready != 0));
        assert(state.public.generation == generation && !state.management_reservation);
        assert(native_receipt().phase == APP_BT_MANAGE_COMPLETE && native_receipt().error == ESP_OK);
        const ble_hid_target_snapshot_t policy = ble_hid_target_snapshot();
        assert(policy.durable && policy.selected && !memcmp(policy.target.bond_id, target.bond_id, BLE_HID_BOND_ID_BYTES));
        assert(target_commit_calls == commits + (selected ? 0U : 1U));
        assert(!state.public.pairing_open && fake_bond_count == 2 && !remove_calls);
    }
    /* Unreserved direct edits remain unable to change an active target. */
    ble_hid_target_t target = current_selection_setup(false, true);
    assert(ble_hid_target_select(ble_hid_bond_list().revision, &target) == ESP_ERR_INVALID_STATE);
    target.bond_id[0] ^= 1U;
    assert(app_bluetooth_management_begin(APP_BT_MANAGE_SELECT, native_host_generation(),
        ble_hid_bond_list().revision, ble_hid_target_snapshot().revision, &target, native_now) == ESP_ERR_NOT_FOUND);
    assert(!close_calls && !native_receipt().active && state.public.ready);
    /* An address-matched provisional link has no verified current authority. */
    native_setup(true); active_peer = 0xc3; connect_peer(); drain(); target = native_target(active_peer);
    assert(!state.public.connection_admitted);
    native_begin(APP_BT_MANAGE_SELECT, &target); native_step(false);
    assert(close_calls == 1 && !state.public.ready);
    disconnect_peer(); native_finish();
    assert(ble_hid_target_snapshot().selected && !state.public.connected && !state.public.pairing_open);
    /* A changed catalogue/authentication before submission does not inherit
     * the current-row claim from the reviewed snapshot. */
    target = current_selection_setup(false, true);
    native_begin(APP_BT_MANAGE_SELECT, &target);
    authentication_event(true, true);
    native_step(false);
    assert(native_receipt().phase == APP_BT_MANAGE_FAILED && !ble_hid_target_snapshot().selected && !close_calls);
    /* Cancellation before persistence retains both connection and preference. */
    target = current_selection_setup(false, true);
    const unsigned commits = target_commit_calls;
    native_begin(APP_BT_MANAGE_SELECT, &target); app_bluetooth_management_cancel(); worker_cycle();
    assert(state.public.ready && !ble_hid_target_snapshot().selected && target_commit_calls == commits && !close_calls);
    /* Failed commits remain failures; a possibly persisted result is explicit. */
    for (unsigned persists = 0; persists < 2; ++persists) {
        target = current_selection_setup(false, true);
        target_commit_error = ESP_FAIL; target_commit_persists_on_error = persists != 0;
        native_begin(APP_BT_MANAGE_SELECT, &target);
        for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(false);
        assert(native_receipt().phase == APP_BT_MANAGE_FAILED && native_receipt().error != ESP_OK);
        assert(state.public.connected && !close_calls && !remove_calls && !state.public.pairing_open);
        assert(!ble_hid_target_snapshot().selected);
        assert(native_receipt().uncertain);
    }
    /* A real link-down during the save may finish the explicit policy change,
     * but only actual advertising is reported; no connection is invented. */
    target = current_selection_setup(false, true); target_commit_hook = disconnect_peer;
    native_begin(APP_BT_MANAGE_SELECT, &target); native_finish();
    assert(!state.public.connected && !state.public.ready && ble_hid_target_snapshot().selected && !close_calls);
    assert(native_receipt().phase == APP_BT_MANAGE_COMPLETE && strstr(native_receipt().message, "not ready"));
    target = current_selection_setup(false, true); target_commit_hook = current_selection_replace_bond;
    native_begin(APP_BT_MANAGE_SELECT, &target);
    for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(false);
    assert(native_receipt().phase == APP_BT_MANAGE_FAILED && native_receipt().uncertain);
    assert(!state.public.ready && !remove_calls && fake_bond_count == 2 && !state.public.pairing_open);
    puts("Current saved-host selection: persist new policy or reuse selected policy without dropping the verified connection passed");
}
