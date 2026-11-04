// SPDX-License-Identifier: Apache-2.0
/* Reuse the coordinator's real input queue, HID engine, overlays and presenter.
 * Link the production media controls, substituting only their runtime/BT ports. */
#define TEST_COORDINATOR_MEDIA_REVIEW
#define main coordinator_suite_main
#include "test_coordinator.c"
#undef main
#include "../../firmware/main/board_recording_flash.h"

static bt_audio_snapshot_t classic;
static unsigned opens, closes, refreshes;
board_recording_flash_status_t board_recording_flash_status(void) { return (board_recording_flash_status_t){0}; }
bt_audio_snapshot_t bt_audio_snapshot(void) { return classic; }
esp_err_t bt_audio_refresh_bonds(void) { ++refreshes; test_classic_bonds.refreshing = true; return ESP_OK; }
bool app_media_runtime_pairing_current(uint64_t generation, uint64_t revision)
{
    return test_media_snapshot.audio.audio_generation == generation && test_media_snapshot.audio.revision == revision &&
        !test_media_snapshot.busy && test_media_snapshot.audio.hardware_off_confirmed;
}
esp_err_t bt_audio_pairing_open_reviewed(uint32_t duration, const bt_audio_snapshot_t *expected,
                                        uint64_t bonds, bool (*resources_current)(void))
{
    assert(duration == BT_AUDIO_PAIRING_DEFAULT_MS && bonds == test_classic_bonds.revision);
    if (expected->a2dp_link_generation != classic.a2dp_link_generation || !resources_current()) return ESP_ERR_INVALID_VERSION;
    ble_hid_pairing_close();
    ++opens; classic.pairing_requested = classic.pairing_busy = true;
    return ESP_OK;
}
void bt_audio_pairing_close(void) { ++closes; classic.pairing_requested = classic.pairing_busy = false; }
void bt_audio_cancel_request(uint64_t token) { (void)token; abort(); }
esp_err_t bt_audio_saved_request(uint64_t generation, uint64_t link_generation, uint64_t revision,
    bt_audio_command_t command, const uint8_t peer[6], uint64_t *id)
{ (void)generation; (void)link_generation; (void)revision; (void)command; (void)peer; (void)id; abort(); }
ae_result_t app_media_runtime_submit_local(const app_media_local_request_t *request)
{ (void)request; abort(); }
bool app_media_runtime_take_result(uint64_t *token, ae_result_t *result)
{ (void)token; (void)result; return false; }

static void present(void)
{
    service_media();
    render_view(&test_link, ESP_OK);
}
static app_intent_t pair_review(app_media_operation_t operation)
{
    app_intent_t request = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    request.media = (app_media_request_t){.operation = operation,
        .management_generation = test_view.media.management_generation, .audio_generation = test_view.media.audio_generation,
        .resource_revision = test_view.media.resource_revision, .peer_id = test_view.media.peer_id};
    deliver(request); present();
    assert(s_overlay == APP_OVERLAY_CONFIRM && test_view.confirmation == APP_CONFIRM_AUDIO_PAIR);
    assert(s_engine.state.epoch != request.epoch && test_view.media_review.valid &&
        test_view.media_review.request.operation == operation);
    app_intent_t accept = make_intent(APP_CONFIRM, 1, 0, NULL);
    accept.media = test_view.media_review.request;
    return accept;
}
int main(void)
{
    setup(INPUT_AUDIO);
    test_brightness = (board_brightness_status_t){.applied_known = true, .applied_percent = 50, .error = ESP_OK};
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true,
        .audio = {.state = AE_OFF, .route = AE_ROUTE_HFP_HF, .audio_generation = 3,
            .revision = 5, .hardware_off_confirmed = true}};
    classic = (bt_audio_snapshot_t){.supported = true, .initialized = true, .classic_bonds_known = true};
    assert(app_media_controls_init());
    action(APP_MENU, APP_OVERLAY_MEDIA_OPTIONS); present();
    assert(test_view.overlay == APP_OVERLAY_MEDIA_OPTIONS && test_view.media.pairing.can_open);
    app_intent_t old = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    assert(!opens);
    ++classic.a2dp_link_generation;
    classic.a2dp_connected = classic.a2dp_peer_known = true;
    classic.a2dp_peer[0] = 3;
    deliver(old); /* A2DP callback after the last media poll. */
    assert(!opens && !test_pair_closes);
    present();
    old = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    ++test_media_snapshot.audio.revision;
    deliver(old); /* Resource owner advanced before the coordinator polled. */
    assert(!opens && !test_pair_closes);
    present();
    old = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    app_intent_t cancel = old; cancel.x = 0;
    deliver(cancel); present();
    assert(test_view.overlay == APP_OVERLAY_MEDIA_OPTIONS && !opens && !test_pair_closes);
    deliver(old); present(); assert(!opens);
    app_intent_t accept = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    deliver(old); present(); assert(!opens && test_view.media_review.valid);
    deliver(accept); present();
    assert(opens == 1 && test_view.overlay == APP_OVERLAY_MEDIA_OPTIONS && test_view.media.pairing.can_close);
    deliver(accept); present(); assert(opens == 1);
    app_intent_t close = pair_review(APP_MEDIA_PAIR_AUDIO_CLOSE);
    deliver(close); present(); assert(closes == 1 && test_view.media.pairing.can_open);
    accept = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    input_link_t host = input_link_snapshot(&test_link), replacement = host;
    ++replacement.generation;
    observe_input_link(&replacement, &host); present();
    assert(!test_view.media_review.valid);
    accept.epoch = s_engine.state.epoch; accept.ui_revision = s_ui_revision;
    deliver(accept); present();
    assert(opens == 1 && test_pair_closes == 1 && s_pair_operation == APP_CONFIRM_NONE);

    accept = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    ++test_media_snapshot.audio.revision; present(); assert(!test_view.media_review.valid);
    deliver(accept); present(); assert(opens == 1 && test_pair_closes == 1);

    accept = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    s_priority_requests = RELEASE_REQUEST; assert(process_priority()); present();
    assert(test_view.overlay == APP_OVERLAY_NONE && !test_view.media_review.valid);
    deliver(accept); present(); assert(opens == 1 && test_pair_closes == 1);

    action(APP_MENU, APP_OVERLAY_MEDIA_OPTIONS); present();
    accept = pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
    s_priority_requests = LOCK_REQUEST; assert(process_priority()); present();
    assert(test_view.locked && !test_view.media_review.valid);
    deliver(accept); present(); assert(opens == 1);
    s_engine.state.locked = false;
    for (int mode=INPUT_MOUSE; mode<=INPUT_AUDIO; ++mode) {
        (void)select_mode((input_mode_t)mode);
        action(APP_BLUETOOTH_DEVICES,1); present();
        assert(test_view.overlay==APP_OVERLAY_BLUETOOTH_DEVICES && test_view.media.pairing.can_open);
        unsigned before=opens;
        app_intent_t approve=pair_review(APP_MEDIA_PAIR_AUDIO_OPEN);
        assert(test_view.media_review.valid && test_view.confirmation==APP_CONFIRM_AUDIO_PAIR);
        app_intent_t cancel_shared=make_intent(APP_CONFIRM,0,0,NULL); deliver(cancel_shared); present();
        assert(opens==before && test_view.overlay==APP_OVERLAY_BLUETOOTH_DEVICES);
        approve=pair_review(APP_MEDIA_PAIR_AUDIO_OPEN); deliver(approve); present();
        assert(opens==before+1 && test_view.overlay==APP_OVERLAY_BLUETOOTH_DEVICES);
        action(APP_CLOSE,0); present();
        assert(test_view.overlay==APP_OVERLAY_NONE && !classic.pairing_requested && test_view.mode==(input_mode_t)mode);
    }
    test_classic_bonds = (bt_audio_bond_list_t){.known = true, .revision = 29, .count = 5};
    for (unsigned i = 0; i < test_classic_bonds.count; ++i) test_classic_bonds.bonds[i].address[5] = (uint8_t)(i + 1);
    update_test_health_ready(); remote_update_service();
    update_control_tap(801, APP_BLUETOOTH_DEVICES, 1); present();
    assert(test_view.local_access == APP_LOCAL_BROWSE && test_view.media.devices.pages == 2);
    app_intent_t navigation = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    navigation.media.operation = APP_MEDIA_DEVICE_PAGE;
    navigation.media.object_revision = test_view.media.devices.revision; navigation.media.value.page_delta = 1;
    navigation.gesture = 802; navigation.surface = APP_SURFACE_CONTROL; local_control_contact(802);
    deliver(navigation); present(); assert(test_view.media.devices.page == 1);
    navigation = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    navigation.media.operation = APP_MEDIA_DEVICE_DETAILS;
    navigation.media.object_revision = test_view.media.devices.revision;
    navigation.media.value.peer_id = test_view.media.devices.rows[0].id;
    navigation.gesture = 803; navigation.surface = APP_SURFACE_CONTROL; local_control_contact(803);
    deliver(navigation); present(); assert(test_view.overlay == APP_OVERLAY_AUDIO_DEVICE);
    assert(test_view.media.devices.selected.bytes[15] == 5);
    navigation = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    navigation.media.operation = APP_MEDIA_DEVICE_LINK; navigation.media.object_id = test_view.media.devices.selected;
    navigation.media.object_revision = test_view.media.devices.revision;
    navigation.gesture = 804; navigation.surface = APP_SURFACE_CONTROL; local_control_contact(804);
    deliver(navigation); present(); /* The SDK seam above aborts if a mutation reaches it. */
    update_control_tap(805, APP_BLUETOOTH_DEVICES, 1); present();
    assert(test_view.overlay == APP_OVERLAY_BLUETOOTH_DEVICES);
    navigation = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    navigation.media.operation = APP_MEDIA_DEVICE_REFRESH;
    navigation.gesture = 807; navigation.surface = APP_SURFACE_CONTROL; local_control_contact(807);
    unsigned before_refresh = refreshes;
    deliver(navigation); present();
    assert(refreshes == before_refresh + 1 && test_view.media.devices.refreshing && test_view.media.devices.count == 5);
    test_classic_bonds.refreshing = false; present();
    test_update_busy = true; remote_update_service();
    navigation = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    navigation.media.operation = APP_MEDIA_DEVICE_REFRESH;
    navigation.gesture = 808; navigation.surface = APP_SURFACE_CONTROL; local_control_contact(808);
    deliver(navigation); present(); assert(refreshes == before_refresh + 1);
    navigation = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    navigation.media.operation = APP_MEDIA_DEVICE_DETAILS;
    navigation.media.object_revision = test_view.media.devices.revision;
    navigation.media.value.peer_id = test_view.media.devices.rows[0].id;
    navigation.gesture = 806; navigation.surface = APP_SURFACE_CONTROL; local_control_contact(806);
    deliver(navigation); present(); assert(test_view.overlay == APP_OVERLAY_BLUETOOTH_DEVICES);
    puts("PASS: coordinator reviews and read-only first-boot device navigation; mutations and paused navigation blocked");
    return 0;
}
