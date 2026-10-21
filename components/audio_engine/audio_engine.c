// SPDX-License-Identifier: Apache-2.0
#include "audio_internal.h"
#include <string.h>

static uint64_t deadline(uint64_t now, uint64_t duration)
{
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static bool bump(uint64_t *value)
{
    if (*value == UINT64_MAX) return false;
    ++*value;
    return true;
}

bool ae_job_active(const ae_job_t *job)
{
    return job->view.state == AE_JOB_STARTING || job->view.state == AE_JOB_RUNNING;
}

bool ae_jobs_active(const ae_engine_t *engine)
{
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i)
        if (ae_job_active(&engine->capture[i])) return true;
    return ae_job_active(&engine->playback);
}

static unsigned capture_count(const ae_engine_t *engine)
{
    unsigned count = 0;
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i)
        if (ae_job_active(&engine->capture[i])) ++count;
    return count;
}

bool ae_cancel_latched(const ae_engine_t *engine)
{
    return engine->dependencies.safety_stop_requested &&
        engine->dependencies.safety_stop_requested(engine->dependencies.policy_context);
}

bool ae_privacy_ready(const ae_engine_t *engine, uint64_t now_ms)
{
    const ae_environment_t *state = &engine->environment;
    return state->unlocked && !state->privacy_latched && !state->maintenance && state->screen_visible &&
        state->indicator_visible && state->indicator_generation == engine->view.audio_generation &&
        state->indicator_seen_ms <= now_ms && now_ms - state->indicator_seen_ms <= AE_INDICATOR_MAX_AGE_MS &&
        !ae_cancel_latched(engine);
}

static void mute_ack(ae_engine_t *engine, uint8_t roles, bool muted)
{
    engine->mute_known_roles |= roles;
    if (roles & AE_MICROPHONE) engine->view.microphone_mute_applied = muted;
    if (roles & AE_SPEAKER) engine->view.speaker_mute_applied = muted;
}

void ae_refresh_view(ae_engine_t *engine)
{
    uint8_t pending = engine->closing_roles | engine->unmute_pending;
    if (engine->codec_stop_pending || engine->control_pending == 3) pending |= AE_MICROPHONE | AE_SPEAKER;
    engine->view.microphone_mute_pending = (pending & AE_MICROPHONE) != 0;
    engine->view.speaker_mute_pending = (pending & AE_SPEAKER) != 0;
    engine->view.microphone_mute_confirmed = (engine->mute_known_roles & AE_MICROPHONE) &&
        !engine->view.microphone_mute_pending && engine->view.microphone_mute_applied == engine->view.microphone_muted;
    engine->view.speaker_mute_confirmed = (engine->mute_known_roles & AE_SPEAKER) &&
        !engine->view.speaker_mute_pending && engine->view.speaker_mute_applied == engine->view.speaker_muted;
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) engine->view.capture[i] = engine->capture[i].view;
    engine->view.playback = engine->playback.view;
    engine->view.capture_indicator_required = engine->microphone_may_be_active || capture_count(engine) != 0;
    engine->view.capture_gate_open = engine->codec_running && capture_count(engine) != 0 &&
        !engine->view.microphone_muted && !ae_cancel_latched(engine);
    engine->view.playback_gate_open = engine->codec_running && ae_job_active(&engine->playback) &&
        !engine->view.speaker_muted && !ae_cancel_latched(engine);
}

void ae_snapshot(const ae_engine_t *engine, ae_snapshot_t *output)
{
    if (!engine || !output) return;
    *output = engine->view;
}

void ae_request_hardware_stop(ae_engine_t *engine, uint64_t now_ms)
{
    if (engine->codec_stop_pending || engine->route_stop_pending) return;
    engine->stop_generation = engine->view.audio_generation;
    engine->stop_route_token = engine->view.route_token;
    if (!bump(&engine->view.audio_generation)) {
        engine->view.quarantined = true;
        engine->view.error = AE_EXHAUSTED;
    }
    (void)bump(&engine->view.revision);
    engine->stop_started_ms = now_ms;
    engine->codec_stop_pending = true;
    engine->route_stop_pending = engine->view.route != AE_ROUTE_LOCAL && engine->view.route != AE_ROUTE_OFF;
    engine->codec_running = false;
    engine->view.state = AE_STOPPING;
    engine->view.microphone_muted = engine->view.speaker_muted = true;
    engine->view.capture_gate_open = engine->view.playback_gate_open = false;
    ae_secure_zero(&engine->work_frame, sizeof(engine->work_frame));
    engine->capture_fill = 0;
    ae_refresh_view(engine);
}

void ae_end_job(ae_engine_t *engine, ae_job_t *job, ae_stop_reason_t reason,
    ae_result_t error, uint64_t now_ms)
{
    if (!ae_job_active(job)) return;
    if (job->request.kind == AE_PLAYBACK_RECORDING && engine->dependencies.store.cancel_read)
        engine->dependencies.store.cancel_read(engine->dependencies.store.context, &job->request.recording_id);
    job->view.stop_reason = reason;
    job->view.error = error;
    job->view.discarded_frames += job->ring.count;
    ae_ring_reset(&job->ring);
    uint64_t old_generation = job->view.consumer_generation;
    if (!bump(&job->view.consumer_generation)) engine->view.quarantined = true;
    if (job->request.kind < AE_CAPTURE_RECORDING || job->request.kind == AE_CAPTURE_RECOGNITION) {
        if (engine->dependencies.capture_cancel)
            engine->dependencies.capture_cancel(engine->dependencies.policy_context,
                job->request.kind, &job->view.job_id, old_generation);
    }
    if (job->request.kind == AE_CAPTURE_RECORDING && (job->reserved || job->reservation_pending)) {
        job->view.state = AE_JOB_FINALIZING;
        job->view.cleanup_pending = true;
        job->finalize_deadline_ms = deadline(now_ms, AE_FINALIZE_TIMEOUT_MS);
        job->stage = 0;
        job->discard = reason == AE_STOP_CANCEL_DISCARD;
    } else {
        job->view.state = reason == AE_STOP_REQUESTED && job->view.first_frame_seen ?
            AE_JOB_COMPLETED : job->view.first_frame_seen ? AE_JOB_PARTIAL : AE_JOB_ERROR;
    }
    if (job->request.kind == AE_PLAYBACK_STREAM && job->route_binding_started) job->view.cleanup_pending = true;
    if (!ae_jobs_active(engine)) ae_request_hardware_stop(engine, now_ms);
    else if (engine->codec_running) {
        if (!capture_count(engine) && ((engine->active_roles | engine->unmute_pending) & AE_MICROPHONE)) {
            if (!(engine->closing_roles&AE_MICROPHONE)) engine->closing_started_ms[0]=now_ms;
            engine->closing_roles |= AE_MICROPHONE; engine->view.microphone_muted = true;
        }
        if (!ae_job_active(&engine->playback) && ((engine->active_roles | engine->unmute_pending) & AE_SPEAKER)) {
            if (!(engine->closing_roles&AE_SPEAKER)) engine->closing_started_ms[1]=now_ms;
            engine->closing_roles |= AE_SPEAKER; engine->view.speaker_muted = true;
        }
    }
    (void)bump(&engine->view.revision);
    ae_refresh_view(engine);
}

void ae_safety_stop(ae_engine_t *engine, ae_stop_reason_t reason, uint64_t now_ms)
{
    if (!engine) return;
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i)
        ae_end_job(engine, &engine->capture[i], reason, AE_PRIVACY, now_ms);
    ae_end_job(engine, &engine->playback, reason, AE_PRIVACY, now_ms);
    if (engine->codec_prepared || engine->microphone_may_be_active || !engine->view.hardware_off_confirmed)
        ae_request_hardware_stop(engine, now_ms);
    ae_refresh_view(engine);
}

bool ae_clock(ae_engine_t *engine, uint64_t now_ms)
{
    if (engine->clock_fault || now_ms < engine->last_ms) {
        engine->clock_fault = true;
        ae_safety_stop(engine, AE_STOP_TIME, now_ms);
        engine->view.quarantined = true;
        engine->view.error = AE_TIME_UNKNOWN;
        return false;
    }
    engine->last_ms = now_ms;
    return true;
}

static ae_result_t codec_format(ae_engine_t *engine, const ae_format_t *format, ae_role_t role)
{
    size_t bytes; uint32_t samples;
    if (ae_format_size(format, &bytes, &samples) != AE_OK) return AE_INVALID;
    if (!engine->dependencies.codec.capabilities) return AE_UNAVAILABLE;
    ae_codec_capabilities_t caps = {0};
    ae_result_t result = engine->dependencies.codec.capabilities(engine->dependencies.codec.context, &caps);
    if (result != AE_OK) return result;
    if (!caps.implemented || !caps.available) return AE_UNAVAILABLE;
    if (caps.format_count > AE_FORMATS_MAX || caps.safe_volume_max > 100) return AE_INVALID;
    engine->codec_capabilities = caps;
    for (unsigned i = 0; i < caps.format_count; ++i)
        if ((caps.formats[i].roles & role) && ae_format_equal(&caps.formats[i].format, format)) return AE_OK;
    return AE_NOT_SUPPORTED;
}

static ae_result_t route_check(ae_engine_t *engine, const ae_start_request_t *request)
{
    if (request->route == AE_ROUTE_LOCAL)
        return ae_route_token_valid(&request->route_token, true) ? AE_OK : AE_INVALID;
    if (request->route <= AE_ROUTE_OFF || request->route >= AE_ROUTE_COUNT ||
        !ae_route_token_valid(&request->route_token, false) || !engine->dependencies.route.capabilities)
        return AE_UNAVAILABLE;
    ae_route_capabilities_t caps = {0};
    ae_result_t result = engine->dependencies.route.capabilities(engine->dependencies.route.context,
        request->route, &caps);
    if (result != AE_OK) return result;
    if (!caps.implemented || !caps.available || !caps.connected) return AE_UNAVAILABLE;
    if (!ae_route_token_equal(&caps.token, &request->route_token)) return AE_STALE;
    if (!ae_format_equal(&caps.format, &request->format)) return AE_NOT_SUPPORTED;
    if (request->kind <= AE_CAPTURE_RECOGNITION && !caps.capture) return AE_NOT_SUPPORTED;
    if (request->kind >= AE_PLAYBACK_STREAM && !caps.playback) return AE_NOT_SUPPORTED;
    return AE_OK;
}

ae_result_t ae_authorize(ae_engine_t *engine, ae_job_t *job, ae_authority_operation_t operation,
    uint64_t now_ms)
{
    if (!engine->dependencies.authority) return AE_DENIED;
    ae_authority_t authority = {0};
    ae_result_t result = engine->dependencies.authority(engine->dependencies.policy_context,
        operation, &job->request, now_ms, &authority);
    if (result != AE_OK) return result;
    if (!authority.allowed) return authority.reason == AE_OK ? AE_DENIED : authority.reason;
    if (operation != AE_AUTH_START && operation != AE_AUTH_RUNNING) return AE_OK;
    bool capture = job->request.kind <= AE_CAPTURE_RECOGNITION;
    if (capture && authority.source_valid_until_ms <= now_ms) return AE_DENIED;
    bool background = job->request.disconnect_policy == AE_DISCONNECT_CONTINUE_LOCAL;
    if (background && (!authority.background_allowed || authority.background_valid_until_ms <= now_ms))
        return AE_DENIED;
    if (!authority.owner_session_alive && !(background && job->view.device_owned)) return AE_UNAVAILABLE;
    return AE_OK;
}

size_t ae_engine_bytes(void) { return sizeof(ae_engine_t); }

ae_result_t ae_init(void *memory, size_t bytes, const ae_dependencies_t *dependencies,
    uint64_t now_ms, ae_engine_t **output)
{
    if (!memory || bytes < sizeof(ae_engine_t) || !dependencies || !output ||
        (uintptr_t)memory % _Alignof(ae_engine_t)) return AE_INVALID;
    ae_engine_t *engine = memory;
    memset(engine, 0, sizeof(*engine));
    engine->dependencies = *dependencies;
    engine->last_ms = now_ms;
    engine->view.audio_generation = engine->view.revision = 1;
    engine->next_consumer_generation = 1;
    engine->view.microphone_muted = engine->view.speaker_muted = true;
    engine->view.volume_percent = 30;
    engine->view.microphone_gain_db = 18;
    engine->view.state = AE_OFF;
    engine->initialized = true;
    *output = engine;
    engine->microphone_may_be_active = true;
    if (!dependencies->codec.stop) {
        engine->view.error = AE_UNAVAILABLE;
        ae_refresh_view(engine);
        return AE_UNAVAILABLE;
    }
    ae_result_t result = dependencies->codec.stop(dependencies->codec.context, 1);
    engine->view.hardware_off_confirmed = result == AE_OK;
    if (result == AE_OK) { mute_ack(engine, AE_MICROPHONE | AE_SPEAKER, true); engine->microphone_may_be_active = false; }
    if (result == AE_AGAIN) {
        engine->codec_stop_pending = true;
        engine->stop_generation = 1;
        engine->stop_started_ms = now_ms;
        engine->view.state = AE_STOPPING;
    } else if (result != AE_OK) {
        engine->view.error = result;
        engine->view.quarantined = true;
        engine->view.state = AE_FAULT;
    }
    ae_refresh_view(engine);
    return result;
}

void ae_environment(ae_engine_t *engine, const ae_environment_t *environment, uint64_t now_ms)
{
    if (!engine || !environment) return;
    engine->environment = *environment;
    if (!ae_clock(engine, now_ms)) return;
    if (!environment->unlocked || environment->privacy_latched || environment->maintenance ||
        (!environment->screen_visible && capture_count(engine)))
        ae_safety_stop(engine, environment->maintenance ? AE_STOP_MAINTENANCE : AE_STOP_PRIVACY, now_ms);
    else if (engine->microphone_may_be_active && capture_count(engine) && !ae_privacy_ready(engine, now_ms))
        ae_safety_stop(engine, AE_STOP_INDICATOR, now_ms);
}

ae_result_t ae_route_select(ae_engine_t *engine, ae_route_t route,
    uint64_t generation, uint64_t revision, uint64_t now_ms)
{
    if (!engine || route < AE_ROUTE_OFF || route >= AE_ROUTE_COUNT) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    if (generation != engine->view.audio_generation || revision != engine->view.revision) return AE_STALE;
    if (ae_jobs_active(engine) || engine->codec_prepared || engine->codec_stop_pending || engine->route_stop_pending)
        return AE_BUSY;
    if (engine->view.quarantined) return AE_UNCERTAIN;
    if (route == engine->view.route) return AE_OK;
    if (route != AE_ROUTE_OFF && route != AE_ROUTE_LOCAL && !engine->dependencies.route.capabilities)
        return AE_UNAVAILABLE;
    if (!bump(&engine->view.audio_generation) || !bump(&engine->view.revision)) return AE_EXHAUSTED;
    engine->view.route = route;
    engine->view.route_token = (ae_route_token_t){0};
    return AE_OK;
}

static bool same_request(const ae_start_request_t *a, const ae_start_request_t *b)
{
    return ae_id_equal(&a->request_id, &b->request_id) && ae_id_equal(&a->principal_id, &b->principal_id) &&
        ae_id_equal(&a->owner_session_id, &b->owner_session_id) && ae_id_equal(&a->consent_id, &b->consent_id) &&
        ae_id_equal(&a->background_grant_id, &b->background_grant_id) && ae_id_equal(&a->recording_id, &b->recording_id) &&
        a->consent_revision == b->consent_revision && a->background_grant_revision == b->background_grant_revision &&
        a->expected_audio_generation == b->expected_audio_generation && a->expected_revision == b->expected_revision &&
        a->kind == b->kind && a->route == b->route && ae_route_token_equal(&a->route_token, &b->route_token) &&
        ae_format_equal(&a->format, &b->format) && a->duration_seconds == b->duration_seconds &&
        a->byte_limit == b->byte_limit && a->retention_seconds == b->retention_seconds &&
        a->export_policy == b->export_policy && a->storage == b->storage && a->disconnect_policy == b->disconnect_policy &&
        a->activate_microphone == b->activate_microphone;
}

static ae_result_t new_id(ae_engine_t *engine, ae_id_t *id)
{
    if (!engine->dependencies.random_id) return AE_UNAVAILABLE;
    ae_result_t result = engine->dependencies.random_id(engine->dependencies.policy_context, id);
    if (result != AE_OK) return result;
    return ae_id_empty(id) ? AE_INTEGRITY : AE_OK;
}

ae_result_t ae_start(ae_engine_t *engine, const ae_start_request_t *request,
    uint64_t now_ms, ae_job_snapshot_t *output)
{
    if (!engine || !request || !output || request->kind < AE_CAPTURE_LIVE || request->kind > AE_PLAYBACK_RECORDING ||
        ae_id_empty(&request->request_id) || ae_id_empty(&request->principal_id) ||
        ae_id_empty(&request->owner_session_id)) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    ae_job_t *job = request->kind <= AE_CAPTURE_RECOGNITION ? &engine->capture[request->kind] : &engine->playback;
    if (job->view.state != AE_JOB_EMPTY && ae_id_equal(&request->request_id, &job->request.request_id)) {
        if (!same_request(request, &job->request)) return AE_INVALID;
        *output = job->view;
        return AE_OK;
    }
    if (request->expected_audio_generation != engine->view.audio_generation ||
        request->expected_revision != engine->view.revision) return AE_STALE;
    if (engine->view.quarantined || engine->codec_stop_pending || engine->route_stop_pending) return AE_UNCERTAIN;
    if (!engine->dependencies.monotonic_ms) return AE_UNAVAILABLE;
    if (request->route != engine->view.route || request->route == AE_ROUTE_OFF) return AE_INVALID;
    if (ae_job_active(job) || job->view.cleanup_pending || engine->control_pending) return AE_BUSY;
    const bool capture = request->kind <= AE_CAPTURE_RECOGNITION;
    if (!engine->environment.unlocked || engine->environment.privacy_latched || engine->environment.maintenance ||
        ae_cancel_latched(engine) || (capture && !engine->environment.screen_visible)) return AE_PRIVACY;
    if (ae_jobs_active(engine) && !(engine->source_roles & (capture ? AE_MICROPHONE : AE_SPEAKER))) return AE_NOT_SUPPORTED;
    if (capture_count(engine) && capture && !engine->dependencies.fanout_enabled) return AE_NOT_SUPPORTED;
    if (ae_jobs_active(engine) && (!ae_format_equal(&request->format, &engine->view.format) ||
        !ae_route_token_equal(&request->route_token, &engine->view.route_token))) return AE_BUSY;
    if (capture && !request->activate_microphone && (!capture_count(engine) || engine->view.microphone_muted)) return AE_PRIVACY;
    if (capture && (ae_id_empty(&request->consent_id) || !request->consent_revision)) return AE_DENIED;
    if ((request->kind == AE_CAPTURE_RECORDING || request->kind == AE_CAPTURE_RECOGNITION) &&
        request->route != AE_ROUTE_LOCAL) return AE_NOT_SUPPORTED;
    if (request->disconnect_policy != AE_DISCONNECT_STOP &&
        (request->disconnect_policy != AE_DISCONNECT_CONTINUE_LOCAL || request->kind != AE_CAPTURE_RECORDING ||
         ae_id_empty(&request->background_grant_id) || !request->background_grant_revision ||
         ae_id_equal(&request->background_grant_id, &request->consent_id))) return AE_DENIED;
    uint32_t duration_max = request->kind == AE_CAPTURE_RECORDING ? 1800 :
        request->kind == AE_CAPTURE_RECOGNITION ? 8 : request->kind == AE_PLAYBACK_RECORDING ? 3600 : 8U * 3600U;
    uint64_t byte_min = request->kind == AE_CAPTURE_RECORDING ? 65536 : 1024;
    uint64_t byte_max = request->kind == AE_CAPTURE_RECORDING ? UINT64_C(32) * 1024U * 1024U : UINT64_C(8) * 1024U * 1024U * 1024U;
    if (!request->duration_seconds || request->duration_seconds > duration_max ||
        request->byte_limit < byte_min || request->byte_limit > byte_max ||
        now_ms > UINT64_MAX - (uint64_t)request->duration_seconds * 1000U - AE_START_TIMEOUT_MS) return AE_INVALID;
    ae_result_t result = codec_format(engine, &request->format, capture ? AE_MICROPHONE : AE_SPEAKER);
    if (result != AE_OK) return result;
    result = route_check(engine, request);
    if (result != AE_OK) return result;
    if (capture && request->kind != AE_CAPTURE_RECORDING && request->route == AE_ROUTE_LOCAL &&
        (!engine->dependencies.capture_sink || !engine->dependencies.capture_cancel)) return AE_UNAVAILABLE;
    ae_job_t candidate = {.request = *request};
    result = ae_authorize(engine, &candidate, AE_AUTH_START, now_ms);
    if (result != AE_OK) return result;
    if (request->kind == AE_CAPTURE_RECORDING) {
        result = ae_record_admit(engine, &candidate, now_ms);
        if (result != AE_OK) return result;
    }
    if (request->kind == AE_PLAYBACK_RECORDING) {
        result = ae_record_source(engine, &candidate, now_ms);
        if (result != AE_OK) return result;
    }
    if (new_id(engine, &candidate.view.job_id) != AE_OK ||
        new_id(engine, &candidate.view.audio_session_id) != AE_OK ||
        ae_id_equal(&candidate.view.job_id, &candidate.view.audio_session_id)) return AE_INTEGRITY;
    if (request->kind == AE_CAPTURE_RECORDING) {
        result = new_id(engine, &candidate.metadata.object_id);
        if (result != AE_OK || ae_id_equal(&candidate.metadata.object_id, &candidate.view.job_id) ||
            ae_id_equal(&candidate.metadata.object_id, &candidate.view.audio_session_id)) return AE_INTEGRITY;
        candidate.view.recording_id = candidate.metadata.object_id;
    } else candidate.view.recording_id = request->recording_id;
    if (engine->next_consumer_generation == UINT64_MAX || engine->view.revision == UINT64_MAX ||
        engine->view.audio_generation == UINT64_MAX) return AE_EXHAUSTED;
    if (!ae_jobs_active(engine)) {
        bool prepared = engine->codec_prepared;
        if (prepared && (!ae_format_equal(&request->format, &engine->view.format) ||
            !(engine->source_roles & (capture ? AE_MICROPHONE : AE_SPEAKER)))) return AE_BUSY;
        if (!prepared) ++engine->view.audio_generation;
        engine->view.format = request->format;
        engine->view.route_token = request->route_token;
        engine->capture_sequence = AE_FIRST_SEQUENCE; engine->capture_sample_offset = 0;
        engine->codec_prepared = prepared; engine->codec_running = false;
        engine->prepare_stage = prepared ? 2 : 0;
        engine->source_roles = prepared ? engine->source_roles : capture ? AE_MICROPHONE : AE_SPEAKER;
        bool duplex = !prepared && engine->codec_capabilities.full_duplex;
        bool both = false;
        for (unsigned i = 0; i < engine->codec_capabilities.format_count; ++i) {
            const ae_profile_t *profile = &engine->codec_capabilities.formats[i];
            if (ae_format_equal(&profile->format, &request->format) &&
                (profile->roles & (AE_MICROPHONE | AE_SPEAKER)) == (AE_MICROPHONE | AE_SPEAKER)) both = true;
        }
        if (duplex && both && request->route != AE_ROUTE_LOCAL) {
            ae_route_capabilities_t caps = {0};
            duplex = engine->dependencies.route.capabilities(engine->dependencies.route.context, request->route, &caps) == AE_OK &&
                caps.full_duplex && caps.capture && caps.playback && ae_route_token_equal(&caps.token, &request->route_token);
        }
        if (duplex && both) engine->source_roles = AE_MICROPHONE | AE_SPEAKER;
        engine->active_roles = engine->io_pending = engine->closing_roles = 0;
        engine->capture_fill = 0;
        engine->view.state = AE_PREPARING;
    }
    candidate.view.consumer_generation = engine->next_consumer_generation++;
    candidate.original_consumer_generation = candidate.view.consumer_generation;
    candidate.view.request_id = request->request_id;
    candidate.view.kind = request->kind;
    candidate.view.state = AE_JOB_STARTING;
    candidate.startup_deadline_ms = now_ms + AE_START_TIMEOUT_MS;
    candidate.next_sync_ms = deadline(now_ms, AE_SYNC_INTERVAL_MS);
    ae_frame_t *storage = capture ? engine->capture_storage[request->kind] : engine->playback_storage;
    result = ae_ring_init(&candidate.ring, storage, capture ? AE_CAPTURE_RING_FRAMES : AE_RING_FRAMES,
        &request->format, engine->view.audio_generation, candidate.view.consumer_generation,
        AE_FIRST_SEQUENCE, 0, &request->route_token);
    if (result != AE_OK) return result;
    *job = candidate;
    ++engine->view.revision;
    *output = job->view;
    ae_refresh_view(engine);
    return AE_OK;
}

ae_result_t ae_stop(ae_engine_t *engine, const ae_id_t *id, uint64_t generation,
    ae_stop_reason_t reason, uint64_t now_ms)
{
    if (!engine || !id || reason > AE_STOP_MAINTENANCE) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    for (unsigned i = 0; i <= AE_CONSUMERS_MAX; ++i) {
        ae_job_t *job = i == AE_CONSUMERS_MAX ? &engine->playback : &engine->capture[i];
        if (!ae_id_equal(id, &job->view.job_id) || job->view.state == AE_JOB_EMPTY) continue;
        if (generation != job->original_consumer_generation) return AE_STALE;
        if (!ae_job_active(job)) {
            if (!job->view.cleanup_pending) return AE_TOO_LATE;
            if (reason == job->view.stop_reason) return AE_OK;
            if (job->request.kind != AE_CAPTURE_RECORDING || job->discard ||
                (reason != AE_STOP_CANCEL_KEEP && reason != AE_STOP_CANCEL_DISCARD)) return AE_BUSY;
            if (reason == AE_STOP_CANCEL_DISCARD) {
                ae_result_t allowed = ae_authorize(engine, job, AE_AUTH_RECORD_DELETE, now_ms);
                if (allowed != AE_OK) return allowed;
            }
            if (job->stage) {
                if (!engine->dependencies.store.cancel_finish) return AE_TOO_LATE;
                ae_result_t cancelled = engine->dependencies.store.cancel_finish(engine->dependencies.store.context,
                    &job->metadata.object_id, job->metadata.revision);
                if (cancelled != AE_OK) return cancelled;
            }
            job->view.stop_reason = reason; job->stage = 0;
            job->discard = reason == AE_STOP_CANCEL_DISCARD;
            ae_refresh_view(engine); return AE_OK;
        }
        if (reason == AE_STOP_CANCEL_DISCARD) {
            if (job->request.kind != AE_CAPTURE_RECORDING) return AE_INVALID;
            ae_result_t result = ae_authorize(engine, job, AE_AUTH_RECORD_DELETE, now_ms);
            if (result != AE_OK) return result;
        }
        ae_end_job(engine, job, reason, AE_OK, now_ms);
        return AE_OK;
    }
    return AE_NOT_FOUND;
}

static ae_result_t failure(ae_engine_t *engine, ae_result_t error, uint64_t now_ms)
{
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i)
        ae_end_job(engine, &engine->capture[i], AE_STOP_IO, error, now_ms);
    ae_end_job(engine, &engine->playback, AE_STOP_IO, error, now_ms);
    engine->view.error = error;
    engine->view.quarantined = true;
    ae_request_hardware_stop(engine, now_ms);
    return error;
}

static uint64_t after_io(ae_engine_t *engine, uint64_t now_ms)
{
    if (engine->dependencies.monotonic_ms)
        now_ms = engine->dependencies.monotonic_ms(engine->dependencies.policy_context);
    (void)ae_clock(engine, now_ms);
    if (ae_cancel_latched(engine)) ae_safety_stop(engine, AE_STOP_PRIVACY, now_ms);
    return now_ms;
}

static void validate_jobs(ae_engine_t *engine, uint64_t now_ms)
{
    if (ae_cancel_latched(engine) || !engine->environment.unlocked || engine->environment.privacy_latched ||
        engine->environment.maintenance) {
        ae_safety_stop(engine, AE_STOP_PRIVACY, now_ms);
        return;
    }
    if (engine->microphone_may_be_active && capture_count(engine) && !ae_privacy_ready(engine, now_ms)) {
        ae_safety_stop(engine, AE_STOP_INDICATOR, now_ms);
        return;
    }
    for (unsigned i = 0; i <= AE_CONSUMERS_MAX; ++i) {
        ae_job_t *job = i == AE_CONSUMERS_MAX ? &engine->playback : &engine->capture[i];
        if (!ae_job_active(job)) continue;
        if (job->request.kind <= AE_CAPTURE_RECOGNITION && job->view.first_frame_seen &&
            now_ms >= engine->last_capture_ms && now_ms - engine->last_capture_ms >= AE_CAPTURE_STALL_MS) {
            ae_end_job(engine, job, AE_STOP_IO, AE_TIMEOUT, now_ms); continue;
        }
        if ((!job->view.first_frame_seen && now_ms >= job->startup_deadline_ms) ||
            (job->view.first_frame_seen && now_ms >= job->view.deadline_ms)) {
            ae_end_job(engine, job, job->view.first_frame_seen ? AE_STOP_DURATION : AE_STOP_START_TIMEOUT,
                AE_TIMEOUT, now_ms);
            continue;
        }
        ae_result_t result = ae_authorize(engine, job, AE_AUTH_RUNNING, now_ms);
        if (result != AE_OK) {
            ae_end_job(engine, job, result == AE_UNAVAILABLE ? AE_STOP_OWNER_LOST : AE_STOP_CONSENT, result, now_ms);
            continue;
        }
        result = route_check(engine, &job->request);
        if (result != AE_OK) ae_end_job(engine, job, AE_STOP_OWNER_LOST, result, now_ms);
    }
}

static ae_result_t stop_hardware(ae_engine_t *engine, uint64_t now_ms)
{
    ae_result_t result = AE_OK;
    if (engine->codec_stop_pending) {
        result = engine->dependencies.codec.stop ? engine->dependencies.codec.stop(
            engine->dependencies.codec.context, engine->stop_generation) : AE_UNAVAILABLE;
        now_ms = after_io(engine, now_ms);
        if (result == AE_OK) {
            engine->codec_stop_pending = false;
            engine->unmute_pending = engine->unmute_cancelled = engine->closing_roles = 0;
            engine->microphone_may_be_active = false;
            mute_ack(engine, AE_MICROPHONE | AE_SPEAKER, true);
        } else if (result != AE_AGAIN) engine->mute_known_roles = 0;
    } else if (engine->route_stop_pending) {
        result = engine->dependencies.route.stop ? engine->dependencies.route.stop(
            engine->dependencies.route.context, engine->view.route, engine->stop_generation,
            &engine->stop_route_token) : AE_UNAVAILABLE;
        now_ms = after_io(engine, now_ms);
        if (result == AE_OK) engine->route_stop_pending = false;
    }
    if (!engine->codec_stop_pending && !engine->route_stop_pending) {
        engine->microphone_may_be_active = false;
        engine->codec_prepared = engine->codec_running = false;
        engine->control_pending = engine->io_pending = engine->closing_roles = engine->active_roles = 0;
        engine->view.microphone_enabled = engine->view.speaker_enabled = false;
        engine->view.microphone_muted = engine->view.speaker_muted = true;
        engine->view.hardware_off_confirmed = true;
        engine->view.state = engine->view.quarantined ? AE_FAULT : AE_OFF;
        ae_refresh_view(engine);
        return AE_OK;
    }
    if (result != AE_OK && result != AE_AGAIN) {
        engine->view.error = result;
        engine->view.quarantined = true;
    }
    if (now_ms < engine->stop_started_ms || now_ms - engine->stop_started_ms >= AE_STOP_TIMEOUT_MS) {
        engine->view.error = AE_TIMEOUT;
        engine->view.quarantined = true;
        engine->view.state = AE_FAULT;
    }
    ae_refresh_view(engine);
    return result == AE_OK ? AE_AGAIN : result;
}

static uint8_t desired_roles(ae_engine_t *engine)
{
    uint8_t roles = ae_job_active(&engine->playback) ? AE_SPEAKER : 0;
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i)
        if (ae_job_active(&engine->capture[i]) && (i != AE_CAPTURE_RECORDING || engine->capture[i].reserved)) roles |= AE_MICROPHONE;
    return roles;
}

static ae_result_t sync_roles(ae_engine_t *engine, uint64_t now_ms)
{
    uint8_t desired = desired_roles(engine);
    uint8_t changes = engine->closing_roles ? engine->closing_roles : engine->unmute_pending ?
        engine->unmute_pending : (uint8_t)(desired ^ engine->active_roles);
    if (!changes) return AE_OK;
    ae_role_t role = (changes & AE_MICROPHONE) ? AE_MICROPHONE : AE_SPEAKER;
    bool muted = (engine->closing_roles & role) || (!(engine->unmute_pending & role) && !(desired & role));
    bool retiring_cancelled = !muted && (engine->unmute_cancelled & role);
    if (muted && (engine->closing_roles&role)) {
        uint64_t since=engine->closing_started_ms[role==AE_MICROPHONE?0:1];
        if (now_ms<since || now_ms-since>=AE_STOP_TIMEOUT_MS) return failure(engine,AE_TIMEOUT,now_ms);
    }
    if (!muted && !engine->unmute_pending &&
        (engine->io_pending || (role == AE_MICROPHONE && !ae_privacy_ready(engine, now_ms)))) return AE_AGAIN;
    if (!muted && !retiring_cancelled && role == AE_MICROPHONE) engine->microphone_may_be_active = true;
    ae_result_t r = engine->dependencies.codec.mute ? engine->dependencies.codec.mute(
        engine->dependencies.codec.context, role, muted, engine->view.audio_generation) : AE_UNAVAILABLE;
    if (!muted && r == AE_AGAIN) engine->unmute_pending |= role;
    if (retiring_cancelled && (r == AE_OK || r == AE_STALE)) {
        /* Safety mute completed after the original unmute work. Retire that
         * exact mailbox result without interpreting it as a new unmute. */
        engine->unmute_pending &= (uint8_t)~role;
        engine->unmute_cancelled &= (uint8_t)~role;
        r = AE_OK;
    } else if (r == AE_OK) {
        mute_ack(engine, (uint8_t)role, muted);
        if (muted) {
            engine->active_roles &= (uint8_t)~role;
            engine->unmute_cancelled |= engine->unmute_pending & role;
        } else {
            engine->active_roles |= role;
            engine->unmute_pending &= (uint8_t)~role;
        }
        engine->closing_roles &= (uint8_t)~role;
        if (role == AE_MICROPHONE) {
            engine->view.microphone_muted = muted; engine->view.microphone_enabled = !muted;
            if (muted) { engine->microphone_may_be_active = false; if (engine->io_pending != AE_MICROPHONE) engine->capture_fill = 0; }
        } else { engine->view.speaker_muted = muted; engine->view.speaker_enabled = !muted; }
        engine->view.state = engine->active_roles ? AE_RUNNING : AE_READY_MUTED;
    } else if (r != AE_AGAIN) engine->mute_known_roles &= (uint8_t)~role;
    /* Apply the physical acknowledgement before policy revalidation. A stop
     * triggered inside the callback must keep its logical gate closed. */
    now_ms = after_io(engine, now_ms); validate_jobs(engine, now_ms);
    if (r != AE_OK && r != AE_AGAIN) return failure(engine, r, now_ms);
    ae_refresh_view(engine); return r;
}

static ae_result_t playback_binding(ae_engine_t *engine, uint64_t now_ms)
{
    ae_job_t *job = &engine->playback;
    if (!engine->dependencies.route.bind_playback || job->request.kind != AE_PLAYBACK_STREAM || job->request.route == AE_ROUTE_LOCAL)
        return AE_OK;
    bool enabled = ae_job_active(job);
    if (enabled && job->route_bound) return AE_OK;
    if (!enabled && !job->route_binding_started) return AE_OK;
    job->route_binding_started = true;
    ae_result_t r = engine->dependencies.route.bind_playback(engine->dependencies.route.context,
        job->request.route, job->ring.audio_generation, &job->request.route_token,
        job->original_consumer_generation, enabled);
    now_ms = after_io(engine, now_ms); validate_jobs(engine, now_ms);
    if (r == AE_OK) {
        job->route_bound = enabled;
        if (!enabled) { job->route_binding_started = false; job->view.cleanup_pending = false; }
    } else if (r != AE_AGAIN && enabled) {
        if (r != AE_UNCERTAIN) job->route_binding_started = false;
        ae_end_job(engine, job, AE_STOP_IO, r, now_ms);
    }
    ae_refresh_view(engine); return r;
}

static ae_result_t prepare_source(ae_engine_t *engine, uint64_t now_ms)
{
    ae_result_t result; ae_codec_port_t *codec = &engine->dependencies.codec;
    if (engine->codec_running) return sync_roles(engine, now_ms);
    if (!desired_roles(engine)) return AE_AGAIN;
    switch (engine->prepare_stage) {
    case 0:
        if (engine->view.route == AE_ROUTE_LOCAL) result = AE_OK;
        else result = engine->dependencies.route.prepare ? engine->dependencies.route.prepare(
            engine->dependencies.route.context, engine->view.route, &engine->view.format, engine->source_roles,
            engine->view.audio_generation, &engine->view.route_token) : AE_UNAVAILABLE;
        break;
    case 1:
        result = playback_binding(engine, now_ms);
        if (result != AE_OK) return result;
        result = codec->prepare ? codec->prepare(codec->context, &engine->view.format, engine->source_roles,
            engine->view.audio_generation) : AE_UNAVAILABLE;
        if (result == AE_OK) {
            mute_ack(engine, AE_MICROPHONE | AE_SPEAKER, true);
            engine->codec_prepared = true; engine->view.hardware_off_confirmed = false;
            engine->view.state = AE_READY_MUTED;
        }
        break;
    case 2:
        result = (engine->source_roles & AE_MICROPHONE) ? (codec->gain ? codec->gain(codec->context,
            engine->view.microphone_gain_db, engine->view.audio_generation) : AE_UNAVAILABLE) : AE_OK;
        break;
    case 3:
        result = (engine->source_roles & AE_SPEAKER) ? (codec->volume ? codec->volume(codec->context,
            engine->view.volume_percent, engine->view.audio_generation) : AE_UNAVAILABLE) : AE_OK;
        break;
    default:
        engine->codec_running = true; engine->last_capture_ms = now_ms;
        return sync_roles(engine, now_ms);
    }
    now_ms = after_io(engine, now_ms); validate_jobs(engine, now_ms);
    if (!ae_jobs_active(engine)) return AE_AGAIN;
    if (result == AE_OK) ++engine->prepare_stage;
    else if (result != AE_AGAIN) return failure(engine, result, now_ms);
    ae_refresh_view(engine); return AE_AGAIN;
}

static void first_frame(ae_job_t *job, uint64_t now_ms)
{
    if (job->view.first_frame_seen) return;
    job->view.first_frame_seen = true;
    job->view.first_frame_ms = now_ms;
    job->view.deadline_ms = deadline(now_ms, (uint64_t)job->request.duration_seconds * 1000);
    job->view.state = AE_JOB_RUNNING;
    job->view.device_owned = job->request.kind == AE_CAPTURE_RECORDING && job->reserved &&
        job->request.disconnect_policy == AE_DISCONNECT_CONTINUE_LOCAL;
}

static ae_result_t capture_frame(ae_engine_t *engine, uint64_t now_ms)
{
    size_t bytes; uint32_t samples;
    if (ae_format_size(&engine->view.format, &bytes, &samples) != AE_OK) return AE_INVALID;
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) {
        ae_job_t *job = &engine->capture[i];
        if (!ae_job_active(job) || job->stage || (i == AE_CAPTURE_RECORDING && !job->reserved)) continue;
        ae_result_t result = ae_ring_init(&job->ring, engine->capture_storage[i], AE_CAPTURE_RING_FRAMES,
            &job->request.format, engine->view.audio_generation, job->view.consumer_generation,
            AE_FIRST_SEQUENCE, 0, &job->request.route_token);
        if (result != AE_OK) { ae_end_job(engine, job, AE_STOP_IO, result, now_ms); continue; }
        job->stage = 1;
    }
    size_t actual = 0;
    const uint64_t generation = engine->view.audio_generation;
    ae_result_t result = engine->dependencies.codec.read ? engine->dependencies.codec.read(
        engine->dependencies.codec.context, generation, engine->work_frame.data + engine->capture_fill,
        bytes - engine->capture_fill, &actual) : AE_UNAVAILABLE;
    engine->io_pending = result == AE_AGAIN ? AE_MICROPHONE : 0;
    now_ms = after_io(engine, now_ms);
    validate_jobs(engine, now_ms);
    if (generation != engine->view.audio_generation || !capture_count(engine)) {
        ae_secure_zero(&engine->work_frame, sizeof(engine->work_frame)); engine->capture_fill = 0;
        return AE_STALE;
    }
    if (actual > bytes - engine->capture_fill || (result != AE_OK && result != AE_AGAIN))
        return failure(engine, result == AE_OK || result == AE_AGAIN ? AE_INTEGRITY : result, now_ms);
    engine->capture_fill += actual;
    if (engine->capture_fill != bytes) return AE_AGAIN;
    engine->capture_fill = 0;
    if (engine->capture_sequence == UINT64_MAX || engine->capture_sample_offset > UINT64_MAX - samples)
        return failure(engine, AE_EXHAUSTED, now_ms);
    ae_frame_t *frame = &engine->work_frame;
    frame->audio_generation = generation;
    frame->sequence = engine->capture_sequence;
    frame->sample_offset = engine->capture_sample_offset;
    frame->route_token = engine->view.route_token;
    frame->format = engine->view.format;
    frame->bytes = (uint16_t)bytes;
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) {
        ae_job_t *job = &engine->capture[i];
        if (!ae_job_active(job) || !job->stage) continue;
        uint64_t maximum = job->request.byte_limit;
        if (i == AE_CAPTURE_RECORDING) maximum = job->metadata.payload_limit_bytes;
        if (job->view.received_bytes > maximum || bytes > maximum - job->view.received_bytes) {
            ae_end_job(engine, job, AE_STOP_BYTES, AE_QUOTA, now_ms);
            continue;
        }
        frame->consumer_generation = job->view.consumer_generation;
        /* Each immutable consumer owns its sequence space, including a new
         * capture admitted while another role keeps the codec running. */
        frame->sequence = job->ring.next_sequence;
        frame->sample_offset = job->ring.next_sample_offset;
        result = ae_ring_push(&job->ring, frame);
        if (result != AE_OK) {
            if (result == AE_OVERFLOW) ++engine->view.overruns;
            ae_end_job(engine, job, AE_STOP_OVERFLOW, result, now_ms);
        } else {
            job->view.received_bytes += bytes;
            first_frame(job, now_ms);
        }
    }
    ++engine->capture_sequence;
    engine->capture_sample_offset += samples;
    engine->last_capture_ms = now_ms;
    ae_secure_zero(frame, sizeof(*frame));
    ae_refresh_view(engine);
    return AE_OK;
}

static ae_result_t deliver_capture(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    const ae_frame_t *frame = ae_ring_front(&job->ring);
    if (!frame || !ae_job_active(job)) return AE_AGAIN;
    if (job->request.kind == AE_CAPTURE_RECORDING) return ae_record_write(engine, job, now_ms);
    ae_result_t result;
    if (job->request.route == AE_ROUTE_LOCAL)
        result = engine->dependencies.capture_sink(engine->dependencies.policy_context,
            job->request.kind, &job->view.job_id, frame);
    else result = engine->dependencies.route.capture ? engine->dependencies.route.capture(
        engine->dependencies.route.context, job->request.route, frame) : AE_UNAVAILABLE;
    size_t bytes = frame->bytes;
    if (result == AE_OK) {
        job->view.delivered_bytes += bytes;
        job->view.sample_frames += bytes / (job->request.format.channels * 2U);
        ae_ring_drop(&job->ring);
    }
    now_ms = after_io(engine, now_ms);
    validate_jobs(engine, now_ms);
    if (!ae_job_active(job)) return AE_STALE;
    if (result != AE_OK && result != AE_AGAIN) ae_end_job(engine, job, AE_STOP_IO, result, now_ms);
    return result;
}

ae_result_t ae_submit_playback(ae_engine_t *engine, const ae_frame_t *frame, uint64_t now_ms)
{
    if (!engine || !frame) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    validate_jobs(engine, now_ms);
    ae_job_t *job = &engine->playback;
    if (!ae_job_active(job) || job->request.kind != AE_PLAYBACK_STREAM) return AE_STALE;
    if (frame->bytes > job->request.byte_limit - job->view.received_bytes) {
        ae_end_job(engine, job, AE_STOP_BYTES, AE_QUOTA, now_ms); return AE_QUOTA;
    }
    ae_result_t result = ae_ring_push(&job->ring, frame);
    if (result == AE_STALE) ++engine->view.stale_frames;
    if (result == AE_OVERFLOW) {
        ++engine->view.overruns;
        ae_end_job(engine, job, AE_STOP_OVERFLOW, result, now_ms);
    }
    if (result == AE_OK) job->view.received_bytes += frame->bytes;
    ae_refresh_view(engine);
    return result;
}

static ae_result_t playback_frame(ae_engine_t *engine, uint64_t now_ms)
{
    ae_job_t *job = &engine->playback;
    if (ae_job_active(job) && engine->io_pending != AE_SPEAKER && job->request.kind == AE_PLAYBACK_RECORDING && !job->ring.count) {
        ae_result_t result = ae_record_source(engine, job, now_ms);
        if (result != AE_OK && result != AE_AGAIN) {
            ae_end_job(engine, job, AE_STOP_IO, result, now_ms); return result;
        }
        if (!ae_job_active(job)) return AE_OK;
    }
    now_ms = after_io(engine, now_ms);
    validate_jobs(engine, now_ms);
    if ((!ae_job_active(job) && engine->io_pending != AE_SPEAKER) || engine->codec_stop_pending) return AE_STALE;
    const ae_frame_t *frame = engine->io_pending == AE_SPEAKER ? &engine->playback_io_frame : ae_ring_front(&job->ring);
    if (!frame) {
        if (job->view.first_frame_seen) {
            ++engine->view.underruns;
            if (now_ms - job->last_data_ms >= AE_PLAYBACK_UNDERRUN_MS)
                ae_end_job(engine, job, AE_STOP_UNDERRUN, AE_TIMEOUT, now_ms);
        }
        return AE_AGAIN;
    }
    if (engine->io_pending != AE_SPEAKER) engine->playback_io_frame = *frame;
    frame = &engine->playback_io_frame;
    size_t actual = 0, bytes = frame->bytes;
    ae_result_t result = engine->dependencies.codec.write ? engine->dependencies.codec.write(
        engine->dependencies.codec.context, frame->audio_generation, frame->data, bytes, &actual) : AE_UNAVAILABLE;
    engine->io_pending = result == AE_AGAIN ? AE_SPEAKER : 0;
    if (actual <= bytes) {
        job->view.delivered_bytes += actual;
        job->view.sample_frames += actual / (job->request.format.channels * 2U);
    }
    if (result == AE_OK && actual == bytes && ae_job_active(job))
        first_frame(job, engine->dependencies.monotonic_ms(engine->dependencies.policy_context));
    now_ms = after_io(engine, now_ms);
    validate_jobs(engine, now_ms);
    if (!ae_job_active(job)) return AE_STALE;
    if (result == AE_AGAIN && actual == 0) return AE_AGAIN;
    if (result != AE_OK || actual != bytes) return failure(engine, result == AE_OK ? AE_IO_ERROR : result, now_ms);
    job->last_data_ms = now_ms;
    if (engine->dependencies.route.playback_consumed && job->request.route != AE_ROUTE_LOCAL)
        engine->dependencies.route.playback_consumed(engine->dependencies.route.context, job->request.route, frame);
    ae_ring_drop(&job->ring);
    if (job->request.kind == AE_PLAYBACK_RECORDING && job->view.delivered_bytes == job->metadata.pcm_bytes)
        ae_end_job(engine, job, AE_STOP_REQUESTED, AE_OK, now_ms);
    ae_refresh_view(engine);
    return AE_OK;
}

ae_result_t ae_poll(ae_engine_t *engine, uint64_t now_ms)
{
    if (!engine) return AE_INVALID;
    (void)ae_clock(engine, now_ms); validate_jobs(engine, now_ms);
    if (engine->codec_stop_pending || engine->route_stop_pending) return stop_hardware(engine, now_ms);
    ae_job_t *record = &engine->capture[AE_CAPTURE_RECORDING];
    if (record->view.cleanup_pending) {
        (void)ae_record_finalize(engine, record, now_ms);
        now_ms = after_io(engine, now_ms); validate_jobs(engine, now_ms);
    } else if (ae_job_active(record) && !record->reserved) {
        ae_result_t r = ae_record_reserve(engine, record, now_ms);
        now_ms = after_io(engine, now_ms); validate_jobs(engine, now_ms);
        if (r != AE_OK && r != AE_AGAIN) ae_end_job(engine, record, AE_STOP_IO, r, now_ms);
    }
    if (engine->codec_stop_pending || engine->route_stop_pending) return stop_hardware(engine, now_ms);
    if (engine->closing_roles && engine->codec_running) {
        ae_result_t r=sync_roles(engine,now_ms);
        if (r!=AE_OK) return r;
    }
    ae_result_t binding_cleanup=AE_OK;
    if (engine->playback.view.cleanup_pending) {
        binding_cleanup=playback_binding(engine,now_ms);
        /* A transport unbind never delays the physical safety mute or drains
         * the independent microphone's frame budget. The old playback slot
         * remains unavailable until its exact binding is acknowledged. */
    }
    if (engine->view.quarantined) { ae_refresh_view(engine); return engine->view.error; }
    if (!ae_jobs_active(engine)) { ae_refresh_view(engine); return binding_cleanup; }
    if (engine->control_pending) return AE_AGAIN;
    if (!engine->codec_running) return prepare_source(engine, now_ms);
    ae_result_t binding=ae_job_active(&engine->playback) ? playback_binding(engine,now_ms) : AE_OK;
    if (binding!=AE_OK) return binding;
    /* The codec has one asynchronous mailbox. Retire its exact pending request
     * before submitting another. Independent safety mutes have a driver lane. */
    if (engine->closing_roles) {
        ae_result_t r = sync_roles(engine, now_ms);
        if (r != AE_OK) return r;
    }
    /* Consumer queues are independent of the codec mailbox. Drain them while
     * the next physical read is pending, so a journal checkpoint does not
     * permanently accumulate one delayed frame on every flush. */
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) {
        if (!ae_job_active(&engine->capture[i]) || (i == AE_CAPTURE_RECORDING && !record->reserved)) continue;
        (void)deliver_capture(engine, &engine->capture[i], now_ms);
        now_ms = after_io(engine, now_ms); validate_jobs(engine, now_ms);
    }
    if (engine->codec_stop_pending) return AE_AGAIN;
    if (engine->io_pending == AE_MICROPHONE) return capture_frame(engine, now_ms);
    if (engine->io_pending == AE_SPEAKER) return playback_frame(engine, now_ms);
    ae_result_t r = sync_roles(engine, now_ms);
    if (r != AE_OK) return r;
    bool capture = capture_count(engine) && (engine->active_roles & AE_MICROPHONE);
    bool playback = ae_job_active(&engine->playback) && (engine->active_roles & AE_SPEAKER);
    if (playback && (!capture || engine->playback_turn)) { engine->playback_turn = false; r = playback_frame(engine, now_ms); }
    else if (capture) { engine->playback_turn = true; r = capture_frame(engine, now_ms); }
    else r = AE_AGAIN;
    ae_refresh_view(engine); return r;
}

static void stop_role(ae_engine_t *engine, ae_role_t role, ae_stop_reason_t reason, ae_result_t error, uint64_t now_ms)
{
    if (role == AE_MICROPHONE) {
        for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) ae_end_job(engine, &engine->capture[i], reason, error, now_ms);
        engine->view.microphone_enabled = false; engine->view.microphone_muted = true;
    } else {
        ae_end_job(engine, &engine->playback, reason, error, now_ms);
        engine->view.speaker_enabled = false; engine->view.speaker_muted = true;
    }
    if (!ae_jobs_active(engine) && engine->codec_prepared) ae_request_hardware_stop(engine, now_ms);
    ae_refresh_view(engine);
}

ae_result_t ae_enable(ae_engine_t *engine, ae_role_t role, bool enabled,
    const ae_format_t *format, uint64_t now_ms)
{
    if (!engine || (role != AE_MICROPHONE && role != AE_SPEAKER)) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    if (!enabled) { stop_role(engine, role, AE_STOP_REQUESTED, AE_OK, now_ms); return AE_OK; }
    if (ae_jobs_active(engine) || engine->codec_stop_pending || engine->route_stop_pending) return AE_BUSY;
    if (engine->view.quarantined || engine->view.route != AE_ROUTE_LOCAL) return AE_UNAVAILABLE;
    if (engine->codec_prepared) {
        if (!ae_format_equal(format,&engine->view.format) || !(engine->source_roles&role)) return AE_BUSY;
        bool *enabled_role=role==AE_MICROPHONE ? &engine->view.microphone_enabled : &engine->view.speaker_enabled;
        if (!*enabled_role) { *enabled_role=true; (void)bump(&engine->view.revision); }
        ae_refresh_view(engine); return AE_OK;
    }
    if (engine->control_pending && (engine->control_pending != 3 || engine->control_value != role)) return AE_BUSY;
    ae_result_t result = codec_format(engine, format, role);
    if (result != AE_OK) return result;
    if (!engine->dependencies.codec.prepare) return AE_UNAVAILABLE;
    uint8_t roles = (uint8_t)role;
    for (unsigned i = 0; engine->codec_capabilities.full_duplex && i < engine->codec_capabilities.format_count; ++i)
        if (ae_format_equal(format, &engine->codec_capabilities.formats[i].format) && engine->codec_capabilities.formats[i].roles == 3) roles = 3;
    result = engine->dependencies.codec.prepare(engine->dependencies.codec.context,
        format, roles, engine->view.audio_generation);
    engine->control_pending = result == AE_AGAIN ? 3 : 0; engine->control_value = (uint8_t)role;
    now_ms = after_io(engine, now_ms);
    if (ae_cancel_latched(engine)) { ae_request_hardware_stop(engine, now_ms); return AE_PRIVACY; }
    if (result == AE_OK) {
        mute_ack(engine, AE_MICROPHONE | AE_SPEAKER, true);
        engine->view.format = *format; engine->source_roles = roles;
        engine->codec_prepared = true;
        engine->view.microphone_enabled = role == AE_MICROPHONE;
        engine->view.speaker_enabled = role == AE_SPEAKER;
        engine->view.microphone_muted = engine->view.speaker_muted = true;
        engine->view.hardware_off_confirmed = false;
        engine->view.state = AE_READY_MUTED;
        (void)bump(&engine->view.revision);
    } else if (result != AE_AGAIN) return failure(engine, result, now_ms);
    ae_refresh_view(engine);
    return result;
}

ae_result_t ae_mute(ae_engine_t *engine, ae_role_t role, bool muted, uint64_t now_ms)
{
    if (!engine || (role != AE_MICROPHONE && role != AE_SPEAKER)) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    if (muted) { stop_role(engine, role, AE_STOP_PRIVACY, AE_PRIVACY, now_ms); return AE_OK; }
    /* A safety mute cancels the bounded job. Unmute cannot silently revive it. */
    return engine->codec_running && ((role == AE_MICROPHONE && !engine->view.microphone_muted) ||
        (role == AE_SPEAKER && !engine->view.speaker_muted)) ? AE_OK : AE_DENIED;
}

ae_result_t ae_volume(ae_engine_t *engine, uint8_t percent, uint64_t now_ms)
{
    if (!engine || percent > 100) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    if (engine->control_pending && (engine->control_pending != 1 || engine->control_value != percent)) return AE_BUSY;
    if (engine->io_pending || engine->unmute_pending) return AE_AGAIN;
    if (!engine->codec_capabilities.available) {
        if (!engine->dependencies.codec.capabilities) return AE_UNAVAILABLE;
        ae_result_t available = engine->dependencies.codec.capabilities(engine->dependencies.codec.context, &engine->codec_capabilities);
        if (available != AE_OK) return available;
        if (!engine->codec_capabilities.implemented || !engine->codec_capabilities.available)
            return engine->codec_capabilities.reason == AE_OK ? AE_UNAVAILABLE : engine->codec_capabilities.reason;
    }
    if (percent > engine->codec_capabilities.safe_volume_max) return AE_INVALID;
    ae_result_t result = AE_OK;
    if (engine->codec_prepared) {
        if (!engine->dependencies.codec.volume) return AE_UNAVAILABLE;
        result = engine->dependencies.codec.volume(engine->dependencies.codec.context, percent, engine->view.audio_generation);
        engine->control_pending = result == AE_AGAIN ? 1 : 0; engine->control_value = percent;
        now_ms = after_io(engine, now_ms);
    }
    if (result == AE_OK) { engine->view.volume_percent = percent; (void)bump(&engine->view.revision); }
    else if (result != AE_AGAIN) return failure(engine, result, now_ms);
    return result;
}

ae_result_t ae_gain(ae_engine_t *engine, uint8_t db, uint64_t now_ms)
{
    if (!engine || db > 42 || db % 6) return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    if (engine->control_pending && (engine->control_pending != 2 || engine->control_value != db)) return AE_BUSY;
    if (engine->io_pending || engine->unmute_pending) return AE_AGAIN;
    ae_result_t result = AE_OK;
    if (engine->codec_prepared) {
        if (!engine->dependencies.codec.gain) return AE_UNAVAILABLE;
        result = engine->dependencies.codec.gain(engine->dependencies.codec.context, db, engine->view.audio_generation);
        engine->control_pending = result == AE_AGAIN ? 2 : 0; engine->control_value = db;
        now_ms = after_io(engine, now_ms);
    }
    if (result == AE_OK) { engine->view.microphone_gain_db = db; (void)bump(&engine->view.revision); }
    else if (result != AE_AGAIN) return failure(engine, result, now_ms);
    return result;
}

ae_result_t ae_recover(ae_engine_t *engine, uint64_t now_ms)
{
    if (!engine || ae_jobs_active(engine)) return AE_BUSY;
    if (!engine->dependencies.codec.stop) return AE_UNAVAILABLE;
    ae_result_t result = engine->dependencies.codec.stop(engine->dependencies.codec.context, engine->view.audio_generation);
    if (result != AE_OK) return result;
    if (engine->route_stop_pending) return AE_BUSY;
    if (engine->capture[AE_CAPTURE_RECORDING].view.cleanup_pending) {
        result = ae_record_reconcile(engine, &engine->capture[AE_CAPTURE_RECORDING], now_ms);
        if (result != AE_OK) return result;
    }
    engine->codec_prepared = engine->codec_running = engine->codec_stop_pending = false;
    engine->unmute_pending = engine->unmute_cancelled = engine->closing_roles = engine->active_roles = engine->io_pending = engine->control_pending = 0;
    mute_ack(engine, AE_MICROPHONE | AE_SPEAKER, true);
    engine->microphone_may_be_active = engine->clock_fault = false;
    engine->view.quarantined = false;
    engine->view.hardware_off_confirmed = true;
    engine->view.microphone_enabled = engine->view.speaker_enabled = false;
    engine->view.microphone_muted = engine->view.speaker_muted = true;
    engine->view.error = AE_OK;
    engine->view.state = AE_OFF;
    engine->last_ms = now_ms;
    if (!bump(&engine->view.audio_generation) || !bump(&engine->view.revision)) return AE_EXHAUSTED;
    ae_refresh_view(engine);
    return AE_OK;
}
