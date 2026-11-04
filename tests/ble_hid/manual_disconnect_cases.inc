// SPDX-License-Identifier: Apache-2.0
/* Real management/worker paths; SDK events and stored bonds are boundaries. */
static void manual_setup(void)
{
    native_setup(true);
    assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK); worker_cycle();
}
static void manual_pause_peer(uint8_t peer)
{
    active_peer = peer; connect_peer(); drain(); authorize_peer(true); subscribe();
    assert(ble_hid_disconnect_user_generation(state.public.generation)); worker_cycle(); disconnect_peer();
}
static void manual_add_preserves_pause(void)
{
    manual_setup(); manual_pause_peer(0xc3);
    native_begin(APP_BT_MANAGE_ADD, NULL); native_finish();
    ble_hid_pairing_close(); active_peer = 0xc3; connect_peer();
    assert(!state.public.connected && state.rejected_pending);
    worker_cycle(); disconnect_peer(); active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.ready && fake_bond_count == 2 && !remove_calls);
}
static void manual_select_is_scoped(void)
{
    manual_setup(); manual_pause_peer(0xc3); manual_pause_peer(0xc2);
    ble_hid_target_t target = native_target(0xc2);
    native_begin(APP_BT_MANAGE_SELECT, &target); native_finish();
    assert(manual_pause_matches_locked((uint8_t[6]){0xc3}, 1));
    assert(!manual_pause_matches_locked((uint8_t[6]){0xc2}, 1));
    active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.ready && fake_bond_count == 2 && !remove_calls);
}
static void manual_forget_preserves_other_pause(void)
{
    manual_setup(); manual_pause_peer(0xc3); manual_pause_peer(0xc2);
    ble_hid_target_t removed = native_target(0xc3), remaining = native_target(0xc2);
    native_begin(APP_BT_MANAGE_FORGET_ONE, &removed); native_finish();
    assert(manual_pause_matches_locked((uint8_t[6]){0xc2}, 1));
    assert(!manual_pause_matches_locked((uint8_t[6]){0xc3}, 1));
    native_begin(APP_BT_MANAGE_SELECT, &remaining); native_finish();
    active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.ready && fake_bond_count == 1 && remove_calls == 1);
}
static void manual_authenticated_pause(void)
{
    manual_setup(); manual_pause_peer(0xc3);
    /* The connection type differs from the exported saved key. Only the
     * authenticated SDK fingerprint may prove the paused identity here. */
    esp_ble_gatts_cb_param_t event = {0}; event.connect.conn_id = 3;
    event.connect.remote_bda[0] = 0xc3; event.connect.ble_addr_type = 3;
    gatts_callback(ESP_GATTS_CONNECT_EVT, 7, &event); drain(); authorize_peer(true);
    assert(!state.public.ready && !state.public.failed && state.link_fault);
}
static void manual_resume_is_scoped(void)
{
    manual_setup(); manual_pause_peer(0xc3); manual_pause_peer(0xc2);
    assert(ble_hid_resume_reconnect());
    assert(state.manual_pause_count == 1 && manual_pause_matches_locked((uint8_t[6]){0xc3}, 1));
    assert(!manual_pause_matches_locked((uint8_t[6]){0xc2}, 1));
    active_peer = 0xc2; connect_peer(); drain();
    assert(!ble_hid_snapshot().connection_admitted);
    assert(!ble_hid_disconnect_user_generation(state.public.generation));
    assert(ble_hid_resume_reconnect() && !state.manual_pause_count);
    authorize_peer(true); assert(ble_hid_snapshot().connection_admitted && state.public.ready);
    disconnect_peer(); connect_peer(); drain(); authorize_peer(true); assert(state.public.ready);
}
static void manual_forget_then_private_reconnect(void)
{
    manual_setup(); save_sdk_bond(0, 0x42, 0x11); save_sdk_bond(2, 0xc4, 0x33);
    assert(refresh_known_bonds() == ESP_OK);
    manual_pause_peer(0xc4); manual_pause_peer(0x42); manual_pause_peer(0xc3);
    ble_hid_target_t removed = native_target(0xc3), target = native_target(0x42);
    native_begin(APP_BT_MANAGE_FORGET_ONE, &removed); native_finish();
    assert(fake_bond_count == 2 && state.manual_pause_count == 2);
    native_begin(APP_BT_MANAGE_RECONNECT, &target); native_finish();
    assert(state.manual_pause_count == 1 && manual_pause_matches_locked((uint8_t[6]){0xc4}, 1));
    active_peer = 0x42; connect_peer(); drain();
    assert(state.reconnect_probe_link && !state.public.connection_admitted && !state.public.ready);
    security_request(); assert(!last_security_accept); /* Existing LTK only, no new pairing grant. */
    authorize_peer(true); assert(state.public.connection_admitted && state.public.ready);
    assert(fake_bond_count == 2 && remove_calls == 1);
}
static void manual_provisional_management(void)
{
    manual_setup(); active_peer = 0xc3; connect_peer(); drain();
    assert(state.public.connected && !state.public.connection_admitted);
    native_context.release_pending = true;
    ble_hid_target_t target = native_target(0xc2);
    native_begin(APP_BT_MANAGE_SELECT, &target); native_step(false); native_step(false);
    assert(close_calls == 1 && !state.public.connection_admitted && !state.public.failed);
    disconnect_peer(); native_finish();
    active_peer = 0xc2; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.connection_admitted && state.public.ready);
}
static void manual_zero_bonds_failed_auth(void)
{
    manual_setup();
    ble_hid_target_t first = native_target(0xc3), last = native_target(0xc2);
    native_begin(APP_BT_MANAGE_FORGET_ONE, &first); native_finish();
    manual_pause_peer(0xc2);
    native_begin(APP_BT_MANAGE_FORGET_ONE, &last); native_finish();
    assert(!fake_bond_count && !state.manual_pause_count && remove_calls == 2 && !state.public.pairing_open);
    native_context.release_pending = true; /* A departed host cannot acknowledge the new neutral barrier. */
    native_begin(APP_BT_MANAGE_ADD, NULL); native_finish();
    assert(state.public.pairing_open && state.public.advertising);
    for (unsigned i = 0; i < 40; ++i) {
        active_peer = 0xc2; connect_peer(); drain();
        assert(!state.public.connection_admitted && !state.public.ready);
        assert(!ble_hid_disconnect_user_generation(state.public.generation));
        esp_ble_gap_cb_param_t failure = {0};
        failure.ble_security.auth_cmpl.bd_addr[0] = active_peer;
        /* Frozen device evidence reports API value 102; the pinned SDK maps
         * BTA_DM_AUTH_FAIL_BASE + SMP_CONN_TOUT to this value. Origin unknown. */
        failure.ble_security.auth_cmpl.fail_reason = 102;
        gap_callback(ESP_GAP_BLE_AUTH_CMPL_EVT, &failure); worker_cycle();
        assert(!state.public.connection_admitted && !state.public.ready && !state.public.failed);
        disconnect_peer(); assert(ble_hid_snapshot().pairing_open);
    }
    active_peer = 0x62; connect_peer(); drain(); save_sdk_bond(0, active_peer, 0x52);
    authorize_peer(true); subscribe();
    assert(state.public.connection_admitted && state.public.ready && fake_bond_count == 1 && remove_calls == 2);
}

static void review_provisional_drop(app_bluetooth_management_operation_t operation)
{
    native_setup(true); active_peer = 0xc3; connect_peer(); drain();
    assert(state.public.connected && !state.public.connection_admitted);
    ble_hid_target_t target = native_target(0xc2);
    native_begin(operation, operation == APP_BT_MANAGE_SELECT ? &target : NULL);
    disconnect_peer(); /* The candidate leaves before management submits a disconnect. */
    native_context.release_pending = true;
    native_finish();
    assert(!close_calls && !remove_calls && fake_bond_count == 2);
    assert(state.public.pairing_open == (operation == APP_BT_MANAGE_ADD));
    assert(ble_hid_target_snapshot().selected == (operation == APP_BT_MANAGE_SELECT));
}

static void review_classic_cannot_overtake(app_bluetooth_management_operation_t operation)
{
    native_setup(true); ble_hid_target_t target = native_target(0xc2);
    native_begin(operation, operation == APP_BT_MANAGE_FORGET_ONE ? &target : NULL);
    bool claimed = ble_hid_pairing_claim_classic(true);
    for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(true);
    app_bluetooth_management_snapshot_t receipt = native_receipt();
    fprintf(stderr, "Shared guard: operation=%u classic_claimed=%u phase=%u error=%d selected=%u removals=%u\n",
        (unsigned)operation, claimed, (unsigned)receipt.phase, receipt.error,
        ble_hid_target_snapshot().selected, remove_calls);
    assert(!claimed && receipt.phase == APP_BT_MANAGE_COMPLETE && receipt.error == ESP_OK);
    assert(fake_bond_count == (operation == APP_BT_MANAGE_ADD ? 2U : 1U));
    assert(remove_calls == (operation == APP_BT_MANAGE_ADD ? 0U : 1U));
    if (operation == APP_BT_MANAGE_ADD) {
        assert(state.public.pairing_open && !ble_hid_pairing_claim_classic(false));
        ble_hid_pairing_close();
    }
    assert(ble_hid_pairing_claim_classic(false)); ble_hid_pairing_release_classic();
}

static void review_management_opposite_boundaries(void)
{
    const app_bluetooth_management_operation_t operations[] = {APP_BT_MANAGE_ADD, APP_BT_MANAGE_SELECT};
    for (unsigned op = 0; op < 2; ++op) {
        for (unsigned changed = 0; changed < 2; ++changed) {
            native_setup(true); active_peer = 0xc3; connect_peer(); drain();
            if (!changed) authorize_peer(true);
            ble_hid_target_t target = native_target(0xc2);
            native_begin(operations[op], op ? &target : NULL);
            disconnect_peer();
            if (changed) { save_sdk_bond(0, 0xc2, 0x55); assert(refresh_known_bonds() == ESP_OK); }
            for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(true);
            assert(native_receipt().phase == APP_BT_MANAGE_FAILED && native_receipt().error == ESP_ERR_INVALID_STATE);
            assert(ble_hid_target_snapshot().selected && !remove_calls && !state.public.pairing_open);
            assert(ble_hid_pairing_claim_classic(false)); ble_hid_pairing_release_classic();
        }
    }
    native_setup(true); assert(ble_hid_pairing_claim_classic(false));
    assert(app_bluetooth_management_begin(APP_BT_MANAGE_ADD, native_host_generation(),
        ble_hid_bond_list().revision, ble_hid_target_snapshot().revision, NULL, native_now) == ESP_ERR_INVALID_STATE);
    assert(state.classic_pairing_reserved && ble_hid_target_snapshot().selected &&
        fake_bond_count == 2 && !state.management_reservation);
    ble_hid_pairing_release_classic();
    native_begin(APP_BT_MANAGE_ADD, NULL); assert(!ble_hid_pairing_claim_classic(false));
    app_bluetooth_management_cancel();
    assert(ble_hid_target_snapshot().selected && fake_bond_count == 2 && !remove_calls);
    assert(ble_hid_pairing_claim_classic(false)); ble_hid_pairing_release_classic();
}

static void manual_disconnect_cases(void)
{
    const char *selected = getenv("BLE_MANUAL_CASE");
    if (!selected || !strcmp(selected, "add")) manual_add_preserves_pause();
    if (!selected || !strcmp(selected, "select")) manual_select_is_scoped();
    if (!selected || !strcmp(selected, "forget")) manual_forget_preserves_other_pause();
    if (!selected || !strcmp(selected, "auth")) manual_authenticated_pause();
    if (!selected || !strcmp(selected, "resume")) manual_resume_is_scoped();
    if (!selected || !strcmp(selected, "private")) manual_forget_then_private_reconnect();
    if (!selected || !strcmp(selected, "provisional")) manual_provisional_management();
    if (!selected || !strcmp(selected, "zero")) manual_zero_bonds_failed_auth();
    if (!selected || !strcmp(selected, "review-drop-add")) review_provisional_drop(APP_BT_MANAGE_ADD);
    if (!selected || !strcmp(selected, "review-drop-select")) review_provisional_drop(APP_BT_MANAGE_SELECT);
    if (!selected || !strcmp(selected, "review-guard-add")) review_classic_cannot_overtake(APP_BT_MANAGE_ADD);
    if (!selected || !strcmp(selected, "review-guard-forget")) review_classic_cannot_overtake(APP_BT_MANAGE_FORGET_ONE);
    if (!selected || !strcmp(selected, "review-boundaries")) review_management_opposite_boundaries();
}
