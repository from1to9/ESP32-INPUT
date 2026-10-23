// SPDX-License-Identifier: Apache-2.0
/* Compose actual manager admission with the production board capabilities and
 * pinned codec driver. USB selection/authority are explicit boundary inputs;
 * admission is not a USB host, acoustic or device qualification claim. */
#define main board_fixture_main
#include "test_board_audio.c"
#undef main
#include "audio_engine.h"
#include "audio_media.h"

static uint64_t ids;
static uint64_t engine_now = 1000;
static const ae_route_token_t usb_token = {.peer_id={{7}}, .stream_id={{8}}, .link_generation=1};
static ae_result_t policy(void *context, ae_authority_operation_t operation,
    const ae_start_request_t *request, uint64_t now, ae_authority_t *authority)
{
    (void)context; (void)operation; (void)request; (void)now;
    *authority=(ae_authority_t){.allowed=true,.owner_session_alive=true,.source_valid_until_ms=100000,.reason=AE_OK};
    return AE_OK;
}
static ae_result_t random_identity(void *context, ae_id_t *id)
{
    (void)context; memset(id,0,sizeof(*id)); uint64_t value=++ids;
    memcpy(id->bytes,&value,sizeof(value)); return AE_OK;
}
static uint64_t clock_ms(void *context) { (void)context; return engine_now; }
static ae_result_t admission_only_sink(void *context, ae_job_kind_t kind, const ae_id_t *job, const ae_frame_t *frame)
{
    (void)context; (void)kind; (void)job; (void)frame;
    assert(!"Admission-only fixture must not execute capture"); return AE_UNAVAILABLE;
}
static void admission_only_cancel(void *context, ae_job_kind_t kind, const ae_id_t *job, uint64_t generation)
{
    (void)context; (void)kind; (void)job; (void)generation;
    assert(!"Admission-only fixture must not execute capture cancellation");
}
static ae_result_t usb_selection(void *context, ae_route_t route, ae_route_capabilities_t *caps)
{
    (void)context; assert(route==AE_ROUTE_USB_UAC);
    /* This exact tuple is the production usb_audio.h capture/playback contract
     * and is independently checked against the UAC descriptor by its suite. */
    *caps=(ae_route_capabilities_t){.implemented=true,.available=true,.connected=true,
        .capture=true,.playback=true,.full_duplex=true,.format={48000,1,10,AE_PCM_S16_LE},.token=usb_token};
    return AE_OK;
}
static void admit(ae_route_t route, ae_job_kind_t kind, ae_format_t format)
{
    void *memory=calloc(1,ae_engine_bytes()); assert(memory);
    ae_dependencies_t dependencies={.codec=port,.route={.capabilities=usb_selection},
        .authority=policy,.random_id=random_identity,.monotonic_ms=clock_ms,
        .capture_sink=admission_only_sink,.capture_cancel=admission_only_cancel};
    ae_engine_t *engine=NULL;
    assert(ae_init(memory,ae_engine_bytes(),&dependencies,1000,&engine)==AE_AGAIN);
    drain(); assert(ae_poll(engine,1000)==AE_OK);
    ae_snapshot_t view; ae_snapshot(engine,&view);
    assert(ae_route_select(engine,route,view.audio_generation,view.revision,1000)==AE_OK);
    ae_snapshot(engine,&view);
    ae_environment_t environment={.unlocked=true,.screen_visible=true,.indicator_visible=true,
        .indicator_generation=view.audio_generation,.indicator_seen_ms=1000};
    ae_environment(engine,&environment,1000);
    ae_start_request_t request={.request_id={{1}},.principal_id={{2}},.owner_session_id={{3}},.consent_id={{4}},
        .consent_revision=1,.expected_audio_generation=view.audio_generation,.expected_revision=view.revision,
        .kind=kind,.route=route,.format=format,.duration_seconds=1,.byte_limit=65536,.activate_microphone=true};
    if(route==AE_ROUTE_USB_UAC) request.route_token=usb_token;
    ae_job_snapshot_t job; unsigned before=calls;
    ae_result_t admitted=ae_start(engine,&request,1000,&job);
    if(admitted!=AE_OK) fprintf(stderr,"Admission route=%d kind=%d result=%d\n",route,kind,admitted);
    assert(admitted==AE_OK && calls==before);
    assert(job.state==AE_JOB_STARTING); /* No codec/USB execution was requested. */
    free(memory);
}
static unsigned capture_frames;
static ae_result_t execution_prepare(void *context, ae_route_t route, const ae_format_t *format,
    uint8_t roles, uint64_t generation, const ae_route_token_t *token)
{
    (void)context; assert(route == AE_ROUTE_USB_UAC && generation && roles == 3 &&
        same_format(format, &(ae_format_t){48000,1,10,AE_PCM_S16_LE}) && ae_route_token_equal(token, &usb_token));
    return AE_OK;
}
static ae_result_t execution_capture(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context; assert(route == AE_ROUTE_USB_UAC && frame->bytes == 960 && frame->data[0] == 1 &&
        frame->data[1] == 2 && frame->data[2] == 5 && frame->data[3] == 6 && ae_route_token_equal(&frame->route_token, &usb_token));
    ++capture_frames; return AE_OK;
}
static ae_result_t execution_stop(void *context, ae_route_t route, uint64_t generation, const ae_route_token_t *token)
{ (void)context; assert(route == AE_ROUTE_USB_UAC && generation && ae_route_token_equal(token, &usb_token)); return AE_OK; }
static ae_result_t execution_bind(void *context, ae_route_t route, uint64_t generation,
    const ae_route_token_t *token, uint64_t consumer, bool enabled)
{ (void)enabled; assert(consumer); return execution_stop(context, route, generation, token); }
static void execution_cancel(void *context, ae_job_kind_t kind, const ae_id_t *job, uint64_t generation)
{ (void)context; assert(kind == AE_CAPTURE_LIVE && !ae_id_empty(job) && generation); }
static void pump(ae_engine_t *engine, unsigned steps)
{
    for (unsigned i = 0; i < steps; ++i) {
        ae_snapshot_t view; ae_snapshot(engine, &view);
        ae_environment_t environment = {.unlocked = true, .screen_visible = true, .indicator_visible = true,
            .indicator_generation = view.audio_generation, .indicator_seen_ms = engine_now};
        ae_environment(engine, &environment, engine_now);
        (void)ae_poll(engine, engine_now); drain(); ++engine_now;
    }
}
static ae_job_snapshot_t start_execution(ae_engine_t *engine, ae_job_kind_t kind)
{
    ae_snapshot_t view; ae_snapshot(engine, &view);
    ae_start_request_t request = {.principal_id = {{2}}, .owner_session_id = {{3}}, .consent_id = {{4}},
        .consent_revision = 1, .expected_audio_generation = view.audio_generation, .expected_revision = view.revision,
        .kind = kind, .route = AE_ROUTE_USB_UAC, .route_token = usb_token, .format = {48000,1,10,AE_PCM_S16_LE},
        .duration_seconds = 8, .byte_limit = 768000, .activate_microphone = kind == AE_CAPTURE_LIVE};
    assert(random_identity(NULL, &request.request_id) == AE_OK);
    ae_job_snapshot_t job; assert(ae_start(engine, &request, engine_now, &job) == AE_OK); return job;
}
static void send_frame(ae_engine_t *engine, const ae_job_snapshot_t *job)
{
    ae_snapshot_t view; ae_snapshot(engine, &view);
    ae_frame_t frame = {.audio_generation = view.audio_generation, .consumer_generation = job->consumer_generation,
        .sequence = 1, .sample_offset = 0, .route_token = usb_token, .format = view.format, .bytes = 960};
    memset(frame.data, 0x53, frame.bytes);
    assert(ae_submit_playback(engine, &frame, engine_now) == AE_OK);
}
static void stop_execution(ae_engine_t *engine, const ae_job_snapshot_t *job)
{ assert(ae_stop(engine, &job->job_id, job->consumer_generation, AE_STOP_REQUESTED, engine_now) == AE_OK); pump(engine, 30); }
static void execute_independent_roles(void)
{
    void *memory = calloc(1, ae_engine_bytes()); assert(memory);
    ae_dependencies_t dependencies = {.codec = port, .authority = policy, .random_id = random_identity,
        .monotonic_ms = clock_ms, .capture_cancel = execution_cancel,
        .route = {.capabilities = usb_selection, .prepare = execution_prepare, .capture = execution_capture,
            .stop = execution_stop, .bind_playback = execution_bind}};
    ae_engine_t *engine = NULL;
    assert(ae_init(memory, ae_engine_bytes(), &dependencies, engine_now, &engine) == AE_AGAIN); drain(); pump(engine, 5);
    ae_snapshot_t view; ae_snapshot(engine, &view);
    assert(ae_route_select(engine, AE_ROUTE_USB_UAC, view.audio_generation, view.revision, engine_now) == AE_OK);
    ae_job_snapshot_t speaker = start_execution(engine, AE_PLAYBACK_STREAM); send_frame(engine, &speaker); pump(engine, 30);
    ae_snapshot(engine, &view);
    assert(view.playback.state == AE_JOB_RUNNING && view.playback.first_frame_seen && view.playback.delivered_bytes == 960);
    assert(view.capture[AE_CAPTURE_LIVE].state == AE_JOB_EMPTY && !s.rx_running && !s.capture_gate && pa_level == 1);
    ae_job_snapshot_t mic = start_execution(engine, AE_CAPTURE_LIVE); pump(engine, 20); ae_snapshot(engine, &view);
    assert(view.capture[AE_CAPTURE_LIVE].first_frame_seen && view.playback.state == AE_JOB_RUNNING && s.rx_running);
    stop_execution(engine, &mic); ae_snapshot(engine, &view);
    assert(view.playback.state == AE_JOB_RUNNING && s.tx_running && !s.rx_running && pa_level == 1);
    stop_execution(engine, &speaker); ae_snapshot(engine, &view);
    assert(view.hardware_off_confirmed && !s.tx_running && !s.rx_running);
    mic = start_execution(engine, AE_CAPTURE_LIVE); pump(engine, 40); ae_snapshot(engine, &view);
    assert(view.capture[AE_CAPTURE_LIVE].state == AE_JOB_RUNNING && view.capture[AE_CAPTURE_LIVE].first_frame_seen && capture_frames);
    assert(s.tx_running && s.rx_running && !s.playback_gate && !pa_level && (registers[0x31] & 0x60U) == 0x60U);
    speaker = start_execution(engine, AE_PLAYBACK_STREAM); send_frame(engine, &speaker); pump(engine, 20);
    ae_snapshot(engine, &view); assert(view.playback.first_frame_seen && pa_level == 1 && s.capture_gate);
    unsigned before = capture_frames; stop_execution(engine, &speaker); ae_snapshot(engine, &view);
    assert(view.capture[AE_CAPTURE_LIVE].state == AE_JOB_RUNNING && capture_frames > before &&
        s.tx_running && s.rx_running && !pa_level && !s.playback_gate);
    assert_cleared(dma_storage, sizeof(dma_storage));
    speaker = start_execution(engine, AE_PLAYBACK_STREAM); send_frame(engine, &speaker); pump(engine, 20);
    before = capture_frames; stop_execution(engine, &mic); ae_snapshot(engine, &view);
    assert(view.playback.state == AE_JOB_RUNNING && s.tx_running && !s.rx_running && pa_level == 1 && capture_frames == before);
    stop_execution(engine, &speaker); ae_snapshot(engine, &view);
    assert(view.hardware_off_confirmed && !view.quarantined && !s.tx_running && !s.rx_running && !s.capture_gate && !s.playback_gate);
    free(memory);
    puts("PASS: actual engine/board/codec cold Speaker, Mic-first duplex, independent stop/restart and muted clock-only capture");
}
int main(void)
{
    port=board_audio_mosaico_port(); assert(board_audio_mosaico_init()==AE_OK); drain();
    admit(AE_ROUTE_USB_UAC,AE_CAPTURE_LIVE,(ae_format_t){48000,1,10,AE_PCM_S16_LE});
    admit(AE_ROUTE_USB_UAC,AE_PLAYBACK_STREAM,(ae_format_t){48000,1,10,AE_PCM_S16_LE});
    admit(AE_ROUTE_LOCAL,AE_CAPTURE_RECOGNITION,(ae_format_t){16000,1,10,AE_PCM_S16_LE});
    execute_independent_roles();
    puts("Actual manager admission + board profiles: UAC capture/playback48k mono10ms and spoken16k mono10ms passed.");
    return 0;
}
