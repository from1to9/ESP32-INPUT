// SPDX-License-Identifier: Apache-2.0
/* Real engine + real Bluetooth owner; only SDK and codec boundaries are models. */
#include "bt_audio.h"
#define main engine_fixture_main
#include "../audio_engine/test_engine.c"
#undef main
void test_transport_setup(void);
void test_transport_teardown(void);
static ae_route_port_t real_route;
static unsigned revoke_after_prepare;
static ae_result_t prepare_then_revoke(void *context, ae_route_t route, const ae_format_t *format,
    uint8_t roles, uint64_t generation, const ae_route_token_t *token)
{
    ae_result_t result = real_route.prepare(context, route, format, roles, generation, token);
    if (revoke_after_prepare == 1) model.owner_alive = false;
    if (revoke_after_prepare == 2) model.cancel = true;
    return result;
}
void test_transport_foreign_event(void);
void test_transport_replace_voice(void);
bool test_transport_microphone_frame(void);
ae_result_t test_transport_playback_frame(ae_frame_t *frame);

static void running_duplex(const ae_route_token_t *token, bool privacy)
{
    model.duplex = true;
    ae_start_request_t capture = request(AE_CAPTURE_LIVE); capture.route_token = *token;
    ae_job_snapshot_t job;
    assert(ae_start(engine, &capture, model.now, &job) == AE_OK); step(8);
    assert(snapshot().capture[AE_CAPTURE_LIVE].state == AE_JOB_RUNNING && test_transport_microphone_frame());
    ae_start_request_t playback = request(AE_PLAYBACK_STREAM); playback.route_token = *token;
    assert(ae_start(engine, &playback, model.now, &job) == AE_OK);
    ae_frame_t frame;
    for (unsigned i = 0; i < 8 && !snapshot().playback_gate_open; ++i) step(1);
    test_transport_foreign_event();
    assert(test_transport_playback_frame(&frame) == AE_OK && frame.data[0] == 42);
    assert(ae_submit_playback(engine, &frame, model.now) == AE_OK); step(2);
    assert(model.writes && model.mic && model.speaker && test_transport_microphone_frame());
    if (privacy) ae_safety_stop(engine, AE_STOP_PRIVACY, model.now);
    else test_transport_replace_voice();
    step(8);
    ae_snapshot_t view = snapshot();
    assert(!view.capture_gate_open && !view.playback_gate_open && view.hardware_off_confirmed && !view.quarantined);
    assert(!test_transport_microphone_frame() && test_transport_playback_frame(&frame) != AE_OK);
    assert(view.capture[AE_CAPTURE_LIVE].stop_reason == (privacy ? AE_STOP_PRIVACY : AE_STOP_OWNER_LOST));
    assert(view.playback.stop_reason == view.capture[AE_CAPTURE_LIVE].stop_reason);
    assert(bt_audio_pairing_open(1000) == ESP_OK); bt_audio_pairing_close(); bt_audio_service();
}

int main(int argc, char **argv)
{
    assert(argc == 2); test_transport_setup();
    ae_route_port_t port = bt_audio_route_port(); ae_route_capabilities_t caps;
    assert(port.capabilities(NULL, AE_ROUTE_HFP_HF, &caps) == AE_OK && caps.connected);
    real_route = port; port.prepare = prepare_then_revoke;
    setup_with_route(&port, &caps.format);
    ae_snapshot_t view = snapshot();
    assert(ae_route_select(engine, AE_ROUTE_HFP_HF, view.audio_generation, view.revision, model.now) == AE_OK);
    if (!strncmp(argv[1], "duplex-", 7)) {
        running_duplex(&caps.token, !strcmp(argv[1], "duplex-privacy"));
        free(memory); test_transport_teardown(); printf("PASS: running HFP Mic/Speaker %s\n", argv[1]); return 0;
    }
    size_t initial_stops = model.stops;
    bool before_start = strcmp(argv[1], "before-prepare") != 0;
    revoke_after_prepare = !strcmp(argv[1], "owner-loss") ? 1U : !strcmp(argv[1], "privacy") ? 2U : 0U;
    if (before_start) { assert(bt_audio_pairing_open(120000) == ESP_OK); bt_audio_service(); }
    ae_start_request_t start = request(AE_CAPTURE_LIVE); start.route_token = caps.token;
    ae_job_snapshot_t job; assert(ae_start(engine, &start, model.now, &job) == AE_OK);
    if (!before_start) { assert(bt_audio_pairing_open(120000) == ESP_OK); bt_audio_service(); }
    assert(bt_audio_snapshot().discoverable);
    model.now += 20; visible(); ae_result_t result = ae_poll(engine, model.now);
    assert(result == AE_BUSY || (revoke_after_prepare && result == AE_AGAIN));
    view = snapshot();
    assert(!view.quarantined && view.state == AE_OFF && view.hardware_off_confirmed && model.prepares == 0 && model.stops == initial_stops);
    assert(view.capture[AE_CAPTURE_LIVE].state == AE_JOB_ERROR);
    assert(view.capture[AE_CAPTURE_LIVE].error == (revoke_after_prepare == 1 ? AE_UNAVAILABLE : revoke_after_prepare == 2 ? AE_PRIVACY : AE_BUSY));
    assert(view.capture[AE_CAPTURE_LIVE].stop_reason ==
        (revoke_after_prepare == 1 ? AE_STOP_OWNER_LOST : revoke_after_prepare == 2 ? AE_STOP_PRIVACY : AE_STOP_BUSY));
    bt_audio_pairing_close(); bt_audio_service(); assert(!bt_audio_snapshot().pairing_busy);
    step(20); view = snapshot(); assert(!view.quarantined && view.state == AE_OFF && model.stops == initial_stops);
    model.owner_alive = true; model.cancel = false; revoke_after_prepare = 0;
    start = request(AE_CAPTURE_LIVE); start.owner_session_id.bytes[0]++; start.route_token = caps.token;
    assert(ae_start(engine, &start, model.now, &job) == AE_OK); step(8); view = snapshot();
    assert(!view.quarantined && model.prepares == 1 && view.capture[AE_CAPTURE_LIVE].state == AE_JOB_RUNNING);
    assert(ae_stop(engine, &view.capture[AE_CAPTURE_LIVE].job_id, view.capture[AE_CAPTURE_LIVE].consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK);
    step(3); free(memory); test_transport_teardown();
    printf("PASS: engine/Bluetooth pairing exclusion and fresh start %s\n", argv[1]); return 0;
}
