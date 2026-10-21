// SPDX-License-Identifier: Apache-2.0
#define main board_fixture_main
#include "test_board_audio.c"
#undef main
#include "audio_engine.h"

static uint64_t startup_clock(void *context) { (void)context; return 1000; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    bool probe_first = !strcmp(argv[1], "probe_first");
    bool failed = !strcmp(argv[1], "probe_failure");
    bool control_failure = !strcmp(argv[1], "control_failure");
    assert(probe_first || failed || control_failure || !strcmp(argv[1], "stop_first"));
    port = board_audio_mosaico_port();
    assert(board_audio_mosaico_init() == AE_OK && s.probe_pending);
    if (failed) fail_new = true;
    if (control_failure) { fail_register = 0x31; fail_read = true; }
    if (probe_first) drain();
    void *memory = calloc(1, ae_engine_bytes()); assert(memory);
    ae_dependencies_t dependencies = {.codec = port, .monotonic_ms = startup_clock};
    ae_engine_t *engine = NULL;
    assert(ae_init(memory, ae_engine_bytes(), &dependencies, 1000, &engine) == AE_AGAIN);
    drain();
    for (unsigned i = 0; i < 20; ++i) { (void)ae_poll(engine, 1000); drain(); }
    ae_snapshot_t view; ae_snapshot(engine, &view);
    board_audio_status_t status = board_audio_mosaico_status();
    ae_codec_capabilities_t caps;
    assert(port.capabilities(NULL, &caps) == AE_OK);
    assert(status.probe_complete && !s.probe_pending && !s.probe_running && channel_allocations == 1);
    assert(!s.prepared && !s.capture_gate && !s.playback_gate && !s.rx_running && !s.tx_running && !io_calls);
    if (failed || control_failure) {
        assert(!status.hardware_observed && !caps.available && status.probe_result != AE_OK && caps.reason != AE_OK);
        assert(status.probe_result == (failed ? AE_BUSY : AE_UNCERTAIN));
    } else {
        assert(view.state == AE_OFF && view.hardware_off_confirmed && !view.quarantined);
        assert(status.hardware_observed && status.probe_result == AE_OK && caps.available);
    }
    unsigned attempts = channel_allocations;
    for (unsigned i = 0; i < 20; ++i) { (void)ae_poll(engine, 1000); drain(); }
    assert(channel_allocations == attempts); /* Never retry failed readiness or start capture automatically. */
    printf("Actual engine/board startup %s: probe_complete=%u observed=%u result=%u off=%u; no PCM/reprobe.\n",
        argv[1], status.probe_complete, status.hardware_observed, (unsigned)status.probe_result, view.hardware_off_confirmed);
    free(memory);
    return 0;
}
