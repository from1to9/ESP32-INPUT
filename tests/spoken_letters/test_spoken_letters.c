// SPDX-License-Identifier: Apache-2.0
#include "spoken_letters.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned resets, calls; ae_result_t reset, process; sl_model_result_t output; } model_t;
typedef struct { spoken_letters_t core; model_t model; sl_binding_t binding; ae_frame_t frame; } fixture_t;

static ae_result_t reset(void *p)
{
    model_t *m = p; ++m->resets; return m->reset;
}

static ae_result_t process(void *p, const int16_t *pcm, size_t samples, sl_model_result_t *out)
{
    model_t *m = p; ++m->calls;
    assert(samples == 160 || samples == 320);
    assert(pcm[0] == INT16_MIN && pcm[1] == INT16_MAX && pcm[2] == -1 && pcm[3] == 0);
    *out = m->output;
    return m->process;
}

static void setup(fixture_t *f)
{
    *f = (fixture_t){.binding = {.job_id = {{7}}, .token = 1, .epoch = 3, .ui_revision = 5,
            .audio_generation = 11, .consumer_generation = 13, .duration_ms = 6000},
        .frame = {.audio_generation = 11, .consumer_generation = 13, .sequence = 9, .sample_offset = 1000,
            .format = {.sample_rate_hz = 16000, .channels = 1, .frame_ms = 10, .encoding = AE_PCM_S16_LE},
            .bytes = 320, .data = {0,128,255,127,255,255,0,0}}};
    const sl_model_port_t port = {.context = &f->model, .reset = reset, .process = process};
    sl_init(&f->core, &port, AE_OK);
    assert(sl_begin(&f->core, &f->binding, 100) == AE_OK);
}

static void alphabet_digits_candidate_confirmation_and_no_replay(void)
{
    for (unsigned id = 1; id <= 36; ++id) {
        fixture_t f; setup(&f);
        assert(sl_command(id));
        f.model.output = (sl_model_result_t){.detected = true, .count = 2,
            .command_id = {(uint8_t)id, (uint8_t)(id == 36 ? 1 : id + 1)}, .probability = {.9f,.2f}};
        assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
        const char expected = (char)(id <= 26 ? 'A' + id - 1 : '0' + id - 27);
        assert(f.core.view.state == SL_CANDIDATE && f.core.view.candidate == expected);
        assert(!f.core.view.acoustically_qualified && f.core.view.runner_up_probability == .2f);
        char character = '!';
        assert(sl_confirm(&f.core, 2, 3, 5, &character) == AE_STALE && character == '!');
        assert(sl_confirm(&f.core, 1, 4, 5, &character) == AE_STALE && character == '!');
        assert(sl_confirm(&f.core, 1, 3, 6, &character) == AE_STALE && character == '!');
        assert(sl_confirm(&f.core, 1, 3, 5, &character) == AE_OK && character == expected);
        assert(sl_confirm(&f.core, 1, 3, 5, &character) == AE_STALE);
        assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 121) == AE_STALE && f.model.calls == 1);
        assert(sl_begin(&f.core, &f.binding, 122) == AE_STALE);
        ++f.binding.token;
        assert(sl_begin(&f.core, &f.binding, 123) == AE_STALE); /* Never reuse buffered old capture. */
        ++f.binding.job_id.bytes[0]; ++f.binding.consumer_generation;
        assert(sl_begin(&f.core, &f.binding, 123) == AE_OK);
        sl_cancel(&f.core, AE_PRIVACY);
        assert(f.core.view.state == SL_CANCELLED && !f.core.view.candidate);
    }
    assert(!sl_command(0) && !sl_command(37));
}

static void format_generation_sequence_and_backend_failures(void)
{
    for (unsigned fault = 0; fault < 13; ++fault) {
        fixture_t f; setup(&f);
        if (fault == 0) f.frame.format.sample_rate_hz = 48000;
        if (fault == 1) f.frame.format.channels = 2;
        if (fault == 2) f.frame.format.frame_ms = 11;
        if (fault == 3) f.frame.format.encoding = (ae_encoding_t)0;
        if (fault == 4) --f.frame.bytes;
        if (fault == 5) f.frame.route_token.peer_id.bytes[0] = 1;
        if (fault == 6) f.frame.route_token.stream_id.bytes[0] = 1;
        if (fault == 7) f.frame.route_token.link_generation = 1;
        if (fault == 8) f.frame.sequence = UINT64_MAX;
        if (fault == 9) f.frame.sample_offset = UINT64_MAX;
        if (fault == 10) f.model.process = AE_IO_ERROR;
        if (fault >= 11) {
            assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 110) == AE_OK);
            ++f.frame.sequence; f.frame.sample_offset += 160;
            if (fault == 11) ++f.frame.sequence;
            if (fault == 12) ++f.frame.sample_offset;
        }
        assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) != AE_OK);
        assert(f.core.view.state == SL_FAULT && !f.core.view.candidate);
    }
    fixture_t f; setup(&f);
    ae_id_t old = {{8}};
    assert(sl_frame(&f.core, &old, &f.frame, 110) == AE_STALE && !f.model.calls);
    --f.frame.audio_generation;
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 111) == AE_STALE && !f.model.calls);
    ++f.frame.audio_generation; --f.frame.consumer_generation;
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 112) == AE_STALE && !f.model.calls);
    assert(f.core.view.state == SL_LISTENING);
    ++f.frame.consumer_generation;
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
    ++f.frame.sequence; f.frame.sample_offset += 160;
    f.frame.format.frame_ms = 20; f.frame.bytes = 640;
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 140) == AE_INTEGRITY);
    setup(&f); f.frame.format.frame_ms = 20; f.frame.bytes = 640;
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
}

static void malformed_ambiguous_results_and_threshold_truth(void)
{
    for (unsigned fault = 0; fault < 7; ++fault) {
        fixture_t f; setup(&f);
        f.model.output = (sl_model_result_t){.detected = true, .count = 1, .command_id = {1}, .probability = {.9f}};
        if (fault == 0) f.model.output.count = 0;
        if (fault == 1) f.model.output.count = 6;
        if (fault == 2) f.model.output.command_id[0] = 0;
        if (fault == 3) f.model.output.command_id[0] = 37;
        if (fault == 4) f.model.output.probability[0] = NAN;
        if (fault == 5) f.model.output.probability[0] = -1;
        if (fault == 6) f.model.output.probability[0] = 1.01f;
        assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_INTEGRITY);
        assert(f.core.view.state == SL_FAULT && !f.core.view.candidate);
    }
    fixture_t f; setup(&f);
    f.model.output = (sl_model_result_t){.detected = true, .count = 2, .command_id = {1,2}, .probability = {.8f,.8f}};
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
    assert(f.core.view.state == SL_REJECTED && !f.core.view.candidate);
    setup(&f);
    f.model.output = (sl_model_result_t){.detected = true, .count = 1, .command_id = {1}, .probability = {0}};
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
    assert(f.core.view.state == SL_REJECTED && !f.core.view.candidate);
    setup(&f); f.model.output.timed_out = true;
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
    assert(f.core.view.state == SL_TIMED_OUT && f.core.view.result == AE_TIMEOUT);
}

static void windows_clock_expiry_and_unavailable_model(void)
{
    fixture_t f; setup(&f);
    sl_tick(&f.core, 6099); assert(f.core.view.state == SL_LISTENING);
    sl_tick(&f.core, 6100); assert(f.core.view.state == SL_TIMED_OUT);
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 6101) == AE_STALE);
    setup(&f); sl_tick(&f.core, 99);
    assert(f.core.view.state == SL_FAULT && f.core.view.result == AE_TIME_UNKNOWN);
    setup(&f);
    f.model.output = (sl_model_result_t){.detected = true, .count = 1, .command_id = {26}, .probability = {.9f}};
    assert(sl_frame(&f.core, &f.binding.job_id, &f.frame, 120) == AE_OK);
    sl_tick(&f.core, 30119); assert(f.core.view.state == SL_CANDIDATE);
    sl_tick(&f.core, 30120); assert(f.core.view.state == SL_TIMED_OUT && !f.core.view.candidate);
    setup(&f); sl_cancel(&f.core, AE_DENIED);
    ++f.binding.token; f.model.reset = AE_QUOTA;
    ++f.binding.job_id.bytes[0]; ++f.binding.consumer_generation;
    assert(sl_begin(&f.core, &f.binding, 130) == AE_QUOTA && f.core.view.state == SL_FAULT);
    sl_init(&f.core, NULL, AE_UNAVAILABLE);
    assert(sl_begin(&f.core, &f.binding, 140) == AE_UNAVAILABLE && f.core.view.state == SL_UNAVAILABLE);
    f.binding.duration_ms = 8001;
    assert(!sl_binding_valid(&f.binding));
}

int main(void)
{
    alphabet_digits_candidate_confirmation_and_no_replay();
    format_generation_sequence_and_backend_failures();
    malformed_ambiguous_results_and_threshold_truth();
    windows_clock_expiry_and_unavailable_model();
    puts("PASS: spoken letters grammar, generation/format continuity, bounded windows, candidate confirmation and rejection");
    return 0;
}
