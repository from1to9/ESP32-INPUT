// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../audio_engine/heap_caps_model.h"
#include "../../firmware/main/app_media_runtime.c"

#ifndef TEST_EXPECT_EXTERNAL_STACK
#define TEST_EXPECT_EXTERNAL_STACK 0
#endif

/* Actual runtime + actual threaded app_audio + actual engine. Only the codec,
 * RTOS, speech/Bluetooth SDK and copied HTTP queue are boundary models. */
struct test_task {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned notifications;
    TaskFunction_t fn;
    void *argument;
};
static struct test_task audio_task;
static atomic_bool hold_audio, audio_held, session_active, grant_active;
static atomic_uint writes, ids;
static atomic_uint_fast64_t channel_generation=17;
static bool microphone, speaker, read_pending;
static uint64_t codec_generation, next_capture;
static ae_format_t pcm={16000,1,10,AE_PCM_S16_LE};
static app_control_media_port_t network;
static const cp_identity_t owner={.principal_id={9},.session_id={10},.scopes=CP_SCOPE_ALL};
typedef struct {
    bool used, cancelled;
    cp_identity_t identity;
    uint8_t job[CP_ID_BYTES], packet[CP_MEDIA_FRAME_BYTES];
    uint64_t consumer, channel;
    size_t bytes;
} wire_packet_t;
static wire_packet_t packets[2];
static pthread_mutex_t wire_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned sent;
static atomic_uint capture_cancelled;
static uint64_t last_sequence, last_offset;

static int64_t micros(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
int64_t esp_timer_get_time(void) { return micros(); }
static void pause_ms(unsigned ms)
{ struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
void *heap_caps_calloc(size_t n, size_t bytes, unsigned caps)
{ assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return test_heap_allocate(4, n, bytes); }
void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t bytes, unsigned caps)
{ assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  assert(alignment >= _Alignof(uint64_t)); return test_heap_allocate(alignment, n, bytes); }
void heap_caps_free(void *data) { test_heap_free(data); }
void esp_fill_random(void *data,size_t bytes)
{ memset(data,0,bytes); unsigned id=atomic_fetch_add(&ids,1)+1U; memcpy(data,&id,bytes<sizeof(id)?bytes:sizeof(id)); }
static void *audio_main(void *context)
{ struct test_task *task=context; task->fn(task->argument); return NULL; }
BaseType_t xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,void *argument,unsigned priority,TaskHandle_t *out)
{
    if (!strcmp(name,"media_control")) {
        assert(!TEST_EXPECT_EXTERNAL_STACK && fn==worker && stack==6144 && priority==3 && !out); return pdPASS;
    }
    assert(stack==8192 && priority==4); audio_task.fn=fn; audio_task.argument=argument;
    assert(!pthread_mutex_init(&audio_task.lock,NULL)); assert(!pthread_cond_init(&audio_task.changed,NULL));
    assert(!pthread_create(&audio_task.thread,NULL,audio_main,&audio_task));
    if (out) *out=&audio_task;
    return pdPASS;
}
BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *out, unsigned caps)
{
    assert(TEST_EXPECT_EXTERNAL_STACK && !strcmp(name, "media_control"));
    assert(fn == worker && stack == 6144 && priority == 3 && argument && !out);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &audio_task; }
void xTaskNotifyGive(TaskHandle_t task)
{ pthread_mutex_lock(&task->lock); ++task->notifications; pthread_cond_signal(&task->changed); pthread_mutex_unlock(&task->lock); }
unsigned ulTaskNotifyTake(BaseType_t clear,TickType_t ticks)
{
    (void)clear;
    while (atomic_load(&hold_audio)) { atomic_store(&audio_held,true); pause_ms(1); }
    atomic_store(&audio_held,false);
    struct timespec until; clock_gettime(CLOCK_REALTIME,&until);
    until.tv_nsec+=(long)ticks*1000000; until.tv_sec+=until.tv_nsec/1000000000; until.tv_nsec%=1000000000;
    pthread_mutex_lock(&audio_task.lock);
    if (!audio_task.notifications) (void)pthread_cond_timedwait(&audio_task.changed,&audio_task.lock,&until);
    unsigned count=audio_task.notifications; audio_task.notifications=0; pthread_mutex_unlock(&audio_task.lock); return count;
}
void vTaskDelay(TickType_t ticks) { pause_ms(ticks); }
void vTaskDelete(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
static ae_result_t codec_caps(void *context,ae_codec_capabilities_t *out)
{
    (void)context; *out=(ae_codec_capabilities_t){.implemented=true,.available=true,.full_duplex=true,
        .safe_volume_max=80,.format_count=1,.formats={{.format={16000,1,10,AE_PCM_S16_LE},.roles=3}}}; return AE_OK;
}
static ae_result_t codec_prepare(void *context,const ae_format_t *format,uint8_t roles,uint64_t generation)
{ (void)context; assert(ae_format_equal(format,&pcm) && roles==3); codec_generation=generation; return AE_OK; }
static ae_result_t codec_mute(void *context,ae_role_t role,bool muted,uint64_t generation)
{ (void)context; assert(generation==codec_generation); if (role&AE_MICROPHONE) microphone=!muted; if (role&AE_SPEAKER) speaker=!muted; return AE_OK; }
static ae_result_t codec_stop(void *context,uint64_t generation)
{ (void)context; (void)generation; microphone=speaker=read_pending=false; return AE_OK; }
static ae_result_t codec_level(void *context,uint8_t level,uint64_t generation)
{ (void)context; (void)level; assert(generation==codec_generation); return AE_OK; }
static ae_result_t codec_read(void *context,uint64_t generation,void *data,size_t bytes,size_t *actual)
{
    (void)context; assert(generation==codec_generation && bytes==320); *actual=0;
    if (!microphone) { assert(read_pending); read_pending=false; return AE_STALE; }
    uint64_t now=clock_ms(NULL); if (now<next_capture) { read_pending=true; return AE_AGAIN; }
    read_pending=false; next_capture=now+10; memset(data,0x5a,bytes); *actual=bytes; return AE_OK;
}
static ae_result_t codec_write(void *context,uint64_t generation,const void *data,size_t bytes,size_t *actual)
{
    (void)context; assert(speaker && generation==codec_generation && bytes==320);
    const uint8_t *pcm_bytes=data; for (size_t i=0;i<bytes;++i) assert(pcm_bytes[i]==0x6b);
    *actual=bytes; atomic_fetch_add(&writes,1); return AE_OK;
}
ae_result_t board_audio_mosaico_init(void) { return AE_OK; }
ae_codec_port_t board_audio_mosaico_port(void)
{ return (ae_codec_port_t){.capabilities=codec_caps,.prepare=codec_prepare,.mute=codec_mute,.stop=codec_stop,
    .volume=codec_level,.gain=codec_level,.read=codec_read,.write=codec_write}; }
board_audio_status_t board_audio_mosaico_status(void)
{ return (board_audio_status_t){.hardware_observed=true,.last_result=AE_OK}; }
esp_err_t bt_audio_init(void) { return ESP_OK; }
void bt_audio_service(void) { }
bt_audio_snapshot_t bt_audio_snapshot(void) { return (bt_audio_snapshot_t){0}; }
void bt_audio_copy_bond_list(bt_audio_bond_list_t *out) { assert(out); memset(out,0,sizeof(*out)); }
void bt_audio_cancel_request(uint64_t id) { (void)id; }
esp_err_t bt_audio_request(uint64_t generation,uint64_t link_generation,bt_audio_command_t command,const uint8_t peer[6],uint64_t *id)
{ (void)generation; (void)link_generation; (void)command; (void)peer; (void)id; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bt_audio_saved_request(uint64_t generation,uint64_t link_generation,uint64_t revision,bt_audio_command_t command,const uint8_t peer[6],uint64_t *id)
{ (void)revision; return bt_audio_request(generation,link_generation,command,peer,id); }
ae_route_port_t usb_audio_route_port(void) { return (ae_route_port_t){0}; }
ae_route_port_t bt_audio_route_port(void) { return (ae_route_port_t){0}; }
ae_result_t usb_audio_receive(ae_frame_t *frame) { (void)frame; return AE_AGAIN; }
ae_result_t bt_audio_receive(ae_frame_t *frame) { (void)frame; return AE_AGAIN; }
ae_result_t app_spoken_letters_init(void) { return AE_OK; }
ae_result_t app_spoken_letters_begin(const sl_binding_t *binding) { (void)binding; return AE_NOT_SUPPORTED; }
ae_result_t app_spoken_letters_capture(const ae_id_t *job,const ae_frame_t *frame) { (void)job; (void)frame; return AE_NOT_SUPPORTED; }
void app_spoken_letters_cancel(const ae_id_t *job,uint64_t generation) { (void)job; (void)generation; }
void app_spoken_letters_capture_closed(const ae_id_t *job,uint64_t generation) { (void)job; (void)generation; }
bool app_spoken_letters_snapshot(sl_view_t *out) { *out=(sl_view_t){.state=SL_IDLE}; return true; }
ae_result_t app_spoken_letters_confirm(uint64_t token,uint64_t epoch,uint64_t revision,char *candidate)
{ (void)token; (void)epoch; (void)revision; (void)candidate; return AE_NOT_SUPPORTED; }
void app_control_server_set_media_port(const app_control_media_port_t *port) { network=*port; }
uint64_t app_control_server_channel_generation(const cp_identity_t *identity)
{ return !memcmp(identity->principal_id,owner.principal_id,16) && !memcmp(identity->session_id,owner.session_id,16) ? atomic_load(&channel_generation) : 0; }
esp_err_t app_control_server_capture_send(const cp_identity_t *identity,const uint8_t job[16],uint64_t consumer,uint64_t channel,const uint8_t *packet,size_t bytes)
{
    if (!channel || channel!=app_control_server_channel_generation(identity)) return ESP_ERR_INVALID_STATE;
    assert(bytes<=CP_MEDIA_FRAME_BYTES); esp_err_t result=ESP_ERR_NO_MEM;
    pthread_mutex_lock(&wire_lock);
    for (unsigned i=0;i<2;++i) if (!packets[i].used) {
        packets[i]=(wire_packet_t){.used=true,.identity=*identity,.consumer=consumer,.channel=channel,.bytes=bytes};
        memcpy(packets[i].job,job,16); memcpy(packets[i].packet,packet,bytes); result=ESP_OK; break;
    }
    pthread_mutex_unlock(&wire_lock); return result;
}
void app_control_server_capture_cancel(const cp_identity_t *identity,const uint8_t job[16],uint64_t consumer,uint64_t channel)
{
    pthread_mutex_lock(&wire_lock); ++capture_cancelled;
    for (unsigned i=0;i<2;++i) if (packets[i].used && packets[i].consumer==consumer && packets[i].channel==channel &&
        !memcmp(packets[i].job,job,16) && !memcmp(packets[i].identity.session_id,identity->session_id,16)) packets[i].cancelled=true;
    pthread_mutex_unlock(&wire_lock);
}
static void wire_drain(bool fail)
{
    /* The HTTP owner drains in FIFO sequence and rechecks immediately before
     * sending, like the actual server. No caller retains borrowed PCM. */
    for (;;) {
        wire_packet_t packet={0}; pthread_mutex_lock(&wire_lock);
        unsigned selected=2; uint64_t lowest=UINT64_MAX;
        for (unsigned i=0;i<2;++i) if (packets[i].used) {
            cp_media_header_t header; assert(cp_media_decode(packets[i].packet,packets[i].bytes,&header));
            if (header.sequence<lowest) { selected=i; lowest=header.sequence; }
        }
        if (selected<2) { packet=packets[selected]; memset(&packets[selected],0,sizeof(packets[selected])); }
        pthread_mutex_unlock(&wire_lock); if (!packet.used) break;
        bool allowed=!packet.cancelled && network.capture_allowed(network.user,&packet.identity,packet.job,packet.consumer,packet.channel);
        if (fail || !allowed) network.capture_failed(network.user,&packet.identity,packet.job,packet.consumer,packet.channel,CP_UNAVAILABLE);
        else {
            cp_media_header_t header; assert(cp_media_decode(packet.packet,packet.bytes,&header));
            assert(header.kind==CP_MEDIA_CAPTURE && header.sequence==last_sequence+1 && header.sample_offset==last_offset);
            for (size_t i=CP_MEDIA_HEADER_BYTES;i<packet.bytes;++i) assert(packet.packet[i]==0x5a);
            ++sent; last_sequence=header.sequence; last_offset+=160;
        }
        ae_secure_zero(&packet,sizeof(packet));
    }
}
static bool session_live(void *context,const cp_identity_t *identity)
{ (void)context; return atomic_load(&session_active) && !memcmp(identity->principal_id,owner.principal_id,16) && !memcmp(identity->session_id,owner.session_id,16); }
static cp_error_t grant_authority(void *context,const cp_capture_authority_request_t *request,cp_capture_authority_t *out)
{
    (void)context; assert(request->purpose==CP_CAPTURE_LIVE && request->live_destination && !request->file_destination && request->grant_revision==1);
    *out=(cp_capture_authority_t){.allowed=atomic_load(&grant_active),.owner_session_alive=session_live(NULL,&request->identity),
        .source_valid_until_ms=clock_ms(NULL)+60000}; return CP_OK;
}
static void tick(unsigned ms,bool drain)
{
    for (unsigned i=0;i<ms;++i) {
        ae_snapshot_t audio;
        if (app_audio_snapshot(&audio)) {
            /* Explicit renderer boundary: the indication was drawn for this
             * observed generation. The production privacy guard stays active. */
            ae_environment_t environment={.unlocked=true,.screen_visible=true,.indicator_visible=true,
                .indicator_generation=audio.audio_generation,.indicator_seen_ms=clock_ms(NULL)};
            app_media_runtime_environment(&environment,10,20,false);
        }
        media_step(media); if (drain) wire_drain(false); pause_ms(1);
    }
}
static ae_snapshot_t snapshot(void) { ae_snapshot_t out; assert(app_audio_snapshot(&out)); return out; }
static app_remote_media_request_t start_request(ae_job_kind_t kind,uint64_t token)
{
    ae_snapshot_t audio=snapshot(); app_remote_media_request_t request={.token=token,.command_key=token,.deadline_ms=clock_ms(NULL)+2000,
        .identity=owner,.operation=kind==AE_CAPTURE_LIVE?CP_OP_AUDIO_CAPTURE_START:CP_OP_AUDIO_PLAYBACK_START,
        .action=APP_REMOTE_MEDIA_START,.audio_generation=audio.audio_generation,.resource_revision=audio.revision,
        .start={.kind=kind,.route=AE_ROUTE_LOCAL,.format={16000,1,10,AE_PCM_S16_LE},.duration_seconds=20,.byte_limit=1048576,
            .principal_id={{9}},.owner_session_id={{10}},.consent_id={{12}},.consent_revision=1,
            .expected_audio_generation=audio.audio_generation,.expected_revision=audio.revision,.activate_microphone=kind==AE_CAPTURE_LIVE}};
    request.start.request_id.bytes[0]=(uint8_t)token; return request;
}
static app_audio_result_t start(app_remote_media_request_t request)
{
    assert(app_media_runtime_submit_remote(&request)==AE_OK); app_audio_result_t result={0};
    for (unsigned i=0;i<500;++i) {
        tick(1,true);
        if (app_media_runtime_poll_remote(request.token,&result)) break;
    }
    assert(result.request_token==request.token && result.result==AE_OK && !ae_id_empty(&result.job.job_id));
    app_media_runtime_release_remote_result(request.token); return result;
}
static size_t playback_packet(const app_audio_result_t *start,uint64_t sequence,uint8_t *packet)
{
    ae_snapshot_t audio=snapshot(); uint8_t pcm_bytes[320]; memset(pcm_bytes,0x6b,sizeof(pcm_bytes));
    cp_media_header_t header={.kind=CP_MEDIA_PLAYBACK,.audio_generation=audio.audio_generation,
        .consumer_generation=start->job.consumer_generation,.sequence=sequence,.sample_offset=(sequence-1)*160,
        .sample_rate_hz=16000,.channels=1,.frame_ms=10,.bytes=320};
    memcpy(header.job_id,start->job.job_id.bytes,16); size_t bytes;
    assert(cp_media_encode(&header,pcm_bytes,packet,CP_MEDIA_FRAME_BYTES,&bytes)); return bytes;
}
int main(void)
{
    atomic_store(&session_active,true); atomic_store(&grant_active,true);
    app_media_services_t services={0}; assert(app_media_runtime_init(&services)==AE_OK);
    assert(network.user==media && network.capture_allowed && network.capture_failed && network.playback && network.channel_lost);
    ae_environment_t environment={.unlocked=true,.screen_visible=true,.indicator_visible=true};
    app_media_runtime_environment(&environment,10,20,false); tick(20,true);
    app_media_remote_authority_t authority={.session_alive=session_live,.capture_authority=grant_authority};
    assert(app_media_runtime_set_remote_authority(&authority)==AE_OK);
    app_media_local_request_t route={.token=1,.epoch=10,.ui_revision=20,.action=APP_MEDIA_ROUTE,.route=AE_ROUTE_LOCAL};
    assert(app_media_runtime_submit_local(&route)==AE_OK); uint64_t token=0; ae_result_t result=AE_AGAIN;
    for (unsigned i=0;i<100;++i) { tick(1,true); if (app_media_runtime_take_result(&token,&result)) break; }
    assert(token==1 && result==AE_OK);
    app_audio_result_t capture=start(start_request(AE_CAPTURE_LIVE,100));
    for (unsigned i=0;i<300 && sent<2;++i) tick(1,true);
    assert(sent>=2 && snapshot().capture[AE_CAPTURE_LIVE].state==AE_JOB_RUNNING);
    tick(35,false); wire_drain(false); tick(20,true); /* Full HTTP queue retries exact samples in order. */
    assert(snapshot().capture[AE_CAPTURE_LIVE].state==AE_JOB_RUNNING);
    app_audio_result_t playback=start(start_request(AE_PLAYBACK_STREAM,101));
    uint8_t packet[CP_MEDIA_FRAME_BYTES]; size_t bytes=playback_packet(&playback,1,packet);
    cp_identity_t stranger=owner; stranger.session_id[0]=11;
    assert(network.playback(network.user,&stranger,atomic_load(&channel_generation),packet,bytes)==CP_FORBIDDEN);
    assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes-1)==CP_INVALID);
    atomic_store(&hold_audio,true);
    for (unsigned i=0;i<100 && !atomic_load(&audio_held);++i) pause_ms(1);
    assert(atomic_load(&audio_held));
    for (uint64_t i=1;i<=4;++i) { bytes=playback_packet(&playback,i,packet); assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes)==CP_OK); }
    bytes=playback_packet(&playback,5,packet); assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes)==CP_BUSY);
    atomic_store(&hold_audio,false); tick(20,true);
    assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes)==CP_OK);
    for (unsigned i=0;i<100 && atomic_load(&writes)<5;++i) tick(1,true);
    assert(atomic_load(&writes)==5 && snapshot().capture[AE_CAPTURE_LIVE].state==AE_JOB_RUNNING);
    assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes)==CP_STALE); /* Duplicate cannot play twice. */
    tick(10,false); wire_drain(true); tick(10,true);
    assert(snapshot().capture[AE_CAPTURE_LIVE].state!=AE_JOB_RUNNING && snapshot().playback.state==AE_JOB_RUNNING);
    assert(capture_cancelled && !network.capture_allowed(network.user,&owner,capture.job.job_id.bytes,capture.job.consumer_generation,atomic_load(&channel_generation)));
    bytes=playback_packet(&playback,6,packet); assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes)==CP_OK); tick(5,true);
    last_sequence=last_offset=0;
    unsigned old_sent=sent;
    app_audio_result_t replacement=start(start_request(AE_CAPTURE_LIVE,102));
    network.capture_failed(network.user,&owner,capture.job.job_id.bytes,capture.job.consumer_generation,atomic_load(&channel_generation),CP_UNAVAILABLE);
    assert(network.capture_allowed(network.user,&owner,replacement.job.job_id.bytes,replacement.job.consumer_generation,atomic_load(&channel_generation)));
    for (unsigned i=0;i<100 && sent==old_sent;++i) tick(1,true);
    assert(sent>old_sent && snapshot().playback.state==AE_JOB_RUNNING);
    tick(15,false); old_sent=sent; atomic_store(&grant_active,false);
    wire_drain(false); tick(10,true);
    assert(sent==old_sent && snapshot().capture[AE_CAPTURE_LIVE].state!=AE_JOB_RUNNING && snapshot().playback.state==AE_JOB_RUNNING);
    atomic_store(&session_active,false);
    bytes=playback_packet(&playback,7,packet); assert(network.playback(network.user,&owner,atomic_load(&channel_generation),packet,bytes)==CP_FORBIDDEN);
    tick(20,true); assert(snapshot().playback.state!=AE_JOB_RUNNING);
    atomic_store(&session_active,true); atomic_store(&grant_active,true);
    unsigned before_close=sent;
    app_remote_media_request_t pending_start=start_request(AE_CAPTURE_LIVE,103);
    assert(app_media_runtime_submit_remote(&pending_start)==AE_OK);
    atomic_store(&channel_generation,0); network.channel_lost(network.user,&owner,17);
    tick(10,true); app_audio_result_t rejected;
    assert(app_media_runtime_poll_remote(103,&rejected) && rejected.result==AE_DENIED && sent==before_close);
    app_media_runtime_release_remote_result(103);
    atomic_store(&channel_generation,18); last_sequence=last_offset=0;
    app_audio_result_t closing_capture=start(start_request(AE_CAPTURE_LIVE,104));
    app_audio_result_t closing_playback=start(start_request(AE_PLAYBACK_STREAM,105));
    uint8_t old_packet[CP_MEDIA_FRAME_BYTES]; size_t old_bytes=playback_packet(&closing_playback,1,old_packet);
    atomic_store(&hold_audio,true);
    for (unsigned i=0;i<100 && !atomic_load(&audio_held);++i) pause_ms(1);
    assert(atomic_load(&audio_held)); wire_drain(false);
    pthread_mutex_lock(&wire_lock); assert(!packets[0].used && !packets[1].used); pthread_mutex_unlock(&wire_lock);
    assert(!snapshot().playback.first_frame_seen); /* No PCM receipt can trigger this close cleanup. */
    atomic_store(&channel_generation,0); network.channel_lost(network.user,&owner,18);
    atomic_store(&channel_generation,19); /* Same logical session immediately opens a replacement. */
    assert(!network.capture_allowed(network.user,&owner,closing_capture.job.job_id.bytes,closing_capture.job.consumer_generation,18));
    assert(network.playback(network.user,&owner,18,old_packet,old_bytes)==CP_FORBIDDEN);
    atomic_store(&hold_audio,false); tick(30,true);
    ae_snapshot_t closed=snapshot(); assert(!active_job(&closed.playback));
    last_sequence=last_offset=0;
    app_audio_result_t fresh_capture=start(start_request(AE_CAPTURE_LIVE,106));
    app_audio_result_t fresh_playback=start(start_request(AE_PLAYBACK_STREAM,107));
    network.channel_lost(network.user,&owner,18); /* Delayed old close cannot reach generation 19. */
    network.capture_failed(network.user,&owner,closing_capture.job.job_id.bytes,closing_capture.job.consumer_generation,18,CP_UNAVAILABLE);
    cp_identity_t unrelated=owner; unrelated.principal_id[0]=8;
    network.channel_lost(network.user,&unrelated,19);
    assert(network.capture_allowed(network.user,&owner,fresh_capture.job.job_id.bytes,fresh_capture.job.consumer_generation,19));
    bytes=playback_packet(&fresh_playback,1,packet);
    assert(network.playback(network.user,&owner,18,packet,bytes)==CP_FORBIDDEN);
    assert(network.playback(network.user,&owner,19,old_packet,old_bytes)==CP_FORBIDDEN);
    assert(network.playback(network.user,&owner,19,packet,bytes)==CP_OK); tick(10,true);
    atomic_store(&channel_generation,0); network.channel_lost(network.user,&owner,19); tick(20,true);
    assert(atomic_load(&session_active)); /* Logical HTTP authority survives WSS loss. */
    assert(snapshot().capture[AE_CAPTURE_LIVE].state!=AE_JOB_RUNNING && snapshot().playback.state!=AE_JOB_RUNNING);
    assert(app_audio_shutdown()==AE_AGAIN); assert(!pthread_join(audio_task.thread,NULL));
    assert(app_audio_shutdown()==AE_OK);
    printf("Resource bytes: media runtime=%zu, network scratch=%zu\n",sizeof(*media),sizeof(media->network_capture)+sizeof(media->network_playback));
    ae_secure_zero(packets,sizeof(packets)); heap_caps_free(media); media=NULL;
    puts("PASS: actual runtime/audio worker/engine WebSocket PCM composition, bounded retry, exact frames, channel generations, zero-PCM close, pending START and replacement isolation");
    return 0;
}
