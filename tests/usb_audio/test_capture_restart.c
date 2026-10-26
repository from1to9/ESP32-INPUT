// SPDX-License-Identifier: Apache-2.0
#define main usb_fixture_main
#include "test_usb_audio.c"
#undef main
#include "audio_engine.h"

static ae_engine_t *engine;
static bool codec_mic, codec_speaker;
static uint64_t next_read_ms, identities = 1000;
static unsigned captured, played;
static uint64_t clock_ms(void *context) { (void)context; return now_us / 1000U; }
static ae_result_t codec_caps(void *context, ae_codec_capabilities_t *out)
{
    (void)context; *out = (ae_codec_capabilities_t){.implemented = true, .available = true, .full_duplex = true,
        .safe_volume_max = 100, .format_count = 1, .formats = {{.format = s_format, .roles = 3}}}; return AE_OK;
}
static ae_result_t codec_prepare(void *context, const ae_format_t *format, uint8_t roles, uint64_t generation)
{ (void)context; assert(same_format(format) && roles == 3 && generation); return AE_OK; }
static ae_result_t codec_mute(void *context, ae_role_t role, bool muted, uint64_t generation)
{ (void)context; assert(generation); if (role == AE_MICROPHONE) codec_mic = !muted; else codec_speaker = !muted; return AE_OK; }
static ae_result_t codec_level(void *context, uint8_t level, uint64_t generation)
{ (void)context; (void)level; assert(generation); return AE_OK; }
static ae_result_t codec_stop(void *context, uint64_t generation)
{ (void)context; assert(generation); codec_mic = codec_speaker = false; return AE_OK; }
static ae_result_t codec_read(void *context, uint64_t generation, void *data, size_t bytes, size_t *actual)
{
    (void)context; assert(generation && bytes == UAC_FRAME_BYTES); *actual = 0;
    if (!codec_mic) return AE_STALE;
    if (clock_ms(NULL) < next_read_ms) return AE_AGAIN;
    memset(data, (int)++captured, bytes); *actual = bytes; next_read_ms = clock_ms(NULL) + 10; return AE_OK;
}
static ae_result_t codec_write(void *context, uint64_t generation, const void *data, size_t bytes, size_t *actual)
{ (void)context; assert(generation && codec_speaker && data && bytes == UAC_FRAME_BYTES); *actual = bytes; ++played; return AE_OK; }
static ae_result_t authorize(void *context, ae_authority_operation_t op, const ae_start_request_t *request,
    uint64_t now, ae_authority_t *out)
{ (void)context; (void)op; (void)request; *out = (ae_authority_t){.allowed = true, .owner_session_alive = true, .source_valid_until_ms = now + 1000000}; return AE_OK; }
static ae_result_t identity(void *context, ae_id_t *out)
{ (void)context; memset(out, 0, sizeof(*out)); uint64_t id = ++identities; memcpy(out->bytes, &id, sizeof(id)); return AE_OK; }
static ae_snapshot_t view(void) { ae_snapshot_t snapshot; ae_snapshot(engine, &snapshot); return snapshot; }
static void step(unsigned ticks)
{
    for (unsigned tick = 0; tick < ticks; ++tick) {
        now_us += 1000;
        if (s_consumer_generation && s_rx_flush_requested == s_rx_flush_completed) {
            assert(input_bytes + 96 <= sizeof(input_data));
            for (unsigned i = 0; i < 96; ++i) input_data[input_bytes + i] = (uint8_t)(input_bytes + i);
            input_bytes += 96;
        }
        service_io();
        ae_frame_t playback;
        if (ae_playback_capacity(engine) && usb_audio_receive(&playback) == AE_OK)
            assert(ae_submit_playback(engine, &playback, clock_ms(NULL)) == AE_OK);
        ae_snapshot_t snapshot = view();
        ae_environment_t environment = {.unlocked = true, .screen_visible = true, .indicator_visible = true,
            .indicator_generation = snapshot.audio_generation, .indicator_seen_ms = clock_ms(NULL)};
        ae_environment(engine, &environment, clock_ms(NULL));
        (void)ae_poll(engine, clock_ms(NULL));
    }
}
static ae_job_snapshot_t begin(ae_job_kind_t kind)
{
    ae_snapshot_t snapshot = view();
    ae_start_request_t request = {.principal_id = {{1}}, .owner_session_id = {{2}}, .consent_id = {{3}},
        .consent_revision = 1, .expected_audio_generation = snapshot.audio_generation, .expected_revision = snapshot.revision,
        .kind = kind, .route = AE_ROUTE_USB_UAC, .route_token = s_token, .format = s_format,
        .duration_seconds = 300, .byte_limit = 1000000, .activate_microphone = true};
    assert(identity(NULL, &request.request_id) == AE_OK);
    ae_job_snapshot_t job; ae_result_t result = ae_start(engine, &request, clock_ms(NULL), &job);
    if (result != AE_OK) { printf("start kind=%d result=%d\n", kind, result); fflush(stdout); }
    assert(result == AE_OK); return job;
}
int main(void)
{
    assert(usb_audio_init() == ESP_OK); usb_audio_stack_started(ESP_OK); usb_audio_stack_event(true, false);
    select_interface(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 1); select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); service_io();
    ae_dependencies_t dependencies = {.codec = {.capabilities = codec_caps, .prepare = codec_prepare, .mute = codec_mute,
        .volume = codec_level, .gain = codec_level, .stop = codec_stop, .read = codec_read, .write = codec_write},
        .route = usb_audio_route_port(), .authority = authorize, .random_id = identity, .monotonic_ms = clock_ms};
    void *memory = calloc(1, ae_engine_bytes()); assert(memory);
    assert(ae_init(memory, ae_engine_bytes(), &dependencies, clock_ms(NULL), &engine) == AE_OK);
    ae_snapshot_t snapshot = view();
    assert(ae_route_select(engine, AE_ROUTE_USB_UAC, snapshot.audio_generation, snapshot.revision, clock_ms(NULL)) == AE_OK);
    ae_environment_t environment = {.unlocked = true, .screen_visible = true}; ae_environment(engine, &environment, clock_ms(NULL));
    ae_job_snapshot_t microphone = begin(AE_CAPTURE_LIVE); step(25);
    assert(view().capture[0].delivered_bytes >= UAC_FRAME_BYTES);
    ae_job_snapshot_t speaker = begin(AE_PLAYBACK_STREAM); step(25);
    snapshot = view(); assert(snapshot.playback.state == AE_JOB_RUNNING && played);
    uint64_t generation = snapshot.audio_generation, link = s_status.link_generation;
    for (unsigned restart = 0; restart < 2; ++restart) {
        uint64_t previous_played = snapshot.playback.delivered_bytes;
        assert(ae_stop(engine, &microphone.job_id, microphone.consumer_generation, AE_STOP_REQUESTED, clock_ms(NULL)) == AE_OK);
        step(3); microphone = begin(AE_CAPTURE_LIVE); step(25); snapshot = view();
        printf("Mic restart%u capture_state=%d received=%llu delivered=%llu error=%d Speaker=%d played=%llu audio_generation=%llu link=%llu\n",
            restart, snapshot.capture[0].state, (unsigned long long)snapshot.capture[0].received_bytes,
            (unsigned long long)snapshot.capture[0].delivered_bytes, snapshot.capture[0].error,
            snapshot.playback.state, (unsigned long long)snapshot.playback.delivered_bytes,
            (unsigned long long)snapshot.audio_generation, (unsigned long long)s_status.link_generation); fflush(stdout);
        assert(snapshot.capture[0].state == AE_JOB_RUNNING && snapshot.capture[0].delivered_bytes >= UAC_FRAME_BYTES && !snapshot.capture[0].error);
        assert(snapshot.audio_generation == generation && s_status.link_generation == link && !snapshot.error && !snapshot.quarantined);
        assert(snapshot.playback.state == AE_JOB_RUNNING && snapshot.playback.consumer_generation == speaker.consumer_generation &&
            snapshot.playback.delivered_bytes > previous_played && !s_status.dropped_playback_frames);
    }
    ae_safety_stop(engine, AE_STOP_PRIVACY, clock_ms(NULL)); step(4);
    assert(!view().capture_gate_open && !view().playback_gate_open);
    free(memory); heap_caps_free(s_memory); s_memory = NULL;
    puts("PASS: actual engine/USB Mic consumer restart preserves the running Speaker and resource identity");
    return 0;
}
