// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/usb_audio.c"

static unsigned lock_depth, allocations, releases, workers, clears, in_clears, out_clears;
static bool fail_allocation, fail_worker;
static uint64_t now_us = 1000;
static uint8_t written_data[16384], input_data[16384], control_data[64];
static size_t written_bytes, input_bytes, input_offset, control_bytes;
static uint16_t maximum_write = UINT16_MAX;
static void (*during_write)(void), (*during_read)(void);
static uint32_t feedback_value = 48U << 16;
static bool high_speed;
tusb_speed_t tud_speed_get(void) { return high_speed ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL; }
#ifndef USB_AUDIO_REAL_STACK
uint16_t tud_audio_n_available(uint8_t function)
{ assert(!lock_depth && function == 0); return (uint16_t)(input_bytes - input_offset); }
bool tud_audio_n_fb_set(uint8_t function, uint32_t feedback)
{ assert(!lock_depth && function == 0); feedback_value = feedback; return true; }
#endif

void usb_test_enter(portMUX_TYPE *lock) { (void)lock; assert(!lock_depth); ++lock_depth; }
void usb_test_leave(portMUX_TYPE *lock) { (void)lock; assert(lock_depth == 1); --lock_depth; }
int64_t esp_timer_get_time(void) { return (int64_t)now_us; }
void *heap_caps_calloc(size_t count_, size_t bytes, unsigned caps)
{
    assert(!lock_depth && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations;
    return fail_allocation ? NULL : calloc(count_, bytes);
}
void heap_caps_free(void *memory) { assert(!lock_depth); ++releases; free(memory); }
void esp_fill_random(void *buffer, size_t bytes) { memset(buffer, 0x6a, bytes); }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(!lock_depth && function == worker && !strcmp(name, "usb_audio"));
    assert(stack == 3072 && !argument && priority == 3 && handle);
    ++workers;
    *handle = fail_worker ? NULL : (void *)&workers;
    return fail_worker ? 0 : pdPASS;
}
void vTaskDelay(TickType_t timeout) { (void)timeout; abort(); } /* Never run an unbounded worker. */
#ifndef USB_AUDIO_REAL_STACK
bool tud_audio_n_clear_ep_in_ff(uint8_t function)
{
    assert(!lock_depth && function == 0);
    ++clears; ++in_clears;
    return true;
}
bool tud_audio_n_clear_ep_out_ff(uint8_t function)
{
    assert(!lock_depth && function == 0);
    ++clears; ++out_clears;
    input_bytes = input_offset = 0;
    return true;
}
uint16_t tud_audio_n_write(uint8_t function, const void *data, uint16_t bytes)
{
    assert(!lock_depth && function == 0 && data && bytes <= 98);
    if (during_write) {
        void (*hook)(void) = during_write;
        during_write = NULL;
        hook();
    }
    if (bytes > maximum_write) bytes = maximum_write;
    assert(written_bytes + bytes <= sizeof(written_data));
    memcpy(written_data + written_bytes, data, bytes);
    written_bytes += bytes;
    return bytes;
}
uint16_t tud_audio_n_read(uint8_t function, void *data, uint16_t bytes)
{
    assert(!lock_depth && function == 0 && data);
    size_t remaining = input_bytes - input_offset;
    if (remaining < bytes) bytes = (uint16_t)remaining;
    memcpy(data, input_data + input_offset, bytes);
    input_offset += bytes;
    if (during_read) {
        void (*hook)(void) = during_read;
        during_read = NULL;
        hook();
    }
    return bytes;
}
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t rhport, const tusb_control_request_t *request,
                                               void *buffer, uint16_t bytes)
{
    assert(!lock_depth && rhport == 0 && buffer && bytes <= sizeof(control_data));
    control_bytes = bytes < request->wLength ? bytes : request->wLength;
    memcpy(control_data, buffer, control_bytes);
    return true;
}
#endif

static void select_interface(uint16_t interface, uint16_t alternate)
{
    tusb_control_request_t request = {.wIndex = interface, .wValue = alternate};
    assert(tud_audio_set_itf_cb(0, &request));
}
static ae_route_token_t start(uint8_t roles)
{
    assert(usb_audio_init() == ESP_OK);
    usb_audio_stack_started(ESP_OK);
    usb_audio_stack_event(true, false);
    select_interface(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 1);
    select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1);
    service_io();
    ae_route_capabilities_t caps;
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK);
    assert(caps.implemented && caps.available && caps.connected && !caps.qualified);
    assert(caps.capture && caps.playback && caps.token.link_generation);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, roles, 11, &caps.token) == AE_AGAIN);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, roles, 11, &caps.token) == AE_AGAIN);
    service_io();
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, roles, 11, &caps.token) == AE_OK);
    if (roles & AE_SPEAKER) {
        assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, true) == AE_AGAIN);
        service_io();
        assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, true) == AE_OK);
    }
    return caps.token;
}
static ae_frame_t make_frame(ae_route_token_t token, uint64_t sequence)
{
    ae_frame_t frame = {.audio_generation = 11, .consumer_generation = 17,
        .sequence = sequence, .route_token = token, .format = s_format, .bytes = UAC_FRAME_BYTES};
    for (unsigned i = 0; i < UAC_FRAME_BYTES; ++i) frame.data[i] = (uint8_t)(i + sequence);
    return frame;
}
static void service(unsigned steps)
{
    for (unsigned i = 0; i < steps; ++i) { now_us += 1000; service_io(); service_feedback(); }
}

static void capture_and_backpressure(void)
{
    ae_route_token_t token = start(AE_MICROPHONE);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_STALE);
    maximum_write = 40;
    service(25);
    assert(written_bytes == UAC_FRAME_BYTES && memcmp(frame.data, written_data, UAC_FRAME_BYTES) == 0);
    assert(!s_tx_count);
    for (unsigned i = 0; i < UAC_QUEUE_FRAMES; ++i) {
        frame.sequence = i + 2;
        assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    }
    frame.sequence = 99;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_AGAIN && s_tx_count == UAC_QUEUE_FRAMES);
    now_us += UAC_FRAME_AGE_US;
    service(UAC_QUEUE_FRAMES);
    assert(!s_tx_count && written_bytes == UAC_FRAME_BYTES);
}
static void disconnect_during_fifo_write(void)
{
    usb_audio_stack_event(false, false);
    usb_audio_stack_event(true, false);
}
static void disconnect_on_next_write(void) { during_write = disconnect_during_fifo_write; }
static void capture_batch_guards(void)
{
    ae_route_token_t token = start(AE_MICROPHONE);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    service(1);
    assert(s_status.capture_write_calls == 4 && written_bytes == 392 && s_memory->tx[s_tx_head].offset == 392);
    maximum_write = 0; service(1);
    assert(s_status.capture_write_calls == 5 && written_bytes == 392 && s_memory->tx[s_tx_head].offset == 392);
    maximum_write = 40; service(1);
    assert(s_status.capture_write_calls == 6 && written_bytes == 432 && s_memory->tx[s_tx_head].offset == 432);
    assert(!memcmp(frame.data, written_data, written_bytes));
    maximum_write = UINT16_MAX; during_write = disconnect_on_next_write; service(1);
    assert(s_status.capture_write_calls == 8 && written_bytes == 628 && !s_tx_count);
    assert(!memcmp(frame.data, written_data, written_bytes));
    assert(!s_status.prepared && s_status.cleanup_pending && s_status.error == ESP_OK);
    service(1); assert(!s_status.cleanup_pending && written_bytes == 628);
}
static void replace_capture_during_write(void)
{
    ae_frame_t replacement = make_frame(s_prepared_token, 1); replacement.consumer_generation = 19;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &replacement) == AE_AGAIN);
}
static void capture_consumer_scope(void)
{
    ae_route_token_t token = start(AE_MICROPHONE | AE_SPEAKER);
    ae_frame_t old = make_frame(token, 20);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &old) == AE_OK);
    memset(input_data, 0x5a, 200); input_bytes = 200; service(1);
    assert(s_memory->tx[s_tx_head].offset == 392 && s_input_bytes == 200);
    unsigned before_in = in_clears, before_out = out_clears;
    ae_frame_t next = make_frame(token, 1); next.consumer_generation = 18;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_AGAIN && !s_tx_count);
    old.sequence = UINT64_MAX; assert(capture(NULL, AE_ROUTE_USB_UAC, &old) == AE_STALE);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_AGAIN);
    service(1); assert(in_clears == before_in + 1 && out_clears == before_out && s_input_bytes == 200 && s_consumer_generation == 17);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_OK);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_STALE);
    next.sequence = 2; assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_OK);
    uint64_t calls_before = s_status.capture_write_calls;
    during_write = replace_capture_during_write; service(1);
    assert(s_status.capture_write_calls == calls_before + 1 && !s_tx_count && s_input_bytes == 200);
    next.sequence = UINT64_MAX; assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_STALE);
    next.consumer_generation = 19; next.sequence = 1;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_AGAIN); service(1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_OK && s_input_bytes == 200 && s_consumer_generation == 17);
    assert(in_clears == before_in + 2 && out_clears == before_out);
}
static void capture_invalid_batch_write(void)
{
    ae_route_token_t token = start(AE_MICROPHONE);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    maximum_write = 1; service(1);
    assert(s_status.capture_write_calls == 1 && s_status.capture_write_bytes == 1);
    assert(s_status.error == ESP_FAIL && !s_status.prepared && s_status.cleanup_pending && !s_tx_count);
    service(1); assert(!s_status.cleanup_pending && s_status.capture_write_calls == 1);
}
static void generations_and_stop(void)
{
    ae_route_token_t token = start(AE_MICROPHONE);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    during_write = disconnect_during_fifo_write;
    service(1);
    const size_t submitted = written_bytes;
    assert(!usb_audio_snapshot().prepared && usb_audio_snapshot().cleanup_pending);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_UNAVAILABLE);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &token) == AE_STALE);
    service(2);
    assert(written_bytes == submitted && !usb_audio_snapshot().cleanup_pending);
    select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1);
    ae_route_capabilities_t caps;
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK && !same_token(&token, &caps.token));
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_AGAIN);
    service(1);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_OK);
    frame = make_frame(caps.token, 2); frame.audio_generation = 12;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    assert(stop(NULL, AE_ROUTE_USB_UAC, 12, &caps.token) == AE_AGAIN);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_UNAVAILABLE);
    service(1);
    assert(stop(NULL, AE_ROUTE_USB_UAC, 12, &caps.token) == AE_OK);
    assert(written_bytes == submitted);
}
static void playback_frames_and_cancel(void)
{
    ae_route_token_t token = start(AE_SPEAKER);
    for (unsigned i = 0; i < UAC_FRAME_BYTES * 2; ++i) input_data[i] = (uint8_t)i;
    input_bytes = 400;
    service(1);
    ae_frame_t frame;
    assert(usb_audio_receive(&frame) == AE_AGAIN);
    input_bytes = UAC_FRAME_BYTES * 2;
    service(2);
    assert(usb_audio_receive(&frame) == AE_OK && frame.bytes == UAC_FRAME_BYTES);
    assert(frame.sequence == 1 && frame.sample_offset == 0 && same_token(&frame.route_token, &token));
    assert(memcmp(frame.data, input_data, UAC_FRAME_BYTES) == 0);
    assert(usb_audio_receive(&frame) == AE_OK && frame.sequence == 2 && frame.sample_offset == 480);
    assert(usb_audio_receive(&frame) == AE_AGAIN);
    select_interface(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 0);
    assert(usb_audio_receive(&frame) == AE_UNAVAILABLE);
    service(1);
    assert(!s_rx_count && !s_input_bytes);
}
static void format_and_clock_requests(void)
{
    ae_route_token_t token = start(AE_MICROPHONE);
    ae_format_t bad = s_format; bad.sample_rate_hz = 44100;
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &bad, AE_MICROPHONE, 11, &token) == AE_INVALID);
    ae_frame_t frame = make_frame(token, 1); --frame.bytes;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_INVALID);
    tusb_control_request_t request = {.wIndex = 0x0403, .wValue = AUDIO20_CS_CTRL_SAM_FREQ << 8,
        .bRequest = AUDIO20_CS_REQ_CUR, .wLength = 4};
    assert(tud_audio_get_req_entity_cb(0, &request) && control_bytes == 4);
    assert(control_data[0] == 0x80 && control_data[1] == 0xbb && !control_data[2] && !control_data[3]);
    request.bRequest = AUDIO20_CS_REQ_RANGE; request.wLength = 14;
    assert(tud_audio_get_req_entity_cb(0, &request) && control_bytes == 14 && control_data[0] == 1);
    request.wIndex = 0x0402;
    assert(!tud_audio_get_req_entity_cb(0, &request));
    request.wIndex = INPUT_USB_AUDIO_CAPTURE_INTERFACE; request.wValue = 2;
    assert(!tud_audio_set_itf_cb(0, &request));
    audio_feedback_params_t feedback;
    tud_audio_feedback_params_cb(0, 1, &feedback);
    assert(feedback.method == AUDIO_FEEDBACK_METHOD_DISABLED && feedback.sample_freq == 48000);
}
static void failed_allocation_stays_unavailable(void)
{
    fail_allocation = true;
    assert(usb_audio_init() == ESP_ERR_NO_MEM && allocations == 1 && !workers);
    fail_allocation = false;
    assert(usb_audio_init() == ESP_ERR_NO_MEM && allocations == 1);
    usb_audio_stack_started(ESP_OK);
    usb_audio_stack_event(true, false);
    ae_route_capabilities_t caps;
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK && !caps.available);
    service(1);
}
static void worker_failure_releases_memory(void)
{
    fail_worker = true;
    assert(usb_audio_init() == ESP_ERR_NO_MEM && allocations == 1 && releases == 1 && !s_memory);
    assert(usb_audio_init() == ESP_ERR_NO_MEM && allocations == 1 && workers == 1);
}
static void saturation_and_time_failure(void)
{
    ae_route_token_t token = start(AE_SPEAKER);
    input_bytes = UAC_FRAME_BYTES * 6;
    service(6);
    assert(s_rx_count == UAC_QUEUE_FRAMES && s_status.dropped_playback_frames == 2);
    usb_audio_stack_event(true, true);
    assert(!s_status.prepared && !s_rx_count);
    service(1);
    assert(stop(NULL, AE_ROUTE_USB_UAC, 11, &token) == AE_AGAIN);
    service(1);
    assert(stop(NULL, AE_ROUTE_USB_UAC, 11, &token) == AE_OK);
}
static void many_reconnections(void)
{
    (void)start(AE_MICROPHONE);
    uint64_t previous = s_status.link_generation;
    for (unsigned i = 0; i < 10000; ++i) {
        usb_audio_stack_event(false, false);
        usb_audio_stack_event(true, false);
        service(1);
        assert(s_status.link_generation > previous && !s_status.prepared && !s_tx_count && !s_rx_count);
        previous = s_status.link_generation;
    }
    assert(allocations == 1 && workers == 1);
}
static void same_alternate_and_resume_require_fresh_preparation(void)
{
    ae_route_token_t token = start(AE_MICROPHONE);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1);
    assert(!s_status.prepared && !s_tx_count && s_status.capture_selected);
    assert(s_status.link_generation > token.link_generation);
    service(1);
    assert(written_bytes == 0);
    usb_audio_stack_event(true, true);
    assert(s_status.capture_selected && s_status.playback_selected && !s_status.prepared);
    usb_audio_stack_event(true, false);
    assert(s_status.capture_selected && s_status.playback_selected && !s_status.prepared);
    ae_route_capabilities_t caps;
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK && caps.capture && caps.playback);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &token) == AE_STALE);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_AGAIN);
    service(1);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_OK);
    assert(written_bytes == 0);
}
static void endpoint_close_revokes_before_reopen(void)
{
    ae_route_token_t token = start(AE_MICROPHONE | AE_SPEAKER);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    tusb_control_request_t request = {.wIndex = INPUT_USB_AUDIO_CAPTURE_INTERFACE, .wValue = 2};
    assert(tud_audio_set_itf_close_ep_cb(0, &request));
    assert(!s_status.prepared && !s_status.capture_selected && !s_tx_count);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_UNAVAILABLE);
    assert(!tud_audio_set_itf_cb(0, &request));
    request.wIndex = INPUT_USB_AUDIO_PLAYBACK_INTERFACE; request.wValue = 1;
    assert(tud_audio_set_itf_close_ep_cb(0, &request));
    service(1); /* SDK endpoint open failed: no final callback. */
    ae_route_capabilities_t caps;
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK && !caps.playback && !caps.capture);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 12, &caps.token) == AE_UNAVAILABLE);
    assert(tud_audio_set_itf_cb(0, &request));
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK && caps.playback && !caps.capture);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 12, &caps.token) == AE_AGAIN);
    service(1);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 12, &caps.token) == AE_OK);
}

static void feedback_tracks_codec_consumption(void)
{
    (void)start(AE_SPEAKER);
    /* A feedback-following host, independent codec rates, bursty downstream
     * scheduling and frames moved eagerly into a manager queue. The receipt
     * occurs only at codec acceptance, never at usb_audio_receive. */
    ae_frame_t manager_queue[16];
    unsigned head = 0, count_ = 0;
    uint64_t host_fraction = 0, codec_credit = 0, consumed = 0;
    unsigned starved_after_start = 0;
    for (unsigned ms = 1; ms <= 120000; ++ms) {
        high_speed = ms > 60000;
        /* Use the preceding feedback in its preceding speed's units. */
        unsigned scale = ms > 60001 ? 8U : 1U;
        host_fraction += (uint64_t)feedback_value * scale;
        unsigned samples = (unsigned)(host_fraction >> 16);
        host_fraction &= 0xffffU;
        assert(samples <= 49);
        size_t left = input_bytes - input_offset;
        memmove(input_data, input_data + input_offset, left);
        input_offset = 0; input_bytes = left + samples * 2U;
        assert(input_bytes <= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ);
        memset(input_data + left, 0x3c, samples * 2U);
        service(1);
        ae_frame_t frame;
        while (usb_audio_receive(&frame) == AE_OK) {
            assert(count_ < 16);
            assert(frame.consumer_generation == 17);
            manager_queue[(head + count_) % 16] = frame;
            ++count_;
        }
        const unsigned rate = ms < 40000 ? 48000U : ms < 80000 ? 48096U : 47904U;
        /* Begin after a 20 ms priming interval. Consume with up to 2 ms jitter. */
        if (ms > 20) codec_credit += rate;
        if (ms % 7U != 0 && ms % 7U != 1) {
            while (codec_credit >= 480000) {
                if (!count_) { if (ms > 2000) ++starved_after_start; break; }
                playback_consumed(NULL, AE_ROUTE_USB_UAC, &manager_queue[head]);
                head = (head + 1) % 16; --count_;
                codec_credit -= 480000; ++consumed;
            }
        }
    }
    assert(consumed > 11900 && !starved_after_start && !s_status.dropped_playback_frames);
    assert(count_ <= 5 && s_sample_offset - s_played_samples < 3000);
    const uint64_t before = s_played_samples;
    ae_frame_t stale = make_frame(s_prepared_token, 99);
    stale.audio_generation = 10; stale.sample_offset = s_sample_offset - 480;
    playback_consumed(NULL, AE_ROUTE_USB_UAC, &stale);
    assert(s_played_samples == before);
}

static void dropped_frames_preserve_stream_offsets(void)
{
    (void)start(AE_SPEAKER);
    input_bytes = UAC_FRAME_BYTES * 6; service(6);
    ae_frame_t frame;
    for (unsigned i = 0; i < 4; ++i) assert(usb_audio_receive(&frame) == AE_OK);
    assert(frame.sequence == 4 && frame.sample_offset == 1440);
    input_bytes += UAC_FRAME_BYTES; service(1);
    assert(usb_audio_receive(&frame) == AE_OK && frame.sequence == 7 && frame.sample_offset == 2880);
}

static ae_route_token_t rebind_token;
static void replace_consumer_during_read(void)
{
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &rebind_token, 19, true) == AE_AGAIN);
}
static void playback_binding_barriers(void)
{
    rebind_token = start(AE_MICROPHONE | AE_SPEAKER);
    input_bytes = UAC_FRAME_BYTES * 4; service(4);
    assert(s_rx_count == 4);
    ae_frame_t capture_frame = make_frame(rebind_token, 1), frame;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &capture_frame) == AE_OK);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &rebind_token, 18, true) == AE_AGAIN);
    assert(usb_audio_receive(&frame) == AE_UNAVAILABLE);
    service(1);
    assert(s_tx_count == 1 && s_status.prepared);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &rebind_token, 18, true) == AE_OK);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &rebind_token, 17, false) == AE_OK);
    input_bytes = UAC_FRAME_BYTES; input_offset = 0;
    during_read = replace_consumer_during_read;
    service(1);
    assert(!s_rx_count && usb_audio_receive(&frame) == AE_UNAVAILABLE);
    service(1);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &rebind_token, 19, true) == AE_OK);
    input_bytes = UAC_FRAME_BYTES; input_offset = 0; service(1);
    assert(usb_audio_receive(&frame) == AE_OK && frame.consumer_generation == 19);
    assert(frame.sequence == 1 && frame.sample_offset == 0);
    ae_frame_t stale = frame; stale.consumer_generation = 18;
    playback_consumed(NULL, AE_ROUTE_USB_UAC, &stale);
    assert(s_played_samples == 0);
    playback_consumed(NULL, AE_ROUTE_USB_UAC, &frame);
    assert(s_played_samples == 480);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &rebind_token, 19, false) == AE_OK);
    assert(usb_audio_receive(&frame) == AE_UNAVAILABLE);
    service(20);
    assert(written_bytes == UAC_FRAME_BYTES); /* Microphone survived both rebinds. */
}

static void other_profiles_keep_existing_admission(void)
{
    assert(!INPUT_USB_AUDIO_PTI_REQUIRED);
    ae_route_token_t token = start(AE_SPEAKER);
    uint64_t link = s_status.link_generation;
    input_usb_audio_dcd_mode(1, true, 0, false);
    input_usb_audio_dcd_mode(2, false, 0, true);
    input_usb_audio_dcd_mode(2, true, USB_AUDIO_MODE_PTI_NOT_ACTIVE, true);
    usb_audio_transport_snapshot_t observed; usb_audio_transport_snapshot(&observed);
    assert(observed.observed && !observed.mode_required && observed.mode_known && !observed.mode_valid);
    assert(s_status.link_generation == link && s_status.prepared && s_consumer_generation == 17 &&
        prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &token) == AE_OK);
}

static void playback_content_observation(void)
{
    usb_audio_pcm_snapshot_t observed;
    usb_audio_pcm_snapshot(&observed);
    assert(!observed.known && !observed.current);
    ae_route_token_t token = start(AE_MICROPHONE | AE_SPEAKER);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    service(10); usb_audio_pcm_snapshot(&observed);
    assert(!observed.known); /* Microphone samples never enter this observation. */
    input_bytes = UAC_FRAME_BYTES; service(1);
    assert(usb_audio_receive(&frame) == AE_OK);
    usb_audio_pcm_snapshot(&observed);
    assert(observed.known && observed.current && observed.frames == 1 && observed.samples == 480 &&
        observed.nonzero_samples == 0 && observed.peak_abs == 0 && observed.last_sequence == 1);
    const uint8_t edges[] = {0, 0, 0, 0x80, 0xff, 0x7f, 0xff, 0xff, 1, 0, 0, 0x10};
    for (unsigned i = 0; i < UAC_FRAME_BYTES; ++i) input_data[i] = edges[i % sizeof(edges)];
    input_offset = 0; service(1);
    assert(usb_audio_receive(&frame) == AE_OK && !memcmp(frame.data, input_data, UAC_FRAME_BYTES));
    usb_audio_pcm_snapshot(&observed);
    assert(observed.frames == 2 && observed.samples == 960 && observed.nonzero_samples == 400 &&
        observed.peak_abs == 32768 && observed.last_sequence == 2 && observed.audio_generation == 11 &&
        observed.consumer_generation == 17 && observed.link_generation == token.link_generation);
    usb_audio_snapshot_t before = usb_audio_snapshot();
    usb_audio_pcm_snapshot_t copy = observed;
    for (unsigned i = 0; i < 10; ++i) usb_audio_pcm_snapshot(&observed);
    usb_audio_snapshot_t after = usb_audio_snapshot();
    assert(!memcmp(&copy, &observed, sizeof(copy)) && !memcmp(&before, &after, sizeof(before)));
    input_offset = 0; service(1); now_us += UAC_FRAME_AGE_US;
    assert(usb_audio_receive(&frame) == AE_AGAIN);
    usb_audio_pcm_snapshot(&observed); assert(observed.frames == 2);
    s_pcm.frames = s_pcm.samples = s_pcm.nonzero_samples = UINT64_MAX - 1;
    input_offset = 0; service(1); assert(usb_audio_receive(&frame) == AE_OK);
    usb_audio_pcm_snapshot(&observed);
    assert(observed.frames == UINT64_MAX && observed.samples == UINT64_MAX && observed.nonzero_samples == UINT64_MAX);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &token, 18, true) == AE_AGAIN);
    observe_playback_pcm(&frame); /* Completion from the replaced consumer. */
    usb_audio_pcm_snapshot(&observed); assert(observed.known && !observed.current && observed.consumer_generation == 17);
    service(1); assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &token, 18, true) == AE_OK);
    memset(input_data, 0, UAC_FRAME_BYTES); input_bytes = UAC_FRAME_BYTES; input_offset = 0; service(1);
    assert(usb_audio_receive(&frame) == AE_OK); usb_audio_pcm_snapshot(&observed);
    assert(observed.current && observed.consumer_generation == 18 && observed.frames == 1 && observed.samples == 480 &&
        observed.nonzero_samples == 0 && observed.peak_abs == 0 && observed.last_sequence == 1);
    usb_audio_stack_event(false, false); usb_audio_pcm_snapshot(&observed);
    assert(observed.known && !observed.current && observed.frames == 1);
    usb_audio_pcm_snapshot(NULL);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    void (*const cases[])(void) = {capture_and_backpressure, generations_and_stop,
        playback_frames_and_cancel, format_and_clock_requests, failed_allocation_stays_unavailable,
        worker_failure_releases_memory, saturation_and_time_failure, many_reconnections,
        same_alternate_and_resume_require_fresh_preparation, endpoint_close_revokes_before_reopen,
        feedback_tracks_codec_consumption, dropped_frames_preserve_stream_offsets, playback_binding_barriers,
        other_profiles_keep_existing_admission, playback_content_observation,
        capture_batch_guards, capture_invalid_batch_write, capture_consumer_scope};
    unsigned index = (unsigned)strtoul(argv[1], NULL, 10);
    assert(index < sizeof(cases) / sizeof(cases[0]));
    cases[index]();
    assert(!lock_depth);
    if (s_memory) { heap_caps_free(s_memory); s_memory = NULL; }
    printf("PASS: USB audio case %u\n", index);
    return 0;
}
