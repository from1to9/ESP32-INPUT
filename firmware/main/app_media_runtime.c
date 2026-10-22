// SPDX-License-Identifier: Apache-2.0
#include "app_media_runtime.h"
#include "app_remote_media_runtime_port.h"
#include "app_control_server.h"
#include "control_media.h"
#include "audio_media.h"
#include "board_audio_mosaico.h"
#include "bt_audio.h"
#include "usb_audio.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "freertos/idf_additions.h"
#endif
#include <limits.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

/* Mutable state is either protected by media_lock or owned by the one worker.
 * Driver/audio/speech callbacks never borrow coordinator or UI storage. */
typedef struct {
    ae_start_request_t request;
    uint64_t expires_ms, token, original_consumer_generation, channel_generation;
    ae_id_t job_id;
    bool cancel_requested;
    ae_stop_reason_t cancel_reason;
    cp_identity_t identity;
    bool valid, remote;
} local_grant_t;
typedef enum { MEDIA_IDLE, MEDIA_ADMIT, MEDIA_ROUTE_WAIT, MEDIA_START_WAIT, MEDIA_CONTROL_WAIT, MEDIA_PHYSICAL_WAIT } media_phase_t;
typedef enum { QUERY_METADATA, QUERY_LIST, QUERY_CONTENT } media_query_kind_t;
typedef struct {
    bool pending, waiting, ready, cancelled;
    media_query_kind_t kind;
    cp_identity_t identity;
    ae_id_t object;
    uint64_t revision, offset, serial, deadline_ms, completed_ms, observed_unix_seconds;
    size_t capacity, bytes, count;
    uint32_t cursor, next;
    uint8_t stage;
    ae_result_t result;
    ae_record_metadata_t metadata, records[AE_LIST_MAX];
    uint8_t data[APP_REMOTE_MEDIA_READ_BYTES];
} media_query_t;
typedef struct {
    app_media_services_t services;
    app_media_local_request_t command;
    app_remote_media_request_t remote_command;
    app_media_remote_authority_t remote_authority;
    media_query_t query;
    bool command_remote, remote_cancelled;
    uint64_t command_channel_generation;
    ae_environment_t environment;
    app_media_snapshot_t view;
    ae_id_t boot_session;
    local_grant_t grants[AE_CONSUMERS_MAX + 1];
    sl_binding_t speech_binding;
    uint64_t epoch, ui_revision, serial, pending_serial, last_token;
    uint64_t cancellation, command_cancellation, result_audio_serial;
    uint64_t bluetooth_request_id, bluetooth_deadline_ms;
    bool cancel_waiting;
    unsigned cancel_slot;
    uint64_t cancel_serial;
    bool speech_context, pending, waiting, speech_bound, speech_stop_sent, execution_checked;
    media_phase_t phase;
    ae_frame_t incoming;
    /* Audio-worker capture scratch and separately claimed HTTP ingress scratch. */
    uint8_t network_capture[CP_MEDIA_FRAME_BYTES];
    ae_frame_t network_playback;
    bool network_playback_busy;
    /* Remote submitters can run on different tasks. Only the claimant may
     * access this fixed PSRAM scratch; Bluetooth callbacks run outside our lock. */
    bt_audio_bond_list_t remote_bonds;
    bool remote_bonds_busy;
} media_state_t;
static portMUX_TYPE media_lock = portMUX_INITIALIZER_UNLOCKED;
static media_state_t *media;
static bool media_attempted;
/* This is a local principal namespace, not a credential. Remote clients never
 * acquire it merely by claiming the bytes. Each capture also needs a private
 * current-boot session and an immutable locally admitted grant. */
static const ae_id_t local_principal = {.bytes = {0x45,0x53,0x50,0x33,0x32,0x2d,0x4c,0x4f,0x43,0x41,0x4c,0,0,0,0,1}};

static uint64_t clock_ms(void *context)
{ (void)context; return (uint64_t)esp_timer_get_time() / 1000U; }
static ae_result_t random_id(void *context, ae_id_t *id)
{
    (void)context;
    esp_fill_random(id->bytes, sizeof id->bytes);
    return ae_id_empty(id) ? AE_INTEGRITY : AE_OK;
}
static ae_result_t wall_time(void *context, uint64_t *seconds)
{
    const media_state_t *state = context;
    return state->services.wall_time ? state->services.wall_time(state->services.clock_context, seconds) : AE_TIME_UNKNOWN;
}
static ae_route_port_t route_port(ae_route_t route)
{
    if (route == AE_ROUTE_USB_UAC) return usb_audio_route_port();
    if (route == AE_ROUTE_HFP_HF || route == AE_ROUTE_A2DP_SINK) return bt_audio_route_port();
    return (ae_route_port_t){0};
}
static ae_result_t route_caps(void *context, ae_route_t route, ae_route_capabilities_t *caps)
{
    (void)context;
    if (!caps) return AE_INVALID;
    if (route == AE_ROUTE_LOCAL) {
        const board_audio_status_t board = board_audio_mosaico_status();
        *caps = (ae_route_capabilities_t){.implemented = true, .available = board.hardware_observed,
            .connected = board.hardware_observed, .capture = true, .playback = true,
            .format = {16000, 1, 10, AE_PCM_S16_LE}, .reason = board.last_result};
        return AE_OK;
    }
    ae_route_port_t port = route_port(route);
    if (!port.capabilities) { *caps = (ae_route_capabilities_t){.reason = AE_UNAVAILABLE}; return AE_UNAVAILABLE; }
    return port.capabilities(port.context, route, caps);
}
static ae_result_t route_prepare(void *context, ae_route_t route, const ae_format_t *format,
    uint8_t roles, uint64_t generation, const ae_route_token_t *token)
{
    (void)context;
    ae_route_port_t port = route_port(route);
    return port.prepare ? port.prepare(port.context, route, format, roles, generation, token) : AE_UNAVAILABLE;
}
static ae_result_t route_capture(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context; ae_route_port_t port = route_port(route);
    return port.capture ? port.capture(port.context, route, frame) : AE_UNAVAILABLE;
}
static ae_result_t route_stop(void *context, ae_route_t route, uint64_t generation, const ae_route_token_t *token)
{
    (void)context; ae_route_port_t port = route_port(route);
    return port.stop ? port.stop(port.context, route, generation, token) : AE_UNAVAILABLE;
}
static ae_result_t route_bind_playback(void *context, ae_route_t route, uint64_t generation,
    const ae_route_token_t *token, uint64_t consumer, bool enabled)
{
    (void)context; ae_route_port_t port=route_port(route);
    return port.bind_playback ? port.bind_playback(port.context,route,generation,token,consumer,enabled) : AE_NOT_SUPPORTED;
}
static void route_consumed(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context; ae_route_port_t port = route_port(route);
    if (port.playback_consumed) port.playback_consumed(port.context, route, frame);
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
        a->export_policy == b->export_policy && a->storage == b->storage &&
        a->disconnect_policy == b->disconnect_policy && a->activate_microphone == b->activate_microphone;
}
static bool remote_alive(const app_media_remote_authority_t *port, const cp_identity_t *identity)
{
    return port->session_alive && port->session_alive(port->context, identity);
}
static bool network_request(const ae_start_request_t *request)
{
    return request->route==AE_ROUTE_LOCAL &&
        (request->kind==AE_CAPTURE_LIVE || request->kind==AE_PLAYBACK_STREAM);
}
static ae_result_t remote_grant_authority(const app_media_remote_authority_t *port,
    const local_grant_t *grant, uint64_t now, ae_authority_t *output)
{
    const ae_start_request_t *r=&grant->request;
    bool alive=remote_alive(port,&grant->identity);
    if (network_request(r)) alive=alive && grant->channel_generation &&
        app_control_server_channel_generation(&grant->identity)==grant->channel_generation;
    if (r->kind>=AE_PLAYBACK_STREAM) {
        bool allowed=(grant->identity.scopes&CP_SCOPE_AUDIO_PLAYBACK) &&
            (r->kind!=AE_PLAYBACK_RECORDING || (grant->identity.scopes&CP_SCOPE_RECORDING_READ));
        *output=(ae_authority_t){.allowed=allowed,.owner_session_alive=alive,.reason=AE_DENIED};
        return AE_OK;
    }
    if (!port->capture_authority) return AE_UNAVAILABLE;
    cp_capture_authority_request_t query={.identity=grant->identity,.grant_revision=r->consent_revision,
        .background_grant_revision=r->background_grant_revision,.bytes=r->byte_limit,.duration_seconds=r->duration_seconds,
        .retention_seconds=r->retention_seconds,.purpose=r->kind==AE_CAPTURE_RECORDING ? CP_CAPTURE_RECORDING :
            r->kind==AE_CAPTURE_RECOGNITION ? CP_CAPTURE_SPEECH : CP_CAPTURE_LIVE,
        .live_destination=r->kind==AE_CAPTURE_LIVE,.file_destination=r->kind==AE_CAPTURE_RECORDING,
        .owner_download=r->export_policy==AE_EXPORT_OWNER_DOWNLOAD,.continue_local=r->disconnect_policy==AE_DISCONNECT_CONTINUE_LOCAL};
    memcpy(query.grant_id,r->consent_id.bytes,CP_ID_BYTES);
    memcpy(query.background_grant_id,r->background_grant_id.bytes,CP_ID_BYTES);
    cp_capture_authority_t allowed={0};
    cp_error_t result=port->capture_authority(port->context,&query,&allowed);
    if (result!=CP_OK) return result==CP_UNAVAILABLE ? AE_UNAVAILABLE : AE_DENIED;
    *output=(ae_authority_t){.allowed=allowed.allowed && allowed.source_valid_until_ms>now,
        .owner_session_alive=alive && allowed.owner_session_alive,.background_allowed=allowed.background_allowed,
        .source_valid_until_ms=allowed.source_valid_until_ms,.background_valid_until_ms=allowed.background_valid_until_ms,.reason=AE_DENIED};
    return AE_OK;
}
static ae_result_t authority(void *context, ae_authority_operation_t operation,
    const ae_start_request_t *request, uint64_t now, ae_authority_t *output)
{
    media_state_t *state=context;
    if (!request || !output) return AE_INVALID;
    *output=(ae_authority_t){.reason=AE_DENIED};
    local_grant_t matched={0}; app_remote_media_request_t query={0};
    cp_identity_t query_identity={0}; ae_id_t query_object={0};
    bool query_active=false, query_list=false;
    portENTER_CRITICAL(&media_lock);
    bool environment=state->environment.unlocked && !state->environment.privacy_latched && !state->environment.maintenance;
    bool local=ae_id_equal(&request->principal_id,&local_principal);
    app_media_remote_authority_t remote=state->remote_authority;
    if (environment) for (unsigned i=0;i<AE_CONSUMERS_MAX+1;++i) {
        const local_grant_t *grant=&state->grants[i];
        bool exact=same_request(&grant->request,request);
        bool file=operation==AE_AUTH_RECORD_READ && grant->request.kind==AE_PLAYBACK_RECORDING &&
            ae_id_equal(&grant->request.principal_id,&request->principal_id) &&
            ae_id_equal(&grant->request.recording_id,&request->recording_id);
        if (grant->valid && (exact || file) && (grant->remote || now<grant->expires_ms)) { matched=*grant; break; }
    }
    if (environment && state->command_remote && !state->remote_cancelled &&
        (state->pending || state->view.result_ready)) query=state->remote_command;
    if (state->query.pending && !state->query.cancelled) {
        query_active=true; query_identity=state->query.identity; query_object=state->query.object;
        query_list=state->query.kind==QUERY_LIST;
    }
    portEXIT_CRITICAL(&media_lock);
    if (!environment) return AE_OK;
    if (local) {
        bool allowed=operation>=AE_AUTH_RECORD_READ || (matched.valid && !matched.remote);
        *output=(ae_authority_t){.allowed=allowed,.owner_session_alive=allowed,
            .source_valid_until_ms=operation>=AE_AUTH_RECORD_READ ? UINT64_MAX : matched.expires_ms,.reason=AE_DENIED};
        return AE_OK;
    }
    if (operation>=AE_AUTH_RECORD_READ) {
        cp_identity_t identity={0}; bool allowed=false;
        if (matched.valid && matched.remote && matched.request.kind==AE_PLAYBACK_RECORDING && operation==AE_AUTH_RECORD_READ) {
            identity=matched.identity; allowed=(identity.scopes&CP_SCOPE_RECORDING_READ)!=0;
        } else if (query.token && !memcmp(query.identity.principal_id,request->principal_id.bytes,CP_ID_BYTES)) {
            identity=query.identity;
            bool list=query.action==APP_REMOTE_MEDIA_RECORD_LIST && ae_id_empty(&request->recording_id);
            bool object=ae_id_equal(&query.object_id,&request->recording_id);
            allowed=operation==AE_AUTH_RECORD_DELETE ? object && query.action==APP_REMOTE_MEDIA_RECORD_DELETE &&
                (identity.scopes&CP_SCOPE_RECORDING_DELETE) : (list || (object &&
                (query.action==APP_REMOTE_MEDIA_RECORD_GET || query.action==APP_REMOTE_MEDIA_RECORD_READ))) &&
                (identity.scopes&CP_SCOPE_RECORDING_READ);
        }
        if (!allowed && operation==AE_AUTH_RECORD_READ && query_active &&
            !memcmp(query_identity.principal_id,request->principal_id.bytes,CP_ID_BYTES) &&
            (query_identity.scopes&CP_SCOPE_RECORDING_READ) &&
            ((query_list && ae_id_empty(&request->recording_id)) || ae_id_equal(&query_object,&request->recording_id))) {
            identity=query_identity; allowed=true;
        }
        allowed=allowed && remote_alive(&remote,&identity);
        *output=(ae_authority_t){.allowed=allowed,.owner_session_alive=allowed,.reason=AE_DENIED}; return AE_OK;
    }
    return matched.valid && matched.remote ? remote_grant_authority(&remote,&matched,now,output) : AE_OK;
}

static bool network_binding(media_state_t *state, ae_job_kind_t kind, const cp_identity_t *identity,
    const uint8_t job_id[CP_ID_BYTES], uint64_t consumer, uint64_t channel, local_grant_t *output, ae_snapshot_t *snapshot)
{
    if (!state || !identity || !job_id || !consumer || !channel ||
        (kind!=AE_CAPTURE_LIVE && kind!=AE_PLAYBACK_STREAM)) return false;
    const unsigned slot=kind==AE_CAPTURE_LIVE ? AE_CAPTURE_LIVE : AE_CONSUMERS_MAX;
    portENTER_CRITICAL(&media_lock);
    local_grant_t grant=state->grants[slot];
    app_media_remote_authority_t policy=state->remote_authority;
    bool allowed=state->environment.unlocked && !state->environment.privacy_latched && !state->environment.maintenance &&
        grant.remote && grant.valid && grant.request.kind==kind && grant.request.route==AE_ROUTE_LOCAL &&
        grant.original_consumer_generation==consumer && grant.channel_generation==channel && !memcmp(grant.job_id.bytes,job_id,CP_ID_BYTES) &&
        !memcmp(grant.identity.principal_id,identity->principal_id,CP_ID_BYTES) &&
        !memcmp(grant.identity.session_id,identity->session_id,CP_ID_BYTES) &&
        (identity->scopes&(kind==AE_CAPTURE_LIVE?CP_SCOPE_AUDIO_CAPTURE:CP_SCOPE_AUDIO_PLAYBACK));
    portEXIT_CRITICAL(&media_lock);
    if (!allowed || !app_audio_snapshot(snapshot)) return false;
    const ae_job_snapshot_t *job=kind==AE_CAPTURE_LIVE ? &snapshot->capture[AE_CAPTURE_LIVE] : &snapshot->playback;
    if (snapshot->route!=AE_ROUTE_LOCAL || job->kind!=kind ||
        (job->state!=AE_JOB_STARTING && job->state!=AE_JOB_RUNNING) ||
        !ae_id_equal(&job->job_id,&grant.job_id) || !ae_id_equal(&job->request_id,&grant.request.request_id) ||
        job->consumer_generation!=consumer || !ae_format_equal(&snapshot->format,&grant.request.format)) return false;
    ae_authority_t authority={0};
    if (remote_grant_authority(&policy,&grant,clock_ms(NULL),&authority)!=AE_OK ||
        !authority.allowed || !authority.owner_session_alive) return false;
    /* Revocation may have happened during the external authority lookup. */
    portENTER_CRITICAL(&media_lock);
    allowed=state->environment.unlocked && !state->environment.privacy_latched && !state->environment.maintenance &&
        state->grants[slot].valid && state->grants[slot].token==grant.token &&
        ae_id_equal(&state->grants[slot].job_id,&grant.job_id);
    portEXIT_CRITICAL(&media_lock);
    if (allowed && output) *output=grant;
    return allowed;
}
static bool network_capture_allowed(void *context, const cp_identity_t *identity,
    const uint8_t job_id[CP_ID_BYTES], uint64_t consumer, uint64_t channel)
{
    ae_snapshot_t snapshot;
    return network_binding(context,AE_CAPTURE_LIVE,identity,job_id,consumer,channel,NULL,&snapshot);
}
static void network_capture_failed(void *context, const cp_identity_t *identity,
    const uint8_t job_id[CP_ID_BYTES], uint64_t consumer, uint64_t channel, cp_error_t error)
{
    media_state_t *state=context;
    if (!state || !identity || !job_id || !channel || error==CP_OK) return;
    bool matching=false;
    portENTER_CRITICAL(&media_lock);
    local_grant_t *grant=&state->grants[AE_CAPTURE_LIVE];
    if (grant->remote && network_request(&grant->request) && grant->original_consumer_generation==consumer && grant->channel_generation==channel &&
        !memcmp(grant->job_id.bytes,job_id,CP_ID_BYTES) &&
        !memcmp(grant->identity.principal_id,identity->principal_id,CP_ID_BYTES) &&
        !memcmp(grant->identity.session_id,identity->session_id,CP_ID_BYTES)) {
        grant->valid=false; matching=true;
    }
    portEXIT_CRITICAL(&media_lock);
    if (matching) app_control_server_capture_cancel(identity,job_id,consumer,channel);
}
static cp_error_t network_playback_receive(void *context, const cp_identity_t *identity, uint64_t channel, const uint8_t *packet, size_t bytes)
{
    media_state_t *state=context; cp_media_header_t header;
    if (!state || !identity || !cp_media_decode(packet,bytes,&header) || header.kind!=CP_MEDIA_PLAYBACK) return CP_INVALID;
    ae_snapshot_t snapshot;
    if (!network_binding(state,AE_PLAYBACK_STREAM,identity,header.job_id,header.consumer_generation,channel,NULL,&snapshot)) return CP_FORBIDDEN;
    ae_format_t format={header.sample_rate_hz,header.channels,header.frame_ms,AE_PCM_S16_LE};
    if (header.audio_generation!=snapshot.audio_generation || !ae_format_equal(&format,&snapshot.format)) return CP_STALE;
    portENTER_CRITICAL(&media_lock);
    bool busy=state->network_playback_busy;
    if (!busy) state->network_playback_busy=true;
    portEXIT_CRITICAL(&media_lock);
    if (busy) return CP_BUSY;
    ae_frame_t *frame=&state->network_playback;
    *frame=(ae_frame_t){.audio_generation=header.audio_generation,.consumer_generation=header.consumer_generation,
        .sequence=header.sequence,.sample_offset=header.sample_offset,.format=format,.bytes=header.bytes};
    memcpy(frame->data,packet+CP_MEDIA_HEADER_BYTES,header.bytes);
    ae_result_t result=app_audio_try_submit_playback(frame);
    ae_secure_zero(frame,sizeof(*frame));
    portENTER_CRITICAL(&media_lock); state->network_playback_busy=false; portEXIT_CRITICAL(&media_lock);
    if (result==AE_OK) return CP_OK;
    if (result==AE_AGAIN || result==AE_BUSY) return CP_BUSY;
    if (result==AE_STALE) return CP_STALE;
    if (result==AE_INVALID) return CP_INVALID;
    if (result==AE_DENIED || result==AE_PRIVACY) return CP_FORBIDDEN;
    return CP_UNAVAILABLE;
}
static void network_channel_lost(void *context, const cp_identity_t *identity, uint64_t channel)
{
    media_state_t *state=context;
    if (!state || !identity || !channel) return;
    local_grant_t capture={0};
    portENTER_CRITICAL(&media_lock);
    for (unsigned i=0;i<=AE_CONSUMERS_MAX;++i) {
        local_grant_t *grant=&state->grants[i];
        if (grant->remote && network_request(&grant->request) && grant->channel_generation==channel &&
            !memcmp(grant->identity.principal_id,identity->principal_id,CP_ID_BYTES) &&
            !memcmp(grant->identity.session_id,identity->session_id,CP_ID_BYTES)) {
            grant->valid=false;
            if (i==AE_CAPTURE_LIVE) capture=*grant;
        }
    }
    /* A close before START reaches the audio owner must also cancel that exact
     * admission. A replacement channel never inherits the pending command. */
    if (state->command_remote && state->remote_command.action==APP_REMOTE_MEDIA_START &&
        network_request(&state->remote_command.start) && state->command_channel_generation==channel &&
        !memcmp(state->remote_command.identity.principal_id,identity->principal_id,CP_ID_BYTES) &&
        !memcmp(state->remote_command.identity.session_id,identity->session_id,CP_ID_BYTES)) state->remote_cancelled=true;
    portEXIT_CRITICAL(&media_lock);
    if (!ae_id_empty(&capture.job_id) && capture.original_consumer_generation)
        app_control_server_capture_cancel(&capture.identity,capture.job_id.bytes,capture.original_consumer_generation,channel);
}
static ae_result_t network_capture_send(media_state_t *state, const ae_id_t *job, const ae_frame_t *frame)
{
    local_grant_t grant;
    portENTER_CRITICAL(&media_lock); grant=state->grants[AE_CAPTURE_LIVE]; portEXIT_CRITICAL(&media_lock);
    if (!grant.remote || !grant.valid) return AE_DENIED;
    /* app_audio can produce before the control worker has consumed START's
     * result. Keep the exact frame queued until its identity is bound. */
    if (ae_id_empty(&grant.job_id)) return AE_AGAIN;
    ae_snapshot_t snapshot;
    if (!network_binding(state,AE_CAPTURE_LIVE,&grant.identity,job->bytes,frame->consumer_generation,grant.channel_generation,NULL,&snapshot)) return AE_DENIED;
    if (frame->audio_generation!=snapshot.audio_generation || !ae_format_equal(&frame->format,&grant.request.format)) return AE_STALE;
    cp_media_header_t header={.kind=CP_MEDIA_CAPTURE,.audio_generation=frame->audio_generation,
        .consumer_generation=frame->consumer_generation,.sequence=frame->sequence,.sample_offset=frame->sample_offset,
        .sample_rate_hz=frame->format.sample_rate_hz,.channels=frame->format.channels,.frame_ms=frame->format.frame_ms,.bytes=frame->bytes};
    memcpy(header.job_id,job->bytes,CP_ID_BYTES); size_t bytes=0;
    if (!cp_media_encode(&header,frame->data,state->network_capture,sizeof(state->network_capture),&bytes)) return AE_INVALID;
    esp_err_t result=app_control_server_capture_send(&grant.identity,job->bytes,frame->consumer_generation,grant.channel_generation,state->network_capture,bytes);
    ae_secure_zero(state->network_capture,sizeof(state->network_capture));
    if (result==ESP_OK) return AE_OK; /* Queue receipt only; async failure revokes this exact grant. */
    return result==ESP_ERR_NO_MEM || result==ESP_ERR_INVALID_STATE || result==ESP_ERR_TIMEOUT ? AE_AGAIN : AE_IO_ERROR;
}

static ae_result_t capture_sink(void *context, ae_job_kind_t kind, const ae_id_t *job, const ae_frame_t *frame)
{
    media_state_t *state = context;
    if (!job || !frame) return AE_INVALID;
    if (kind==AE_CAPTURE_LIVE) return network_capture_send(state,job,frame);
    if (kind!=AE_CAPTURE_RECOGNITION) return AE_NOT_SUPPORTED;
    portENTER_CRITICAL(&media_lock);
    bool allowed = state->speech_bound && state->speech_context &&
        state->environment.unlocked && !state->environment.privacy_latched && !state->environment.maintenance &&
        state->grants[AE_CAPTURE_RECOGNITION].valid &&
        state->epoch == state->speech_binding.epoch && state->ui_revision == state->speech_binding.ui_revision &&
        ae_id_equal(job, &state->speech_binding.job_id) &&
        frame->consumer_generation == state->speech_binding.consumer_generation;
    portEXIT_CRITICAL(&media_lock);
    return allowed ? app_spoken_letters_capture(job, frame) : AE_AGAIN;
}
static void capture_closed(void *context, ae_job_kind_t kind, const ae_id_t *job, uint64_t generation)
{
    media_state_t *state=context;
    if (kind == AE_CAPTURE_RECOGNITION) app_spoken_letters_capture_closed(job, generation);
    if (kind==AE_CAPTURE_LIVE && job) {
        portENTER_CRITICAL(&media_lock); local_grant_t grant=state->grants[AE_CAPTURE_LIVE]; portEXIT_CRITICAL(&media_lock);
        if (grant.remote && network_request(&grant.request) && grant.channel_generation &&
            ae_id_equal(&grant.job_id,job) && grant.original_consumer_generation==generation)
            app_control_server_capture_cancel(&grant.identity,job->bytes,generation,grant.channel_generation);
    }
}
static bool active_job(const ae_job_snapshot_t *job)
{ return job->state == AE_JOB_STARTING || job->state == AE_JOB_RUNNING || job->cleanup_pending; }

static void finish(media_state_t *state, ae_result_t result)
{
    portENTER_CRITICAL(&media_lock);
    state->view.request_token = state->command.token;
    state->view.result = result; state->view.result_ready = true; state->view.busy = false;
    state->pending = false; state->waiting = false; state->phase = MEDIA_IDLE;
    state->bluetooth_request_id = 0;
    ae_secure_zero(&state->command, sizeof state->command);
    portEXIT_CRITICAL(&media_lock);
}
static ae_result_t send_audio(media_state_t *state, app_audio_command_t *command, media_phase_t phase)
{
    if (state->serial == UINT64_MAX) return AE_EXHAUSTED;
    command->request_token = ++state->serial;
    ae_result_t result = app_audio_submit(command);
    if (result == AE_OK) { state->pending_serial = command->request_token; state->waiting = true; state->phase = phase; }
    return result;
}
static ae_result_t start_job(media_state_t *state, const ae_snapshot_t *audio)
{
    const app_media_local_request_t *command = &state->command;
    ae_route_capabilities_t route;
    ae_result_t result = route_caps(NULL, command->route, &route);
    if (result != AE_OK || !route.available || !route.connected) return AE_UNAVAILABLE;
    const unsigned slot = command->kind <= AE_CAPTURE_RECOGNITION ? (unsigned)command->kind : AE_CONSUMERS_MAX;
    if (active_job(slot == AE_CONSUMERS_MAX ? &audio->playback : &audio->capture[slot])) return AE_BUSY;
    app_audio_command_t request = {.kind = APP_AUDIO_CMD_START};
    ae_start_request_t *start = &request.start;
    *start = (ae_start_request_t){.principal_id = local_principal, .owner_session_id = state->boot_session,
        .expected_audio_generation = audio->audio_generation, .expected_revision = audio->revision,
        .kind = command->kind, .route = command->route, .route_token = route.token,
        .format = command->format, .duration_seconds = command->duration_seconds,
        .byte_limit = command->byte_limit, .retention_seconds = command->retention_seconds,
        .export_policy = command->export_policy, .storage = command->storage,
        .disconnect_policy = AE_DISCONNECT_STOP, .activate_microphone = command->kind <= AE_CAPTURE_RECOGNITION,
        .recording_id = command->recording_id};
    if (state->command_remote) {
        *start=state->remote_command.start;
        if (network_request(start) && (!state->command_channel_generation ||
            app_control_server_channel_generation(&state->remote_command.identity)!=state->command_channel_generation)) return AE_UNAVAILABLE;
    }
    if (!state->command_remote && (random_id(NULL, &start->request_id) != AE_OK || random_id(NULL, &start->consent_id) != AE_OK)) return AE_INTEGRITY;
    if (!state->command_remote) start->consent_revision = 1;
    const uint64_t now = clock_ms(NULL), lifetime = (uint64_t)start->duration_seconds * 1000U + AE_START_TIMEOUT_MS;
    if (now > UINT64_MAX - lifetime) return AE_EXHAUSTED;
    portENTER_CRITICAL(&media_lock);
    if (state->cancellation != state->command_cancellation || state->remote_cancelled || !state->environment.unlocked ||
        state->environment.privacy_latched || state->environment.maintenance ||
        (command->kind == AE_CAPTURE_RECOGNITION && (!state->speech_context ||
            state->epoch != command->epoch || state->ui_revision != command->ui_revision))) {
        portEXIT_CRITICAL(&media_lock); return AE_PRIVACY;
    }
    state->grants[slot] = (local_grant_t){.request = *start, .expires_ms = now + lifetime, .valid = true,
        .remote=state->command_remote,.identity=state->remote_command.identity,.token=command->token,
        .channel_generation=state->command_remote ? state->command_channel_generation : 0};
    portEXIT_CRITICAL(&media_lock);
    result = send_audio(state, &request, MEDIA_START_WAIT);
    if (result != AE_OK) {
        portENTER_CRITICAL(&media_lock); state->grants[slot].valid = false; portEXIT_CRITICAL(&media_lock);
    }
    return result;
}
static bool role_acknowledged(const ae_snapshot_t *audio, const app_media_local_request_t *command)
{
    ae_role_t role=command->role;
    if (command->action==APP_MEDIA_STOP_JOB) {
        const ae_job_snapshot_t *job=NULL;
        for (unsigned i=0;i<AE_CONSUMERS_MAX;++i)
            if (ae_id_equal(&audio->capture[i].job_id,&command->job_id)) job=&audio->capture[i];
        if (ae_id_equal(&audio->playback.job_id,&command->job_id)) job=&audio->playback;
        if (!job) return false;
        role=job->kind<=AE_CAPTURE_RECOGNITION ? AE_MICROPHONE : AE_SPEAKER;
        if (role==AE_MICROPHONE) for (unsigned i=0;i<AE_CONSUMERS_MAX;++i)
            if (!ae_id_equal(&audio->capture[i].job_id,&job->job_id) &&
                (audio->capture[i].state==AE_JOB_STARTING || audio->capture[i].state==AE_JOB_RUNNING)) return true;
    }
    return role==AE_MICROPHONE ? audio->microphone_muted && audio->microphone_mute_confirmed :
        audio->speaker_muted && audio->speaker_mute_confirmed;
}
static void process_command(media_state_t *state, const ae_snapshot_t *audio)
{
    const app_media_local_request_t *command = &state->command;
    portENTER_CRITICAL(&media_lock);
    const bool cancelled = (state->cancellation != state->command_cancellation || state->remote_cancelled) && command->action != APP_MEDIA_STOP;
    portEXIT_CRITICAL(&media_lock);
    /* Drain a submitted audio result before accepting a replacement command. */
    if (cancelled && !state->waiting) {
        if (state->bluetooth_request_id) (void)bt_audio_cancel();
        finish(state, AE_PRIVACY); return;
    }
    if (!state->execution_checked) {
        state->execution_checked = true;
        if (state->command_remote && (clock_ms(NULL)>=state->remote_command.deadline_ms ||
            !remote_alive(&state->remote_authority,&state->remote_command.identity))) {
            finish(state,AE_DENIED); return;
        }
        if ((command->expected_audio_generation && command->expected_audio_generation != audio->audio_generation) ||
            (command->expected_audio_revision && command->expected_audio_revision != audio->revision)) {
            finish(state, AE_STALE); return;
        }
    }
    if (state->phase==MEDIA_PHYSICAL_WAIT) {
        if (role_acknowledged(audio,command)) finish(state,AE_OK);
        else if (audio->quarantined) finish(state,AE_UNCERTAIN);
        return;
    }
    if (command->action == APP_MEDIA_LINK_CONTROL) {
        if (command->route != AE_ROUTE_HFP_HF && command->route != AE_ROUTE_A2DP_SINK) {
            finish(state, AE_NOT_SUPPORTED); return;
        }
        if (!state->bluetooth_request_id) {
            bt_audio_command_t op = command->route == AE_ROUTE_HFP_HF ?
                (command->value ? BT_AUDIO_CONNECT_HFP : BT_AUDIO_DISCONNECT_HFP) :
                (command->value ? BT_AUDIO_CONNECT_A2DP : BT_AUDIO_DISCONNECT_A2DP);
            esp_err_t error = command->bluetooth_bond_revision && command->value ?
                bt_audio_saved_request(command->bluetooth_generation, command->bluetooth_bond_revision,
                    op, command->bluetooth_peer, &state->bluetooth_request_id) :
                bt_audio_request(command->bluetooth_generation, op,
                    command->value ? command->bluetooth_peer : NULL, &state->bluetooth_request_id);
            if (error != ESP_OK) { finish(state, error == ESP_ERR_INVALID_STATE ? AE_BUSY : AE_UNAVAILABLE); return; }
            uint64_t now = clock_ms(NULL);
            if (now > UINT64_MAX - 15000U) { (void)bt_audio_cancel(); finish(state, AE_EXHAUSTED); return; }
            state->bluetooth_deadline_ms = now + 15000U;
        }
        bt_audio_snapshot_t bt = bt_audio_snapshot();
        if (bt.request_id != state->bluetooth_request_id) { finish(state, AE_STALE); return; }
        if (bt.request_status == BT_AUDIO_JOB_SUCCEEDED) finish(state, AE_OK);
        else if (bt.request_status == BT_AUDIO_JOB_FAILED) finish(state, AE_IO_ERROR);
        else if (bt.request_status == BT_AUDIO_JOB_CANCELLED) finish(state, AE_STALE);
        else if (bt.request_status == BT_AUDIO_JOB_UNCERTAIN) finish(state, AE_UNCERTAIN);
        else if (clock_ms(NULL) >= state->bluetooth_deadline_ms) { (void)bt_audio_cancel(); finish(state, AE_TIMEOUT); }
        return;
    }
    if (state->waiting) {
        app_audio_result_t result;
        if (!app_audio_take_result(&result)) return;
        state->waiting = false;
        if (result.request_token != state->pending_serial) { finish(state, AE_STALE); return; }
        if (state->phase==MEDIA_START_WAIT && result.result==AE_OK) {
            const unsigned slot=command->kind<=AE_CAPTURE_RECOGNITION ? (unsigned)command->kind : AE_CONSUMERS_MAX;
            portENTER_CRITICAL(&media_lock);
            local_grant_t *grant=&state->grants[slot];
            if (ae_id_equal(&result.job.request_id,&grant->request.request_id)) {
                grant->job_id=result.job.job_id; grant->original_consumer_generation=result.job.consumer_generation;
            }
            portEXIT_CRITICAL(&media_lock);
        }
        if (cancelled) { finish(state, AE_PRIVACY); return; }
        portENTER_CRITICAL(&media_lock);
        state->view.result_detail = result;
        state->result_audio_serial = result.request_token;
        portEXIT_CRITICAL(&media_lock);
        if (result.result != AE_OK) { finish(state, result.result); return; }
        if (state->phase == MEDIA_ROUTE_WAIT && command->action == APP_MEDIA_START) {
            state->phase = MEDIA_ADMIT; return;
        }
        if (state->phase == MEDIA_START_WAIT && command->kind == AE_CAPTURE_RECOGNITION) {
            ae_snapshot_t current;
            if (!app_audio_snapshot(&current)) { finish(state, AE_UNAVAILABLE); return; }
            sl_binding_t binding = {.job_id = result.job.job_id, .token = command->token,
                .epoch = command->epoch, .ui_revision = command->ui_revision,
                .audio_generation = current.audio_generation, .consumer_generation = result.job.consumer_generation,
                .duration_ms = command->duration_seconds * 1000U};
            portENTER_CRITICAL(&media_lock);
            bool valid = state->cancellation == state->command_cancellation && state->speech_context &&
                state->epoch == binding.epoch && state->ui_revision == binding.ui_revision &&
                state->environment.unlocked && !state->environment.privacy_latched && !state->environment.maintenance;
            state->speech_bound = false; /* PCM cannot enter before the exact context is rechecked. */
            portEXIT_CRITICAL(&media_lock);
            ae_result_t begun = valid ? app_spoken_letters_begin(&binding) : AE_PRIVACY;
            portENTER_CRITICAL(&media_lock);
            valid = begun == AE_OK && state->cancellation == state->command_cancellation &&
                state->speech_context && state->epoch == binding.epoch && state->ui_revision == binding.ui_revision &&
                state->environment.unlocked && !state->environment.privacy_latched && !state->environment.maintenance;
            if (valid) {
                state->speech_binding = binding; state->speech_bound = true; state->speech_stop_sent = false;
            } else state->grants[AE_CAPTURE_RECOGNITION].valid = false;
            portEXIT_CRITICAL(&media_lock);
            if (!valid) {
                if (begun == AE_OK) app_spoken_letters_cancel(&binding.job_id, binding.consumer_generation);
                finish(state, begun == AE_OK ? AE_PRIVACY : begun); return;
            }
        }
        if (state->command_remote && (command->action==APP_MEDIA_STOP_JOB ||
            (command->action==APP_MEDIA_MUTE_ROLE && command->value) ||
            (command->action==APP_MEDIA_ENABLE_ROLE && !command->value)) && !role_acknowledged(audio,command)) {
            state->phase=MEDIA_PHYSICAL_WAIT; return;
        }
        finish(state, AE_OK); return;
    }
    if (command->action == APP_MEDIA_START && audio->route == command->route) {
        ae_result_t result = start_job(state, audio);
        if (result != AE_OK) finish(state, result);
        return;
    }
    app_audio_command_t request = {.expected_generation = audio->audio_generation, .expected_revision = audio->revision};
    switch (command->action) {
    case APP_MEDIA_START: case APP_MEDIA_ROUTE:
        request.kind = APP_AUDIO_CMD_ROUTE; request.route = command->route; break;
    case APP_MEDIA_VOLUME: request.kind = APP_AUDIO_CMD_VOLUME; request.level = command->level; break;
    case APP_MEDIA_GAIN: request.kind = APP_AUDIO_CMD_GAIN; request.level = command->level; break;
    case APP_MEDIA_RECOVER: request.kind = APP_AUDIO_CMD_RECOVER; break;
    case APP_MEDIA_ENABLE_ROLE: case APP_MEDIA_MUTE_ROLE:
        request.kind = command->action == APP_MEDIA_ENABLE_ROLE ? APP_AUDIO_CMD_ENABLE : APP_AUDIO_CMD_MUTE;
        request.role = command->role; request.value = command->value; request.format = command->format;
        break;
    case APP_MEDIA_STOP_JOB: {
        const ae_job_snapshot_t *job = NULL;
        for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i)
            if (ae_id_equal(&audio->capture[i].job_id, &command->job_id)) job = &audio->capture[i];
        if (ae_id_equal(&audio->playback.job_id, &command->job_id)) job = &audio->playback;
        if (!job || ae_id_empty(&command->job_id) || job->consumer_generation != command->consumer_generation) {
            finish(state, AE_STALE); return;
        }
        request.kind = APP_AUDIO_CMD_STOP; request.job_id = command->job_id;
        request.consumer_generation = command->consumer_generation; request.stop_reason = command->stop_reason;
        /* Public snapshots advance their frame generation when capture closes.
         * Stop retries still address the original immutable engine consumer. */
        portENTER_CRITICAL(&media_lock);
        for (unsigned i=0;i<=AE_CONSUMERS_MAX;++i)
            if (ae_id_equal(&state->grants[i].job_id,&command->job_id) && state->grants[i].original_consumer_generation)
                request.consumer_generation=state->grants[i].original_consumer_generation;
        portEXIT_CRITICAL(&media_lock);
        break;
    }
    case APP_MEDIA_QUERY_GET: case APP_MEDIA_QUERY_LIST: case APP_MEDIA_QUERY_READ: case APP_MEDIA_QUERY_DELETE:
        request.kind = command->action == APP_MEDIA_QUERY_GET ? APP_AUDIO_CMD_RECORD_GET :
            command->action == APP_MEDIA_QUERY_LIST ? APP_AUDIO_CMD_RECORD_LIST :
            command->action == APP_MEDIA_QUERY_READ ? APP_AUDIO_CMD_RECORD_READ : APP_AUDIO_CMD_RECORD_DELETE;
        request.principal_id = state->command_remote ? (ae_id_t){0} : local_principal;
        if (state->command_remote) memcpy(request.principal_id.bytes,state->remote_command.identity.principal_id,CP_ID_BYTES);
        request.object_id = command->recording_id;
        request.object_revision = command->object_revision; request.cursor = command->cursor;
        request.capacity = command->capacity; request.offset = command->offset;
        break;
    case APP_MEDIA_PREPARE_STORE:
        request.kind = APP_AUDIO_CMD_STORE_PROVISION; request.object_limit = command->object_limit;
        request.quota_bytes = command->quota_bytes;
        break;
    case APP_MEDIA_LINK_CONTROL: return; /* Handled before app_audio command creation. */
    case APP_MEDIA_STOP:
        if (audio->hardware_off_confirmed) finish(state, AE_OK);
        return;
    }
    ae_result_t result = send_audio(state, &request,
        request.kind == APP_AUDIO_CMD_ROUTE ? MEDIA_ROUTE_WAIT : MEDIA_CONTROL_WAIT);
    if (result != AE_OK) finish(state, result);
}

static void cancellation_step(media_state_t *state, const ae_snapshot_t *audio)
{
    portENTER_CRITICAL(&media_lock);
    bool busy=state->pending || state->query.pending || state->view.result_ready;
    bool waiting=state->cancel_waiting;
    portEXIT_CRITICAL(&media_lock);
    if (busy) return;
    if (waiting) {
        app_audio_result_t result;
        if (!app_audio_take_result(&result)) return;
        portENTER_CRITICAL(&media_lock);
        local_grant_t *grant=&state->grants[state->cancel_slot];
        /* A cancelled terminal commit can already be irreversible. Actual
         * audio job state remains the outcome; never manufacture completion. */
        if (result.request_token==state->cancel_serial && result.result!=AE_AGAIN && result.result!=AE_BUSY)
            grant->cancel_requested=false;
        state->cancel_waiting=false;
        portEXIT_CRITICAL(&media_lock);
        return;
    }
    app_audio_command_t request={.kind=APP_AUDIO_CMD_STOP};
    unsigned selected=AE_CONSUMERS_MAX+1U;
    portENTER_CRITICAL(&media_lock);
    if (state->pending || state->query.pending || state->view.result_ready || state->cancel_waiting) {
        portEXIT_CRITICAL(&media_lock); return;
    }
    for (unsigned i=0;i<=AE_CONSUMERS_MAX;++i) {
        local_grant_t *grant=&state->grants[i];
        if (!grant->cancel_requested) continue;
        const ae_job_snapshot_t *job=i==AE_CONSUMERS_MAX ? &audio->playback : &audio->capture[i];
        if (!grant->original_consumer_generation || !ae_id_equal(&grant->job_id,&job->job_id) ||
            !ae_id_equal(&grant->request.request_id,&job->request_id) || !active_job(job)) {
            grant->cancel_requested=false; continue;
        }
        selected=i; request.job_id=grant->job_id;
        request.consumer_generation=grant->original_consumer_generation; request.stop_reason=grant->cancel_reason;
        state->cancel_waiting=true; state->cancel_slot=i; break;
    }
    portEXIT_CRITICAL(&media_lock);
    if (selected>AE_CONSUMERS_MAX) return;
    ae_result_t result=AE_EXHAUSTED;
    if (state->serial!=UINT64_MAX) { request.request_token=++state->serial; result=app_audio_submit(&request); }
    portENTER_CRITICAL(&media_lock);
    if (result==AE_OK) state->cancel_serial=request.request_token;
    else {
        state->cancel_waiting=false;
        if (result!=AE_BUSY && result!=AE_AGAIN) state->grants[selected].cancel_requested=false;
    }
    portEXIT_CRITICAL(&media_lock);
}

static void query_step(media_state_t *state);

static void media_step(media_state_t *state)
{
    /* Bluetooth control servicing may wait on its own SDK queue; this worker
     * is separate from the HID coordinator, renderer and capture worker. */
    bt_audio_service();
    ae_snapshot_t audio;
    bool known = app_audio_snapshot(&audio);
    if (known) {
        if (active_job(&audio.playback) && audio.playback.kind == AE_PLAYBACK_STREAM) {
            /* Catch up after ordinary scheduling jitter without draining into
             * a full manager ring. One bounded pass transfers at most four
             * complete frames; capacity is observed before transport pop. */
            for (unsigned i = 0; i < 4; ++i) {
                if (!app_audio_transport_playback_capacity(audio.route, audio.audio_generation)) break;
                ae_result_t received = audio.route == AE_ROUTE_USB_UAC ? usb_audio_receive(&state->incoming) :
                    audio.route == AE_ROUTE_HFP_HF || audio.route == AE_ROUTE_A2DP_SINK ? bt_audio_receive(&state->incoming) : AE_AGAIN;
                if (received != AE_OK) break;
                ae_result_t admitted = app_audio_submit_transport_playback(audio.route, &state->incoming);
                ae_secure_zero(&state->incoming, sizeof state->incoming);
                /* A changed route/consumer is never relabelled or replayed;
                 * the ingress owner handles an actual overflow as a fault. */
                if (admitted != AE_OK) break;
            }
        }
        portENTER_CRITICAL(&media_lock);
        const bool pending = state->pending;
        portEXIT_CRITICAL(&media_lock);
        if (pending) process_command(state, &audio);
        cancellation_step(state,&audio);
    }
    query_step(state);
    sl_view_t speech;
    bool speech_known = app_spoken_letters_snapshot(&speech);
    portENTER_CRITICAL(&media_lock);
    const bool close_speech = known && speech_known && state->speech_bound &&
        speech.token == state->speech_binding.token && speech.state != SL_LISTENING && !state->speech_stop_sent;
    if (close_speech) {
        state->speech_stop_sent = true;
        /* Revoke only this consumer. The audio owner observes the immutable
         * grant on every frame and closes its exact producer; duplex playback
         * and independent media never inherit a stale speech global stop. */
        state->grants[AE_CAPTURE_RECOGNITION].valid = false;
    }
    portEXIT_CRITICAL(&media_lock);
    portENTER_CRITICAL(&media_lock);
    state->view.audio_known = known; state->view.speech_known = speech_known;
    if (known) state->view.audio = audio;
    if (speech_known) state->view.speech = speech;
    portEXIT_CRITICAL(&media_lock);
}
static void worker(void *context)
{
    media_state_t *state = context;
    for (;;) { media_step(state); vTaskDelay(pdMS_TO_TICKS(10)); }
}

ae_result_t app_media_runtime_init(const app_media_services_t *services)
{
    if (!services) return AE_INVALID;
    portENTER_CRITICAL(&media_lock);
    if (media_attempted) { portEXIT_CRITICAL(&media_lock); return AE_BUSY; }
    media_attempted = true;
    portEXIT_CRITICAL(&media_lock);
    media_state_t *state = heap_caps_aligned_calloc(_Alignof(media_state_t), 1, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state) return AE_EXHAUSTED;
    state->services = *services;
    ae_result_t result = random_id(NULL, &state->boot_session);
    if (result == AE_OK) result = board_audio_mosaico_init();
    if (result != AE_OK) { heap_caps_free(state); return result; }
    (void)bt_audio_init(); /* Independent failure remains in its actual route capability. */
    (void)app_spoken_letters_init();
    ae_dependencies_t dependencies = {.codec = board_audio_mosaico_port(), .store = services->store,
        .route = {.capabilities = route_caps, .prepare = route_prepare, .capture = route_capture,
            .stop = route_stop, .playback_consumed = route_consumed, .bind_playback=route_bind_playback},
        .policy_context = state, .authority = authority, .random_id = random_id,
        .trusted_unix_time = wall_time, .monotonic_ms = clock_ms,
        .capture_sink = capture_sink, .capture_cancel = capture_closed, .fanout_enabled = false};
    result = app_audio_init(&dependencies);
    if (result != AE_OK) { heap_caps_free(state); return result; }
    portENTER_CRITICAL(&media_lock); media = state; portEXIT_CRITICAL(&media_lock);
    /* This resident worker uses cached policy and asynchronous owner APIs.
     * NVS/storage and model execution stay on their separate internal tasks.
     * WithCaps keeps its TCB internal and unwinds partial creation; it must
     * never retry with an internal stack after external allocation fails. */
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    BaseType_t created = xTaskCreateWithCaps(worker, "media_control", 6144, state, 3, NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t created = xTaskCreate(worker, "media_control", 6144, state, 3, NULL);
#endif
    if (created != pdPASS) {
        /* The audio owner can still reference policy_context. Retain fixed
         * memory, close gates and expose failure instead of freeing it early. */
        app_audio_request_safety_stop(AE_STOP_IO);
        portENTER_CRITICAL(&media_lock); state->view.result = AE_EXHAUSTED; portEXIT_CRITICAL(&media_lock);
        return AE_EXHAUSTED;
    }
    portENTER_CRITICAL(&media_lock); state->view.initialized = true; portEXIT_CRITICAL(&media_lock);
    const app_control_media_port_t network={.user=state,.capture_allowed=network_capture_allowed,
        .capture_failed=network_capture_failed,.playback=network_playback_receive,.channel_lost=network_channel_lost};
    app_control_server_set_media_port(&network);
    return AE_OK;
}
void app_media_runtime_environment(const ae_environment_t *environment, uint64_t epoch,
    uint64_t revision, bool speech_context)
{
    if (!environment) return;
    sl_binding_t cancel = {0};
    portENTER_CRITICAL(&media_lock);
    if (media) {
        media->environment = *environment; media->epoch = epoch; media->ui_revision = revision;
        media->speech_context = speech_context;
        if (media->speech_bound && (!speech_context || !environment->unlocked || environment->privacy_latched ||
            environment->maintenance || epoch != media->speech_binding.epoch || revision != media->speech_binding.ui_revision)) {
            cancel = media->speech_binding; media->speech_bound = false;
            media->grants[AE_CAPTURE_RECOGNITION].valid = false;
        }
        if (media->pending && media->command.action == APP_MEDIA_START &&
            media->command.kind == AE_CAPTURE_RECOGNITION &&
            (!speech_context || media->command.epoch != epoch || media->command.ui_revision != revision)) {
            if (media->cancellation != UINT64_MAX) ++media->cancellation;
            media->grants[AE_CAPTURE_RECOGNITION].valid = false;
        }
        if (!environment->unlocked || environment->privacy_latched || environment->maintenance) {
            media->query.cancelled=true; media->query.ready=false;
            if (!media->query.pending) ae_secure_zero(media->query.data,sizeof(media->query.data));
            if (media->cancellation != UINT64_MAX) ++media->cancellation;
            for (unsigned i = 0; i < AE_CONSUMERS_MAX + 1; ++i) media->grants[i].valid = false;
        }
    }
    portEXIT_CRITICAL(&media_lock);
    app_audio_set_environment(environment);
    if (cancel.token) app_spoken_letters_cancel(&cancel.job_id, cancel.consumer_generation);
}
ae_result_t app_media_runtime_submit_local(const app_media_local_request_t *request)
{
    if (!request || !request->token || request->action > APP_MEDIA_LINK_CONTROL || request->action < APP_MEDIA_START ||
        request->kind < AE_CAPTURE_LIVE || request->kind > AE_PLAYBACK_RECORDING) return AE_INVALID;
    portENTER_CRITICAL(&media_lock);
    ae_result_t result = AE_OK;
    if (!media || !media->view.initialized) result = AE_UNAVAILABLE;
    else if (media->cancellation == UINT64_MAX) result = AE_EXHAUSTED;
    else if (media->pending || media->view.result_ready || media->query.pending || media->cancel_waiting) result = AE_BUSY;
    else if (!media->environment.unlocked || media->environment.privacy_latched || media->environment.maintenance) result = AE_PRIVACY;
    else if (request->epoch != media->epoch || request->ui_revision != media->ui_revision || request->token <= media->last_token) result = AE_STALE;
    else if (request->kind == AE_CAPTURE_RECOGNITION && request->action == APP_MEDIA_START && !media->speech_context) result = AE_DENIED;
    else {
        media->query.ready=false; media->query.cancelled=true;
        ae_secure_zero(media->query.data,sizeof(media->query.data));
        media->command_remote=false; media->remote_cancelled=false; media->command_channel_generation=0;
        ae_secure_zero(&media->remote_command,sizeof(media->remote_command));
        media->command = *request; media->pending = true; media->phase = MEDIA_ADMIT;
        media->execution_checked = false;
        media->command_cancellation = media->cancellation;
        media->view.busy = true; media->last_token = request->token;
        media->view.result_detail = (app_audio_result_t){0}; media->result_audio_serial = 0;
    }
    portEXIT_CRITICAL(&media_lock);
    if (result == AE_OK && request->action == APP_MEDIA_STOP) app_media_runtime_stop(AE_STOP_REQUESTED);
    return result;
}
void app_media_runtime_stop(ae_stop_reason_t reason)
{
    sl_binding_t cancel = {0};
    portENTER_CRITICAL(&media_lock);
    if (media) {
        media->query.cancelled=true; media->query.ready=false;
        if (!media->query.pending) ae_secure_zero(media->query.data,sizeof(media->query.data));
        if (media->cancellation != UINT64_MAX) ++media->cancellation;
        for (unsigned i = 0; i < AE_CONSUMERS_MAX + 1; ++i) media->grants[i].valid = false;
        if (media->speech_bound) { cancel = media->speech_binding; media->speech_bound = false; }
    }
    portEXIT_CRITICAL(&media_lock);
    if (cancel.token) app_spoken_letters_cancel(&cancel.job_id, cancel.consumer_generation);
    app_audio_request_safety_stop(reason);
}
bool app_media_runtime_snapshot(app_media_snapshot_t *output)
{
    if (!output) return false;
    portENTER_CRITICAL(&media_lock);
    bool available = media != NULL;
    if (available) {
        *output=media->view;
        if (media->command_remote) {
            output->result_ready=false; output->request_token=0;
            output->result_detail=(app_audio_result_t){0};
        }
    }
    portEXIT_CRITICAL(&media_lock);
    return available;
}
bool app_media_runtime_take_result(uint64_t *token, ae_result_t *result)
{
    if (!token || !result) return false;
    portENTER_CRITICAL(&media_lock);
    bool ready = media && media->view.result_ready && !media->command_remote;
    if (ready) { *token = media->view.request_token; *result = media->view.result; media->view.result_ready = false; }
    portEXIT_CRITICAL(&media_lock);
    return ready;
}
ae_result_t app_media_runtime_confirm(uint64_t token, uint64_t epoch, uint64_t revision, char *candidate)
{
    portENTER_CRITICAL(&media_lock);
    bool valid = media && media->speech_context && media->environment.unlocked &&
        !media->environment.privacy_latched && !media->environment.maintenance && media->epoch == epoch &&
        media->ui_revision == revision && media->speech_bound && media->speech_binding.token == token;
    portEXIT_CRITICAL(&media_lock);
    ae_result_t result = valid ? app_spoken_letters_confirm(token, epoch, revision, candidate) : AE_DENIED;
    if (result == AE_OK) {
        portENTER_CRITICAL(&media_lock);
        if (media && media->speech_bound && media->speech_binding.token == token) {
            media->speech_bound = false;
            media->grants[AE_CAPTURE_RECOGNITION].valid = false;
        }
        portEXIT_CRITICAL(&media_lock);
    }
    return result;
}
ae_result_t app_media_runtime_reject(uint64_t token, uint64_t epoch, uint64_t revision)
{
    sl_binding_t cancel = {0};
    portENTER_CRITICAL(&media_lock);
    bool valid = media && media->speech_context && media->environment.unlocked &&
        !media->environment.privacy_latched && !media->environment.maintenance && media->epoch == epoch &&
        media->ui_revision == revision && media->speech_bound && media->speech_binding.token == token &&
        media->view.speech_known && media->view.speech.state == SL_CANDIDATE;
    if (valid) { cancel = media->speech_binding; media->speech_bound = false; }
    portEXIT_CRITICAL(&media_lock);
    if (cancel.token) app_spoken_letters_cancel(&cancel.job_id, cancel.consumer_generation);
    return valid ? AE_OK : AE_DENIED;
}
ae_result_t app_media_runtime_route(ae_route_t route, ae_route_capabilities_t *output)
{ return route_caps(NULL, route, output); }

ae_result_t app_media_runtime_copy_records(uint64_t token, ae_record_metadata_t *items, size_t capacity)
{
    portENTER_CRITICAL(&media_lock);
    const uint64_t serial = media && !media->command_remote && media->view.result_ready && media->view.request_token == token ? media->result_audio_serial : 0;
    portEXIT_CRITICAL(&media_lock);
    return serial ? app_audio_copy_record_list(serial, items, capacity) : AE_STALE;
}
ae_result_t app_media_runtime_copy_record_data(uint64_t token, size_t offset, void *buffer, size_t bytes)
{
    portENTER_CRITICAL(&media_lock);
    const uint64_t serial = media && !media->command_remote && media->view.result_ready && media->view.request_token == token ? media->result_audio_serial : 0;
    portEXIT_CRITICAL(&media_lock);
    return serial ? app_audio_copy_record_data(serial, offset, buffer, bytes) : AE_STALE;
}

static bool remote_identity_equal(const cp_identity_t *a, const cp_identity_t *b)
{
    return !memcmp(a->principal_id,b->principal_id,CP_ID_BYTES) && !memcmp(a->session_id,b->session_id,CP_ID_BYTES);
}
ae_result_t app_media_runtime_set_remote_authority(const app_media_remote_authority_t *port)
{
    if (!port || !port->session_alive || !port->capture_authority) return AE_INVALID;
    portENTER_CRITICAL(&media_lock);
    ae_result_t result=!media ? AE_UNAVAILABLE : media->remote_authority.session_alive ? AE_BUSY : AE_OK;
    if (result==AE_OK) media->remote_authority=*port;
    portEXIT_CRITICAL(&media_lock); return result;
}
static cp_scope_mask_t remote_scope(const app_remote_media_request_t *r)
{
    switch (r->operation) {
    case CP_OP_AUDIO_CAPTURE_START: case CP_OP_AUDIO_CAPTURE_STOP: case CP_OP_AUDIO_MICROPHONE_ENABLE:
    case CP_OP_AUDIO_MICROPHONE_MUTE: case CP_OP_SPEECH_LISTEN_START: case CP_OP_SPEECH_LISTEN_STOP: return CP_SCOPE_AUDIO_CAPTURE;
    case CP_OP_AUDIO_PLAYBACK_START: case CP_OP_AUDIO_PLAYBACK_STOP: case CP_OP_AUDIO_SPEAKER_ENABLE:
    case CP_OP_AUDIO_SPEAKER_MUTE: return CP_SCOPE_AUDIO_PLAYBACK;
    case CP_OP_RECORDING_START: case CP_OP_RECORDING_STOP: case CP_OP_RECORDING_CANCEL: return CP_SCOPE_RECORDING_CONTROL;
    case CP_OP_RECORDING_DELETE: return CP_SCOPE_RECORDING_DELETE;
    case CP_OP_RECORDINGS_GET: case CP_OP_RECORDINGS_LIST: case CP_OP_RECORDING_READ: return CP_SCOPE_RECORDING_READ;
    case CP_OP_AUDIO_ROUTE_SELECT: case CP_OP_AUDIO_ENABLE_SET: case CP_OP_AUDIO_MUTE_SET:
    case CP_OP_AUDIO_LINK_CONNECT: case CP_OP_AUDIO_LINK_DISCONNECT: return CP_SCOPE_AUDIO_CONTROL;
    default: return 0;
    }
}
static bool remote_job_owned_locked(const app_remote_media_request_t *request)
{
    for (unsigned i=0;i<AE_CONSUMERS_MAX+1;++i) {
        const ae_job_snapshot_t *job=i==AE_CONSUMERS_MAX ? &media->view.audio.playback : &media->view.audio.capture[i];
        const local_grant_t *grant=&media->grants[i];
        if (ae_id_equal(&job->job_id,&request->job_id) && grant->remote &&
            ae_id_equal(&grant->request.request_id,&job->request_id) &&
            !memcmp(request->identity.principal_id,grant->identity.principal_id,CP_ID_BYTES)) {
            return job->kind==AE_CAPTURE_LIVE ? request->operation==CP_OP_AUDIO_CAPTURE_STOP :
                job->kind==AE_CAPTURE_RECOGNITION ? request->operation==CP_OP_SPEECH_LISTEN_STOP :
                job->kind==AE_CAPTURE_RECORDING ? request->operation==CP_OP_RECORDING_STOP || request->operation==CP_OP_RECORDING_CANCEL :
                request->operation==CP_OP_AUDIO_PLAYBACK_STOP;
        }
    }
    return false;
}
static ae_result_t remote_link_bond(app_media_local_request_t *command, const char *peer_id)
{
    portENTER_CRITICAL(&media_lock);
    media_state_t *state=media;
    ae_result_t result=!state || !state->view.initialized ? AE_UNAVAILABLE :
        state->remote_bonds_busy ? AE_BUSY : AE_OK;
    if (result==AE_OK) state->remote_bonds_busy=true;
    portEXIT_CRITICAL(&media_lock);
    if (result!=AE_OK) return result;

    /* The runtime retains its state for the audio owner's lifetime. Claiming
     * this buffer does not hold media_lock while acquiring the Bluetooth lock. */
    bt_audio_bond_list_t *bonds=&state->remote_bonds;
    bt_audio_copy_bond_list(bonds);
    if (!bonds->known || bonds->refreshing || !bonds->revision) result=AE_UNAVAILABLE;
    else {
        unsigned bytes[6]; char tail;
        if (sscanf(peer_id,"%2x:%2x:%2x:%2x:%2x:%2x%c",&bytes[0],&bytes[1],&bytes[2],&bytes[3],&bytes[4],&bytes[5],&tail)!=6)
            result=AE_INVALID;
        else {
            for (unsigned i=0;i<6;++i) command->bluetooth_peer[i]=(uint8_t)bytes[i];
            bool found=false;
            for (unsigned i=0;i<bonds->count && i<BT_AUDIO_CLASSIC_BONDS_MAX;++i)
                found|=!memcmp(bonds->bonds[i].address,command->bluetooth_peer,6);
            if (!found) result=AE_STALE;
            else command->bluetooth_bond_revision=bonds->revision;
        }
    }
    ae_secure_zero(bonds,sizeof(*bonds));
    portENTER_CRITICAL(&media_lock);
    state->remote_bonds_busy=false;
    portEXIT_CRITICAL(&media_lock);
    return result;
}
ae_result_t app_media_runtime_submit_remote(const app_remote_media_request_t *request)
{
    if (!request || !request->token || !request->command_key) return AE_INVALID;
    cp_scope_mask_t scope=remote_scope(request);
    ae_id_t principal={0},session={0};
    memcpy(principal.bytes,request->identity.principal_id,CP_ID_BYTES);
    memcpy(session.bytes,request->identity.session_id,CP_ID_BYTES);
    if (!scope || (request->identity.scopes&scope)!=scope || ae_id_empty(&principal) || ae_id_empty(&session) ||
        ae_id_equal(&principal,&local_principal)) return AE_DENIED;
    portENTER_CRITICAL(&media_lock);
    app_media_remote_authority_t policy=media ? media->remote_authority : (app_media_remote_authority_t){0};
    portEXIT_CRITICAL(&media_lock);
    if (!remote_alive(&policy,&request->identity)) return AE_DENIED;
    uint64_t channel_generation=0;
    app_media_local_request_t command={.token=request->token,.epoch=request->input_epoch,.ui_revision=request->ui_revision,
        .expected_audio_generation=request->audio_generation,.expected_audio_revision=request->resource_revision,
        .route=request->route,.role=request->role,.format=request->format,.value=request->value,
        .job_id=request->job_id,.consumer_generation=request->consumer_generation,.stop_reason=request->stop_reason,
        .recording_id=request->object_id,.object_revision=request->object_revision,.offset=request->offset,
        .cursor=request->cursor,.capacity=request->capacity};
    switch (request->action) {
    case APP_REMOTE_MEDIA_START: {
        const ae_start_request_t *start=&request->start;
        if (!ae_id_equal(&start->principal_id,&principal) || !ae_id_equal(&start->owner_session_id,&session) ||
            start->expected_audio_generation!=request->audio_generation || start->expected_revision!=request->resource_revision ||
            start->kind<AE_CAPTURE_LIVE || start->kind>AE_PLAYBACK_RECORDING) return AE_INVALID;
        bool matches=request->operation==CP_OP_AUDIO_CAPTURE_START ? start->kind==AE_CAPTURE_LIVE :
            request->operation==CP_OP_RECORDING_START ? start->kind==AE_CAPTURE_RECORDING :
            request->operation==CP_OP_SPEECH_LISTEN_START ? start->kind==AE_CAPTURE_RECOGNITION :
            request->operation==CP_OP_AUDIO_PLAYBACK_START && start->kind>=AE_PLAYBACK_STREAM;
        if (!matches) return AE_INVALID;
        if (network_request(start)) {
            channel_generation=app_control_server_channel_generation(&request->identity);
            if (!channel_generation) return AE_UNAVAILABLE;
        }
        local_grant_t grant={.request=*start,.identity=request->identity,.valid=true,.remote=true,.channel_generation=channel_generation};
        ae_authority_t allowed={0};
        ae_result_t valid=remote_grant_authority(&policy,&grant,clock_ms(NULL),&allowed);
        if (valid!=AE_OK || !allowed.allowed || !allowed.owner_session_alive) return AE_DENIED;
        command.action=APP_MEDIA_START; command.kind=start->kind; command.route=start->route;
        command.format=start->format; command.duration_seconds=start->duration_seconds; command.byte_limit=start->byte_limit;
        command.retention_seconds=start->retention_seconds; command.storage=start->storage;
        command.export_policy=start->export_policy; command.recording_id=start->recording_id; break;
    }
    case APP_REMOTE_MEDIA_STOP_JOB:
        command.expected_audio_revision=0;
        if (request->stop_reason==AE_STOP_CANCEL_DISCARD && !(request->identity.scopes&CP_SCOPE_RECORDING_DELETE)) return AE_DENIED;
        command.action=APP_MEDIA_STOP_JOB; break;
    case APP_REMOTE_MEDIA_ROUTE: if (request->operation!=CP_OP_AUDIO_ROUTE_SELECT) return AE_INVALID; command.action=APP_MEDIA_ROUTE; break;
    case APP_REMOTE_MEDIA_ENABLE:
        if (request->operation!=CP_OP_AUDIO_ENABLE_SET &&
            !(request->operation==CP_OP_AUDIO_MICROPHONE_ENABLE && request->role==AE_MICROPHONE) &&
            !(request->operation==CP_OP_AUDIO_SPEAKER_ENABLE && request->role==AE_SPEAKER)) return AE_INVALID;
        command.action=APP_MEDIA_ENABLE_ROLE; break;
    case APP_REMOTE_MEDIA_MUTE:
        if (request->operation!=CP_OP_AUDIO_MUTE_SET &&
            !(request->operation==CP_OP_AUDIO_MICROPHONE_MUTE && request->role==AE_MICROPHONE) &&
            !(request->operation==CP_OP_AUDIO_SPEAKER_MUTE && request->role==AE_SPEAKER)) return AE_INVALID;
        command.action=APP_MEDIA_MUTE_ROLE; break;
    case APP_REMOTE_MEDIA_RECORD_GET: if (request->operation!=CP_OP_RECORDINGS_GET) return AE_INVALID; command.action=APP_MEDIA_QUERY_GET; command.expected_audio_revision=0; break;
    case APP_REMOTE_MEDIA_RECORD_LIST: if (request->operation!=CP_OP_RECORDINGS_LIST) return AE_INVALID; command.action=APP_MEDIA_QUERY_LIST; command.expected_audio_revision=0; break;
    case APP_REMOTE_MEDIA_RECORD_READ: if (request->operation!=CP_OP_RECORDING_READ) return AE_INVALID; command.action=APP_MEDIA_QUERY_READ; command.expected_audio_revision=0; break;
    case APP_REMOTE_MEDIA_RECORD_DELETE: if (request->operation!=CP_OP_RECORDING_DELETE) return AE_INVALID; command.action=APP_MEDIA_QUERY_DELETE; command.expected_audio_revision=0; break;
    case APP_REMOTE_MEDIA_LINK: {
        if (request->operation!=(request->value?CP_OP_AUDIO_LINK_CONNECT:CP_OP_AUDIO_LINK_DISCONNECT)) return AE_INVALID;
        command.action=APP_MEDIA_LINK_CONTROL;
        bt_audio_snapshot_t bt=bt_audio_snapshot(); command.bluetooth_generation=bt.generation;
        if (request->value) {
            ae_result_t linked=remote_link_bond(&command,request->peer_id);
            if (linked!=AE_OK) return linked;
        }
        break;
    }
    default: return AE_NOT_SUPPORTED;
    }
    bool safety=command.action==APP_MEDIA_STOP_JOB || (command.action==APP_MEDIA_MUTE_ROLE && command.value) ||
        (command.action==APP_MEDIA_ENABLE_ROLE && !command.value) || (command.action==APP_MEDIA_LINK_CONTROL && !command.value);
    portENTER_CRITICAL(&media_lock);
    ae_result_t result=AE_OK;
    if (!media || !media->view.initialized) result=AE_UNAVAILABLE;
    else if (media->pending || media->view.result_ready || media->query.pending || media->cancel_waiting) result=AE_BUSY;
    else if (clock_ms(NULL)>=request->deadline_ms) result=AE_TIMEOUT;
    else if (!safety && (!media->environment.unlocked || media->environment.privacy_latched || media->environment.maintenance)) result=AE_PRIVACY;
    else if (command.action==APP_MEDIA_STOP_JOB && !remote_job_owned_locked(request)) result=AE_DENIED;
    else if (command.action==APP_MEDIA_START && command.route!=media->view.audio.route) result=AE_STALE;
    else if (command.action==APP_MEDIA_START && command.kind==AE_CAPTURE_RECOGNITION && (!media->speech_context ||
        command.epoch!=media->epoch || command.ui_revision!=media->ui_revision)) result=AE_STALE;
    else {
        media->query.ready=false; media->query.cancelled=true;
        ae_secure_zero(media->query.data,sizeof(media->query.data));
        media->command=command; media->remote_command=*request; media->command_remote=true; media->remote_cancelled=false;
        media->command_channel_generation=channel_generation;
        media->pending=true; media->phase=MEDIA_ADMIT; media->execution_checked=false;
        media->command_cancellation=media->cancellation; media->view.busy=true;
        media->view.result_detail=(app_audio_result_t){0}; media->result_audio_serial=0;
    }
    portEXIT_CRITICAL(&media_lock); return result;
}
bool app_media_runtime_poll_remote(uint64_t token, app_audio_result_t *output)
{
    if (!output || !token) return false;
    cp_identity_t identity={0}; app_media_remote_authority_t policy={0};
    portENTER_CRITICAL(&media_lock);
    bool ready=media && media->command_remote && media->view.result_ready && media->view.request_token==token;
    if (ready) {
        *output=media->view.result_detail; output->request_token=token; output->result=media->view.result;
        identity=media->remote_command.identity; policy=media->remote_authority;
        if (media->remote_cancelled || !media->environment.unlocked || media->environment.privacy_latched || media->environment.maintenance)
            *output=(app_audio_result_t){.request_token=token,.result=AE_DENIED};
    }
    portEXIT_CRITICAL(&media_lock);
    if (ready && !remote_alive(&policy,&identity)) { *output=(app_audio_result_t){.request_token=token,.result=AE_DENIED}; }
    return ready;
}
void app_media_runtime_release_remote_result(uint64_t token)
{
    portENTER_CRITICAL(&media_lock);
    if (media && media->command_remote && media->view.result_ready && media->view.request_token==token) {
        media->view.result_ready=false; media->command_remote=false; media->command_channel_generation=0;
        ae_secure_zero(&media->remote_command,sizeof(media->remote_command));
        ae_secure_zero(&media->view.result_detail,sizeof(media->view.result_detail));
    }
    portEXIT_CRITICAL(&media_lock);
}
void app_media_runtime_cancel_remote(uint64_t token, const cp_identity_t *identity, ae_stop_reason_t reason)
{
    if (!token || !identity) return;
    sl_binding_t speech={0};
    portENTER_CRITICAL(&media_lock);
    if (media) {
        if (media->command_remote && media->remote_command.token==token && remote_identity_equal(identity,&media->remote_command.identity))
            media->remote_cancelled=true;
        for (unsigned i=0;i<AE_CONSUMERS_MAX+1;++i) {
            local_grant_t *grant=&media->grants[i];
            if (grant->remote && grant->token==token && remote_identity_equal(identity,&grant->identity)) {
                grant->valid=false;
                if (reason==AE_STOP_CANCEL_KEEP || reason==AE_STOP_CANCEL_DISCARD) {
                    grant->cancel_requested=true; grant->cancel_reason=reason;
                }
                if (i==AE_CAPTURE_RECOGNITION && media->speech_bound && media->speech_binding.token==token) {
                    speech=media->speech_binding; media->speech_bound=false;
                }
            }
        }
    }
    portEXIT_CRITICAL(&media_lock);
    if (speech.token) app_spoken_letters_cancel(&speech.job_id,speech.consumer_generation);
    /* The worker serializes explicit cancellation with the sole audio result owner. */
}

static void query_finish(media_state_t *state, ae_result_t result)
{
    uint64_t seconds=0;
    bool known=state->services.wall_time && state->services.wall_time(state->services.clock_context,&seconds)==AE_OK;
    if (result==AE_OK && (!known || seconds<state->query.observed_unix_seconds)) result=AE_TIME_UNKNOWN;
    portENTER_CRITICAL(&media_lock);
    media_query_t *q=&state->query;
    if (known && seconds>q->observed_unix_seconds) q->observed_unix_seconds=seconds;
    q->result=result; q->pending=q->waiting=false; q->ready=true; q->completed_ms=clock_ms(NULL);
    if (result!=AE_OK) {
        ae_secure_zero(q->data,sizeof(q->data)); ae_secure_zero(q->records,sizeof(q->records));
        ae_secure_zero(&q->metadata,sizeof(q->metadata)); q->bytes=q->count=0;
    }
    portEXIT_CRITICAL(&media_lock);
}
static void query_step(media_state_t *state)
{
    portENTER_CRITICAL(&media_lock);
    bool pending=state->query.pending;
    bool cancelled=state->query.cancelled || !state->environment.unlocked || state->environment.privacy_latched || state->environment.maintenance;
    cp_identity_t identity=state->query.identity;
    portEXIT_CRITICAL(&media_lock);
    if (!pending) return;
    media_query_t *q=&state->query;
    cancelled|=!remote_alive(&state->remote_authority,&identity) || clock_ms(NULL)>=q->deadline_ms;
    if (cancelled) {
        portENTER_CRITICAL(&media_lock); q->cancelled=true; portEXIT_CRITICAL(&media_lock);
        if (!q->waiting) { query_finish(state,AE_DENIED); return; }
    }
    if (q->waiting) {
        app_audio_result_t result;
        if (!app_audio_take_result(&result)) return;
        q->waiting=false;
        if (cancelled) { query_finish(state,AE_DENIED); return; }
        if (result.request_token!=q->serial) { query_finish(state,AE_STALE); return; }
        if (result.result!=AE_OK) { query_finish(state,result.result); return; }
        if (q->kind==QUERY_LIST) {
            if (result.count>q->capacity || result.count>AE_LIST_MAX) { query_finish(state,AE_INTEGRITY); return; }
            ae_result_t copied=app_audio_copy_record_list(q->serial,q->records,q->capacity);
            if (copied!=AE_OK) { query_finish(state,copied); return; }
            for (size_t i=0;i<result.count;++i) if (memcmp(q->records[i].principal_id.bytes,identity.principal_id,CP_ID_BYTES)) {
                query_finish(state,AE_INTEGRITY); return;
            }
            q->count=result.count; q->next=result.next_cursor; query_finish(state,AE_OK); return;
        }
        if (q->kind==QUERY_METADATA || q->stage==0 || q->stage==2) {
            if (!ae_id_equal(&result.metadata.object_id,&q->object) ||
                memcmp(result.metadata.principal_id.bytes,identity.principal_id,CP_ID_BYTES)) {
                query_finish(state,AE_INTEGRITY); return;
            }
            if (q->kind==QUERY_METADATA) { q->metadata=result.metadata; query_finish(state,AE_OK); return; }
            if (result.metadata.revision!=q->revision || result.metadata.access_revoked) { query_finish(state,AE_STALE); return; }
            if (q->stage==2) { query_finish(state,AE_OK); return; }
            if (result.metadata.state!=AE_RECORD_COMPLETED || !result.metadata.integrity_verified || !result.metadata.hash_available ||
                !result.metadata.sync_acknowledged || result.metadata.export_policy!=AE_EXPORT_OWNER_DOWNLOAD) {
                query_finish(state,AE_DENIED); return;
            }
            q->metadata=result.metadata; q->stage=1;
        } else {
            if (result.bytes!=q->capacity) { query_finish(state,AE_INTEGRITY); return; }
            ae_result_t copied=app_audio_copy_record_data(q->serial,0,q->data,result.bytes);
            if (copied!=AE_OK) { query_finish(state,copied); return; }
            q->bytes=result.bytes; q->stage=2;
        }
    }
    if (state->serial==UINT64_MAX) { query_finish(state,AE_EXHAUSTED); return; }
    app_audio_command_t request={.request_token=++state->serial,.principal_id={{0}},.object_id=q->object,
        .offset=q->offset,.capacity=(uint16_t)q->capacity,.cursor=q->cursor};
    memcpy(request.principal_id.bytes,identity.principal_id,CP_ID_BYTES);
    request.kind=q->kind==QUERY_LIST ? APP_AUDIO_CMD_RECORD_LIST : q->kind==QUERY_CONTENT && q->stage==1 ?
        APP_AUDIO_CMD_RECORD_READ : APP_AUDIO_CMD_RECORD_GET;
    ae_result_t submitted=app_audio_submit(&request);
    if (submitted!=AE_OK) { query_finish(state,submitted); return; }
    q->serial=request.request_token; q->waiting=true;
}
static ae_result_t query_admit(const cp_identity_t *identity, const ae_id_t *object,
    media_query_kind_t kind, uint64_t revision, uint64_t offset, size_t capacity, uint32_t cursor)
{
    if (!identity || !object || !(identity->scopes&CP_SCOPE_RECORDING_READ) ||
        (kind!=QUERY_LIST && ae_id_empty(object)) || !memcmp(identity->principal_id,local_principal.bytes,CP_ID_BYTES)) return AE_DENIED;
    portENTER_CRITICAL(&media_lock);
    app_media_remote_authority_t policy=media ? media->remote_authority : (app_media_remote_authority_t){0};
    app_media_services_t services=media ? media->services : (app_media_services_t){0};
    portEXIT_CRITICAL(&media_lock);
    if (!remote_alive(&policy,identity)) return AE_DENIED;
    uint64_t seconds=0;
    bool time_known=services.wall_time && services.wall_time(services.clock_context,&seconds)==AE_OK;
    uint64_t now=clock_ms(NULL);
    portENTER_CRITICAL(&media_lock);
    ae_result_t result=AE_AGAIN;
    if (!media || !media->view.initialized) result=AE_UNAVAILABLE;
    else if (!media->environment.unlocked || media->environment.privacy_latched || media->environment.maintenance) result=AE_PRIVACY;
    else {
        media_query_t *q=&media->query;
        bool same=remote_identity_equal(identity,&q->identity) && identity->scopes==q->identity.scopes &&
            ae_id_equal(object,&q->object) && kind==q->kind && revision==q->revision && offset==q->offset &&
            capacity==q->capacity && cursor==q->cursor;
        if (q->pending) result=same ? AE_AGAIN : AE_BUSY;
        else if (same && q->ready && !q->cancelled && now>=q->completed_ms && now-q->completed_ms<=250) {
            result=q->result;
            if (result==AE_OK) {
                if (!time_known || seconds<q->observed_unix_seconds) result=AE_TIME_UNKNOWN;
                else if (kind!=QUERY_LIST && seconds>=q->metadata.expires_unix_seconds) result=AE_DENIED;
                else if (kind==QUERY_LIST) for (size_t i=0;i<q->count;++i)
                    if (seconds>=q->records[i].expires_unix_seconds) result=AE_DENIED;
            }
        }
        else if (media->pending || media->view.result_ready || media->cancel_waiting) result=AE_BUSY;
        else if (now>UINT64_MAX-30000U) result=AE_EXHAUSTED;
        else {
            ae_secure_zero(q,sizeof(*q));
            q->identity=*identity; q->object=*object; q->kind=kind; q->revision=revision; q->offset=offset;
            q->capacity=capacity; q->cursor=cursor; q->deadline_ms=now+30000U; q->pending=true; q->observed_unix_seconds=time_known?seconds:0;
        }
    }
    portEXIT_CRITICAL(&media_lock); return result;
}
static bool query_publication_allowed(const cp_identity_t *identity, uint64_t expires, uint64_t observed)
{
    portENTER_CRITICAL(&media_lock);
    bool ready=media && media->environment.unlocked && !media->environment.privacy_latched && !media->environment.maintenance;
    app_media_remote_authority_t policy=media ? media->remote_authority : (app_media_remote_authority_t){0};
    app_media_services_t services=media ? media->services : (app_media_services_t){0};
    portEXIT_CRITICAL(&media_lock);
    uint64_t seconds=0;
    return ready && remote_alive(&policy,identity) && services.wall_time &&
        services.wall_time(services.clock_context,&seconds)==AE_OK && seconds>=observed && seconds<expires;
}
ae_result_t app_media_runtime_record_metadata(const cp_identity_t *identity, const ae_id_t *object, ae_record_metadata_t *output)
{
    if (!output) return AE_INVALID;
    uint64_t observed=0;
    ae_result_t result=query_admit(identity,object,QUERY_METADATA,0,0,0,0);
    if (result!=AE_OK) { ae_secure_zero(output,sizeof(*output)); return result; }
    portENTER_CRITICAL(&media_lock);
    if (media && media->query.ready && !media->query.cancelled && remote_identity_equal(identity,&media->query.identity) &&
        ae_id_equal(object,&media->query.object) && media->environment.unlocked &&
        !media->environment.privacy_latched && !media->environment.maintenance) { *output=media->query.metadata; observed=media->query.observed_unix_seconds; }
    else result=AE_STALE;
    portEXIT_CRITICAL(&media_lock);
    if (result==AE_OK && !query_publication_allowed(identity,output->expires_unix_seconds,observed)) result=AE_DENIED;
    if (result!=AE_OK) ae_secure_zero(output,sizeof(*output));
    return result;
}
ae_result_t app_media_runtime_content_read(const cp_identity_t *identity, const ae_id_t *object, uint64_t revision,
    uint64_t offset, void *output, size_t capacity, size_t *actual)
{
    if (!output || !actual || !revision || !capacity || capacity>APP_REMOTE_MEDIA_READ_BYTES) return AE_INVALID;
    *actual=0; ae_secure_zero(output,capacity); uint64_t expires=0,observed=0;
    ae_result_t result=query_admit(identity,object,QUERY_CONTENT,revision,offset,capacity,0);
    if (result!=AE_OK) return result;
    portENTER_CRITICAL(&media_lock);
    if (media && media->query.ready && !media->query.cancelled && media->query.kind==QUERY_CONTENT &&
        media->query.revision==revision && media->query.offset==offset && media->query.bytes==capacity &&
        remote_identity_equal(identity,&media->query.identity) && ae_id_equal(object,&media->query.object) &&
        media->environment.unlocked && !media->environment.privacy_latched && !media->environment.maintenance) {
        memcpy(output,media->query.data,capacity); *actual=capacity; expires=media->query.metadata.expires_unix_seconds; observed=media->query.observed_unix_seconds;
        media->query.ready=false; media->query.cancelled=true;
        ae_secure_zero(media->query.data,sizeof(media->query.data));
    } else result=AE_STALE;
    portEXIT_CRITICAL(&media_lock);
    if (result==AE_OK && !query_publication_allowed(identity,expires,observed)) result=AE_DENIED;
    if (result!=AE_OK) { ae_secure_zero(output,capacity); *actual=0; }
    return result;
}
void app_media_runtime_content_cancel(const cp_identity_t *identity, const ae_id_t *object)
{
    if (!identity || !object) return;
    portENTER_CRITICAL(&media_lock);
    if (media && remote_identity_equal(identity,&media->query.identity) && ae_id_equal(object,&media->query.object)) {
        media->query.cancelled=true; media->query.ready=false;
        /* A pending worker owns its scratch until the in-flight result retires.
         * Closing publication is immediate; only that worker clears its bytes. */
        if (!media->query.pending) ae_secure_zero(media->query.data,sizeof(media->query.data));
    }
    portEXIT_CRITICAL(&media_lock);
}

static bool query_json(char *json, size_t capacity, size_t *length, const char *format, ...)
{
    if (*length>=capacity) return false;
    va_list args; va_start(args,format); int n=vsnprintf(json+*length,capacity-*length,format,args); va_end(args);
    if (n<0 || (size_t)n>=capacity-*length) return false;
    *length+=(size_t)n; return true;
}
static void query_hex(const uint8_t *bytes, size_t count, char *output)
{
    static const char digits[]="0123456789abcdef";
    for (size_t i=0;i<count;++i) { output[i*2]=digits[bytes[i]>>4]; output[i*2+1]=digits[bytes[i]&15U]; }
    output[count*2]=0;
}
static bool query_id(const char *text, ae_id_t *object)
{
    if (strlen(text)!=32) return false;
    for (unsigned i=0;i<16;++i) {
        unsigned value=0;
        for (unsigned j=0;j<2;++j) {
            char c=text[i*2+j]; unsigned digit=c>='0' && c<='9' ? (unsigned)(c-'0') :
                c>='a' && c<='f' ? (unsigned)(c-'a'+10) : c>='A' && c<='F' ? (unsigned)(c-'A'+10) : 16U;
            if (digit==16) return false;
            value=value*16U+digit;
        }
        object->bytes[i]=(uint8_t)value;
    }
    return !ae_id_empty(object);
}
static bool metadata_json(char *json, size_t capacity, size_t *length, const ae_record_metadata_t *m, bool detail)
{
    char id[33]; query_hex(m->object_id.bytes,16,id);
    bool ok=query_json(json,capacity,length,"{\"id\":\"%s\",\"revision\":\"%" PRIu64 "\",\"state\":%u,"
        "\"pcm_bytes\":\"%" PRIu64 "\",\"sample_frames\":\"%" PRIu64 "\",\"expires_unix_seconds\":\"%" PRIu64 "\"",
        id,m->revision,(unsigned)m->state,m->pcm_bytes,m->sample_frames,m->expires_unix_seconds);
    if (ok && detail) {
        char hash[65]; query_hex(m->sha256,32,hash);
        ok=query_json(json,capacity,length,",\"created_unix_seconds\":\"%" PRIu64 "\",\"storage\":%u,"
            "\"sample_rate_hz\":%u,\"channels\":%u,\"stored_bytes\":\"%" PRIu64 "\",\"verified_pcm_bytes\":\"%" PRIu64
            "\",\"synced_pcm_bytes\":\"%" PRIu64 "\",\"owner_download\":%s,\"integrity_verified\":%s,"
            "\"sync_acknowledged\":%s,\"hash_available\":%s,\"sha256\":\"%s\"",m->created_unix_seconds,(unsigned)m->storage,
            (unsigned)m->format.sample_rate_hz,m->format.channels,m->stored_bytes,m->verified_pcm_bytes,m->synced_pcm_bytes,
            m->export_policy==AE_EXPORT_OWNER_DOWNLOAD?"true":"false",m->integrity_verified?"true":"false",
            m->sync_acknowledged?"true":"false",m->hash_available?"true":"false",m->hash_available?hash:"");
    }
    return ok && query_json(json,capacity,length,"}");
}
cp_error_t app_media_runtime_read_recordings(const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{
    if (!identity || !resource || !json || !capacity || !length) return CP_INVALID;
    *length=0; json[0]=0;
    const char *root="/api/v1/recordings"; size_t root_bytes=strlen(root);
    if (strncmp(resource,root,root_bytes)) return CP_NOT_IMPLEMENTED;
    const char *tail=resource+root_bytes; ae_id_t object={0};
    bool list=*tail==0 || *tail=='?'; uint32_t cursor=0; size_t limit=AE_LIST_MAX;
    if (!list) { if (*tail!='/' || !query_id(tail+1,&object)) return CP_INVALID; }
    else if (*tail=='?') {
        ++tail; bool cursor_seen=false,limit_seen=false;
        while (*tail) {
            bool is_cursor=!strncmp(tail,"cursor=",7), is_limit=!strncmp(tail,"limit=",6);
            if ((!is_cursor && !is_limit) || (is_cursor && cursor_seen) || (is_limit && limit_seen)) return CP_INVALID;
            tail+=is_cursor?7:6; uint64_t number=0; unsigned digits=0;
            while (*tail>='0' && *tail<='9') {
                if (++digits>10) return CP_INVALID;
                number=number*10U+(unsigned)(*tail++-'0'); if (number>UINT32_MAX) return CP_INVALID;
            }
            if (!digits || (*tail && *tail!='&')) return CP_INVALID;
            if (is_cursor) { cursor=(uint32_t)number; cursor_seen=true; }
            else { if (!number || number>32) return CP_INVALID; limit=number>AE_LIST_MAX?AE_LIST_MAX:(size_t)number; limit_seen=true; }
            if (*tail) { ++tail; if (!*tail) return CP_INVALID; }
        }
    }
    ae_result_t result=query_admit(identity,&object,list?QUERY_LIST:QUERY_METADATA,0,0,list?limit:0,cursor);
    if (result!=AE_OK) return result==AE_AGAIN || result==AE_BUSY ? CP_BUSY : result==AE_DENIED || result==AE_PRIVACY ?
        CP_FORBIDDEN : result==AE_NOT_FOUND ? CP_STALE : result==AE_TIME_UNKNOWN || result==AE_UNCERTAIN || result==AE_INTEGRITY ?
        CP_STORAGE_UNCERTAIN : CP_UNAVAILABLE;
    portENTER_CRITICAL(&media_lock);
    bool current=media && media->query.ready && !media->query.cancelled &&
        remote_identity_equal(identity,&media->query.identity) && ae_id_equal(&object,&media->query.object) &&
        media->environment.unlocked && !media->environment.privacy_latched && !media->environment.maintenance;
    bool ok=current; uint64_t expires=UINT64_MAX,observed=0;
    if (current) {
        const media_query_t *q=&media->query; observed=q->observed_unix_seconds;
        if (!list) expires=q->metadata.expires_unix_seconds;
        else for (size_t i=0;i<q->count;++i) if (q->records[i].expires_unix_seconds<expires) expires=q->records[i].expires_unix_seconds;
        if (list) {
            ok=query_json(json,capacity,length,"{\"items\":[");
            for (size_t i=0;ok && i<q->count;++i)
                ok=(!i || query_json(json,capacity,length,",")) && metadata_json(json,capacity,length,&q->records[i],false);
            ok=ok && query_json(json,capacity,length,"],\"next_cursor\":%u}",q->next);
        } else ok=metadata_json(json,capacity,length,&q->metadata,true);
    }
    portEXIT_CRITICAL(&media_lock);
    if (!ok) { ae_secure_zero(json,capacity); *length=0; return current?CP_TOO_LARGE:CP_STALE; }
    if (!query_publication_allowed(identity,expires,observed)) { ae_secure_zero(json,capacity); *length=0; return CP_FORBIDDEN; }
    return CP_OK;
}

static uint64_t remote_payload_serial(uint64_t token)
{
    app_media_remote_authority_t policy={0}; cp_identity_t identity={0}; uint64_t serial=0;
    portENTER_CRITICAL(&media_lock);
    if (media && media->command_remote && media->view.result_ready && media->view.request_token==token &&
        !media->remote_cancelled && media->environment.unlocked && !media->environment.privacy_latched && !media->environment.maintenance) {
        policy=media->remote_authority; identity=media->remote_command.identity; serial=media->result_audio_serial;
    }
    portEXIT_CRITICAL(&media_lock);
    return serial && remote_alive(&policy,&identity) ? serial : 0;
}
ae_result_t app_media_runtime_copy_remote_records(uint64_t token, ae_record_metadata_t *items, size_t capacity)
{
    if (!items || capacity>AE_LIST_MAX) return AE_INVALID;
    uint64_t serial=remote_payload_serial(token);
    ae_result_t result=serial ? app_audio_copy_record_list(serial,items,capacity) : AE_DENIED;
    if (result==AE_OK && remote_payload_serial(token)!=serial) result=AE_DENIED;
    if (result!=AE_OK) ae_secure_zero(items,capacity*sizeof(*items));
    return result;
}
ae_result_t app_media_runtime_copy_remote_data(uint64_t token, size_t offset, void *output, size_t bytes)
{
    if (!output || !bytes || bytes>8192) return AE_INVALID;
    uint64_t serial=remote_payload_serial(token);
    ae_result_t result=serial ? app_audio_copy_record_data(serial,offset,output,bytes) : AE_DENIED;
    if (result==AE_OK && remote_payload_serial(token)!=serial) result=AE_DENIED;
    if (result!=AE_OK) ae_secure_zero(output,bytes);
    return result;
}
