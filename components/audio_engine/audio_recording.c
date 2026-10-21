// SPDX-License-Identifier: Apache-2.0
#include "audio_internal.h"
#include <string.h>

ae_result_t ae_store_time(ae_engine_t *engine, uint64_t *seconds)
{
    if (!engine->dependencies.trusted_unix_time) return AE_TIME_UNKNOWN;
    ae_result_t result = engine->dependencies.trusted_unix_time(engine->dependencies.policy_context, seconds);
    if (result != AE_OK || !*seconds || *seconds < engine->last_unix_seconds) return AE_TIME_UNKNOWN;
    engine->last_unix_seconds = *seconds;
    return AE_OK;
}

static ae_result_t store_ready(ae_engine_t *engine)
{
    ae_store_port_t *store = &engine->dependencies.store;
    if (!store->capabilities || !store->get || !store->read || !store->list || !store->remove)
        return AE_UNAVAILABLE;
    ae_store_capabilities_t caps = {0};
    ae_result_t result = store->capabilities(store->context, &caps);
    if (result != AE_OK) return result;
    if (!caps.implemented || !caps.available || !caps.geometry_verified || !caps.owned_and_provisioned ||
        !caps.encryption_ready || !caps.authenticated_metadata || !caps.recovery_complete) return AE_UNAVAILABLE;
    if (!caps.retention_time_known) return AE_TIME_UNKNOWN;
    bool internal = caps.storage == AE_STORAGE_RECORDS_PARTITION;
    if (!internal && caps.storage != AE_STORAGE_NAND) return AE_INVALID;
    uint64_t minimum = internal ? 65536 : 1024U * 1024U;
    uint64_t maximum = internal ? 512U * 1024U : 32U * 1024U * 1024U;
    if (caps.object_limit > AE_OBJECTS_MAX || !caps.object_limit || caps.object_count > caps.object_limit ||
        caps.quota_bytes < minimum || caps.quota_bytes > maximum ||
        caps.quota_bytes % 4096 || caps.used_bytes > caps.quota_bytes ||
        caps.reserved_bytes > caps.quota_bytes - caps.used_bytes ||
        (!caps.media_chunk_bytes && caps.media_chunk_overhead_bytes)) return AE_QUOTA;
    uint64_t reserve = caps.usable_bytes / 10;
    uint64_t minimum_reserve = internal ? 32768 : 4U * 1024U * 1024U;
    if (reserve < minimum_reserve) reserve = minimum_reserve;
    if (caps.usable_bytes < reserve || caps.quota_bytes > caps.usable_bytes - reserve) return AE_QUOTA;
    engine->store_capabilities = caps;
    return AE_OK;
}

ae_result_t ae_record_admit(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    (void)now_ms;
    ae_store_port_t *store = &engine->dependencies.store;
    if (!store->reserve || !store->append || !store->checkpoint || !store->finish) return AE_UNAVAILABLE;
    ae_result_t result = store_ready(engine);
    if (result != AE_OK) return result;
    const ae_start_request_t *request = &job->request;
    const ae_store_capabilities_t *caps = &engine->store_capabilities;
    if (request->storage != caps->storage) return AE_INVALID;
    if (request->retention_seconds < 3600 || request->retention_seconds > 2592000 ||
        request->export_policy < AE_EXPORT_NONE || request->export_policy > AE_EXPORT_OWNER_DOWNLOAD) return AE_INVALID;
    if (caps->object_count >= caps->object_limit || request->byte_limit >
        caps->quota_bytes - caps->used_bytes - caps->reserved_bytes) return AE_QUOTA;
    uint64_t overhead = (uint64_t)caps->per_object_overhead_bytes + AE_WAV_HEADER_BYTES;
    if (overhead >= request->byte_limit) return AE_QUOTA;
    uint64_t payload = request->byte_limit - overhead;
    size_t frame_bytes; uint32_t samples;
    if (ae_format_size(&request->format, &frame_bytes, &samples) != AE_OK) return AE_INVALID;
    if (caps->media_chunk_bytes) {
        uint64_t cost = (uint64_t)caps->media_chunk_bytes + caps->media_chunk_overhead_bytes;
        uint64_t usable = caps->media_chunk_bytes - caps->media_chunk_bytes % frame_bytes;
        payload = payload / cost * usable;
    }
    if (payload < frame_bytes) return AE_QUOTA;
    payload -= payload % frame_bytes;
    uint64_t seconds;
    result = ae_store_time(engine, &seconds);
    if (result != AE_OK) return result;
    if (seconds > UINT64_MAX - request->retention_seconds) return AE_INVALID;
    job->metadata = (ae_record_metadata_t){.principal_id = request->principal_id,
        .consent_id = request->consent_id, .consent_revision = request->consent_revision,
        .revision = 1, .created_unix_seconds = seconds, .expires_unix_seconds = seconds + request->retention_seconds,
        .reserved_bytes = request->byte_limit, .payload_limit_bytes = payload,
        .format = request->format, .storage = request->storage,
        .state = AE_RECORD_STARTING, .export_policy = request->export_policy};
    return AE_OK;
}

ae_result_t ae_record_reserve(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    if (ae_cancel_latched(engine)) return AE_PRIVACY;
    ae_result_t result = ae_authorize(engine, job, AE_AUTH_START, now_ms);
    if (result != AE_OK) return result;
    uint64_t seconds;
    result = ae_store_time(engine, &seconds);
    if (result != AE_OK || seconds >= job->metadata.expires_unix_seconds) return AE_TIME_UNKNOWN;
    job->reservation_pending = true;
    result = engine->dependencies.store.reserve(engine->dependencies.store.context, &job->metadata);
    if (result == AE_OK) { job->reserved = true; job->reservation_pending = false; }
    /* An unknown reserve may already own durable bytes. Reconcile the named
     * object during finalization; never lose its cleanup obligation. */
    if (result == AE_UNCERTAIN) job->reserved = true;
    if (result != AE_OK && result != AE_AGAIN && result != AE_UNCERTAIN && result != AE_IO_ERROR && result != AE_TIMEOUT)
        job->reservation_pending = false;
    return result;
}

static bool valid_evidence(const ae_job_t *job, const ae_record_metadata_t *actual)
{
    return ae_id_equal(&job->metadata.object_id, &actual->object_id) &&
        ae_id_equal(&job->metadata.principal_id, &actual->principal_id) &&
        ae_id_equal(&job->metadata.consent_id, &actual->consent_id) &&
        actual->consent_revision == job->metadata.consent_revision &&
        ae_format_equal(&job->metadata.format, &actual->format) &&
        actual->expires_unix_seconds == job->metadata.expires_unix_seconds &&
        actual->created_unix_seconds == job->metadata.created_unix_seconds &&
        actual->reserved_bytes == job->metadata.reserved_bytes &&
        actual->payload_limit_bytes == job->metadata.payload_limit_bytes &&
        actual->export_policy == job->metadata.export_policy &&
        actual->storage == job->metadata.storage &&
        actual->pcm_bytes <= job->metadata.pcm_bytes && actual->stored_bytes <= actual->reserved_bytes &&
        actual->verified_pcm_bytes <= actual->pcm_bytes && actual->synced_pcm_bytes <= actual->verified_pcm_bytes &&
        actual->verified_pcm_bytes % (actual->format.channels * 2U) == 0;
}

ae_result_t ae_record_write(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    const ae_frame_t *frame = ae_ring_front(&job->ring);
    if (!frame || !job->reserved || ae_cancel_latched(engine)) return AE_AGAIN;
    uint64_t seconds;
    ae_result_t result = ae_store_time(engine, &seconds);
    if (result != AE_OK || seconds >= job->metadata.expires_unix_seconds) {
        ae_end_job(engine, job, AE_STOP_CONSENT, AE_TIME_UNKNOWN, now_ms); return AE_TIME_UNKNOWN;
    }
    ae_store_port_t *store = &engine->dependencies.store;
    if (now_ms >= job->next_sync_ms) {
        ae_record_metadata_t evidence = {0};
        result = store->checkpoint(store->context, &job->metadata.object_id, &evidence);
        if (result == AE_OK) {
            if (!valid_evidence(job, &evidence) || !evidence.integrity_verified || !evidence.sync_acknowledged ||
                evidence.verified_pcm_bytes != job->metadata.pcm_bytes ||
                evidence.synced_pcm_bytes != job->metadata.pcm_bytes) result = AE_INTEGRITY;
            else {
                job->metadata = evidence;
                job->next_sync_ms = now_ms > UINT64_MAX - AE_SYNC_INTERVAL_MS ? UINT64_MAX : now_ms + AE_SYNC_INTERVAL_MS;
            }
        }
        if (result != AE_OK && result != AE_AGAIN) ae_end_job(engine, job, AE_STOP_IO, result, now_ms);
        return result;
    }
    size_t actual = 0;
    result = store->append(store->context, &job->metadata.object_id, job->metadata.pcm_bytes,
        frame->data, frame->bytes, &actual);
    if (actual > frame->bytes) result = AE_INTEGRITY;
    else {
        job->metadata.pcm_bytes += actual;
        job->view.delivered_bytes += actual;
        job->view.sample_frames = job->metadata.pcm_bytes / (job->request.format.channels * 2U);
    }
    if (result == AE_OK && actual == frame->bytes) {
        job->metadata.state = AE_RECORD_RECORDING;
        ae_ring_drop(&job->ring);
    } else if (result != AE_AGAIN || actual) {
        if (result == AE_OK || result == AE_AGAIN) result = AE_IO_ERROR;
        ae_end_job(engine, job, AE_STOP_IO, result, now_ms);
    }
    return result;
}

ae_result_t ae_record_finalize(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    if (!job->view.cleanup_pending) return AE_OK;
    ae_store_port_t *store = &engine->dependencies.store;
    if (now_ms >= job->finalize_deadline_ms) {
        job->view.state = AE_JOB_ERROR;
        job->view.error = AE_UNCERTAIN;
        /* Retain cleanup_pending, reservation and object identity. Explicit
         * recovery/reconciliation is required; no new writer replaces it. */
        return AE_TIMEOUT;
    }
    if (job->discard) {
        if (!job->stage) {
            ae_record_metadata_t current;
            ae_result_t found = store->get(store->context, &job->metadata.object_id, &current);
            if (found != AE_OK) return found;
            if (!valid_evidence(job, &current)) return AE_INTEGRITY;
            job->metadata = current; job->stage = 1;
        }
        bool revoked = false, complete = false;
        ae_result_t result = store->remove(store->context, &job->metadata.object_id,
            job->metadata.revision, &revoked, &complete);
        if (revoked) job->metadata.access_revoked = true;
        if (result == AE_OK && revoked && complete) {
            job->view.cleanup_pending = false;
            job->view.state = AE_JOB_PARTIAL;
            job->metadata.state = AE_RECORD_DELETING;
            return AE_OK;
        }
        return result == AE_OK ? AE_UNCERTAIN : result;
    }
    if (!job->stage) {
        ae_record_metadata_t evidence = {0};
        ae_result_t result = store->checkpoint(store->context, &job->metadata.object_id, &evidence);
        if (result == AE_AGAIN) return result;
        if (result == AE_NOT_FOUND && store_ready(engine) == AE_OK) {
            job->view.cleanup_pending = false; job->reserved = job->reservation_pending = false;
            job->view.state = AE_JOB_ERROR; job->view.error = AE_NOT_FOUND; return AE_OK;
        }
        if (result != AE_OK || !valid_evidence(job, &evidence) || !evidence.integrity_verified ||
            !evidence.sync_acknowledged || evidence.synced_pcm_bytes != evidence.verified_pcm_bytes) {
            job->view.error = result == AE_OK ? AE_INTEGRITY : result;
            return job->view.error;
        }
        bool complete = job->view.stop_reason == AE_STOP_REQUESTED && job->view.error == AE_OK &&
            evidence.verified_pcm_bytes != 0 && evidence.verified_pcm_bytes == job->metadata.pcm_bytes;
        job->metadata = evidence;
        job->metadata.pcm_bytes = evidence.verified_pcm_bytes;
        job->metadata.sample_frames = job->metadata.pcm_bytes / (job->metadata.format.channels * 2U);
        job->metadata.state = complete ? AE_RECORD_COMPLETED : job->metadata.pcm_bytes ? AE_RECORD_PARTIAL : AE_RECORD_ERROR;
        job->metadata.reason = job->view.error;
        if (job->metadata.reason == AE_OK && !complete) job->metadata.reason = AE_TOO_LATE;
        if (job->metadata.revision == UINT64_MAX) return AE_EXHAUSTED;
        ++job->metadata.revision;
        if (ae_wav_header(&job->metadata.format, job->metadata.pcm_bytes, engine->wav_header) != AE_OK)
            return AE_INTEGRITY;
        job->stage = 1;
    }
    ae_record_metadata_t evidence = {0};
    ae_result_t result = store->finish(store->context, &job->metadata, engine->wav_header, &evidence);
    if (result == AE_AGAIN) return result;
    if (result != AE_OK || !valid_evidence(job, &evidence) || !evidence.integrity_verified ||
        !evidence.sync_acknowledged || !evidence.hash_available || evidence.state != job->metadata.state ||
        evidence.revision != job->metadata.revision || evidence.pcm_bytes != job->metadata.pcm_bytes ||
        evidence.verified_pcm_bytes != evidence.pcm_bytes || evidence.synced_pcm_bytes != evidence.pcm_bytes ||
        evidence.sample_frames != job->metadata.sample_frames) {
        job->view.error = result == AE_OK ? AE_INTEGRITY : result;
        return job->view.error;
    }
    job->metadata = evidence;
    job->view.cleanup_pending = false;
    job->view.sample_frames = evidence.sample_frames;
    job->view.state = evidence.state == AE_RECORD_COMPLETED ? AE_JOB_COMPLETED :
        evidence.state == AE_RECORD_PARTIAL ? AE_JOB_PARTIAL : AE_JOB_ERROR;
    return AE_OK;
}

ae_result_t ae_record_reconcile(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    ae_store_port_t *store = &engine->dependencies.store;
    ae_result_t result = store_ready(engine);
    if (result != AE_OK) {
        if (!store->recover) return result;
        result = store->recover(store->context);
        if (result != AE_OK) return result;
        result = store_ready(engine); if (result != AE_OK) return result;
    }
    ae_record_metadata_t current;
    result = store->get(store->context, &job->view.recording_id, &current);
    if (result == AE_NOT_FOUND) {
        job->reserved = job->reservation_pending = job->view.cleanup_pending = false;
        job->view.state = AE_JOB_ERROR; job->view.error = AE_NOT_FOUND; return AE_OK;
    }
    if (result != AE_OK) return result;
    if (!valid_evidence(job, &current)) return AE_INTEGRITY;
    if (current.state >= AE_RECORD_COMPLETED && current.integrity_verified && current.sync_acknowledged &&
        current.verified_pcm_bytes == current.pcm_bytes && current.synced_pcm_bytes == current.pcm_bytes) {
        job->metadata = current; job->view.cleanup_pending = job->reservation_pending = false;
        job->view.state = current.state == AE_RECORD_COMPLETED ? AE_JOB_COMPLETED :
            current.state == AE_RECORD_PARTIAL ? AE_JOB_PARTIAL : AE_JOB_ERROR;
        job->view.error = current.reason; job->view.sample_frames = current.sample_frames;
        ae_refresh_view(engine); return AE_OK;
    }
    if (now_ms >= job->finalize_deadline_ms) {
        if (now_ms > UINT64_MAX - AE_FINALIZE_TIMEOUT_MS) return AE_EXHAUSTED;
        job->finalize_deadline_ms = now_ms + AE_FINALIZE_TIMEOUT_MS;
        job->stage = 0;
    }
    return AE_AGAIN;
}

static ae_result_t owned_metadata(ae_engine_t *engine, const ae_id_t *principal,
    const ae_id_t *object, ae_record_metadata_t *output, uint64_t now_ms, ae_authority_operation_t operation)
{
    if (ae_id_empty(principal) || ae_id_empty(object) || !output) return AE_INVALID;
    ae_result_t result = store_ready(engine);
    if (result != AE_OK) return result;
    ae_job_t request = {.request = {.principal_id = *principal, .recording_id = *object,
        .kind = AE_PLAYBACK_RECORDING}};
    result = ae_authorize(engine, &request, operation, now_ms);
    if (result != AE_OK) return result;
    ae_record_metadata_t metadata = {0};
    result = engine->dependencies.store.get(engine->dependencies.store.context, object, &metadata);
    if (result != AE_OK) return result;
    if (!ae_id_equal(object, &metadata.object_id)) return AE_INTEGRITY;
    if (!ae_id_equal(principal, &metadata.principal_id)) return AE_DENIED;
    if (metadata.access_revoked && operation != AE_AUTH_RECORD_DELETE) return AE_DENIED;
    uint64_t seconds;
    result = ae_store_time(engine, &seconds);
    if (result != AE_OK) return result;
    if (seconds >= metadata.expires_unix_seconds && operation != AE_AUTH_RECORD_DELETE) return AE_DENIED;
    *output = metadata;
    return AE_OK;
}

ae_result_t ae_record_get(ae_engine_t *engine, const ae_id_t *principal, const ae_id_t *object,
    ae_record_metadata_t *output, uint64_t now_ms)
{
    if (!engine || !ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    return owned_metadata(engine, principal, object, output, now_ms, AE_AUTH_RECORD_READ);
}

ae_result_t ae_record_list(ae_engine_t *engine, const ae_id_t *principal, uint32_t cursor,
    ae_record_metadata_t *items, size_t capacity, size_t *count, uint32_t *next, uint64_t now_ms)
{
    if (!engine || ae_id_empty(principal) || !items || !count || !next || !capacity || capacity > AE_LIST_MAX)
        return AE_INVALID;
    if (!ae_clock(engine, now_ms)) return AE_TIME_UNKNOWN;
    ae_result_t result = store_ready(engine);
    if (result != AE_OK) return result;
    ae_job_t request = {.request = {.principal_id = *principal, .kind = AE_PLAYBACK_RECORDING}};
    result = ae_authorize(engine, &request, AE_AUTH_RECORD_READ, now_ms);
    if (result != AE_OK) return result;
    uint64_t seconds;
    result = ae_store_time(engine, &seconds);
    if (result != AE_OK) return result;
    *count = 0;
    result = engine->dependencies.store.list(engine->dependencies.store.context, principal,
        cursor, items, capacity, count, next);
    if (result != AE_OK) return result;
    if (*count > capacity) { ae_secure_zero(items, capacity * sizeof(*items)); *count = 0; return AE_INTEGRITY; }
    size_t retained = 0;
    for (size_t i = 0; i < *count; ++i) {
        if (!ae_id_equal(&items[i].principal_id, principal)) {
            ae_secure_zero(items, capacity * sizeof(*items)); *count = 0; return AE_INTEGRITY;
        }
        if (items[i].access_revoked || seconds >= items[i].expires_unix_seconds) continue;
        items[retained++] = items[i];
    }
    ae_secure_zero(items + retained, (capacity - retained) * sizeof(*items));
    *count = retained;
    return AE_OK;
}

static bool playable(const ae_record_metadata_t *metadata)
{
    return metadata->state == AE_RECORD_COMPLETED && metadata->pcm_bytes && metadata->integrity_verified &&
        metadata->sync_acknowledged && metadata->hash_available &&
        metadata->verified_pcm_bytes == metadata->pcm_bytes && metadata->synced_pcm_bytes == metadata->pcm_bytes;
}

ae_result_t ae_record_read(ae_engine_t *engine, const ae_id_t *principal, const ae_id_t *object,
    uint64_t offset, void *output, size_t capacity, size_t *actual, uint64_t now_ms)
{
    if (!engine || !output || !actual || !capacity || capacity > 8192) return AE_INVALID;
    ae_record_metadata_t metadata;
    ae_result_t result = ae_record_get(engine, principal, object, &metadata, now_ms);
    if (result != AE_OK) return result;
    if (!playable(&metadata) || metadata.export_policy != AE_EXPORT_OWNER_DOWNLOAD) return AE_DENIED;
    uint64_t total = metadata.pcm_bytes + AE_WAV_HEADER_BYTES;
    if (offset >= total || capacity > total - offset) return AE_INVALID;
    *actual = 0;
    result = engine->dependencies.store.read(engine->dependencies.store.context, object, offset, output, capacity, actual);
    if (result == AE_OK && *actual != capacity) result = AE_INTEGRITY;
    if (result == AE_OK) {
        ae_record_metadata_t current;
        uint64_t after = engine->dependencies.monotonic_ms ? engine->dependencies.monotonic_ms(engine->dependencies.policy_context) : now_ms;
        result = ae_record_get(engine, principal, object, &current, after);
        if (result == AE_OK && (current.revision != metadata.revision || ae_cancel_latched(engine))) result = AE_DENIED;
    }
    if (result != AE_OK) { ae_secure_zero(output, capacity); *actual = 0; }
    return result;
}

ae_result_t ae_record_delete(ae_engine_t *engine, const ae_id_t *principal, const ae_id_t *object,
    uint64_t revision, bool *revoked, bool *complete, uint64_t now_ms)
{
    if (!engine || !revoked || !complete || !ae_clock(engine, now_ms)) return AE_INVALID;
    ae_job_t *record = &engine->capture[AE_CAPTURE_RECORDING];
    if ((ae_job_active(record) || record->view.cleanup_pending) && ae_id_equal(object, &record->view.recording_id))
        return AE_BUSY;
    if (ae_job_active(&engine->playback) && ae_id_equal(object, &engine->playback.view.recording_id)) return AE_BUSY;
    ae_record_metadata_t metadata;
    ae_result_t result = owned_metadata(engine, principal, object, &metadata, now_ms, AE_AUTH_RECORD_DELETE);
    if (result != AE_OK) return result;
    if (revision != metadata.revision && !(metadata.access_revoked && revision != UINT64_MAX && revision + 1 == metadata.revision)) return AE_STALE;
    *revoked = *complete = false;
    result = engine->dependencies.store.remove(engine->dependencies.store.context, object, revision, revoked, complete);
    return result == AE_OK && (!*revoked || !*complete) ? AE_UNCERTAIN : result;
}

ae_result_t ae_record_source(ae_engine_t *engine, ae_job_t *job, uint64_t now_ms)
{
    ae_record_metadata_t metadata;
    ae_result_t result = owned_metadata(engine, &job->request.principal_id, &job->request.recording_id,
        &metadata, now_ms, AE_AUTH_RECORD_READ);
    if (result != AE_OK) return result;
    if (!playable(&metadata) || metadata.format.sample_rate_hz != job->request.format.sample_rate_hz ||
        metadata.format.channels != job->request.format.channels || metadata.format.encoding != job->request.format.encoding)
        return AE_NOT_SUPPORTED;
    size_t bytes; uint32_t samples;
    if (ae_format_size(&job->request.format, &bytes, &samples) != AE_OK || metadata.pcm_bytes % bytes)
        return AE_NOT_SUPPORTED;
    if (job->view.state == AE_JOB_EMPTY) {
        uint8_t header[AE_WAV_HEADER_BYTES]; size_t actual = 0;
        result = engine->dependencies.store.read(engine->dependencies.store.context, &metadata.object_id,
            0, header, sizeof(header), &actual);
        if (result != AE_OK) return result;
        ae_wav_info_t wav;
        if (actual != sizeof(header) || ae_wav_inspect(header, metadata.pcm_bytes + AE_WAV_HEADER_BYTES, &wav) != AE_OK ||
            wav.pcm_bytes != metadata.pcm_bytes || wav.sample_rate_hz != metadata.format.sample_rate_hz ||
            wav.channels != metadata.format.channels) return AE_INTEGRITY;
        job->metadata = metadata;
        return AE_OK;
    }
    if (metadata.revision != job->metadata.revision) return AE_STALE;
    if (job->playback_offset == metadata.pcm_bytes) {
        ae_end_job(engine, job, AE_STOP_REQUESTED, AE_OK, now_ms); return AE_OK;
    }
    if (job->view.received_bytes > job->request.byte_limit || bytes > job->request.byte_limit - job->view.received_bytes) {
        ae_end_job(engine, job, AE_STOP_BYTES, AE_QUOTA, now_ms); return AE_OK;
    }
    ae_frame_t *frame = &engine->playback_io_frame;
    size_t actual = 0;
    result = engine->dependencies.store.read(engine->dependencies.store.context, &metadata.object_id,
        AE_WAV_HEADER_BYTES + job->playback_offset, frame->data, bytes, &actual);
    if (result != AE_OK || actual != bytes || ae_cancel_latched(engine)) {
        ae_secure_zero(frame, sizeof(*frame));
        return result == AE_OK ? AE_INTEGRITY : result;
    }
    frame->audio_generation = engine->view.audio_generation;
    frame->consumer_generation = job->view.consumer_generation;
    frame->route_token = job->request.route_token;
    frame->format = job->request.format;
    frame->sequence = job->ring.next_sequence;
    frame->sample_offset = job->ring.next_sample_offset;
    frame->bytes = (uint16_t)bytes;
    result = ae_ring_push(&job->ring, frame);
    ae_secure_zero(frame, sizeof(*frame));
    if (result == AE_OK) { job->playback_offset += bytes; job->view.received_bytes += bytes; }
    return result;
}
