// SPDX-License-Identifier: Apache-2.0
#define main board_fixture_main
#include "test_board_audio.c"
#undef main
#include "audio_engine.h"

static void *engine_memory;
static ae_engine_t *engine;
static uint64_t engine_time_us = 1000000;
static const char *interleaving;
static const char *phase;
static bool mute_safety;
static unsigned calls_at_stop;
static uint64_t stop_time_us, acknowledged_us;
static uint64_t engine_clock(void *context) { (void)context; return engine_time_us / 1000U; }

static void initial_stop_during_probe(void)
{
    control_hook = initial_stop_during_probe; control_hook_at = UINT32_MAX;
    /* Model each admitted control callback, rather than freezing the engine's
     * clock until the entire probe has completed. */
    engine_time_us += 500;
    bool constructor = !s.codec && references[CODEC_REF_STAGE_OPEN] == 1;
    bool configure = s.codec && !references[CODEC_REF_STAGE_ENABLE];
    bool enabling = s.codec && references[CODEC_REF_STAGE_ENABLE] == 1 && !pcm_observation.mute_requested;
    bool muting = s.codec && pcm_observation.mute_requested;
    bool selected = (!strcmp(phase, "constructor") && constructor) ||
        (!strcmp(phase, "configure") && configure) ||
        ((!strcmp(phase, "enable") || !strcmp(phase, "enable_failure") || !strcmp(phase, "enable_blocked")) && enabling) ||
        (!strcmp(phase, "initial_mute") && muting);
    if (!engine && selected) {
        assert(s.probe_running && !s.capture_gate && !s.playback_gate && !io_calls && !pa_level);
        ae_dependencies_t dependencies = {.codec = port, .monotonic_ms = engine_clock};
        assert(ae_init(engine_memory, ae_engine_bytes(), &dependencies, engine_clock(NULL), &engine) == AE_AGAIN);
        stop_time_us = engine_time_us;
        if (mute_safety) assert(port.mute(NULL, AE_MICROPHONE, true, 1) == AE_AGAIN);
        assert(s.stop_pending); calls_at_stop = initialization_calls;
        if (!strcmp(phase, "enable_failure")) control_transfer_error = ESP_ERR_INVALID_RESPONSE;
        if (!strcmp(phase, "enable_blocked")) {
            /* Poll the actual engine while the currently admitted SDK call is
             * still blocked for its maximum advertised wait. */
            for (int ms = 0; ms < control_timeout_argument; ++ms) {
                engine_time_us += 1000; (void)ae_poll(engine, engine_clock(NULL));
            }
        }
    }
    if (engine) (void)ae_poll(engine, engine_clock(NULL));
}

int main(int argc, char **argv)
{
    assert(argc == 2); interleaving = argv[1]; mute_safety = !strncmp(interleaving, "mute_", 5);
    phase = mute_safety ? interleaving + 5 : interleaving;
    assert(!strcmp(phase, "constructor") || !strcmp(phase, "configure") ||
        !strcmp(phase, "enable") || !strcmp(phase, "initial_mute") ||
        !strcmp(phase, "enable_failure") || !strcmp(phase, "enable_blocked"));
    engine_memory = calloc(1, ae_engine_bytes()); assert(engine_memory);
    port = board_audio_mosaico_port(); assert(board_audio_mosaico_init() == AE_OK);
    control_hook = initial_stop_during_probe; control_hook_at = UINT32_MAX;
    drain(); assert(engine);
    for (unsigned i = 0; i < 20; ++i) {
        engine_time_us += 10000; (void)ae_poll(engine, engine_clock(NULL)); drain();
        ae_snapshot_t current; ae_snapshot(engine, &current);
        if (current.hardware_off_confirmed && !acknowledged_us) acknowledged_us = engine_time_us;
    }
    board_audio_status_t status = board_audio_mosaico_status();
    ae_snapshot_t view; ae_snapshot(engine, &view);
    printf("probe interleaving=%s probe=%d observed=%d firstSDK=%d uncertain=%d enabled_ref=%d engine=%d error=%d off=%d allocations=%u pcm=%u\n",
        interleaving, status.probe_result, status.hardware_observed, status.first_sdk_error,
        status.cleanup_uncertain, references[CODEC_REF_STAGE_ENABLE], view.state, view.error,
        view.hardware_off_confirmed, channel_allocations, io_calls); fflush(stdout);
    if (!strcmp(phase, "enable_failure")) {
        assert(status.probe_result == AE_UNCERTAIN && status.first_sdk_error == ESP_ERR_INVALID_RESPONSE);
        assert(status.cleanup_uncertain && !status.hardware_observed && !view.hardware_off_confirmed && view.quarantined);
    } else {
        assert(status.probe_complete && status.probe_result == AE_OK && status.hardware_observed);
        assert(!status.cleanup_uncertain && view.state == AE_OFF && view.hardware_off_confirmed && !view.quarantined);
        assert(references[0] == 0 && references[1] == 0 && !s.codec && !s.control.device);
        assert(initialization_calls == calls_at_stop); /* No new 100 ms wait after the stop. */
        assert(acknowledged_us >= stop_time_us && acknowledged_us - stop_time_us < AE_STOP_TIMEOUT_MS * 1000U);
    }
    assert(channel_allocations == 1 && !io_calls && !s.capture_gate && !s.playback_gate && !s.probe_pending);
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) assert(view.capture[i].state == AE_JOB_EMPTY);
    assert(view.playback.state == AE_JOB_EMPTY);
    free(engine_memory);
    return 0;
}
