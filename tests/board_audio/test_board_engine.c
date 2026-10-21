// SPDX-License-Identifier: Apache-2.0
/* Compose actual manager admission with the production board capabilities and
 * pinned codec driver. USB selection/authority are explicit boundary inputs;
 * admission is not a USB host, acoustic or device qualification claim. */
#define main board_fixture_main
#include "test_board_audio.c"
#undef main
#include "audio_engine.h"

static uint64_t ids;
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
static uint64_t clock_ms(void *context) { (void)context; return 1000; }
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
        .capture=true,.playback=true,.format={48000,1,10,AE_PCM_S16_LE},.token=usb_token};
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
int main(void)
{
    port=board_audio_mosaico_port(); assert(board_audio_mosaico_init()==AE_OK); drain();
    admit(AE_ROUTE_USB_UAC,AE_CAPTURE_LIVE,(ae_format_t){48000,1,10,AE_PCM_S16_LE});
    admit(AE_ROUTE_USB_UAC,AE_PLAYBACK_STREAM,(ae_format_t){48000,1,10,AE_PCM_S16_LE});
    admit(AE_ROUTE_LOCAL,AE_CAPTURE_RECOGNITION,(ae_format_t){16000,1,10,AE_PCM_S16_LE});
    puts("Actual manager admission + board profiles: UAC capture/playback48k mono10ms and spoken16k mono10ms passed.");
    return 0;
}
