// SPDX-License-Identifier: Apache-2.0
#define main store_fixture_main
#define memory store_memory
#include "test_store.c"
#undef memory
#undef main
#define main engine_fixture_main
#include "test_engine.c"
#undef main
#include "audio_internal.h"

static void progress(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; ++i) {
        ++model.now; visible();
        /* A hardware sample clock admits one exact frame every 20 ms. */
        model.read_enabled = model.now % 20 == 0;
        (void)ae_poll(engine, model.now); (void)ae_store_service(store);
    }
}
int main(void)
{
    fresh(true); setup(); engine->dependencies.store = port;
    ae_start_request_t record = request(AE_CAPTURE_RECORDING);
    record.storage = AE_STORAGE_RECORDS_PARTITION; record.byte_limit = 256U * 1024U;
    ae_job_snapshot_t job;
    assert(ae_start(engine, &record, model.now, &job) == AE_OK); progress(2400);
    ae_snapshot_t live = snapshot(); fprintf(stderr, "composition state=%d error=%d stop=%d received=%llu delivered=%llu store=%d\n", live.capture[1].state, live.capture[1].error, live.capture[1].stop_reason, (unsigned long long)live.capture[1].received_bytes, (unsigned long long)live.capture[1].delivered_bytes, ae_store_status(store).error); assert(live.capture[1].state == AE_JOB_RUNNING && live.capture[1].delivered_bytes > 60000);
    assert(ae_stop(engine, &job.job_id, job.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK);
    for (unsigned i = 0; i < 10000 && snapshot().capture[1].cleanup_pending; ++i) progress(1);
    assert(snapshot().capture[1].state == AE_JOB_COMPLETED && !snapshot().capture[1].cleanup_pending);
    ae_record_metadata_t meta; assert(ae_record_get(engine, &record.principal_id, &job.recording_id, &meta, model.now) == AE_OK);
    assert(meta.pcm_bytes == live.capture[1].delivered_bytes && meta.hash_available && meta.integrity_verified);
    uint8_t wav[8192]; size_t actual = 0;
    ae_result_t result = AE_AGAIN;
    for (unsigned i = 0; i < 500 && result == AE_AGAIN; ++i) {
        result = ae_record_read(engine, &record.principal_id, &job.recording_id, 0, wav, sizeof(wav), &actual, model.now);
        progress(1);
    }
    assert(result == AE_OK && actual == sizeof(wav) && !memcmp(wav, "RIFF", 4));
    /* Actual encrypted file becomes the local playback source. */
    ae_start_request_t playback = request(AE_PLAYBACK_RECORDING); playback.recording_id = job.recording_id; playback.byte_limit = 256U * 1024U;
    ae_job_snapshot_t player;
    result = AE_AGAIN;
    for (unsigned i = 0; i < 500 && result == AE_AGAIN; ++i) {
        result = ae_start(engine, &playback, model.now, &player); progress(1);
    }
    assert(result == AE_OK);
    /* Stop before the synthetic speaker's small fixture buffer can overflow. */
    for (unsigned i = 0; i < 500 && !model.writes; ++i) progress(1);
    assert(model.writes && snapshot().playback.state == AE_JOB_RUNNING);
    assert(ae_stop(engine, &player.job_id, player.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); progress(4);
    bool revoked = false, complete = false;
    result = AE_AGAIN;
    for (unsigned i = 0; i < 500 && result == AE_AGAIN; ++i) {
        result = ae_record_delete(engine, &record.principal_id, &job.recording_id, meta.revision, &revoked, &complete, model.now);
        progress(1);
    }
    assert(result == AE_OK && revoked && complete);
    assert(ae_record_get(engine, &record.principal_id, &job.recording_id, &meta, model.now) == AE_DENIED);
    reboot(); mount(); assert(ae_store_status(store).mounted && ae_store_status(store).objects == 0);
    ae_store_deinit(store); free(store_memory); free(memory);
    puts("PASS: actual engine plus encrypted async store: two checkpoints, durable finish, download, recorded playback, delete acknowledgement and reboot");
    return 0;
}
