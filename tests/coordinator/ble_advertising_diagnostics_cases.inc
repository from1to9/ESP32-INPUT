// SPDX-License-Identifier: Apache-2.0
static void advertising_failure_replay_preserves_domains_and_input(void)
{
    setup(INPUT_MOUSE);
    test_diagnostic_logs_enabled = true;
    report_bluetooth_advertising_diagnostics();
    assert(!test_diagnostic_log_count); /* Missing trace is not a fabricated event. */
    test_advertising_trace.frozen = true;
    test_advertising_trace.overflowed = true;
    test_advertising_trace.total_events = 12;
    test_advertising_trace.count = BLE_HID_ADVERTISING_TRACE_CAPACITY;
    for (size_t i = 0; i < test_advertising_trace.count; ++i)
        test_advertising_trace.events[i] = (ble_hid_advertising_observation_t){
            .sequence = i + 5, .event = BLE_HID_ADV_START_COMPLETE, .uptime_ms = 660845,
            .command_sequence = 4, .generation = 7, .status_known = true, .status = 263};
    test_advertising_trace.events[0].event = BLE_HID_ADV_START_RETURN;
    test_advertising_trace.events[0].status = ESP_OK;
    test_advertising_trace.events[1].event = BLE_HID_ADV_START_REQUEST;
    test_advertising_trace.events[1].status_known = false;
    test_advertising_trace.events[1].memory = (ble_hid_advertising_memory_t){
        .known = true, .sampled_uptime_ms = 660800, .free_internal = 42000, .largest_internal = 25000};
    const hid_engine_t engine = s_engine;
    const size_t reports = trace_count, heap_reads = test_heap_observation_count;
    report_bluetooth_advertising_diagnostics();
    assert(test_diagnostic_log_count == BLE_HID_ADVERTISING_TRACE_CAPACITY + 1);
    assert(strstr(test_diagnostic_logs[0], "overflowed=1"));
    assert(strstr(test_diagnostic_logs[1], "status_domain=esp_err status=0"));
    assert(strstr(test_diagnostic_logs[2], "status_known=0 status_domain=none"));
    assert(strstr(test_diagnostic_logs[2], "memory_known=1 memory_uptime_ms=660800"));
    assert(strstr(test_diagnostic_logs[3], "status_domain=bt_status status=263 memory_known=0"));
    assert(!memcmp(&engine, &s_engine, sizeof engine) && reports == trace_count && heap_reads == test_heap_observation_count);
}
