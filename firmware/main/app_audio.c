// SPDX-License-Identifier: Apache-2.0
#include "app_audio.h"
#include "audio_media.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INGRESS_FRAMES 4U
#define WORKER_STACK_BYTES 8192U
#define COMMAND_TIMEOUT_MS 30000U
#define PROGRESS_PASSES 4U
#define PROGRESS_BUDGET_MS 4U

typedef struct {
    ae_dependencies_t original;
    ae_engine_t *engine;
    void *engine_memory;
    ae_snapshot_t view;
    ae_environment_t environment;
    app_audio_command_t command;
    app_audio_result_t result;
    ae_ring_t ingress;
    ae_frame_t ingress_frames[INGRESS_FRAMES];
    ae_frame_t frame, ingress_bound;
    ae_record_metadata_t records[AE_LIST_MAX];
    uint8_t record_data[8192];
    uint64_t payload_token;
    size_t payload_bytes, payload_count;
    TaskHandle_t task;
    uint64_t command_deadline;
    bool command_pending, result_pending, view_valid, ingress_open;
} app_state_t;

static struct {
    portMUX_TYPE lock;
    app_state_t *state;
    bool initializing;
    ae_result_t unavailable_result;
    atomic_uint safety_sequence;
    atomic_uint handled_sequence;
    atomic_int safety_reason;
    atomic_bool shutting_down;
} app = {.lock = portMUX_INITIALIZER_UNLOCKED, .unavailable_result = AE_UNAVAILABLE};

static void wake(void *context) { app_state_t *state = context; if (state->task) xTaskNotifyGive(state->task); }
static void completion_wake(void *context)
{
    (void)context;
    /* Drivers can retain a copied callback across unregister. A completion is
     * only a wake hint, so resolve the current owner under its lifetime lock
     * instead of borrowing freed state or a deleted task handle. */
    portENTER_CRITICAL(&app.lock);
    if (app.state) wake(app.state);
    portEXIT_CRITICAL(&app.lock);
}
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static uint64_t policy_clock(void *context) { (void)context; return now_ms(); }
static bool safety_pending(void)
{
    unsigned current = atomic_load_explicit(&app.safety_sequence, memory_order_acquire);
    return current == UINT_MAX || current != atomic_load_explicit(&app.handled_sequence, memory_order_acquire);
}
static bool policy_stop(void *context)
{
    app_state_t *state = context;
    return safety_pending() || (state->original.safety_stop_requested &&
        state->original.safety_stop_requested(state->original.policy_context));
}
static ae_result_t policy_authority(void *context, ae_authority_operation_t operation,
    const ae_start_request_t *request, uint64_t time, ae_authority_t *authority)
{
    app_state_t *state = context;
    return state->original.authority ? state->original.authority(state->original.policy_context,
        operation, request, time, authority) : AE_DENIED;
}
static ae_result_t policy_id(void *context, ae_id_t *id)
{
    app_state_t *state = context;
    if (state->original.random_id) return state->original.random_id(state->original.policy_context, id);
    esp_fill_random(id->bytes, sizeof(id->bytes));
    return ae_id_empty(id) ? AE_INTEGRITY : AE_OK;
}
static ae_result_t policy_time(void *context, uint64_t *seconds)
{
    app_state_t *state = context;
    return state->original.trusted_unix_time ? state->original.trusted_unix_time(
        state->original.policy_context, seconds) : AE_TIME_UNKNOWN;
}
static ae_result_t policy_sink(void *context, ae_job_kind_t kind, const ae_id_t *job, const ae_frame_t *frame)
{
    app_state_t *state = context;
    if (policy_stop(state)) return AE_PRIVACY;
    return state->original.capture_sink ? state->original.capture_sink(state->original.policy_context,
        kind, job, frame) : AE_UNAVAILABLE;
}
static void policy_cancel(void *context, ae_job_kind_t kind, const ae_id_t *job, uint64_t generation)
{
    app_state_t *state = context;
    if (state->original.capture_cancel)
        state->original.capture_cancel(state->original.policy_context, kind, job, generation);
}

static void publish(app_state_t *state)
{
    ae_snapshot_t view;
    ae_snapshot(state->engine, &view);
    portENTER_CRITICAL(&app.lock);
    bool open = !safety_pending() && !atomic_load(&app.shutting_down) &&
        view.playback.kind == AE_PLAYBACK_STREAM &&
        (view.playback.state == AE_JOB_STARTING || view.playback.state == AE_JOB_RUNNING);
    if (open && (!state->ingress_open || view.audio_generation != state->view.audio_generation ||
        view.playback.consumer_generation != state->view.playback.consumer_generation ||
        !ae_id_equal(&view.playback.job_id, &state->view.playback.job_id))) {
        ae_result_t result = ae_ring_init(&state->ingress, state->ingress_frames, INGRESS_FRAMES,
            &view.format, view.audio_generation, view.playback.consumer_generation, AE_FIRST_SEQUENCE, 0, &view.route_token);
        open = result == AE_OK;
    }
    if (!open) ae_ring_reset(&state->ingress);
    state->ingress_open = open;
    state->view = view;
    state->view_valid = true;
    portEXIT_CRITICAL(&app.lock);
}

static ae_result_t dispatch(app_state_t *state, const app_audio_command_t *command, app_audio_result_t *output)
{
    ae_snapshot_t view;
    ae_snapshot(state->engine, &view);
    if (command->kind != APP_AUDIO_CMD_START && command->kind != APP_AUDIO_CMD_STOP && command->kind != APP_AUDIO_CMD_RECOVER && command->kind < APP_AUDIO_CMD_RECORD_GET &&
        (command->expected_generation != view.audio_generation || command->expected_revision != view.revision)) return AE_STALE;
    uint64_t time = now_ms();
    switch (command->kind) {
    case APP_AUDIO_CMD_ROUTE: return ae_route_select(state->engine, command->route,
        command->expected_generation, command->expected_revision, time);
    case APP_AUDIO_CMD_ENABLE: return ae_enable(state->engine, command->role, command->value, &command->format, time);
    case APP_AUDIO_CMD_MUTE: return ae_mute(state->engine, command->role, command->value, time);
    case APP_AUDIO_CMD_VOLUME: return ae_volume(state->engine, command->level, time);
    case APP_AUDIO_CMD_GAIN: return ae_gain(state->engine, command->level, time);
    case APP_AUDIO_CMD_START: return ae_start(state->engine, &command->start, time, &output->job);
    case APP_AUDIO_CMD_STOP: return ae_stop(state->engine, &command->job_id,
        command->consumer_generation, command->stop_reason, time);
    case APP_AUDIO_CMD_RECOVER: return ae_recover(state->engine, time);
    case APP_AUDIO_CMD_RECORD_GET: return ae_record_get(state->engine, &command->principal_id, &command->object_id, &output->metadata, time);
    case APP_AUDIO_CMD_RECORD_LIST: return ae_record_list(state->engine, &command->principal_id, command->cursor,
        state->records, command->capacity, &output->count, &output->next_cursor, time);
    case APP_AUDIO_CMD_RECORD_READ: return ae_record_read(state->engine, &command->principal_id, &command->object_id,
        command->offset, state->record_data, command->capacity, &output->bytes, time);
    case APP_AUDIO_CMD_RECORD_DELETE: return ae_record_delete(state->engine, &command->principal_id, &command->object_id,
        command->object_revision, &output->access_revoked, &output->cleanup_complete, time);
    case APP_AUDIO_CMD_STORE_PROVISION:
        if (!state->environment.unlocked || state->environment.privacy_latched || state->environment.maintenance)
            return AE_PRIVACY;
        if (view.capture_indicator_required || view.playback_gate_open) return AE_BUSY;
        return state->original.store.provision ? state->original.store.provision(state->original.store.context,
            command->object_limit, command->quota_bytes) : AE_UNAVAILABLE;
    }
    return AE_INVALID;
}

static void release_worker(app_state_t *state, ae_result_t unavailable_result)
{
    if (state->original.codec.completion_notify)
        state->original.codec.completion_notify(state->original.codec.context, NULL, NULL);
    if (state->original.store.completion_notify)
        state->original.store.completion_notify(state->original.store.context, NULL, NULL);
    portENTER_CRITICAL(&app.lock);
    app.state = NULL;
    app.initializing = false;
    app.unavailable_result = unavailable_result;
    portEXIT_CRITICAL(&app.lock);
    ae_secure_zero(state->engine_memory, ae_engine_bytes());
    heap_caps_free(state->engine_memory);
    ae_secure_zero(state, sizeof(*state));
    heap_caps_free(state);
    vTaskDelete(NULL);
}

static void worker(void *context)
{
    app_state_t *state = context;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&app.lock);
    state->task = task;
    portEXIT_CRITICAL(&app.lock);
    if (state->original.codec.completion_notify)
        state->original.codec.completion_notify(state->original.codec.context, completion_wake, NULL);
    if (state->original.store.completion_notify)
        state->original.store.completion_notify(state->original.store.context, completion_wake, NULL);
    ae_dependencies_t dependencies = state->original;
    dependencies.policy_context = state;
    dependencies.authority = policy_authority;
    dependencies.random_id = policy_id;
    dependencies.trusted_unix_time = policy_time;
    dependencies.monotonic_ms = policy_clock;
    dependencies.safety_stop_requested = policy_stop;
    dependencies.capture_sink = state->original.capture_sink ? policy_sink : NULL;
    dependencies.capture_cancel = state->original.capture_cancel ? policy_cancel : NULL;
    ae_result_t initialized = ae_init(state->engine_memory, ae_engine_bytes(), &dependencies, now_ms(), &state->engine);
    if (!state->engine) {
        /* A rejected arena has no snapshot or hardware ownership. Keep it
         * unknown and preserve the exact rejection for command admission.
         * A returned engine, including STOPPING/FAULT, still owns recovery. */
        release_worker(state, initialized);
        return;
    }
    publish(state);
    for (;;) {
        unsigned sequence = atomic_load_explicit(&app.safety_sequence, memory_order_acquire);
        bool stopped = sequence == UINT_MAX || sequence != atomic_load(&app.handled_sequence);
        if (stopped) {
            ae_safety_stop(state->engine, (ae_stop_reason_t)atomic_load(&app.safety_reason), now_ms());
            atomic_store_explicit(&app.handled_sequence, sequence, memory_order_release);
        }
        ae_environment_t environment;
        app_audio_command_t command;
        bool pending;
        portENTER_CRITICAL(&app.lock);
        environment = state->environment;
        pending = state->command_pending && !state->result_pending;
        command = state->command;
        portEXIT_CRITICAL(&app.lock);
        ae_environment(state->engine, &environment, now_ms());
        if (pending) {
            ae_result_t result = stopped || safety_pending() ? AE_PRIVACY : dispatch(state, &command, &state->result);
            if (result == AE_AGAIN && now_ms() >= state->command_deadline) {
                ae_safety_stop(state->engine, AE_STOP_IO, now_ms());
                result = AE_TIMEOUT;
            }
            if (result != AE_AGAIN) {
                if (result != AE_OK && command.kind == APP_AUDIO_CMD_RECORD_READ && state->original.store.cancel_read)
                    state->original.store.cancel_read(state->original.store.context, &command.object_id);
                publish(state);
                portENTER_CRITICAL(&app.lock);
                if (safety_pending() || atomic_load(&app.shutting_down)) result=AE_PRIVACY;
                state->result.request_token = command.request_token; state->result.result = result;
                state->payload_token = command.request_token;
                state->payload_bytes = result == AE_OK ? state->result.bytes : 0;
                state->payload_count = result == AE_OK ? state->result.count : 0;
                if (result != AE_OK) { ae_secure_zero(state->record_data, sizeof(state->record_data));
                    ae_secure_zero(state->records, sizeof(state->records)); }
                state->command_pending = false;
                state->result_pending = true;
                ae_secure_zero(&state->command, sizeof(state->command));
                portEXIT_CRITICAL(&app.lock);
            }
        }
        /* A bounded completion pump retires and enqueues within one wake. A
         * single completion must not insert an extra RTOS tick per frame or
         * storage stage. Safety is rechecked inside each engine poll. */
        uint64_t budget_start = now_ms();
        for (unsigned pass = 0; pass < PROGRESS_PASSES; ++pass) {
            /* Make the authorized binding visible before route.prepare can start a
             * transport producer. No pre-start media queue is opened by a driver. */
            publish(state);
            /* Only this worker mutates the engine. Retain ingress ownership
             * while its downstream ring is full; polling may free a slot. */
            size_t engine_capacity = ae_playback_capacity(state->engine);
            portENTER_CRITICAL(&app.lock);
            ae_result_t frame_result = state->ingress_open && engine_capacity ?
                ae_ring_pop(&state->ingress, &state->frame) : AE_AGAIN;
            portEXIT_CRITICAL(&app.lock);
            if (frame_result == AE_OK) {
                (void)ae_submit_playback(state->engine, &state->frame, now_ms());
                ae_secure_zero(&state->frame, sizeof(state->frame));
            }
            (void)ae_poll(state->engine, now_ms());
            if (state->original.store.service)
                (void)state->original.store.service(state->original.store.context);
            publish(state);
            if (now_ms() - budget_start >= PROGRESS_BUDGET_MS) break;
        }
        ae_snapshot_t current;
        ae_snapshot(state->engine, &current);
        bool cleanup = current.playback.cleanup_pending;
        for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) cleanup |= current.capture[i].cleanup_pending;
        if (atomic_load(&app.shutting_down) && current.hardware_off_confirmed && !cleanup) {
            release_worker(state, AE_UNAVAILABLE);
            return;
        }
        (void)ulTaskNotifyTake(pdTRUE, 1);
    }
}

ae_result_t app_audio_init(const ae_dependencies_t *dependencies)
{
    if (!dependencies) return AE_INVALID;
    portENTER_CRITICAL(&app.lock);
    if (app.state || app.initializing) { portEXIT_CRITICAL(&app.lock); return AE_BUSY; }
    app.initializing = true;
    app.unavailable_result = AE_UNAVAILABLE;
    portEXIT_CRITICAL(&app.lock);
    app_state_t *state = heap_caps_aligned_calloc(_Alignof(app_state_t), 1, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *engine_memory = state ? heap_caps_aligned_calloc(ae_engine_alignment(), 1, ae_engine_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!state || !engine_memory) {
        if (state) heap_caps_free(state);
        portENTER_CRITICAL(&app.lock); app.initializing = false; app.unavailable_result = AE_EXHAUSTED; portEXIT_CRITICAL(&app.lock);
        return AE_EXHAUSTED;
    }
    state->original = *dependencies;
    state->engine_memory = engine_memory;
    atomic_store(&app.safety_sequence, 0);
    atomic_store(&app.handled_sequence, 0);
    atomic_store(&app.safety_reason, AE_STOP_PRIVACY);
    atomic_store(&app.shutting_down, false);
    portENTER_CRITICAL(&app.lock); app.state = state; portEXIT_CRITICAL(&app.lock);
    if (xTaskCreate(worker, "audio_manager", WORKER_STACK_BYTES, state, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&app.lock); app.state = NULL; app.initializing = false; app.unavailable_result = AE_EXHAUSTED; portEXIT_CRITICAL(&app.lock);
        heap_caps_free(engine_memory); heap_caps_free(state); return AE_EXHAUSTED;
    }
    return AE_OK;
}

ae_result_t app_audio_submit(const app_audio_command_t *command)
{
    if (!command || !command->request_token || command->kind < APP_AUDIO_CMD_ROUTE || command->kind > APP_AUDIO_CMD_STORE_PROVISION)
        return AE_INVALID;
    if (safety_pending() || atomic_load(&app.shutting_down)) return AE_PRIVACY;
    portENTER_CRITICAL(&app.lock);
    app_state_t *state = app.state;
    ae_result_t result = AE_OK;
    if (!state) result = app.unavailable_result;
    else if (!state->view_valid) result = AE_UNAVAILABLE;
    else if (state->command_pending || state->result_pending) result = AE_BUSY;
    else {
        if ((command->kind == APP_AUDIO_CMD_RECORD_READ && (!command->capacity || command->capacity > sizeof(state->record_data))) ||
            (command->kind == APP_AUDIO_CMD_RECORD_LIST && (!command->capacity || command->capacity > AE_LIST_MAX))) {
            portEXIT_CRITICAL(&app.lock); return AE_INVALID;
        }
        state->payload_token = state->payload_bytes = state->payload_count = 0;
        ae_secure_zero(state->record_data, sizeof(state->record_data));
        ae_secure_zero(state->records, sizeof(state->records));
        ae_secure_zero(&state->result, sizeof(state->result));
        state->command = *command;
        state->command_deadline = now_ms() + COMMAND_TIMEOUT_MS;
        state->command_pending = true;
        wake(state);
    }
    portEXIT_CRITICAL(&app.lock);
    return result;
}

bool app_audio_take_result(app_audio_result_t *output)
{
    if (!output) return false;
    portENTER_CRITICAL(&app.lock);
    bool ready = app.state && app.state->result_pending;
    if (ready) {
        *output = app.state->result;
        ae_secure_zero(&app.state->result, sizeof(app.state->result));
        app.state->result_pending = false;
    }
    portEXIT_CRITICAL(&app.lock);
    return ready;
}

void app_audio_request_safety_stop(ae_stop_reason_t reason)
{
    atomic_store(&app.safety_reason, reason);
    unsigned sequence = atomic_load(&app.safety_sequence);
    while (sequence != UINT_MAX && !atomic_compare_exchange_weak_explicit(&app.safety_sequence,
        &sequence, sequence + 1, memory_order_release, memory_order_relaxed)) { }
    portENTER_CRITICAL(&app.lock);
    if (app.state) {
        app.state->ingress_open=false; ae_ring_reset(&app.state->ingress);
        app.state->payload_token=app.state->payload_bytes=app.state->payload_count=0;
        wake(app.state);
    }
    portEXIT_CRITICAL(&app.lock);
}

void app_audio_set_environment(const ae_environment_t *environment)
{
    if (!environment) return;
    bool safety = !environment->unlocked || environment->privacy_latched || environment->maintenance;
    portENTER_CRITICAL(&app.lock);
    if (app.state) {
        app.state->environment = *environment;
        safety |= !environment->screen_visible && app.state->view.capture_indicator_required;
        safety |= !environment->indicator_visible && app.state->view.capture_gate_open;
    }
    portEXIT_CRITICAL(&app.lock);
    if (safety) app_audio_request_safety_stop(environment->maintenance ? AE_STOP_MAINTENANCE : AE_STOP_PRIVACY);
}

static ae_result_t submit_frame(ae_route_t route, const ae_frame_t *frame, bool transport, bool retry_full)
{
    if (!frame) return AE_INVALID;
    if (safety_pending() || atomic_load(&app.shutting_down)) return AE_PRIVACY;
    portENTER_CRITICAL(&app.lock);
    app_state_t *state = app.state;
    ae_result_t result = AE_STALE;
    if (state && state->ingress_open && (!transport || state->view.route == route)) {
        if (transport && ((state->original.route.bind_playback && !frame->consumer_generation) ||
            (frame->consumer_generation && frame->consumer_generation != state->view.playback.consumer_generation))) {
            portEXIT_CRITICAL(&app.lock); return AE_STALE;
        }
        if (transport) {
            /* Copy into the next ring slot through a small metadata override;
             * use the worker's PSRAM scratch only in the worker, never here. */
            state->ingress_bound = *frame;
            state->ingress_bound.consumer_generation = state->view.playback.consumer_generation;
            result = ae_ring_push(&state->ingress, &state->ingress_bound);
            ae_secure_zero(&state->ingress_bound, sizeof(state->ingress_bound));
        } else result = ae_ring_push(&state->ingress, frame);
        if (result == AE_OK) wake(state);
    }
    portEXIT_CRITICAL(&app.lock);
    if (result == AE_OVERFLOW && retry_full) return AE_AGAIN;
    if (result == AE_OVERFLOW) app_audio_request_safety_stop(AE_STOP_OVERFLOW);
    return result;
}

ae_result_t app_audio_submit_playback(const ae_frame_t *frame) { return submit_frame(AE_ROUTE_OFF, frame, false, false); }
ae_result_t app_audio_try_submit_playback(const ae_frame_t *frame)
{ return submit_frame(AE_ROUTE_OFF, frame, false, true); }
ae_result_t app_audio_submit_transport_playback(ae_route_t route, const ae_frame_t *frame)
{
    if (route <= AE_ROUTE_LOCAL || route >= AE_ROUTE_COUNT) return AE_INVALID;
    return submit_frame(route, frame, true, false);
}
size_t app_audio_transport_playback_capacity(ae_route_t route, uint64_t generation)
{
    portENTER_CRITICAL(&app.lock);
    const app_state_t *state = app.state;
    size_t capacity = state && state->ingress_open && !safety_pending() && !atomic_load(&app.shutting_down) &&
        route > AE_ROUTE_LOCAL && route < AE_ROUTE_COUNT && state->view.route == route &&
        state->view.audio_generation == generation ? state->ingress.capacity - state->ingress.count : 0;
    portEXIT_CRITICAL(&app.lock);
    return capacity;
}
bool app_audio_snapshot(ae_snapshot_t *output)
{
    if (!output) return false;
    portENTER_CRITICAL(&app.lock);
    bool ready = app.state && app.state->view_valid;
    if (ready) *output = app.state->view;
    portEXIT_CRITICAL(&app.lock);
    return ready;
}
ae_result_t app_audio_shutdown(void)
{
    portENTER_CRITICAL(&app.lock);
    bool active = app.state != NULL;
    portEXIT_CRITICAL(&app.lock);
    if (!active) return AE_OK;
    atomic_store(&app.shutting_down, true);
    app_audio_request_safety_stop(AE_STOP_REQUESTED);
    return AE_AGAIN;
}

ae_result_t app_audio_copy_record_data(uint64_t token, size_t offset, void *output, size_t bytes)
{
    if (!output || !bytes) return AE_INVALID;
    portENTER_CRITICAL(&app.lock);
    app_state_t *s = app.state;
    ae_result_t r = safety_pending() || atomic_load(&app.shutting_down) || (s && (!s->environment.unlocked ||
        s->environment.privacy_latched || s->environment.maintenance)) ? AE_PRIVACY : !s || s->payload_token != token ? AE_STALE :
        offset > s->payload_bytes || bytes > s->payload_bytes - offset ? AE_INVALID : AE_OK;
    if (r == AE_OK) memcpy(output, s->record_data + offset, bytes);
    else ae_secure_zero(output,bytes);
    portEXIT_CRITICAL(&app.lock); return r;
}
ae_result_t app_audio_copy_record_list(uint64_t token, ae_record_metadata_t *output, size_t capacity)
{
    if (!output || capacity > AE_LIST_MAX) return AE_INVALID;
    portENTER_CRITICAL(&app.lock);
    app_state_t *s = app.state;
    ae_result_t r = safety_pending() || atomic_load(&app.shutting_down) || (s && (!s->environment.unlocked ||
        s->environment.privacy_latched || s->environment.maintenance)) ? AE_PRIVACY :
        !s || s->payload_token != token ? AE_STALE : capacity < s->payload_count ? AE_INVALID : AE_OK;
    if (r == AE_OK) memcpy(output, s->records, s->payload_count * sizeof(*output));
    else ae_secure_zero(output,capacity*sizeof(*output));
    portEXIT_CRITICAL(&app.lock); return r;
}
