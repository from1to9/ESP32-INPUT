// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/app_audio_diagnostics.c"

static app_media_snapshot_t owner;
static usb_audio_snapshot_t usb;
static board_audio_status_t codec;
static usb_audio_transport_snapshot_t transport;
static bool available = true;
static unsigned media_reads, usb_reads, codec_reads, mutations;
static char lines[128][1024];
static size_t line_count;

bool app_media_runtime_snapshot(app_media_snapshot_t *out)
{ ++media_reads; if (available) *out = owner; return available; }
usb_audio_snapshot_t usb_audio_snapshot(void) { ++usb_reads; return usb; }
void usb_audio_transport_snapshot(usb_audio_transport_snapshot_t *out) { *out = transport; }
board_audio_status_t board_audio_mosaico_status(void) { ++codec_reads; return codec; }
/* Forbidden mutations are linked and counted independently of the observer. */
ae_result_t app_media_runtime_submit_local(const app_media_local_request_t *request)
{ (void)request; ++mutations; return AE_INVALID; }
void app_media_runtime_stop(ae_stop_reason_t reason) { (void)reason; ++mutations; }
void app_audio_request_safety_stop(ae_stop_reason_t reason) { (void)reason; ++mutations; }
ae_result_t app_audio_submit(const app_audio_command_t *command)
{ (void)command; ++mutations; return AE_INVALID; }

void audio_diagnostics_test_log(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "audio_diag") && line_count < 128);
    va_list arguments; va_start(arguments, format);
    int count = vsnprintf(lines[line_count], sizeof(lines[line_count]), format, arguments);
    va_end(arguments);
    assert(count > 0 && (size_t)count < (strstr(format, "USB transport") ? 1024U : 768U)); ++line_count;
}
static bool contains_since(size_t first, const char *text)
{
    for (size_t i = first; i < line_count; ++i) if (strstr(lines[i], text)) return true;
    return false;
}
static void poll_at(uint64_t now)
{
    app_media_snapshot_t before = owner;
    usb_audio_snapshot_t usb_before = usb;
    board_audio_status_t codec_before = codec;
    usb_audio_transport_snapshot_t transport_before = transport;
    app_audio_diagnostics_poll(now);
    assert(!memcmp(&before, &owner, sizeof(owner)) && !memcmp(&usb_before, &usb, sizeof(usb)) &&
        !memcmp(&codec_before, &codec, sizeof(codec)) && !memcmp(&transport_before, &transport, sizeof(transport)) && !mutations);
    const unsigned char *bytes = (const unsigned char *)&diagnostics.scratch;
    for (size_t i = 0; i < sizeof(diagnostics.scratch); ++i) assert(!bytes[i]);
}
static void setup(void)
{
    owner = (app_media_snapshot_t){.initialized = true, .audio_known = true, .speech_known = true,
        .audio = {.state = AE_OFF, .route = AE_ROUTE_USB_UAC, .audio_generation = 9, .hardware_off_confirmed = true},
        .speech = {.state = SL_IDLE}};
    usb = (usb_audio_snapshot_t){.initialized = true, .stack_ready = true, .mounted = true,
        .playback_selected = true, .link_generation = 8};
    codec = (board_audio_status_t){.worker_started = true, .probe_complete = true, .hardware_observed = true,
        .speaker_muted = true, .format = {48000, 1, 10, AE_PCM_S16_LE}};
}
static void test_unknown_is_not_success(void)
{
    setup(); available = false; usb = (usb_audio_snapshot_t){0}; codec = (board_audio_status_t){0};
    owner.audio.state = AE_RUNNING; owner.audio.playback.received_bytes = 918273;
    poll_at(0);
    assert(line_count == 5 && contains_since(0, "audio known=0 state=unknown snapshot_available=0") &&
        contains_since(0, "USB known=0 state=unknown error_known=0 error=-1") &&
        contains_since(0, "codec known=0 state=unknown error_known=0 error=-1"));
    assert(!contains_since(0, "received_bytes=") && !contains_since(0, "918273"));
    usb.error = ESP_ERR_NO_MEM; codec.last_result = AE_EXHAUSTED; poll_at(100);
    assert(line_count == 7 && contains_since(5, "error_known=1"));
}
static void test_idle_throttle_and_counter_summary(void)
{
    setup(); poll_at(0); assert(line_count == 9 && media_reads == 1);
    for (uint64_t i = 1; i < 100; ++i) poll_at(i);
    assert(media_reads == 1 && usb_reads == 1 && codec_reads == 1 && line_count == 9);
    for (uint64_t i = 100; i < 10000; i += 100) {
        owner.audio.playback.received_bytes += 960; usb.dropped_playback_frames += 1; poll_at(i);
    }
    assert(line_count == 9 && media_reads == 100);
    poll_at(10000); assert(line_count == 18 && contains_since(9, "received_bytes=95040") &&
        contains_since(9, "dropped_playback=99"));
}
static void test_failed_job_retained_and_replaced(void)
{
    setup(); poll_at(0);
    ae_job_snapshot_t *job = &owner.audio.playback;
    job->kind = AE_PLAYBACK_STREAM; job->state = AE_JOB_STARTING;
    owner.audio.state = AE_PREPARING; owner.audio.hardware_off_confirmed = false;
    poll_at(100); assert(line_count == 11);
    job->state = AE_JOB_ERROR; job->stop_reason = AE_STOP_START_TIMEOUT; job->error = AE_TIMEOUT;
    job->cleanup_pending = true; poll_at(2100);
    assert(line_count == 12 && contains_since(11, "first_frame=0") && contains_since(11, "cleanup=1"));
    owner.audio.state = AE_OFF; owner.audio.hardware_off_confirmed = true; job->cleanup_pending = false;
    poll_at(2200); size_t after_stop = line_count;
    poll_at(2300); assert(line_count == after_stop && diagnostics.previous.jobs[AE_CONSUMERS_MAX].error == AE_TIMEOUT);
    poll_at(10000); assert(contains_since(after_stop, "stop_reason=") &&
        diagnostics.previous.jobs[AE_CONSUMERS_MAX].stop_reason == AE_STOP_START_TIMEOUT);
    job->state = AE_JOB_STARTING; job->error = AE_OK; job->stop_reason = AE_STOP_REQUESTED;
    poll_at(10100); assert(diagnostics.previous.jobs[AE_CONSUMERS_MAX].state == AE_JOB_STARTING);
    job->state = AE_JOB_RUNNING; job->first_frame_seen = true; job->first_frame_ms = 10150;
    job->deadline_ms = 3610150; job->received_bytes = job->delivered_bytes = 960; job->sample_frames = 480;
    size_t prior = line_count; poll_at(10200);
    assert(line_count == prior + 1 && contains_since(prior, "first_frame=1 first_frame_ms=10150") &&
        contains_since(prior, "sample_frames=480"));
}
static void test_availability_recovery(void)
{
    setup(); owner.audio.capture[AE_CAPTURE_LIVE] = (ae_job_snapshot_t){.state = AE_JOB_RUNNING,
        .first_frame_seen = true, .first_frame_ms = 5, .received_bytes = 960};
    poll_at(0); available = false; poll_at(100);
    assert(contains_since(9, "audio known=0 state=unknown") && !contains_since(9, "received_bytes=0"));
    size_t prior = line_count; available = true; poll_at(200);
    assert(contains_since(prior, "audio known=1") && contains_since(prior, "received_bytes=960"));
    owner.audio_known = false; owner.speech_known = false; prior = line_count; poll_at(300);
    assert(contains_since(prior, "snapshot_available=1 initialized=1") &&
        contains_since(prior, "speech known=0 state=unknown") && !contains_since(prior, "received_bytes="));
}
static void test_transport_and_codec_errors(void)
{
    setup(); poll_at(0);
    usb.suspended = true; usb.prepared = false; ++usb.link_generation;
    codec.last_result = AE_IO_ERROR; codec.last_sdk_error = 1234; codec.cleanup_uncertain = true;
    codec.last_sdk_operation = BOARD_AUDIO_SDK_I2C_READ;
    codec.first_sdk_error = 1234; codec.first_sdk_operation = BOARD_AUDIO_SDK_I2C_READ;
    size_t prior = line_count; poll_at(100);
    assert(line_count == prior + 2 && contains_since(prior, "suspended=1") &&
        contains_since(prior, "cleanup_uncertain=1") && contains_since(prior, "sdk_error=1234") &&
        contains_since(prior, "sdk_operation=i2c_master_transmit_receive"));
    owner.audio.error = AE_UNCERTAIN; prior = line_count; poll_at(200);
    assert(line_count == prior + 1 && contains_since(prior, "audio known=1"));
    codec.probe_result = AE_TIMEOUT; codec.format.sample_rate_hz = 16000; prior = line_count; poll_at(300);
    assert(line_count == prior + 1 && contains_since(prior, "rate=16000"));
    codec.tx_clock_running = true; codec.speaker_muted = codec.speaker_mute_confirmed = true;
    codec.last_result = AE_OK; codec.cleanup_uncertain = false; prior = line_count; poll_at(400);
    assert(line_count == prior + 1 && contains_since(prior, "tx_clock_running=1") &&
        contains_since(prior, "playback_running=0 speaker_muted=1 speaker_mute_confirmed=1") &&
        contains_since(prior, "first_sdk_error=1234 first_sdk_operation=i2c_master_transmit_receive"));
}
static void test_usb_observation_boundaries(void)
{
    setup(); poll_at(0); size_t prior = line_count;
    usb.out_callbacks = 2; usb.out_callback_bytes = 96; usb.out_zero_callbacks = 1;
    usb.in_callbacks = 3; usb.in_callback_bytes = 192; usb.in_zero_callbacks = 1;
    usb.playback_read_calls = 4; usb.playback_read_bytes = 96; usb.playback_frames_enqueued = 0;
    usb.capture_write_calls = 5; usb.capture_write_bytes = 392;
    usb.queued_playback_frames = 1; usb.partial_playback_bytes = 96; usb.queued_capture_frames = 2;
    poll_at(100); assert(line_count == prior); /* Counter/queue samples wait for the summary. */
    usb.playback_bound = true; usb.playback_flush_pending = true; poll_at(200);
    assert(line_count == prior + 1 && contains_since(prior, "out_cb=2 out_bytes=96 out_zero=1") &&
        contains_since(prior, "in_cb=3 in_bytes=192 in_zero=1") &&
        contains_since(prior, "read_calls=4 read_bytes=96 enqueued=0") &&
        contains_since(prior, "bound=1 flush=1 queued=1 partial=96 c_calls=5 c_bytes=392 c_queued=2"));
    prior = line_count; usb.playback_flush_pending = false; poll_at(300);
    assert(line_count == prior + 1 && contains_since(prior, "bound=1 flush=0"));
    usb.link_generation = usb.audio_generation = UINT64_MAX;
    usb.dropped_playback_frames = usb.rejected_capture_frames = UINT64_MAX;
    usb.out_callbacks = usb.out_callback_bytes = usb.out_zero_callbacks = UINT64_MAX;
    usb.in_callbacks = usb.in_callback_bytes = usb.in_zero_callbacks = UINT64_MAX;
    usb.playback_read_calls = usb.playback_read_bytes = usb.playback_frames_enqueued = UINT64_MAX;
    usb.capture_write_calls = usb.capture_write_bytes = UINT64_MAX;
    usb.queued_playback_frames = usb.partial_playback_bytes = usb.queued_capture_frames = UINT16_MAX;
    prior = line_count; poll_at(10000);
    assert(contains_since(prior, "out_cb=18446744073709551615") &&
        contains_since(prior, "c_bytes=18446744073709551615 c_queued=65535"));
}
static void test_transport_mode_and_results(void)
{
    setup(); transport.observed = transport.mode_required = true; poll_at(0);
    assert(contains_since(0, "transport known=1 required=1 mode_known=0 valid=0"));
    transport.mode_known = true; transport.reset_epoch = 3; transport.mode_reject = 8;
    size_t prior = line_count; poll_at(100);
    assert(line_count == prior + 1 && contains_since(prior, "mode_known=1 valid=0 epoch=3 reject=8"));
    transport.mode_valid = true; transport.mode_reject = 0;
    transport.feedback_calculated_known = transport.feedback_submitted_known = true;
    transport.feedback_calculated = transport.feedback_submitted = 0x60000;
    transport.out.completed_failed = 1; transport.out.first_failure_known = true;
    prior = line_count; poll_at(200);
    assert(line_count == prior + 1 && contains_since(prior, "fb_known=1/1/0") && contains_since(prior, "out=0/0/0/0/1/0/0"));
    transport.in.completed_failed = 10; prior = line_count; poll_at(300); assert(line_count == prior);
    usb_audio_endpoint_observation_t *endpoints[] = {&transport.out, &transport.in, &transport.feedback};
    for (unsigned i = 0; i < 3; ++i) {
        endpoints[i]->submitted = endpoints[i]->accepted = endpoints[i]->rejected = UINT64_MAX;
        endpoints[i]->completed_ok = endpoints[i]->completed_failed = endpoints[i]->successful_bytes = endpoints[i]->successful_zero = UINT64_MAX;
    }
    transport.class_out_entries = transport.class_out_rearm_calls = transport.class_out_rearm_rejected = UINT64_MAX;
    transport.feedback_calculated = transport.feedback_submitted = transport.feedback_completed = UINT32_MAX;
    transport.feedback_calculated_epoch = transport.feedback_submitted_epoch = transport.feedback_completed_epoch = UINT32_MAX;
    transport.feedback_submitted_length = transport.feedback_completed_length = UINT8_MAX;
    transport.reset_epoch = transport.mode_reject = UINT32_MAX;
    memset(&transport.registers, 0xff, sizeof(transport.registers));
    memset(&transport.pending_registers, 0xff, sizeof(transport.pending_registers));
    transport.registers_known = transport.pending_registers_known = true;
    prior = line_count; poll_at(10000);
    assert(contains_since(prior, "out=18446744073709551615/") && contains_since(prior, "regs=ffffffff/") &&
        contains_since(prior, "pending=1:4294967295:4294967295:ffffffff:ffffffff:ffffffff"));
}
static void test_private_metadata_wiped(void)
{
    setup();
    memset(owner.audio.route_token.peer_id.bytes, 'X', AE_ID_BYTES);
    memset(owner.audio.route_token.stream_id.bytes, 'Y', AE_ID_BYTES);
    memset(owner.audio.playback.job_id.bytes, 'Z', AE_ID_BYTES);
    memset(owner.result_detail.metadata.principal_id.bytes, 'W', AE_ID_BYTES);
    owner.speech.candidate = 'Q'; owner.speech.state = SL_CANDIDATE; owner.speech.token = UINT64_C(98765432101234);
    poll_at(0);
    assert(!contains_since(0, "58585858") && !contains_since(0, "59595959") &&
        !contains_since(0, "5a5a5a5a") && !contains_since(0, "57575757") &&
        !contains_since(0, "98765432101234") && !contains_since(0, "candidate") && !contains_since(0, "Q"));
    assert(!diagnostics.scratch.media.speech.candidate);
}
static void test_clock_regression_is_observer_only(void)
{
    setup(); poll_at(UINT64_MAX - 10000); poll_at(UINT64_MAX); assert(line_count == 18);
    unsigned reads = media_reads; poll_at(UINT64_MAX - 1); assert(line_count == 19 && diagnostics.clock_failed);
    poll_at(UINT64_MAX); assert(line_count == 19 && media_reads == reads && contains_since(18, "observer_stopped=1"));
}
static void test_capture_context_is_copied_observation(void)
{
    setup(); owner.audio.capture_indicator_required = true;
    poll_at(0);
    assert(line_count == 10 && contains_since(0, "capture context known=0"));
    app_audio_capture_context_t context = {.unlocked = true, .screen_visible = true,
        .bound = true, .armed = true, .owner_alive = true, .clock_valid = true,
        .capture_generation = 9, .display_generation = 3, .indicator_revision = 4};
    app_audio_capture_context_t before = context;
    unsigned reads = media_reads;
    app_audio_diagnostics_capture_context(100, &context);
    assert(!memcmp(&before, &context, sizeof(context)) && media_reads == reads && !mutations);
    context.usable = true; /* The observer retained the prior copied value. */
    size_t first = line_count; poll_at(100);
    assert(contains_since(first, "capture context known=1 age_known=1 age_ms=0") &&
        contains_since(first, "transfer=0 owner_alive=1 clock_valid=1 usable=0"));
    first = line_count;
    for (uint64_t now = 101; now < 200; ++now) {
        context.heartbeat_age_ms = now;
        app_audio_diagnostics_capture_context(now, &context); poll_at(now);
    }
    assert(line_count == first);
    poll_at(200);
    assert(contains_since(first, "usable=1") && contains_since(first, "age_ms=1"));
    first = line_count; context.heartbeat_age_ms = 3; context.completion_age_ms = 9;
    app_audio_diagnostics_capture_context(300, &context); poll_at(300);
    assert(line_count == first); /* Admitted age-only observation stays quiet. */
    context.transfer_confirmed = true;
    app_audio_diagnostics_capture_context(500, &context); first = line_count; poll_at(400);
    assert(contains_since(first, "age_known=0 age_ms=0"));
    app_audio_diagnostics_capture_context(500, NULL); first = line_count; poll_at(500);
    assert(contains_since(first, "capture context known=0"));
    assert(!mutations);
}
static void test_capture_context_does_not_invent_owner_state(void)
{
    setup(); owner.audio.capture_indicator_required = true;
    app_audio_capture_context_t context = {.unlocked = true, .screen_visible = true,
        .bound = true, .armed = true, .owner_alive = true, .clock_valid = true,
        .capture_generation = UINT64_MAX, .display_generation = UINT64_MAX, .indicator_revision = UINT64_MAX,
        .heartbeat_age_ms = UINT64_MAX, .completion_age_ms = UINT64_MAX, .display_error = INT32_MIN};
    app_audio_diagnostics_capture_context(0, &context); poll_at(UINT64_MAX - 100);
    assert(line_count == 10 && contains_since(0, "age_ms=18446744073709551515") &&
        contains_since(0, "display_error=-2147483648"));
    size_t first = line_count; available = false; poll_at(UINT64_MAX);
    assert(contains_since(first, "capture context known=1") &&
        contains_since(first, "audio_known=0 required=-1 generation_known=0") &&
        !contains_since(first, "required=0 audio_generation=0"));
    assert(!mutations);
}
int main(int argc, char **argv)
{
    const char *names[] = {"unknown_is_not_success", "idle_throttle_and_counter_summary", "failed_job_retained_and_replaced",
        "availability_recovery", "transport_and_codec_errors", "usb_observation_boundaries", "transport_mode_and_results", "private_metadata_wiped", "clock_regression_is_observer_only", "capture_context_is_copied_observation", "capture_context_does_not_invent_owner_state"};
    void (*cases[])(void) = {test_unknown_is_not_success, test_idle_throttle_and_counter_summary, test_failed_job_retained_and_replaced,
        test_availability_recovery, test_transport_and_codec_errors, test_usb_observation_boundaries, test_transport_mode_and_results, test_private_metadata_wiped, test_clock_regression_is_observer_only, test_capture_context_is_copied_observation, test_capture_context_does_not_invent_owner_state};
    assert(argc == 2);
    if (!strcmp(argv[1], "--list")) { for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) puts(names[i]); return 0; }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(argv[1], names[i])) { cases[i](); printf("PASS: audio diagnostics %s\n", names[i]); return 0; }
    return 2;
}
