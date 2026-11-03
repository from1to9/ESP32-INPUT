// SPDX-License-Identifier: Apache-2.0
/* Coordinator-owned observation only. One record per observed transition, not
 * one per log interval. Durations cover sampled states, not unsampled hardware.
 * The ring and formatting workspace live in PSRAM, never on the task stack. */
#define UPDATE_HEALTH_TRACE_CAPACITY 32U
#define UPDATE_HEALTH_REPORT_MS 1000U
#define UPDATE_HEALTH_REPORT_EVENTS 2U

typedef enum {
    UPDATE_HEALTH_BEGIN, UPDATE_HEALTH_CHANGE, UPDATE_HEALTH_CLOCK,
    UPDATE_HEALTH_REQUEST, UPDATE_HEALTH_END
} update_health_event_kind_t;
typedef struct {
    uint64_t at_ms, missing, previous, state_ms, reset_ms;
    update_health_event_kind_t kind;
    esp_err_t request_error;
    bool duration_known, reset;
} update_health_event_t;
static EXT_RAM_BSS_ATTR struct {
    update_health_event_t events[UPDATE_HEALTH_TRACE_CAPACITY];
    size_t head, count;
    bool active, reported, request_seen;
    uint64_t missing, state_since_ms, last_sample_ms, last_report_ms, max_gap_ms;
    uint64_t resets, dropped, dropped_resets, dropped_missing, dropped_max_reset_ms, dropped_clock_back;
    esp_err_t request_error;
} s_update_health_trace;

static void remote_update_health_record(update_health_event_kind_t kind, uint64_t missing,
                                        uint64_t previous_since, esp_err_t error)
{
    const uint64_t now = s_now_ms;
    bool reset = previous_since && !s_update_health_since;
    uint64_t reset_ms = reset && now >= previous_since ? now - previous_since : 0;
    if (s_update_health_trace.count == UPDATE_HEALTH_TRACE_CAPACITY) {
        /* Preserve the queued chronology. Explicit loss counters retain cause
         * coverage even when changes arrive faster than the serial budget. */
        ++s_update_health_trace.dropped;
        s_update_health_trace.dropped_missing |= missing;
        if (reset) ++s_update_health_trace.dropped_resets;
        if (kind == UPDATE_HEALTH_CLOCK) ++s_update_health_trace.dropped_clock_back;
        if (reset_ms > s_update_health_trace.dropped_max_reset_ms)
            s_update_health_trace.dropped_max_reset_ms = reset_ms;
        return;
    }
    size_t tail = (s_update_health_trace.head + s_update_health_trace.count) % UPDATE_HEALTH_TRACE_CAPACITY;
    s_update_health_trace.events[tail] = (update_health_event_t){
        .at_ms = now, .missing = missing, .previous = s_update_health_trace.missing,
        .state_ms = now >= s_update_health_trace.state_since_ms ?
            now - s_update_health_trace.state_since_ms : 0,
        .reset_ms = reset_ms, .kind = kind, .request_error = error,
        .duration_known = kind != UPDATE_HEALTH_CLOCK && now >= s_update_health_trace.state_since_ms,
        .reset = reset
    };
    ++s_update_health_trace.count;
}

static void remote_update_health_observe(uint64_t missing, uint64_t previous_since,
                                         bool requested, esp_err_t error)
{
    if (!s_update_health_trace.active) {
        s_update_health_trace.active = true;
        s_update_health_trace.request_seen = false;
        s_update_health_trace.state_since_ms = s_now_ms;
        s_update_health_trace.missing = missing;
        s_update_health_trace.last_sample_ms = s_now_ms;
        remote_update_health_record(UPDATE_HEALTH_BEGIN, missing, 0, ESP_OK);
    } else {
        bool clock_back = s_now_ms < s_update_health_trace.last_sample_ms;
        uint64_t gap = clock_back ? 0 : s_now_ms - s_update_health_trace.last_sample_ms;
        if (gap > s_update_health_trace.max_gap_ms) s_update_health_trace.max_gap_ms = gap;
        if (previous_since && !s_update_health_since) ++s_update_health_trace.resets;
        if (clock_back || missing != s_update_health_trace.missing) {
            remote_update_health_record(clock_back ? UPDATE_HEALTH_CLOCK : UPDATE_HEALTH_CHANGE,
                                        missing, previous_since, ESP_OK);
            s_update_health_trace.state_since_ms = s_now_ms;
            s_update_health_trace.missing = missing;
        }
        s_update_health_trace.last_sample_ms = s_now_ms;
    }
    if (requested && (!s_update_health_trace.request_seen || s_update_health_trace.request_error != error)) {
        remote_update_health_record(UPDATE_HEALTH_REQUEST, missing, 0, error);
        s_update_health_trace.request_seen = true;
        s_update_health_trace.request_error = error;
    }
}

/* Leaf names match the owning acceptance predicate. Unknown media
 * snapshots/audio are reported at their owning gate; their contents are not
 * treated as observed failures. IMU gates follow presence/successful init, not
 * transient availability. The stable imu_fault bit means current phase or a
 * newly observed recovery boundary; historical fault_latched stays diagnostic. */
#define UPDATE_HEALTH_GATES(X) \
    X(display, capabilities.display) \
    X(touch, capabilities.touch) \
    X(lock_button, capabilities.lock_button) \
    X(multitouch, s_multitouch_available) \
    X(ui_initialized, ui.initialized) \
    X(ui_init_error, ui.init_error == ESP_OK) \
    X(ui_applied, ui.has_applied) \
    X(ui_apply_age, !ui.has_applied || ui.apply_age_ms <= 500) \
    X(renderer_bound, renderer.bound) \
    X(renderer_alive, renderer.owner_alive) \
    X(renderer_error, renderer.error == ESP_OK) \
    X(display_handle, display_present) \
    X(settings_available, settings.available) \
    X(settings_pending, !settings.pending) \
    X(settings_error, settings.error == ESP_OK) \
    X(settings_valid, settings_valid) \
    X(imu_initialized, !imu_required || imu.initialization_succeeded) \
    X(imu_started, !imu_required || imu.start_succeeded) \
    X(imu_worker, !imu_required || imu.worker_running) \
    X(imu_sample_valid, !imu_required || imu.sample_valid) \
    X(imu_sample_fresh, !imu_required || imu.sample_fresh) \
    X(imu_fault, !imu_required || imu_stable) \
    X(imu_error, !imu_required || imu.error == ESP_OK) \
    X(ble_initialized, bluetooth.initialized) \
    X(ble_failed, !bluetooth.failed) \
    X(usb_initialized, usb.initialized) \
    X(usb_failed, !usb.failed) \
    X(media_known, media_known) \
    X(media_initialized, !media_known || media.initialized) \
    X(audio_known, !media_known || media.audio_known) \
    X(media_busy, !media_known || !media.busy) \
    X(audio_off, !audio_known || media.audio.hardware_off_confirmed) \
    X(capture_gate, !audio_known || !media.audio.capture_gate_open) \
    X(playback_gate, !audio_known || !media.audio.playback_gate_open) \
    X(microphone, !audio_known || !media.audio.microphone_enabled) \
    X(speaker, !audio_known || !media.audio.speaker_enabled) \
    X(capture_indicator, !audio_known || !media.audio.capture_indicator_required) \
    X(audio_quarantined, !audio_known || !media.audio.quarantined) \
    X(capture0_cleanup, !audio_known || !media.audio.capture[0].cleanup_pending) \
    X(capture0_idle, !audio_known || remote_update_health_job_idle(media.audio.capture[0].state)) \
    X(capture1_cleanup, !audio_known || !media.audio.capture[1].cleanup_pending) \
    X(capture1_idle, !audio_known || remote_update_health_job_idle(media.audio.capture[1].state)) \
    X(capture2_cleanup, !audio_known || !media.audio.capture[2].cleanup_pending) \
    X(capture2_idle, !audio_known || remote_update_health_job_idle(media.audio.capture[2].state)) \
    X(playback_cleanup, !audio_known || !media.audio.playback.cleanup_pending) \
    X(playback_idle, !audio_known || remote_update_health_job_idle(media.audio.playback.state))

#define UPDATE_HEALTH_ENUM(name, condition) UPDATE_HEALTH_GATE_##name,
enum { UPDATE_HEALTH_GATES(UPDATE_HEALTH_ENUM) UPDATE_HEALTH_GATE_COUNT };
#undef UPDATE_HEALTH_ENUM
_Static_assert(UPDATE_HEALTH_GATE_COUNT <= 64, "Health gates must fit the trace mask");
_Static_assert(AE_CONSUMERS_MAX == 3, "Extend capture health diagnostics with the acceptance gate");
#define UPDATE_HEALTH_NAME(name, condition) #name,
static const char *const s_update_health_gate_names[] = {UPDATE_HEALTH_GATES(UPDATE_HEALTH_NAME)};
#undef UPDATE_HEALTH_NAME
#define UPDATE_HEALTH_NAME_SIZE(name, condition) + sizeof(#name)
static EXT_RAM_BSS_ATTR char s_update_health_gate_text[1 UPDATE_HEALTH_GATES(UPDATE_HEALTH_NAME_SIZE)];
#undef UPDATE_HEALTH_NAME_SIZE

static bool remote_update_health_job_idle(ae_job_state_t state)
{
    return state != AE_JOB_STARTING && state != AE_JOB_RUNNING && state != AE_JOB_STOPPING &&
           state != AE_JOB_FINALIZING;
}

static const char *remote_update_health_gate_text(uint64_t missing)
{
    size_t used = 0;
    for (unsigned bit = 0; bit < UPDATE_HEALTH_GATE_COUNT; ++bit) {
        if (!(missing & (UINT64_C(1) << bit))) continue;
        if (used) s_update_health_gate_text[used++] = '|';
        size_t length = strlen(s_update_health_gate_names[bit]);
        memcpy(s_update_health_gate_text + used, s_update_health_gate_names[bit], length);
        used += length;
    }
    s_update_health_gate_text[used] = 0;
    return used ? s_update_health_gate_text : "none";
}

static void __attribute__((noinline)) remote_update_health_report(app_update_state_t state)
{
    if (state != APP_UPDATE_PENDING_HEALTH && s_update_health_trace.active) {
        remote_update_health_record(UPDATE_HEALTH_END, s_update_health_trace.missing, 0, ESP_OK);
        s_update_health_trace.active = false;
    }
    if (!s_update_health_trace.active && !s_update_health_trace.count) return;
    /* A backward clock never grants an extra serial burst. Reporting resumes
     * once the previous reporting timestamp plus the interval is reached. */
    if (s_update_health_trace.reported && (s_now_ms < s_update_health_trace.last_report_ms ||
        s_now_ms - s_update_health_trace.last_report_ms < UPDATE_HEALTH_REPORT_MS)) return;
    s_update_health_trace.reported = true;
    s_update_health_trace.last_report_ms = s_now_ms;
    uint64_t sampled_ms = s_update_health_trace.active ? s_now_ms : s_update_health_trace.last_sample_ms;
    ESP_LOGI(TAG, "OTAHealth now_ms=%llu state=%u active=%d sample_ms=%llu missing=0x%llx state_ms=%llu healthy_ms=%llu "
        "resets=%llu max_sample_gap_ms=%llu queued=%u dropped=%llu dropped_resets=%llu "
        "dropped_missing=0x%llx dropped_max_reset_ms=%llu dropped_clock_back=%llu dropped_gates=%s",
        (unsigned long long)s_now_ms, (unsigned)state, s_update_health_trace.active,
        (unsigned long long)s_update_health_trace.last_sample_ms,
        (unsigned long long)s_update_health_trace.missing,
        (unsigned long long)(sampled_ms >= s_update_health_trace.state_since_ms ?
            sampled_ms - s_update_health_trace.state_since_ms : 0),
        (unsigned long long)(s_update_health_since && s_now_ms >= s_update_health_since ?
            s_now_ms - s_update_health_since : 0),
        (unsigned long long)s_update_health_trace.resets,
        (unsigned long long)s_update_health_trace.max_gap_ms, (unsigned)s_update_health_trace.count,
        (unsigned long long)s_update_health_trace.dropped,
        (unsigned long long)s_update_health_trace.dropped_resets,
        (unsigned long long)s_update_health_trace.dropped_missing,
        (unsigned long long)s_update_health_trace.dropped_max_reset_ms,
        (unsigned long long)s_update_health_trace.dropped_clock_back,
        remote_update_health_gate_text(s_update_health_trace.dropped_missing));
    static const char *const kinds[] = {"begin", "change", "clock_back", "request", "end"};
    for (unsigned emitted = 0; emitted < UPDATE_HEALTH_REPORT_EVENTS && s_update_health_trace.count; ++emitted) {
        const update_health_event_t *event = &s_update_health_trace.events[s_update_health_trace.head];
        ESP_LOGI(TAG, "OTAHealthEvent at_ms=%llu kind=%s previous=0x%llx missing=0x%llx "
            "reset=%d duration_known=%d previous_state_ms=%llu reset_ms=%llu request_error=%d gates=%s",
            (unsigned long long)event->at_ms, kinds[event->kind],
            (unsigned long long)event->previous, (unsigned long long)event->missing,
            event->reset, event->duration_known,
            (unsigned long long)event->state_ms, (unsigned long long)event->reset_ms,
            (int)event->request_error, remote_update_health_gate_text(event->missing));
        s_update_health_trace.head = (s_update_health_trace.head + 1U) % UPDATE_HEALTH_TRACE_CAPACITY;
        --s_update_health_trace.count;
    }
}
