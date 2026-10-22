// SPDX-License-Identifier: Apache-2.0
#include "app_remote_media.h"
#include "audio_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_remote_media_state_t state;
static app_remote_media_request_t submitted;
static cp_capture_authority_request_t checked;
static app_audio_result_t reply;
static cp_result_t completed;
static unsigned submissions, cancellations, publications, random_counter;
static bool alive, grant_live, result_ready, publish_allowed, candidate_allowed, other_session_live;
static uint64_t now=1000, key=1;
static const cp_identity_t owner={.principal_id={1},.session_id={2},.scopes=CP_SCOPE_ALL};
static bool observe(void *ctx, app_remote_media_state_t *out) { (void)ctx; *out=state; return true; }
static bool available(void *ctx, cp_operation_id_t operation, char reason[CP_REASON_CAPACITY])
{ (void)ctx; (void)operation; reason[0]=0; return true; }
static bool random_bytes(void *ctx, uint8_t *out, size_t bytes)
{ (void)ctx; memset(out,(int)++random_counter,bytes); return true; }
static bool session(void *ctx, const cp_identity_t *id)
{ (void)ctx; return alive && (other_session_live || (!memcmp(id->principal_id,owner.principal_id,CP_ID_BYTES) && !memcmp(id->session_id,owner.session_id,CP_ID_BYTES))); }
static bool grant(void *ctx, const uint8_t id[CP_ID_BYTES], cp_capture_grant_t *out)
{
    (void)ctx; if (id[0]!=3 && id[0]!=4) return false;
    *out=(cp_capture_grant_t){.revision=id[0]==3?7:9,.principal_id={1}}; memcpy(out->id,id,CP_ID_BYTES); return true;
}
static cp_error_t authority(void *ctx, const cp_capture_authority_request_t *request, cp_capture_authority_t *out)
{
    (void)ctx; checked=*request;
    *out=(cp_capture_authority_t){.allowed=grant_live,.owner_session_alive=alive,.background_allowed=grant_live,
        .source_valid_until_ms=now+100000,.background_valid_until_ms=now+100000}; return CP_OK;
}
static ae_result_t submit(void *ctx, const app_remote_media_request_t *request)
{ (void)ctx; assert(!result_ready); submitted=*request; ++submissions; return AE_OK; }
static bool poll(void *ctx, uint64_t token, app_audio_result_t *out)
{ (void)ctx; if (!result_ready || reply.request_token!=token) return false; *out=reply; return true; }
static void release(void *ctx, uint64_t token)
{ (void)ctx; assert(result_ready && token==reply.request_token); result_ready=false; }
static void cancel(void *ctx, uint64_t token, const cp_identity_t *id, ae_stop_reason_t reason)
{ (void)ctx; (void)reason; assert(token && !memcmp(id->principal_id,owner.principal_id,CP_ID_BYTES)); ++cancellations; }
static bool complete(void *ctx, const cp_result_t *result)
{ (void)ctx; completed=*result; ++publications; return publish_allowed; }
static bool candidate(void *ctx, const cp_command_t *command)
{ (void)ctx; (void)command; return candidate_allowed; }
static app_remote_media_t *adapter;
static void *memory;
static void setup(void)
{
    free(memory); memory=calloc(1,app_remote_media_bytes()); assert(memory);
    alive=grant_live=publish_allowed=true; result_ready=candidate_allowed=other_session_live=false;
    submissions=cancellations=publications=0; now=1000; key=1;
    state=(app_remote_media_state_t){.device={.management_generation=11,.input_epoch=12,.ui_revision=13},
        .audio_known=true,.speech_known=true,.speech_context=true,.audio={.audio_generation=21,.revision=22,.route=AE_ROUTE_LOCAL,
            .microphone_muted=true,.speaker_muted=true,.microphone_mute_applied=true,.speaker_mute_applied=true,
            .microphone_mute_confirmed=true,.speaker_mute_confirmed=true},
        .codec={.implemented=true,.available=true,.full_duplex=true,.format_count=2,
            .formats={{.format={16000,1,20,AE_PCM_S16_LE},.roles=3},{.format={16000,1,10,AE_PCM_S16_LE},.roles=3}}}};
    app_remote_media_port_t port={.observe=observe,.available=available,.random=random_bytes,.session_alive=session,
        .grant=grant,.capture_authority=authority,.submit=submit,.poll=poll,.release_result=release,.cancel=cancel,
        .complete=complete,.candidate_context=candidate};
    assert(app_remote_media_init(memory,app_remote_media_bytes(),&port,&adapter)==CP_OK);
}
static cp_command_t command(cp_operation_id_t operation, const char *args)
{
    cp_command_t c={.version=CP_API_VERSION,.operation=operation,.command_key=key++,.identity=owner,
        .deadline_ms=now+2000,.management_generation=11,.resource_revision=22};
    assert(strlen(args)<sizeof(c.args_json)); strcpy(c.args_json,args); return c;
}
static void storage_alignment_rejects_without_mutation_and_recovers(void)
{
    setup();
    size_t bytes = app_remote_media_bytes(), alignment = app_remote_media_alignment();
    assert(alignment > 4 && !(alignment & (alignment - 1)));
    unsigned char *storage = malloc(bytes + alignment); assert(storage);
    memset(storage, 0xa5, bytes + alignment);
    void *aligned = (void *)(((uintptr_t)storage + alignment - 1) & ~(uintptr_t)(alignment - 1));
    void *weak = (unsigned char *)aligned + 4;
    app_remote_media_t *out = adapter;
    app_remote_media_port_t port = {.observe = observe, .submit = submit, .poll = poll,
        .release_result = release, .session_alive = session};
    assert(app_remote_media_init(weak, bytes, &port, &out) == CP_INVALID && out == adapter);
    for (size_t i = 0; i < bytes + alignment; ++i) assert(storage[i] == 0xa5);
    assert(app_remote_media_init(aligned, bytes, &port, &out) == CP_OK && out == aligned);
    free(storage);
}
static const char *record_args="{\"source\":\"local_microphone\",\"format\":\"wav_pcm_s16le\",\"sample_profile\":\"pcm16_mono_16k\",\"storage\":\"records_partition\",\"duration_seconds\":5,\"byte_limit\":65536,\"retention\":{\"policy\":\"expire_after\",\"duration_seconds\":3600},\"export_policy\":\"owner_download\",\"disconnect_policy\":\"continue_local\",\"activate_microphone\":true,\"consent_id\":\"03000000000000000000000000000000\",\"background_grant_id\":\"04000000000000000000000000000000\"}";
static void started(void)
{
    reply=(app_audio_result_t){.request_token=submitted.token,.result=AE_OK,
        .job={.job_id={{5}},.audio_session_id={{6}},.recording_id={{7}},.request_id=submitted.start.request_id,
            .consumer_generation=31,.kind=submitted.start.kind,.state=AE_JOB_STARTING}};
    state.audio.capture[submitted.start.kind]=reply.job;
    result_ready=true; app_remote_media_service(adapter,++now); assert(!result_ready);
}
static void immutable_and_lifecycle(void)
{
    setup(); cp_command_t c=command(CP_OP_RECORDING_START,record_args); cp_result_t result;
    cp_error_t initial=app_remote_media_execute(adapter,&c,now,&result);
    if (initial!=CP_OK) fprintf(stderr,"initial: %s reason=%s\n",cp_error_name(initial),result.reason);
    assert(initial==CP_OK && result.state==CP_RESULT_ACCEPTED);
    uint64_t token=result.job_id;
    assert(submitted.identity.principal_id[0]==1 && submitted.start.principal_id.bytes[0]==1 &&
        submitted.start.owner_session_id.bytes[0]==2 && submitted.start.consent_revision==7 && submitted.start.background_grant_revision==9);
    assert(checked.file_destination && !checked.live_destination && checked.owner_download && checked.continue_local && checked.bytes==65536);
    assert(app_remote_media_execute(adapter,&c,now,&result)==CP_OK && result.job_id==token && submissions==1);
    c.deadline_ms++; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_REQUEST_CONFLICT); c.deadline_ms--;
    started(); state.audio.capture[1].state=AE_JOB_RUNNING; state.audio.capture[1].device_owned=true;
    state.audio.capture[1].first_frame_seen=true; state.audio.microphone_muted=false; state.audio.microphone_mute_applied=false;
    publish_allowed=false; app_remote_media_service(adapter,++now); assert(completed.state==CP_RESULT_RUNNING);
    unsigned attempts=publications; app_remote_media_service(adapter,++now); assert(publications>attempts && submissions==1);
    publish_allowed=true; app_remote_media_service(adapter,++now);
    char json[2048],resource[96]; size_t length;
    (void)snprintf(resource,sizeof(resource),"/api/v1/jobs/%llu",(unsigned long long)token);
    assert(app_remote_media_read(adapter,&owner,resource,json,sizeof(json),&length)==CP_OK && strstr(json,"\"state\":\"running\""));
    assert(app_remote_media_read(adapter,&owner,"/api/v1/audio/sessions/06000000000000000000000000000000",json,sizeof(json),&length)==CP_OK);
    cp_identity_t restricted=owner; restricted.scopes=CP_SCOPE_VIEW;
    assert(app_remote_media_read(adapter,&restricted,resource,json,sizeof(json),&length)==CP_FORBIDDEN && !length);
    restricted=owner; restricted.session_id[0]=3; other_session_live=true;
    assert(app_remote_media_read(adapter,&restricted,resource,json,sizeof(json),&length)==CP_RESULT_UNKNOWN && !length);
    other_session_live=false; state.device.locked=true;
    assert(app_remote_media_read(adapter,&owner,resource,json,sizeof(json),&length)==CP_LOCKED && !length);
    state.device.locked=false;
    assert(app_remote_media_read(adapter,&owner,"/api/v1/jobs/18446744073709551616",json,sizeof(json),&length)==CP_INVALID && !length);
    const cp_safety_reason_t input_reasons[]={CP_SAFETY_LOCAL_OVERRIDE,CP_SAFETY_LEASE_EXPIRED,
        CP_SAFETY_STATE_CHANGED,CP_SAFETY_CLIENT_STOP,CP_SAFETY_REQUEST_CONFLICT};
    for (unsigned i=0;i<sizeof(input_reasons)/sizeof(input_reasons[0]);++i) {
        app_remote_media_revoke(adapter,input_reasons[i],NULL,++now);
        app_remote_media_revoke(adapter,input_reasons[i],&owner,++now);
        app_remote_media_service(adapter,++now);
        assert(!cancellations && completed.state==CP_RESULT_RUNNING);
    }
    alive=false; app_remote_media_service(adapter,++now); assert(!cancellations); /* Explicit file-only handoff. */
    alive=true; app_remote_media_revoke(adapter,CP_SAFETY_REVOKED,&owner,++now); assert(cancellations==1);
    state.audio.capture[1].state=AE_JOB_PARTIAL; state.audio.capture[1].error=AE_DENIED;
    app_remote_media_service(adapter,++now); assert(completed.state==CP_RESULT_CLEANUP_PENDING);
    state.audio.microphone_muted=state.audio.microphone_mute_applied=state.audio.microphone_mute_confirmed=true;
    app_remote_media_service(adapter,++now); assert(completed.state==CP_RESULT_CANCELLED);
    puts("PASS: typed immutable remote recording, real identity/grant bounds, duplicate conflict, background handoff and physical acknowledgement");
}
static void guards_and_redaction(void)
{
    setup();
    assert(app_remote_media_owns(CP_OP_RECORDING_START) && app_remote_media_owns(CP_OP_AUDIO_STATE));
    assert(!app_remote_media_owns(CP_OP_INPUT_RELEASE_ALL));
    cp_result_t result; cp_command_t c=command(CP_OP_RECORDING_START,record_args);
    c.identity.scopes=CP_SCOPE_VIEW; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_FORBIDDEN);
    c.identity=owner; c.resource_revision--; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_STALE); c.resource_revision++;
    grant_live=false; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_FORBIDDEN && !submissions);
    grant_live=true; state.device.locked=true; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_LOCKED); state.device.locked=false;
    c.deadline_ms=now; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_EXPIRED);
    c=command(CP_OP_AUDIO_PLAYBACK_PAUSE,"{}"); assert(app_remote_media_execute(adapter,&c,now,&result)==CP_NOT_IMPLEMENTED);
    c=command(CP_OP_SPEECH_LISTEN_START,"{\"profile\":\"phonetic_alphanumeric_en\",\"duration_seconds\":5,\"consent_id\":\"03000000000000000000000000000000\"}");
    assert(app_remote_media_execute(adapter,&c,now,&result)==CP_OK);
    assert(submitted.input_epoch==12 && submitted.ui_revision==13 && submitted.start.format.frame_ms==10 && submitted.start.byte_limit==160000);
    started(); state.speech=(sl_view_t){.state=SL_CANDIDATE,.token=submitted.token,.epoch=12,.ui_revision=13,.candidate='B'};
    char json[4096]; size_t length;
    cp_identity_t limited=owner; limited.scopes=CP_SCOPE_VIEW;
    assert(app_remote_media_read(adapter,&limited,"/api/v1/speech/state",json,sizeof(json),&length)==CP_OK && !strstr(json,"\"candidate\":"));
    assert(app_remote_media_read(adapter,&owner,"/api/v1/speech/state",json,sizeof(json),&length)==CP_OK && strstr(json,"\"candidate\":\"B\""));
    c=command(CP_OP_SPEECH_CANDIDATE_CONFIRM,"{\"candidate_id\":\"05000000000000000000000000000000\",\"revision\":\"13\"}");
    assert(app_remote_media_execute(adapter,&c,now,&result)==CP_FORBIDDEN && submissions==1);
    candidate_allowed=true; assert(app_remote_media_execute(adapter,&c,now,&result)==CP_OK && submissions==2);
    assert(submitted.action==APP_REMOTE_MEDIA_SPEECH_CONFIRM);
    assert(app_remote_media_read(adapter,&owner,"/api/v1/audio/capabilities",json,sizeof(json),&length)==CP_OK && strstr(json,"s16le_16000_1_10"));
    assert(app_remote_media_read(adapter,&owner,"/api/v1/audio/state",json,8,&length)==CP_TOO_LARGE && !length && !json[0]);
    assert(app_remote_media_read(adapter,&owner,"/api/v1/recordings/07000000000000000000000000000000/content",json,sizeof(json),&length)==CP_NOT_IMPLEMENTED);
    puts("PASS: current scopes/revisions/session/grants, exact speech context, private candidate redaction and truthful unsupported operations");
}
static void local_network_streams(void)
{
    setup(); cp_result_t result;
    cp_command_t capture=command(CP_OP_AUDIO_CAPTURE_START,"{\"source\":\"local_microphone\",\"sample_profile\":\"s16le_16000_1_10\",\"consent_id\":\"03000000000000000000000000000000\",\"activate_microphone\":true,\"duration_seconds\":5,\"byte_limit\":160000}");
    assert(app_remote_media_execute(adapter,&capture,now,&result)==CP_OK);
    assert(submitted.start.route==AE_ROUTE_LOCAL && submitted.start.kind==AE_CAPTURE_LIVE &&
        checked.live_destination && !checked.file_destination && submitted.start.consent_revision==7);
    started(); char json[2048]; size_t length;
    assert(app_remote_media_read(adapter,&owner,"/api/v1/audio/sessions/06000000000000000000000000000000",json,sizeof(json),&length)==CP_OK);
    assert(strstr(json,"\"media_job_id\":\"05000000000000000000000000000000\"") &&
        strstr(json,"\"consumer_generation\":\"31\"") && strstr(json,"\"audio_generation\":\"21\""));
    setup(); cp_command_t playback=command(CP_OP_AUDIO_PLAYBACK_START,"{\"source\":\"live_stream\",\"sample_profile\":\"s16le_16000_1_10\",\"duration_seconds\":5,\"byte_limit\":160000}");
    assert(app_remote_media_execute(adapter,&playback,now,&result)==CP_OK);
    assert(submitted.start.route==AE_ROUTE_LOCAL && submitted.start.kind==AE_PLAYBACK_STREAM);
    puts("PASS: local-route WebSocket capture/playback admission and private immutable frame binding");
}
int main(void)
{
    storage_alignment_rejects_without_mutation_and_recovers();
    immutable_and_lifecycle(); guards_and_redaction(); local_network_streams();
    printf("Resource bytes: remote media adapter=%zu\n",app_remote_media_bytes()); free(memory); return 0;
}
