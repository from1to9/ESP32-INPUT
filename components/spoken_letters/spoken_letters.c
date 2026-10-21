// SPDX-License-Identifier: Apache-2.0
#include "spoken_letters.h"
#include <math.h>
#include <string.h>

static const char *const commands[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
    "golf", "hotel", "india", "juliett", "kilo", "lima", "mike", "november", "oscar", "papa",
    "quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey", "x ray", "yankee", "zulu",
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};

const char *sl_command(unsigned id)
{
    return id && id <= sizeof commands / sizeof commands[0] ? commands[id - 1] : NULL;
}

static bool id_nonzero(const ae_id_t *id)
{
    for (size_t i = 0; i < sizeof id->bytes; ++i) if (id->bytes[i]) return true;
    return false;
}

bool sl_binding_valid(const sl_binding_t *b)
{
    return b && id_nonzero(&b->job_id) && b->token && b->epoch && b->ui_revision &&
        b->audio_generation && b->consumer_generation && b->duration_ms >= 1000 && b->duration_ms <= 8000;
}

void sl_init(spoken_letters_t *m, const sl_model_port_t *port, ae_result_t availability)
{
    *m = (spoken_letters_t){0};
    m->available = availability == AE_OK && port && port->process && port->reset;
    if (port) m->model = *port;
    m->view.state = m->available ? SL_IDLE : SL_UNAVAILABLE;
    m->view.result = m->available ? AE_OK : availability == AE_OK ? AE_UNAVAILABLE : availability;
}

static void finish(spoken_letters_t *m, sl_state_t state, ae_result_t reason)
{
    const ae_result_t reset = m->available ? m->model.reset(m->model.context) : AE_UNAVAILABLE;
    m->frame_seen = false;
    m->view.candidate = 0; m->view.probability = 0; m->view.runner_up_probability = 0;
    m->view.state = reset == AE_OK ? state : SL_FAULT;
    m->view.result = reset == AE_OK ? reason : reset;
}

void sl_cancel(spoken_letters_t *m, ae_result_t reason)
{
    if (m->available) finish(m, reason == AE_TIMEOUT ? SL_TIMED_OUT :
        reason == AE_DENIED || reason == AE_PRIVACY || reason == AE_STALE ? SL_CANCELLED : SL_FAULT, reason);
}

void sl_tick(spoken_letters_t *m, uint64_t now_ms)
{
    const bool regressed = m->clock_seen && now_ms < m->now_ms;
    m->clock_seen = true; m->now_ms = now_ms;
    if (!m->available) return;
    if (regressed) finish(m, SL_FAULT, AE_TIME_UNKNOWN);
    else if (m->view.state == SL_LISTENING && now_ms - m->started_ms >= m->binding.duration_ms)
        finish(m, SL_TIMED_OUT, AE_TIMEOUT);
    else if (m->view.state == SL_CANDIDATE && now_ms - m->candidate_ms >= SL_CANDIDATE_TTL_MS)
        finish(m, SL_TIMED_OUT, AE_TIMEOUT);
}

ae_result_t sl_begin(spoken_letters_t *m, const sl_binding_t *b, uint64_t now_ms)
{
    if (!sl_binding_valid(b)) return AE_INVALID;
    sl_tick(m, now_ms);
    if (!m->available) return m->view.result;
    if (m->view.state == SL_LISTENING || m->view.state == SL_CANDIDATE) return AE_BUSY;
    if (b->token <= m->view.token) return AE_STALE;
    if (m->view.token && (!memcmp(&b->job_id, &m->binding.job_id, sizeof b->job_id) ||
        (b->audio_generation == m->binding.audio_generation &&
         b->consumer_generation == m->binding.consumer_generation))) return AE_STALE;
    const ae_result_t reset = m->model.reset(m->model.context);
    if (reset != AE_OK) { m->view.state = SL_FAULT; m->view.result = reset; return reset; }
    m->binding = *b; m->started_ms = now_ms; m->frame_seen = false;
    m->view = (sl_view_t){.state = SL_LISTENING, .result = AE_OK,
        .token = b->token, .epoch = b->epoch, .ui_revision = b->ui_revision};
    return AE_OK;
}

static ae_result_t frame_fault(spoken_letters_t *m, ae_result_t result)
{
    finish(m, SL_FAULT, result);
    return result;
}

ae_result_t sl_frame(spoken_letters_t *m, const ae_id_t *job, const ae_frame_t *f, uint64_t now_ms)
{
    sl_tick(m, now_ms);
    if (!job || !f) return AE_INVALID;
    if (m->view.state != SL_LISTENING || memcmp(job, &m->binding.job_id, sizeof *job) ||
        f->audio_generation != m->binding.audio_generation ||
        f->consumer_generation != m->binding.consumer_generation) return AE_STALE;
    const bool format = f->format.sample_rate_hz == 16000 && f->format.channels == 1 &&
        f->format.encoding == AE_PCM_S16_LE && (f->format.frame_ms == 10 || f->format.frame_ms == 20);
    if (!format || (m->frame_seen && f->format.frame_ms != m->frame_ms) ||
        f->bytes != (unsigned)f->format.frame_ms * 32U ||
        f->bytes > SL_FRAME_SAMPLES_MAX * 2 || id_nonzero(&f->route_token.peer_id) ||
        id_nonzero(&f->route_token.stream_id) || f->route_token.link_generation)
        return frame_fault(m, AE_INTEGRITY);
    const size_t samples = f->bytes / 2U;
    if (f->sequence == UINT64_MAX || f->sample_offset > UINT64_MAX - samples)
        return frame_fault(m, AE_EXHAUSTED);
    if (m->frame_seen && (f->sequence != m->sequence || f->sample_offset != m->sample_offset))
        return frame_fault(m, AE_STALE);
    m->sequence = f->sequence + 1; m->sample_offset = f->sample_offset + samples; m->frame_seen = true;
    m->frame_ms = f->format.frame_ms;
    int16_t pcm[SL_FRAME_SAMPLES_MAX];
    for (size_t i = 0; i < samples; ++i) {
        const uint16_t raw = (uint16_t)((uint16_t)f->data[i * 2] | (uint16_t)((uint16_t)f->data[i * 2 + 1] << 8));
        pcm[i] = (int16_t)(raw >= 32768U ? (int32_t)raw - 65536 : (int32_t)raw);
    }
    sl_model_result_t detected = {0};
    const ae_result_t result = m->model.process(m->model.context, pcm, samples, &detected);
    volatile int16_t *clear = pcm;
    for (size_t i = 0; i < SL_FRAME_SAMPLES_MAX; ++i) clear[i] = 0;
    if (result != AE_OK) return frame_fault(m, result);
    if (detected.timed_out) { finish(m, SL_TIMED_OUT, AE_TIMEOUT); return AE_OK; }
    if (!detected.detected) return AE_OK;
    if (!detected.count || detected.count > SL_RESULTS_MAX) return frame_fault(m, AE_INTEGRITY);
    size_t best = 0;
    for (size_t i = 0; i < detected.count; ++i) {
        if (!sl_command(detected.command_id[i]) || !isfinite(detected.probability[i]) ||
            detected.probability[i] < 0 || detected.probability[i] > 1) return frame_fault(m, AE_INTEGRITY);
        if (detected.probability[i] > detected.probability[best]) best = i;
    }
    float runner = 0;
    if (detected.probability[best] == 0) {
        finish(m, SL_REJECTED, AE_DENIED); return AE_OK;
    }
    for (size_t i = 0; i < detected.count; ++i) {
        if (detected.command_id[i] == detected.command_id[best]) continue;
        if (detected.probability[i] == detected.probability[best]) {
            finish(m, SL_REJECTED, AE_DENIED); return AE_OK;
        }
        runner = fmaxf(runner, detected.probability[i]);
    }
    const uint8_t id = detected.command_id[best];
    const ae_result_t reset = m->model.reset(m->model.context);
    if (reset != AE_OK) return frame_fault(m, reset);
    m->view.state = SL_CANDIDATE; m->view.result = AE_OK; m->candidate_ms = now_ms;
    m->view.candidate = (char)(id <= 26 ? 'A' + id - 1 : '0' + id - 27);
    m->view.probability = detected.probability[best]; m->view.runner_up_probability = runner;
    return AE_OK;
}

ae_result_t sl_confirm(spoken_letters_t *m, uint64_t token, uint64_t epoch,
                       uint64_t revision, char *candidate)
{
    if (!candidate) return AE_INVALID;
    if (m->view.state != SL_CANDIDATE || token != m->view.token ||
        epoch != m->view.epoch || revision != m->view.ui_revision) return AE_STALE;
    *candidate = m->view.candidate;
    m->view.candidate = 0; m->view.probability = 0; m->view.runner_up_probability = 0;
    m->view.state = SL_IDLE;
    return AE_OK;
}
