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
