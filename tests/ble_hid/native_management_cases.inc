// SPDX-License-Identifier: Apache-2.0
/* Production native workflow + production BLE worker. Only the existing
 * SDK/NVS/radio callback boundary is substituted by this suite. */
static uint64_t native_now;
static app_bluetooth_management_context_t native_context;
static app_bluetooth_management_snapshot_t native_receipt(void)
{
    app_bluetooth_management_snapshot_t receipt; app_bluetooth_management_snapshot(&receipt); return receipt;
}
static ble_hid_target_t native_target(uint8_t address)
{
    ble_hid_bond_list_t list = ble_hid_bond_list();
    for (unsigned i = 0; i < list.count; ++i) if (list.bonds[i].address[0] == address) return public_target(&list.bonds[i]);
    assert(false); return (ble_hid_target_t){0};
}
static void native_setup(bool subscriptions_required)
{
    prepare_durable_targets();
    if (subscriptions_required) {
        for (unsigned i = 0; i < 2; ++i) {
            active_peer = (uint8_t)(0xc2U + i); connect_peer(); drain(); authorize_peer(true); subscribe(); disconnect_peer();
        }
        assert(subscriptions.count == 2);
    }
    ble_hid_target_t selected = native_target(0xc3);
    assert(ble_hid_target_select(ble_hid_bond_list().revision, &selected) == ESP_OK); worker_cycle();
    assert(ble_hid_target_snapshot().selected && ble_hid_target_snapshot().durable);
    if (state.adv_pending) {
        esp_ble_gap_cb_param_t started = {.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS};
        gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &started);
    }
    app_bluetooth_management_init(); native_now = 1000; native_context = (app_bluetooth_management_context_t){0};
}
static void native_begin(app_bluetooth_management_operation_t operation, const ble_hid_target_t *target)
{
    assert(app_bluetooth_management_begin(operation, ble_hid_snapshot().generation, ble_hid_bond_list().revision,
        ble_hid_target_snapshot().revision, target, native_now) == ESP_OK);
}
static void native_step(bool complete_removal)
{
    native_now += 20;
    app_bluetooth_management_service(&native_context, native_now);
    worker_cycle();
    if (state.adv_stop_pending) advertising_stopped(ESP_BT_STATUS_SUCCESS);
    if (state.adv_pending) {
        esp_ble_gap_cb_param_t started = {.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS};
        gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &started);
    }
    if (complete_removal && state.remove_inflight && !state.remove_completed) {
        esp_bd_addr_t address; memcpy(address, last_remove_address, sizeof(address));
        remove_complete(address, ESP_BT_STATUS_SUCCESS, true);
    }
    if (complete_removal && state.single_forget.sdk_inflight && !state.single_completed) {
        esp_bd_addr_t address; memcpy(address, last_remove_address, sizeof(address));
        remove_complete(address, ESP_BT_STATUS_SUCCESS, true);
    }
}
static void native_finish(void)
{
    for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(true);
    app_bluetooth_management_snapshot_t receipt = native_receipt();
    if (receipt.active || receipt.error != ESP_OK || receipt.phase != APP_BT_MANAGE_COMPLETE)
        fprintf(stderr, "Native workflow phase=%u active=%u error=%d message=%s\n",
            (unsigned)receipt.phase, receipt.active, receipt.error, receipt.message);
    assert(!receipt.active && receipt.phase == APP_BT_MANAGE_COMPLETE && receipt.error == ESP_OK);
}
static void native_management_selected_add_and_reconnect(void)
{
    native_setup(false);
    uint64_t previous = ble_hid_target_snapshot().revision;
    assert(!ble_hid_pairing_open(1000)); /* The actual selected-policy gate, before the native repair. */
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
    assert(state.public.connected && state.public.ready);
    native_begin(APP_BT_MANAGE_ADD, NULL); native_context.release_pending = true;
    native_step(false); assert(ble_hid_target_snapshot().revision == previous && !state.public.pairing_open && !close_calls);
    native_context.release_pending = false; native_step(false);
    assert(state.public.connected && close_calls && native_receipt().phase == APP_BT_MANAGE_DISCONNECT);
    disconnect_peer(); native_finish();
    assert(!ble_hid_target_snapshot().selected && ble_hid_target_snapshot().durable &&
        ble_hid_target_snapshot().revision == previous + 1U && state.public.pairing_open && state.public.advertising);
    assert(fake_bond_count == 2 && remove_calls == 0);
    active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true); subscribe();
    assert(ble_hid_snapshot().ready); uint64_t generation = ble_hid_snapshot().generation;
    assert(ble_hid_disconnect_generation(generation)); worker_cycle(); assert(close_calls);
    disconnect_peer(); worker_cycle();
    if (state.adv_pending) {
        esp_ble_gap_cb_param_t started = {.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS};
        gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &started);
    }
    assert(state.public.advertising && !state.public.failed);
    connect_peer(); drain(); authorize_peer(true); worker_cycle();
    assert(ble_hid_snapshot().connected && ble_hid_snapshot().ready && fake_bond_count == 2);
}
static void native_management_all_and_single_removal(void)
{
    native_setup(true); native_begin(APP_BT_MANAGE_FORGET_ALL, NULL);
    native_finish();
    assert(!fake_bond_count && !subscriptions.count && !state.public.pairing_open &&
        !ble_hid_target_snapshot().selected && ble_hid_target_snapshot().durable);
    reboot_target_preferences(); assert(!ble_hid_target_snapshot().selected && !state.public.pairing_open);
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    for (unsigned selected = 0; selected < 2; ++selected) {
        native_setup(true);
        ble_hid_target_t target = native_target(selected ? 0xc3 : 0xc2);
        ble_hid_target_t retained = native_target(selected ? 0xc2 : 0xc3);
        native_begin(APP_BT_MANAGE_FORGET_ONE, &target); native_finish();
        ble_hid_bond_list_t list = ble_hid_bond_list();
        assert(list.count == 1 && list.bonds[0].address[0] == retained.address[0] &&
            !memcmp(list.bonds[0].bond_id, retained.bond_id, BLE_HID_BOND_ID_BYTES));
        assert(subscriptions.count == 1 && subscription_mask(retained.bond_id) == 3 && remove_calls == 1 && !state.public.pairing_open);
        assert(ble_hid_target_snapshot().selected == !selected && ble_hid_target_snapshot().durable);
        reboot_target_preferences(); assert(ble_hid_target_snapshot().selected == !selected);
    }
}
static void native_management_generation_and_replacement(void)
{
    const app_bluetooth_management_operation_t operations[] = {APP_BT_MANAGE_ADD, APP_BT_MANAGE_FORGET_ALL};
    for (unsigned op = 0; op < sizeof(operations) / sizeof(operations[0]); ++op) {
        /* A real disconnected/reconnected callback before acceptance cannot
         * authorize disconnecting even the same saved host's new link. */
        native_setup(false); active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
        native_begin(operations[op], NULL);
        disconnect_peer(); connect_peer(); drain(); authorize_peer(true); subscribe();
        const uint64_t unchanged_policy = ble_hid_target_snapshot().revision;
        native_step(false);
        assert(native_receipt().phase == APP_BT_MANAGE_FAILED && !close_calls && !remove_calls &&
            ble_hid_target_snapshot().revision == unchanged_policy && !state.public.pairing_open);

        for (unsigned replacement = 0; replacement < 3; ++replacement) {
            native_setup(false);
            /* Admit either saved identity so the replacement is a different
             * actual peer, rather than editing a cached generation in a test. */
            ble_hid_target_preferences_request_t policy = target_request();
            policy.selected = false; memset(&policy.target, 0, sizeof(policy.target));
            assert(ble_hid_target_preferences_request(&policy, NULL) == ESP_OK); worker_cycle();
            assert(!ble_hid_target_snapshot().selected && ble_hid_target_snapshot().durable);
            active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
            native_begin(operations[op], NULL);
            uint64_t generation = ble_hid_snapshot().generation;
            uint64_t policy_revision = ble_hid_target_snapshot().revision;
            native_step(false);
            assert(native_receipt().phase == APP_BT_MANAGE_DISCONNECT && close_calls == 1 &&
                state.public.connected && state.public.generation == generation + 1U && !state.public.ready);
            if (replacement) {
                disconnect_peer(); active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true); subscribe();
                assert(state.public.connected && state.public.ready && state.public.generation > generation + 1U);
            }
            for (unsigned poll = 0; poll < 50; ++poll) {
                native_step(false);
                assert(native_receipt().phase == APP_BT_MANAGE_DISCONNECT && close_calls == 1 && !remove_calls &&
                    ble_hid_target_snapshot().revision == policy_revision && !state.public.pairing_open);
            }
            if (replacement == 2) {
                app_bluetooth_management_service(&native_context, 31000);
                assert(native_receipt().phase == APP_BT_MANAGE_FAILED && native_receipt().error == ESP_ERR_TIMEOUT &&
                    close_calls == 1 && !remove_calls && !state.public.pairing_open);
                disconnect_peer(); native_step(true);
                assert(native_receipt().phase == APP_BT_MANAGE_FAILED && fake_bond_count == 2 &&
                    ble_hid_target_snapshot().revision == policy_revision && !state.public.pairing_open);
            } else {
                disconnect_peer(); native_finish();
                assert(close_calls == 1 && state.public.pairing_open == (operations[op] == APP_BT_MANAGE_ADD));
                assert(fake_bond_count == (operations[op] == APP_BT_MANAGE_ADD ? 2 : 0));
                native_step(true); assert(close_calls == 1);
            }
        }
    }
}
static void native_management_cancellation_failure_and_stale(void)
{
    native_setup(false); ble_hid_target_t target = native_target(0xc3);
    assert(app_bluetooth_management_begin(APP_BT_MANAGE_FORGET_ONE, state.public.generation,
        ble_hid_bond_list().revision + 1U, ble_hid_target_snapshot().revision, &target, native_now) == ESP_ERR_INVALID_STATE);
    target.bond_id[0] ^= 1U;
    assert(app_bluetooth_management_begin(APP_BT_MANAGE_FORGET_ONE, state.public.generation,
        ble_hid_bond_list().revision, ble_hid_target_snapshot().revision, &target, native_now) == ESP_ERR_NOT_FOUND);
    native_setup(false); native_begin(APP_BT_MANAGE_ADD, NULL);
    target_commit_error = ESP_FAIL; target_commit_persists_on_error = true;
    for (unsigned i = 0; i < 10 && native_receipt().active; ++i) native_step(false);
    assert(!native_receipt().active && native_receipt().error != ESP_OK && native_receipt().uncertain && !state.public.pairing_open);
    native_setup(true); target = native_target(0xc3); native_begin(APP_BT_MANAGE_FORGET_ONE, &target);
    for (unsigned i = 0; i < 10 && !state.single_forget.sdk_inflight; ++i) native_step(false);
    assert(state.single_forget.sdk_inflight && native_receipt().irreversible);
    uint64_t deletion = native_receipt().deletion_request_id;
    native_context.locked = true; native_step(false);
    assert(native_receipt().phase == APP_BT_MANAGE_CANCELLED && native_receipt().uncertain &&
        native_receipt().deletion_request_id == deletion);
    remove_complete(target.address, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
    for (unsigned i = 0; i < 5; ++i) native_step(true);
    assert(fake_bond_count == 1 && native_receipt().phase == APP_BT_MANAGE_CANCELLED &&
        native_receipt().uncertain && native_receipt().deletion_request_id == deletion && !state.public.pairing_open);
    native_setup(false); native_begin(APP_BT_MANAGE_ADD, NULL); app_bluetooth_management_cancel();
    for (unsigned i = 0; i < 5; ++i) native_step(false);
    assert(native_receipt().phase == APP_BT_MANAGE_CANCELLED && !state.public.pairing_open && fake_bond_count == 2);
    native_setup(false); native_begin(APP_BT_MANAGE_ADD, NULL); native_context.release_pending = true;
    app_bluetooth_management_service(&native_context, native_now + 30000);
    assert(native_receipt().phase == APP_BT_MANAGE_FAILED && !state.public.pairing_open);
}
