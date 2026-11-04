// SPDX-License-Identifier: Apache-2.0
/* Compose the production coordinator, media controls and Classic dispatcher. */
#define TEST_COORDINATOR_MEDIA_REVIEW
#define TEST_COORDINATOR_REAL_CLASSIC
#define main coordinator_fixture_main
#include "test_coordinator.c"
#undef main
#include "../../firmware/main/board_recording_flash.h"

void device_radio_init(void);
void device_radio_dispatch(void);
unsigned device_radio_calls(void);
void device_radio_hfp(bool foreign);
void device_radio_before_lock(void (*callback)(void));

board_recording_flash_status_t board_recording_flash_status(void) { return (board_recording_flash_status_t){0}; }
bool app_media_runtime_pairing_current(uint64_t generation, uint64_t revision)
{
    return test_media_snapshot.audio.audio_generation == generation && test_media_snapshot.audio.revision == revision &&
        !test_media_snapshot.busy && test_media_snapshot.audio.hardware_off_confirmed;
}
ae_result_t app_media_runtime_submit_local(const app_media_local_request_t *request)
{ (void)request; abort(); }
bool app_media_runtime_take_result(uint64_t *token, ae_result_t *result)
{ (void)token; (void)result; return false; }

static void present(void) { service_media(); render_view(&test_link, ESP_OK); }
static const char *scenario;
static void revoke(void)
{
    if (strstr(scenario, "leave")) {
        action(APP_CLOSE, 0); assert(s_overlay == APP_OVERLAY_NONE);
    } else if (!strcmp(scenario, "back")) {
        action(APP_BLUETOOTH_DEVICES, 1); assert(s_overlay == APP_OVERLAY_BLUETOOTH_DEVICES);
    } else if (!strcmp(scenario, "device-pause")) {
        remote_device_set_maintenance(true); assert(local_access() == APP_LOCAL_PAUSED);
    } else if (strstr(scenario, "pause")) {
        test_update_busy = true; remote_update_service(); assert(local_access() == APP_LOCAL_PAUSED);
    } else {
        s_priority_requests = LOCK_REQUEST;
        assert(process_priority() && s_engine.state.locked);
    }
}

static void dispatch_before_revoke(void)
{
    assert(!s_engine.state.locked && s_overlay == APP_OVERLAY_AUDIO_DEVICE && local_access() == APP_LOCAL_NORMAL);
    device_radio_dispatch();
}

int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    setup(INPUT_KEYBOARD); fill_preview(2); device_radio_init();
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true,
        .audio = {.state = AE_OFF, .route = AE_ROUTE_USB_UAC, .audio_generation = 3,
                  .revision = 5, .hardware_off_confirmed = true}};
    assert(app_media_controls_init()); action(APP_BLUETOOTH_DEVICES, 1); present();
    app_intent_t choose = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    choose.media.operation = APP_MEDIA_DEVICE_DETAILS;
    choose.media.object_revision = test_view.media.devices.revision;
    choose.media.value.peer_id = test_view.media.devices.rows[0].id;
    deliver(choose); present(); assert(s_overlay == APP_OVERLAY_AUDIO_DEVICE);
    bool expiry = strstr(scenario, "expiry") != NULL;
    input_link_t observed = input_link_snapshot(&test_link), previous = observed;
    uint64_t admitted_epoch = s_engine.state.epoch;
    if (expiry) {
        assert(observed.connected && observed.ready && !observed.congested);
        observed.congested = true; observe_input_link(&observed, &previous);
        uint64_t start = s_now_ms;
        while (s_now_ms + COORDINATOR_PERIOD_MS < start + HID_ENGINE_CONGESTION_TIMEOUT_MS) {
            s_now_ms += COORDINATOR_PERIOD_MS; observe_input_link(&observed, &previous); present();
        }
        assert(s_engine.state.epoch == admitted_epoch && s_engine.congested && test_view.media.devices.can_connect[1]);
    }
    bool voice = !strncmp(scenario, "voice", 5);
    app_intent_t connect = make_intent(APP_MEDIA_CONTROL, 0, 0, NULL);
    connect.media = (app_media_request_t){.operation = APP_MEDIA_DEVICE_LINK,
        .management_generation = test_view.media.management_generation, .audio_generation = test_view.media.audio_generation,
        .resource_revision = test_view.media.resource_revision, .peer_id = test_view.media.peer_id,
        .object_id = test_view.media.devices.selected, .object_revision = test_view.media.devices.revision,
        .value.page_delta = voice ? 0 : 2};
    deliver(connect); assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
    bt_audio_service(); assert(device_radio_calls() == 0);
    unsigned already_dispatched = 0;
    if (voice) {
        device_radio_dispatch(); device_radio_hfp(false); present(); bt_audio_service();
        assert(device_radio_calls() == 1 && bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
        already_dispatched = 1;
    }
    bool normal = strstr(scenario, "normal") != NULL;
    bool replacement = !strcmp(scenario, "replacement-owner");
    bool dispatch_first = !strcmp(scenario, "dispatch-first") || !strcmp(scenario, "expiry-dispatch-first");
    if (replacement) {
        bt_audio_snapshot_t current = bt_audio_snapshot();
        bt_audio_cancel_request(current.request_id);
        assert(bt_audio_saved_request(current.generation, current.a2dp_link_generation,
            test_view.media.devices.revision, BT_AUDIO_CONNECT_A2DP, test_view.media.devices.address, NULL) == ESP_OK);
        device_radio_dispatch(); bt_audio_service(); /* Drain only the retired posted item. */
    }
    if (expiry) {
        if (dispatch_first) device_radio_dispatch();
        if (!normal) {
            s_now_ms += COORDINATOR_PERIOD_MS; observe_input_link(&observed, &previous);
            assert(s_engine.state.epoch != admitted_epoch && s_engine.congestion_cancelled && s_overlay == APP_OVERLAY_AUDIO_DEVICE);
            printf("timeout=%u period=%u epoch %llu -> %llu; ", (unsigned)HID_ENGINE_CONGESTION_TIMEOUT_MS,
                (unsigned)COORDINATOR_PERIOD_MS, (unsigned long long)admitted_epoch, (unsigned long long)s_engine.state.epoch);
        }
        if (strstr(scenario, "after-sync")) service_media();
        device_radio_dispatch();
    } else if (!strcmp(scenario, "revoke-first")) {
        device_radio_before_lock(revoke); device_radio_dispatch();
    } else {
        if (dispatch_first) device_radio_before_lock(dispatch_before_revoke);
        if (!normal) revoke();
        if (strstr(scenario, "after-sync")) service_media();
        device_radio_dispatch();
    }
    unsigned expected = already_dispatched + (normal || replacement || dispatch_first ? 1U : 0U);
    printf("%s: SDK calls=%u expected=%u locked=%u overlay=%d access=%d\n", scenario,
        device_radio_calls(), expected, s_engine.state.locked, (int)s_overlay, (int)local_access());
    assert(device_radio_calls() == expected);
    if (dispatch_first) assert(bt_audio_snapshot().control_inflight); /* No retroactive withdrawal. */
    assert(test_media_snapshot.audio.route == AE_ROUTE_USB_UAC && !strcmp(s_interaction.view.preview, "AB"));
    return 0;
}
