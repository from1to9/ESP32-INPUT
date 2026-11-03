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
    test_link.connected = false;
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
static void update_test_health_ready(void)
{
    update_test_quiet_media();
    test_update_snapshot.state = APP_UPDATE_PENDING_HEALTH;
    s_multitouch_available = true;
    test_update_imu = (board_imu_status_t){.initialization_succeeded = true,
                                           .start_succeeded = true,
                                           .worker_running = true,
                                           .sample_valid = true,
                                           .sample_fresh = true};
    test_update_presentation =
        (app_presenter_status_t){.initialized = true, .has_applied = true, .apply_age_ms = 10};
    test_indicator = (app_presenter_indicator_status_t){.bound = true, .owner_alive = true};
    test_boot_boundary_enabled = true;
    test_boot_display = (lv_display_t *)(uintptr_t)1;
    test_link.initialized = true;
    test_link.failed = false;
    test_link.connected = false;
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
    s_now_ms = 2000;
    remote_update_health_service();
    assert(s_update_health_since == 2000);
    test_storage.error = ESP_FAIL;
    s_now_ms = 47000;
    remote_update_health_service();
    assert(!s_update_health_since && !test_update_health_requests);
}
#define REMOTE_UPDATE_CASES(RUN)                                                                   \
    RUN(update_maintenance_waits_for_actual_neutral_and_cleanup);                                  \
    RUN(update_mailbox_rejects_competing_stale_and_lost_authority);                                \
    RUN(update_lock_and_transport_ack_are_independent_gates);                                      \
    RUN(update_health_requires_continuous_local_proof_and_no_host);                                \
    RUN(update_health_rejects_queued_render_codec_and_clock_regression)
