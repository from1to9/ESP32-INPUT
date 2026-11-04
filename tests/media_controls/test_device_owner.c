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
void device_radio_auth(bool success);
void device_radio_catalogue(esp_err_t error, bool empty);
unsigned device_radio_discoveries(void);

static void catalogue_transition(const char *scenario)
{
    assert(view.media.devices.known && view.media.devices.count == 2);
    app_media_id_t retained = view.media.devices.rows[0].id;
    bool inbound = !strcmp(scenario, "catalogue-inbound");
    if (inbound) device_radio_speaker();
    device_radio_auth(strcmp(scenario, "catalogue-auth-failure") != 0);
    poll_at(1011);
    assert(!view.media.devices.known && view.media.devices.refreshing);
    assert(view.media.devices.count == 2 && same_ui_id(view.media.devices.rows[0].id, retained));
    assert(view.media.devices.rows[0].connected == inbound);
    app_intent_t choose = intent(APP_MEDIA_DEVICE_DETAILS);
    choose.media.object_revision = view.media.devices.revision;
    choose.media.value.peer_id = retained;
    apply_effect(handle(choose));
    assert(view.media.devices.identity[0] && !view.media.devices.selected_saved);
    assert(!view.media.devices.can_connect[0] && !view.media.devices.can_connect[1] &&
        !view.media.devices.can_disconnect[0] && !view.media.devices.can_disconnect[1]);
    bool failed = !strcmp(scenario, "catalogue-error");
    bool empty = !strcmp(scenario, "catalogue-empty");
    device_radio_catalogue(failed ? ESP_FAIL : ESP_OK, empty);
    bt_audio_service(); poll_at(1012);
    assert(view.media.devices.identity[0] && view.media.devices.connected[1] == inbound);
    assert(view.media.devices.known == !failed && !view.media.devices.refreshing);
    assert(view.media.devices.count == (empty ? 0U : 2U));
    assert(view.media.devices.selected_saved == (!failed && !empty));
    if (failed || empty) {
        app_intent_t link = intent(APP_MEDIA_DEVICE_LINK);
        link.media.object_revision = view.media.devices.revision;
        link.media.object_id = retained; link.media.value.page_delta = 2;
        handle(link); bt_audio_service(); device_radio_dispatch();
        assert(!device_radio_calls());
    }
    if (failed) {
        assert(view.media.devices.can_refresh);
        device_radio_catalogue(ESP_OK, false);
        context.local_access = APP_LOCAL_BROWSE; app_media_controls_context(&context);
        app_intent_t refresh = intent(APP_MEDIA_DEVICE_REFRESH);
        handle(refresh); app_media_controls_fill(&view);
        assert(view.media.devices.refreshing && view.media.devices.count == 2 && !view.media.devices.can_refresh);
        assert(!view.media.devices.can_connect[1]);
        bt_audio_service(); poll_at(1013);
        assert(view.media.devices.known && view.media.devices.selected_saved);
        context.local_access = APP_LOCAL_NORMAL; poll_at(1014);
        app_intent_t link = intent(APP_MEDIA_DEVICE_LINK);
        link.media.object_revision = view.media.devices.revision;
        link.media.object_id = retained; link.media.value.page_delta = 2;
        handle(link); bt_audio_service(); device_radio_dispatch();
        assert(device_radio_calls() == 1);
        app_media_controls_fill(&view); assert(!view.media.devices.can_refresh);
        device_radio_speaker(); poll_at(1015);
        assert(view.media.devices.connected[1]);
    }
    assert(!device_radio_discoveries() && !bt_audio_snapshot().pairing_requested && !submissions);
    assert(owner.audio.route == AE_ROUTE_USB_UAC);
    printf("PASS: shared device owner %s\n", scenario);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    device_radio_init(); setup();
    context.mode = INPUT_KEYBOARD; context.overlay = APP_OVERLAY_BLUETOOTH_DEVICES;
    context.bluetooth_audio_devices = true; owner.audio.route = AE_ROUTE_USB_UAC;
    if (!strcmp(scenario, "voice-existing")) device_radio_hfp(false);
    poll_at(1010);
    if (!strncmp(scenario, "catalogue-", 10)) { catalogue_transition(scenario); return 0; }
    app_intent_t choose = intent(APP_MEDIA_DEVICE_DETAILS);
    choose.media.object_revision = view.media.devices.revision;
    choose.media.value.peer_id = view.media.devices.rows[0].id;
    apply_effect(handle(choose)); poll_at(1020);
    assert(!bt_audio_snapshot().pairing_requested && !device_radio_discoveries());
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
    assert(!bt_audio_snapshot().pairing_requested && !device_radio_discoveries());
    printf("PASS: shared device owner %s\n", scenario);
    return 0;
}
