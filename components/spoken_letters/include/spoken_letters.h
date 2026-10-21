// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_io.h"

#define SL_RESULTS_MAX 5U
#define SL_FRAME_SAMPLES_MAX 320U
#define SL_CANDIDATE_TTL_MS 30000U

typedef enum { SL_UNAVAILABLE, SL_IDLE, SL_LISTENING, SL_CANDIDATE,
    SL_REJECTED, SL_TIMED_OUT, SL_CANCELLED, SL_FAULT } sl_state_t;
typedef struct {
    bool detected, timed_out;
    uint8_t count;
    uint8_t command_id[SL_RESULTS_MAX]; /* 1..26 Alpha-Zulu, 27..36 zero-nine. */
    float probability[SL_RESULTS_MAX]; /* Native model scores, not calibrated confidence. */
} sl_model_result_t;
typedef struct {
    void *context;
    ae_result_t (*reset)(void *);
    /* Consumes all samples or returns an error. No pointer retention. Run on
     * the speech worker, never the manager's latency-sensitive capture sink. */
    ae_result_t (*process)(void *, const int16_t *, size_t samples, sl_model_result_t *);
} sl_model_port_t;
typedef struct {
    ae_id_t job_id;
    uint64_t token, epoch, ui_revision, audio_generation, consumer_generation;
    uint32_t duration_ms; /* Explicit window 1000..8000 ms. */
} sl_binding_t;
typedef struct {
    sl_state_t state;
    ae_result_t result;
    uint64_t token, epoch, ui_revision;
    char candidate; /* Printable ASCII only; preview confirmation remains external. */
    float probability, runner_up_probability;
    bool cleanup_pending; /* Runtime cancellation latch precedes worker cleanup. */
    bool capture_closed; /* Runtime PCM closure; an existing candidate may remain. */
    bool acoustically_qualified; /* False until measured qualification is supplied. */
} sl_view_t;
typedef struct {
    sl_model_port_t model;
    sl_binding_t binding;
    sl_view_t view;
    bool available, clock_seen, frame_seen;
    uint8_t frame_ms;
    uint64_t now_ms, started_ms, candidate_ms, sequence, sample_offset;
} spoken_letters_t;

/* Pure window/frame policy, one owner, no heap/clock/codec/HID/storage access.
 * The target backend uses the native model detection threshold. Tied distinct
 * candidates are rejected; scores are not claimed as measured confidence.
 * Additional probability/margin tuning requires actual acoustic evidence. */
bool sl_binding_valid(const sl_binding_t *);
void sl_init(spoken_letters_t *, const sl_model_port_t *, ae_result_t availability);
ae_result_t sl_begin(spoken_letters_t *, const sl_binding_t *, uint64_t now_ms);
ae_result_t sl_frame(spoken_letters_t *, const ae_id_t *job, const ae_frame_t *, uint64_t now_ms);
void sl_tick(spoken_letters_t *, uint64_t now_ms);
void sl_cancel(spoken_letters_t *, ae_result_t reason);
ae_result_t sl_confirm(spoken_letters_t *, uint64_t token, uint64_t epoch,
    uint64_t ui_revision, char *candidate);
const char *sl_command(unsigned command_id);
