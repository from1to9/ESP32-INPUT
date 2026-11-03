// SPDX-License-Identifier: Apache-2.0
/* Inject real link-down callbacks at the public getter/command lock boundary. */
static void edge_provisional(void)
{
    native_setup(true); active_peer = 0xc3; connect_peer(); drain();
    assert(state.public.connected && !state.public.connection_admitted);
}
static void edge_before_command(void (*hook)(void))
{
    /* Manager snapshot, target snapshot and bond copy each take one lock;
     * the fourth acquisition is reserve or disconnect submission. */
    model_before_state_lock = hook; model_before_state_lock_count = 4;
}
static void edge_begin(bool at_reserve)
{
    const app_bluetooth_management_operation_t operations[] = {
        APP_BT_MANAGE_ADD, APP_BT_MANAGE_SELECT, APP_BT_MANAGE_FORGET_ALL};
    unsigned rejected = 0;
    for (unsigned op = 0; op < 3; ++op) {
        for (unsigned churn = 0; churn < 2; ++churn) {
            edge_provisional();
            const ble_hid_snapshot_t approved = ble_hid_snapshot();
            const uint64_t generation = approved.connection_admitted ? approved.generation : 0;
            const uint64_t bonds = ble_hid_bond_list().revision, policy = ble_hid_target_snapshot().revision;
            ble_hid_target_t target = native_target(0xc2);
            if (churn) {
                if (at_reserve) edge_before_command(disconnect_peer);
                else disconnect_peer();
            }
            esp_err_t error = app_bluetooth_management_begin(operations[op], generation, bonds,
                policy, op == 1 ? &target : NULL, native_now);
            assert(!model_before_state_lock_count);
            fprintf(stderr, "Begin edge: reserve=%u operation=%u churn=%u raw=%llu->%llu error=%d\n",
                at_reserve, (unsigned)operations[op], churn, (unsigned long long)approved.generation,
                (unsigned long long)state.public.generation, error);
            if (error != ESP_OK) { ++rejected; continue; }
            if (!churn) { app_bluetooth_management_cancel(); continue; }
            native_context.release_pending = true; native_finish();
            assert(!close_calls && !state.public.connection_admitted);
            assert(fake_bond_count == (op == 2 ? 0 : 2) && remove_calls == (op == 2 ? 2U : 0U));
        }
    }
    assert(!rejected);
}
static void edge_disconnect(void)
{
    unsigned rejected = 0;
    for (unsigned select = 0; select < 2; ++select) {
        edge_provisional(); ble_hid_target_t target = native_target(0xc2);
        native_begin(select ? APP_BT_MANAGE_SELECT : APP_BT_MANAGE_ADD, select ? &target : NULL);
        edge_before_command(disconnect_peer); native_step(false);
        app_bluetooth_management_snapshot_t receipt = native_receipt();
        fprintf(stderr, "Disconnect edge: operation=%u error=%d active=%u SDK closes=%u\n",
            (unsigned)receipt.operation, receipt.error, receipt.active, close_calls);
        assert(!model_before_state_lock_count && !close_calls);
        if (!receipt.active || receipt.error != ESP_OK) { ++rejected; continue; }
        native_context.release_pending = true; native_finish();
        assert(!close_calls && !remove_calls && fake_bond_count == 2);
    }
    assert(!rejected);
}
static void edge_replacement(void)
{
    disconnect_peer(); connect_peer(); drain(); authorize_peer(true);
    assert(state.public.connection_admitted && state.public.ready);
}
static void edge_catalogue_change(void)
{
    disconnect_peer(); save_sdk_bond(0, 0xc2, 0x55);
    assert(refresh_known_bonds() == ESP_OK);
}
static void edge_policy_change(void)
{
    disconnect_peer();
    assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK); worker_cycle();
}
static void edge_transport_failure(void)
{
    disconnect_peer(); fail_transport("Injected transport failure", ESP_FAIL);
}
static void edge_counterexamples(void)
{
    void (*changes[])(void) = {edge_replacement, edge_catalogue_change, edge_policy_change, disconnect_peer};
    for (unsigned where = 0; where < 2; ++where) {
        for (unsigned changed = 0; changed < 4; ++changed) {
            edge_provisional();
            if (changed == 3) authorize_peer(true);
            const uint64_t host_generation = native_host_generation();
            const uint64_t bonds = ble_hid_bond_list().revision, policy = ble_hid_target_snapshot().revision;
            if (where) edge_before_command(changes[changed]); else changes[changed]();
            assert(app_bluetooth_management_begin(APP_BT_MANAGE_ADD, host_generation, bonds, policy, NULL, native_now) == ESP_ERR_INVALID_STATE);
            assert(!model_before_state_lock_count && !state.management_reservation && !native_receipt().active);
            assert(!close_calls && !remove_calls && fake_bond_count == 2);
            if (!changed) assert(state.public.connection_admitted && state.public.ready);
        }
    }
    for (unsigned changed = 0; changed < 4; ++changed) {
        edge_provisional();
        if (!changed) authorize_peer(true); /* An admitted host's loss is not provisional churn. */
        native_begin(APP_BT_MANAGE_ADD, NULL);
        void (*change)(void) = changed == 0 ? disconnect_peer : changed == 1 ? edge_catalogue_change :
            changed == 2 ? edge_policy_change : edge_transport_failure;
        edge_before_command(change); native_step(false);
        assert(!model_before_state_lock_count);
        for (unsigned i = 0; i < 64 && native_receipt().active; ++i) native_step(true);
        assert(native_receipt().phase == APP_BT_MANAGE_FAILED && native_receipt().error != ESP_OK);
        assert(!state.management_reservation && !remove_calls && fake_bond_count == 2 && !state.public.pairing_open);
        assert(ble_hid_target_snapshot().selected == (changed != 2));
    }
}
static void management_edges_cases(void)
{
    const char *selected = getenv("BLE_MANAGEMENT_EDGES");
    if (selected && !strcmp(selected, "all")) selected = NULL;
    if (!selected || !strcmp(selected, "begin")) edge_begin(false);
    if (!selected || !strcmp(selected, "reserve")) edge_begin(true);
    if (!selected || !strcmp(selected, "disconnect")) edge_disconnect();
    if (!selected || !strcmp(selected, "counter")) edge_counterexamples();
    if (selected && !strcmp(selected, "callers")) {
        native_review_probe_expiry_wrap();
        native_management_manual_disconnect_and_private_reconnect();
        native_management_select_and_reconnect();
        native_management_cancellation_failure_and_stale();
    }
}
