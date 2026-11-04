// SPDX-License-Identifier: Apache-2.0
/* Included by test_coordinator.c after its real-main harness helpers.
 * Public boundary controls required: test_update_imu (board_imu_status_t),
 * test_update_presentation (app_presenter_status_t), existing test_indicator,
 * media/settings/BLE/Wi-Fi/USB and virtual monotonic clock snapshots. */
static void update_test_quiet_media(void)
{
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true};
    test_media_snapshot.audio.hardware_off_confirmed = true;
    test_bonds.known = true;
    test_bonds.error = ESP_OK;
    test_update_target = (ble_hid_target_snapshot_t){
        .loaded = true, .available = true, .request_status = BLE_HID_TARGET_IDLE};
    test_wifi.initialized = true;
    test_storage.available = true;
    test_storage.pending = false;
    test_storage.error = ESP_OK;
}
static cp_command_t update_test_claim(uint64_t key)
{
    return remote_test_command(
        CP_OP_FIRMWARE_DOWNLOAD, key,
        "{\"release_id\":\"release-2\",\"digest\":"
        "\"1111111111111111111111111111111111111111111111111111111111111111\",\"policy_id\":"
        "\"22222222222222222222222222222222\",\"policy_revision\":\"3\"}");
}
static void update_test_allow(void)
{
    test_update_snapshot =
        (app_update_snapshot_t){.state = APP_UPDATE_AVAILABLE, .available = true};
    test_update_authority_error = CP_OK;
    test_update_authority = (app_update_authority_t){.authorized = true,
                                                     .policy_revision = 3,
                                                     .authority_expires_ms = s_now_ms + 60000,
                                                     .restart_authorized = true};
    strcpy(test_update_authority.expected_digest,
           "1111111111111111111111111111111111111111111111111111111111111111");
    update_test_quiet_media();
    remote_update_service();
}
static void update_maintenance_waits_for_actual_neutral_and_cleanup(void)
{
    setup(INPUT_MOUSE);
    update_test_allow();
    cp_command_t claim = update_test_claim(900);
    app_update_authority_t authority;
    test_media_snapshot.audio.hardware_off_confirmed = false;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK && authority.authorized &&
           authority.unlocked && !authority.maintenance_ready);
    assert(!remote_update_maintenance() && test_media_stops == 0);
    remote_update_service();
    assert(remote_update_maintenance() && test_wifi_maintenance && test_media_stops == 1 &&
           s_engine.state.release_pending);
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    assert(authority.observation.owner_rejected & APP_UPDATE_OWNER_MEDIA);
    for (unsigned i = 0; i < 12; ++i) {
        s_now_ms += 10;
        remote_update_service();
    }
    assert(test_media_stops == 1); /* A stopped request is not a repeated side effect. */
    release_barrier();
    test_media_snapshot.audio.hardware_off_confirmed = true;
    test_media_snapshot.audio.capture[0].state = AE_JOB_FINALIZING;
    test_media_snapshot.audio.capture[0].cleanup_pending = true;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    test_media_snapshot.audio.capture[0].state = AE_JOB_COMPLETED;
    test_media_snapshot.audio.capture[0].cleanup_pending = false;
    test_storage.pending = true;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    assert(authority.observation.owner_rejected & APP_UPDATE_OWNER_FLASH);
    test_storage.pending = false;
    test_bonds.known = false;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    test_bonds.known = true;
    test_update_target.request_status = BLE_HID_TARGET_PENDING;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    test_update_target.request_status = BLE_HID_TARGET_APPLYING;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    test_update_target.request_status = BLE_HID_TARGET_UNCERTAIN;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           !authority.maintenance_ready);
    test_update_target.request_status = BLE_HID_TARGET_APPLIED;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           authority.maintenance_ready);
    unsigned stops = test_media_stops;
    test_update_snapshot.state = APP_UPDATE_STAGED;
    remote_update_service();
    assert(!remote_update_maintenance() && !test_wifi_maintenance && test_media_stops == stops &&
           !s_update_gate.active);
}
static void update_mailbox_rejects_competing_stale_and_lost_authority(void)
{
    setup(INPUT_MOUSE);
    update_test_allow();
    cp_command_t claim = update_test_claim(901);
    app_update_authority_t authority;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK);
    cp_command_t changed = claim;
    changed.command_key++;
    assert(remote_update_authority(NULL, &changed, &authority) == CP_BUSY && !authority.authorized);
    assert(authority.observation.owner_rejected & APP_UPDATE_OWNER_CLAIM);
    changed = claim;
    changed.identity.session_id[0]++;
    assert(remote_update_authority(NULL, &changed, &authority) == CP_BUSY && !authority.authorized);
    remote_update_service();
    release_barrier();
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           authority.maintenance_ready);
    test_update_authority_error = CP_UNAUTHENTICATED;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_UNAUTHENTICATED);
    assert(authority.observation.runtime_status_known &&
           authority.observation.runtime_status == CP_UNAUTHENTICATED &&
           !authority.observation.owner_observed);
    remote_update_service();
    assert(!s_update_gate.maintenance_ready && remote_update_maintenance());
    test_update_snapshot.state = APP_UPDATE_CANCELLED;
    remote_update_service();
    assert(!remote_update_maintenance() && !test_wifi_maintenance);
    test_update_authority_error = CP_OK;
    s_now_ms += 2000;
    cp_command_t expired = update_test_claim(902);
    expired.deadline_ms = s_now_ms - 1;
    assert(remote_update_authority(NULL, &expired, &authority) == CP_EXPIRED &&
           !authority.authorized && !s_update_gate.active);
    cp_command_t fresh = update_test_claim(903);
    remote_update_service();
    assert(remote_update_authority(NULL, &fresh, &authority) == CP_OK);
    s_now_ms += 250;
    assert(remote_update_authority(NULL, &fresh, &authority) == CP_OK && authority.authorized &&
           !(authority.observation.owner_rejected & APP_UPDATE_OWNER_STALE));
    ++s_now_ms;
    assert(remote_update_authority(NULL, &fresh, &authority) == CP_OK && !authority.authorized &&
           !authority.maintenance_ready);
    assert(authority.observation.owner_observed &&
           authority.observation.observed_ms - authority.observation.owner_cached_ms == 251 &&
           (authority.observation.owner_rejected & APP_UPDATE_OWNER_STALE));
}
static void update_lock_and_transport_ack_are_independent_gates(void)
{
    setup(INPUT_MOUSE);
    update_test_allow();
    cp_command_t claim = update_test_claim(904);
    app_update_authority_t authority;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK);
    remote_update_service();
    release_barrier();
    s_engine.state.locked = true;
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK && !authority.unlocked &&
           !authority.maintenance_ready);
    assert(authority.observation.owner_rejected & APP_UPDATE_OWNER_LOCKED);
    s_engine.state.locked = false;
    test_link.connection_admitted = test_link.connected = false;
    test_link.ready = false;
    hid_engine_set_link(&s_engine, false, false);
    remote_update_service();
    assert(s_engine.state.release_pending);
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           authority
               .maintenance_ready); /* Actual disconnected transport needs no invented host ACK. */
    test_update_snapshot.state = APP_UPDATE_CANCELLED;
    remote_update_service();
    assert(!remote_update_maintenance());
}
static cp_command_t update_test_streaming_claim(uint64_t key)
{
    setup(INPUT_MOUSE);
    update_test_allow();
    cp_command_t claim = update_test_claim(key);
    app_update_authority_t authority;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK);
    test_update_snapshot.state = APP_UPDATE_DOWNLOADING;
    remote_update_service();
    release_barrier();
    remote_update_service();
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK &&
           authority.authorized && authority.unlocked && authority.maintenance_ready);
    test_clock_override = true;
    test_clock_ms = s_now_ms;
    return claim;
}
static void update_test_pause_reader(void) { test_clock_ms += 263; }
static void update_reader_cannot_accept_owner_expired_while_acquiring_lock(void)
{
    cp_command_t claim = update_test_streaming_claim(905);
    app_update_authority_t authority;
    test_update_gate_entry_hook = update_test_pause_reader;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK && !authority.authorized &&
           !authority.unlocked && !authority.maintenance_ready);
    assert(!test_update_gate_entry_hook && authority.observation.runtime_status == CP_OK &&
           authority.observation.observed_ms - authority.observation.owner_cached_ms == 263 &&
           authority.observation.owner_rejected == (APP_UPDATE_OWNER_STALE | APP_UPDATE_OWNER_NOT_READY));
}
static void update_test_publish_while_reader_waits(void)
{
    s_now_ms = ++test_clock_ms;
    remote_update_service();
}
static void update_reader_uses_new_publication_without_false_clock_regression(void)
{
    cp_command_t claim = update_test_streaming_claim(906);
    app_update_authority_t authority;
    test_update_gate_entry_hook = update_test_publish_while_reader_waits;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK && authority.authorized &&
           authority.unlocked && authority.maintenance_ready);
    assert(!test_update_gate_entry_hook && authority.observation.observed_ms == test_clock_ms &&
           authority.observation.owner_cached_ms == test_clock_ms && !authority.observation.owner_rejected);
}
static void update_real_owner_gap_and_clock_regression_stay_rejected(void)
{
    cp_command_t claim = update_test_streaming_claim(907);
    app_update_authority_t authority;
    /* The real device's 263 ms owner gap is a genuine missing-freshness
     * rejection, independent of where this reader is scheduled. */
    test_clock_ms += 263;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK && !authority.authorized &&
           !authority.unlocked && !authority.maintenance_ready);
    assert(authority.observation.owner_rejected == (APP_UPDATE_OWNER_STALE | APP_UPDATE_OWNER_NOT_READY));
    test_clock_ms = s_update_gate.cached_ms - 1;
    assert(remote_update_authority(NULL, &claim, &authority) == CP_OK && !authority.authorized &&
           (authority.observation.owner_rejected & APP_UPDATE_OWNER_STALE));
}
static void update_test_health_ready(void)
{
    update_test_quiet_media();
    test_update_snapshot.state = APP_UPDATE_PENDING_HEALTH;
    s_multitouch_available = true;
    test_update_imu = (board_imu_status_t){.initialization_attempted = true,
                                           .initialization_succeeded = true,
                                           .start_succeeded = true,
                                           .worker_running = true,
                                           .available = true,
                                           .phase = BOARD_IMU_ACTIVE,
                                           .sample_valid = true,
                                           .sample_fresh = true};
    test_update_presentation =
        (app_presenter_status_t){.initialized = true, .has_applied = true, .apply_age_ms = 10};
    test_indicator = (app_presenter_indicator_status_t){.bound = true, .owner_alive = true};
    test_boot_boundary_enabled = true;
    test_boot_display = (lv_display_t *)(uintptr_t)1;
    test_link.initialized = true;
    test_link.failed = false;
    test_link.connection_admitted = test_link.connected = false;
    test_link.ready = false;
    test_usb.initialized = true;
    test_usb.failed = false;
    test_usb.mounted = false;
}
static void update_health_requires_continuous_local_proof_and_no_host(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    s_now_ms = 1000;
    remote_update_health_service();
    assert(!test_update_health_requests);
    s_now_ms = 45999;
    remote_update_health_service();
    assert(!test_update_health_requests);
    test_update_imu.sample_fresh = false;
    s_now_ms = 46000;
    remote_update_health_service();
    assert(s_update_health_since == 0 && !test_update_health_requests);
    test_update_imu.sample_fresh = true;
    s_now_ms = 47000;
    remote_update_health_service();
    s_now_ms = 91999;
    remote_update_health_service();
    assert(!test_update_health_requests);
    s_now_ms = 92000;
    remote_update_health_service();
    assert(test_update_health_requests == 1 && !test_link.connected && !test_usb.mounted);
    test_update_snapshot.state = APP_UPDATE_CONFIRMED;
    remote_update_health_service();
    assert(s_update_health_since == 0);
}
static void update_health_rejects_queued_render_codec_and_clock_regression(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    s_now_ms = 1000;
    test_update_presentation.has_applied = false;
    remote_update_health_service();
    assert(!s_update_health_since);
    test_update_presentation.has_applied = true;
    test_indicator.owner_alive = false;
    remote_update_health_service();
    assert(!s_update_health_since);
    test_indicator.owner_alive = true;
    test_media_snapshot.audio.hardware_off_confirmed = false;
    remote_update_health_service();
    assert(!s_update_health_since);
    test_media_snapshot.audio.hardware_off_confirmed = true;
    remote_update_health_service();
    assert(s_update_health_since == 1000);
    s_now_ms = 999;
    remote_update_health_service();
    assert(!s_update_health_since && !test_update_health_requests);
    const size_t last = (s_update_health_trace.head + s_update_health_trace.count - 1U) %
                        UPDATE_HEALTH_TRACE_CAPACITY;
    assert(s_update_health_trace.events[last].kind == UPDATE_HEALTH_CLOCK &&
           s_update_health_trace.events[last].reset && !s_update_health_trace.events[last].duration_known);
    s_now_ms = 2000;
    remote_update_health_service();
    assert(s_update_health_since == 2000);
    test_storage.error = ESP_FAIL;
    s_now_ms = 47000;
    remote_update_health_service();
    assert(!s_update_health_since && !test_update_health_requests);
}
static void update_health_imu_recovery_caps_cannot_bypass(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    remote_update_health_service();
    assert(s_update_health_since == 1000);
    s_now_ms = 5000;
    test_capabilities.imu = false;
    test_update_imu.available = false;
    test_update_imu.phase = BOARD_IMU_RECOVERING;
    test_update_imu.sample_valid = test_update_imu.sample_fresh = false;
    test_update_imu.error = ESP_ERR_TIMEOUT;
    test_update_imu.fault_latched = true;
    test_update_imu.failure_streak = 1;
    remote_update_health_service();
    assert(!s_update_health_since && !test_update_health_requests);
    assert(s_update_health_trace.missing & (UINT64_C(1) << UPDATE_HEALTH_GATE_imu_fault));
    s_now_ms = 6000;
    test_capabilities.imu = test_update_imu.available = true;
    test_update_imu.phase = BOARD_IMU_ACTIVE;
    test_update_imu.sample_valid = test_update_imu.sample_fresh = true;
    test_update_imu.error = ESP_OK;
    test_update_imu.failure_streak = 0;
    test_update_imu.recovery_count = 1;
    remote_update_health_service();
    assert(!s_update_health_since); /* The recovery boundary discards the old streak. */
    s_now_ms = 6001;
    remote_update_health_service();
    assert(s_update_health_since == 6001 && !s_update_health_trace.missing);
    s_now_ms = 51000;
    remote_update_health_service();
    assert(!test_update_health_requests);
    s_now_ms = 51001;
    remote_update_health_service();
    assert(test_update_health_requests == 1 && test_update_imu.fault_latched);
}

static void update_health_imu_history_does_not_block_recovered_samples(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    test_update_imu.fault_latched = true;
    test_update_imu.first_fault_stage = "cache_age";
    test_update_imu.first_fault_error = ESP_ERR_TIMEOUT;
    test_update_imu.first_fault_us = 100;
    test_update_imu.recovery_count = 2;
    const board_imu_status_t recovered = test_update_imu;
    remote_update_health_service();
    s_now_ms = 1001;
    remote_update_health_service();
    assert(s_update_health_since == 1001 && !s_update_health_trace.missing);
    s_now_ms = 46000;
    remote_update_health_service();
    assert(!test_update_health_requests);
    s_now_ms = 46001;
    remote_update_health_service();
    assert(test_update_health_requests == 1);
    assert(memcmp(&recovered, &test_update_imu, sizeof recovered) == 0);
}

static void update_health_imu_unobserved_recovery_breaks_continuity(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    remote_update_health_service();
    s_now_ms = 30000;
    /* Recovery completes between coordinator polls: all current leaves are
     * good, but the old 29 seconds cannot cross this hardware boundary. */
    test_update_imu.recovery_count = 1;
    test_update_imu.fault_latched = true;
    remote_update_health_service();
    assert(!s_update_health_since && !test_update_health_requests);
    assert(s_update_health_trace.missing == (UINT64_C(1) << UPDATE_HEALTH_GATE_imu_fault));
    s_now_ms = 30001;
    remote_update_health_service();
    assert(s_update_health_since == 30001 && !s_update_health_trace.missing);
    s_now_ms = 59999;
    remote_update_health_service();
    assert(!test_update_health_requests);
    /* The updater still owns its 60-second deadline. Leaving probation does
     * not let this incomplete replacement streak make a confirmation call. */
    test_update_snapshot.state = APP_UPDATE_BOOT_SELECTION_UNCERTAIN;
    s_now_ms = 60000;
    remote_update_health_service();
    assert(!s_update_health_since && !test_update_health_requests);
}

static void update_health_imu_optional_absence_and_real_failures_stay_distinct(void)
{
    /* An absent optional device may be unprobed or have a failed init probe.
     * Neither has successfully established IMU capability on this boot. */
    for (unsigned attempted = 0; attempted < 2; ++attempted) {
        setup(INPUT_MOUSE);
        update_test_health_ready();
        test_capabilities.imu = false;
        test_update_imu = (board_imu_status_t){.initialization_attempted = attempted != 0,
            .phase = attempted ? BOARD_IMU_FATAL : BOARD_IMU_UNINITIALIZED,
            .fault_latched = attempted != 0, .error = ESP_ERR_NOT_FOUND};
        remote_update_health_service();
        assert(s_update_health_since == 1000 && !s_update_health_trace.missing);
        s_now_ms = 46000;
        remote_update_health_service();
        assert(test_update_health_requests == 1);
    }
    /* A formerly initialized device cannot become optional when start, worker
     * or sampling fails. Inactive phase rejects even stale success-shaped leaves. */
    const board_imu_phase_t failed[] = {BOARD_IMU_UNINITIALIZED, BOARD_IMU_RECOVERING, BOARD_IMU_FATAL};
    for (unsigned i = 0; i < sizeof failed / sizeof failed[0]; ++i) {
        setup(INPUT_MOUSE);
        update_test_health_ready();
        test_capabilities.imu = test_update_imu.available = false;
        test_update_imu.phase = failed[i];
        remote_update_health_service();
        assert(!s_update_health_since && !test_update_health_requests);
        assert(s_update_health_trace.missing == (UINT64_C(1) << UPDATE_HEALTH_GATE_imu_fault));
    }
}

static void update_health_trace_retains_subsecond_failures(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    test_diagnostic_logs_enabled = true;
    remote_update_health_service();
    assert(test_diagnostic_log_count == 2);
    s_now_ms = 1110;
    test_update_presentation.apply_age_ms = 501;
    remote_update_health_service();
    s_now_ms = 1120;
    test_update_presentation.apply_age_ms = 500;
    remote_update_health_service();
    s_now_ms = 1130;
    test_update_imu.sample_fresh = false;
    remote_update_health_service();
    s_now_ms = 1140;
    test_storage.pending = true;
    remote_update_health_service();
    s_now_ms = 1150;
    test_update_imu.sample_fresh = true;
    test_storage.pending = false;
    remote_update_health_service();
    assert(test_diagnostic_log_count == 2); /* No I/O for the five fast changes. */
    assert(s_update_health_trace.count == 5 && s_update_health_trace.resets == 2);
    const update_health_event_t *event = &s_update_health_trace.events[s_update_health_trace.head];
    assert(event->at_ms == 1110 && event->reset_ms == 110 && event->state_ms == 110);
    assert(event->missing == (UINT64_C(1) << UPDATE_HEALTH_GATE_ui_apply_age));
    assert(s_update_health_trace.events[2].at_ms == 1120 &&
           s_update_health_trace.events[2].state_ms == 10);
    assert(s_update_health_trace.events[3].reset_ms == 10);
    assert(!test_update_health_requests && s_update_health_since == 1150);
    for (s_now_ms = 2000; s_now_ms <= 4000; s_now_ms += 1000) remote_update_health_service();
    assert(!s_update_health_trace.count && !s_update_health_trace.dropped);
    assert(strstr(test_diagnostic_logs[3], "at_ms=1110 kind=change"));
    assert(strstr(test_diagnostic_logs[3], "reset_ms=110 request_error=0 gates=ui_apply_age"));
    assert(strstr(test_diagnostic_logs[4], "previous_state_ms=10 reset_ms=0"));
    assert(strstr(test_diagnostic_logs[6], "gates=imu_sample_fresh"));
    assert(strstr(test_diagnostic_logs[7], "gates=settings_pending|imu_sample_fresh"));
    assert(strstr(test_diagnostic_logs[9], "previous_state_ms=10 reset_ms=0"));
    s_now_ms = 46149;
    remote_update_health_service();
    assert(!test_update_health_requests);
    s_now_ms = 46150;
    remote_update_health_service();
    assert(test_update_health_requests == 1);
}

static void update_health_trace_has_bounded_output_and_explicit_loss(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    test_diagnostic_logs_enabled = true;
    remote_update_health_service();
    for (unsigned i = 1; i <= 200; ++i) {
        s_now_ms = 1000 + i;
        test_update_imu.sample_fresh = (i % 2U) == 0;
        remote_update_health_service();
    }
    assert(test_diagnostic_log_count == 2);
    assert(s_update_health_trace.count == UPDATE_HEALTH_TRACE_CAPACITY);
    assert(s_update_health_trace.resets == 100 && s_update_health_trace.dropped == 168);
    assert(s_update_health_trace.dropped_resets == 84 && s_update_health_trace.dropped_max_reset_ms == 1);
    assert(s_update_health_trace.dropped_missing == (UINT64_C(1) << UPDATE_HEALTH_GATE_imu_sample_fresh));
    s_now_ms = 2000;
    remote_update_health_service();
    assert(test_diagnostic_log_count == 5 && s_update_health_trace.count == 30);
    assert(strstr(test_diagnostic_logs[2], "dropped=168 dropped_resets=84"));
    assert(strstr(test_diagnostic_logs[2], "dropped_gates=imu_sample_fresh"));
    for (unsigned i = 0; i < 1000; ++i) remote_update_health_service();
    assert(test_diagnostic_log_count == 5 && !test_update_health_requests);
    s_now_ms = 1999;
    remote_update_health_service();
    assert(test_diagnostic_log_count == 5); /* Regression never bypasses the I/O limit. */
}

static void update_health_trace_names_each_observed_gate(void)
{
#define FAIL_GATE(name, mutation) do { \
    setup(INPUT_MOUSE); update_test_health_ready(); mutation; remote_update_health_service(); \
    assert(s_update_health_trace.missing == (UINT64_C(1) << UPDATE_HEALTH_GATE_##name)); \
    assert(!s_update_health_since && !test_update_health_requests); \
} while (0)
    FAIL_GATE(display, test_capabilities.display = false);
    FAIL_GATE(touch, test_capabilities.touch = false);
    FAIL_GATE(lock_button, test_capabilities.lock_button = false);
    FAIL_GATE(multitouch, s_multitouch_available = false);
    FAIL_GATE(ui_initialized, test_update_presentation.initialized = false);
    FAIL_GATE(ui_init_error, test_update_presentation.init_error = ESP_FAIL);
    FAIL_GATE(ui_applied, test_update_presentation.has_applied = false);
    FAIL_GATE(ui_apply_age, test_update_presentation.apply_age_ms = 501);
    FAIL_GATE(renderer_bound, test_indicator.bound = false);
    FAIL_GATE(renderer_alive, test_indicator.owner_alive = false);
    FAIL_GATE(renderer_error, test_indicator.error = ESP_FAIL);
    FAIL_GATE(display_handle, test_boot_display = NULL);
    FAIL_GATE(settings_available, test_storage.available = false);
    FAIL_GATE(settings_pending, test_storage.pending = true);
    FAIL_GATE(settings_error, test_storage.error = ESP_FAIL);
    FAIL_GATE(settings_valid, s_settings.brightness_percent = 0);
    FAIL_GATE(imu_initialized, test_update_imu.initialization_succeeded = false);
    FAIL_GATE(imu_started, test_update_imu.start_succeeded = false);
    FAIL_GATE(imu_worker, test_update_imu.worker_running = false);
    FAIL_GATE(imu_sample_valid, test_update_imu.sample_valid = false);
    FAIL_GATE(imu_sample_fresh, test_update_imu.sample_fresh = false);
    FAIL_GATE(imu_fault, test_update_imu.phase = BOARD_IMU_RECOVERING);
    FAIL_GATE(imu_error, test_update_imu.error = ESP_FAIL);
    FAIL_GATE(ble_initialized, test_link.initialized = false);
    FAIL_GATE(ble_failed, test_link.failed = true);
    FAIL_GATE(usb_initialized, test_usb.initialized = false);
    FAIL_GATE(usb_failed, test_usb.failed = true);
    FAIL_GATE(media_known, test_media_snapshot_unavailable = true);
    FAIL_GATE(media_initialized, test_media_snapshot.initialized = false;
              test_media_uninitialized_snapshot_available = true);
    FAIL_GATE(audio_known, test_media_snapshot.audio_known = false);
    FAIL_GATE(media_busy, test_media_snapshot.busy = true);
    FAIL_GATE(audio_off, test_media_snapshot.audio.hardware_off_confirmed = false);
    FAIL_GATE(capture_gate, test_media_snapshot.audio.capture_gate_open = true);
    FAIL_GATE(playback_gate, test_media_snapshot.audio.playback_gate_open = true);
    FAIL_GATE(microphone, test_media_snapshot.audio.microphone_enabled = true);
    FAIL_GATE(speaker, test_media_snapshot.audio.speaker_enabled = true);
    FAIL_GATE(capture_indicator, test_media_snapshot.audio.capture_indicator_required = true);
    FAIL_GATE(audio_quarantined, test_media_snapshot.audio.quarantined = true);
    FAIL_GATE(capture0_cleanup, test_media_snapshot.audio.capture[0].cleanup_pending = true);
    FAIL_GATE(capture1_cleanup, test_media_snapshot.audio.capture[1].cleanup_pending = true);
    FAIL_GATE(capture2_cleanup, test_media_snapshot.audio.capture[2].cleanup_pending = true);
    FAIL_GATE(playback_cleanup, test_media_snapshot.audio.playback.cleanup_pending = true);
    for (ae_job_state_t state = AE_JOB_STARTING; state <= AE_JOB_FINALIZING; ++state) {
        FAIL_GATE(capture0_idle, test_media_snapshot.audio.capture[0].state = state);
        FAIL_GATE(capture1_idle, test_media_snapshot.audio.capture[1].state = state);
        FAIL_GATE(capture2_idle, test_media_snapshot.audio.capture[2].state = state);
        FAIL_GATE(playback_idle, test_media_snapshot.audio.playback.state = state);
    }
#undef FAIL_GATE
    setup(INPUT_MOUSE);
    update_test_health_ready();
    test_capabilities.imu = false;
    test_update_imu = (board_imu_status_t){.fault_latched = true, .error = ESP_FAIL};
    remote_update_health_service();
    assert(!s_update_health_trace.missing && s_update_health_since == s_now_ms);
    test_media_snapshot.audio_known = false;
    test_media_snapshot.audio.quarantined = true;
    remote_update_health_service();
    assert(s_update_health_trace.missing == (UINT64_C(1) << UPDATE_HEALTH_GATE_audio_known));
}

static void update_health_trace_observes_confirmation_without_changing_retries(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    test_diagnostic_logs_enabled = true;
    remote_update_health_service();
    test_update_health_error = ESP_FAIL;
    s_now_ms = 46000;
    remote_update_health_service();
    assert(test_update_health_requests == 1 && s_update_health_trace.request_error == ESP_FAIL);
    assert(strstr(test_diagnostic_logs[3], "kind=request"));
    s_now_ms = 47000;
    remote_update_health_service();
    assert(test_update_health_requests == 2 && s_update_health_trace.count == 0);
    test_update_health_error = ESP_OK;
    s_now_ms = 48000;
    remote_update_health_service();
    assert(test_update_health_requests == 3 && s_update_health_trace.request_error == ESP_OK);
    test_update_snapshot.state = APP_UPDATE_CONFIRMED;
    s_now_ms = 48001;
    remote_update_health_service();
    assert(!s_update_health_since && !s_update_health_trace.active);
    s_now_ms = 49000;
    remote_update_health_service();
    assert(strstr(test_diagnostic_logs[test_diagnostic_log_count - 1U], "kind=end"));
}

/* Match the LVGL control path: press, release, then CLICKED with the
 * context captured at press. All three traverse the real coordinator FIFO. */
static void update_control_tap(uint32_t gesture, app_action_t kind, int value)
{
    app_intent_t intent = make_intent(kind, value, 0, NULL);
    intent.gesture = gesture;
    intent.surface = APP_SURFACE_CONTROL;
    local_control_contact(gesture);
    deliver(intent);
}

static void first_boot_local_navigation(void)
{
    setup(INPUT_MOUSE);
    update_test_health_ready();
    remote_update_service();
    assert(remote_update_maintenance() && test_wifi_maintenance);
    render_view(&test_link, ESP_OK);
    assert(test_view.local_access == APP_LOCAL_BROWSE);
    update_control_tap(101, APP_MENU, APP_OVERLAY_MODES);
    assert(s_overlay == APP_OVERLAY_MODES);
    update_control_tap(102, APP_MENU, APP_OVERLAY_TARGET);
    assert(s_overlay == APP_OVERLAY_TARGET);
    update_control_tap(103, APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_DISPLAY);
    update_control_tap(104, APP_THEME, 0);
    assert(s_dark_theme);
    update_control_tap(120, APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
    update_control_tap(121, APP_BLUETOOTH_DEVICES, 0);
    assert(s_overlay == APP_OVERLAY_BLUETOOTH_DEVICES && !s_bluetooth_audio_devices);
    update_control_tap(122, APP_PAIRING_REQUEST, APP_PAIR_OPEN);
    assert(s_overlay == APP_OVERLAY_BLUETOOTH_DEVICES && s_pair_operation == APP_CONFIRM_NONE);
    update_control_tap(123, APP_BLUETOOTH_DEVICES, 1);
    assert(s_overlay == APP_OVERLAY_BLUETOOTH_DEVICES && s_bluetooth_audio_devices);
    update_control_tap(105, APP_CLOSE, 0);
    assert(s_overlay == APP_OVERLAY_NONE);
    assert(!test_update_health_requests);
    update_control_tap(114, APP_MODE, INPUT_KEYBOARD);
    assert(s_engine.state.mode == INPUT_KEYBOARD);
    update_control_tap(115, APP_KEY, 4);
    assert(!s_engine.count && !set_remote_allowance(true));

    /* A real update claim, busy flash, or restart independently wins. */
    test_update_busy = true; remote_update_service();
    update_control_tap(106, APP_MENU, APP_OVERLAY_MODES);
    assert(s_overlay == APP_OVERLAY_NONE);
    update_control_tap(124, APP_BLUETOOTH_DEVICES, 1);
    assert(s_overlay == APP_OVERLAY_NONE);
    test_update_busy = false; s_update_gate.active = true; remote_update_service();
    update_control_tap(107, APP_MENU, APP_OVERLAY_MODES);
    assert(s_overlay == APP_OVERLAY_NONE);
    s_update_gate.active = false; s_remote_device_maintenance_active = true; remote_update_service();
    render_view(&test_link, ESP_OK);
    assert(test_view.local_access == APP_LOCAL_PAUSED);
    update_control_tap(108, APP_MENU, APP_OVERLAY_MODES);
    assert(s_overlay == APP_OVERLAY_NONE);
    s_remote_device_maintenance_active = false; remote_update_service();

    update_control_tap(109, APP_MENU, APP_OVERLAY_SETTINGS);
    update_control_tap(110, APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
    app_intent_t held = make_intent(APP_THEME, 0, 0, NULL);
    held.gesture = 111; held.surface = APP_SURFACE_CONTROL;
    local_control_contact(111);
    test_update_snapshot.state = APP_UPDATE_CONFIRMED; remote_update_service();
    deliver(held);
    assert(s_dark_theme && !remote_update_maintenance());
    update_control_tap(112, APP_THEME, 0);
    assert(!s_dark_theme);

    setup(INPUT_MOUSE);
    test_update_snapshot.state = APP_UPDATE_IDLE; remote_update_service();
    update_control_tap(113, APP_MENU, APP_OVERLAY_MODES);
    assert(s_overlay == APP_OVERLAY_MODES && !remote_update_maintenance() && !test_wifi_maintenance);
    test_update_busy = true; remote_update_service();
    update_control_tap(116, APP_MENU, APP_OVERLAY_SETTINGS);
    assert(s_overlay == APP_OVERLAY_MODES);
    test_update_busy = false; test_update_snapshot.state = APP_UPDATE_PENDING_HEALTH;
    remote_update_service();
    update_control_tap(117, APP_LOCK, 0);
    assert(process_priority() && s_engine.state.locked);
    update_control_tap(118, APP_MENU, APP_OVERLAY_MODES);
    assert(s_engine.state.locked && s_overlay == APP_OVERLAY_NONE);
}

static void first_boot_button_queued_before_confirmation(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    test_boot_boundary_enabled = true;
    test_update_snapshot.state = APP_UPDATE_PENDING_HEALTH;
    remote_update_service(); release_barrier(); sync_interaction();
    uint64_t down = s_now_ms + 10;
    button_edge(true, down); button_edge(false, down + 20); buttons_at(down + 300);
    assert(!s_engine.state.gyro_enabled);
    down += 400;
    physical_button(BOARD_BUTTON_DOWN, down * 1000);
    s_now_ms = down + 1; /* Confirmation occurs after the queued hardware edge. */
    test_update_snapshot.state = APP_UPDATE_CONFIRMED;
    remote_update_service();
    buttons_at(down + 1);
    button_edge(false, down + 20);
    buttons_at(down + 300);
    assert(!s_engine.state.gyro_enabled);
    button_edge(true, down + 310);
    button_edge(false, down + 330);
    buttons_at(down + 610);
    assert(s_engine.state.gyro_enabled);
}

/* A pending first click cannot pair with a fresh click after maintenance.
 * The second DOWN is in the same millisecond, just after the policy change. */
static void button_double_does_not_span_maintenance(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    remote_device_set_maintenance(true);
    uint64_t down = s_now_ms + 10;
    button_edge(true, down); button_edge(false, down + 20);
    s_now_ms = down + 100;
    remote_device_set_maintenance(false);
    physical_button(BOARD_BUTTON_DOWN, s_now_ms * 1000U + 1);
    buttons_at(s_now_ms);
    button_edge(false, down + 120);
    assert(s_overlay == APP_OVERLAY_NONE && !s_engine.state.gyro_enabled);
    buttons_at(down + 400);
    assert(s_engine.state.gyro_enabled);
}

static void first_boot_button_menu_policy(void)
{
    const struct {
        app_update_state_t update;
        bool flash_busy, restart, menu_opens;
    } cases[] = {
        {.update = APP_UPDATE_IDLE, .flash_busy = true},
        {.update = APP_UPDATE_IDLE, .restart = true},
        {.update = APP_UPDATE_PENDING_HEALTH, .menu_opens = true},
        {.update = APP_UPDATE_IDLE, .menu_opens = true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        setup(INPUT_MOUSE);
        test_boot_boundary_enabled = true;
        test_update_busy = cases[i].flash_busy;
        remote_device_set_maintenance(cases[i].restart);
        test_update_snapshot.state = cases[i].update;
        remote_update_service();
        if (s_engine.state.release_pending) release_barrier();
        sync_interaction();
        uint64_t down = s_now_ms + 10;
        button_edge(true, down); button_edge(false, down + 20);
        button_edge(true, down + 200); button_edge(false, down + 220);
        assert(s_overlay == (cases[i].menu_opens ? APP_OVERLAY_MODES : APP_OVERLAY_NONE));
        button_edge(true, down + 500); buttons_at(down + 1400);
        assert(s_engine.state.locked && s_engine.state.release_pending);
        release_barrier();
        button_edge(false, down + 1450);
    }
}

#define REMOTE_UPDATE_CASES(RUN)                                                                   \
    RUN(first_boot_button_queued_before_confirmation);                                              \
    RUN(button_double_does_not_span_maintenance);                                                  \
    RUN(first_boot_button_menu_policy);                                                           \
    RUN(first_boot_local_navigation);                                                              \
    RUN(update_maintenance_waits_for_actual_neutral_and_cleanup);                                  \
    RUN(update_mailbox_rejects_competing_stale_and_lost_authority);                                \
    RUN(update_lock_and_transport_ack_are_independent_gates);                                      \
    RUN(update_reader_cannot_accept_owner_expired_while_acquiring_lock);                           \
    RUN(update_reader_uses_new_publication_without_false_clock_regression);                        \
    RUN(update_real_owner_gap_and_clock_regression_stay_rejected);                                 \
    RUN(update_health_requires_continuous_local_proof_and_no_host);                                \
    RUN(update_health_rejects_queued_render_codec_and_clock_regression);                            \
    RUN(update_health_imu_recovery_caps_cannot_bypass);                                            \
    RUN(update_health_imu_history_does_not_block_recovered_samples);                               \
    RUN(update_health_imu_unobserved_recovery_breaks_continuity);                                  \
    RUN(update_health_imu_optional_absence_and_real_failures_stay_distinct);                        \
    RUN(update_health_trace_retains_subsecond_failures);                                           \
    RUN(update_health_trace_has_bounded_output_and_explicit_loss);                                 \
    RUN(update_health_trace_names_each_observed_gate);                                             \
    RUN(update_health_trace_observes_confirmation_without_changing_retries)
