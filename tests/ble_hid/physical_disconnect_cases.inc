// SPDX-License-Identifier: Apache-2.0
/* Production management/adapter with independently controlled SDK logical and
 * physical completions. The model follows the pinned L2CAP contracts retained
 * in source-proof-hashes.json; it does not stand in for device qualification. */
static void model_logical_disconnect(void)
{
    esp_ble_gatts_cb_param_t event = {0};
    event.disconnect.conn_id = 3;
    memcpy(event.disconnect.remote_bda,model_acl_address,ESP_BD_ADDR_LEN);
    gatts_callback(ESP_GATTS_DISCONNECT_EVT,7,&event);
}

static void model_complete_acl(const uint8_t address[6], uint16_t conn_id)
{
    if (!memcmp(address,model_acl_address,ESP_BD_ADDR_LEN)) {
        model_acl_present = false;model_acl_closing = false;
    }
    esp_ble_gatts_cb_param_t event = {0};event.disconnect.conn_id = conn_id;
    memcpy(event.disconnect.remote_bda,address,ESP_BD_ADDR_LEN);
    callback_active = true;gatts_callback(ESP_GATTS_DISCONNECT_EVT,7,&event);callback_active = false;
}

static void model_connected(void)
{
    active_peer = 0xc3;connect_peer();drain();authorize_peer(true);subscribe();
    assert(state.public.connected && state.public.ready && state.public.peer_known);
    memcpy(model_acl_address,state.address,ESP_BD_ADDR_LEN);
    model_acl_lifecycle = model_acl_present = true;
    gatts_close_calls = gap_disconnect_calls = premature_advertising_calls = 0;
    close_calls = 0;
}

static void model_step(void)
{
    native_now += 20;
    app_bluetooth_management_service(&native_context,native_now);
    worker_cycle();
    if (state.adv_stop_pending)advertising_stopped(ESP_BT_STATUS_SUCCESS);
    if (state.adv_pending)trace_completion(model_acl_present ? 263U:0U);
}

static void physical_disconnect_add_waits_for_acl(void)
{
    native_setup(false);model_connected();
    const unsigned bonds = (unsigned)fake_bond_count;
    const uint64_t preference = ble_hid_target_snapshot().revision;
    native_begin(APP_BT_MANAGE_ADD,NULL);
    model_step();model_step();
    printf("ACL barrier before completion: connected=%u closing=%u GAP=%u GATT=%u early_adv=%u failed=%u\n",
        state.public.connected,model_acl_closing,gap_disconnect_calls,gatts_close_calls,
        premature_advertising_calls,state.public.failed);
    assert(state.public.connected && !state.public.ready && model_acl_present && model_acl_closing);
    assert(!premature_advertising_calls && !state.public.failed && gap_disconnect_calls == 1 && !gatts_close_calls);
    assert(native_receipt().phase == APP_BT_MANAGE_DISCONNECT);
    assert(ble_hid_target_snapshot().revision == preference && !state.public.pairing_open);
    model_complete_acl(model_acl_address,3);
    for (unsigned i = 0; i < 64 && native_receipt().active; ++i)model_step();
    assert(native_receipt().phase == APP_BT_MANAGE_COMPLETE && !native_receipt().active);
    assert(state.public.advertising && state.public.pairing_open && !state.public.failed);
    assert(!model_acl_present && !premature_advertising_calls && fake_bond_count == (int)bonds && !remove_calls);
    assert(!ble_hid_target_snapshot().selected && ble_hid_target_snapshot().durable);
}

static void physical_disconnect_missing_error_and_identity(void)
{
    for (unsigned error = 0; error < 2; ++error) {
        native_setup(false);model_connected();
        const uint64_t revision = ble_hid_target_snapshot().revision;
        if(error)model_gap_disconnect_result = ESP_FAIL;
        native_begin(APP_BT_MANAGE_ADD,NULL);model_step();
        assert(state.public.connected && !state.public.ready && gap_disconnect_calls == 1 && !gatts_close_calls);
        if(error)assert(ble_hid_failure().recorded && ble_hid_failure().error == ESP_FAIL);
        /* No physical completion follows an accepted-but-failed command either.
         * Existing management deadline must fail without a false disconnect. */
        native_now += 30000;app_bluetooth_management_service(&native_context,native_now);
        assert(!native_receipt().active && native_receipt().phase == APP_BT_MANAGE_FAILED);
        assert(ble_hid_target_snapshot().revision == revision && !state.public.pairing_open);
        for(unsigned i=0;i<4;++i)worker_cycle();
        assert(gap_disconnect_calls == 1 && !premature_advertising_calls && state.public.connected);
    }
    reset();
    /* A real CONNECT callback supplies the full live random/privacy address.
     * The inactive stored target key is deliberately different. */
    const uint8_t live[6] = {0x42,0x12,0x23,0x34,0x45,0x56};
    esp_ble_gatts_cb_param_t live_event = {0};live_event.connect.conn_id = 3;
    memcpy(live_event.connect.remote_bda,live,6);live_event.connect.ble_addr_type = 1;
    gatts_callback(ESP_GATTS_CONNECT_EVT,7,&live_event);
    assert(state.public.peer_known && state.peer_address_type_known && state.peer_address_type == 1);
    assert(!memcmp(state.address,live,6));
    memset(state.target.target.address,0x99,6);
    assert(ble_hid_disconnect_generation(state.public.generation));worker_cycle();
    assert(gap_disconnect_calls == 1 && !memcmp(last_gap_disconnect_address,live,6));
    assert(!gatts_close_calls);
    reset();connect_peer();drain();subscribe();
    state.public.peer_known = false;
    assert(ble_hid_disconnect_generation(state.public.generation));worker_cycle();
    assert(!gap_disconnect_calls && !gatts_close_calls && state.public.failed);
    assert(ble_hid_failure().recorded && ble_hid_failure().error == ESP_ERR_INVALID_STATE);
}

static void physical_disconnect_stale_duplicate_and_rejected_peers(void)
{
    native_setup(false);model_connected();
    const uint64_t old_generation = state.public.generation;
    assert(ble_hid_disconnect_generation(old_generation));worker_cycle();
    assert(!ble_hid_disconnect_generation(old_generation));
    const uint8_t rejected[6] = {0xc2};
    esp_ble_gatts_cb_param_t incoming = {0};incoming.connect.conn_id = 4;
    memcpy(incoming.connect.remote_bda,rejected,6);incoming.connect.ble_addr_type = 1;
    gatts_callback(ESP_GATTS_CONNECT_EVT,7,&incoming);worker_cycle();
    assert(state.public.connected && !memcmp(state.address,model_acl_address,6) && state.rejected_pending);
    assert(gap_disconnect_calls == 2 && !memcmp(last_gap_disconnect_address,rejected,6));
    model_complete_acl(rejected,4);assert(state.public.connected && !state.rejected_pending);
    model_complete_acl(model_acl_address,3);assert(!state.public.connected);
    const uint64_t disconnected = state.public.generation;
    model_complete_acl(model_acl_address,3);assert(state.public.generation == disconnected);
    /* A delayed prior peer callback cannot tear down a replacement connection. */
    active_peer = 0xc3;connect_peer();drain();authorize_peer(true);subscribe();
    const uint64_t replacement = state.public.generation;
    model_complete_acl(rejected,4);
    assert(state.public.connected && state.public.generation == replacement && state.public.ready);
}


static void physical_disconnect_reused_ids_and_obsolete_reject(void)
{
    reset();connect_peer();drain();subscribe();
    const uint8_t prior[6] = {0x42};
    model_acl_lifecycle = model_acl_present = true;memcpy(model_acl_address,prior,6);
    assert(ble_hid_disconnect_generation(state.public.generation));worker_cycle();
    model_complete_acl(prior,3);
    active_peer = 0x43;connect_peer();drain();subscribe();
    const uint64_t current = state.public.generation;
    assert(state.public.connected && state.public.ready);
    model_complete_acl(prior,3); /* Same SDK conn_id, previous live address. */
    assert(state.public.connected && state.public.generation == current && state.public.ready);
    reset();connect_peer();drain();subscribe();
    esp_ble_gatts_cb_param_t peer = {0};peer.connect.conn_id = 3;
    peer.connect.remote_bda[0] = 0x43;peer.connect.ble_addr_type = 1;
    gatts_callback(ESP_GATTS_CONNECT_EVT,7,&peer);
    gatts_callback(ESP_GATTS_CONNECT_EVT,7,&peer); /* Supersedes the earlier rejected token. */
    assert(state.rejected_pending && state.rejected_generation == 2);
    worker_cycle();assert(!gap_disconnect_calls); /* Obsolete queued identity is skipped. */
    worker_cycle();
    assert(gap_disconnect_calls == 1 && !gatts_close_calls && last_gap_disconnect_address[0] == 0x43);
    assert(state.public.connected && state.address[0] == 0x42);
}


static void model_auth_during_close_reservation(void)
{
    authentication_event(true,true);
}
static void model_replacement_during_close_reservation(void)
{
    model_complete_acl(model_acl_address,3);
    connect_peer(); /* Reuses both SDK conn_id and address; connection generation changes. */
    model_acl_present = true;
}
static void physical_disconnect_reservation_auth_and_replacement(void)
{
    for (unsigned error = 0; error < 2; ++error) {
        native_setup(false);model_connected();
        const uint32_t auth = state.auth_revision;
        if (error) model_gap_disconnect_result = ESP_FAIL;
        assert(ble_hid_disconnect_generation(state.public.generation));
        model_after_close_reservation = model_auth_during_close_reservation;
        worker_cycle();
        printf("ACL reservation auth race: hooks=%u auth_before=%u auth_after=%u GAP=%u closing=%u\n",
            model_reservation_hooks,(unsigned)auth,(unsigned)state.auth_revision,gap_disconnect_calls,state.closing);
        assert(model_reservation_hooks == 1 && state.auth_revision == auth + 1U);
        assert(gap_disconnect_calls == 1 && state.closing && state.public.connected);
        if (error) assert(state.public.failed && ble_hid_failure().error == ESP_FAIL);
        worker_cycle();assert(gap_disconnect_calls == 1);
        model_complete_acl(model_acl_address,3);assert(!state.public.connected && !state.closing);
    }
    native_setup(false);model_connected();
    const uint64_t connection = state.connection_generation;
    assert(ble_hid_disconnect_generation(state.public.generation));
    model_after_close_reservation = model_replacement_during_close_reservation;
    worker_cycle();
    assert(model_reservation_hooks == 1 && state.public.connected && state.connection_generation != connection);
    assert(!gap_disconnect_calls && !gatts_close_calls && !state.closing && !state.link_fault);
    authorize_peer(true);subscribe();assert(state.public.ready && !state.public.failed);
}
