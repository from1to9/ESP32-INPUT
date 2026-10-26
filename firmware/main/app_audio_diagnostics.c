// SPDX-License-Identifier: Apache-2.0
#include "app_audio_diagnostics.h"
#include "app_media_runtime.h"
#include "audio_media.h"
#include "board_audio_mosaico.h"
#include "usb_audio.h"
#include "usb_audio_transport.h"
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
    bool observed, known, pending, microphone_start, speaker_start, volume_enabled;
    app_media_state_t state;
    app_media_start_block_t microphone_block, speaker_block;
    uint64_t observed_ms, generation, revision;
} diagnostic_admission_t;
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
    uint8_t volume_percent;
    bool capture_context_known;
    uint64_t capture_context_ms;
    app_audio_capture_context_t capture_context;
    diagnostic_job_t jobs[DIAGNOSTIC_JOBS];
    sl_state_t speech_state;
    ae_result_t speech_result;
    bool speech_cleanup;
    usb_audio_snapshot_t usb;
    usb_audio_transport_snapshot_t transport;
    board_audio_status_t codec;
    usb_audio_pcm_snapshot_t usb_pcm;
    board_audio_pcm_snapshot_t codec_pcm;
    diagnostic_admission_t admission;
} diagnostic_observation_t;
typedef struct {
    app_media_snapshot_t media;
} diagnostic_scratch_t;
typedef struct {
    bool clock_known, clock_failed, observed;
    uint64_t last_check_ms, last_summary_ms;
    diagnostic_scratch_t scratch;
    diagnostic_observation_t current, previous;
    bool capture_context_known;
    uint64_t capture_context_ms;
    app_audio_capture_context_t capture_context;
    diagnostic_admission_t admission;
} diagnostics_t;

/* Coordinator-only, resident after PSRAM startup. Retained observations contain
 * public state/counters only; the full runtime copy is transient and wiped. */
static AUDIO_DIAGNOSTICS_BSS diagnostics_t diagnostics;
static const char *const TAG = "audio_diag";

void app_audio_diagnostics_media_view(uint64_t observed_ms, const app_media_view_t *view)
{
    diagnostics.admission = (diagnostic_admission_t){.observed = true,
        .known = view && view->known, .observed_ms = observed_ms};
    if (!view || !view->known) return;
    diagnostics.admission.pending = view->command_pending;
    diagnostics.admission.state = view->state;
    diagnostics.admission.microphone_start = view->microphone.can_start;
    diagnostics.admission.speaker_start = view->speaker.can_start;
    diagnostics.admission.microphone_block = view->microphone.start_block;
    diagnostics.admission.speaker_block = view->speaker.start_block;
    diagnostics.admission.volume_enabled = view->can_set_volume;
    diagnostics.admission.generation = view->audio_generation;
    diagnostics.admission.revision = view->resource_revision;
}

void app_audio_diagnostics_capture_context(uint64_t observed_ms,
    const app_audio_capture_context_t *context)
{
    diagnostics.capture_context_known = context != NULL;
    diagnostics.capture_context_ms = observed_ms;
    diagnostics.capture_context = context ? *context : (app_audio_capture_context_t){0};
}

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
    out->admission = diagnostics.admission;
    out->capture_context_known = diagnostics.capture_context_known;
    out->capture_context_ms = diagnostics.capture_context_ms;
    out->capture_context = diagnostics.capture_context;
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
        out->volume_percent = audio->volume_percent;
        for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) observe_job(&out->jobs[i], &audio->capture[i]);
        observe_job(&out->jobs[AE_CONSUMERS_MAX], &audio->playback);
    }
    if (out->speech_known) {
        out->speech_state = media->speech.state; out->speech_result = media->speech.result;
        out->speech_cleanup = media->speech.cleanup_pending;
    }
    out->usb = usb_audio_snapshot();
    usb_audio_transport_snapshot(&out->transport);
    out->codec = board_audio_mosaico_status();
    usb_audio_pcm_snapshot(&out->usb_pcm);
    board_audio_pcm_snapshot(&out->codec_pcm);
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
        a->speaker_mute_confirmed != b->speaker_mute_confirmed || a->volume_percent != b->volume_percent;
}
static bool job_changed(const diagnostic_job_t *a, const diagnostic_job_t *b)
{
    return a->kind != b->kind || a->state != b->state || a->stop_reason != b->stop_reason || a->error != b->error ||
        a->first_frame_seen != b->first_frame_seen || a->cleanup_pending != b->cleanup_pending ||
        a->first_frame_ms != b->first_frame_ms || a->deadline_ms != b->deadline_ms;
}
static bool capture_context_changed(const diagnostic_observation_t *a, const diagnostic_observation_t *b)
{
    const app_audio_capture_context_t *x = &a->capture_context, *y = &b->capture_context;
    return a->capture_context_known != b->capture_context_known ||
        x->unlocked != y->unlocked || x->privacy_latched != y->privacy_latched ||
        x->maintenance != y->maintenance || x->screen_visible != y->screen_visible ||
        x->bound != y->bound || x->armed != y->armed || x->transfer_confirmed != y->transfer_confirmed ||
        x->owner_alive != y->owner_alive || x->clock_valid != y->clock_valid || x->usable != y->usable ||
        x->display_generation != y->display_generation || x->capture_generation != y->capture_generation ||
        x->indicator_revision != y->indicator_revision || x->display_error != y->display_error;
}
static void log_capture_context(const diagnostic_observation_t *audio, uint64_t now_ms)
{
    if (!audio->capture_context_known) {
        ESP_LOGI(TAG, "Audio diag capture context known=0");
        return;
    }
    const app_audio_capture_context_t *c = &audio->capture_context;
    bool age_known = now_ms >= audio->capture_context_ms;
    ESP_LOGI(TAG, "Audio diag capture context known=1 age_known=%d age_ms=%" PRIu64
        " audio_known=%d required=%d generation_known=%d audio_generation=%" PRIu64
        " unlocked=%d privacy=%d maintenance=%d screen_visible=%d"
        " bound=%d armed=%d transfer=%d owner_alive=%d clock_valid=%d usable=%d"
        " display_generation=%" PRIu64 " capture_generation=%" PRIu64 " indicator_revision=%" PRIu64
        " heartbeat_age_ms=%" PRIu64 " completion_age_ms=%" PRIu64 " display_error=%d",
        age_known, age_known ? now_ms - audio->capture_context_ms : 0,
        audio->audio_known, audio->audio_known ? audio->indicator_required : -1, audio->audio_known,
        audio->generation, c->unlocked, c->privacy_latched, c->maintenance, c->screen_visible,
        c->bound, c->armed, c->transfer_confirmed, c->owner_alive, c->clock_valid, c->usable,
        c->display_generation, c->capture_generation, c->indicator_revision,
        c->heartbeat_age_ms, c->completion_age_ms, c->display_error);
}
static bool usb_changed(const usb_audio_snapshot_t *a, const usb_audio_snapshot_t *b)
{
    return a->initialized != b->initialized || a->stack_ready != b->stack_ready || a->mounted != b->mounted ||
        a->suspended != b->suspended || a->playback_selected != b->playback_selected ||
        a->capture_selected != b->capture_selected || a->prepared != b->prepared ||
        a->cleanup_pending != b->cleanup_pending || a->link_generation != b->link_generation ||
        a->audio_generation != b->audio_generation || a->error != b->error ||
        a->playback_bound != b->playback_bound || a->playback_flush_pending != b->playback_flush_pending;
}
static bool codec_changed(const board_audio_status_t *a, const board_audio_status_t *b)
{
    return a->worker_started != b->worker_started || a->probe_complete != b->probe_complete ||
        a->probe_result != b->probe_result || a->hardware_observed != b->hardware_observed ||
        a->prepared != b->prepared || a->capture_running != b->capture_running ||
        a->playback_running != b->playback_running || a->speaker_muted != b->speaker_muted ||
        a->speaker_mute_confirmed != b->speaker_mute_confirmed || a->tx_clock_running != b->tx_clock_running ||
        a->cleanup_uncertain != b->cleanup_uncertain || a->generation != b->generation || a->roles != b->roles ||
        !ae_format_equal(&a->format, &b->format) || a->last_result != b->last_result || a->last_sdk_error != b->last_sdk_error ||
        a->last_sdk_operation != b->last_sdk_operation || a->first_sdk_error != b->first_sdk_error ||
        a->first_sdk_operation != b->first_sdk_operation;
}
static bool transport_changed(const usb_audio_transport_snapshot_t *a, const usb_audio_transport_snapshot_t *b)
{
    return a->observed != b->observed || a->mode_required != b->mode_required || a->mode_known != b->mode_known ||
        a->mode_valid != b->mode_valid || a->reset_epoch != b->reset_epoch || a->mode_reject != b->mode_reject ||
        a->out.first_failure_known != b->out.first_failure_known || a->in.first_failure_known != b->in.first_failure_known ||
        a->feedback.first_failure_known != b->feedback.first_failure_known;
}

#define TRANSPORT_COUNTS "%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
#define TRANSPORT_VALUES(ep) (ep).submitted, (ep).accepted, (ep).rejected, (ep).completed_ok, \
    (ep).completed_failed, (ep).successful_bytes, (ep).successful_zero
static void log_transport(const usb_audio_transport_snapshot_t *transport)
{
    if (!transport->observed) {
        ESP_LOGI(TAG, "Audio diag USB transport known=0 required=%d mode_known=0", transport->mode_required);
        return;
    }
    const usb_audio_dcd_registers_t *reg = &transport->registers;
    const usb_audio_dcd_registers_t *pending = &transport->pending_registers;
    ESP_LOGI(TAG, "Audio diag USB transport known=1 required=%d mode_known=%d valid=%d epoch=%" PRIu32 " reject=%" PRIu32
        " out=" TRANSPORT_COUNTS " in=" TRANSPORT_COUNTS " fb=" TRANSPORT_COUNTS
        " entry=%" PRIu64 "/%" PRIu64 "/%" PRIu64
        " fb_known=%d/%d/%d fb_value=%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32
        " fb_epoch=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " fb_len=%u/%u"
        " sample=%d:%u:%" PRIu32 ":%" PRIu32 ":%" PRIu32 ":%08" PRIx32 ":%08" PRIx32
        " regs=%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32
        "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32
        " pending=%d:%" PRIu32 ":%" PRIu32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32,
        transport->mode_required, transport->mode_known, transport->mode_valid, transport->reset_epoch, transport->mode_reject,
        TRANSPORT_VALUES(transport->out), TRANSPORT_VALUES(transport->in), TRANSPORT_VALUES(transport->feedback),
        transport->class_out_entries, transport->class_out_rearm_calls, transport->class_out_rearm_rejected,
        transport->feedback_calculated_known, transport->feedback_submitted_known, transport->feedback_completed_known,
        transport->feedback_calculated, transport->feedback_submitted, transport->feedback_completed,
        transport->feedback_calculated_epoch, transport->feedback_submitted_epoch, transport->feedback_completed_epoch,
        transport->feedback_submitted_length, transport->feedback_completed_length,
        transport->registers_known, reg->point, reg->epoch_before, reg->epoch_after, reg->out_activation,
        reg->frame_before, reg->frame_after,
        reg->core_id, reg->hwcfg2, reg->gahbcfg, reg->dcfg, reg->dctl, reg->dthrctl,
        reg->gintsts, reg->gintmsk, reg->daint, reg->out_ctl, reg->out_int, reg->out_size,
        transport->pending_registers_known, pending->epoch_before, pending->out_activation,
        pending->gintsts, pending->out_ctl, pending->out_size);
}
#undef TRANSPORT_COUNTS
#undef TRANSPORT_VALUES
static void log_audio(const diagnostic_observation_t *audio)
{
    if (!audio->audio_known) {
        ESP_LOGI(TAG, "Audio diag audio known=0 state=unknown snapshot_available=%d initialized=%d",
            audio->snapshot_available, audio->initialized);
        return;
    }
    ESP_LOGI(TAG, "Audio diag audio known=1 state=%u route=%u error=%d generation=%" PRIu64
        " quarantine=%d off_confirmed=%d capture_gate=%d playback_gate=%d indicator_required=%d"
        " microphone_mute_confirmed=%d speaker_mute_confirmed=%d busy=%d result_known=%d result=%d volume_percent=%u",
        (unsigned)audio->state, (unsigned)audio->route, (int)audio->error, audio->generation, audio->quarantined, audio->off_confirmed,
        audio->capture_gate, audio->playback_gate, audio->indicator_required, audio->microphone_mute_confirmed,
        audio->speaker_mute_confirmed, audio->busy, audio->result_known, audio->result_known ? (int)audio->result : -1,
        audio->volume_percent);
}

static void log_usb_pcm(const usb_audio_pcm_snapshot_t *pcm)
{
    ESP_LOGI(TAG, "Audio diag USB PCM known=%d current=%d link_generation=%" PRIu64
        " audio_generation=%" PRIu64 " consumer_generation=%" PRIu64
        " frames=%" PRIu64 " samples=%" PRIu64 " nonzero=%" PRIu64 " peak=%u sequence=%" PRIu64,
        pcm->known, pcm->current, pcm->link_generation, pcm->audio_generation, pcm->consumer_generation,
        pcm->frames, pcm->samples, pcm->nonzero_samples, pcm->peak_abs, pcm->last_sequence);
}

static void log_codec_pcm(const board_audio_pcm_snapshot_t *pcm)
{
    ESP_LOGI(TAG, "Audio diag codec PCM generation_known=%d generation=%" PRIu64
        " pcm_known=%d samples=%" PRIu64 " nonzero=%" PRIu64 " peak=%u saturated=%d"
        " calls=%" PRIu64 " failed=%" PRIu64 " invalid=%" PRIu64
        " result_known=%d sdk_result=%d prefix_valid=%d"
        " volume_requested_known=%d volume_requested=%u volume_read_known=%d volume_register=%u volume_matches=%d"
        " mute_read_known=%d mute_requested=%d mute_register=%u mute_matches=%d",
        pcm->generation_known, pcm->generation, pcm->pcm_known, pcm->reported_samples,
        pcm->reported_nonzero_samples, pcm->reported_peak_abs, pcm->saturated,
        pcm->write_calls, pcm->failed_writes, pcm->invalid_prefix_writes,
        pcm->write_result_known, pcm->last_write_sdk_result, pcm->last_prefix_valid,
        pcm->volume_requested_known, pcm->volume_requested, pcm->volume_read_known, pcm->volume_register, pcm->volume_matches,
        pcm->mute_read_known, pcm->mute_requested, pcm->mute_register, pcm->mute_matches);
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
        " dropped_playback=%" PRIu64 " rejected_capture=%" PRIu64 " error=%d"
        " out_cb=%" PRIu64 " out_bytes=%" PRIu64 " out_zero=%" PRIu64
        " in_cb=%" PRIu64 " in_bytes=%" PRIu64 " in_zero=%" PRIu64
        " read_calls=%" PRIu64 " read_bytes=%" PRIu64 " enqueued=%" PRIu64
        " bound=%d flush=%d queued=%u partial=%u c_calls=%" PRIu64 " c_bytes=%" PRIu64 " c_queued=%u",
        usb->stack_ready, usb->mounted, usb->suspended, usb->playback_selected, usb->capture_selected,
        usb->prepared, usb->cleanup_pending, usb->link_generation, usb->audio_generation,
        usb->dropped_playback_frames, usb->rejected_capture_frames, (int)usb->error,
        usb->out_callbacks, usb->out_callback_bytes, usb->out_zero_callbacks,
        usb->in_callbacks, usb->in_callback_bytes, usb->in_zero_callbacks,
        usb->playback_read_calls, usb->playback_read_bytes, usb->playback_frames_enqueued,
        usb->playback_bound, usb->playback_flush_pending, usb->queued_playback_frames, usb->partial_playback_bytes,
        usb->capture_write_calls, usb->capture_write_bytes, usb->queued_capture_frames);
}
static bool admission_changed(const diagnostic_admission_t *a, const diagnostic_admission_t *b)
{
    return a->observed != b->observed || a->known != b->known || a->pending != b->pending ||
        a->state != b->state || a->microphone_start != b->microphone_start ||
        a->speaker_start != b->speaker_start || a->microphone_block != b->microphone_block ||
        a->speaker_block != b->speaker_block || a->volume_enabled != b->volume_enabled ||
        a->generation != b->generation || a->revision != b->revision;
}
static void log_admission(const diagnostic_admission_t *a, uint64_t now_ms)
{
    bool age_known = now_ms >= a->observed_ms;
    if (!a->known) {
        ESP_LOGI(TAG, "Audio diag admission known=0 source=presenter_candidate age_known=%d age_ms=%" PRIu64,
            age_known, age_known ? now_ms - a->observed_ms : 0);
        return;
    }
    ESP_LOGI(TAG, "Audio diag admission known=1 source=presenter_candidate age_known=%d age_ms=%" PRIu64
        " state=%u pending=%d generation=%" PRIu64 " revision=%" PRIu64
        " mic_start=%d mic_block=%u speaker_start=%d speaker_block=%u volume_enabled=%d",
        age_known, age_known ? now_ms - a->observed_ms : 0, (unsigned)a->state, a->pending,
        a->generation, a->revision, a->microphone_start, (unsigned)a->microphone_block,
        a->speaker_start, (unsigned)a->speaker_block, a->volume_enabled);
}
static void log_codec(const board_audio_status_t *codec)
{
    if (!codec->worker_started) {
        ESP_LOGI(TAG, "Audio diag codec known=0 state=unknown error_known=%d error=%d",
            codec->last_result != AE_OK, codec->last_result != AE_OK ? (int)codec->last_result : -1);
        return;
    }
    ESP_LOGI(TAG, "Audio diag codec known=1 probe_complete=%d probe_result=%d hardware_observed=%d prepared=%d"
        " capture_running=%d playback_running=%d speaker_muted=%d speaker_mute_confirmed=%d tx_clock_running=%d"
        " cleanup_uncertain=%d generation=%" PRIu64
        " roles=%u rate=%u channels=%u frame_ms=%u encoding=%u last_result=%d sdk_error=%d sdk_operation=%s"
        " first_sdk_error=%d first_sdk_operation=%s",
        codec->probe_complete, (int)codec->probe_result, codec->hardware_observed, codec->prepared,
        codec->capture_running, codec->playback_running, codec->speaker_muted, codec->speaker_mute_confirmed,
        codec->tx_clock_running, codec->cleanup_uncertain,
        codec->generation, codec->roles, (unsigned)codec->format.sample_rate_hz, codec->format.channels,
        codec->format.frame_ms, (unsigned)codec->format.encoding, (int)codec->last_result, codec->last_sdk_error,
        board_audio_sdk_operation_name(codec->last_sdk_operation), codec->first_sdk_error,
        board_audio_sdk_operation_name(codec->first_sdk_operation));
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
    if (current->admission.observed && (summary || admission_changed(&current->admission, &previous->admission)))
        log_admission(&current->admission, now_ms);
    if (summary || audio_changed(current, previous)) log_audio(current);
    if ((current->indicator_required || previous->indicator_required) &&
        (summary || audio_changed(current, previous) || capture_context_changed(current, previous)))
        log_capture_context(current, now_ms);
    for (unsigned i = 0; i < DIAGNOSTIC_JOBS; ++i)
        if (current->audio_known && (summary || !previous->audio_known || job_changed(&current->jobs[i], &previous->jobs[i])))
            log_job(i, &current->jobs[i]);
    if (summary || current->speech_known != previous->speech_known || current->speech_state != previous->speech_state ||
        current->speech_result != previous->speech_result || current->speech_cleanup != previous->speech_cleanup) log_speech(current);
    if (summary || usb_changed(&current->usb, &previous->usb)) log_usb(&current->usb);
    if (summary || transport_changed(&current->transport, &previous->transport)) log_transport(&current->transport);
    if (summary || codec_changed(&current->codec, &previous->codec)) log_codec(&current->codec);
    /* No per-frame logging. Unknown observations do not invent a zero signal;
     * the first measured frame and stream replacement remain distinguishable. */
    if ((current->usb_pcm.known || previous->usb_pcm.known) &&
        (summary || current->usb_pcm.known != previous->usb_pcm.known ||
         current->usb_pcm.current != previous->usb_pcm.current ||
         current->usb_pcm.audio_generation != previous->usb_pcm.audio_generation ||
         current->usb_pcm.consumer_generation != previous->usb_pcm.consumer_generation ||
         current->usb_pcm.link_generation != previous->usb_pcm.link_generation)) log_usb_pcm(&current->usb_pcm);
    if ((current->codec_pcm.generation_known || previous->codec_pcm.generation_known) &&
        (summary || current->codec_pcm.generation != previous->codec_pcm.generation ||
         current->codec_pcm.generation_known != previous->codec_pcm.generation_known ||
         current->codec_pcm.pcm_known != previous->codec_pcm.pcm_known ||
         codec_changed(&current->codec, &previous->codec) || current->volume_percent != previous->volume_percent))
        log_codec_pcm(&current->codec_pcm);
    diagnostics.previous = diagnostics.current;
    diagnostics.observed = true;
    if (summary) diagnostics.last_summary_ms = now_ms;
}
