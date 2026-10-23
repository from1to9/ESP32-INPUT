// SPDX-License-Identifier: Apache-2.0
#include "app_audio_diagnostics.h"
#include "app_media_runtime.h"
#include "audio_media.h"
#include "board_audio_mosaico.h"
#include "usb_audio.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define AUDIO_DIAGNOSTICS_BSS EXT_RAM_BSS_ATTR
#else
#define AUDIO_DIAGNOSTICS_BSS
#endif
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#define DIAGNOSTIC_CHECK_MS 100U
#define DIAGNOSTIC_SUMMARY_MS 10000U
#define DIAGNOSTIC_JOBS (AE_CONSUMERS_MAX + 1U)

typedef struct {
    ae_job_kind_t kind;
    ae_job_state_t state;
    ae_stop_reason_t stop_reason;
    ae_result_t error;
    bool first_frame_seen, cleanup_pending;
    uint64_t first_frame_ms, deadline_ms;
    uint64_t received_bytes, delivered_bytes, sample_frames, discarded_frames;
} diagnostic_job_t;
typedef struct {
    bool snapshot_available, initialized, audio_known, speech_known, busy, result_known;
    ae_result_t result;
    ae_state_t state;
    ae_route_t route;
    ae_result_t error;
    uint64_t generation;
    bool quarantined, off_confirmed, capture_gate, playback_gate, indicator_required;
    bool microphone_mute_confirmed, speaker_mute_confirmed;
    diagnostic_job_t jobs[DIAGNOSTIC_JOBS];
    sl_state_t speech_state;
    ae_result_t speech_result;
    bool speech_cleanup;
    usb_audio_snapshot_t usb;
    board_audio_status_t codec;
} diagnostic_observation_t;
typedef struct {
    app_media_snapshot_t media;
} diagnostic_scratch_t;
typedef struct {
    bool clock_known, clock_failed, observed;
    uint64_t last_check_ms, last_summary_ms;
    diagnostic_scratch_t scratch;
    diagnostic_observation_t current, previous;
} diagnostics_t;

/* Coordinator-only, resident after PSRAM startup. Retained observations contain
 * public state/counters only; the full runtime copy is transient and wiped. */
static AUDIO_DIAGNOSTICS_BSS diagnostics_t diagnostics;
static const char *const TAG = "audio_diag";

static void clear_scratch(void)
{
    volatile unsigned char *bytes = (volatile unsigned char *)&diagnostics.scratch;
    for (size_t i = 0; i < sizeof(diagnostics.scratch); ++i) bytes[i] = 0;
}
static void observe_job(diagnostic_job_t *out, const ae_job_snapshot_t *job)
{
    *out = (diagnostic_job_t){.kind = job->kind, .state = job->state, .stop_reason = job->stop_reason,
        .error = job->error, .first_frame_seen = job->first_frame_seen, .cleanup_pending = job->cleanup_pending,
        .first_frame_ms = job->first_frame_ms, .deadline_ms = job->deadline_ms,
        .received_bytes = job->received_bytes, .delivered_bytes = job->delivered_bytes,
        .sample_frames = job->sample_frames, .discarded_frames = job->discarded_frames};
}
static void observe(void)
{
    diagnostic_observation_t *out = &diagnostics.current;
    app_media_snapshot_t *media = &diagnostics.scratch.media;
    memset(out, 0, sizeof(*out));
    out->snapshot_available = app_media_runtime_snapshot(media);
    out->initialized = out->snapshot_available && media->initialized;
    out->audio_known = out->initialized && media->audio_known;
    out->speech_known = out->initialized && media->speech_known;
    out->busy = out->initialized && media->busy;
    out->result_known = out->initialized && media->result_ready;
    if (out->result_known) out->result = media->result;
    if (out->audio_known) {
        const ae_snapshot_t *audio = &media->audio;
        out->state = audio->state; out->route = audio->route;
        out->error = audio->error;
        out->generation = audio->audio_generation; out->quarantined = audio->quarantined;
        out->off_confirmed = audio->hardware_off_confirmed;
        out->capture_gate = audio->capture_gate_open; out->playback_gate = audio->playback_gate_open;
        out->indicator_required = audio->capture_indicator_required;
        out->microphone_mute_confirmed = audio->microphone_mute_confirmed;
        out->speaker_mute_confirmed = audio->speaker_mute_confirmed;
        for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) observe_job(&out->jobs[i], &audio->capture[i]);
        observe_job(&out->jobs[AE_CONSUMERS_MAX], &audio->playback);
    }
    if (out->speech_known) {
        out->speech_state = media->speech.state; out->speech_result = media->speech.result;
        out->speech_cleanup = media->speech.cleanup_pending;
    }
    out->usb = usb_audio_snapshot();
    out->codec = board_audio_mosaico_status();
    clear_scratch();
}
static bool audio_changed(const diagnostic_observation_t *a, const diagnostic_observation_t *b)
{
    return a->snapshot_available != b->snapshot_available || a->initialized != b->initialized ||
        a->audio_known != b->audio_known || a->busy != b->busy || a->result_known != b->result_known ||
        (a->result_known && a->result != b->result) || a->state != b->state || a->route != b->route || a->error != b->error ||
        a->generation != b->generation || a->quarantined != b->quarantined || a->off_confirmed != b->off_confirmed ||
        a->capture_gate != b->capture_gate || a->playback_gate != b->playback_gate ||
        a->indicator_required != b->indicator_required || a->microphone_mute_confirmed != b->microphone_mute_confirmed ||
        a->speaker_mute_confirmed != b->speaker_mute_confirmed;
}
static bool job_changed(const diagnostic_job_t *a, const diagnostic_job_t *b)
{
    return a->kind != b->kind || a->state != b->state || a->stop_reason != b->stop_reason || a->error != b->error ||
        a->first_frame_seen != b->first_frame_seen || a->cleanup_pending != b->cleanup_pending ||
        a->first_frame_ms != b->first_frame_ms || a->deadline_ms != b->deadline_ms;
}
static bool usb_changed(const usb_audio_snapshot_t *a, const usb_audio_snapshot_t *b)
{
    return a->initialized != b->initialized || a->stack_ready != b->stack_ready || a->mounted != b->mounted ||
        a->suspended != b->suspended || a->playback_selected != b->playback_selected ||
        a->capture_selected != b->capture_selected || a->prepared != b->prepared ||
        a->cleanup_pending != b->cleanup_pending || a->link_generation != b->link_generation ||
        a->audio_generation != b->audio_generation || a->error != b->error;
}
static bool codec_changed(const board_audio_status_t *a, const board_audio_status_t *b)
{
    return a->worker_started != b->worker_started || a->probe_complete != b->probe_complete ||
        a->probe_result != b->probe_result || a->hardware_observed != b->hardware_observed ||
        a->prepared != b->prepared || a->capture_running != b->capture_running ||
        a->playback_running != b->playback_running || a->speaker_muted != b->speaker_muted ||
        a->cleanup_uncertain != b->cleanup_uncertain || a->generation != b->generation || a->roles != b->roles ||
        !ae_format_equal(&a->format, &b->format) || a->last_result != b->last_result || a->last_sdk_error != b->last_sdk_error;
}
static void log_audio(const diagnostic_observation_t *audio)
{
    if (!audio->audio_known) {
        ESP_LOGI(TAG, "Audio diag audio known=0 state=unknown snapshot_available=%d initialized=%d",
            audio->snapshot_available, audio->initialized);
        return;
    }
    ESP_LOGI(TAG, "Audio diag audio known=1 state=%u route=%u error=%d generation=%" PRIu64
        " quarantine=%d off_confirmed=%d capture_gate=%d playback_gate=%d indicator_required=%d"
        " microphone_mute_confirmed=%d speaker_mute_confirmed=%d busy=%d result_known=%d result=%d",
        (unsigned)audio->state, (unsigned)audio->route, (int)audio->error, audio->generation, audio->quarantined, audio->off_confirmed,
        audio->capture_gate, audio->playback_gate, audio->indicator_required, audio->microphone_mute_confirmed,
        audio->speaker_mute_confirmed, audio->busy, audio->result_known, audio->result_known ? (int)audio->result : -1);
}
static void log_job(unsigned slot, const diagnostic_job_t *job)
{
    ESP_LOGI(TAG, "Audio diag job slot=%u kind=%u state=%u stop_reason=%u error=%d first_frame=%d"
        " first_frame_ms=%" PRIu64 " deadline_ms=%" PRIu64 " cleanup=%d received_bytes=%" PRIu64
        " delivered_bytes=%" PRIu64 " sample_frames=%" PRIu64 " discarded_frames=%" PRIu64,
        slot, (unsigned)job->kind, (unsigned)job->state, (unsigned)job->stop_reason, (int)job->error,
        job->first_frame_seen, job->first_frame_ms, job->deadline_ms, job->cleanup_pending,
        job->received_bytes, job->delivered_bytes, job->sample_frames, job->discarded_frames);
}
static void log_speech(const diagnostic_observation_t *speech)
{
    if (!speech->speech_known) { ESP_LOGI(TAG, "Audio diag speech known=0 state=unknown"); return; }
    ESP_LOGI(TAG, "Audio diag speech known=1 state=%u result=%d cleanup=%d", (unsigned)speech->speech_state,
        (int)speech->speech_result, speech->speech_cleanup);
}
static void log_usb(const usb_audio_snapshot_t *usb)
{
    if (!usb->initialized) {
        ESP_LOGI(TAG, "Audio diag USB known=0 state=unknown error_known=%d error=%d",
            usb->error != ESP_OK, usb->error != ESP_OK ? (int)usb->error : -1);
        return;
    }
    ESP_LOGI(TAG, "Audio diag USB known=1 stack_ready=%d mounted=%d suspended=%d playback_selected=%d capture_selected=%d"
        " prepared=%d cleanup=%d link_generation=%" PRIu64 " audio_generation=%" PRIu64
        " dropped_playback=%" PRIu64 " rejected_capture=%" PRIu64 " error=%d",
        usb->stack_ready, usb->mounted, usb->suspended, usb->playback_selected, usb->capture_selected,
        usb->prepared, usb->cleanup_pending, usb->link_generation, usb->audio_generation,
        usb->dropped_playback_frames, usb->rejected_capture_frames, (int)usb->error);
}
static void log_codec(const board_audio_status_t *codec)
{
    if (!codec->worker_started) {
        ESP_LOGI(TAG, "Audio diag codec known=0 state=unknown error_known=%d error=%d",
            codec->last_result != AE_OK, codec->last_result != AE_OK ? (int)codec->last_result : -1);
        return;
    }
    ESP_LOGI(TAG, "Audio diag codec known=1 probe_complete=%d probe_result=%d hardware_observed=%d prepared=%d"
        " capture_running=%d playback_running=%d speaker_muted=%d cleanup_uncertain=%d generation=%" PRIu64
        " roles=%u rate=%u channels=%u frame_ms=%u encoding=%u last_result=%d sdk_error=%d",
        codec->probe_complete, (int)codec->probe_result, codec->hardware_observed, codec->prepared,
        codec->capture_running, codec->playback_running, codec->speaker_muted, codec->cleanup_uncertain,
        codec->generation, codec->roles, (unsigned)codec->format.sample_rate_hz, codec->format.channels,
        codec->format.frame_ms, (unsigned)codec->format.encoding, (int)codec->last_result, codec->last_sdk_error);
}
void app_audio_diagnostics_poll(uint64_t now_ms)
{
    if (diagnostics.clock_failed) return;
    if (diagnostics.clock_known && now_ms < diagnostics.last_check_ms) {
        diagnostics.clock_failed = true;
        ESP_LOGI(TAG, "Audio diag clock=regressed observer_stopped=1");
        return;
    }
    if (diagnostics.clock_known && now_ms - diagnostics.last_check_ms < DIAGNOSTIC_CHECK_MS) return;
    diagnostics.clock_known = true; diagnostics.last_check_ms = now_ms;
    observe();
    const diagnostic_observation_t *current = &diagnostics.current, *previous = &diagnostics.previous;
    bool summary = !diagnostics.observed || now_ms - diagnostics.last_summary_ms >= DIAGNOSTIC_SUMMARY_MS;
    if (summary || audio_changed(current, previous)) log_audio(current);
    for (unsigned i = 0; i < DIAGNOSTIC_JOBS; ++i)
        if (current->audio_known && (summary || !previous->audio_known || job_changed(&current->jobs[i], &previous->jobs[i])))
            log_job(i, &current->jobs[i]);
    if (summary || current->speech_known != previous->speech_known || current->speech_state != previous->speech_state ||
        current->speech_result != previous->speech_result || current->speech_cleanup != previous->speech_cleanup) log_speech(current);
    if (summary || usb_changed(&current->usb, &previous->usb)) log_usb(&current->usb);
    if (summary || codec_changed(&current->codec, &previous->codec)) log_codec(&current->codec);
    diagnostics.previous = diagnostics.current;
    diagnostics.observed = true;
    if (summary) diagnostics.last_summary_ms = now_ms;
}
