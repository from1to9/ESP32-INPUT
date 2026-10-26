// SPDX-License-Identifier: Apache-2.0
/* Actual diagnostic owner + board adapter + pinned codec; only SDK timing,
 * bus, GPIO and I2S boundaries are modeled by the existing board fixture. */
#define main board_fixture_main
#define vTaskDelay board_fixture_delay
#define ulTaskNotifyTake board_fixture_wait
#include "../board_audio/test_board_audio.c"
#undef main
#undef vTaskDelay
#undef ulTaskNotifyTake
void vTaskDelay(TickType_t ticks);
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
#include "../../firmware/main/app_local_speaker_test.c"
#include "audio_engine.h"

static uint64_t fake_ms = 1000;
static const char *scenario;
static bool saw_output, saw_cancel;
static unsigned replacement_wakes;
int64_t esp_timer_get_time(void) { return (int64_t)(fake_ms * 1000U); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)&fake_ms; }
void vTaskDelay(TickType_t ticks) { fake_ms += ticks; }
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    assert(clear == pdTRUE && ticks == 1 && !lock_depth);
    bool stalled = (!strcmp(scenario, "prepare-timeout") && s.slot == SLOT_PENDING && s.request.operation == OP_PREPARE) ||
        (!strcmp(scenario, "write-timeout") && s.slot == SLOT_PENDING && s.request.operation == OP_WRITE) ||
        (!strcmp(scenario, "stop-timeout") && s.stop_pending);
    if (!strcmp(scenario, "cancel-unmute") && s.slot == SLOT_PENDING && s.request.operation == OP_UNMUTE) {
        saw_cancel = true; app_local_speaker_test_cancel();
    }
    if (!stalled || (s.stop_pending && strcmp(scenario, "stop-timeout"))) drain();
    if (io_calls && !saw_output) {
        saw_output = true;
        assert(pa_level == 1 && (registers[0x31] & 0x60U) == 0 && registers[0x32] == 0xb2);
        assert(emitted_bytes && emitted_bytes <= sizeof(emitted));
        for (size_t i = 0; i < emitted_bytes / 4; ++i) {
            int16_t expected = wave[i % 48];
            assert(sample_get(emitted + i * 4) == expected && sample_get(emitted + i * 4 + 2) == expected);
        }
        if (!strcmp(scenario, "cancel")) { saw_cancel = true; app_local_speaker_test_cancel(); }
        if (!strcmp(scenario, "clock-regression")) fake_ms = 0;
    }
    assert(!s.rx_running && !s.capture_gate); /* No microphone is opened. */
    ++fake_ms;
    return 1;
}
static void replacement_notify(void *context) { assert(context == &replacement_wakes); ++replacement_wakes; }
static uint64_t normal_clock(void *context) { (void)context; return fake_ms; }
static void verify_normal_engine_handover(void)
{
    unsigned tasks = task_creations;
    assert(board_audio_mosaico_init() == AE_OK && task_creations == tasks && !s.probe_pending);
    assert(!s.completion_notify && !s.completion_context);
    port = board_audio_mosaico_port();
    port.completion_notify(NULL, replacement_notify, &replacement_wakes);
    void *memory = calloc(1, ae_engine_bytes()); assert(memory);
    ae_engine_t *engine = NULL;
    ae_dependencies_t dependencies = {.codec = port, .monotonic_ms = normal_clock};
    assert(ae_init(memory, ae_engine_bytes(), &dependencies, fake_ms, &engine) == AE_AGAIN);
    drain(); assert(ae_poll(engine, fake_ms) == AE_OK);
    ae_snapshot_t view; ae_snapshot(engine, &view);
    assert(view.audio_generation == 1 && view.hardware_off_confirmed);
    assert(ae_route_select(engine, AE_ROUTE_LOCAL, view.audio_generation, view.revision, fake_ms) == AE_OK);
    ae_snapshot(engine, &view); assert(view.audio_generation == 2);
    ae_result_t enabled = AE_AGAIN;
    for (unsigned i = 0; i < 10 && enabled == AE_AGAIN; ++i) {
        enabled = ae_enable(engine, AE_SPEAKER, true, &format, fake_ms); drain();
    }
    assert(enabled == AE_OK && s.last_generation == 2 && replacement_wakes);
    ae_safety_stop(engine, AE_STOP_PRIVACY, fake_ms);
    for (unsigned i = 0; i < 20; ++i) { (void)ae_poll(engine, fake_ms); drain(); }
    assert(!s.prepared && !s.tx_running && !s.rx_running && !pa_level);
    port.completion_notify(NULL, NULL, NULL); free(memory);
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1]; port = board_audio_mosaico_port();
    bool locked = !strcmp(scenario, "locked");
    if (!strcmp(scenario, "cancel-before")) app_local_speaker_test_cancel();
    if (!strcmp(scenario, "short-write")) transfer_limit = 4;
    if (!strcmp(scenario, "sdk-error")) transfer_error = ESP_FAIL;
    if (!strcmp(scenario, "probe-error")) fail_new = true;
    if (!strcmp(scenario, "volume-error")) corrupt_volume = true;
    bool handover = app_local_speaker_test_run(locked);
    unsigned submitted = io_calls, tasks = task_creations, notices = notifications;
    assert(app_local_speaker_test_run(false) == handover && io_calls == submitted && task_creations == tasks);
    wake(NULL); assert(notifications == notices); /* Copied callback after detach is inert. */
    assert(!s.completion_notify && !s.completion_context);
    if (locked || !strcmp(scenario, "cancel-before")) {
        assert(handover && !result.attempted && !tasks && !io_calls && !result.produced_bytes && !result.stop_confirmed);
    } else if (!strcmp(scenario, "success")) {
        assert(handover && result.result == AE_OK && result.stop_confirmed && saw_output);
        assert(result.produced_bytes == 288000 && result.submitted_bytes == 288000 && result.accepted_bytes == 288000);
        assert(io_calls == 300 && result.write_calls == 600 && fake_ms < 9000);
        verify_normal_engine_handover();
    } else if (!strcmp(scenario, "stop-timeout")) {
        assert(!handover && !result.stop_confirmed && result.stop_result == AE_TIMEOUT && s.stop_pending);
    } else if (!strcmp(scenario, "probe-error")) {
        assert(!handover && result.result != AE_OK && !io_calls);
    } else if (!strcmp(scenario, "clock-regression")) {
        assert(!handover && result.result == AE_TIME_UNKNOWN && !result.stop_confirmed && s.stop_pending);
    } else {
        assert(handover && result.stop_confirmed && result.result != AE_OK);
        if (!strcmp(scenario, "cancel")) assert(saw_cancel && result.result == AE_PRIVACY && !result.accepted_bytes);
        if (!strcmp(scenario, "cancel-unmute")) assert(saw_cancel && result.result == AE_PRIVACY && !io_calls);
        if (!strcmp(scenario, "volume-error")) assert(result.result == AE_INTEGRITY && !io_calls);
        if (!strcmp(scenario, "short-write")) assert(result.result == AE_TIMEOUT && result.accepted_bytes == 2 && io_calls == 1);
        if (!strcmp(scenario, "sdk-error")) assert(result.result == AE_IO_ERROR && io_calls == 1);
        if (!strcmp(scenario, "prepare-timeout") || !strcmp(scenario, "write-timeout")) assert(result.result == AE_TIMEOUT);
    }
    for (size_t i = 0; i < sizeof(frame); ++i) assert(frame[i] == 0);
    printf("PASS: local speaker %s produced=%u submitted=%u accepted=%u result=%d stopped=%d handover=%d\n",
        scenario, result.produced_bytes, result.submitted_bytes, result.accepted_bytes,
        (int)result.result, result.stop_confirmed, handover);
    return 0;
}
