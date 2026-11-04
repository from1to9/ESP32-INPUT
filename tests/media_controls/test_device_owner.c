// SPDX-License-Identifier: Apache-2.0
#define main media_fixture_main
#define TEST_MEDIA_REAL_BLUETOOTH
#include "test_media_controls.c"
#undef main

void device_radio_init(void);
void device_radio_dispatch(void);
unsigned device_radio_calls(void);
void device_radio_hfp(bool foreign);
void device_radio_speaker(void);
void device_radio_voice(void);

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    device_radio_init(); setup();
    context.mode = INPUT_KEYBOARD; context.overlay = APP_OVERLAY_BLUETOOTH_DEVICES;
    context.bluetooth_audio_devices = true; owner.audio.route = AE_ROUTE_USB_UAC;
    if (!strcmp(scenario, "voice-existing")) device_radio_hfp(false);
    poll_at(1010);
    app_intent_t choose = intent(APP_MEDIA_DEVICE_DETAILS);
    choose.media.object_revision = view.media.devices.revision;
    choose.media.value.peer_id = view.media.devices.rows[0].id;
    apply_effect(handle(choose)); poll_at(1020);
    if (!strcmp(scenario, "refresh")) {
        device_radio_speaker(); poll_at(1021);
        assert(view.media.devices.connected[1]);
        assert(bt_audio_refresh_bonds() == ESP_OK); poll_at(1022);
        assert(view.media.devices.identity[0] && view.media.devices.connected[1]);
    } else {
        bool microphone = !strncmp(scenario, "voice", 5);
        app_intent_t connect = intent(APP_MEDIA_DEVICE_LINK);
        connect.media.object_revision = view.media.devices.revision;
        connect.media.object_id = view.media.devices.selected;
        connect.media.value.page_delta = microphone ? 0 : 2;
        if (!strcmp(scenario, "other-before-submit")) device_radio_hfp(true);
        handle(connect); bt_audio_service();
        if (!strcmp(scenario, "other-before-sdk")) device_radio_hfp(true);
        if (!strcmp(scenario, "same-before-sdk")) device_radio_hfp(false);
        if (!strcmp(scenario, "lock")) context.locked = true;
        if (!strcmp(scenario, "maintenance")) context.local_access = APP_LOCAL_PAUSED;
        if (!strcmp(scenario, "leave")) {
            ++context.epoch; context.overlay = APP_OVERLAY_NONE; context.bluetooth_audio_devices = false;
        }
        app_media_controls_context(&context); device_radio_dispatch();
        if (!strcmp(scenario, "normal") || !strcmp(scenario, "same-before-sdk")) assert(device_radio_calls() == 1);
        else if (microphone) {
            assert(device_radio_calls() == 1);
            if (!strcmp(scenario, "voice-existing")) {
                device_radio_voice(); poll_at(1040);
                assert(bt_audio_snapshot().hfp_audio_connected && !submissions);
                puts("PASS: shared device owner existing SLC opens only SCO"); return 0;
            }
            if (!strcmp(scenario, "voice-lock")) { context.locked = true; app_media_controls_context(&context); }
            device_radio_hfp(false); poll_at(1030); bt_audio_service();
            if (!strcmp(scenario, "voice-second-lock")) { context.locked = true; app_media_controls_context(&context); }
            device_radio_dispatch();
            assert(device_radio_calls() == (context.locked ? 1U : 2U));
            if (!context.locked) {
                device_radio_voice(); poll_at(1040);
                assert(bt_audio_snapshot().hfp_audio_connected);
            }
        } else assert(device_radio_calls() == 0);
    }
    assert(!submissions && owner.audio.route == AE_ROUTE_USB_UAC);
    printf("PASS: shared device owner %s\n", scenario);
    return 0;
}
