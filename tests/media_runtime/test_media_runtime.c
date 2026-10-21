// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_media_runtime.c"

static unsigned lock_depth, id_counter, safety_stops, commands, speech_starts, speech_cancels;
static uint64_t now = 1000;
static ae_dependencies_t captured;
static ae_snapshot_t audio;
static app_audio_command_t submitted;
static app_audio_result_t completion;
static bool completion_ready;
static bool remote_session_live=true, remote_grant_live=true, query_copy_allowed;
static unsigned session_calls, session_fail_after;
static uint64_t query_unix_seconds=1000;
static cp_capture_authority_request_t checked_authority;
static ae_record_metadata_t query_metadata;
static sl_view_t speech;
static bt_audio_snapshot_t bluetooth;
static unsigned bluetooth_requests, bluetooth_cancels;
static size_t ingress_capacity;
static unsigned transport_frames, transport_reads, ingress_frames;
static unsigned playback_bindings;
static app_control_media_port_t network_port;
static uint64_t channel_generation=17;
static ae_route_token_t route_token = {.peer_id={{1}}, .stream_id={{2}}, .link_generation=3};
void usb_test_enter(portMUX_TYPE *lock) { (void)lock; assert(!lock_depth); ++lock_depth; }
void usb_test_leave(portMUX_TYPE *lock) { (void)lock; assert(lock_depth == 1); --lock_depth; }
int64_t esp_timer_get_time(void) { return (int64_t)(now * 1000); }
void esp_fill_random(void *data, size_t bytes) { memset(data, (int)++id_counter, bytes); }
void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps)
{ assert(!lock_depth && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return calloc(count, bytes); }
void heap_caps_free(void *data) { free(data); }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *argument,
    unsigned priority, TaskHandle_t *handle)
{ assert(fn == worker && !strcmp(name,"media_control") && stack==6144 && argument && priority==3 && !handle); return pdPASS; }
void vTaskDelay(TickType_t delay) { (void)delay; abort(); }
ae_result_t board_audio_mosaico_init(void) { return AE_OK; }
ae_codec_port_t board_audio_mosaico_port(void) { return (ae_codec_port_t){0}; }
board_audio_status_t board_audio_mosaico_status(void)
{ return (board_audio_status_t){.hardware_observed=true, .last_result=AE_OK}; }
esp_err_t bt_audio_init(void) { return ESP_OK; }
void bt_audio_service(void) { }
bt_audio_snapshot_t bt_audio_snapshot(void) { return bluetooth; }
bt_audio_bond_list_t bt_audio_bond_list(void) { return (bt_audio_bond_list_t){.known=true,.revision=9,.count=1,.bonds={{{1,2,3,4,5,6},false,{0},false,false}}}; }
uint64_t bt_audio_cancel(void) { ++bluetooth_cancels; return ++bluetooth.generation; }
esp_err_t bt_audio_request(uint64_t generation, bt_audio_command_t command, const uint8_t peer[6], uint64_t *id)
{
    (void)command; (void)peer;
    if (generation != bluetooth.generation) return ESP_ERR_INVALID_STATE;
    ++bluetooth_requests; *id = ++bluetooth.request_id; bluetooth.request_status = BT_AUDIO_JOB_PENDING;
    return ESP_OK;
}
esp_err_t bt_audio_saved_request(uint64_t generation, uint64_t revision, bt_audio_command_t command,
    const uint8_t peer[6], uint64_t *id)
{ assert(revision == 9); return bt_audio_request(generation, command, peer, id); }

static ae_result_t caps(void *context, ae_route_t route, ae_route_capabilities_t *out)
{ (void)context; assert(route==AE_ROUTE_USB_UAC || route==AE_ROUTE_HFP_HF || route==AE_ROUTE_A2DP_SINK);
  *out=(ae_route_capabilities_t){.implemented=true,.available=true,.connected=true,
    .capture=true,.playback=true,.token=route_token,.format={48000,1,10,AE_PCM_S16_LE}}; return AE_OK; }
static ae_result_t bind_playback(void *context, ae_route_t route, uint64_t generation,
    const ae_route_token_t *token, uint64_t consumer, bool enabled)
{
    assert(!context && !lock_depth && route==AE_ROUTE_USB_UAC && generation==23 &&
        token && ae_route_token_equal(token,&route_token) && consumer==29);
    ++playback_bindings; return enabled ? AE_AGAIN : AE_OK;
}
ae_route_port_t usb_audio_route_port(void) { return (ae_route_port_t){.capabilities=caps,.bind_playback=bind_playback}; }
ae_route_port_t bt_audio_route_port(void) { return (ae_route_port_t){.capabilities=caps}; }
ae_result_t usb_audio_receive(ae_frame_t *frame)
{
    ++transport_reads;
    if (!transport_frames) return AE_AGAIN;
    --transport_frames; *frame = (ae_frame_t){.sequence = transport_reads}; return AE_OK;
}
ae_result_t bt_audio_receive(ae_frame_t *frame) { (void)frame; return AE_AGAIN; }
void app_control_server_set_media_port(const app_control_media_port_t *port) { network_port=*port; }
uint64_t app_control_server_channel_generation(const cp_identity_t *identity)
{ assert(!lock_depth); return identity->principal_id[0]==9 && identity->session_id[0]==10 ? channel_generation : 0; }
esp_err_t app_control_server_capture_send(const cp_identity_t *identity, const uint8_t job[16],
    uint64_t consumer, uint64_t channel, const uint8_t *bytes, size_t length)
{ (void)identity; (void)job; (void)consumer; (void)channel; (void)bytes; (void)length; return ESP_ERR_INVALID_STATE; }
void app_control_server_capture_cancel(const cp_identity_t *identity, const uint8_t job[16], uint64_t consumer, uint64_t channel)
{ (void)identity; (void)job; (void)consumer; (void)channel; }
ae_result_t app_audio_try_submit_playback(const ae_frame_t *frame) { (void)frame; return AE_AGAIN; }
ae_result_t app_audio_init(const ae_dependencies_t *dependencies) { captured=*dependencies; return AE_OK; }
ae_result_t app_audio_submit(const app_audio_command_t *command)
{ assert(!completion_ready); submitted=*command; ++commands; return AE_OK; }
bool app_audio_take_result(app_audio_result_t *out)
{ if(!completion_ready) return false; *out=completion; completion_ready=false; return true; }
void app_audio_set_environment(const ae_environment_t *environment) { (void)environment; }
void app_audio_request_safety_stop(ae_stop_reason_t reason) { (void)reason; ++safety_stops; }
size_t app_audio_transport_playback_capacity(ae_route_t route, uint64_t generation)
{ assert(route == audio.route && generation == audio.audio_generation); return ingress_capacity; }
ae_result_t app_audio_submit_transport_playback(ae_route_t route, const ae_frame_t *frame)
{
    assert(route == audio.route && ingress_capacity && frame->sequence == transport_reads);
    --ingress_capacity; ++ingress_frames; return AE_OK;
}
bool app_audio_snapshot(ae_snapshot_t *out) { *out=audio; return true; }
ae_result_t app_spoken_letters_init(void) { speech=(sl_view_t){.state=SL_IDLE}; return AE_OK; }
ae_result_t app_spoken_letters_begin(const sl_binding_t *binding)
{ ++speech_starts; speech=(sl_view_t){.state=SL_LISTENING,.token=binding->token,.epoch=binding->epoch,.ui_revision=binding->ui_revision}; return AE_OK; }
ae_result_t app_spoken_letters_capture(const ae_id_t *job, const ae_frame_t *frame)
{ (void)job; (void)frame; return AE_OK; }
void app_spoken_letters_cancel(const ae_id_t *job, uint64_t generation)
{ (void)job; (void)generation; ++speech_cancels; speech.state=SL_CANCELLED; }
void app_spoken_letters_capture_closed(const ae_id_t *job, uint64_t generation)
{ (void)job; (void)generation; }
bool app_spoken_letters_snapshot(sl_view_t *out) { *out=speech; return true; }
ae_result_t app_spoken_letters_confirm(uint64_t token, uint64_t epoch, uint64_t revision, char *candidate)
{ assert(token==speech.token && epoch==speech.epoch && revision==speech.ui_revision);
  if(speech.state!=SL_CANDIDATE) return AE_STALE; *candidate=speech.candidate; speech.state=SL_IDLE; return AE_OK; }

static void setup(void)
{
    if(media) free(media); media=NULL; media_attempted=false;
    ingress_capacity = 4; transport_frames = transport_reads = ingress_frames = 0;
    bluetooth = (bt_audio_snapshot_t){.generation = 7}; bluetooth_requests = bluetooth_cancels = 0;
    commands=safety_stops=speech_starts=speech_cancels=0; completion_ready=false; channel_generation=17;
    audio=(ae_snapshot_t){.route=AE_ROUTE_OFF,.audio_generation=1,.revision=1,.hardware_off_confirmed=true};
    const app_media_services_t services={0};
    assert(app_media_runtime_init(&services)==AE_OK);
    assert(network_port.user==media && network_port.capture_allowed && network_port.capture_failed && network_port.playback && network_port.channel_lost);
    ae_environment_t environment={.unlocked=true,.screen_visible=true};
    app_media_runtime_environment(&environment,10,20,true);
}
static app_media_local_request_t request(ae_job_kind_t kind, ae_route_t route)
{
    return (app_media_local_request_t){.token=1,.epoch=10,.ui_revision=20,.action=APP_MEDIA_START,
        .kind=kind,.route=route,.format={route==AE_ROUTE_LOCAL?16000U:48000U,1,10,AE_PCM_S16_LE},
        .duration_seconds=kind==AE_CAPTURE_RECOGNITION?8U:300U,.byte_limit=32U*1024U*1024U};
}
static void acknowledge(ae_result_t result)
{
    completion=(app_audio_result_t){.request_token=submitted.request_token,.result=result,
        .job={.job_id={{8}},.request_id=submitted.start.request_id,.consumer_generation=7}};
    if(result==AE_OK && submitted.kind==APP_AUDIO_CMD_ROUTE) { audio.route=submitted.route; ++audio.revision; }
    if(result==AE_OK && submitted.kind==APP_AUDIO_CMD_START) { ++audio.audio_generation; ++audio.revision; }
    completion_ready=true; media_step(media);
}
static void admit_start(app_media_local_request_t req)
{
    assert(app_media_runtime_submit_local(&req)==AE_OK);
    media_step(media); assert(submitted.kind==APP_AUDIO_CMD_ROUTE);
    acknowledge(AE_OK); media_step(media); assert(submitted.kind==APP_AUDIO_CMD_START);
}
static bool session_live(void *context, const cp_identity_t *identity)
{ (void)context; assert(!lock_depth); ++session_calls; return (!session_fail_after || session_calls<=session_fail_after) && remote_session_live && identity->principal_id[0]==9 && identity->session_id[0]==10; }
static cp_error_t capture_authority(void *context, const cp_capture_authority_request_t *request, cp_capture_authority_t *out)
{
    (void)context; assert(!lock_depth); checked_authority=*request;
    *out=(cp_capture_authority_t){.allowed=remote_grant_live,.owner_session_alive=remote_session_live,
        .background_allowed=true,.source_valid_until_ms=now+60000,.background_valid_until_ms=now+60000}; return CP_OK;
}
static cp_identity_t remote_identity(void)
{ return (cp_identity_t){.principal_id={9},.session_id={10},.scopes=CP_SCOPE_RECORDING_CONTROL|CP_SCOPE_RECORDING_READ|CP_SCOPE_RECORDING_DELETE|CP_SCOPE_AUDIO_CONTROL}; }
static ae_result_t query_clock(void *context, uint64_t *seconds) { (void)context; *seconds=query_unix_seconds; return AE_OK; }
static void remote_setup(void)
{
    setup(); session_calls=session_fail_after=0; query_unix_seconds=1000; remote_session_live=remote_grant_live=true; query_copy_allowed=true; media->services.wall_time=query_clock;
    audio.route=AE_ROUTE_LOCAL; media_step(media);
    app_media_remote_authority_t auth={.session_alive=session_live,.capture_authority=capture_authority};
    assert(app_media_runtime_set_remote_authority(&auth)==AE_OK);
    query_metadata=(ae_record_metadata_t){.object_id={{55}},.principal_id={{9}},.revision=7,.state=AE_RECORD_COMPLETED,
        .format={16000,1,10,AE_PCM_S16_LE},.pcm_bytes=4096,.sample_frames=2048,.export_policy=AE_EXPORT_OWNER_DOWNLOAD,
        .integrity_verified=true,.hash_available=true,.sync_acknowledged=true,.expires_unix_seconds=100000};
}
static void query_ack(void)
{
    completion=(app_audio_result_t){.request_token=submitted.request_token,.result=AE_OK,
        .metadata=query_metadata,.bytes=submitted.kind==APP_AUDIO_CMD_RECORD_READ?submitted.capacity:0,.count=1};
    completion_ready=true; media_step(media);
}
static void remote_and_queries(void)
{
    remote_setup(); cp_identity_t identity=remote_identity();
    app_remote_media_request_t request={.token=1000,.command_key=2000,.deadline_ms=now+2000,.identity=identity,
        .operation=CP_OP_RECORDING_START,.action=APP_REMOTE_MEDIA_START,.audio_generation=audio.audio_generation,
        .resource_revision=audio.revision,.input_epoch=10,.ui_revision=20,
        .start={.request_id={{11}},.principal_id={{9}},.owner_session_id={{10}},.consent_id={{12}},.consent_revision=4,
            .expected_audio_generation=audio.audio_generation,.expected_revision=audio.revision,.kind=AE_CAPTURE_RECORDING,
            .route=AE_ROUTE_LOCAL,.format={16000,1,10,AE_PCM_S16_LE},.duration_seconds=5,.byte_limit=65536,
            .retention_seconds=3600,.storage=AE_STORAGE_RECORDS_PARTITION,.activate_microphone=true}};
    assert(app_media_runtime_submit_remote(&request)==AE_OK); media_step(media);
    assert(submitted.kind==APP_AUDIO_CMD_START && same_request(&submitted.start,&request.start));
    assert(checked_authority.grant_revision==4 && checked_authority.bytes==65536 &&
        checked_authority.duration_seconds==5 && checked_authority.file_destination && !checked_authority.live_destination);
    ae_authority_t permission; ae_start_request_t widened=request.start; ++widened.byte_limit;
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&widened,now,&permission)==AE_OK && !permission.allowed);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && permission.allowed);
    remote_grant_live=false;
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && !permission.allowed);
    remote_grant_live=true; acknowledge(AE_OK);
    channel_generation=0; network_port.channel_lost(network_port.user,&identity,17);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && permission.allowed);
    assert(!media->remote_cancelled); /* File-only capture does not require a WSS channel. */
    uint64_t token; ae_result_t result; app_audio_result_t reply;
    assert(!app_media_runtime_take_result(&token,&result));
    app_media_snapshot_t view; assert(app_media_runtime_snapshot(&view) && !view.result_ready);
    assert(!app_media_runtime_poll_remote(999,&reply));
    assert(app_media_runtime_poll_remote(1000,&reply) && reply.request_token==1000 && reply.result==AE_OK);
    app_media_runtime_release_remote_result(999); assert(app_media_runtime_poll_remote(1000,&reply));
    app_media_runtime_release_remote_result(1000); assert(!app_media_runtime_poll_remote(1000,&reply));
    app_media_runtime_cancel_remote(999,&identity,AE_STOP_OWNER_LOST);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && permission.allowed);
    app_media_runtime_cancel_remote(1000,&identity,AE_STOP_OWNER_LOST);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && !permission.allowed);
    assert(!safety_stops); /* Exact remote revocation cannot stop independent local jobs. */
    audio.capture[AE_CAPTURE_RECORDING]=(ae_job_snapshot_t){.job_id={{8}},.request_id=request.start.request_id,
        .consumer_generation=8,.state=AE_JOB_FINALIZING,.cleanup_pending=true,.kind=AE_CAPTURE_RECORDING};
    app_media_runtime_cancel_remote(1000,&identity,AE_STOP_CANCEL_KEEP); media_step(media);
    assert(submitted.kind==APP_AUDIO_CMD_STOP && submitted.consumer_generation==7 && submitted.stop_reason==AE_STOP_CANCEL_KEEP);
    assert(media->cancel_waiting && !safety_stops);
    assert(app_media_runtime_record_metadata(&identity,&query_metadata.object_id,&query_metadata)==AE_BUSY);
    acknowledge(AE_TOO_LATE);
    assert(!media->cancel_waiting && !media->grants[AE_CAPTURE_RECORDING].cancel_requested);
    assert(!media->view.result_ready && !app_media_runtime_poll_remote(1000,&reply));
    app_remote_media_request_t stop=request;
    stop.token=1001; stop.command_key=2001; stop.action=APP_REMOTE_MEDIA_STOP_JOB; stop.operation=CP_OP_RECORDING_CANCEL;
    stop.audio_generation=audio.audio_generation; stop.resource_revision=audio.revision;
    stop.job_id=audio.capture[AE_CAPTURE_RECORDING].job_id; stop.consumer_generation=8; stop.stop_reason=AE_STOP_CANCEL_KEEP;
    assert(app_media_runtime_submit_remote(&stop)==AE_OK); media_step(media);
    assert(submitted.kind==APP_AUDIO_CMD_STOP && submitted.consumer_generation==7);
    acknowledge(AE_TOO_LATE);
    assert(app_media_runtime_poll_remote(1001,&reply) && reply.result==AE_TOO_LATE);
    app_media_runtime_release_remote_result(1001);
    app_media_local_request_t local={.token=1,.epoch=10,.ui_revision=20,.action=APP_MEDIA_VOLUME,.level=35};
    assert(app_media_runtime_submit_local(&local)==AE_OK); media_step(media); acknowledge(AE_OK);
    assert(!app_media_runtime_poll_remote(1,&reply));
    assert(app_media_runtime_take_result(&token,&result) && token==1);
    remote_setup(); identity=remote_identity(); channel_generation=0; uint8_t bytes[32]; size_t actual=99;
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,7,44,bytes,sizeof(bytes),&actual)==AE_AGAIN && !actual);
    media_step(media); assert(submitted.kind==APP_AUDIO_CMD_RECORD_GET);
    ae_start_request_t query={.principal_id={{9}},.recording_id={{55}},.kind=AE_PLAYBACK_RECORDING};
    assert(captured.authority(captured.policy_context,AE_AUTH_RECORD_READ,&query,now,&permission)==AE_OK && permission.allowed);
    query.recording_id.bytes[0]=56;
    assert(captured.authority(captured.policy_context,AE_AUTH_RECORD_READ,&query,now,&permission)==AE_OK && !permission.allowed);
    query_ack(); assert(submitted.kind==APP_AUDIO_CMD_RECORD_READ && submitted.offset==44 && submitted.capacity==32);
    query_ack(); assert(submitted.kind==APP_AUDIO_CMD_RECORD_GET);
    query_ack();
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,7,44,bytes,sizeof(bytes),&actual)==AE_OK && actual==32);
    for (size_t i=0;i<actual;++i) assert(bytes[i]==0x5a);
    assert(!media->query.ready); /* A successful chunk is consumed, never a reusable stale plaintext cache. */
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,7,76,bytes,sizeof(bytes),&actual)==AE_AGAIN);
    media_step(media); query_ack(); query_ack(); ++query_metadata.revision; query_ack();
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,7,76,bytes,sizeof(bytes),&actual)==AE_STALE && !actual);
    for (size_t i=0;i<sizeof(bytes);++i) assert(!bytes[i]);
    app_media_runtime_content_cancel(&identity,&query_metadata.object_id);
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,8,76,bytes,sizeof(bytes),&actual)==AE_AGAIN);
    media_step(media); remote_session_live=false; query_ack();
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,8,76,bytes,sizeof(bytes),&actual)==AE_DENIED && !actual);
    assert(!media->query.pending && !media->query.bytes);
    remote_session_live=true; app_media_runtime_content_cancel(&identity,&query_metadata.object_id);
    char json[4096]; size_t length;
    assert(app_media_runtime_read_recordings(&identity,"/api/v1/recordings?limit=32&cursor=0",json,sizeof(json),&length)==CP_BUSY);
    media_step(media); assert(submitted.kind==APP_AUDIO_CMD_RECORD_LIST && submitted.capacity==16); query_ack();
    assert(app_media_runtime_read_recordings(&identity,"/api/v1/recordings?limit=32&cursor=0",json,sizeof(json),&length)==CP_OK && length);
    assert(strstr(json,"37000000000000000000000000000000"));
    cp_identity_t other=identity; other.principal_id[0]=8;
    assert(app_media_runtime_read_recordings(&other,"/api/v1/recordings",json,sizeof(json),&length)==CP_FORBIDDEN);
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,8,44,bytes,sizeof(bytes),&actual)==AE_AGAIN);
    media_step(media); query_ack(); query_ack(); query_ack();
    session_fail_after=session_calls+1;
    assert(app_media_runtime_content_read(&identity,&query_metadata.object_id,8,44,bytes,sizeof(bytes),&actual)==AE_DENIED && !actual);
    for (size_t i=0;i<sizeof(bytes);++i) assert(!bytes[i]);
    session_fail_after=0; ae_record_metadata_t metadata;
    assert(app_media_runtime_record_metadata(&identity,&query_metadata.object_id,&metadata)==AE_AGAIN);
    media_step(media); query_ack(); query_unix_seconds=999;
    assert(app_media_runtime_record_metadata(&identity,&query_metadata.object_id,&metadata)==AE_TIME_UNKNOWN);
    assert(ae_id_empty(&metadata.object_id));
    query_unix_seconds=query_metadata.expires_unix_seconds;
    assert(app_media_runtime_record_metadata(&identity,&query_metadata.object_id,&metadata)==AE_DENIED);
    assert(ae_id_empty(&metadata.object_id));
    puts("PASS: remote immutable authority, separate result ownership, exact cancellation, async download revision checks, expiry and publication-time session revocation");
}

static void channel_lifetime_authority(void)
{
    remote_setup(); cp_identity_t identity=remote_identity(); identity.scopes|=CP_SCOPE_AUDIO_CAPTURE;
    app_remote_media_request_t request={.token=3000,.command_key=3000,.deadline_ms=now+2000,.identity=identity,
        .operation=CP_OP_AUDIO_CAPTURE_START,.action=APP_REMOTE_MEDIA_START,.audio_generation=audio.audio_generation,
        .resource_revision=audio.revision,.input_epoch=10,.ui_revision=20,
        .start={.request_id={{71}},.principal_id={{9}},.owner_session_id={{10}},.consent_id={{12}},.consent_revision=4,
            .expected_audio_generation=audio.audio_generation,.expected_revision=audio.revision,.kind=AE_CAPTURE_LIVE,
            .route=AE_ROUTE_LOCAL,.format={16000,1,10,AE_PCM_S16_LE},.duration_seconds=5,.byte_limit=65536,.activate_microphone=true}};
    channel_generation=0; assert(app_media_runtime_submit_remote(&request)==AE_UNAVAILABLE && !commands);
    channel_generation=17; assert(app_media_runtime_submit_remote(&request)==AE_OK);
    channel_generation=18; network_port.channel_lost(network_port.user,&identity,17); media_step(media);
    app_audio_result_t result;
    assert(!commands && app_media_runtime_poll_remote(3000,&result) && result.result==AE_DENIED);
    app_media_runtime_release_remote_result(3000);
    request.token=request.command_key=3001;
    assert(app_media_runtime_submit_remote(&request)==AE_OK);
    network_port.channel_lost(network_port.user,&identity,17); media_step(media);
    assert(submitted.kind==APP_AUDIO_CMD_START); acknowledge(AE_OK);
    assert(app_media_runtime_poll_remote(3001,&result) && result.result==AE_OK);
    app_media_runtime_release_remote_result(3001);
    ae_authority_t permission;
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && permission.allowed && permission.owner_session_alive);
    cp_identity_t other=identity; other.session_id[0]=11;
    network_port.channel_lost(network_port.user,&other,18);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && permission.allowed);
    channel_generation=19; /* Cache revalidation closes admission even if notification was missed. */
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && !permission.owner_session_alive);
    network_port.channel_lost(network_port.user,&identity,18);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && !permission.allowed);
    assert(!safety_stops);
    const ae_route_t independent[]={AE_ROUTE_USB_UAC,AE_ROUTE_HFP_HF};
    for (unsigned i=0;i<sizeof(independent)/sizeof(independent[0]);++i) {
        remote_setup(); channel_generation=0; audio.route=independent[i]; media_step(media);
        request.start.route=independent[i]; request.start.route_token=route_token; request.start.format.sample_rate_hz=48000;
        request.audio_generation=request.start.expected_audio_generation=audio.audio_generation;
        request.resource_revision=request.start.expected_revision=audio.revision;
        assert(app_media_runtime_submit_remote(&request)==AE_OK); media_step(media); acknowledge(AE_OK);
        network_port.channel_lost(network_port.user,&identity,19);
        assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&request.start,now,&permission)==AE_OK && permission.allowed && permission.owner_session_alive);
        assert(!media->remote_cancelled && !safety_stops);
    }
    puts("PASS: pinned WSS admission, close-before-START cancellation, stale close rejection and independent USB/HFP authority");
}
int main(void)
{
    setup(); assert(!commands && !safety_stops && !audio.capture_gate_open);
    assert(captured.route.bind_playback && captured.route.bind_playback(captured.route.context,
        AE_ROUTE_USB_UAC,23,&route_token,29,true)==AE_AGAIN);
    assert(captured.route.bind_playback(captured.route.context,AE_ROUTE_USB_UAC,23,&route_token,29,false)==AE_OK);
    assert(playback_bindings==2);
    assert(captured.route.bind_playback(captured.route.context,AE_ROUTE_HFP_HF,23,&route_token,29,true)==AE_NOT_SUPPORTED);
    app_media_local_request_t req=request(AE_CAPTURE_LIVE,AE_ROUTE_USB_UAC);
    admit_start(req);
    ae_authority_t permission;
    ae_start_request_t original=submitted.start;
    assert(captured.authority(captured.policy_context,AE_AUTH_START,&original,now,&permission)==AE_OK && permission.allowed);
    ae_start_request_t widened=original; ++widened.duration_seconds;
    assert(captured.authority(captured.policy_context,AE_AUTH_START,&widened,now,&permission)==AE_OK && !permission.allowed);
    acknowledge(AE_OK);
    uint64_t token; ae_result_t result;
    assert(app_media_runtime_take_result(&token,&result) && token==1 && result==AE_OK);
    assert(!app_media_runtime_take_result(&token,&result));
    assert(app_media_runtime_submit_local(&req)==AE_STALE);
    ae_environment_t environment={.unlocked=true,.screen_visible=true};
    app_media_runtime_environment(&environment,11,21,false);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&original,now,&permission)==AE_OK && permission.allowed);
    environment.unlocked=false; app_media_runtime_environment(&environment,12,22,false);
    assert(captured.authority(captured.policy_context,AE_AUTH_RUNNING,&original,now,&permission)==AE_OK && !permission.allowed);

    setup(); req=request(AE_CAPTURE_LIVE,AE_ROUTE_USB_UAC);
    assert(app_media_runtime_submit_local(&req)==AE_OK);
    app_media_runtime_stop(AE_STOP_PRIVACY); media_step(media);
    assert(!commands && safety_stops==1);
    assert(app_media_runtime_take_result(&token,&result) && result==AE_PRIVACY);

    setup(); admit_start(request(AE_CAPTURE_LIVE,AE_ROUTE_USB_UAC));
    app_media_runtime_stop(AE_STOP_PRIVACY); acknowledge(AE_OK);
    assert(app_media_runtime_take_result(&token,&result) && result==AE_PRIVACY);
    assert(!completion_ready && !media->grants[0].valid);

    setup(); admit_start(request(AE_CAPTURE_RECOGNITION,AE_ROUTE_LOCAL)); acknowledge(AE_OK);
    assert(speech_starts==1 && media->speech_bound);
    speech.state=SL_CANDIDATE; speech.candidate='A'; media_step(media);
    assert(!safety_stops && !speech_cancels && speech.state==SL_CANDIDATE && !media->grants[AE_CAPTURE_RECOGNITION].valid);
    media_step(media); assert(!safety_stops);
    char candidate=0; assert(app_media_runtime_confirm(1,10,20,&candidate)==AE_OK && candidate=='A');
    assert(app_media_runtime_confirm(1,10,20,&candidate)==AE_DENIED && !media->speech_bound);
    environment=(ae_environment_t){.unlocked=true,.screen_visible=true};
    app_media_runtime_environment(&environment,10,21,true);
    assert(!speech_cancels && !safety_stops && app_media_runtime_confirm(1,10,21,&candidate)==AE_DENIED);
    /* A context change while the start result is in flight revokes only the
     * pending recognition grant and cannot publish an old speech binding. */
    setup(); admit_start(request(AE_CAPTURE_RECOGNITION, AE_ROUTE_LOCAL));
    original = submitted.start;
    environment = (ae_environment_t){.unlocked = true, .screen_visible = true};
    app_media_runtime_environment(&environment, 11, 21, false);
    acknowledge(AE_OK);
    assert(!speech_starts && !media->speech_bound && !safety_stops && !completion_ready);
    assert(app_media_runtime_take_result(&token, &result) && result == AE_PRIVACY);
    assert(captured.authority(captured.policy_context, AE_AUTH_RUNNING, &original, now, &permission) == AE_OK && !permission.allowed);

    /* An unconfirmed terminal candidate also cannot stop unrelated playback. */
    setup(); admit_start(request(AE_CAPTURE_RECOGNITION, AE_ROUTE_LOCAL)); acknowledge(AE_OK);
    speech.state = SL_CANDIDATE; speech.candidate = 'B'; media_step(media);
    audio.playback = (ae_job_snapshot_t){.state = AE_JOB_RUNNING, .job_id = {{99}}, .consumer_generation = 42};
    app_media_runtime_environment(&environment, 11, 21, false);
    assert(speech_cancels == 1 && !safety_stops && audio.playback.state == AE_JOB_RUNNING);
    assert(!media->speech_bound && !media->grants[AE_CAPTURE_RECOGNITION].valid);
    setup();
    req = (app_media_local_request_t){.token = 1, .epoch = 10, .ui_revision = 20,
        .action = APP_MEDIA_LINK_CONTROL, .route = AE_ROUTE_HFP_HF, .value = true,
        .bluetooth_generation = 7, .bluetooth_bond_revision = 9, .bluetooth_peer = {1,2,3,4,5,6}};
    assert(app_media_runtime_submit_local(&req) == AE_OK);
    media_step(media); assert(bluetooth_requests == 1 && !commands);
    assert(!app_media_runtime_take_result(&token, &result));
    for (unsigned i = 0; i < 10; ++i) media_step(media);
    assert(bluetooth_requests == 1); /* Pending poll never resubmits a connection. */
    bluetooth.request_status = BT_AUDIO_JOB_SUCCEEDED;
    media_step(media);
    assert(app_media_runtime_take_result(&token, &result) && result == AE_OK);
    req.token = 2; assert(app_media_runtime_submit_local(&req) == AE_OK);
    media_step(media); now += 15000; media_step(media);
    assert(app_media_runtime_take_result(&token, &result) && result == AE_TIMEOUT && bluetooth_cancels == 1);
    setup(); req.bluetooth_generation = 6;
    assert(app_media_runtime_submit_local(&req) == AE_OK); media_step(media);
    assert(app_media_runtime_take_result(&token, &result) && result == AE_BUSY && !bluetooth_requests);
    setup(); audio.route = AE_ROUTE_USB_UAC;
    audio.playback = (ae_job_snapshot_t){.state = AE_JOB_RUNNING, .kind = AE_PLAYBACK_STREAM};
    transport_frames = 9; ingress_capacity = 4;
    media_step(media); assert(transport_frames == 5 && transport_reads == 4 && ingress_frames == 4);
    media_step(media); assert(transport_reads == 4); /* Full ring must not pop/drop a transport frame. */
    ingress_capacity = 1; media_step(media);
    assert(transport_frames == 4 && ingress_frames == 5 && transport_reads == 5);
    ingress_capacity = 4; media_step(media);
    assert(!transport_frames && ingress_frames == 9 && transport_reads == 9);
    remote_and_queries(); channel_lifetime_authority();
    assert(!lock_depth);
    free(media); media=NULL;
    puts("PASS: media runtime local grants, route/start ordering, cancellation, independent HID context and speech preview");
    return 0;
}

ae_result_t app_audio_copy_record_list(uint64_t token, ae_record_metadata_t *items, size_t capacity)
{ (void)token; if (!query_copy_allowed || !capacity) return AE_NOT_FOUND; items[0]=query_metadata; return AE_OK; }
ae_result_t app_audio_copy_record_data(uint64_t token, size_t offset, void *data, size_t bytes)
{ (void)token; (void)offset; if (!query_copy_allowed) return AE_NOT_FOUND; memset(data,0x5a,bytes); return AE_OK; }
