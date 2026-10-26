// SPDX-License-Identifier: Apache-2.0
#include "usb_audio.h"
#include "usb_audio_descriptors.h"
#include "usb_audio_transport.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include <limits.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define USB_AUDIO_OBSERVATION_BSS EXT_RAM_BSS_ATTR
#else
#define USB_AUDIO_OBSERVATION_BSS
#endif

#if CFG_TUD_AUDIO != 1
#error "The composite stack must enable exactly one UAC2 function"
#endif

enum { UAC_FRAME_BYTES = 960, UAC_QUEUE_FRAMES = 4, UAC_FRAME_AGE_US = 100000, UAC_TX_WRITES_PER_PASS = 4 };
typedef struct {
    ae_frame_t frame;
    uint64_t admitted_us;
    uint16_t offset;
} queued_frame_t;
typedef struct {
    queued_frame_t tx[UAC_QUEUE_FRAMES];
    queued_frame_t rx[UAC_QUEUE_FRAMES];
    uint8_t input[UAC_FRAME_BYTES];
    uint8_t output[98];
} uac_memory_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_audio_snapshot_t s_status;
static usb_audio_transport_snapshot_t s_transport;
static uac_memory_t *s_memory;
static TaskHandle_t s_worker;
static bool s_attempted;
static ae_route_token_t s_token, s_prepared_token, s_stop_token;
static ae_id_t s_boot_id;
static uint64_t s_flush_requested, s_flush_completed, s_stop_generation;
static uint64_t s_rx_sequence, s_tx_sequence, s_sample_offset;
static uint64_t s_input_started_us;
static uint64_t s_consumer_generation, s_rx_flush_requested, s_rx_flush_completed;
static uint8_t s_roles;
static unsigned s_tx_head, s_tx_count, s_rx_head, s_rx_count, s_input_bytes;
static bool s_tx_seen;
static uint64_t s_played_samples;
static int64_t s_feedback_level_q16;
/* Task-only metadata; lifecycle/ISR state remains in the existing internal
 * storage. Neither observation nor copying reserves internal DMA payloads. */
static USB_AUDIO_OBSERVATION_BSS usb_audio_pcm_snapshot_t s_pcm;

static const ae_format_t s_format = {
    .sample_rate_hz = INPUT_USB_AUDIO_RATE, .channels = 1,
    .frame_ms = 10, .encoding = AE_PCM_S16_LE,
};

static bool same_token(const ae_route_token_t *a, const ae_route_token_t *b)
{
    return a && b && a->link_generation == b->link_generation &&
        memcmp(a->peer_id.bytes, b->peer_id.bytes, AE_ID_BYTES) == 0 &&
        memcmp(a->stream_id.bytes, b->stream_id.bytes, AE_ID_BYTES) == 0;
}

static bool same_format(const ae_format_t *format)
{
    return format && format->sample_rate_hz == s_format.sample_rate_hz &&
        format->channels == 1 && format->frame_ms == 10 && format->encoding == AE_PCM_S16_LE;
}

static void count(uint64_t *value)
{
    if (*value != UINT64_MAX) ++*value;
}
static void add_count(uint64_t *value, uint16_t amount)
{
    *value = *value > UINT64_MAX - amount ? UINT64_MAX : *value + amount;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t bytes, uint8_t function, uint8_t endpoint, uint8_t alternate)
{
    (void)rhport; (void)alternate;
    if (function != 0 || endpoint != 0x04) return true;
    portENTER_CRITICAL_ISR(&s_lock);
    count(&s_status.out_callbacks); add_count(&s_status.out_callback_bytes, bytes);
    if (!bytes) count(&s_status.out_zero_callbacks);
    portEXIT_CRITICAL_ISR(&s_lock);
    return true;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t bytes, uint8_t function, uint8_t endpoint, uint8_t alternate)
{
    (void)rhport; (void)alternate;
    if (function != 0 || endpoint != 0x84) return true;
    portENTER_CRITICAL_ISR(&s_lock);
    count(&s_status.in_callbacks); add_count(&s_status.in_callback_bytes, bytes);
    if (!bytes) count(&s_status.in_zero_callbacks);
    portEXIT_CRITICAL_ISR(&s_lock);
    return true;
}

static void request_flush_locked(void)
{
    s_status.prepared = false;
    s_consumer_generation = 0;
    s_tx_count = s_rx_count = s_input_bytes = 0;
    s_tx_head = s_rx_head = 0;
    s_tx_seen = false;
    s_rx_sequence = s_sample_offset = s_played_samples = 0;
    s_feedback_level_q16 = INT64_C(960) << 16;
    if (s_flush_requested == UINT64_MAX) {
        s_status.error = ESP_ERR_INVALID_STATE;
        s_status.stack_ready = false;
        return;
    }
    ++s_flush_requested;
    s_status.cleanup_pending = true;
}

static void replace_link_locked(bool new_attachment)
{
    request_flush_locked();
    s_roles = 0;
    if (s_status.link_generation == UINT64_MAX) {
        s_status.error = ESP_ERR_INVALID_STATE;
        s_status.stack_ready = false;
        return;
    }
    ++s_status.link_generation;
    s_token.link_generation = s_status.link_generation;
    /* USB has no authenticated host name/address. These opaque IDs identify
     * this observed attachment/stream only, and change across reconnection. */
    if (new_attachment) {
        s_token.peer_id = s_boot_id;
        for (unsigned i = 0; i < 8; ++i)
            s_token.peer_id.bytes[i] ^= (uint8_t)(s_status.link_generation >> (i * 8U));
        s_token.peer_id.bytes[15] = 1;
    }
    s_token.stream_id = s_boot_id;
    for (unsigned i = 0; i < 8; ++i)
        s_token.stream_id.bytes[i + 8] ^= (uint8_t)(s_status.link_generation >> (i * 8U));
    s_token.stream_id.bytes[0] = 2;
}

static bool live_locked(void)
{
    return s_status.initialized && s_status.stack_ready && s_status.mounted &&
        !s_status.suspended && s_status.error == ESP_OK &&
        (!INPUT_USB_AUDIO_PTI_REQUIRED || (s_transport.mode_known && s_transport.mode_valid));
}

void input_usb_audio_dcd_mode(uint32_t epoch, bool known, uint32_t reject, bool in_isr)
{
    if (in_isr) portENTER_CRITICAL_ISR(&s_lock); else portENTER_CRITICAL(&s_lock);
    /* Reset can precede the deferred USB attachment callback. Revoke authority
     * at this owning lifecycle boundary so old FIFO/consumer state cannot revive. */
    const bool new_epoch = epoch != s_transport.reset_epoch && s_transport.reset_epoch;
    const bool lost_mode = s_transport.mode_valid && (!known || !epoch || reject);
    if (INPUT_USB_AUDIO_PTI_REQUIRED && (new_epoch || lost_mode) && s_status.initialized)
        replace_link_locked(false);
    s_transport.observed = true; s_transport.mode_required = INPUT_USB_AUDIO_PTI_REQUIRED;
    s_transport.reset_epoch = epoch; s_transport.mode_known = known;
    s_transport.mode_reject = reject;
    s_transport.mode_valid = known && epoch && !reject;
    if (known && reject && !s_transport.first_mode_reject) {
        s_transport.first_mode_reject = reject; s_transport.first_mode_reject_epoch = epoch;
    }
    if (in_isr) portEXIT_CRITICAL_ISR(&s_lock); else portEXIT_CRITICAL(&s_lock);
}

void input_usb_audio_dcd_transfer(uint32_t epoch, uint8_t endpoint, bool completion,
    uint8_t result, uint16_t bytes, uint32_t feedback_value, uint8_t feedback_length, bool in_isr)
{
    usb_audio_endpoint_observation_t *counter = endpoint == 0x04 ? &s_transport.out :
        endpoint == 0x84 ? &s_transport.in : endpoint == 0x85 ? &s_transport.feedback : NULL;
    if (!counter) return;
    if (in_isr) portENTER_CRITICAL_ISR(&s_lock); else portENTER_CRITICAL(&s_lock);
    s_transport.observed = true;
    if (completion) {
        if (result == XFER_RESULT_SUCCESS) {
            count(&counter->completed_ok); add_count(&counter->successful_bytes, bytes);
            if (!bytes) count(&counter->successful_zero);
        } else count(&counter->completed_failed);
    } else {
        count(&counter->submitted);
        if (result == XFER_RESULT_SUCCESS) count(&counter->accepted); else count(&counter->rejected);
    }
    if (result != XFER_RESULT_SUCCESS && !counter->first_failure_known) {
        counter->first_failure_known = true; counter->first_failure_epoch = epoch;
        counter->first_failure_result = result;
    }
    if (endpoint == 0x85 && result == XFER_RESULT_SUCCESS && (feedback_length == 3 || feedback_length == 4)) {
        if (completion) {
            s_transport.feedback_completed_known = true; s_transport.feedback_completed = feedback_value;
            s_transport.feedback_completed_length = feedback_length; s_transport.feedback_completed_epoch = epoch;
        } else {
            s_transport.feedback_submitted_known = true; s_transport.feedback_submitted = feedback_value;
            s_transport.feedback_submitted_length = feedback_length; s_transport.feedback_submitted_epoch = epoch;
        }
    }
    if (in_isr) portEXIT_CRITICAL_ISR(&s_lock); else portEXIT_CRITICAL(&s_lock);
}

void input_usb_audio_dcd_registers(const usb_audio_dcd_registers_t *registers, bool in_isr)
{
    if (!registers) return;
    if (in_isr) portENTER_CRITICAL_ISR(&s_lock); else portENTER_CRITICAL(&s_lock);
    s_transport.registers = *registers; s_transport.registers_known = true;
    if (registers->point == USB_AUDIO_REG_PENDING_IRQ && (!s_transport.pending_registers_known ||
        s_transport.pending_registers.epoch_before != registers->epoch_before ||
        s_transport.pending_registers.out_activation != registers->out_activation)) {
        s_transport.pending_registers = *registers; s_transport.pending_registers_known = true;
    }
    if (in_isr) portEXIT_CRITICAL_ISR(&s_lock); else portEXIT_CRITICAL(&s_lock);
}

void input_usb_audio_class_out(bool rearm, bool accepted, uint8_t result, uint16_t bytes)
{
    (void)result; (void)bytes;
    portENTER_CRITICAL_ISR(&s_lock);
    if (rearm) { count(&s_transport.class_out_rearm_calls); if (!accepted) count(&s_transport.class_out_rearm_rejected); }
    else count(&s_transport.class_out_entries);
    portEXIT_CRITICAL_ISR(&s_lock);
}

void usb_audio_transport_snapshot(usb_audio_transport_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock); *out = s_transport; out->mode_required = INPUT_USB_AUDIO_PTI_REQUIRED;
    portEXIT_CRITICAL(&s_lock);
}

void usb_audio_stack_started(esp_err_t result)
{
    portENTER_CRITICAL(&s_lock);
    s_status.stack_ready = result == ESP_OK && s_status.initialized;
    if (result != ESP_OK) s_status.error = result;
    request_flush_locked();
    portEXIT_CRITICAL(&s_lock);
}

void usb_audio_stack_event(bool mounted, bool suspended)
{
    portENTER_CRITICAL(&s_lock);
    if (mounted != s_status.mounted || suspended != s_status.suspended) {
        const bool attachment = mounted != s_status.mounted;
        s_status.mounted = mounted;
        s_status.suspended = suspended;
        /* USB suspend does not reset alternate-interface selection. Preserve
         * the host's choices across resume, but revoke all stream authority.
         * Detach/new attachment does reset both alternate settings. */
        if (attachment) s_status.capture_selected = s_status.playback_selected = false;
        replace_link_locked(attachment);
    }
    portEXIT_CRITICAL(&s_lock);
}

static ae_result_t capabilities(void *context, ae_route_t route, ae_route_capabilities_t *out)
{
    (void)context;
    if (!out || route != AE_ROUTE_USB_UAC) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    *out = (ae_route_capabilities_t){
        .implemented = true, .available = s_status.initialized && s_status.stack_ready &&
            (!INPUT_USB_AUDIO_PTI_REQUIRED || (s_transport.mode_known && s_transport.mode_valid)),
        .connected = live_locked(), .qualified = false,
        .capture = live_locked() && s_status.capture_selected,
        .playback = live_locked() && s_status.playback_selected,
        .full_duplex = true, .format = s_format, .token = s_token,
        .reason = live_locked() ? AE_OK : AE_UNAVAILABLE,
    };
    portEXIT_CRITICAL(&s_lock);
    return AE_OK;
}

static ae_result_t prepare(void *context, ae_route_t route, const ae_format_t *format,
                           uint8_t roles, uint64_t generation, const ae_route_token_t *expected)
{
    (void)context;
    if (route != AE_ROUTE_USB_UAC || !same_format(format) || !generation || !expected ||
        !roles || (roles & ~(AE_MICROPHONE | AE_SPEAKER))) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result = AE_OK;
    if (!live_locked()) result = AE_UNAVAILABLE;
    else if (!same_token(expected, &s_token)) result = AE_STALE;
    else if (generation == s_stop_generation && same_token(expected, &s_stop_token)) result = AE_STALE;
    else if (((roles & AE_MICROPHONE) && !s_status.capture_selected) ||
             ((roles & AE_SPEAKER) && !s_status.playback_selected)) result = AE_UNAVAILABLE;
    else if (s_status.audio_generation != generation || s_roles != roles ||
             !same_token(&s_prepared_token, expected)) {
        request_flush_locked();
        s_status.audio_generation = generation;
        s_prepared_token = *expected;
        s_roles = roles;
        result = AE_AGAIN;
    } else if (s_flush_requested != s_flush_completed) result = AE_AGAIN;
    else s_status.prepared = true;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

static ae_result_t capture(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context;
    if (!frame || route != AE_ROUTE_USB_UAC || !same_format(&frame->format) ||
        frame->bytes != UAC_FRAME_BYTES || !frame->consumer_generation || !frame->sequence) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result = AE_OK;
    if (!live_locked() || !s_status.prepared || !(s_roles & AE_MICROPHONE)) result = AE_UNAVAILABLE;
    else if (frame->audio_generation != s_status.audio_generation ||
             !same_token(&frame->route_token, &s_prepared_token) ||
             !same_token(&frame->route_token, &s_token) ||
             (s_tx_seen && frame->sequence <= s_tx_sequence)) result = AE_STALE;
    else if (s_tx_count == UAC_QUEUE_FRAMES) result = AE_AGAIN;
    else {
        queued_frame_t *slot = &s_memory->tx[(s_tx_head + s_tx_count) % UAC_QUEUE_FRAMES];
        slot->frame = *frame;
        slot->offset = 0;
        slot->admitted_us = (uint64_t)esp_timer_get_time();
        ++s_tx_count;
        s_tx_sequence = frame->sequence;
        s_tx_seen = true;
    }
    if (result != AE_OK) count(&s_status.rejected_capture_frames);
    portEXIT_CRITICAL(&s_lock);
    return result;
}

static ae_result_t stop(void *context, ae_route_t route, uint64_t generation,
                        const ae_route_token_t *expected)
{
    (void)context;
    if (route != AE_ROUTE_USB_UAC || !generation || !expected) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result;
    if (generation == s_stop_generation && same_token(expected, &s_stop_token))
        result = s_flush_completed == s_flush_requested ? AE_OK : AE_AGAIN;
    else if (generation != s_status.audio_generation || !same_token(expected, &s_prepared_token))
        result = AE_STALE;
    else {
        s_stop_generation = generation;
        s_stop_token = *expected;
        request_flush_locked();
        s_roles = 0;
        result = AE_AGAIN;
    }
    portEXIT_CRITICAL(&s_lock);
    /* Already submitted USB packets cannot be recalled. OK confirms the
     * producer/queued FIFO barrier, not removal from the host's own buffers. */
    return result;
}

/* A separate OUT barrier preserves an independently authorized microphone. */
static ae_result_t bind_playback(void *context, ae_route_t route, uint64_t generation,
    const ae_route_token_t *expected, uint64_t consumer, bool enabled)
{
    (void)context;
    if (route != AE_ROUTE_USB_UAC || !generation || !expected || !consumer) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result = AE_OK;
    const bool current = generation == s_status.audio_generation &&
        same_token(expected, &s_prepared_token);
    if (!enabled && (!current || consumer != s_consumer_generation)) {
        /* A delayed close cannot revoke its replacement. */
    } else if (!live_locked() || !s_status.prepared || !(s_roles & AE_SPEAKER)) {
        result = enabled ? AE_UNAVAILABLE : AE_OK;
    } else if (!current) result = AE_STALE;
    else if (enabled && consumer == s_consumer_generation) {
        result = s_rx_flush_requested == s_rx_flush_completed ? AE_OK : AE_AGAIN;
    } else if (s_rx_flush_requested == UINT64_MAX) {
        s_consumer_generation = 0;
        s_status.error = ESP_ERR_INVALID_STATE;
        result = AE_UNAVAILABLE;
    } else {
        s_consumer_generation = enabled ? consumer : 0;
        ++s_rx_flush_requested;
        s_rx_count = s_rx_head = s_input_bytes = 0;
        s_rx_sequence = s_sample_offset = s_played_samples = 0;
        s_feedback_level_q16 = INT64_C(960) << 16;
        /* Old frames are already inadmissible. Enabling waits for FIFO wipe. */
        result = enabled ? AE_AGAIN : AE_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return result;
}

static void playback_consumed(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context;
    if (!frame || route != AE_ROUTE_USB_UAC || !same_format(&frame->format) ||
        frame->bytes != UAC_FRAME_BYTES || !frame->consumer_generation) return;
    portENTER_CRITICAL(&s_lock);
    if (live_locked() && s_status.prepared && (s_roles & AE_SPEAKER) &&
        frame->audio_generation == s_status.audio_generation &&
        frame->consumer_generation == s_consumer_generation &&
        same_token(&frame->route_token, &s_prepared_token) &&
        frame->sample_offset <= s_sample_offset &&
        s_sample_offset - frame->sample_offset >= UAC_FRAME_BYTES / 2) {
        const uint64_t end = frame->sample_offset + UAC_FRAME_BYTES / 2;
        if (end > s_played_samples) s_played_samples = end;
    }
    portEXIT_CRITICAL(&s_lock);
}

ae_route_port_t usb_audio_route_port(void)
{
    return (ae_route_port_t){.capabilities = capabilities, .prepare = prepare,
        .capture = capture, .stop = stop, .playback_consumed = playback_consumed,
        .bind_playback = bind_playback};
}

static void observe_playback_pcm(const ae_frame_t *frame)
{
    uint16_t nonzero = 0, peak = 0;
    /* The caller owns this complete copy. Keep the scan outside the IRQ lock. */
    for (unsigned i = 0; i < frame->bytes; i += 2) {
        uint16_t bits = (uint16_t)frame->data[i] | (uint16_t)((uint16_t)frame->data[i + 1] << 8);
        int32_t value = bits < 0x8000U ? bits : (int32_t)bits - 65536;
        uint16_t magnitude = (uint16_t)(value < 0 ? -value : value);
        if (magnitude) ++nonzero;
        if (magnitude > peak) peak = magnitude;
    }
    portENTER_CRITICAL(&s_lock);
    /* Lifecycle can revoke a frame while it is being measured. Observation
     * must not publish that old frame as evidence for a replacement stream. */
    if (live_locked() && s_status.prepared && frame->audio_generation == s_status.audio_generation &&
        frame->consumer_generation == s_consumer_generation && same_token(&frame->route_token, &s_token)) {
        if (!s_pcm.known || s_pcm.link_generation != frame->route_token.link_generation ||
            s_pcm.audio_generation != frame->audio_generation || s_pcm.consumer_generation != frame->consumer_generation)
            s_pcm = (usb_audio_pcm_snapshot_t){.known = true, .link_generation = frame->route_token.link_generation,
                .audio_generation = frame->audio_generation, .consumer_generation = frame->consumer_generation};
        count(&s_pcm.frames);
        add_count(&s_pcm.samples, (uint16_t)(frame->bytes / 2U));
        add_count(&s_pcm.nonzero_samples, nonzero);
        s_pcm.last_sequence = frame->sequence;
        if (peak > s_pcm.peak_abs) s_pcm.peak_abs = peak;
    }
    portEXIT_CRITICAL(&s_lock);
}

void usb_audio_pcm_snapshot(usb_audio_pcm_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_pcm;
    out->current = out->known && live_locked() && s_status.prepared &&
        out->link_generation == s_status.link_generation && out->audio_generation == s_status.audio_generation &&
        out->consumer_generation == s_consumer_generation && s_rx_flush_requested == s_rx_flush_completed;
    portEXIT_CRITICAL(&s_lock);
}

ae_result_t usb_audio_receive(ae_frame_t *frame)
{
    if (!frame) return AE_INVALID;
    portENTER_CRITICAL(&s_lock);
    ae_result_t result = AE_AGAIN;
    if (!live_locked() || !s_status.prepared || !(s_roles & AE_SPEAKER) ||
        !s_consumer_generation || s_rx_flush_requested != s_rx_flush_completed) result = AE_UNAVAILABLE;
    else while (s_rx_count) {
        queued_frame_t *slot = &s_memory->rx[s_rx_head];
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const bool fresh = now >= slot->admitted_us && now - slot->admitted_us < UAC_FRAME_AGE_US &&
            slot->frame.consumer_generation == s_consumer_generation;
        if (fresh) *frame = slot->frame;
        memset(slot, 0, sizeof(*slot));
        s_rx_head = (s_rx_head + 1) % UAC_QUEUE_FRAMES;
        --s_rx_count;
        if (fresh) { result = AE_OK; break; }
        count(&s_status.dropped_playback_frames);
    }
    portEXIT_CRITICAL(&s_lock);
    if (result == AE_OK) observe_playback_pcm(frame);
    return result;
}

usb_audio_snapshot_t usb_audio_snapshot(void)
{
    portENTER_CRITICAL(&s_lock);
    usb_audio_snapshot_t result = s_status;
    result.playback_bound = s_consumer_generation != 0;
    result.playback_flush_pending = s_rx_flush_requested != s_rx_flush_completed;
    result.queued_playback_frames = (uint16_t)s_rx_count;
    result.partial_playback_bytes = (uint16_t)s_input_bytes;
    result.queued_capture_frames = (uint16_t)s_tx_count;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

/* Only this worker calls FIFO APIs. Lifecycle callbacks revoke admission
 * immediately; prepare/stop wait for the observed flush barrier. Calls into
 * TinyUSB are outside our critical section and never run on the coordinator. */
static void service_io(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint64_t flush = s_flush_requested;
    const bool need_flush = flush != s_flush_completed;
    const uint64_t rx_flush = s_rx_flush_requested;
    const bool need_rx_flush = rx_flush != s_rx_flush_completed;
    const bool ready = s_status.stack_ready;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) return;
    if (need_flush || need_rx_flush) {
        if (need_flush) (void)tud_audio_clear_ep_in_ff();
        (void)tud_audio_clear_ep_out_ff();
        /* Admission remains closed until this worker acknowledges the flush.
         * No producer can touch memory, so wipe PSRAM without masking IRQs. */
        if (need_flush) memset(s_memory, 0, sizeof(*s_memory));
        else {
            memset(s_memory->rx, 0, sizeof(s_memory->rx));
            memset(s_memory->input, 0, sizeof(s_memory->input));
        }
        portENTER_CRITICAL(&s_lock);
        if (rx_flush == s_rx_flush_requested) s_rx_flush_completed = rx_flush;
        if (flush == s_flush_requested) {
            s_flush_completed = flush;
            s_status.cleanup_pending = false;
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    const uint64_t active_flush = flush;
    /* FIFO admission is independent of USB packet timing. Fill up to four
     * packet-sized chunks per wake so scheduling jitter does not cap capture
     * below its PCM rate. Keep the original scratch and short lock scopes;
     * a short/zero write retains its suffix until a later worker pass. */
    for (unsigned pass = 0; pass < UAC_TX_WRITES_PER_PASS; ++pass) {
        const uint64_t output_now = (uint64_t)esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        bool output = live_locked() && s_status.prepared && (s_roles & AE_MICROPHONE) &&
            active_flush == s_flush_requested && s_tx_count;
        uint16_t output_bytes = 0;
        if (output) {
            queued_frame_t *slot = &s_memory->tx[s_tx_head];
            if (output_now < slot->admitted_us || output_now - slot->admitted_us >= UAC_FRAME_AGE_US) {
                memset(slot, 0, sizeof(*slot));
                s_tx_head = (s_tx_head + 1) % UAC_QUEUE_FRAMES;
                --s_tx_count;
                count(&s_status.rejected_capture_frames);
                output = false;
            } else {
                output_bytes = (uint16_t)(UAC_FRAME_BYTES - slot->offset);
                if (output_bytes > sizeof(s_memory->output)) output_bytes = sizeof(s_memory->output);
                memcpy(s_memory->output, slot->frame.data + slot->offset, output_bytes);
            }
        }
        if (output) count(&s_status.capture_write_calls);
        portEXIT_CRITICAL(&s_lock);
        if (!output) break;
        uint16_t written = tud_audio_write(s_memory->output, output_bytes);
        portENTER_CRITICAL(&s_lock);
        add_count(&s_status.capture_write_bytes, written);
        if (active_flush == s_flush_requested && s_tx_count) {
            queued_frame_t *slot = &s_memory->tx[s_tx_head];
            if (written > output_bytes || (written & 1U)) {
                s_status.error = ESP_FAIL;
                request_flush_locked();
            } else {
                slot->offset = (uint16_t)(slot->offset + written);
                if (slot->offset == UAC_FRAME_BYTES) {
                    memset(slot, 0, sizeof(*slot));
                    s_tx_head = (s_tx_head + 1) % UAC_QUEUE_FRAMES;
                    --s_tx_count;
                }
            }
        }
        const bool current = active_flush == s_flush_requested;
        portEXIT_CRITICAL(&s_lock);
        if (!current || written < output_bytes) break;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool input = live_locked() && s_status.prepared && (s_roles & AE_SPEAKER) &&
        active_flush == s_flush_requested && s_consumer_generation &&
        s_rx_flush_requested == s_rx_flush_completed;
    if (input && s_input_bytes && (now < s_input_started_us || now - s_input_started_us >= UAC_FRAME_AGE_US)) {
        memset(s_memory->input, 0, sizeof(s_memory->input));
        s_input_bytes = 0;
        count(&s_status.dropped_playback_frames);
    }
    const uint64_t consumer = s_consumer_generation;
    const uint64_t input_flush = s_rx_flush_requested;
    unsigned offset = s_input_bytes;
    if (input) count(&s_status.playback_read_calls);
    portEXIT_CRITICAL(&s_lock);
    if (!input) return;
    uint16_t received = tud_audio_read(s_memory->input + offset, (uint16_t)(UAC_FRAME_BYTES - offset));
    portENTER_CRITICAL(&s_lock);
    add_count(&s_status.playback_read_bytes, received);
    if (active_flush == s_flush_requested && s_status.prepared &&
        consumer == s_consumer_generation && input_flush == s_rx_flush_requested) {
        if (received > UAC_FRAME_BYTES - offset || (received & 1U)) {
            s_status.error = ESP_FAIL;
            request_flush_locked();
        } else {
            if (!offset && received) s_input_started_us = now;
            s_input_bytes += received;
            if (s_input_bytes == UAC_FRAME_BYTES) {
                if (s_rx_sequence == UINT64_MAX || s_sample_offset > UINT64_MAX - UAC_FRAME_BYTES / 2) {
                    s_status.error = ESP_ERR_INVALID_STATE;
                    request_flush_locked();
                } else {
                    ++s_rx_sequence;
                    const uint64_t offset_samples = s_sample_offset;
                    s_sample_offset += UAC_FRAME_BYTES / 2;
                    if (s_rx_count == UAC_QUEUE_FRAMES) count(&s_status.dropped_playback_frames);
                    else {
                    queued_frame_t *slot = &s_memory->rx[(s_rx_head + s_rx_count) % UAC_QUEUE_FRAMES];
                    ae_frame_t *frame = &slot->frame;
                    *frame = (ae_frame_t){.audio_generation = s_status.audio_generation,
                        .consumer_generation = consumer,
                        .sequence = s_rx_sequence, .sample_offset = offset_samples,
                        .route_token = s_prepared_token, .format = s_format, .bytes = UAC_FRAME_BYTES};
                    memcpy(frame->data, s_memory->input, UAC_FRAME_BYTES);
                    slot->admitted_us = now;
                    ++s_rx_count;
                    count(&s_status.playback_frames_enqueued);
                    }
                }
                memset(s_memory->input, 0, sizeof(s_memory->input));
                s_input_bytes = 0;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

/* The feedback observes the entire unconsumed stream: TinyUSB FIFO, partial
 * frame, USB queue and frames admitted to the manager but not yet accepted by
 * the codec. Eager queue transfers therefore cannot falsify sink consumption.
 * A 20 ms target and 64 ms low-pass absorb 10 ms frame granularity. Proportional
 * correction is bounded to +/-1000 Hz, within the 49-sample packet maximum.
 * This is bounded queue regulation, not a claim of physical clock measurement. */
static void service_feedback(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool stack_ready = s_status.stack_ready;
    portEXIT_CRITICAL(&s_lock);
    if (!stack_ready) return;
    const uint16_t fifo_bytes = tud_audio_available();
    const bool high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
    portENTER_CRITICAL(&s_lock);
    const bool active = live_locked() && s_status.prepared && (s_roles & AE_SPEAKER) &&
        s_consumer_generation && s_rx_flush_requested == s_rx_flush_completed;
    int64_t correction = 0;
    if (active) {
        uint64_t pending = s_sample_offset - s_played_samples;
        pending += (s_input_bytes + fifo_bytes) / 2U;
        /* Clamp before signed arithmetic; excessive backlog still reduces the
         * offered rate and is handled by normal overrun/age rejection. */
        if (pending > 9600) pending = 9600;
        s_feedback_level_q16 += (((int64_t)pending << 16) - s_feedback_level_q16) / 64;
        correction = (INT64_C(960) << 16) - s_feedback_level_q16;
        correction = correction * 2 / 1000;
        if (correction > 65536) correction = 65536;
        if (correction < -65536) correction = -65536;
    }
    uint32_t feedback = (uint32_t)((INT64_C(48) << 16) + correction);
    if (high_speed) feedback /= 8;
    const bool ready = s_status.stack_ready && s_status.mounted && !s_status.suspended;
    if (ready) {
        s_transport.feedback_calculated = feedback; s_transport.feedback_calculated_known = true;
        s_transport.feedback_calculated_epoch = s_transport.reset_epoch;
    }
    portEXIT_CRITICAL(&s_lock);
    if (ready) (void)tud_audio_n_fb_set(0, feedback);
}

static void worker(void *argument)
{
    (void)argument;
    TickType_t delay = pdMS_TO_TICKS(1);
    if (!delay) delay = 1;
    for (;;) {
        service_io();
        service_feedback();
        vTaskDelay(delay);
    }
}

esp_err_t usb_audio_init(void)
{
    if (s_attempted) return s_status.error;
    s_attempted = true;
    s_status.error = ESP_ERR_NO_MEM;
    s_memory = heap_caps_calloc(1, sizeof(*s_memory), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_memory) return s_status.error;
    esp_fill_random(s_boot_id.bytes, sizeof(s_boot_id.bytes));
    if (xTaskCreate(worker, "usb_audio", 3072, NULL, 3, &s_worker) != pdPASS) {
        heap_caps_free(s_memory);
        s_memory = NULL;
        return s_status.error;
    }
    s_status.error = ESP_OK;
    s_status.initialized = true;
    return ESP_OK;
}

bool tud_audio_set_itf_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport;
    const uint16_t interface = tu_le16toh(request->wIndex);
    const uint16_t alternate = tu_le16toh(request->wValue);
    if (alternate > 1 || (interface != INPUT_USB_AUDIO_PLAYBACK_INTERFACE &&
                         interface != INPUT_USB_AUDIO_CAPTURE_INTERFACE)) return false;
    portENTER_CRITICAL(&s_lock);
    bool *selected = interface == INPUT_USB_AUDIO_PLAYBACK_INTERFACE ?
        &s_status.playback_selected : &s_status.capture_selected;
    /* SET_INTERFACE closes/reopens endpoints even for the same alt number.
     * It replaces the stream and cannot retain old queued samples/consent. */
    *selected = alternate != 0;
    replace_link_locked(false);
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport;
    const uint16_t interface = tu_le16toh(request->wIndex);
    if (interface != INPUT_USB_AUDIO_PLAYBACK_INTERFACE &&
        interface != INPUT_USB_AUDIO_CAPTURE_INTERFACE) return false;
    portENTER_CRITICAL(&s_lock);
    /* TinyUSB closes the old endpoint before validating/opening the requested
     * alternate. Revoke it even for invalid alt values; only the final successful
     * callback can publish selection again. Do not abort SDK cleanup. */
    if (interface == INPUT_USB_AUDIO_PLAYBACK_INTERFACE) s_status.playback_selected = false;
    else s_status.capture_selected = false;
    replace_link_locked(false);
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    const uint16_t index = tu_le16toh(request->wIndex);
    const uint16_t value = tu_le16toh(request->wValue);
    const uint16_t length = tu_le16toh(request->wLength);
    if (index != ((INPUT_USB_AUDIO_CLOCK_ID << 8) | INPUT_USB_AUDIO_CONTROL_INTERFACE) ||
        (value & 0xffU) != 0) return false;
    if (value == (AUDIO20_CS_CTRL_SAM_FREQ << 8)) {
        if (request->bRequest == AUDIO20_CS_REQ_CUR && length == 4) {
            const uint32_t rate = tu_htole32(INPUT_USB_AUDIO_RATE);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, (void *)&rate, sizeof(rate));
        }
        if (request->bRequest == AUDIO20_CS_REQ_RANGE && length >= 2) {
            const uint8_t range[] = {1, 0, 0x80, 0xbb, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0, 0};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, (void *)range, sizeof(range));
        }
    } else if (value == (AUDIO20_CS_CTRL_CLK_VALID << 8) &&
               request->bRequest == AUDIO20_CS_REQ_CUR && length == 1) {
        const uint8_t valid = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, (void *)&valid, 1);
    }
    return false;
}

void tud_audio_feedback_params_cb(uint8_t function, uint8_t alternate, audio_feedback_params_t *parameters)
{
    if (!parameters) return;
    *parameters = (audio_feedback_params_t){0};
    if (function == 0 && alternate == 1) {
        parameters->method = AUDIO_FEEDBACK_METHOD_DISABLED; /* Manual coupled feedback. */
        parameters->sample_freq = INPUT_USB_AUDIO_RATE;
    }
}
