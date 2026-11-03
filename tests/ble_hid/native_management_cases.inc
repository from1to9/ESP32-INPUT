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
static void native_review_former_unbonded_add(void)
{
    native_setup(false); assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK); worker_cycle();
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    active_peer = 0xd4; connect_peer(); drain(); authorize_peer(false); subscribe();
    assert(state.public.ready && !state.peer_saved && fake_bond_count == 2);
    native_begin(APP_BT_MANAGE_ADD, NULL); native_step(false); disconnect_peer(); native_finish();
    connect_peer(); assert(!state.public.connected && state.rejected_pending);
    worker_cycle(); disconnect_peer();
    active_peer = 0xd5; connect_peer(); drain(); worker_cycle();
    assert(state.public.connected && state.pairing_admitted && fake_bond_count == 2 && !remove_calls);
    disconnect_peer(); ble_hid_pairing_close(); native_begin(APP_BT_MANAGE_ADD, NULL); native_finish();
    active_peer = 0xd4; connect_peer(); drain(); worker_cycle();
    assert(state.public.connected && state.pairing_admitted); /* A fresh offline Add displaces no former link. */

    native_setup(true); ble_hid_target_t removed = native_target(0xc3);
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe(); disconnect_peer();
    native_begin(APP_BT_MANAGE_FORGET_ONE, &removed); native_finish();
    assert(fake_bond_count == 1); native_begin(APP_BT_MANAGE_ADD, NULL); native_finish();
    connect_peer(); drain(); worker_cycle();
    assert(state.public.connected && state.pairing_admitted && fake_bond_count == 1 && remove_calls == 1);
}

static void native_review_unverifiable_target_admission(void)
{
    native_setup(true); esp_ble_bond_dev_t complete = fake_bonds[0];
    fake_bonds[0].bond_key.key_mask = ESP_BLE_ID_KEY_MASK; /* Real SDK export lacks verifiable encryption material. */
    assert(refresh_known_bonds() == ESP_OK); ble_hid_target_t target = native_target(0xc2);
    assert(!target.bond_id_known);
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
    const uint64_t generation = state.public.generation, policy = state.target.revision;
    assert(app_bluetooth_management_begin(APP_BT_MANAGE_RECONNECT, generation, ble_hid_bond_list().revision,
        policy, &target, native_now) == ESP_ERR_NOT_SUPPORTED);
    assert(state.public.ready && !state.management_reservation && !close_calls && !remove_calls &&
        state.public.generation == generation && state.target.revision == policy);
    fake_bonds[0] = complete; assert(refresh_known_bonds() == ESP_OK);
    target = native_target(0xc2); target.bond_id[0] ^= 1U;
    assert(app_bluetooth_management_begin(APP_BT_MANAGE_RECONNECT, generation, ble_hid_bond_list().revision,
        policy, &target, native_now) == ESP_ERR_NOT_FOUND);
    assert(state.public.ready && !state.management_reservation && !close_calls);
}

static void native_review_probe_expiry_wrap(void)
{
    reset(); make_bond(0, 0x42, 0x11); ble_hid_pairing_close();
    ble_hid_target_t target = native_target(0x42); uint64_t reservation = 0;
    assert(ble_hid_management_reserve(state.public.generation, &reservation));
    ble_hid_management_reconnect(reservation, &target);
    fake_tick = PAIRING_HANDSHAKE_TIMEOUT_MS; worker_cycle();
    fake_tick = UINT32_MAX; worker_cycle(); fake_tick = 1; active_peer = 0x77; connect_peer();
    assert(!state.public.connected && state.rejected_pending && !state.reconnect_probe);
    worker_cycle(); disconnect_peer();
    fake_tick = 0; fake_uptime_us = 0;
    assert(ble_hid_management_reserve(state.public.generation, &reservation));
    ble_hid_management_reconnect(reservation, &target);
    /* No worker/callback observed the first deadline: monotonic uptime still
     * expires the grant even though the low32 tick appears only one tick old. */
    fake_tick = 1; fake_uptime_us = (INT64_C(1) << 32) * INT64_C(1000) + 1000;
    connect_peer(); assert(!state.public.connected && state.rejected_pending && !state.reconnect_probe);
    worker_cycle(); disconnect_peer();
    assert(ble_hid_management_reserve(state.public.generation, &reservation));
    ble_hid_management_reconnect(reservation, &target); connect_peer();
    assert(state.public.connected && state.reconnect_probe_link && !state.pairing_admission_pending);
}

static void native_review_blocker_cases(void)
{
    const char *selected = getenv("BLE_REVIEW_CASE");
    if (!selected || !strcmp(selected, "B1")) native_review_former_unbonded_add();
    if (!selected || !strcmp(selected, "B2")) native_review_unverifiable_target_admission();
    if (!selected || !strcmp(selected, "B3")) native_review_probe_expiry_wrap();
}

static void native_management_selected_add_and_reconnect(void)
{
    native_review_blocker_cases();
    native_setup(true); active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
    native_begin(APP_BT_MANAGE_ADD, NULL); native_step(false); assert(close_calls == 1);
    disconnect_peer(); connect_peer();
    assert(!state.public.connected && state.rejected_pending); /* Old host cannot overtake the approved Add. */
    worker_cycle(); disconnect_peer(); native_finish();
    connect_peer(); assert(!state.public.connected && state.rejected_pending);
    worker_cycle(); disconnect_peer();
    active_peer = 0xd4; connect_peer(); assert(state.public.connected && state.pairing_admission_pending);

    native_setup(false); native_begin(APP_BT_MANAGE_ADD, NULL);
    for (unsigned poll = 0; poll < 32 && (native_receipt().phase != APP_BT_MANAGE_POLICY ||
            ble_hid_target_snapshot().request_status != BLE_HID_TARGET_APPLIED); ++poll) native_step(false);
    assert(native_receipt().phase == APP_BT_MANAGE_POLICY);
    active_peer = 0xc3; connect_peer(); assert(state.rejected_pending && !state.public.connected);
    native_step(false); assert(native_receipt().active && native_receipt().phase == APP_BT_MANAGE_POLICY && !state.public.pairing_open);
    disconnect_peer(); native_finish(); assert(state.public.pairing_open && fake_bond_count == 2);

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
    ble_hid_pairing_close(); /* Add is for new identities; close it before saved reconnect. */
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
static void native_management_disconnected_release_pending(void)
{
    const app_bluetooth_management_operation_t operations[] = {
        APP_BT_MANAGE_ADD, APP_BT_MANAGE_FORGET_ALL,
        APP_BT_MANAGE_FORGET_ONE, APP_BT_MANAGE_FORGET_ONE};
    for (unsigned op = 0; op < sizeof(operations) / sizeof(operations[0]); ++op) {
        for (unsigned connected = 0; connected < 2; ++connected) {
            native_setup(true);
            input_state_t input; input_init(&input);
            ble_hid_target_t target = native_target(op == 3 ? 0xc2 : 0xc3);
            const ble_hid_target_snapshot_t original_policy = ble_hid_target_snapshot();
            if (connected) {
                active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
                assert(state.public.connected && state.public.ready);
                input_set_connected(&input, true);
            }
            native_begin(operations[op], operations[op] == APP_BT_MANAGE_FORGET_ONE ? &target : NULL);
            native_context.release_pending = input.release_pending;
            if (connected) {
                const uint64_t policy_revision = ble_hid_target_snapshot().revision;
                for (unsigned poll = 0; poll < 3; ++poll) native_step(false);
                assert(!close_calls && !remove_calls && !state.public.pairing_open &&
                    ble_hid_target_snapshot().revision == policy_revision);
                assert(input_ack_release(&input, input.epoch)); /* Model the live host's accepted zero reports. */
                native_context.release_pending = input.release_pending;
                state.link_congested = true; refresh_locked(); native_step(false);
                assert(!close_calls && !remove_calls); /* A live congested host still blocks. */
                state.link_congested = false; refresh_locked(); native_step(false);
                assert(close_calls == 1 && native_receipt().phase == APP_BT_MANAGE_DISCONNECT);
                disconnect_peer();
                input_set_connected(&input, false);
                native_context.release_pending = input.release_pending;
            }
            const uint64_t disconnected_epoch = input.epoch;
            assert(!input_ack_release(&input, disconnected_epoch));
            if (operations[op] != APP_BT_MANAGE_ADD) {
                bool removed = false;
                for (unsigned poll = 0; poll < 64 && !removed; ++poll) {
                    native_step(true);
                    removed = operations[op] == APP_BT_MANAGE_FORGET_ALL ?
                        state.public.bond_forget_state == BLE_HID_FORGET_DONE :
                        state.single_forget.status == BLE_HID_BOND_FORGET_SUCCEEDED;
                }
                const ble_hid_target_snapshot_t policy = ble_hid_target_snapshot();
                assert(removed && native_receipt().phase == APP_BT_MANAGE_DELETE &&
                    policy.revision == original_policy.revision && policy.selected == original_policy.selected &&
                    !memcmp(policy.target.address, original_policy.target.address, sizeof(policy.target.address)) &&
                    !memcmp(policy.target.bond_id, original_policy.target.bond_id, BLE_HID_BOND_ID_BYTES));
                assert(policy.available == (op == 3)); /* A removed selected key stays blocked until reconciliation. */
            }
            native_finish();
            assert(native_context.release_pending && input.release_pending && input.epoch == disconnected_epoch &&
                !input.connected && !input_can_emit(&input, input.epoch, INPUT_LOCAL, INPUT_MOUSE));
            assert(close_calls == connected && !state.public.connected);
            if (operations[op] == APP_BT_MANAGE_ADD) {
                assert(state.public.pairing_open && state.public.advertising && fake_bond_count == 2 &&
                    subscriptions.count == 2 && !remove_calls && !ble_hid_target_snapshot().selected);
            } else if (operations[op] == APP_BT_MANAGE_FORGET_ALL) {
                assert(!state.public.pairing_open && !fake_bond_count && !subscriptions.count &&
                    remove_calls == 2 && !ble_hid_target_snapshot().selected);
            } else {
                ble_hid_bond_list_t list = ble_hid_bond_list();
                const uint8_t retained = op == 3 ? 0xc3 : 0xc2;
                assert(!state.public.pairing_open && fake_bond_count == 1 && subscriptions.count == 1 &&
                    list.count == 1 && list.bonds[0].address[0] == retained && remove_calls == 1 &&
                    ble_hid_target_snapshot().selected == (op == 3));
            }
            assert(ble_hid_target_snapshot().durable);
            input_set_connected(&input, true);
            assert(input.release_pending && !input_ack_release(&input, disconnected_epoch) &&
                !input_can_emit(&input, input.epoch, INPUT_LOCAL, INPUT_MOUSE));
        }
    }
    puts("Native Bluetooth management: disconnected pending releases preserved across Add, Forget all and selected/unselected Forget one; live release/congestion guards retained");
}

static void native_management_manual_disconnect_and_private_reconnect(void)
{
    native_setup(true);
    assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK); worker_cycle();
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
    assert(ble_hid_disconnect_user_generation(state.public.generation)); worker_cycle(); disconnect_peer();
    assert(ble_hid_snapshot().reconnect_paused && state.manual_pause_count == 1);
    connect_peer(); assert(!state.public.connected && state.rejected_pending);
    uint64_t reservation = 0; assert(ble_hid_management_reserve(state.public.generation, &reservation));
    assert(!ble_hid_management_ready(reservation));
    ble_hid_management_release(reservation); /* Cancellation keeps the user's earlier pause. */
    assert(state.manual_pause_count == 1); worker_cycle(); disconnect_peer();
    active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true); subscribe();
    assert(state.public.ready && !ble_hid_resume_reconnect() && state.manual_pause_count == 1);
    assert(ble_hid_disconnect_user_generation(state.public.generation)); worker_cycle(); disconnect_peer();
    assert(state.manual_pause_count == 2); connect_peer(); assert(!state.public.connected && state.rejected_pending);
    worker_cycle(); disconnect_peer(); assert(ble_hid_resume_reconnect() && !state.manual_pause_count);
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); assert(state.public.ready);
    assert(fake_bond_count == 2 && subscriptions.count == 2 && !remove_calls);
    disconnect_peer(); connect_peer(); drain(); authorize_peer(true); assert(state.public.ready); /* Spontaneous loss is normal. */

    native_setup(false); save_sdk_bond(0, 0x42, 0x11); assert(refresh_known_bonds() == ESP_OK);
    ble_hid_target_t target = native_target(0x42);
    assert(!ble_hid_bond_list().bonds[1].selection_supported || !ble_hid_bond_list().bonds[0].selection_supported);
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
    native_begin(APP_BT_MANAGE_RECONNECT, &target); native_step(false); disconnect_peer(); native_finish();
    assert(!ble_hid_target_snapshot().selected && state.manual_pause_count == 1 && !state.public.pairing_open && state.reconnect_probe);
    active_peer = 0x42; connect_peer(); drain(); assert(state.public.connected && !state.public.ready && !state.pairing_admission_pending);
    security_request(); assert(!last_security_accept); authorize_peer(true); subscribe();
    assert(state.public.ready && fake_bond_count == 2 && !remove_calls);

    disconnect_peer(); assert(ble_hid_management_reserve(state.public.generation, &reservation));
    ble_hid_management_reconnect(reservation, &target);
    active_peer = 0x66; connect_peer(); drain(); assert(state.public.connected && !state.public.ready && !state.pairing_admission_pending);
    security_request(); assert(!last_security_accept); authentication_event(false, true); worker_cycle();
    assert(!state.public.ready && !state.public.failed && state.link_fault);
    assert(fake_bond_count == 2 && !remove_calls); /* Mismatch fails closed without deleting any bond. */
    reset(); make_bond(0, 0x42, 0x11); target = native_target(0x42); ble_hid_pairing_close();
    assert(ble_hid_management_reserve(state.public.generation, &reservation));
    ble_hid_management_reconnect(reservation, &target);
    fake_tick = PAIRING_HANDSHAKE_TIMEOUT_MS; active_peer = 0x67; connect_peer();
    assert(!state.public.connected && state.rejected_pending); /* Probe expiry cannot authorize unknown peers. */
    reset(); connect_peer(); drain(); assert(ble_hid_disconnect_user_generation(state.public.generation));
    worker_cycle(); disconnect_peer(); connect_peer(); assert(!state.public.connected && state.rejected_pending);
    worker_cycle(); disconnect_peer(); assert(ble_hid_resume_reconnect()); /* Unbonded/unresolved observed tuple can be resumed. */
    reset();
    for (unsigned i = 0; i < BLE_HID_MAX_BONDS; ++i) {
        active_peer = (uint8_t)(0x10U + i); connect_peer(); drain(); assert(state.public.connected);
        assert(ble_hid_disconnect_user_generation(state.public.generation)); worker_cycle(); disconnect_peer();
    }
    active_peer = 0x70; connect_peer(); drain(); uint64_t generation = state.public.generation;
    assert(state.public.connected && state.manual_pause_count == BLE_HID_MAX_BONDS &&
        !ble_hid_disconnect_user_generation(generation) && state.public.generation == generation);
    assert(ble_hid_disconnect_generation(generation)); worker_cycle(); disconnect_peer();
    assert(ble_hid_resume_reconnect() && !state.manual_pause_count);
    puts("Manual Disconnect: former identities paused, other peer remains actionable, explicit Resume/cancel, private-key fallback pre-auth only, mismatch denial and expiry passed");
}

static void native_management_select_and_reconnect(void);
static void native_management_all_and_single_removal(void)
{
    native_management_manual_disconnect_and_private_reconnect();
    native_management_select_and_reconnect();
    native_management_disconnected_release_pending();
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
        native_setup(false); active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
        native_begin(operations[op], NULL);
        disconnect_peer(); connect_peer(); assert(!state.public.connected && state.rejected_pending);
        const uint64_t unchanged_policy = ble_hid_target_snapshot().revision;
        native_step(false); disconnect_peer(); native_step(false);
        assert(native_receipt().phase == APP_BT_MANAGE_FAILED && !remove_calls &&
            ble_hid_target_snapshot().revision == unchanged_policy && !state.public.pairing_open && !state.management_reservation);
        disconnect_peer();

        for (unsigned replacement = 0; replacement < 3; ++replacement) {
            native_setup(false); active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe();
            native_begin(operations[op], NULL);
            uint64_t generation = ble_hid_snapshot().generation;
            uint64_t policy_revision = ble_hid_target_snapshot().revision;
            native_step(false);
            assert(native_receipt().phase == APP_BT_MANAGE_DISCONNECT && close_calls == 1 &&
                state.public.connected && state.public.generation == generation + 1U && !state.public.ready);
            for (unsigned poll = 0; poll < 20; ++poll) native_step(false);
            assert(native_receipt().phase == APP_BT_MANAGE_DISCONNECT && close_calls == 1 && !remove_calls);
            if (replacement == 2) {
                app_bluetooth_management_service(&native_context, 31000);
                assert(native_receipt().phase == APP_BT_MANAGE_FAILED && native_receipt().error == ESP_ERR_TIMEOUT &&
                    !state.management_reservation && !remove_calls && ble_hid_target_snapshot().revision == policy_revision);
                disconnect_peer(); native_step(true); assert(fake_bond_count == 2 && !state.public.pairing_open);
            } else {
                disconnect_peer();
                if (replacement) {
                    active_peer = 0xc2; connect_peer(); assert(!state.public.connected && state.rejected_pending);
                    worker_cycle(); disconnect_peer();
                }
                native_finish();
                assert(!state.management_reservation && state.public.pairing_open == (operations[op] == APP_BT_MANAGE_ADD));
                assert(fake_bond_count == (operations[op] == APP_BT_MANAGE_ADD ? 2 : 0));
            }
        }
    }
}

static void native_management_select_and_reconnect(void)
{
    for (unsigned connected = 0; connected < 2; ++connected) {
        native_setup(true); ble_hid_target_t target = native_target(0xc2);
        if (connected) { active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe(); }
        native_begin(APP_BT_MANAGE_SELECT, &target);
        if (connected) {
            native_context.release_pending = true; native_step(false); assert(!close_calls);
            native_context.release_pending = false; native_step(false); assert(close_calls == 1);
            disconnect_peer();
            active_peer = 0xc3; connect_peer(); assert(!state.public.connected && state.rejected_pending);
            worker_cycle(); disconnect_peer();
        }
        native_finish();
        ble_hid_target_snapshot_t policy = ble_hid_target_snapshot();
        assert(policy.selected && policy.durable && !state.management_reservation && !state.public.connected &&
            !memcmp(policy.target.bond_id, target.bond_id, BLE_HID_BOND_ID_BYTES) && !state.public.pairing_open &&
            fake_bond_count == 2 && subscriptions.count == 2 && !remove_calls && strstr(native_receipt().message, "not ready"));
        active_peer = 0xc3; connect_peer(); assert(!state.public.connected && state.rejected_pending);
        worker_cycle(); disconnect_peer();
        active_peer = 0xc2; connect_peer(); drain(); assert(state.public.connected && !state.public.ready);
        authorize_peer(true); assert(state.public.ready); /* Stored per-host subscriptions restore only after real auth. */
        assert(ble_hid_disconnect_generation(state.public.generation)); worker_cycle(); disconnect_peer();
        connect_peer(); drain(); assert(state.public.connected && !state.public.ready);
        authorize_peer(true); assert(state.public.ready && fake_bond_count == 2 && !remove_calls);
    }
    reset(); uint64_t first = 0, second = 0;
    assert(ble_hid_management_reserve(state.public.generation, &first));
    ble_hid_management_release(first); assert(ble_hid_management_reserve(state.public.generation, &second));
    ble_hid_management_release(first); assert(state.management_reservation == second);
    ble_hid_management_release(second); assert(!state.management_reservation);
    state.management_sequence = UINT64_MAX;
    assert(!ble_hid_management_reserve(state.public.generation, &second) && !state.public.failed);
    puts("Native saved-host selection: live release, overtaking rejection, durable exact policy, pending versus authenticated readiness, ordinary reconnect and reservation ownership passed");
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
