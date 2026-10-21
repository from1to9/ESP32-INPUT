// SPDX-License-Identifier: Apache-2.0
#include "app_spoken_letters.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <string.h>

#define SL_QUEUE_FRAMES 8U
#define SL_MODEL_CHUNK_MAX 1024U
#define SL_QUEUE_MAX_AGE_MS 200U

typedef struct { ae_id_t job; ae_frame_t frame; uint64_t admitted_ms; } queued_frame_t;
typedef struct {
    srmodel_list_t *models;
    esp_mn_iface_t *mn;
    model_iface_data_t *mn_data;
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    size_t chunk, output_chunk, fill;
    bool grammar_allocated;
    int16_t pcm[SL_MODEL_CHUNK_MAX];
} model_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started, s_accepting, s_start_pending, s_cancel_pending, s_confirm_pending;
static bool s_bound, s_processing, s_capture_closed;
static ae_result_t s_cancel_reason = AE_DENIED;
static uint64_t s_last_token, s_candidate_started_ms, s_candidate_deadline_ms;
static sl_binding_t s_binding;
static sl_view_t s_view = {.state = SL_UNAVAILABLE, .result = AE_UNAVAILABLE};
static queued_frame_t *s_queue;
static size_t s_head, s_count;
static model_t s_model;

static uint64_t now_ms(void)
{
    const int64_t us = esp_timer_get_time();
    return us < 0 ? 0 : (uint64_t)us / 1000;
}

static void zero(void *p, size_t bytes)
{
    volatile unsigned char *v = p;
    while (bytes--) *v++ = 0;
}

static ae_result_t prepare_afe(model_t *m);

static ae_result_t model_reset(void *context)
{
    model_t *m = context;
    zero(m->pcm, sizeof m->pcm); m->fill = 0;
    if (!m->mn_data) return AE_UNAVAILABLE;
    m->mn->clean(m->mn_data);
    /* Destroy the private AFE instance, including its worker, between windows.
     * Flushing rings alone cannot prove that in-flight old PCM is gone. */
    if (m->afe_data) m->afe->destroy(m->afe_data);
    m->afe_data = NULL;
    return AE_OK;
}

static ae_result_t fetch_results(model_t *m, sl_model_result_t *out)
{
    /* Nonblocking AFE fetch: internal AFE processing may complete between raw
     * manager frames. Never claim silence or a complete chunk on timeout. */
    for (unsigned i = 0; i < 4; ++i) {
        afe_fetch_result_t *f = m->afe->fetch_with_delay(m->afe_data, 0);
        if (!f || f->ret_value == ESP_ERR_TIMEOUT) return AE_OK;
        if (f->ret_value == ESP_FAIL && (!f->data || !f->data_size)) return AE_OK;
        if (f->ret_value != ESP_OK || !f->data || f->data_size != (int)(m->output_chunk * sizeof(int16_t)))
            return AE_INTEGRITY;
        const esp_mn_state_t state = m->mn->detect(m->mn_data, f->data);
        if (state == ESP_MN_STATE_TIMEOUT) { out->timed_out = true; return AE_OK; }
        if (state != ESP_MN_STATE_DETECTED) continue;
        const esp_mn_results_t *r = m->mn->get_results(m->mn_data);
        if (!r || r->num < 1 || r->num > (int)SL_RESULTS_MAX) return AE_INTEGRITY;
        out->detected = true; out->count = (uint8_t)r->num;
        for (int n = 0; n < r->num; ++n) {
            if (r->command_id[n] < 1 || r->command_id[n] > 36) return AE_INTEGRITY;
            out->command_id[n] = (uint8_t)r->command_id[n]; out->probability[n] = r->prob[n];
        }
        return AE_OK;
    }
    return AE_OK;
}

static ae_result_t model_process(void *context, const int16_t *pcm, size_t samples, sl_model_result_t *out)
{
    model_t *m = context;
    *out = (sl_model_result_t){0};
    if (!m->afe_data) {
        const ae_result_t ready = prepare_afe(m);
        if (ready != AE_OK) return ready;
    }
    for (size_t i = 0; i < samples; ++i) {
        m->pcm[m->fill++] = pcm[i];
        if (m->fill != m->chunk) continue;
        if (m->afe->feed(m->afe_data, m->pcm) < 0) return AE_IO_ERROR;
        zero(m->pcm, sizeof m->pcm); m->fill = 0;
    }
    return fetch_results(m, out);
}

static void destroy_model(model_t *m)
{
    if (m->afe_data) m->afe->destroy(m->afe_data);
    if (m->grammar_allocated) (void)esp_mn_commands_free();
    if (m->mn_data) m->mn->destroy(m->mn_data);
    if (m->models) esp_srmodel_deinit(m->models);
    zero(m, sizeof *m);
}

static ae_result_t load_model(model_t *m)
{
    m->models = esp_srmodel_init("model");
    if (!m->models) return AE_UNAVAILABLE;
    char *name = esp_srmodel_filter(m->models, ESP_MN_PREFIX, "mn7_en");
    if (!name) return AE_UNAVAILABLE;
    m->mn = esp_mn_handle_from_name(name);
    if (!m->mn) return AE_UNAVAILABLE;
    m->mn_data = m->mn->create(name, 8000);
    if (!m->mn_data) return AE_QUOTA;
    if (m->mn->get_samp_rate(m->mn_data) != 16000) return AE_NOT_SUPPORTED;
    /* The component owns the one process-wide MultiNet grammar. No test or
     * sample commands survive a successful grammar initialization. */
    if (esp_mn_commands_alloc(m->mn, m->mn_data) != ESP_OK) return AE_QUOTA;
    m->grammar_allocated = true;
    esp_mn_commands_clear();
    for (unsigned id = 1; id <= 36; ++id)
        if (esp_mn_commands_add((int)id, sl_command(id)) != ESP_OK) return AE_INVALID;
    if (esp_mn_commands_update()) return AE_INVALID;
    const ae_result_t ready = prepare_afe(m);
    return ready == AE_OK ? model_reset(m) : ready;
}

static ae_result_t prepare_afe(model_t *m)
{
    afe_config_t *config = afe_config_init("M", m->models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!config) return AE_QUOTA;
    config->wakenet_init = false; /* Explicit short local listening window. */
    config->aec_init = false; /* No unqualified full-duplex playback reference. */
    config->se_init = false; /* One physical microphone. */
    config->ns_init = true; config->ns_model_name = NULL; config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    config->vad_init = true; config->vad_model_name = NULL;
    config->afe_ringbuf_size = 4;
    m->afe = esp_afe_handle_from_config(config);
    if (m->afe) m->afe_data = m->afe->create_from_config(config);
    afe_config_free(config);
    if (!m->afe_data) return AE_QUOTA;
    const int feed = m->afe->get_feed_chunksize(m->afe_data);
    const int fetch = m->afe->get_fetch_chunksize(m->afe_data);
    const int mn = m->mn->get_samp_chunksize(m->mn_data);
    if (m->afe->get_feed_channel_num(m->afe_data) != 1 ||
        m->afe->get_samp_rate(m->afe_data) != 16000 || feed <= 0 || feed > (int)SL_MODEL_CHUNK_MAX ||
        fetch <= 0 || fetch > (int)SL_MODEL_CHUNK_MAX || fetch != mn) return AE_NOT_SUPPORTED;
    m->chunk = (size_t)feed;
    m->output_chunk = (size_t)fetch;
    return AE_OK;
}

static bool matches(const ae_id_t *job, uint64_t generation)
{
    return s_bound && job && !memcmp(job, &s_binding.job_id, sizeof *job) &&
        generation == s_binding.consumer_generation;
}

static void worker(void *unused)
{
    (void)unused;
    const ae_result_t availability = load_model(&s_model);
    if (availability != AE_OK) destroy_model(&s_model);
    sl_model_port_t port = {.context = &s_model, .reset = model_reset, .process = model_process};
    spoken_letters_t core;
    sl_init(&core, &port, availability);
    portENTER_CRITICAL(&s_lock); s_view = core.view; portEXIT_CRITICAL(&s_lock);
    queued_frame_t frame;
    for (;;) {
        bool start, cancel, confirm, got = false;
        ae_result_t cancel_reason;
        sl_binding_t binding;
        portENTER_CRITICAL(&s_lock);
        start = s_start_pending; cancel = s_cancel_pending; confirm = s_confirm_pending;
        binding = s_binding;
        cancel_reason = s_cancel_reason;
        s_start_pending = false; s_cancel_pending = false; s_confirm_pending = false;
        if (s_count && !start && !cancel && !confirm) {
            frame = s_queue[s_head]; zero(&s_queue[s_head], sizeof *s_queue);
            s_head = (s_head + 1) % SL_QUEUE_FRAMES; --s_count; got = true;
        }
        s_processing = start || cancel || confirm || got;
        portEXIT_CRITICAL(&s_lock);
        if (cancel) {
            /* A cancellation may win before the worker starts its admitted
             * window. Preserve that request's identity in the terminal result. */
            core.binding = binding;
            core.view.token = binding.token; core.view.epoch = binding.epoch;
            core.view.ui_revision = binding.ui_revision;
            sl_cancel(&core, cancel_reason);
        }
        else if (confirm) {
            char ignored;
            (void)sl_confirm(&core, binding.token, binding.epoch, binding.ui_revision, &ignored);
        } else if (start) {
            const ae_result_t begun = sl_begin(&core, &binding, now_ms());
            if (begun != AE_OK) { core.view.state = SL_FAULT; core.view.result = begun; }
        } else if (got) {
            const uint64_t current = now_ms();
            if (current < frame.admitted_ms || current - frame.admitted_ms >= SL_QUEUE_MAX_AGE_MS)
                sl_cancel(&core, AE_TIMEOUT);
            else (void)sl_frame(&core, &frame.job, &frame.frame, current);
            zero(&frame, sizeof frame);
            /* An expensive DSP call cannot publish a candidate after the
             * capture window's actual monotonic deadline. */
            const uint64_t completed = now_ms();
            if (core.view.state == SL_CANDIDATE &&
                (completed < core.started_ms || completed - core.started_ms >= core.binding.duration_ms))
                sl_cancel(&core, AE_TIMEOUT);
        }
        sl_tick(&core, now_ms());
        portENTER_CRITICAL(&s_lock);
        /* Safety cancellation/confirmation may arrive while the model runs.
         * Never publish an older candidate across that synchronous latch. */
        if (!s_cancel_pending && !s_confirm_pending && !s_start_pending) {
            s_view = core.view;
            s_view.capture_closed = s_capture_closed;
            s_candidate_started_ms = core.candidate_ms;
            s_candidate_deadline_ms = core.candidate_ms > UINT64_MAX - SL_CANDIDATE_TTL_MS ?
                UINT64_MAX : core.candidate_ms + SL_CANDIDATE_TTL_MS;
            if (core.view.state != SL_LISTENING) s_accepting = false;
        }
        if (core.view.state != SL_LISTENING || cancel || confirm) {
            for (size_t i = 0; i < SL_QUEUE_FRAMES; ++i) zero(&s_queue[i], sizeof *s_queue);
            s_head = 0; s_count = 0;
        }
        s_processing = false;
        portEXIT_CRITICAL(&s_lock);
        if (!got && !start && !cancel && !confirm) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

ae_result_t app_spoken_letters_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_started) { portEXIT_CRITICAL(&s_lock); return AE_BUSY; }
    s_started = true; s_view.result = AE_AGAIN;
    portEXIT_CRITICAL(&s_lock);
    s_queue = heap_caps_calloc(SL_QUEUE_FRAMES, sizeof *s_queue, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue) {
        portENTER_CRITICAL(&s_lock); s_view.result = AE_QUOTA; portEXIT_CRITICAL(&s_lock);
        return AE_QUOTA;
    }
    if (xTaskCreate(worker, "spoken_letters", 6144, NULL, 4, NULL) != pdPASS) {
        zero(s_queue, SL_QUEUE_FRAMES * sizeof *s_queue); heap_caps_free(s_queue); s_queue = NULL;
        portENTER_CRITICAL(&s_lock); s_view.result = AE_QUOTA; portEXIT_CRITICAL(&s_lock);
        return AE_QUOTA;
    }
    return AE_OK;
}

ae_result_t app_spoken_letters_begin(const sl_binding_t *binding)
{
    if (!sl_binding_valid(binding)) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result = AE_OK;
    if (!s_started || s_view.state == SL_UNAVAILABLE) result = s_view.result;
    else if (s_processing || s_start_pending || s_cancel_pending || s_confirm_pending || s_accepting || s_view.state == SL_CANDIDATE)
        result = AE_BUSY;
    else if (binding->token <= s_last_token) result = AE_STALE;
    else if (s_bound && (!memcmp(&binding->job_id, &s_binding.job_id, sizeof binding->job_id) ||
        (binding->audio_generation == s_binding.audio_generation &&
         binding->consumer_generation == s_binding.consumer_generation))) result = AE_STALE;
    else {
        s_last_token = binding->token;
        s_binding = *binding; s_bound = true; s_accepting = true; s_start_pending = true;
        s_capture_closed = false;
        s_view = (sl_view_t){.state = SL_LISTENING, .result = AE_AGAIN,
            .token = binding->token, .epoch = binding->epoch, .ui_revision = binding->ui_revision};
    }
    portEXIT_CRITICAL(&s_lock);
    return result;
}

ae_result_t app_spoken_letters_capture(const ae_id_t *job, const ae_frame_t *frame)
{
    if (!job || !frame) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result;
    if (!s_started || s_view.state == SL_UNAVAILABLE) result = s_view.result;
    else if (!s_bound) result = AE_AGAIN;
    else if (!matches(job, frame->consumer_generation) || frame->audio_generation != s_binding.audio_generation)
        result = !s_accepting && !s_processing && !s_start_pending && !s_cancel_pending &&
            !s_confirm_pending && s_view.state != SL_LISTENING ? AE_AGAIN : AE_STALE;
    else if (s_cancel_pending || s_confirm_pending) result = AE_STALE;
    else if (s_capture_closed) result = AE_STALE;
    else if (!s_accepting) result = s_view.state == SL_CANCELLED || s_view.state == SL_IDLE ? AE_STALE : AE_AGAIN;
    else if (s_count == SL_QUEUE_FRAMES) {
        s_accepting = false; s_cancel_pending = true; s_cancel_reason = AE_OVERFLOW;
        s_view.state = SL_FAULT; s_view.result = AE_OVERFLOW; s_view.cleanup_pending = true; result = AE_OVERFLOW;
    } else {
        queued_frame_t *target = &s_queue[(s_head + s_count) % SL_QUEUE_FRAMES];
        target->job = *job; target->frame = *frame; target->admitted_ms = now_ms();
        ++s_count; result = AE_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return result;
}

void app_spoken_letters_cancel(const ae_id_t *job, uint64_t generation)
{
    portENTER_CRITICAL(&s_lock);
    if (matches(job, generation)) {
        s_accepting = false; s_start_pending = false; s_cancel_pending = true; s_cancel_reason = AE_DENIED;
        s_view.state = SL_CANCELLED; s_view.result = AE_DENIED; s_view.candidate = 0;
        s_view.cleanup_pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void app_spoken_letters_capture_closed(const ae_id_t *job, uint64_t generation)
{
    portENTER_CRITICAL(&s_lock);
    if (matches(job, generation)) {
        s_capture_closed = true; s_view.capture_closed = true; s_accepting = false;
        for (size_t i = 0; i < SL_QUEUE_FRAMES; ++i) zero(&s_queue[i], sizeof *s_queue);
        s_head = 0; s_count = 0;
        const uint64_t current = now_ms();
        const bool candidate = s_view.state == SL_CANDIDATE && current >= s_candidate_started_ms &&
            current < s_candidate_deadline_ms;
        if (!s_cancel_pending && !s_confirm_pending &&
            (s_view.state == SL_LISTENING || (s_view.state == SL_CANDIDATE && !candidate))) {
            s_start_pending = false; s_cancel_pending = true;
            s_cancel_reason = s_view.state != SL_CANDIDATE ? AE_STALE :
                current < s_candidate_started_ms ? AE_TIME_UNKNOWN : AE_TIMEOUT;
            s_view.state = s_cancel_reason == AE_TIMEOUT ? SL_TIMED_OUT :
                s_cancel_reason == AE_TIME_UNKNOWN ? SL_FAULT : SL_CANCELLED;
            s_view.result = s_cancel_reason; s_view.candidate = 0; s_view.cleanup_pending = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

bool app_spoken_letters_snapshot(sl_view_t *view)
{
    if (!view) return false;
    portENTER_CRITICAL(&s_lock); *view = s_view; const bool started = s_started; portEXIT_CRITICAL(&s_lock);
    return started;
}

ae_result_t app_spoken_letters_confirm(uint64_t token, uint64_t epoch, uint64_t revision, char *candidate)
{
    if (!candidate) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result = AE_STALE;
    const uint64_t current = now_ms();
    if (!s_cancel_pending && !s_confirm_pending && current >= s_candidate_started_ms &&
        current < s_candidate_deadline_ms &&
        s_view.state == SL_CANDIDATE &&
        token == s_view.token && epoch == s_view.epoch && revision == s_view.ui_revision) {
        *candidate = s_view.candidate; s_view.candidate = 0; s_view.state = SL_IDLE;
        s_view.cleanup_pending = true;
        s_accepting = false; s_confirm_pending = true; result = AE_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return result;
}
