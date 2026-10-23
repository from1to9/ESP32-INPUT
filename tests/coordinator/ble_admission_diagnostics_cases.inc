// SPDX-License-Identifier: Apache-2.0
static void admission_failure_replay_is_bounded_repeatable_and_observation_only(void)
{
    setup(INPUT_MOUSE);
    test_diagnostic_logs_enabled = true;
    report_bluetooth_admission_diagnostics();
    assert(test_diagnostic_log_count == 0);
    test_admission_trace.frozen = test_admission_trace.overflowed = true;
    test_admission_trace.open_known = test_admission_trace.connect_known = true;
    test_admission_trace.admission_known = test_admission_trace.first_failure_known = true;
    test_admission_trace.count = BLE_HID_ADMISSION_TRACE_CAPACITY;
    test_admission_trace.total_events = 99;
    test_admission_trace.first_failure = (ble_hid_admission_observation_t){
        .sequence = 1, .event = BLE_HID_ADMISSION_AUTH, .status_known = true,
        .status_domain = BLE_HID_ADMISSION_STATUS_AUTH_REASON, .status = 86};
    test_admission_trace.last_open.sequence = 2;
    test_admission_trace.connect.sequence = 3;
    test_admission_trace.first_admission.sequence = 4;
    for (size_t i = 0; i < BLE_HID_ADMISSION_TRACE_CAPACITY; ++i)
        test_admission_trace.events[i].sequence = i + 5U;
    test_admission_trace.events[0].status_known = true;
    test_admission_trace.events[0].status_domain = BLE_HID_ADMISSION_STATUS_ESP_ERROR;
    test_admission_trace.events[0].status = ESP_OK;
    const ble_hid_admission_diagnostics_t original = test_admission_trace;
    const hid_engine_t engine = s_engine;
    const device_settings_t settings = s_settings;
    const size_t reports = trace_count, heap_reads = test_heap_observation_count;
    unsigned seen[13] = {0};
    for (unsigned round = 0; round < 12; ++round) {
        test_diagnostic_log_count = 0;
        report_bluetooth_admission_diagnostics();
        assert(test_diagnostic_log_count == 7); /* Header plus two three-part records. */
        assert(strstr(test_diagnostic_logs[0], "count_valid=1"));
        for (size_t i = 1; i < test_diagnostic_log_count; i += 3) {
            unsigned long long sequence = 0;
            const char *field = strstr(test_diagnostic_logs[i], "sequence=");
            assert(field && sscanf(field, "sequence=%llu", &sequence) == 1);
            assert(sequence >= 1 && sequence <= 12);
            ++seen[sequence];
            assert(strstr(test_diagnostic_logs[i], "tick_known=0"));
            assert(strstr(test_diagnostic_logs[i + 2], "auth_reason_origin=unknown"));
            if (sequence == 1)
                assert(strstr(test_diagnostic_logs[i + 2], "status_domain=auth_reason status=86"));
            else if (sequence == 5)
                assert(strstr(test_diagnostic_logs[i + 2], "status_domain=esp_err status=0"));
            else assert(strstr(test_diagnostic_logs[i + 2], "status_known=0 status_domain=none"));
        }
    }
    for (unsigned i = 1; i <= 12; ++i) assert(seen[i] == 2); /* Late readers see another full cycle. */
    assert(!memcmp(&original, &test_admission_trace, sizeof original));
    assert(!memcmp(&engine, &s_engine, sizeof engine));
    assert(!memcmp(&settings, &s_settings, sizeof settings));
    assert(reports == trace_count && heap_reads == test_heap_observation_count);

    /* An invalid producer count remains visible without indexing beyond the ring. */
    test_admission_trace.count = BLE_HID_ADMISSION_TRACE_CAPACITY + 1U;
    test_diagnostic_log_count = 0;
    report_bluetooth_admission_diagnostics();
    assert(test_diagnostic_log_count == 7);
    assert(strstr(test_diagnostic_logs[0], "count_valid=0"));
    for (size_t i = 0; i < test_diagnostic_log_count; ++i)
        assert(!strstr(test_diagnostic_logs[i], "origin=tail"));
    memset(&test_admission_trace, 0, sizeof test_admission_trace);
    test_admission_trace.frozen = true;
    test_diagnostic_log_count = 0;
    report_bluetooth_admission_diagnostics();
    assert(test_diagnostic_log_count == 1); /* Unknown anchors do not fabricate zero-valued events. */
}
