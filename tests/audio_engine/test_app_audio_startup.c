// SPDX-License-Identifier: Apache-2.0
/* Reuse the real threaded worker boundary, without running the streaming case. */
#define main stream_fixture_main
#include "test_app_audio.c"
#undef main

static void join_worker(void)
{
    assert(!pthread_join(task.thread, NULL));
    assert(!pthread_cond_destroy(&task.changed));
    assert(!pthread_mutex_destroy(&task.lock));
    assert(!atomic_load(&allocations) && !notify_done && !notify_context);
    /* Match a driver which copied the callback before unregister and invokes
     * it after both owner arenas and its task handle have been destroyed. */
    assert(retired_notify);
    unsigned notifications = task.notifications;
    retired_notify(retired_context);
    assert(task.notifications == notifications);
}

static void unknown_snapshot(void)
{
    ae_snapshot_t view;
    unsigned char before[sizeof(view)];
    memset(&view, 0xa5, sizeof(view));
    memcpy(before, &view, sizeof(view));
    assert(!app_audio_snapshot(&view));
    assert(!memcmp(before, &view, sizeof(view)));
    app_audio_result_t result;
    assert(!app_audio_take_result(&result));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    ae_dependencies_t dependencies = {.codec = {.stop = stop_codec, .completion_notify = completion}};
    app_audio_command_t request = {.kind = APP_AUDIO_CMD_VOLUME, .request_token = 1, .level = 30};
    unknown_snapshot();

    /* Reproduce the actual SDK boundary: an ordinary allocation is valid at
     * four bytes but rejected by the real opaque engine before codec I/O. */
    void *weak = heap_caps_calloc(1, ae_engine_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert((uintptr_t)weak % 8 == 4 && ae_engine_alignment() >= 8);
    ae_engine_t *engine = NULL;
    assert(ae_init(weak, ae_engine_bytes(), &dependencies, 1, &engine) == AE_INVALID);
    assert(!engine && !atomic_load(&stops));
    test_heap_free(weak);

    if (!strcmp(scenario, "state-allocation")) fail_allocation = 1;
    else if (!strcmp(scenario, "engine-allocation")) fail_allocation = 2;
    else if (!strcmp(scenario, "task-create")) fail_task_create = true;
    else if (!strcmp(scenario, "rejected-arena")) reject_engine_arena = true;
    else if (!strcmp(scenario, "codec-pending")) atomic_store(&stop_result, AE_AGAIN);
    else if (!strcmp(scenario, "codec-error")) atomic_store(&stop_result, AE_IO_ERROR);
    else if (!strcmp(scenario, "codec-invalid")) atomic_store(&stop_result, AE_INVALID);
    else assert(!strcmp(scenario, "aligned") || !strcmp(scenario, "replacement-startup"));

    if (!strcmp(scenario, "replacement-startup")) {
        assert(app_audio_init(&dependencies) == AE_OK);
        assert(snapshot().hardware_off_confirmed);
        assert(app_audio_shutdown() == AE_AGAIN);
        join_worker();
    }

    if (fail_allocation || fail_task_create) {
        assert(app_audio_init(&dependencies) == AE_EXHAUSTED);
        assert(!atomic_load(&allocations) && !atomic_load(&stops));
        unknown_snapshot();
        assert(app_audio_submit(&request) == AE_EXHAUSTED);
        assert(app_audio_shutdown() == AE_OK);
        fail_allocation = 0;
        fail_task_create = false;
    } else if (reject_engine_arena) {
        /* Fault-inject a violated allocation contract to exercise the real
         * asynchronous init rejection, callback teardown and retry lifecycle. */
        assert(app_audio_init(&dependencies) == AE_OK);
        join_worker();
        unknown_snapshot();
        assert(!atomic_load(&stops));
        assert(app_audio_submit(&request) == AE_INVALID);
        assert(app_audio_shutdown() == AE_OK);
        reject_engine_arena = false;
    }

    void (*previous_notify)(void *) = retired_notify;
    void *previous_context = retired_context;
    atomic_store(&hold_task_handle, previous_notify != NULL);
    atomic_store(&hold_worker, true);
    assert(app_audio_init(&dependencies) == AE_OK);
    if (previous_notify) {
        for (unsigned i = 0; i < 500 && !atomic_load(&task_handle_held); ++i) pause_ms(1);
        assert(atomic_load(&task_handle_held));
        /* Replacement state is published, but its task handle is not yet
         * available. A copied prior completion must neither dereference the
         * old owner nor notify an unpublished handle. */
        unsigned notifications = task.notifications;
        previous_notify(previous_context);
        assert(task.notifications == notifications);
        unknown_snapshot();
        atomic_store(&hold_task_handle, false);
    }
    for (unsigned i = 0; i < 500 && !atomic_load(&worker_held); ++i) pause_ms(1);
    assert(atomic_load(&worker_held));
    if (previous_notify) {
        unsigned notifications = task.notifications;
        previous_notify(previous_context);
        assert(task.notifications == notifications + 1);
    }
    ae_snapshot_t view = snapshot();
    ae_result_t expected = (ae_result_t)atomic_load(&stop_result);
    assert(atomic_load(&allocations) == 2);
    if (expected == AE_AGAIN) {
        assert(view.state == AE_STOPPING && !view.hardware_off_confirmed);
        assert(view.error == AE_OK && view.capture_indicator_required);
    } else if (expected != AE_OK) {
        /* The locked initial environment advances a faulted engine into its
         * hardware-stop retry while retaining the original error/quarantine. */
        assert(view.state == AE_STOPPING && view.error == expected);
        assert(view.quarantined && !view.hardware_off_confirmed && view.capture_indicator_required);
    } else assert(view.state == AE_OFF && view.error == AE_OK && view.hardware_off_confirmed);
    assert(app_audio_init(&dependencies) == AE_BUSY);
    /* A valid non-OK engine must survive until hardware stop is acknowledged. */
    atomic_store(&stop_result, AE_OK);
    assert(app_audio_shutdown() == AE_AGAIN);
    atomic_store(&hold_worker, false);
    join_worker();
    unknown_snapshot();
    assert(app_audio_shutdown() == AE_OK);
    printf("PASS: audio startup %s, weak heap rejection, truthful snapshots and complete ownership cleanup\n", scenario);
    return 0;
}
