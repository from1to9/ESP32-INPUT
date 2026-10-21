// SPDX-License-Identifier: Apache-2.0
/* Driver/store boundary models inject explicit acknowledged, pending and failed
 * outcomes. They do not stand in for storage, radio or acoustic qualification. */
#include "audio_engine.h"
#include "audio_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ae_format_t mono = {16000, 1, 20, AE_PCM_S16_LE};
static struct {
    uint64_t now, unix_seconds, id, generation;
    bool cancel, allowed, owner_alive, background, trusted_time;
    bool mic, speaker, fail_stop, pending_stop, partial_write, read_enabled;
    bool revoke_in_read, cancel_in_read, replace_in_read, expire_in_read;
    bool block_live, fail_append, fail_finish, corrupt_finish, delete_allowed;
    bool corrupt_store_owner, revoke_only, duplex, reject_reserve, verify_fragments;
    bool pending_mute, pending_unmute, unmute_cancelled, deny_recognition;
    ae_role_t held_role;
    bool pending_unbind;
    uint64_t bound_consumer;
    unsigned bind_calls, unbind_calls;
    unsigned store_read_fault;
    size_t reads, writes, sinks[3], cancellations[3], stops, prepares, actual_limit;
    uint64_t sink_samples[3];
    ae_route_token_t token;
    bool route_connected;
    ae_record_metadata_t metadata;
    bool object_present;
    uint8_t pcm[65536], wav_header[44], speaker_data[65536];
    size_t pcm_bytes, speaker_bytes;
} model;
static ae_engine_t *engine;
static void *memory;

static ae_result_t codec_caps(void *context, ae_codec_capabilities_t *caps)
{
    (void)context;
    *caps = (ae_codec_capabilities_t){.implemented = true, .available = true,
        .qualified = false, .full_duplex = model.duplex, .safe_volume_max = 80, .format_count = 1,
        .formats = {{.format = mono, .roles = AE_MICROPHONE | AE_SPEAKER}}};
    return AE_OK;
}
static ae_result_t prepare(void *context, const ae_format_t *format, uint8_t roles, uint64_t generation)
{
    (void)context; assert(ae_format_equal(format, &mono)); assert(roles == AE_MICROPHONE || roles == AE_SPEAKER || (model.duplex && roles == 3));
    ++model.prepares; model.generation = generation; model.mic = model.speaker = false; return AE_OK;
}
static ae_result_t mute(void *context, ae_role_t role, bool value, uint64_t generation)
{
    (void)context; assert(generation == model.generation);
    if (role == model.held_role) {
        if (value && model.pending_mute) return AE_AGAIN;
        if (!value && model.unmute_cancelled) { model.unmute_cancelled = false; return AE_STALE; }
        if (!value && model.pending_unmute) return AE_AGAIN;
        if (value && model.pending_unmute) model.unmute_cancelled = true;
    }
    if (role == AE_MICROPHONE) model.mic = !value; else model.speaker = !value;
    return AE_OK;
}
static ae_result_t level(void *context, uint8_t value, uint64_t generation)
{
    (void)context; (void)value; assert(generation == model.generation); return AE_OK;
}
static ae_result_t read_pcm(void *context, uint64_t generation, void *data, size_t capacity, size_t *actual)
{
    (void)context; assert(model.mic && generation == model.generation);
    *actual = 0;
    if (!model.read_enabled) return AE_AGAIN;
    size_t count = model.actual_limit && model.actual_limit < capacity ? model.actual_limit : capacity;
    memset(data, (int)(model.reads + 1), count); *actual = count; ++model.reads;
    if (model.revoke_in_read) model.allowed = false;
    if (model.cancel_in_read) model.cancel = true;
    if (model.replace_in_read) ++model.token.link_generation;
    if (model.expire_in_read) model.now += 2000;
    return AE_OK;
}
static ae_result_t write_pcm(void *context, uint64_t generation, const void *data, size_t bytes, size_t *actual)
{
    (void)context; assert(model.speaker && generation == model.generation);
    *actual = model.partial_write ? bytes / 2 : bytes;
    assert(model.speaker_bytes + *actual <= sizeof(model.speaker_data));
    memcpy(model.speaker_data + model.speaker_bytes, data, *actual);
    model.speaker_bytes += *actual; ++model.writes; return AE_OK;
}
static ae_result_t stop(void *context, uint64_t generation)
{
    (void)context; (void)generation; ++model.stops;
    if (model.fail_stop) return AE_IO_ERROR;
    if (model.pending_stop) return AE_AGAIN;
    model.mic = model.speaker = false; return AE_OK;
}
static ae_result_t authority(void *context, ae_authority_operation_t operation,
    const ae_start_request_t *request, uint64_t now, ae_authority_t *out)
{
    (void)context; (void)request; (void)now;
    *out = (ae_authority_t){.allowed = model.allowed && !(model.deny_recognition && request->kind == AE_CAPTURE_RECOGNITION) && (operation != AE_AUTH_RECORD_DELETE || model.delete_allowed),
        .owner_session_alive = model.owner_alive, .background_allowed = model.background,
        .source_valid_until_ms = UINT64_MAX - 1, .background_valid_until_ms = UINT64_MAX - 1,
        .reason = AE_DENIED};
    return AE_OK;
}
static ae_result_t random_id(void *context, ae_id_t *id)
{
    (void)context; memset(id, 0, sizeof(*id)); uint64_t value = ++model.id;
    memcpy(id->bytes, &value, sizeof(value)); return AE_OK;
}
static ae_result_t unix_time(void *context, uint64_t *seconds)
{
    (void)context; *seconds = model.unix_seconds; return model.trusted_time ? AE_OK : AE_TIME_UNKNOWN;
}
static uint64_t clock_now(void *context) { (void)context; return model.now; }
static bool cancelled(void *context) { (void)context; return model.cancel; }
static ae_result_t sink(void *context, ae_job_kind_t kind, const ae_id_t *job, const ae_frame_t *frame)
{
    (void)context; assert(!ae_id_empty(job)); assert(!model.cancel && model.allowed);
    if (model.verify_fragments) for (unsigned i = 0; i < frame->bytes; ++i)
        assert(frame->data[i] == (uint8_t)((frame->sequence - 1U) * 4U + i / 160U + 1U));
    if (model.block_live && kind == AE_CAPTURE_LIVE) return AE_AGAIN;
    assert(frame->bytes == 640); ++model.sinks[kind]; model.sink_samples[kind] += 320; return AE_OK;
}
static void cancel_sink(void *context, ae_job_kind_t kind, const ae_id_t *job, uint64_t generation)
{
    (void)context; assert(!ae_id_empty(job) && generation); ++model.cancellations[kind];
}
static ae_result_t route_caps(void *context, ae_route_t route, ae_route_capabilities_t *caps)
{
    (void)context; assert(route == AE_ROUTE_HFP_HF || route == AE_ROUTE_USB_UAC || route == AE_ROUTE_A2DP_SINK);
    *caps = (ae_route_capabilities_t){.implemented = true, .available = true, .connected = model.route_connected,
        .capture = route != AE_ROUTE_A2DP_SINK, .playback = true, .full_duplex=model.duplex, .format = mono, .token = model.token}; return AE_OK;
}
static ae_result_t route_prepare(void *context, ae_route_t route, const ae_format_t *format,
    uint8_t roles, uint64_t generation, const ae_route_token_t *token)
{
    (void)context; (void)route; (void)roles; assert(generation && ae_format_equal(format, &mono));
    return ae_route_token_equal(token, &model.token) ? AE_OK : AE_STALE;
}
static ae_result_t route_capture(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context; (void)route; assert(ae_route_token_equal(&frame->route_token, &model.token));
    ++model.sinks[AE_CAPTURE_LIVE]; return AE_OK;
}
static ae_result_t route_stop(void *context, ae_route_t route, uint64_t generation, const ae_route_token_t *token)
{
    (void)context; (void)route; (void)generation; (void)token; return AE_OK;
}
static ae_result_t bind_playback(void *context, ae_route_t route, uint64_t generation,
    const ae_route_token_t *token, uint64_t consumer, bool enabled)
{
    (void)context; (void)route; assert(generation && consumer && ae_route_token_equal(token,&model.token));
    if (enabled) { ++model.bind_calls; model.bound_consumer=consumer; return AE_OK; }
    ++model.unbind_calls;
    if (model.pending_unbind) return AE_AGAIN;
    if (model.bound_consumer==consumer) model.bound_consumer=0;
    return AE_OK;
}
static ae_result_t store_caps(void *context, ae_store_capabilities_t *caps)
{
    (void)context;
    *caps = (ae_store_capabilities_t){.storage = AE_STORAGE_NAND,
        .implemented = true, .available = true, .geometry_verified = true,
        .owned_and_provisioned = true, .encryption_ready = true, .authenticated_metadata = true,
        .recovery_complete = true, .retention_time_known = model.trusted_time,
        .usable_bytes = 64U * 1024U * 1024U, .quota_bytes = 32U * 1024U * 1024U,
        .object_limit = 32, .object_count = model.object_present, .per_object_overhead_bytes = 512}; return AE_OK;
}
static ae_result_t reserve_record(void *context, const ae_record_metadata_t *metadata)
{
    (void)context; if (model.reject_reserve) return AE_QUOTA; if (model.object_present) return AE_BUSY;
    model.metadata = *metadata; model.object_present = true; model.pcm_bytes = 0; return AE_OK;
}
static ae_result_t append_record(void *context, const ae_id_t *id, uint64_t offset,
    const void *data, size_t bytes, size_t *actual)
{
    (void)context; assert(model.object_present && ae_id_equal(id, &model.metadata.object_id));
    assert(offset == model.pcm_bytes && bytes <= sizeof(model.pcm) - model.pcm_bytes);
    *actual = model.fail_append ? bytes / 2 : bytes;
    memcpy(model.pcm + model.pcm_bytes, data, *actual); model.pcm_bytes += *actual;
    model.metadata.pcm_bytes = model.pcm_bytes;
    model.metadata.stored_bytes = model.pcm_bytes + 44 + 512;
    return model.fail_append ? AE_IO_ERROR : AE_OK;
}
static ae_result_t checkpoint(void *context, const ae_id_t *id, ae_record_metadata_t *evidence)
{
    (void)context; if (!model.object_present || !ae_id_equal(id, &model.metadata.object_id)) return AE_NOT_FOUND;
    model.metadata.verified_pcm_bytes = model.metadata.synced_pcm_bytes = model.pcm_bytes;
    model.metadata.integrity_verified = model.metadata.sync_acknowledged = true;
    *evidence = model.metadata; return AE_OK;
}
static ae_result_t finish_record(void *context, const ae_record_metadata_t *metadata,
    const uint8_t header[44], ae_record_metadata_t *evidence)
{
    (void)context; if (model.fail_finish) return AE_IO_ERROR;
    memcpy(model.wav_header, header, 44); model.metadata = *metadata;
    model.metadata.hash_available = true; model.metadata.sha256[0] = 1; /* Model evidence, not a cryptographic test. */
    model.metadata.integrity_verified = model.metadata.sync_acknowledged = true;
    *evidence = model.metadata;
    if (model.corrupt_finish) ++evidence->pcm_bytes;
    return AE_OK;
}
static ae_result_t get_record(void *context, const ae_id_t *id, ae_record_metadata_t *metadata)
{
    (void)context; if (!model.object_present || !ae_id_equal(id, &model.metadata.object_id)) return AE_NOT_FOUND;
    *metadata = model.metadata; return AE_OK;
}
static ae_result_t list_records(void *context, const ae_id_t *principal, uint32_t cursor,
    ae_record_metadata_t *items, size_t capacity, size_t *count, uint32_t *next)
{
    (void)context; assert(capacity); *count = 0; *next = 0;
    if (!cursor && model.object_present && ae_id_equal(principal, &model.metadata.principal_id)) {
        items[0] = model.metadata; *count = 1;
        if (model.corrupt_store_owner) ++items[0].principal_id.bytes[0];
    }
    return AE_OK;
}
static ae_result_t read_record(void *context, const ae_id_t *id, uint64_t offset,
    void *output, size_t capacity, size_t *actual)
{
    (void)context; if (!model.object_present || !ae_id_equal(id, &model.metadata.object_id)) return AE_NOT_FOUND;
    if (offset + capacity > 44 + model.pcm_bytes) return AE_INVALID;
    uint8_t *out = output;
    for (size_t i = 0; i < capacity; ++i)
        out[i] = offset + i < 44 ? model.wav_header[offset + i] : model.pcm[offset + i - 44];
    *actual = capacity;
    if (offset >= 44) {
        if (model.store_read_fault == 1) model.allowed = false;
        if (model.store_read_fault == 2) model.owner_alive = false;
        if (model.store_read_fault == 3) model.now += 20000;
    }
    return AE_OK;
}
static ae_result_t remove_record(void *context, const ae_id_t *id, uint64_t revision, bool *revoked, bool *complete)
{
    (void)context; if (!model.object_present || !ae_id_equal(id, &model.metadata.object_id)) return AE_NOT_FOUND;
    if (revision != model.metadata.revision) return AE_STALE;
    *revoked = model.metadata.access_revoked = true; *complete = !model.revoke_only;
    if (*complete) model.object_present = false;
    return *complete ? AE_OK : AE_AGAIN;
}

static ae_snapshot_t snapshot(void) { ae_snapshot_t value; ae_snapshot(engine, &value); return value; }
static void visible(void)
{
    ae_environment_t env = {.unlocked = true, .screen_visible = true, .indicator_visible = true,
        .indicator_generation = snapshot().audio_generation, .indicator_seen_ms = model.now};
    ae_environment(engine, &env, model.now);
}
static void step(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) { model.now += 20; visible(); (void)ae_poll(engine, model.now); }
}
static void setup(void)
{
    free(memory); memory = calloc(1, ae_engine_bytes()); assert(memory);
    memset(&model, 0, sizeof(model)); model.now = 1000; model.unix_seconds = 100000;
    model.allowed = model.owner_alive = model.background = model.trusted_time = model.read_enabled = model.delete_allowed = true;
    model.route_connected = true;
    model.token = (ae_route_token_t){.peer_id = {{7}}, .stream_id = {{8}}, .link_generation = 11};
    ae_dependencies_t dependencies = {
        .codec = {.capabilities = codec_caps, .prepare = prepare, .mute = mute, .volume = level,
            .gain = level, .read = read_pcm, .write = write_pcm, .stop = stop},
        .route = {.capabilities = route_caps, .prepare = route_prepare, .capture = route_capture, .stop = route_stop, .bind_playback=bind_playback},
        .store = {.capabilities = store_caps, .reserve = reserve_record, .append = append_record,
            .checkpoint = checkpoint, .finish = finish_record, .get = get_record, .list = list_records,
            .read = read_record, .remove = remove_record},
        .authority = authority, .random_id = random_id, .trusted_unix_time = unix_time,
        .monotonic_ms = clock_now, .safety_stop_requested = cancelled,
        .capture_sink = sink, .capture_cancel = cancel_sink, .fanout_enabled = true,
    };
    assert(ae_init(memory, ae_engine_bytes(), &dependencies, model.now, &engine) == AE_OK);
    ae_snapshot_t view = snapshot(); assert(view.state == AE_OFF && view.hardware_off_confirmed);
    assert(view.microphone_muted && view.speaker_muted && !model.mic && !model.speaker);
    assert(ae_route_select(engine, AE_ROUTE_LOCAL, view.audio_generation, view.revision, model.now) == AE_OK);
    visible();
}
static ae_start_request_t request(ae_job_kind_t kind)
{
    ae_snapshot_t view = snapshot();
    ae_start_request_t value = {.principal_id = {{1}}, .owner_session_id = {{2}}, .consent_id = {{3}},
        .consent_revision = 1, .expected_audio_generation = view.audio_generation, .expected_revision = view.revision,
        .kind = kind, .route = view.route, .format = mono, .duration_seconds = kind == AE_CAPTURE_RECOGNITION ? 8 : 10,
        .byte_limit = 65536, .retention_seconds = 3600, .export_policy = AE_EXPORT_OWNER_DOWNLOAD, .storage = AE_STORAGE_NAND,
        .activate_microphone = true};
    (void)random_id(NULL, &value.request_id);
    if (value.route != AE_ROUTE_LOCAL) value.route_token = model.token;
    return value;
}
static ae_job_snapshot_t start(ae_job_kind_t kind)
{
    ae_start_request_t value = request(kind); ae_job_snapshot_t job;
    assert(ae_start(engine, &value, model.now, &job) == AE_OK); return job;
}

static void lifecycle(void)
{
    setup(); ae_start_request_t value = request(AE_CAPTURE_LIVE); ae_job_snapshot_t job, duplicate;
    value.activate_microphone = false;
    assert(ae_start(engine, &value, model.now, &job) == AE_PRIVACY && model.prepares == 0);
    value.activate_microphone = true;
    assert(ae_start(engine, &value, model.now, &job) == AE_OK);
    assert(ae_start(engine, &value, model.now, &duplicate) == AE_OK && ae_id_equal(&job.job_id, &duplicate.job_id));
    ++value.duration_seconds;
    assert(ae_start(engine, &value, model.now, &duplicate) == AE_INVALID);
    assert(snapshot().capture_indicator_required && !model.mic);
    /* No render acknowledgement: preparation may occur, unmute may not. */
    for (unsigned i = 0; i < 6; ++i) { model.now += 20; (void)ae_poll(engine, model.now); }
    assert(model.prepares == 1 && !model.mic && model.reads == 0);
    step(6); assert(model.mic && model.sinks[0] > 0);
    size_t reads = model.reads;
    /* Unrelated HID ownership is absent from the audio environment. A normal
     * foreground/local-control refresh must not alter independent jobs. */
    visible(); step(2); assert(model.reads > reads && snapshot().capture[0].state == AE_JOB_RUNNING);
    assert(ae_stop(engine, &job.job_id, job.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK);
    assert(!snapshot().capture_gate_open); step(3);
    assert(snapshot().hardware_off_confirmed && !model.mic && !snapshot().capture_indicator_required);
    assert(ae_mute(engine, AE_MICROPHONE, false, model.now) == AE_DENIED);
    assert(ae_start(engine, &value, model.now, &duplicate) == AE_INVALID);
    puts("PASS: explicit start, render-confirmed indication, immutable retry, independent media ownership and no mute revival");
}

static void callback_races(void)
{
    for (unsigned cause = 0; cause < 4; ++cause) {
        setup(); (void)start(AE_CAPTURE_LIVE); step(4);
        if (cause == 0) model.cancel_in_read = true;
        if (cause == 1) model.revoke_in_read = true;
        if (cause == 2) model.expire_in_read = true;
        if (cause == 3) model.owner_alive = false;
        step(2);
        assert(model.sinks[0] == 0 && !snapshot().capture_gate_open);
    }
    setup(); ae_snapshot_t state = snapshot();
    assert(ae_route_select(engine, AE_ROUTE_HFP_HF, state.audio_generation, state.revision, model.now) == AE_OK);
    (void)start(AE_CAPTURE_LIVE); step(4); model.replace_in_read = true; step(2);
    assert(model.sinks[0] == 0 && !snapshot().capture_gate_open);
    puts("PASS: stop/revocation/deadline/owner loss during I/O and same-format link replacement cannot publish old samples");
}

static void safety_and_overflow(void)
{
    setup(); ae_job_snapshot_t live = start(AE_CAPTURE_LIVE); step(7);
    (void)start(AE_CAPTURE_RECOGNITION); model.block_live = true; step(12);
    ae_snapshot_t state = snapshot();
    assert(state.capture[0].state == AE_JOB_PARTIAL && state.capture[0].stop_reason == AE_STOP_OVERFLOW);
    assert(state.capture[2].state == AE_JOB_RUNNING && model.sinks[2] > 0 && model.mic);
    assert(ae_stop(engine, &live.job_id, live.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_TOO_LATE);
    model.fail_stop = true;
    ae_safety_stop(engine, AE_STOP_PRIVACY, model.now); step(8);
    state = snapshot(); assert(state.quarantined && !state.hardware_off_confirmed && state.capture_indicator_required);
    model.fail_stop = false; step(2);
    assert(snapshot().hardware_off_confirmed && snapshot().quarantined);
    assert(ae_recover(engine, model.now) == AE_OK && !snapshot().quarantined && !model.mic);
    setup(); (void)start(AE_CAPTURE_LIVE); step(6);
    model.now += AE_INDICATOR_MAX_AGE_MS + 1; (void)ae_poll(engine, model.now);
    assert(!snapshot().capture_gate_open && snapshot().capture[0].stop_reason == AE_STOP_INDICATOR);
    setup(); (void)start(AE_CAPTURE_LIVE); model.actual_limit = 160;
    for (unsigned i = 0; i < 20 && model.reads < 4; ++i) step(1);
    assert(model.reads == 4 && model.sinks[0] == 0); step(1); assert(model.sinks[0] == 1); /* Four genuine fragments, no fake full read. */
    model.now -= 1; (void)ae_poll(engine, model.now); assert(snapshot().quarantined);
    puts("PASS: independent fan-out overflow, persistent stop uncertainty, explicit recovery, stalled indicator and partial-frame assembly");
}

static ae_frame_t playback_frame(ae_job_snapshot_t job, uint64_t sequence)
{
    ae_frame_t frame = {.audio_generation = snapshot().audio_generation, .consumer_generation = job.consumer_generation,
        .sequence = sequence + AE_FIRST_SEQUENCE, .sample_offset = sequence * 320, .format = mono, .bytes = 640,
        .route_token = snapshot().route_token};
    memset(frame.data, 0x36, frame.bytes); return frame;
}
static void playback_checks(void)
{
    setup(); ae_job_snapshot_t job = start(AE_PLAYBACK_STREAM);
    ae_frame_t frame = playback_frame(job, 0);
    assert(ae_submit_playback(engine, &frame, model.now) == AE_OK);
    for (unsigned i = 0; i < 20 && !model.writes; ++i) step(1);
    assert(model.writes == 1 && snapshot().playback.state == AE_JOB_RUNNING);
    assert(ae_submit_playback(engine, &frame, model.now) == AE_STALE);
    model.now += 500; visible(); (void)ae_poll(engine, model.now);
    assert(snapshot().playback.stop_reason == AE_STOP_UNDERRUN && !snapshot().playback_gate_open);
    setup(); job = start(AE_PLAYBACK_STREAM); frame = playback_frame(job, 0);
    assert(ae_submit_playback(engine, &frame, model.now) == AE_OK); model.partial_write = true;
    for (unsigned i = 0; i < 20 && !model.writes; ++i) step(1);
    assert(snapshot().quarantined && snapshot().playback.state == AE_JOB_ERROR);
    setup(); job = start(AE_PLAYBACK_STREAM);
    for (unsigned i = 0; i <= AE_RING_FRAMES; ++i) {
        frame = playback_frame(job, i);
        assert(ae_submit_playback(engine, &frame, model.now) == (i < AE_RING_FRAMES ? AE_OK : AE_OVERFLOW));
    }
    assert(model.writes == 0 && !snapshot().playback_gate_open);
    puts("PASS: exact playback sequence, explicit underrun timeout, partial-write fault and bounded ingress overflow");
}

static void recording_checks(void)
{
    setup(); ae_job_snapshot_t job = start(AE_CAPTURE_RECORDING); step(9);
    assert(model.pcm_bytes > 0);
    size_t accepted = model.pcm_bytes;
    assert(ae_stop(engine, &job.job_id, job.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK);
    step(4); assert(snapshot().capture[1].state == AE_JOB_COMPLETED);
    ae_wav_info_t wav; assert(ae_wav_inspect(model.wav_header, accepted + 44, &wav) == AE_OK && wav.pcm_bytes == accepted);
    ae_id_t owner = {{1}}, stranger = {{9}};
    ae_record_metadata_t metadata;
    assert(ae_record_get(engine, &stranger, &job.recording_id, &metadata, model.now) == AE_DENIED);
    assert(ae_record_get(engine, &owner, &job.recording_id, &metadata, model.now) == AE_OK);
    uint8_t bytes[44]; size_t actual;
    assert(ae_record_read(engine, &owner, &job.recording_id, 0, bytes, sizeof(bytes), &actual, model.now) == AE_OK);
    assert(actual == sizeof(bytes) && memcmp(bytes, model.wav_header, sizeof(bytes)) == 0);
    ae_start_request_t play = request(AE_PLAYBACK_RECORDING); play.recording_id = job.recording_id;
    ae_job_snapshot_t playback; assert(ae_start(engine, &play, model.now, &playback) == AE_OK);
    bool revoked, complete;
    assert(ae_record_delete(engine, &owner, &job.recording_id, metadata.revision, &revoked, &complete, model.now) == AE_BUSY);
    step(16); assert(snapshot().playback.state == AE_JOB_COMPLETED && model.speaker_bytes == accepted);
    assert(memcmp(model.speaker_data, model.pcm, accepted) == 0);
    ae_record_metadata_t list[2]; size_t count; uint32_t next;
    assert(ae_record_list(engine, &owner, 0, list, 2, &count, &next, model.now) == AE_OK && count == 1);
    model.corrupt_store_owner = true;
    assert(ae_record_list(engine, &owner, 0, list, 2, &count, &next, model.now) == AE_INTEGRITY && count == 0);
    model.corrupt_store_owner = false; model.revoke_only = true;
    assert(ae_record_delete(engine, &owner, &job.recording_id, metadata.revision, &revoked, &complete, model.now) == AE_AGAIN);
    assert(revoked && !complete);
    assert(ae_record_read(engine, &owner, &job.recording_id, 0, bytes, sizeof(bytes), &actual, model.now) == AE_DENIED);
    model.revoke_only = false;
    assert(ae_record_delete(engine, &owner, &job.recording_id, metadata.revision, &revoked, &complete, model.now) == AE_OK);
    assert(!model.object_present && revoked && complete);

    setup(); (void)start(AE_CAPTURE_RECORDING); step(8); model.fail_append = true; step(5);
    assert(snapshot().capture[1].state == AE_JOB_PARTIAL && model.metadata.state == AE_RECORD_PARTIAL);
    assert(ae_record_read(engine, &owner, &model.metadata.object_id, 0, bytes, 44, &actual, model.now) == AE_DENIED);
    setup(); job = start(AE_CAPTURE_RECORDING); step(8); model.corrupt_finish = true;
    assert(ae_stop(engine, &job.job_id, job.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK);
    step(4); assert(snapshot().capture[1].cleanup_pending && snapshot().capture[1].state != AE_JOB_COMPLETED);
    model.now += AE_FINALIZE_TIMEOUT_MS; visible(); (void)ae_poll(engine, model.now);
    assert(snapshot().capture[1].state == AE_JOB_ERROR && snapshot().capture[1].cleanup_pending);
    setup(); model.trusted_time = false; ae_start_request_t value = request(AE_CAPTURE_RECORDING);
    assert(ae_start(engine, &value, model.now, &job) == AE_TIME_UNKNOWN && !model.object_present);
    puts("PASS: bounded recording prefix, WAV playback/list/delete, ACLs, revoke-before-reclaim, partial faults and no false finalization");
}

static void background_checks(void)
{
    setup(); ae_start_request_t value = request(AE_CAPTURE_RECORDING); ae_job_snapshot_t job;
    value.disconnect_policy = AE_DISCONNECT_CONTINUE_LOCAL;
    assert(ae_start(engine, &value, model.now, &job) == AE_DENIED);
    value.background_grant_id = value.consent_id; value.background_grant_revision = 1;
    assert(ae_start(engine, &value, model.now, &job) == AE_DENIED);
    value.background_grant_id.bytes[0] = 4;
    assert(ae_start(engine, &value, model.now, &job) == AE_OK); step(8);
    assert(snapshot().capture[1].device_owned);
    model.owner_alive = false; step(3);
    assert(snapshot().capture[1].state == AE_JOB_RUNNING && model.mic);
    model.background = false; step(4);
    assert(snapshot().capture[1].state == AE_JOB_PARTIAL && !model.mic);
    puts("PASS: distinct bounded background grant and explicit ownership transfer; revocation still stops capture");
}

static void duplex_file_fragments(void)
{
    setup(); ae_job_snapshot_t record = start(AE_CAPTURE_RECORDING); step(10);
    assert(ae_stop(engine, &record.job_id, record.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(4);
    model.reads = 0; model.actual_limit = 160; model.duplex = model.verify_fragments = true;
    (void)start(AE_CAPTURE_LIVE);
    ae_start_request_t playback = request(AE_PLAYBACK_RECORDING); playback.recording_id = record.recording_id;
    ae_job_snapshot_t player; assert(ae_start(engine, &playback, model.now, &player) == AE_OK); step(30);
    assert(model.sinks[0] > 0 && model.writes > 0 && model.mic);
    puts("PASS: partial microphone assembly stays separate from simultaneous recorded-playback reads");
}

static void uncertain_reconciliation(void)
{
    setup(); ae_job_snapshot_t job = start(AE_CAPTURE_RECORDING); step(9);
    model.corrupt_finish = true;
    assert(ae_stop(engine, &job.job_id, job.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(4);
    model.now += AE_FINALIZE_TIMEOUT_MS; visible(); (void)ae_poll(engine, model.now);
    assert(snapshot().capture[1].cleanup_pending);
    model.corrupt_finish = false;
    assert(ae_recover(engine, model.now) == AE_OK);
    assert(!snapshot().capture[1].cleanup_pending && snapshot().hardware_off_confirmed && !model.mic);
    puts("PASS: explicit recovery reconciles actual terminal metadata after uncertain finalization without resuming capture");
}

static void transport_binding_lifecycle(void)
{
    setup(); model.duplex=true;
    ae_snapshot_t view=snapshot();
    assert(ae_route_select(engine,AE_ROUTE_USB_UAC,view.audio_generation,view.revision,model.now)==AE_OK);
    (void)start(AE_CAPTURE_LIVE); step(8);
    ae_job_snapshot_t first=start(AE_PLAYBACK_STREAM); ae_frame_t frame=playback_frame(first,0);
    frame.route_token=model.token;
    assert(ae_submit_playback(engine,&frame,model.now)==AE_OK); step(4);
    assert(model.bound_consumer==first.consumer_generation && model.mic && model.speaker);
    model.pending_unbind=true; size_t received=model.sinks[0];
    assert(ae_stop(engine,&first.job_id,first.consumer_generation,AE_STOP_REQUESTED,model.now)==AE_OK); step(8);
    assert(!model.speaker && model.mic && model.sinks[0]>received && snapshot().playback.cleanup_pending);
    ae_start_request_t request2=request(AE_PLAYBACK_STREAM); ae_job_snapshot_t second;
    assert(ae_start(engine,&request2,model.now,&second)==AE_BUSY);
    model.pending_unbind=false; step(1);
    assert(!snapshot().playback.cleanup_pending && !model.bound_consumer);
    second=start(AE_PLAYBACK_STREAM); step(1);
    assert(model.bound_consumer==second.consumer_generation && second.consumer_generation!=first.consumer_generation);
    assert(ae_submit_playback(engine,&frame,model.now)==AE_STALE);
    frame=playback_frame(second,0); frame.route_token=model.token;
    assert(ae_submit_playback(engine,&frame,model.now)==AE_OK); step(2);
    assert(model.mic && model.speaker && model.bind_calls==2 && model.unbind_calls>=2);
    puts("PASS: exact transport consumer rebind, old callback rejection, physical mute before pending unbind and independent capture progress");
}

static void physical_mute_acknowledgement(void)
{
    setup(); model.duplex = true;
    (void)start(AE_CAPTURE_LIVE); step(8);
    ae_job_snapshot_t playback = start(AE_PLAYBACK_STREAM);
    ae_frame_t frame = playback_frame(playback, 0);
    assert(ae_submit_playback(engine, &frame, model.now) == AE_OK); step(4);
    ae_snapshot_t view = snapshot();
    assert(view.microphone_mute_confirmed && view.speaker_mute_confirmed);
    assert(!view.microphone_mute_applied && !view.speaker_mute_applied);
    model.held_role = AE_MICROPHONE; model.pending_mute = true;
    assert(ae_mute(engine, AE_MICROPHONE, true, model.now) == AE_OK); step(2);
    view = snapshot();
    assert(view.microphone_muted && view.microphone_mute_pending && !view.microphone_mute_confirmed);
    assert(!view.microphone_mute_applied && model.mic && model.speaker);
    assert(!view.capture_gate_open && view.capture_indicator_required && view.playback_gate_open);
    assert(view.speaker_mute_confirmed && !view.speaker_mute_pending);
    model.pending_mute = false; step(2); view = snapshot();
    assert(view.microphone_mute_applied && view.microphone_mute_confirmed && !view.microphone_mute_pending);
    assert(!model.mic && model.speaker && !view.capture_indicator_required);

    /* Revoking only recognition never stops an independent speaker job. */
    (void)start(AE_CAPTURE_RECOGNITION); step(3); model.deny_recognition = true; step(3);
    assert(snapshot().capture[2].stop_reason == AE_STOP_CONSENT && model.speaker && !model.mic);
    assert(snapshot().playback.state == AE_JOB_RUNNING);

    /* Retire an unmute cancelled by the independent safety lane before any
     * later codec I/O; it cannot reopen the logical gate or be abandoned. */
    model.pending_unmute = true;
    ae_job_snapshot_t capture = start(AE_CAPTURE_LIVE); step(1);
    assert(snapshot().microphone_mute_pending);
    assert(ae_stop(engine, &capture.job_id, capture.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK);
    step(2); view = snapshot();
    assert(!model.mic && model.speaker && view.microphone_mute_applied && view.microphone_mute_confirmed);
    assert(!view.microphone_mute_pending && !view.capture_gate_open);
    model.pending_unmute = false;
    (void)start(AE_CAPTURE_LIVE); step(2); model.held_role = AE_SPEAKER; model.pending_mute = true;
    assert(ae_mute(engine, AE_SPEAKER, true, model.now) == AE_OK); step(1); view = snapshot();
    assert(view.speaker_mute_pending && !view.speaker_mute_confirmed && model.speaker && model.mic);
    model.pending_mute = false; step(1); view = snapshot();
    assert(view.speaker_mute_confirmed && view.speaker_mute_applied && model.mic && !model.speaker);

    setup(); model.duplex=true; (void)start(AE_CAPTURE_LIVE); step(8);
    playback=start(AE_PLAYBACK_STREAM); frame=playback_frame(playback,0);
    assert(ae_submit_playback(engine,&frame,model.now)==AE_OK); step(3);
    model.held_role=AE_MICROPHONE; model.pending_mute=true;
    assert(ae_mute(engine,AE_MICROPHONE,true,model.now)==AE_OK); step(6);
    assert(snapshot().quarantined && snapshot().error==AE_TIMEOUT && !model.mic && !model.speaker);
    free(memory); memory = calloc(1, ae_engine_bytes()); assert(memory);
    memset(&model, 0, sizeof(model)); model.now = 1000; model.mic = model.speaker = model.pending_stop = true;
    ae_dependencies_t deps = {.codec = {.stop = stop}};
    assert(ae_init(memory, ae_engine_bytes(), &deps, model.now, &engine) == AE_AGAIN);
    view = snapshot();
    assert(view.microphone_muted && view.speaker_muted && view.capture_indicator_required);
    assert(!view.microphone_mute_confirmed && !view.speaker_mute_confirmed);
    assert(view.microphone_mute_pending && view.speaker_mute_pending);
    model.pending_stop = false; assert(ae_poll(engine, ++model.now) == AE_OK); view = snapshot();
    assert(view.microphone_mute_confirmed && view.speaker_mute_confirmed && view.hardware_off_confirmed);
    assert(!view.microphone_mute_pending && !view.speaker_mute_pending && !view.capture_indicator_required);
    setup(); (void)start(AE_CAPTURE_LIVE); step(8); model.read_enabled = false; step(30);
    assert(snapshot().capture[0].stop_reason == AE_STOP_IO && snapshot().capture[0].error == AE_TIMEOUT);
    assert(!model.mic && snapshot().microphone_mute_confirmed);
    puts("PASS: acknowledged per-role mute, asynchronous initial stop, cancelled unmute retirement, recognition-only revocation and stalled capture timeout");
}

static void duplex_and_storage_boundaries(void)
{
    setup(); assert(ae_volume(engine, 40, model.now) == AE_OK && snapshot().volume_percent == 40);
    model.duplex = true; ae_job_snapshot_t capture = start(AE_CAPTURE_LIVE); step(10);
    uint64_t generation = snapshot().audio_generation; size_t reads = model.reads;
    ae_job_snapshot_t playback = start(AE_PLAYBACK_STREAM);
    ae_frame_t frame = playback_frame(playback, 0);
    assert(ae_submit_playback(engine, &frame, model.now) == AE_OK); step(5);
    assert(model.mic && model.speaker && model.reads > reads && model.writes == 1 && snapshot().audio_generation == generation);
    assert(ae_stop(engine, &capture.job_id, capture.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(3);
    assert(!model.mic && model.speaker && snapshot().audio_generation == generation && !snapshot().capture_indicator_required);
    capture = start(AE_CAPTURE_LIVE); step(3);
    assert(model.mic && model.speaker);
    assert(ae_mute(engine, AE_MICROPHONE, true, model.now) == AE_OK); step(3);
    assert(!model.mic && model.speaker && snapshot().playback.state == AE_JOB_RUNNING);
    assert(ae_mute(engine, AE_MICROPHONE, false, model.now) == AE_DENIED);
    capture = start(AE_CAPTURE_LIVE); step(3);
    assert(ae_stop(engine, &playback.job_id, playback.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(3);
    assert(model.mic && !model.speaker && snapshot().audio_generation == generation);
    assert(ae_stop(engine, &capture.job_id, capture.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(3);
    assert(snapshot().hardware_off_confirmed);
    setup(); model.duplex = true;
    assert(ae_enable(engine, AE_MICROPHONE, true, &mono, model.now) == AE_OK);
    assert(!model.mic && snapshot().microphone_muted);
    assert(ae_enable(engine,AE_SPEAKER,true,&mono,model.now)==AE_OK);
    assert(snapshot().speaker_enabled && snapshot().speaker_muted && snapshot().speaker_mute_confirmed);
    size_t prepared = model.prepares; (void)start(AE_CAPTURE_LIVE); step(8);
    assert(model.mic && model.prepares == prepared);
    setup(); model.reject_reserve = true; (void)start(AE_CAPTURE_RECORDING); step(5);
    assert(!snapshot().capture[1].cleanup_pending && !model.object_present);
    model.reject_reserve = false; (void)start(AE_CAPTURE_RECORDING); step(9); assert(model.pcm_bytes);
    setup(); capture = start(AE_CAPTURE_RECORDING); step(9); (void)start(AE_CAPTURE_RECOGNITION); step(3);
    reads = model.sinks[2]; model.fail_finish = true;
    assert(ae_stop(engine, &capture.job_id, capture.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(8);
    assert(snapshot().capture[1].cleanup_pending && model.sinks[2] > reads && model.mic);
    for (unsigned reason = 1; reason <= 3; ++reason) {
        setup(); capture = start(AE_CAPTURE_RECORDING); step(9);
        assert(ae_stop(engine, &capture.job_id, capture.consumer_generation, AE_STOP_REQUESTED, model.now) == AE_OK); step(4);
        ae_start_request_t value = request(AE_PLAYBACK_RECORDING); value.recording_id = capture.recording_id;
        assert(ae_start(engine, &value, model.now, &playback) == AE_OK);
        model.store_read_fault = reason; step(10);
        assert(model.writes == 0 && snapshot().playback.state == AE_JOB_ERROR);
    }
    setup(); ae_start_request_t long_call = request(AE_CAPTURE_LIVE); long_call.duration_seconds = 8U * 3600U;
    long_call.byte_limit = UINT64_C(8) * 1024U * 1024U * 1024U;
    assert(ae_start(engine, &long_call, model.now, &capture) == AE_OK);
    puts("PASS: same-session duplex independent stops, off-state volume, eight-hour bounded admission, reservation rejection, independent finalization and post-storage revocation");
}

int main(void)
{
    lifecycle(); callback_races(); safety_and_overflow(); playback_checks(); recording_checks(); background_checks(); duplex_and_storage_boundaries(); uncertain_reconciliation(); duplex_file_fragments(); physical_mute_acknowledgement(); transport_binding_lifecycle();
    printf("Resource bytes: engine=%zu (caller-provided external memory)\n", ae_engine_bytes());
    free(memory); return 0;
}
