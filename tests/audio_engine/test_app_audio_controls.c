// SPDX-License-Identifier: Apache-2.0
/* Reuse the threaded production-worker fixture. Only the clock and codec
 * completion boundary change: a write cannot finish in its submitting wake. */
#define main stream_fixture_main
#define esp_timer_get_time stream_timer_get_time
#define ulTaskNotifyTake stream_notify_take
#include "test_app_audio.c"
#undef main
#undef esp_timer_get_time
#undef ulTaskNotifyTake

static atomic_bool pressure, finished;
static atomic_uint_fast64_t clock_ms = 1000;
static unsigned iteration, stage, requested_at, writes_before, stage_at;
static uint64_t write_started, request_token = 100;
static ae_frame_t pressure_frame;
static bool privacy_case;

int64_t esp_timer_get_time(void) { return (int64_t)atomic_load(&clock_ms) * 1000; }

static ae_result_t delayed_write(void *context, uint64_t generation, const void *data, size_t bytes, size_t *actual)
{
    uint64_t now = atomic_load(&clock_ms);
    if (pending == 5 && write_started == now) {
        assert(!memcmp(data, submitted, bytes)); *actual = 0; return AE_AGAIN;
    }
    bool submitting = !pending;
    ae_result_t result = write_codec(context, generation, data, bytes, actual);
    if (submitting) write_started = now;
    return result;
}

static void submit_control(app_audio_command_t request, const ae_snapshot_t *view)
{
    request.expected_generation = view->audio_generation; request.expected_revision = view->revision;
    request.request_token = ++request_token;
    assert(app_audio_submit(&request) == AE_OK);
    requested_at = iteration; writes_before = atomic_load(&writes);
}

unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    if (!atomic_load(&pressure)) return stream_notify_take(clear, ticks);
    ++iteration; atomic_fetch_add(&clock_ms, 1);
    ae_snapshot_t view = snapshot();
    app_audio_result_t result;
    if ((stage == 1 || stage == 3 || stage == 5 || stage == 7) && app_audio_take_result(&result)) {
        assert(result.request_token == request_token);
        printf("control stage=%u result=%d elapsed_ms=%u writes=%u received=%llu delivered=%llu state=%d stop=%d error=%d\n",
            stage, result.result, iteration - requested_at, atomic_load(&writes),
            (unsigned long long)view.playback.received_bytes, (unsigned long long)view.playback.delivered_bytes,
            view.playback.state, view.playback.stop_reason, view.playback.error);
        fflush(stdout);
        if (stage == 7) {
            assert(result.result == AE_PRIVACY && view.volume_percent == 35);
            assert(view.playback.state == AE_JOB_PARTIAL && view.playback.stop_reason == AE_STOP_PRIVACY);
            assert(view.playback.error == AE_PRIVACY && !view.playback_gate_open);
            atomic_store(&pressure, false); atomic_store(&finished, true); return 0;
        }
        assert(result.result == AE_OK && iteration - requested_at <= 8);
        if (stage != 5) {
            assert(view.playback.state == AE_JOB_RUNNING && !view.overruns && !view.playback.discarded_frames);
            assert(stage == 1 ? view.volume_percent == 45 : view.microphone_gain_db == 6);
        }
        stage_at = iteration; ++stage;
    }
    if (privacy_case && stage == 1 && iteration == requested_at + 1) {
        app_audio_request_safety_stop(AE_STOP_PRIVACY); stage = 7;
    } else if (!stage && iteration >= 32) {
        assert(view.playback.state == AE_JOB_RUNNING && pending == 5);
        submit_control((app_audio_command_t){.kind = APP_AUDIO_CMD_VOLUME, .level = 45}, &view); stage = 1;
    } else if ((stage == 2 || stage == 4) && iteration - stage_at >= 32) {
        assert(atomic_load(&writes) > writes_before + 16 && view.playback.state == AE_JOB_RUNNING);
        assert(!view.overruns && !view.playback.discarded_frames);
        if (stage == 2) submit_control((app_audio_command_t){.kind = APP_AUDIO_CMD_GAIN, .level = 6}, &view);
        else submit_control((app_audio_command_t){.kind = APP_AUDIO_CMD_STOP, .job_id = view.playback.job_id,
            .consumer_generation = view.playback.consumer_generation, .stop_reason = AE_STOP_REQUESTED}, &view);
        ++stage;
    } else if (stage == 6) {
        assert(view.playback.state == AE_JOB_COMPLETED && view.playback.stop_reason == AE_STOP_REQUESTED);
        atomic_store(&pressure, false); atomic_store(&finished, true); return 0;
    }
    if (stage < 5) {
        size_t capacity = app_audio_transport_playback_capacity(AE_ROUTE_USB_UAC, view.audio_generation);
        for (size_t i = 0; i < capacity; ++i) {
            pressure_frame.sequence++; pressure_frame.sample_offset = (pressure_frame.sequence - 1) * 480;
            memset(pressure_frame.data, (int)pressure_frame.sequence, pressure_frame.bytes);
            assert(app_audio_submit_transport_playback(AE_ROUTE_USB_UAC, &pressure_frame) == AE_OK);
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "privacy")));
    privacy_case = argc == 2;
    ae_dependencies_t dependencies = {.codec = {.capabilities = capabilities, .prepare = prepare, .mute = mute_codec,
        .volume = level, .gain = level, .stop = stop_codec, .write = delayed_write, .completion_notify = completion},
        .route = {.capabilities = route_caps, .prepare = route_prepare, .stop = route_stop, .playback_consumed = consumed},
        .authority = authority, .trusted_unix_time = wall};
    assert(app_audio_init(&dependencies) == AE_OK);
    ae_environment_t environment = {.unlocked = true, .screen_visible = true}; app_audio_set_environment(&environment);
    ae_snapshot_t view = snapshot();
    app_audio_command_t request = {.kind = APP_AUDIO_CMD_VOLUME, .level = 35,
        .expected_generation = view.audio_generation, .expected_revision = view.revision};
    assert(command(&request).result == AE_OK); view = snapshot();
    request = (app_audio_command_t){.kind = APP_AUDIO_CMD_ROUTE, .route = AE_ROUTE_USB_UAC,
        .expected_generation = view.audio_generation, .expected_revision = view.revision};
    assert(command(&request).result == AE_OK); view = snapshot();
    request = (app_audio_command_t){.kind = APP_AUDIO_CMD_START, .start = {.request_id = {{1}}, .principal_id = {{2}}, .owner_session_id = {{3}},
        .expected_audio_generation = view.audio_generation, .expected_revision = view.revision, .kind = AE_PLAYBACK_STREAM,
        .route = AE_ROUTE_USB_UAC, .route_token = route_token, .format = pcm, .duration_seconds = 3600, .byte_limit = 100000000}};
    assert(command(&request).result == AE_OK); view = snapshot();
    pressure_frame = (ae_frame_t){.audio_generation = view.audio_generation, .format = pcm, .route_token = route_token, .bytes = 960};
    atomic_store(&verify_payload, true); atomic_store(&pressure, true); xTaskNotifyGive(&task);
    for (unsigned i = 0; i < 10000 && !atomic_load(&finished); ++i) pause_ms(1);
    assert(atomic_load(&finished));
    assert(app_audio_shutdown() == AE_AGAIN);
    assert(!pthread_join(task.thread, NULL));
    assert(!pthread_cond_destroy(&task.changed)); assert(!pthread_mutex_destroy(&task.lock));
    assert(!atomic_load(&allocations) && !notify_done);
    puts(privacy_case ? "PASS: safety stop cancels a reserved control before codec submission, preserves the old volume and reports privacy rejection" :
        "PASS: real audio worker retires live volume/gain under continuous asynchronous PCM, preserves every delivered sample and accepts targeted Stop");
    return 0;
}
