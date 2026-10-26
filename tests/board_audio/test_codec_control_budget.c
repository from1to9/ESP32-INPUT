// SPDX-License-Identifier: Apache-2.0
#define main board_fixture_main
#include "test_board_audio.c"
#undef main

static void cancel_stop(void) { assert(port.stop(NULL, 1) == AE_AGAIN); }
static void cancel_mute(void) { assert(port.mute(NULL, AE_MICROPHONE, true, 1) == AE_AGAIN); }
static void transfer_failure(void) { control_transfer_error = ESP_ERR_INVALID_RESPONSE; }

int main(int argc, char **argv)
{
    assert(argc == 2); port = board_audio_mosaico_port();
    assert(board_audio_mosaico_init() == AE_OK); drain();
    unsigned initial = initialization_calls, runtime = runtime_control_calls, payloads = control_payloads;
    unsigned waited = control_waited_ms;
    ae_result_t expected = AE_OK;
    bool cancelled_stop = !strcmp(argv[1], "stop"), cancelled_mute = !strcmp(argv[1], "mute");
    if (!strcmp(argv[1], "shared_write") || !strcmp(argv[1], "shared_read")) {
        peer_busy_ms = 6; peer_on_write = !strcmp(argv[1], "shared_write");
    } else if (!strcmp(argv[1], "exhausted")) {
        peer_busy_ms = 101; peer_on_write = true; expected = AE_TIMEOUT;
    } else if (!strcmp(argv[1], "transfer_failure")) {
        control_hook = transfer_failure; control_hook_at = initial + 2; expected = AE_IO_ERROR;
    } else {
        assert(cancelled_stop || cancelled_mute);
        control_hook = cancelled_stop ? cancel_stop : cancel_mute;
        control_hook_at = initial + 2; expected = AE_STALE;
    }
    assert(port.prepare(NULL, &mono, 3, 1) == AE_AGAIN); drain();
    ae_result_t result = port.prepare(NULL, &mono, 3, 1);
    assert(result == expected && !s.control.initializing && !s.capture_gate && !s.playback_gate);
    if (expected != AE_OK) {
        /* The real constructor continues callbacks after an error. The owner
         * must reject those before any further SDK wait or payload submission. */
        assert(initialization_calls == initial + 2 && !s.prepared && !s.codec && !s.control.device);
        assert(references[0] == 0 && references[1] == 0 && !s.codec_lifecycle_uncertain);
        if (expected == AE_TIMEOUT) {
            assert(control_payloads == payloads + 1 && s.status.first_sdk_error == ESP_ERR_TIMEOUT);
            assert(s.status.first_sdk_operation == BOARD_AUDIO_SDK_I2C_WRITE);
            assert(control_waited_ms - waited == 100 && peer_busy_ms == 1);
        } else if (expected == AE_IO_ERROR) {
            assert(control_payloads == payloads + 2 && s.status.first_sdk_error == ESP_ERR_INVALID_RESPONSE);
        } else assert(s.status.first_sdk_error == ESP_OK); /* Cancellation is not an SDK failure. */
        if (cancelled_stop) assert(port.stop(NULL, 1) == AE_OK);
        if (cancelled_mute) assert(port.mute(NULL, AE_MICROPHONE, true, 1) == AE_OK);
        peer_busy_ms = 0; control_transfer_error = ESP_OK;
        open_audio(&mono, 3, 2); /* Explicit later attempt, never an automatic retry. */
    } else assert(!peer_busy_ms && s.prepared && control_waited_ms - waited == 6);
    uint64_t generation = s.generation;
    unsigned before = runtime_control_calls;
    assert(port.volume(NULL, 40, generation) == AE_AGAIN); drain();
    assert(port.volume(NULL, 40, generation) == AE_OK && runtime_control_calls > before);
    unmute_role(AE_MICROPHONE, generation); unmute_role(AE_SPEAKER, generation);
    before = runtime_control_calls; assert(port.mute(NULL, AE_SPEAKER, true, generation) == AE_AGAIN); drain();
    assert(port.mute(NULL, AE_SPEAKER, true, generation) == AE_OK && runtime_control_calls > before);
    before = runtime_control_calls; close_audio(generation, AE_OK);
    assert(runtime_control_calls > before && runtime_control_calls > runtime && !s.control.initializing);
    printf("PASS: initialization-only codec budget %s result=%d; finite shared-bus wait, no repeated wait after failure/cancel, runtime/mute/cleanup5ms\n", argv[1], result);
    return 0;
}
