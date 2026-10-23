// SPDX-License-Identifier: Apache-2.0
/* Real native management/adapter execution; substitutes stay at SDK/RTOS/NVS
 * boundaries. No production admission rule is replaced by an observer model. */
static const ble_hid_admission_observation_t *admission_last(
    const ble_hid_admission_diagnostics_t *trace, ble_hid_admission_event_t event)
{
    for (size_t i = trace->count; i; --i) if (trace->events[i - 1].event == event) return &trace->events[i - 1];
    assert(false); return NULL;
}

static void admission_queue_security(void)
{
    esp_ble_gap_cb_param_t p = {0}; p.ble_security.ble_req.bd_addr[0] = active_peer;
    callback_active = true; gap_callback(ESP_GAP_BLE_SEC_REQ_EVT, &p); callback_active = false;
}

static void admission_auth_failure(uint8_t peer, uint8_t reason)
{
    esp_ble_gap_cb_param_t p = {0}; p.ble_security.auth_cmpl.bd_addr[0] = peer;
    p.ble_security.auth_cmpl.fail_reason = reason;
    callback_active = true; gap_callback(ESP_GAP_BLE_AUTH_CMPL_EVT, &p); callback_active = false;
}

static void test_admission_saved_denial_and_explicit_add(void)
{
    reset(); make_bond(0, active_peer, 0x11); ble_hid_pairing_close(); connect_peer(); drain();
    security_request(); assert(!last_security_accept && !encryption_calls);
    ble_hid_admission_diagnostics_t trace; ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.connect_known && trace.connect.accepted && trace.connect.subject_saved_known && trace.connect.subject_saved);
    assert(trace.connect.window_known && !trace.connect.window_allowed && !(trace.connect.flags & BLE_HID_ADMISSION_PENDING));
    const ble_hid_admission_observation_t *decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(!decision->accepted && decision->current && !decision->deadline_known);
    assert((decision->rejected_predicates & (BLE_HID_ADMISSION_DENIED_COMMAND | BLE_HID_ADMISSION_NOT_GRANTED)) ==
        (BLE_HID_ADMISSION_DENIED_COMMAND | BLE_HID_ADMISSION_NOT_GRANTED));
    assert(admission_last(&trace, BLE_HID_ADMISSION_SEC_RETURN)->status_known);

    native_setup(false); native_begin(APP_BT_MANAGE_ADD, NULL); native_finish();
    assert(state.public.pairing_open && !ble_hid_target_snapshot().selected);
    active_peer = 0xd4; connect_peer(); drain(); service_pairing_admission();
    assert(state.pairing_admitted && encryption_calls == 1);
    ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.open_known && trace.last_open.accepted && trace.last_open.window_allowed && trace.last_open.window_remaining_ms);
    assert(trace.connect.accepted && !trace.connect.subject_saved && trace.connect.window_allowed);
    assert(trace.connect.flags & BLE_HID_ADMISSION_PENDING);
    assert(trace.admission_known && trace.first_admission.committed && trace.first_admission.accepted &&
        trace.first_admission.current && trace.first_admission.deadline_known && trace.first_admission.deadline_ok);
    assert(trace.first_admission.refresh_known && trace.first_admission.refresh_error == ESP_OK);
#if CONFIG_BT_CLASSIC_ENABLED
    assert(trace.first_admission.shared_count_known);
#else
    assert(!trace.first_admission.shared_count_known);
#endif
    ble_hid_pairing_close(); security_request(); assert(last_security_accept);
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(decision->accepted && decision->window_known && !decision->window_allowed && !decision->rejected_predicates);
    assert(!admission_last(&trace, BLE_HID_ADMISSION_SEC_RETURN)->tick_known);
}

static void admission_changed_auth_during_refresh(void)
{
    bond_hook = NULL; authentication_event(false, true);
}

static void admission_invalidated_during_refresh(void)
{
    bond_hook = NULL;
    portENTER_CRITICAL(&state_lock); invalidate_bonds_locked(); portEXIT_CRITICAL(&state_lock);
}

static void test_admission_worker_predicates_and_stale_commands(void)
{
    reset(); connect_peer(); drain(); service_pairing_admission();
    admission_queue_security(); control_command_t command = pop();
    command.generation += 1; post_control(&command); worker_cycle();
    ble_hid_admission_diagnostics_t trace; ble_hid_copy_admission_diagnostics(&trace);
    const ble_hid_admission_observation_t *decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(!last_security_accept && !decision->current && !decision->address_match_known &&
        (decision->rejected_predicates & BLE_HID_ADMISSION_NOT_CURRENT));
    admission_queue_security(); command = pop(); command.address[0] ^= 1U; post_control(&command); worker_cycle();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(!last_security_accept && decision->address_match_known && !decision->address_match);
    admission_queue_security(); command = pop(); command.auth_revision += 1; post_control(&command); worker_cycle();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(last_security_accept && decision->current && decision->command_auth_revision != decision->auth_revision);
    /* The existing SECURITY worker owns physical identity, with no revision gate. */
    admission_queue_security(); command = pop(); command.accept = false; post_control(&command); worker_cycle();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(!last_security_accept && decision->current && decision->deadline_known && decision->deadline_ok &&
        decision->rejected_predicates == BLE_HID_ADMISSION_DENIED_COMMAND);
    fake_tick = PAIRING_HANDSHAKE_TIMEOUT_MS; admission_queue_security(); command = pop();
    command.accept = true; post_control(&command); worker_cycle();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION);
    assert(!last_security_accept && decision->deadline_known && !decision->deadline_ok &&
        (decision->rejected_predicates & BLE_HID_ADMISSION_DEADLINE));

    reset(); connect_peer(); drain(); bond_result = ESP_FAIL; service_pairing_admission();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_CHECK);
    assert(!decision->retry && decision->committed && !decision->accepted && !decision->deadline_known &&
        decision->refresh_known && decision->refresh_error == ESP_FAIL && trace.admission_known && !encryption_calls);
    reset(); connect_peer(); drain(); bond_hook = admission_invalidated_during_refresh; service_pairing_admission();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_CHECK);
    assert(decision->retry && !decision->committed && !decision->accepted && !decision->deadline_known &&
        decision->refresh_known && decision->refresh_error == ESP_ERR_INVALID_STATE && !trace.admission_known);
    service_pairing_admission(); ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.admission_known && trace.first_admission.accepted && encryption_calls == 1);

    reset(); connect_peer(); drain();
    for (unsigned i = 0; i < PAIRING_BOND_CAPACITY; ++i) save_sdk_bond(i, (uint8_t)(0x60U + i), (uint8_t)(i + 1U));
    service_pairing_admission(); ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.first_admission.committed && !trace.first_admission.accepted && !trace.first_admission.deadline_known &&
        trace.first_admission.bond_count == PAIRING_BOND_CAPACITY &&
        (trace.first_admission.rejected_predicates & BLE_HID_ADMISSION_FULL) && !encryption_calls);
#if CONFIG_BT_CLASSIC_ENABLED
    reset(); connect_peer(); drain(); shared_bond_count = -1; service_pairing_admission();
    ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.first_admission.shared_count_known && trace.first_admission.shared_count == -1 &&
        !trace.first_admission.accepted && !trace.first_admission.deadline_known &&
        (trace.first_admission.rejected_predicates & BLE_HID_ADMISSION_SHARED_UNAVAILABLE));
#endif

    reset(); connect_peer(); drain(); bond_hook = admission_changed_auth_during_refresh; service_pairing_admission();
    ble_hid_copy_admission_diagnostics(&trace); decision = admission_last(&trace, BLE_HID_ADMISSION_CHECK);
    assert(!decision->current && decision->address_match_known && decision->address_match &&
        decision->auth_revision != decision->command_auth_revision && !decision->committed && !encryption_calls);
}

static void test_admission_anchors_overflow_freeze_and_copy(void)
{
    reset(); unsigned initial_ticks = tick_reads; assert(!ble_hid_pairing_open(0));
    ble_hid_admission_diagnostics_t invalid; ble_hid_copy_admission_diagnostics(&invalid);
    assert(invalid.open_known && !invalid.last_open.accepted && !invalid.last_open.tick_known &&
        !invalid.last_open.window_known && initial_ticks == tick_reads);
    reset(); fake_tick = UINT32_MAX - 1000U; assert(ble_hid_pairing_open(2000));
    fake_tick += 1999U; connect_peer(); drain(); service_pairing_admission();
    for (unsigned i = 0; i < 12; ++i) security_request();
    ble_hid_admission_diagnostics_t trace; ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.overflowed && trace.count == BLE_HID_ADMISSION_TRACE_CAPACITY && !trace.frozen);
    assert(trace.last_open.tick_low32 == UINT32_MAX - 1000U && trace.last_open.window_remaining_ms == 2000);
    assert(trace.connect.window_remaining_ms == 1 && trace.admission_known && trace.first_admission.accepted);
    assert(trace.last_open.sequence < trace.events[0].sequence && trace.connect.sequence < trace.events[0].sequence &&
        trace.first_admission.sequence < trace.events[0].sequence);
    for (size_t i = 1; i < trace.count; ++i) assert(trace.events[i].sequence == trace.events[i - 1].sequence + 1U);
    admission_auth_failure(active_peer, 86); ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.frozen && trace.first_failure_known && trace.first_failure.status_known &&
        trace.first_failure.status == 86 && trace.first_failure.status_domain == BLE_HID_ADMISSION_STATUS_AUTH_REASON &&
        trace.first_failure.current && !trace.first_failure.command_known);
    const ble_hid_admission_diagnostics_t frozen = trace;
    unsigned ticks = tick_reads, clocks = uptime_reads, sdk = sdk_calls, heaps = advertising_heap_reads;
    unsigned count_reads = bond_count_reads, list_reads = bond_list_reads;
    for (unsigned i = 0; i < 100; ++i) {
        ble_hid_copy_admission_diagnostics(NULL); ble_hid_copy_admission_diagnostics(&trace);
        assert(!memcmp(&trace, &frozen, sizeof trace)); trace.connect.accepted = false;
    }
    assert(ticks == tick_reads && clocks == uptime_reads && sdk == sdk_calls && heaps == advertising_heap_reads &&
        count_reads == bond_count_reads && list_reads == bond_list_reads);
    disconnect_peer(); assert(refresh_known_bonds() == ESP_OK);
    assert(ble_hid_pairing_open(1000)); active_peer = 0x43; connect_peer();
    ble_hid_copy_admission_diagnostics(&trace); assert(!memcmp(&trace, &frozen, sizeof trace));

    reset(); admission_diagnostics.total_events = UINT64_MAX; assert(ble_hid_pairing_open(1));
    ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.sequence_exhausted && trace.frozen && !trace.count && !state.public.failed);
    reset(); assert(ble_hid_pairing_open(1)); uint64_t events = admission_diagnostics.total_events;
    fake_tick = 1; notification_watchdog(NULL); assert(!state.public.pairing_open && admission_diagnostics.total_events == events);
    connect_peer(); ble_hid_copy_admission_diagnostics(&trace);
    assert(!trace.events[trace.count - 1].accepted && trace.events[trace.count - 1].command_is_rejected &&
        trace.events[trace.count - 1].window_known && !trace.events[trace.count - 1].window_allowed);
    assert(!strcmp(ble_hid_admission_event_name((ble_hid_admission_event_t)999), "unknown"));
}

static void admission_failure_inside_security_api(void)
{
    security_response_hook = NULL; admission_auth_failure(active_peer, 86);
}

static void test_admission_failure_before_api_return_and_unknown_origin(void)
{
    reset(); connect_peer(); drain(); service_pairing_admission();
    security_response_hook = admission_failure_inside_security_api; security_request();
    ble_hid_admission_diagnostics_t trace; ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.frozen && trace.first_failure.status == 86);
    assert(admission_last(&trace, BLE_HID_ADMISSION_SEC_DECISION)->accepted);
    for (size_t i = 0; i < trace.count; ++i) assert(trace.events[i].event != BLE_HID_ADMISSION_SEC_RETURN);
    assert(trace.events[trace.count - 1].event == BLE_HID_ADMISSION_AUTH);
    reset(); connect_peer(); drain(); admission_auth_failure(0x43, 86); ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.frozen && !trace.first_failure.current && trace.first_failure.address_match_known &&
        !trace.first_failure.address_match && !state.link_fault && !ble_hid_failure().recorded);
    reset(); connect_peer(); drain(); disconnect_peer(); admission_auth_failure(active_peer, 86);
    ble_hid_copy_admission_diagnostics(&trace);
    assert(trace.frozen && !trace.first_failure.current && !trace.first_failure.address_match_known);
    reset(); connect_peer(); drain(); service_pairing_admission(); security_response_result = ESP_FAIL; security_request();
    ble_hid_copy_admission_diagnostics(&trace);
    const ble_hid_admission_observation_t *result = admission_last(&trace, BLE_HID_ADMISSION_SEC_RETURN);
    assert(result->accepted && result->status_known && result->status == ESP_FAIL &&
        result->status_domain == BLE_HID_ADMISSION_STATUS_ESP_ERROR && !result->tick_known);
}

typedef struct {
    unsigned char state_bytes[sizeof state], name_bytes[sizeof device_name], queue_bytes[sizeof queue_storage];
    unsigned char subscription_bytes[sizeof subscriptions], pending_bytes[sizeof pending_subscriptions];
    unsigned sdk, encryption, ticks, clocks, heaps, count_reads, list_reads, writes, target_writes, queue_count;
    bool security_accept;
} admission_control_receipt_t;

static admission_control_receipt_t admission_neutrality_run(bool observing)
{
    native_setup(false); admission_diagnostics.frozen = !observing;
    native_begin(APP_BT_MANAGE_ADD, NULL); native_finish();
    active_peer = 0xd4; connect_peer(); drain(); service_pairing_admission();
    ble_hid_pairing_close(); security_request(); authentication_event(false, true); worker_cycle();
    disconnect_peer(); worker_cycle();
    admission_control_receipt_t receipt = {0};
    memcpy(receipt.state_bytes, &state, sizeof state); memcpy(receipt.name_bytes, &device_name, sizeof device_name);
    memcpy(receipt.queue_bytes, &queue_storage, sizeof queue_storage);
    memcpy(receipt.subscription_bytes, &subscriptions, sizeof subscriptions);
    memcpy(receipt.pending_bytes, &pending_subscriptions, sizeof pending_subscriptions);
    receipt.sdk = sdk_calls; receipt.encryption = encryption_calls; receipt.ticks = tick_reads;
    receipt.clocks = uptime_reads; receipt.heaps = advertising_heap_reads;
    receipt.count_reads = bond_count_reads; receipt.list_reads = bond_list_reads;
    receipt.writes = commit_calls; receipt.target_writes = target_commit_calls;
    receipt.queue_count = queue_storage.count; receipt.security_accept = last_security_accept;
    return receipt;
}

static void test_admission_observation_neutrality(void)
{
    admission_control_receipt_t suppressed = admission_neutrality_run(false), observed = admission_neutrality_run(true);
    assert(!memcmp(&suppressed, &observed, sizeof observed));
    assert(observed.security_accept && observed.encryption == 1);
    printf("Observer-neutral receipt: sdk=%u encryption=%u ticks=%u uptime=%u heap=%u bond_count=%u bond_list=%u nvs_commits=%u target_commits=%u queue=%u\n",
        observed.sdk, observed.encryption, observed.ticks, observed.clocks, observed.heaps, observed.count_reads,
        observed.list_reads, observed.writes, observed.target_writes, observed.queue_count);
    puts("BLE admission diagnostics: native Add, saved denial, short-circuit deadlines, stale identities, overflow anchors, AUTH freeze, missing API return and unchanged control/SDK/clock/storage receipts passed");
}
