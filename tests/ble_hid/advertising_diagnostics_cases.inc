// SPDX-License-Identifier: Apache-2.0
static void trace_completion(unsigned status)
{
    esp_ble_gap_cb_param_t p = {0};
    p.adv_start_cmpl.status = (int)status;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &p);
    callback_active = false;
}

static void test_advertising_diagnostics_first_fault_and_copy(void)
{
    reset();
    ble_hid_advertising_diagnostics_t trace;
    ble_hid_copy_advertising_diagnostics(NULL);
    ble_hid_copy_advertising_diagnostics(&trace);
    assert(!trace.count && !trace.frozen && !trace.total_events);
    fake_uptime_us = 1000000;
    service_link_control();
    assert(advertising_calls == 1 && state.adv_pending && advertising_heap_reads == 4);
    ble_hid_copy_advertising_diagnostics(&trace);
    assert(trace.count == 2 && trace.events[0].event == BLE_HID_ADV_START_REQUEST);
    const ble_hid_advertising_observation_t request = trace.events[0];
    assert(!request.status_known && request.memory.known && request.memory.sampled_uptime_ms == 1000);
    assert(request.memory.free_internal == 42000 && request.memory.largest_internal == 25000);
    assert(request.memory.free_dma == 9000 && request.memory.largest_dma == 4000);
    assert(request.flags & BLE_HID_ADV_START_PENDING);
    assert(trace.events[1].event == BLE_HID_ADV_START_RETURN && trace.events[1].status_known && !trace.events[1].status);
    fake_uptime_us = 1250000;
    trace_completion(0x100U + 0x07U); /* Pinned ESP status domain: HCI memory capacity. */
    const ble_hid_failure_t first = ble_hid_failure();
    assert(state.public.failed && !state.public.ready && first.sdk_status == 263 && first.uptime_ms == 1250);
    ble_hid_copy_advertising_diagnostics(&trace);
    assert(trace.frozen && !trace.overflowed && trace.count == 3);
    assert(trace.events[2].event == BLE_HID_ADV_START_COMPLETE && trace.events[2].status_known && trace.events[2].status == 263);
    assert(trace.events[2].uptime_ms == 1250 && !trace.events[2].memory.known);
    assert(trace.events[2].command_sequence == request.sequence);
    const ble_hid_advertising_diagnostics_t frozen = trace;
    const unsigned reads = uptime_reads, heaps = advertising_heap_reads, calls = sdk_calls;
    for (unsigned i = 0; i < 100; ++i) {
        ble_hid_copy_advertising_diagnostics(&trace);
        assert(!memcmp(&trace, &frozen, sizeof trace));
        trace.events[0].memory.free_internal = 1; /* Caller-owned copy. */
    }
    assert(reads == uptime_reads && heaps == advertising_heap_reads && calls == sdk_calls);
    fake_uptime_us = 5000000;
    trace_completion(5);
    service_link_control();
    ble_hid_copy_advertising_diagnostics(&trace);
    assert(!memcmp(&trace, &frozen, sizeof trace) && advertising_calls == 1);
    assert_same_failure(first); /* Later cleanup cannot replace the original fault. */
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
}

static void trace_failure_inside_api(void)
{
    fake_uptime_us = 1100000;
    trace_completion(263);
}

static void test_advertising_diagnostics_completion_before_api_return(void)
{
    reset();fake_uptime_us = 1000000;
    advertising_start_hook = trace_failure_inside_api;
    service_link_control();
    ble_hid_advertising_diagnostics_t trace;ble_hid_copy_advertising_diagnostics(&trace);
    assert(state.public.failed && advertising_calls == 1 && trace.frozen && trace.count == 2);
    assert(trace.events[0].event == BLE_HID_ADV_START_REQUEST && trace.events[0].memory.known);
    assert(trace.events[1].event == BLE_HID_ADV_START_COMPLETE && trace.events[1].status == 263);
    assert(trace.events[1].command_sequence == trace.events[0].sequence);
    assert(trace.events[1].uptime_ms == 1100 && ble_hid_failure().uptime_ms == 1100);
    assert(!trace.events[1].memory.known); /* No post-fault API return is invented. */
}

static void test_advertising_diagnostics_bounds_missing_and_benign_stop(void)
{
    reset();
    for (unsigned i = 0; i < 12; ++i) { fake_uptime_us = (int64_t)i * 1000;trace_completion(0); }
    ble_hid_advertising_diagnostics_t trace;ble_hid_copy_advertising_diagnostics(&trace);
    assert(trace.count == BLE_HID_ADVERTISING_TRACE_CAPACITY && trace.total_events == 12 && trace.overflowed && !trace.frozen);
    for (size_t i = 0; i < trace.count; ++i) {
        assert(trace.events[i].sequence == i + 5 && trace.events[i].uptime_ms == i + 4);
        assert(!trace.events[i].command_sequence && !trace.events[i].memory.known);
    }
    state.adv_stop_pending = true;state.adv_stopped_by_connect = true;
    esp_ble_gap_cb_param_t p = {0};p.adv_stop_cmpl.status = 5;
    callback_active = true;gap_callback(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, &p);callback_active = false;
    ble_hid_copy_advertising_diagnostics(&trace);
    assert(!state.public.failed && !trace.frozen && !state.adv_stop_pending);
    assert(trace.events[trace.count - 1].flags & BLE_HID_ADV_STOPPED_BY_CONNECT);
    reset();advertising_diagnostics.total_events = UINT64_MAX;
    service_link_control();ble_hid_copy_advertising_diagnostics(&trace);
    assert(trace.sequence_exhausted && trace.frozen && !trace.count && !state.public.failed && advertising_calls == 1);
    trace_completion(0);assert(state.advertising && !state.public.failed);
}

static void test_advertising_diagnostics_name_fault_classification(void)
{
    const ble_hid_advertising_event_t expected[] = {BLE_HID_ADV_DATA_COMPLETE,
        BLE_HID_ADV_SCAN_COMPLETE, BLE_HID_ADV_START_COMPLETE, BLE_HID_ADV_STOP_COMPLETE};
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset();
        if (mode == 3) {
            state.advertising = true;
            assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK);
            worker_cycle();assert(device_name.ack == NAME_ACK_STOP);
            advertising_stopped(263);
        } else {
            name_begin_runtime("Changed", false);
            worker_cycle();assert(device_name.ack == NAME_ACK_ADV);
            if (mode == 0) name_payload_complete(false, 263);
            else {
                name_payload_complete(false, ESP_BT_STATUS_SUCCESS);
                worker_cycle();assert(device_name.ack == NAME_ACK_SCAN);
                if (mode == 1) name_payload_complete(true, 263);
                else {
                    name_payload_complete(true, ESP_BT_STATUS_SUCCESS);
                    worker_cycle();assert(device_name.ack == NAME_ACK_START);
                    name_start_complete(263);
                }
            }
        }
        ble_hid_advertising_diagnostics_t trace;ble_hid_copy_advertising_diagnostics(&trace);
        assert(trace.frozen && trace.count && trace.events[trace.count - 1].event == expected[mode]);
        assert(trace.events[trace.count - 1].status_known && trace.events[trace.count - 1].status == 263);
        assert(ble_hid_failure().recorded && ble_hid_failure().sdk_status == 263);
        assert(ble_hid_name_snapshot().status != BLE_HID_NAME_APPLIED);
    }
}

static void test_advertising_diagnostics_name_benign_and_late_drain(void)
{
    reset();state.advertising = true;
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK);
    worker_cycle();assert(device_name.ack == NAME_ACK_STOP);
    connect_peer();advertising_stopped(5);
    ble_hid_advertising_diagnostics_t trace;ble_hid_copy_advertising_diagnostics(&trace);
    assert(!trace.frozen && !state.public.failed && device_name.phase == NAME_GET_PREVIOUS);
    assert(trace.events[trace.count - 1].flags & BLE_HID_ADV_STOPPED_BY_CONNECT);
    reset();name_begin_runtime("Changed", false);
    worker_cycle();name_payload_complete(false, ESP_BT_STATUS_SUCCESS);
    worker_cycle();name_payload_complete(true, ESP_BT_STATUS_SUCCESS);
    worker_cycle();assert(device_name.ack == NAME_ACK_START);
    fake_tick = BLE_HID_NAME_TIMEOUT_MS;notification_watchdog(NULL);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN);
    const ble_hid_failure_t first = ble_hid_failure();
    name_start_complete(263); /* Quarantined late completion is drained, not reclassified. */
    ble_hid_copy_advertising_diagnostics(&trace);
    assert(!trace.frozen && ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN);
    assert_same_failure(first);
}
