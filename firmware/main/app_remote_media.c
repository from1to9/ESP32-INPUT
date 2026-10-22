// SPDX-License-Identifier: Apache-2.0
#include "app_remote_media.h"
#include "audio_media.h"
#include "../../components/control_protocol/control_registry_internal.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool used, waiting, watching, terminal, publication_pending, cancelling;
    uint8_t fingerprint[32];
    app_remote_media_request_t request;
    ae_job_snapshot_t job;
    cp_result_t result;
} media_job_t;
struct app_remote_media {
    app_remote_media_port_t port;
    uint64_t next_token, last_ms;
    bool clock_failed;
    media_job_t jobs[APP_REMOTE_MEDIA_JOBS];
    app_remote_media_state_t state;
    cj_document_t document;
};

static bool same_owner(const cp_identity_t *a, const cp_identity_t *b)
{
    return !memcmp(a->principal_id, b->principal_id, CP_ID_BYTES) &&
        !memcmp(a->session_id, b->session_id, CP_ID_BYTES);
}
static bool empty_id(const uint8_t id[CP_ID_BYTES])
{
    unsigned any = 0; for (unsigned i = 0; i < CP_ID_BYTES; ++i) any |= id[i]; return any == 0;
}
static void hex(const ae_id_t *id, char output[CP_ID_HEX_CAPACITY])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < CP_ID_BYTES; ++i) {
        output[2U*i] = digits[id->bytes[i] >> 4]; output[2U*i+1] = digits[id->bytes[i] & 15U];
    }
    output[32] = 0;
}
static bool decode_id(const char *text, ae_id_t *id)
{
    if (!text || strlen(text) != 32) return false;
    for (unsigned i = 0; i < CP_ID_BYTES; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = text[2U*i+j];
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c-'0') :
                c >= 'a' && c <= 'f' ? (unsigned)(c-'a'+10) : c >= 'A' && c <= 'F' ? (unsigned)(c-'A'+10) : 16U;
            if (d == 16) return false;
            v = 16U*v + d;
        }
        id->bytes[i] = (uint8_t)v;
    }
    return !ae_id_empty(id);
}
static const cp_operation_info_t *info(cp_operation_id_t operation)
{
    for (size_t i = 0; i < cp_operation_count(); ++i) {
        const cp_operation_info_t *p = cp_operation_at(i); if (p && p->id == operation) return p;
    }
    return NULL;
}
static cp_error_t error(ae_result_t r)
{
    switch (r) {
    case AE_OK: return CP_OK;
    case AE_AGAIN: case AE_BUSY: return CP_BUSY;
    case AE_INVALID: return CP_INVALID;
    case AE_DENIED: case AE_PRIVACY: return CP_FORBIDDEN;
    case AE_STALE: case AE_NOT_FOUND: return CP_STALE;
    case AE_TIMEOUT: return CP_EXPIRED;
    case AE_NOT_SUPPORTED: return CP_NOT_IMPLEMENTED;
    case AE_UNCERTAIN: case AE_INTEGRITY: case AE_IO_ERROR: case AE_TIME_UNKNOWN: return CP_STORAGE_UNCERTAIN;
    case AE_TOO_LATE: return CP_TOO_LATE;
    default: return CP_UNAVAILABLE;
    }
}
static cp_error_t reject(cp_result_t *result, cp_error_t e)
{
    result->state = CP_RESULT_REJECTED; result->error = e;
    (void)snprintf(result->reason, sizeof(result->reason), "%s", cp_error_name(e)); return e;
}
bool app_remote_media_owns(cp_operation_id_t operation)
{
    return (operation >= CP_OP_AUDIO_KIND_SELECT && operation <= CP_OP_RECORDINGS_GET) ||
        (operation >= CP_OP_SPEECH_LISTEN_START && operation <= CP_OP_SPEECH_CANDIDATE_REJECT) ||
        operation == CP_OP_AUDIO_CAPABILITIES || operation == CP_OP_AUDIO_STATE || operation == CP_OP_SPEECH_STATE ||
        operation == CP_OP_RECORDINGS_LIST || operation == CP_OP_JOB_GET || operation == CP_OP_JOB_CANCEL;
}
static bool clock_ok(app_remote_media_t *s, uint64_t now)
{
    if (s->clock_failed || now < s->last_ms) { s->clock_failed = true; return false; }
    s->last_ms = now; return true;
}
static const char *string(const app_remote_media_t *s, const char *key)
{ return cj_string(&s->document, cj_get(&s->document, 0, key)); }
static bool unsigned_number(const cj_document_t *document, uint16_t node, uint64_t *out)
{
    int64_t value;
    if (cj_integer(document,node,&value)) { if (value<0) return false; *out=(uint64_t)value; return true; }
    return cj_u64(document,node,out);
}
static bool integer(const app_remote_media_t *s, const char *key, uint64_t *out)
{ return unsigned_number(&s->document,cj_get(&s->document,0,key),out); }
static bool boolean(const app_remote_media_t *s, const char *key)
{
    uint16_t n = cj_get(&s->document, 0, key);
    return n != CJ_NONE && s->document.nodes[n].type == CJ_BOOL && s->document.nodes[n].value.boolean;
}
static bool is_string(const char *a, const char *b) { return a && !strcmp(a, b); }
static ae_route_t route_id(const char *name)
{
    if (is_string(name, "local")) return AE_ROUTE_LOCAL;
    if (is_string(name, "hfp_hf")) return AE_ROUTE_HFP_HF;
    if (is_string(name, "a2dp_sink")) return AE_ROUTE_A2DP_SINK;
    if (is_string(name, "usb_uac")) return AE_ROUTE_USB_UAC;
    return AE_ROUTE_OFF;
}
static bool profile(const char *name, ae_format_t *format)
{
    if (is_string(name, "pcm16_mono_16k")) { *format = (ae_format_t){16000,1,20,AE_PCM_S16_LE}; return true; }
    if (!name || strncmp(name,"s16le_",6)) return false;
    const char *p=name+6; uint32_t values[3]={0};
    for (unsigned i=0;i<3;++i) {
        const char *start=p;
        while (*p>='0' && *p<='9') {
            unsigned digit=(unsigned)(*p++-'0');
            if (values[i]>(UINT32_MAX-digit)/10U) return false;
            values[i]=values[i]*10U+digit;
        }
        if (p==start || (p-start>1 && *start=='0') || (i<2 ? *p!='_' : *p!=0)) return false;
        if (i<2) ++p;
    }
    if (values[1]>2 || values[2]>20) return false;
    *format=(ae_format_t){values[0],(uint8_t)values[1],(uint8_t)values[2],AE_PCM_S16_LE};
    size_t bytes; uint32_t samples;
    return ae_format_size(format, &bytes, &samples)==AE_OK;
}
static bool supported_format(const app_remote_media_state_t *state, const ae_format_t *f, ae_role_t role)
{
    if (!state->codec.implemented || !state->codec.available || state->codec.format_count > AE_FORMATS_MAX) return false;
    for (unsigned i = 0; i < state->codec.format_count; ++i)
        if ((state->codec.formats[i].roles & role) && ae_format_equal(f, &state->codec.formats[i].format)) return true;
    return false;
}
static bool random_id(app_remote_media_t *s, ae_id_t *id)
{ return s->port.random && s->port.random(s->port.user, id->bytes, sizeof(id->bytes)) && !ae_id_empty(id); }
static void fingerprint(const cp_command_t *c, uint8_t digest[32])
{
    /* Serialize fields explicitly; ABI padding and transport callback addresses
     * must never enter an immutable command fingerprint. */
    uint8_t buffer[CP_MAX_ARGS_BYTES + 240U]; size_t n = 0;
    const uint64_t values[] = {c->version,c->operation,c->command_key,c->deadline_ms,c->management_generation,
        c->resource_revision,c->input_epoch,c->transport_generation,c->ui_revision,c->lease_expires_ms,c->identity.scopes,c->identity.browser};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i)
        for (unsigned b = 0; b < 8; ++b) buffer[n++] = (uint8_t)(values[i] >> (8U*b));
    memcpy(buffer+n, c->identity.principal_id, CP_ID_BYTES); n += CP_ID_BYTES;
    memcpy(buffer+n, c->identity.session_id, CP_ID_BYTES); n += CP_ID_BYTES;
    memcpy(buffer+n, c->lease_id, CP_ID_BYTES); n += CP_ID_BYTES;
    memcpy(buffer+n, c->target_id, sizeof(c->target_id)); n += sizeof(c->target_id);
    size_t length = strlen(c->args_json); memcpy(buffer+n, c->args_json, length); n += length;
    cp_sha256(buffer, n, digest); ae_secure_zero(buffer, sizeof(buffer));
}
static media_job_t *find_job(app_remote_media_t *s, uint64_t token, uint64_t command_key, const cp_identity_t *identity)
{
    for (unsigned i = 0; i < APP_REMOTE_MEDIA_JOBS; ++i) {
        media_job_t *j = &s->jobs[i];
        if (j->used && same_owner(identity, &j->request.identity) &&
            ((token && token == j->request.token) || (command_key && command_key == j->request.command_key))) return j;
    }
    return NULL;
}
static media_job_t *owned_source(app_remote_media_t *s, const cp_identity_t *identity,
    const ae_id_t *id, ae_job_kind_t kind, bool recording)
{
    for (unsigned i = 0; i < APP_REMOTE_MEDIA_JOBS; ++i) {
        media_job_t *j = &s->jobs[i];
        if (!j->used || j->request.action != APP_REMOTE_MEDIA_START || j->request.start.kind != kind ||
            memcmp(identity->principal_id, j->request.identity.principal_id, CP_ID_BYTES)) continue;
        const ae_id_t *key = recording ? &j->job.recording_id : &j->job.audio_session_id;
        if ((!id && !j->terminal) || (id && ae_id_equal(id, key))) return j;
    }
    return NULL;
}
static cp_error_t source_grants(app_remote_media_t *s, app_remote_media_request_t *r)
{
    ae_start_request_t *a = &r->start;
    if (!s->port.grant || !s->port.capture_authority) return CP_UNAVAILABLE;
    cp_capture_grant_t grant;
    if (!decode_id(string(s,"consent_id"), &a->consent_id) ||
        !s->port.grant(s->port.user,a->consent_id.bytes,&grant) ||
        memcmp(grant.principal_id,r->identity.principal_id,CP_ID_BYTES)) return CP_FORBIDDEN;
    a->consent_revision = grant.revision;
    if (a->disconnect_policy == AE_DISCONNECT_CONTINUE_LOCAL) {
        if (!decode_id(string(s,"background_grant_id"),&a->background_grant_id) ||
            ae_id_equal(&a->consent_id,&a->background_grant_id) ||
            !s->port.grant(s->port.user,a->background_grant_id.bytes,&grant) ||
            memcmp(grant.principal_id,r->identity.principal_id,CP_ID_BYTES)) return CP_FORBIDDEN;
        a->background_grant_revision = grant.revision;
    }
    cp_capture_authority_request_t request = {.identity=r->identity,.grant_revision=a->consent_revision,
        .background_grant_revision=a->background_grant_revision,.bytes=a->byte_limit,.duration_seconds=a->duration_seconds,
        .retention_seconds=a->retention_seconds,.purpose=a->kind==AE_CAPTURE_RECOGNITION ? CP_CAPTURE_SPEECH :
            a->kind==AE_CAPTURE_RECORDING ? CP_CAPTURE_RECORDING : CP_CAPTURE_LIVE,
        .live_destination=a->kind==AE_CAPTURE_LIVE,.file_destination=a->kind==AE_CAPTURE_RECORDING,
        .owner_download=a->export_policy==AE_EXPORT_OWNER_DOWNLOAD,.continue_local=a->disconnect_policy==AE_DISCONNECT_CONTINUE_LOCAL};
    memcpy(request.grant_id,a->consent_id.bytes,CP_ID_BYTES);
    memcpy(request.background_grant_id,a->background_grant_id.bytes,CP_ID_BYTES);
    cp_capture_authority_t authority = {0};
    cp_error_t e = s->port.capture_authority(s->port.user,&request,&authority);
    if (e != CP_OK) return e;
    return authority.allowed && authority.owner_session_alive && authority.source_valid_until_ms>s->last_ms &&
        (!request.continue_local || (authority.background_allowed && authority.background_valid_until_ms>s->last_ms)) ? CP_OK : CP_FORBIDDEN;
}
static cp_error_t start_request(app_remote_media_t *s, app_remote_media_request_t *r)
{
    ae_start_request_t *a = &r->start; uint64_t duration = 0;
    r->action = APP_REMOTE_MEDIA_START;
    a->kind = r->operation==CP_OP_AUDIO_CAPTURE_START ? AE_CAPTURE_LIVE : r->operation==CP_OP_RECORDING_START ?
        AE_CAPTURE_RECORDING : r->operation==CP_OP_SPEECH_LISTEN_START ? AE_CAPTURE_RECOGNITION :
        is_string(string(s,"source"),"recording") ? AE_PLAYBACK_RECORDING : AE_PLAYBACK_STREAM;
    a->route = s->state.audio.route;
    if (a->route<=AE_ROUTE_OFF || a->route>=AE_ROUTE_COUNT) return CP_UNAVAILABLE;
    if (!integer(s,"duration_seconds",&duration) || duration>UINT32_MAX) return CP_INVALID;
    a->duration_seconds=(uint32_t)duration;
    if (a->kind==AE_CAPTURE_RECOGNITION) {
        if (!is_string(string(s,"profile"),"phonetic_alphanumeric_en") || !s->state.speech_context) return CP_UNAVAILABLE;
        r->input_epoch=s->state.device.input_epoch; r->ui_revision=s->state.device.ui_revision;
        a->format=(ae_format_t){16000,1,10,AE_PCM_S16_LE}; a->byte_limit=duration*32000U;
        a->activate_microphone=true;
    } else {
        if (!profile(string(s,"sample_profile"),&a->format) || !integer(s,"byte_limit",&a->byte_limit)) return CP_INVALID;
        a->activate_microphone=boolean(s,"activate_microphone");
    }
    if (a->kind==AE_CAPTURE_RECORDING) {
        if (!is_string(string(s,"format"),"wav_pcm_s16le")) return CP_NOT_IMPLEMENTED;
        a->storage=is_string(string(s,"storage"),"records_partition") ? AE_STORAGE_RECORDS_PARTITION : AE_STORAGE_NAND;
        uint16_t retention=cj_get(&s->document,0,"retention"); uint64_t seconds;
        if (!unsigned_number(&s->document,cj_get(&s->document,retention,"duration_seconds"),&seconds) || seconds>UINT32_MAX) return CP_INVALID;
        a->retention_seconds=(uint32_t)seconds;
        a->export_policy=is_string(string(s,"export_policy"),"owner_download") ? AE_EXPORT_OWNER_DOWNLOAD : AE_EXPORT_NONE;
        a->disconnect_policy=is_string(string(s,"disconnect_policy"),"continue_local") ? AE_DISCONNECT_CONTINUE_LOCAL : AE_DISCONNECT_STOP;
    }
    if (a->kind==AE_PLAYBACK_RECORDING) {
        if (!(r->identity.scopes&CP_SCOPE_RECORDING_READ)) return CP_FORBIDDEN;
        if (!decode_id(string(s,"recording_id"),&a->recording_id)) return CP_INVALID;
    }
    ae_role_t role=a->kind<=AE_CAPTURE_RECOGNITION ? AE_MICROPHONE : AE_SPEAKER;
    if (!supported_format(&s->state,&a->format,role)) return CP_UNAVAILABLE;
    if (a->route!=AE_ROUTE_LOCAL) {
        const ae_route_capabilities_t *caps=&s->state.routes[a->route];
        if (!caps->implemented || !caps->available || !caps->connected ||
            !ae_format_equal(&caps->format,&a->format) || (role==AE_MICROPHONE ? !caps->capture : !caps->playback)) return CP_UNAVAILABLE;
        a->route_token=caps->token;
    }
    memcpy(a->principal_id.bytes,r->identity.principal_id,CP_ID_BYTES);
    memcpy(a->owner_session_id.bytes,r->identity.session_id,CP_ID_BYTES);
    a->expected_audio_generation=r->audio_generation; a->expected_revision=r->resource_revision;
    if (!random_id(s,&a->request_id)) return CP_INTERNAL;
    return a->kind<=AE_CAPTURE_RECOGNITION ? source_grants(s,r) : CP_OK;
}
static cp_error_t decode(app_remote_media_t *s, const cp_command_t *c, app_remote_media_request_t *r)
{
    *r=(app_remote_media_request_t){.operation=c->operation,.command_key=c->command_key,.deadline_ms=c->deadline_ms,
        .identity=c->identity,.management_generation=c->management_generation,.audio_generation=s->state.audio.audio_generation,
        .resource_revision=c->resource_revision,.input_epoch=c->input_epoch,.ui_revision=c->ui_revision,
        .transport_generation=c->transport_generation,.lease_expires_ms=c->lease_expires_ms};
    memcpy(r->lease_id,c->lease_id,CP_ID_BYTES); memcpy(r->target_id,c->target_id,sizeof(r->target_id));
    switch (c->operation) {
    case CP_OP_AUDIO_CAPTURE_START: case CP_OP_AUDIO_PLAYBACK_START: case CP_OP_RECORDING_START: case CP_OP_SPEECH_LISTEN_START:
        return start_request(s,r);
    case CP_OP_AUDIO_KIND_SELECT:
        r->action=APP_REMOTE_MEDIA_KIND; r->role=is_string(string(s,"kind"),"voice") ? AE_MICROPHONE : AE_SPEAKER; return CP_OK;
    case CP_OP_AUDIO_ROUTE_SELECT: case CP_OP_AUDIO_LINK_CONNECT: case CP_OP_AUDIO_LINK_DISCONNECT:
        r->route=route_id(string(s,"route")); if (r->route==AE_ROUTE_OFF) return CP_INVALID;
        r->action=c->operation==CP_OP_AUDIO_ROUTE_SELECT ? APP_REMOTE_MEDIA_ROUTE : APP_REMOTE_MEDIA_LINK;
        r->value=c->operation==CP_OP_AUDIO_LINK_CONNECT;
        if (string(s,"peer_id")) (void)snprintf(r->peer_id,sizeof(r->peer_id),"%s",string(s,"peer_id"));
        return CP_OK;
    case CP_OP_AUDIO_ENABLE_SET: case CP_OP_AUDIO_MICROPHONE_ENABLE: case CP_OP_AUDIO_SPEAKER_ENABLE:
    case CP_OP_AUDIO_MUTE_SET: case CP_OP_AUDIO_MICROPHONE_MUTE: case CP_OP_AUDIO_SPEAKER_MUTE:
        r->role=c->operation==CP_OP_AUDIO_MICROPHONE_ENABLE || c->operation==CP_OP_AUDIO_MICROPHONE_MUTE ||
            is_string(string(s,"role"),"microphone") ? AE_MICROPHONE : AE_SPEAKER;
        r->action=c->operation==CP_OP_AUDIO_ENABLE_SET || c->operation==CP_OP_AUDIO_MICROPHONE_ENABLE ||
            c->operation==CP_OP_AUDIO_SPEAKER_ENABLE ? APP_REMOTE_MEDIA_ENABLE : APP_REMOTE_MEDIA_MUTE;
        r->value=boolean(s,r->action==APP_REMOTE_MEDIA_ENABLE ? "enabled" : "muted");
        r->format=s->state.audio.format;
        if (!supported_format(&s->state,&r->format,r->role) && s->state.audio.route>AE_ROUTE_OFF && s->state.audio.route<AE_ROUTE_COUNT)
            r->format=s->state.routes[s->state.audio.route].format;
        if (!supported_format(&s->state,&r->format,r->role)) {
            for (unsigned i=0;i<s->state.codec.format_count && i<AE_FORMATS_MAX;++i)
                if (s->state.codec.formats[i].roles&r->role) { r->format=s->state.codec.formats[i].format; break; }
        }
        return CP_OK;
    case CP_OP_AUDIO_CAPTURE_STOP: case CP_OP_AUDIO_PLAYBACK_STOP: case CP_OP_RECORDING_STOP:
    case CP_OP_RECORDING_CANCEL: case CP_OP_SPEECH_LISTEN_STOP: {
        bool recording=c->operation==CP_OP_RECORDING_STOP || c->operation==CP_OP_RECORDING_CANCEL;
        ae_job_kind_t kind=recording ? AE_CAPTURE_RECORDING : c->operation==CP_OP_AUDIO_CAPTURE_STOP ? AE_CAPTURE_LIVE :
            c->operation==CP_OP_SPEECH_LISTEN_STOP ? AE_CAPTURE_RECOGNITION : AE_PLAYBACK_STREAM;
        ae_id_t id={0}; const ae_id_t *key=&id;
        if (kind==AE_CAPTURE_RECOGNITION) key=NULL;
        else if (!decode_id(string(s,recording ? "recording_id" : "session_id"),&id)) return CP_INVALID;
        media_job_t *source=owned_source(s,&c->identity,key,kind,recording);
        if (!source && kind==AE_PLAYBACK_STREAM) source=owned_source(s,&c->identity,key,AE_PLAYBACK_RECORDING,false);
        if (!source || ae_id_empty(&source->job.job_id)) return CP_STALE;
        r->action=APP_REMOTE_MEDIA_STOP_JOB; r->job_id=source->job.job_id;
        r->consumer_generation=source->job.consumer_generation; r->object_id=source->job.recording_id;
        r->stop_reason=c->operation==CP_OP_RECORDING_CANCEL ? AE_STOP_CANCEL_KEEP : AE_STOP_REQUESTED;
        if (is_string(string(s,"disposition"),"discard")) {
            if (!(c->identity.scopes&CP_SCOPE_RECORDING_DELETE)) return CP_FORBIDDEN;
            r->stop_reason=AE_STOP_CANCEL_DISCARD;
        } else if (is_string(string(s,"disposition"),"keep_partial")) r->stop_reason=AE_STOP_CANCEL_KEEP;
        return CP_OK;
    }
    case CP_OP_RECORDING_DELETE:
        if (!decode_id(string(s,"recording_id"),&r->object_id) || !c->resource_revision) return CP_INVALID;
        r->action=APP_REMOTE_MEDIA_RECORD_DELETE; r->object_revision=c->resource_revision; return CP_OK;
    case CP_OP_SPEECH_CANDIDATE_CONFIRM: case CP_OP_SPEECH_CANDIDATE_REJECT: {
        ae_id_t id; uint64_t revision;
        if (!decode_id(string(s,"candidate_id"),&id) || !integer(s,"revision",&revision)) return CP_INVALID;
        media_job_t *source=NULL;
        for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i) {
            media_job_t *j=&s->jobs[i];
            if (j->used && j->request.start.kind==AE_CAPTURE_RECOGNITION && same_owner(&j->request.identity,&c->identity) &&
                ae_id_equal(&j->job.job_id,&id)) source=j;
        }
        if (!source || !s->state.speech_known || s->state.speech.state!=SL_CANDIDATE ||
            s->state.speech.token!=source->request.token || revision!=s->state.speech.ui_revision) return CP_STALE;
        if (c->operation==CP_OP_SPEECH_CANDIDATE_CONFIRM &&
            (!(c->identity.scopes&CP_SCOPE_INPUT_CONTROL) || !s->port.candidate_context ||
                !s->port.candidate_context(s->port.user,c))) return CP_FORBIDDEN;
        r->action=c->operation==CP_OP_SPEECH_CANDIDATE_CONFIRM ? APP_REMOTE_MEDIA_SPEECH_CONFIRM : APP_REMOTE_MEDIA_SPEECH_REJECT;
        r->job_id=source->job.job_id; r->consumer_generation=source->job.consumer_generation;
        r->candidate_token=source->request.token; r->candidate_revision=revision;
        if (c->operation==CP_OP_SPEECH_CANDIDATE_REJECT) { r->input_epoch=s->state.speech.epoch; r->ui_revision=s->state.speech.ui_revision; }
        return CP_OK;
    }
    default: return CP_NOT_IMPLEMENTED;
    }
}
static void publish(app_remote_media_t *s, media_job_t *j)
{
    if (j->publication_pending && s->port.complete && s->port.complete(s->port.user,&j->result)) j->publication_pending=false;
}
static void changed(media_job_t *j, cp_result_state_t state, cp_error_t e, const char *reason, bool terminal)
{
    if (j->result.state!=state || j->result.error!=e || strcmp(j->result.reason,reason)) j->publication_pending=true;
    j->result.state=state; j->result.error=e;
    (void)snprintf(j->result.reason,sizeof(j->result.reason),"%s",reason);
    j->terminal=terminal;
}
size_t app_remote_media_bytes(void) { return sizeof(app_remote_media_t); }
size_t app_remote_media_alignment(void) { return _Alignof(app_remote_media_t); }
cp_error_t app_remote_media_init(void *memory, size_t bytes, const app_remote_media_port_t *port, app_remote_media_t **out)
{
    if (!memory || !port || !out || bytes<sizeof(app_remote_media_t) || (uintptr_t)memory%_Alignof(app_remote_media_t) ||
        !port->observe || !port->submit || !port->poll || !port->release_result || !port->session_alive) return CP_INVALID;
    app_remote_media_t *s=memory; memset(s,0,sizeof(*s)); s->port=*port;
    s->next_token=UINT64_C(0x4000000000000000); *out=s; return CP_OK;
}
bool app_remote_media_available(app_remote_media_t *s, cp_operation_id_t operation, char reason[CP_REASON_CAPACITY])
{
    if (!s || !reason) return false;
    const cp_operation_info_t *op=info(operation);
    if (!app_remote_media_owns(operation) || !op || !op->initial_v1_supported) { (void)snprintf(reason,CP_REASON_CAPACITY,"not_implemented"); return false; }
    if (operation==CP_OP_JOB_GET || operation==CP_OP_JOB_CANCEL) { reason[0]=0; return true; }
    if (!s->port.available) { (void)snprintf(reason,CP_REASON_CAPACITY,"owner_unavailable"); return false; }
    return s->port.available(s->port.user,operation,reason);
}
cp_error_t app_remote_media_execute(app_remote_media_t *s, const cp_command_t *c, uint64_t now, cp_result_t *out)
{
    if (!out) return CP_INVALID;
    *out=(cp_result_t){.command_key=c ? c->command_key : 0,.uncertain_index=-1};
    if (!s || !c || c->version!=CP_API_VERSION || !c->command_key || !memchr(c->args_json,0,sizeof(c->args_json)) ||
        !memchr(c->target_id,0,sizeof(c->target_id))) return reject(out,CP_INVALID);
    if (!clock_ok(s,now)) return reject(out,CP_STORAGE_UNCERTAIN);
    if (empty_id(c->identity.principal_id) || empty_id(c->identity.session_id) ||
        !s->port.session_alive(s->port.user,&c->identity)) return reject(out,CP_UNAUTHENTICATED);
    const cp_operation_info_t *op=info(c->operation);
    if (!op || !app_remote_media_owns(c->operation) || !op->initial_v1_supported) return reject(out,CP_NOT_IMPLEMENTED);
    if ((c->identity.scopes&op->scopes)!=op->scopes) return reject(out,CP_FORBIDDEN);
    const cs_node_t *schema=cp_argument_schema(c->operation);
    if (!schema || !cj_parse(&s->document,c->args_json,strlen(c->args_json)) || !cs_validate(&s->document,0,schema)) return reject(out,CP_INVALID);
    uint8_t hash[32]; fingerprint(c,hash);
    media_job_t *old=find_job(s,0,c->command_key,&c->identity);
    if (old) { if (memcmp(hash,old->fingerprint,sizeof(hash))) return reject(out,CP_REQUEST_CONFLICT); *out=old->result; return CP_OK; }
    if (c->deadline_ms<=now) return reject(out,CP_EXPIRED);
    if (c->operation==CP_OP_JOB_GET || c->operation==CP_OP_JOB_CANCEL) {
        uint64_t token=0,key=0; (void)integer(s,"job_id",&token); (void)integer(s,"command_key",&key);
        media_job_t *j=find_job(s,token,key,&c->identity);
        if (!j) return reject(out,CP_RESULT_UNKNOWN);
        if (c->operation==CP_OP_JOB_CANCEL) {
            const cp_operation_info_t *original=info(j->request.operation);
            if (!original || (c->identity.scopes&original->scopes)!=original->scopes) return reject(out,CP_FORBIDDEN);
            if (j->terminal) return reject(out,CP_TOO_LATE);
            if (!s->port.cancel) return reject(out,CP_UNAVAILABLE);
            s->port.cancel(s->port.user,j->request.token,&j->request.identity,AE_STOP_CANCEL_KEEP); j->cancelling=true;
            *out=j->result; out->command_key=c->command_key; out->state=CP_RESULT_ACCEPTED; return CP_OK;
        }
        *out=j->result; out->command_key=c->command_key; return CP_OK;
    }
    if (!s->port.observe(s->port.user,&s->state) || !s->state.audio_known) return reject(out,CP_UNAVAILABLE);
    if (s->state.device.management_generation!=c->management_generation) return reject(out,CP_STALE);
    if (!op->safety && (s->state.device.locked || s->state.privacy_latched || s->state.maintenance)) return reject(out,CP_LOCKED);
    if (c->operation!=CP_OP_RECORDING_DELETE && !op->safety && c->resource_revision!=s->state.audio.revision) return reject(out,CP_STALE);
    char reason[CP_REASON_CAPACITY]; if (!app_remote_media_available(s,c->operation,reason)) return reject(out,CP_UNAVAILABLE);
    app_remote_media_request_t request; cp_error_t decoded=decode(s,c,&request);
    if (decoded!=CP_OK) return reject(out,decoded);
    media_job_t *slot=NULL;
    for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i) if (!s->jobs[i].used) { slot=&s->jobs[i]; break; }
    if (!slot) for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i) {
        media_job_t *j=&s->jobs[i];
        bool candidate=s->state.speech_known && s->state.speech.state==SL_CANDIDATE && s->state.speech.token==j->request.token;
        if (j->terminal && !j->publication_pending && !candidate && (!slot || j->request.token<slot->request.token)) slot=j;
    }
    if (!slot || s->next_token==UINT64_MAX) return reject(out,CP_BUSY);
    request.token=++s->next_token;
    ae_result_t accepted=s->port.submit(s->port.user,&request);
    if (accepted!=AE_OK) return reject(out,error(accepted));
    memset(slot,0,sizeof(*slot)); slot->used=slot->waiting=true; slot->request=request;
    memcpy(slot->fingerprint,hash,sizeof(hash));
    slot->result=(cp_result_t){.state=CP_RESULT_ACCEPTED,.command_key=c->command_key,.job_id=request.token,.uncertain_index=-1};
    *out=slot->result; return CP_OK;
}
static bool role_stopped(const app_remote_media_t *s, const ae_job_snapshot_t *job)
{
    const ae_snapshot_t *a=&s->state.audio;
    if (job->kind>=AE_PLAYBACK_STREAM) return a->speaker_muted && a->speaker_mute_confirmed;
    for (unsigned i=0;i<AE_CONSUMERS_MAX;++i) {
        const ae_job_snapshot_t *other=&a->capture[i];
        if (!ae_id_equal(&job->job_id,&other->job_id) && (other->state==AE_JOB_STARTING || other->state==AE_JOB_RUNNING)) return true;
    }
    return a->microphone_muted && a->microphone_mute_confirmed;
}
static const ae_job_snapshot_t *observed_job(const app_remote_media_t *s, const ae_id_t *id)
{
    for (unsigned i=0;i<AE_CONSUMERS_MAX;++i) if (ae_id_equal(id,&s->state.audio.capture[i].job_id)) return &s->state.audio.capture[i];
    return ae_id_equal(id,&s->state.audio.playback.job_id) ? &s->state.audio.playback : NULL;
}
void app_remote_media_service(app_remote_media_t *s, uint64_t now)
{
    if (!s) return;
    bool time=clock_ok(s,now); bool observed=s->port.observe(s->port.user,&s->state);
    for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i) {
        media_job_t *j=&s->jobs[i]; if (!j->used) continue;
        if (!j->terminal && !j->cancelling && (!time || !s->port.session_alive(s->port.user,&j->request.identity)) &&
            !(time && j->request.action==APP_REMOTE_MEDIA_START && j->request.start.disconnect_policy==AE_DISCONNECT_CONTINUE_LOCAL && j->job.device_owned)) {
            if (s->port.cancel) s->port.cancel(s->port.user,j->request.token,&j->request.identity,time ? AE_STOP_OWNER_LOST : AE_STOP_TIME);
            j->cancelling=true;
        }
        if (j->waiting) {
            app_audio_result_t result;
            if (s->port.poll(s->port.user,j->request.token,&result)) {
                j->waiting=false;
                if (result.request_token!=j->request.token) {
                    changed(j,CP_RESULT_FAILED,CP_STALE,"result_token_mismatch",true);
                } else if (result.result!=AE_OK) changed(j,CP_RESULT_FAILED,error(result.result),cp_error_name(error(result.result)),true);
                else if (j->request.action==APP_REMOTE_MEDIA_START) {
                    if (ae_id_empty(&result.job.job_id) || !result.job.consumer_generation ||
                        !ae_id_equal(&result.job.request_id,&j->request.start.request_id) || result.job.kind!=j->request.start.kind) {
                        changed(j,CP_RESULT_FAILED,CP_STORAGE_UNCERTAIN,"invalid_start_evidence",true);
                    } else { j->job=result.job; j->watching=true; }
                    hex(ae_id_empty(&j->job.recording_id) ? &j->job.audio_session_id : &j->job.recording_id,j->result.object_id);
                } else if (j->request.action==APP_REMOTE_MEDIA_STOP_JOB) {
                    j->job.job_id=j->request.job_id; j->watching=true;
                } else if (j->request.action==APP_REMOTE_MEDIA_RECORD_DELETE && !result.cleanup_complete) {
                    changed(j,CP_RESULT_FAILED,CP_STORAGE_UNCERTAIN,"cleanup_evidence_unavailable",true);
                } else changed(j,j->cancelling ? CP_RESULT_CANCELLED : CP_RESULT_APPLIED,CP_OK,"owner_acknowledged",true);
                if (result.request_token==j->request.token) s->port.release_result(s->port.user,j->request.token);
            }
        }
        if (j->watching && !j->terminal && observed && s->state.audio_known) {
            const ae_job_snapshot_t *current=observed_job(s,&j->job.job_id);
            if (!current) changed(j,CP_RESULT_FAILED,CP_RESULT_UNKNOWN,"job_evidence_unavailable",true);
            else {
                j->job=*current; j->result.resource_revision=s->state.audio.revision;
                if (current->cleanup_pending) changed(j,CP_RESULT_CLEANUP_PENDING,CP_OK,"cleanup_pending",false);
                else if (current->state==AE_JOB_RUNNING) changed(j,CP_RESULT_RUNNING,CP_OK,"first_frame_received",false);
                else if ((current->state==AE_JOB_COMPLETED || current->state==AE_JOB_PARTIAL || current->state==AE_JOB_ERROR) && !role_stopped(s,current))
                    changed(j,CP_RESULT_CLEANUP_PENDING,s->state.audio.quarantined ? CP_STORAGE_UNCERTAIN : CP_OK,"physical_role_stop_pending",false);
                else if (current->state==AE_JOB_COMPLETED) changed(j,CP_RESULT_COMPLETED,CP_OK,"completed",true);
                else if (current->state==AE_JOB_PARTIAL || current->state==AE_JOB_ERROR) {
                    bool cancel=current->stop_reason==AE_STOP_CANCEL_KEEP || current->stop_reason==AE_STOP_CANCEL_DISCARD || j->cancelling;
                    changed(j,cancel ? CP_RESULT_CANCELLED : CP_RESULT_FAILED,error(current->error),
                        current->state==AE_JOB_PARTIAL ? "partial" : "error",true);
                }
            }
        }
        publish(s,j);
    }
}
void app_remote_media_revoke(app_remote_media_t *s, cp_safety_reason_t reason, const cp_identity_t *identity, uint64_t now)
{
    if (!s) return;
    (void)clock_ok(s,now);
    /* These reasons revoke the independent input lease, not capture grants.
     * In particular, control.release and input.release_all emit CLIENT_STOP. */
    if (reason==CP_SAFETY_LOCAL_OVERRIDE || reason==CP_SAFETY_LEASE_EXPIRED ||
        reason==CP_SAFETY_STATE_CHANGED || reason==CP_SAFETY_CLIENT_STOP ||
        reason==CP_SAFETY_REQUEST_CONFLICT) return;
    for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i) {
        media_job_t *j=&s->jobs[i];
        if (!j->used || j->terminal || (identity && !same_owner(identity,&j->request.identity))) continue;
        if (reason==CP_SAFETY_SESSION_LOST && j->request.action==APP_REMOTE_MEDIA_START &&
            j->request.start.disconnect_policy==AE_DISCONNECT_CONTINUE_LOCAL && j->job.device_owned) continue;
        if (s->port.cancel) s->port.cancel(s->port.user,j->request.token,&j->request.identity,
            reason==CP_SAFETY_SESSION_LOST ? AE_STOP_OWNER_LOST : AE_STOP_PRIVACY);
        j->cancelling=true;
    }
}
static bool append(char *json, size_t capacity, size_t *length, const char *format, ...)
{
    if (*length>=capacity) return false;
    va_list args; va_start(args,format); int n=vsnprintf(json+*length,capacity-*length,format,args); va_end(args);
    if (n<0 || (size_t)n>=capacity-*length) return false;
    *length+=(size_t)n; return true;
}
static bool decimal_token(const char *text, uint64_t *token)
{
    if (!text || *text<'1' || *text>'9') return false;
    uint64_t value=0;
    for (const char *p=text;*p;++p) {
        if (*p<'0' || *p>'9') return false;
        unsigned digit=(unsigned)(*p-'0');
        if (value>(UINT64_MAX-digit)/10U) return false;
        value=value*10U+digit;
    }
    *token=value; return true;
}
cp_error_t app_remote_media_read(app_remote_media_t *s, const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{
    if (!s || !identity || !resource || !json || !length || !capacity) return CP_INVALID;
    *length=0; json[0]=0;
    if (!s->port.session_alive(s->port.user,identity)) return CP_UNAUTHENTICATED;
    if (!strncmp(resource,"/api/v1/recordings",18)) {
        if (!(identity->scopes&CP_SCOPE_RECORDING_READ)) return CP_FORBIDDEN;
        if (strstr(resource,"/content")) return CP_NOT_IMPLEMENTED;
        return s->port.read_recordings ? s->port.read_recordings(s->port.user,identity,resource,json,capacity,length) : CP_UNAVAILABLE;
    }
    if (!(identity->scopes&CP_SCOPE_VIEW)) return CP_FORBIDDEN;
    if (!s->port.observe(s->port.user,&s->state)) return CP_UNAVAILABLE;
    const ae_snapshot_t *a=&s->state.audio; bool ok=false;
    if (!strncmp(resource,"/api/v1/jobs/",13) || !strncmp(resource,"/api/v1/audio/sessions/",23)) {
        if (s->state.device.locked || s->state.privacy_latched || s->state.maintenance) return CP_LOCKED;
        media_job_t *job=NULL;
        if (!strncmp(resource,"/api/v1/jobs/",13)) {
            uint64_t token;
            if (!decimal_token(resource+13,&token)) return CP_INVALID;
            job=find_job(s,token,0,identity);
        } else {
            ae_id_t id;
            if (!decode_id(resource+23,&id)) return CP_INVALID;
            for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i) {
                media_job_t *candidate=&s->jobs[i];
                if (candidate->used && candidate->request.action==APP_REMOTE_MEDIA_START &&
                    same_owner(identity,&candidate->request.identity) && ae_id_equal(&id,&candidate->job.audio_session_id)) {
                    job=candidate; break;
                }
            }
        }
        if (!job) return CP_RESULT_UNKNOWN;
        const cp_operation_info_t *operation=info(job->request.operation);
        if (!operation || (identity->scopes&operation->scopes)!=operation->scopes) return CP_FORBIDDEN;
        const ae_job_snapshot_t *current=s->state.audio_known ? observed_job(s,&job->job.job_id) : NULL;
        if (!current && job->watching && !job->terminal) return CP_UNAVAILABLE;
        const ae_job_snapshot_t *view=current ? current : &job->job;
        char session_id[CP_ID_HEX_CAPACITY],media_job_id[CP_ID_HEX_CAPACITY];
        hex(&view->audio_session_id,session_id); hex(&view->job_id,media_job_id);
        ok=append(json,capacity,length,"{\"job_id\":\"%" PRIu64 "\",\"command_key\":\"%" PRIu64
            "\",\"state\":\"%s\",\"error\":\"%s\",\"session_id\":\"%s\",\"media_state\":%u,"
            "\"cleanup_pending\":%s,\"first_frame_seen\":%s,\"received_bytes\":\"%" PRIu64
            "\",\"delivered_bytes\":\"%" PRIu64 "\",\"sample_frames\":\"%" PRIu64 "\","
            "\"media_job_id\":\"%s\",\"audio_generation\":\"%" PRIu64 "\",\"consumer_generation\":\"%" PRIu64
            "\",\"sample_rate_hz\":%u,\"channels\":%u,\"frame_ms\":%u}",
            job->request.token,job->request.command_key,cp_result_state_name(job->result.state),cp_error_name(job->result.error),
            session_id,(unsigned)view->state,view->cleanup_pending?"true":"false",view->first_frame_seen?"true":"false",
            view->received_bytes,view->delivered_bytes,view->sample_frames,media_job_id,
            current ? a->audio_generation : 0,view->consumer_generation,
            (unsigned)job->request.start.format.sample_rate_hz,job->request.start.format.channels,job->request.start.format.frame_ms);
    } else if (!strcmp(resource,"/api/v1/audio/state")) {
        if (!s->state.audio_known) return CP_UNAVAILABLE;
        ok=append(json,capacity,length,"{\"audio_generation\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64
            "\",\"state\":%u,\"route\":%u,\"microphone_muted\":%s,\"microphone_mute_applied\":%s,\"microphone_mute_confirmed\":%s,"
            "\"microphone_mute_pending\":%s,\"speaker_muted\":%s,\"speaker_mute_applied\":%s,\"speaker_mute_confirmed\":%s,"
            "\"speaker_mute_pending\":%s,\"capture_indicator_required\":%s,\"volume_percent\":%u,\"gain_db\":%u}",
            a->audio_generation,a->revision,(unsigned)a->state,(unsigned)a->route,a->microphone_muted?"true":"false",
            a->microphone_mute_applied?"true":"false",a->microphone_mute_confirmed?"true":"false",a->microphone_mute_pending?"true":"false",
            a->speaker_muted?"true":"false",a->speaker_mute_applied?"true":"false",a->speaker_mute_confirmed?"true":"false",
            a->speaker_mute_pending?"true":"false",a->capture_indicator_required?"true":"false",a->volume_percent,a->microphone_gain_db);
    } else if (!strcmp(resource,"/api/v1/audio/capabilities")) {
        const ae_codec_capabilities_t *c=&s->state.codec;
        ok=append(json,capacity,length,"{\"implemented\":%s,\"available\":%s,\"qualified\":%s,\"profiles\":[",
            c->implemented?"true":"false",c->available?"true":"false",c->qualified?"true":"false");
        for (unsigned i=0;ok && i<c->format_count && i<AE_FORMATS_MAX;++i) {
            const ae_format_t *f=&c->formats[i].format;
            ok=append(json,capacity,length,"%s{\"id\":\"s16le_%u_%u_%u\",\"roles\":%u}",i?",":"",
                (unsigned)f->sample_rate_hz,f->channels,f->frame_ms,c->formats[i].roles);
        }
        ok=ok && append(json,capacity,length,"],\"speech_profile\":\"phonetic_alphanumeric_en\"}");
    } else if (!strcmp(resource,"/api/v1/speech/state")) {
        if (!s->state.speech_known) return CP_UNAVAILABLE;
        /* Ordinary view never exposes a candidate/transcript or another
         * principal's session identity. Owner reads require capture scope. */
        media_job_t *owner=NULL;
        for (unsigned i=0;i<APP_REMOTE_MEDIA_JOBS;++i)
            if (s->jobs[i].used && s->jobs[i].request.token==s->state.speech.token &&
                same_owner(identity,&s->jobs[i].request.identity)) owner=&s->jobs[i];
        bool private_view=owner && (identity->scopes&CP_SCOPE_AUDIO_CAPTURE);
        ok=append(json,capacity,length,"{\"state\":%u,\"cleanup_pending\":%s,\"candidate_available\":%s",
            (unsigned)s->state.speech.state,s->state.speech.cleanup_pending?"true":"false",
            private_view && s->state.speech.state==SL_CANDIDATE?"true":"false");
        if (ok && private_view && s->state.speech.state==SL_CANDIDATE) {
            char id[CP_ID_HEX_CAPACITY]; hex(&owner->job.job_id,id);
            char candidate=s->state.speech.candidate;
            if (!((candidate>='A' && candidate<='Z') || (candidate>='0' && candidate<='9'))) return CP_INTERNAL;
            ok=append(json,capacity,length,",\"candidate_id\":\"%s\",\"revision\":\"%" PRIu64 "\",\"candidate\":\"%c\"",id,s->state.speech.ui_revision,candidate);
        }
        ok=ok && append(json,capacity,length,"}");
    } else return CP_NOT_IMPLEMENTED;
    if (!ok) { ae_secure_zero(json,capacity); *length=0; return CP_TOO_LARGE; }
    if (!s->port.session_alive(s->port.user,identity)) {
        ae_secure_zero(json,capacity); *length=0; return CP_UNAUTHENTICATED;
    }
    return CP_OK;
}
