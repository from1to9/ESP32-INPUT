// SPDX-License-Identifier: Apache-2.0
#include "app_local_speaker_test.h"
#include "board_audio_mosaico.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

enum { FRAME_BYTES = 960, TONE_FRAMES = 300, START_MS = 2000,
    WRITE_MS = 100, PLAY_MS = 3000, CLOSE_MS = 500, VOLUME = 80 };
typedef enum { READY, PREPARE, VOLUME_SET, UNMUTE, WRITE, MUTE, STOP } phase_t;
typedef struct {
    bool called, clock_known, clock_failed, attempted, stop_confirmed, handover;
    uint64_t last_ms;
    uint32_t produced_bytes, submitted_bytes, accepted_bytes, write_calls;
    ae_result_t result, mute_result, stop_result;
} test_result_t;
static test_result_t result;
static uint8_t frame[FRAME_BYTES];
static atomic_bool cancelled;
static portMUX_TYPE wake_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t waiter;
static const ae_format_t format = {48000, 1, 10, AE_PCM_S16_LE};
/* Exactly 48 samples/cycle at 48 kHz: 1 kHz, peak 4096/32768 = 0.125 FS. */
static const int16_t wave[48] = {
    0,535,1060,1567,2048,2494,2896,3250,3547,3784,3956,4061,
    4096,4061,3956,3784,3547,3250,2896,2494,2048,1567,1060,535,
    0,-535,-1060,-1567,-2048,-2494,-2896,-3250,-3547,-3784,-3956,-4061,
    -4096,-4061,-3956,-3784,-3547,-3250,-2896,-2494,-2048,-1567,-1060,-535
};

static void wake(void *unused)
{
    (void)unused;
    /* A copied late callback cannot notify a deleted boot task: publication,
     * notification and clearing share this lock. No driver pointer is kept. */
    portENTER_CRITICAL(&wake_lock);
    if (waiter) xTaskNotifyGive(waiter);
    portEXIT_CRITICAL(&wake_lock);
}
void app_local_speaker_test_cancel(void)
{
    atomic_store(&cancelled, true);
    wake(NULL);
}
static bool clock_ms(uint64_t *now)
{
    int64_t raw = esp_timer_get_time();
    uint64_t value = raw >= 0 ? (uint64_t)raw / 1000U : 0;
    if (raw < 0 || result.clock_failed || (result.clock_known && value < result.last_ms)) {
        result.clock_failed = true; return false;
    }
    result.clock_known = true; result.last_ms = value; *now = value; return true;
}
static ae_result_t perform(const ae_codec_port_t *port, phase_t phase, uint64_t deadline)
{
    bool submitted = false;
    for (;;) {
        uint64_t now;
        const bool closing = phase == MUTE || phase == STOP;
        if (!closing) {
            if (atomic_load(&cancelled)) return AE_PRIVACY;
            if (!clock_ms(&now)) return AE_TIME_UNKNOWN;
            if (now >= deadline) return AE_TIMEOUT;
        }
        ae_result_t r;
        switch (phase) {
        case READY: {
            board_audio_status_t status = board_audio_mosaico_status();
            r = status.probe_complete ? status.probe_result : AE_AGAIN;
            break;
        }
        case PREPARE: r = port->prepare(port->context, &format, AE_SPEAKER, 1); break;
        case VOLUME_SET: r = port->volume(port->context, VOLUME, 1); break;
        case UNMUTE: r = port->mute(port->context, AE_SPEAKER, false, 1); break;
        case MUTE: r = port->mute(port->context, AE_SPEAKER, true, 1); break;
        case STOP: r = port->stop(port->context, 1); break;
        case WRITE: {
            size_t actual = 0;
            if (!submitted) { result.submitted_bytes += FRAME_BYTES; submitted = true; }
            ++result.write_calls;
            r = port->write(port->context, 1, frame, sizeof(frame), &actual);
            if (actual > sizeof(frame) || (actual & 1U)) return AE_INTEGRITY;
            result.accepted_bytes += (uint32_t)actual;
            if (r == AE_AGAIN && actual) return AE_INTEGRITY;
            if (r == AE_OK && actual != sizeof(frame)) return AE_IO_ERROR;
            break;
        }
        default: return AE_INVALID;
        }
        if (r != AE_AGAIN) return r;
        if (!clock_ms(&now)) return AE_TIME_UNKNOWN;
        if (now >= deadline) return AE_TIMEOUT;
        TickType_t tick = pdMS_TO_TICKS(1);
        (void)ulTaskNotifyTake(pdTRUE, tick ? tick : 1);
    }
}

bool app_local_speaker_test_run(bool physically_locked)
{
    if (result.called) return result.handover;
    result.called = true; result.result = AE_PRIVACY;
    result.mute_result = result.stop_result = AE_UNAVAILABLE;
    if (physically_locked || atomic_load(&cancelled)) {
        result.handover = true;
        ESP_LOGI("local_speaker", "Local speaker test skipped: physical lock/button; codec untouched");
        return true;
    }
    uint64_t now = 0;
    result.result = clock_ms(&now) ? board_audio_mosaico_init() : AE_TIME_UNKNOWN;
    ae_codec_port_t port = board_audio_mosaico_port();
    if (result.result != AE_OK) goto report;
    result.attempted = true;
    portENTER_CRITICAL(&wake_lock); waiter = xTaskGetCurrentTaskHandle(); portEXIT_CRITICAL(&wake_lock);
    port.completion_notify(port.context, wake, NULL);
    const uint64_t start_deadline = now + START_MS;
    result.result = perform(&port, READY, start_deadline);
    if (result.result == AE_OK) result.result = perform(&port, PREPARE, start_deadline);
    if (result.result == AE_OK) result.result = perform(&port, VOLUME_SET, start_deadline);
    uint64_t play_deadline = 0;
    if (result.result == AE_OK && !clock_ms(&now)) result.result = AE_TIME_UNKNOWN;
    if (result.result == AE_OK) {
        play_deadline = now + PLAY_MS;
        ESP_LOGI("local_speaker", "Local speaker tone about to start: 1000 Hz, mono48000 PCM16LE, 3 seconds, peak4096, volume80; no microphone");
        result.result = perform(&port, UNMUTE, start_deadline);
    }
    if (result.result == AE_OK && !clock_ms(&now)) result.result = AE_TIME_UNKNOWN;
    if (result.result == AE_OK) {
        for (unsigned n = 0; n < TONE_FRAMES && result.result == AE_OK; ++n) {
            for (unsigned i = 0; i < FRAME_BYTES / 2; ++i) {
                uint16_t sample = (uint16_t)wave[i % 48];
                frame[i * 2] = (uint8_t)sample; frame[i * 2 + 1] = (uint8_t)(sample >> 8);
            }
            result.produced_bytes += FRAME_BYTES;
            if (!clock_ms(&now)) { result.result = AE_TIME_UNKNOWN; break; }
            uint64_t deadline = now + WRITE_MS;
            if (deadline > play_deadline) deadline = play_deadline;
            result.result = perform(&port, WRITE, deadline);
        }
    }
    /* Safety operations are attempted even with cancelled/unknown time. A
     * missing clock permits one request, never an unbounded completion wait. */
    now = result.last_ms;
    result.mute_result = perform(&port, MUTE, now + CLOSE_MS);
    now = result.last_ms;
    result.stop_result = perform(&port, STOP, now + CLOSE_MS);
    {
        board_audio_status_t status = board_audio_mosaico_status();
        result.stop_confirmed = result.stop_result == AE_OK && status.probe_complete &&
            !status.prepared && !status.capture_running && !status.playback_running &&
            !status.tx_clock_running && status.speaker_mute_confirmed && status.speaker_muted && !status.cleanup_uncertain;
    }
    portENTER_CRITICAL(&wake_lock); waiter = NULL; portEXIT_CRITICAL(&wake_lock);
    port.completion_notify(port.context, NULL, NULL);
    result.handover = result.stop_confirmed;
report:
    memset(frame, 0, sizeof(frame));
    {
        board_audio_status_t status = board_audio_mosaico_status();
        ESP_LOGI("local_speaker", "Local speaker test result=%d attempted=%d produced_bytes=%" PRIu32
            " submitted_bytes=%" PRIu32 " accepted_bytes=%" PRIu32 " write_calls=%" PRIu32
            " mute_result=%d stop_result=%d stop_confirmed=%d handover=%d sdk_error=%d sdk_operation=%s acoustic=unverified",
            (int)result.result, result.attempted, result.produced_bytes, result.submitted_bytes,
            result.accepted_bytes, result.write_calls, (int)result.mute_result, (int)result.stop_result,
            result.stop_confirmed, result.handover, status.last_sdk_error,
            board_audio_sdk_operation_name(status.last_sdk_operation));
    }
    return result.handover;
}
