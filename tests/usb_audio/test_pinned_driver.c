// SPDX-License-Identifier: Apache-2.0
#define USB_AUDIO_REAL_STACK 1
#define main fifo_fixture_main
#include "test_usb_audio.c"
#undef main
#include "device/usbd_pvt.h"

bool stack_out_armed;
static bool reject_rearm;
static unsigned out_rearms;
bool stack_in_armed;
static uint8_t *in_data;
static uint16_t in_bytes;
static unsigned in_packet_sizes;
static uint8_t completed_data[8192];
static size_t completed_bytes;
void stack_driver_init(void);
bool stack_complete_out(const uint8_t *, uint16_t);
void stack_driver_reset(void);
void stack_out_overwrite(bool);
bool stack_in_overwritable(void);

/* USBD/DCD submission is the boundary. No read, write, clear or FIFO routine
 * is replaced: those execute the actual pinned TinyUSB implementation. */
bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *data, uint16_t bytes, bool is_isr)
{
    (void)is_isr;
    assert(rhport == 0 && data);
    if (ep == 0x04) {
        assert(bytes == 98 && !stack_out_armed);
        if (reject_rearm) return false;
        stack_out_armed = true; ++out_rearms;
    } else if (ep == 0x84) {
        assert(!stack_in_armed && bytes <= 98 && !(bytes & 1U));
        stack_in_armed = true; in_data = data; in_bytes = bytes;
        if (bytes == 94) in_packet_sizes |= 1U;
        if (bytes == 96) in_packet_sizes |= 2U;
        if (bytes == 98) in_packet_sizes |= 4U;
    }
    return true;
}
bool usbd_edpt_claim(uint8_t rhport, uint8_t ep) { (void)rhport; (void)ep; return true; }
bool usbd_edpt_release(uint8_t rhport, uint8_t ep) { (void)rhport; (void)ep; return true; }
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep) { (void)rhport; (void)ep; return false; }
bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request, void *buffer, uint16_t bytes)
{ (void)rhport; (void)request; (void)buffer; (void)bytes; abort(); }
bool tud_control_status(uint8_t rhport, const tusb_control_request_t *request)
{ (void)rhport; (void)request; abort(); }

static void test_out(void)
{
    stack_driver_init();
    assert(usb_audio_init() == ESP_OK); usb_audio_stack_started(ESP_OK); usb_audio_stack_event(true, false);
    select_interface(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 1);
    select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); service_io();
    assert(stack_complete_out(NULL, 0));
    if (!stack_out_armed)
        fprintf(stderr, "FAIL: actual DMA OUT class consumed ZLP without rearming; FIFO=%u rearms=%u\n",
            tud_audio_available(), out_rearms);
    assert(stack_out_armed && out_rearms == 2 && tud_audio_available() == 0);
    ae_frame_t frame;
    assert(usb_audio_receive(&frame) != AE_OK && !s_rx_sequence && !s_sample_offset);
    uint8_t packet[96]; memset(packet, 0x27, sizeof(packet));
    for (unsigned i = 0; i < 20; ++i) assert(stack_complete_out(packet, sizeof(packet)));
    ae_route_capabilities_t caps; assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &caps.token) == AE_AGAIN);
    service_io(); assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &caps.token) == AE_OK);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, true) == AE_AGAIN);
    service_io(); assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, true) == AE_OK);
    assert(tud_audio_available() == 0); /* Unconsented pre-start bytes were purged. */
    for (unsigned i = 0; i < 10; ++i) {
        assert(stack_complete_out(NULL, 0) && stack_out_armed);
        assert(stack_complete_out(packet, sizeof(packet))); service_io(); now_us += 1000;
    }
    assert(usb_audio_receive(&frame) == AE_OK && frame.bytes == 960 && frame.sequence == 1 && frame.sample_offset == 0 &&
        frame.consumer_generation == 17 && frame.data[0] == 0x27 && !(s_roles & AE_MICROPHONE));
    usb_audio_snapshot_t snapshot = usb_audio_snapshot();
    assert(snapshot.out_zero_callbacks == 11 && snapshot.out_callback_bytes == 2880 &&
        snapshot.playback_read_bytes == 960 && snapshot.playback_frames_enqueued == 1 && snapshot.playback_bound);
    for (unsigned i = 0; i < 100; ++i) {
        assert(stack_complete_out(NULL, 0)); service_io(); now_us += 1000;
    }
    assert(usb_audio_receive(&frame) == AE_AGAIN && s_rx_sequence == 1 && s_sample_offset == 480);
    assert(usb_audio_snapshot().playback_read_bytes == 960); /* Empty is not silence PCM. */
    for (unsigned i = 0; i < 3; ++i) { assert(stack_complete_out(packet, sizeof(packet))); service_io(); }
    assert(s_input_bytes == 288);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 18, true) == AE_AGAIN);
    assert(usb_audio_snapshot().playback_flush_pending); service_io();
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 18, true) == AE_OK);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, false) == AE_OK);
    memset(packet, 0x3a, sizeof(packet));
    for (unsigned i = 0; i < 10; ++i) { assert(stack_complete_out(packet, sizeof(packet))); service_io(); }
    assert(usb_audio_receive(&frame) == AE_OK && frame.consumer_generation == 18 && frame.sequence == 1 &&
        frame.sample_offset == 0 && frame.data[0] == 0x3a);
    uint64_t observed = usb_audio_snapshot().out_callbacks;
    reject_rearm = true; assert(stack_complete_out(packet, sizeof(packet)));
    assert(!stack_out_armed && !stack_complete_out(packet, sizeof(packet)) && usb_audio_snapshot().out_callbacks == observed);
    assert(tud_audio_available() == 96); /* Real rearm failure is not fabricated success. */
    reject_rearm = false;
    stack_driver_reset(); usb_audio_stack_event(false, false); service_io();
    assert(!usb_audio_snapshot().playback_bound && usb_audio_receive(&frame) != AE_OK);
    stack_driver_init(); usb_audio_stack_event(true, false);
    select_interface(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 1); select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); service_io();
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 12, &caps.token) == AE_STALE);
    assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_AGAIN);
    service_io(); assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_OK);
    assert(stop(NULL, AE_ROUTE_USB_UAC, 12, &caps.token) == AE_AGAIN); service_io();
    assert(stop(NULL, AE_ROUTE_USB_UAC, 12, &caps.token) == AE_OK && !s_tx_count && !s_rx_count);
    stack_driver_reset(); stack_driver_init(); stack_out_overwrite(false);
    for (unsigned i = 0; i < 4; ++i) assert(stack_complete_out(packet, sizeof(packet)));
    assert(stack_complete_out(packet, 8) && tud_audio_available() == 392 && stack_out_armed);
    assert(stack_complete_out(packet, sizeof(packet)) && !stack_out_armed); /* Preserve positive FIFO failure. */
    puts("PASS: real TinyUSB DMA class/FIFO ZLP rearm, no fake PCM and consent-bound complete playback frame");
    puts("PASS: flush/consumer replacement, real rearm/FIFO errors and reset identity");
}


static void complete_in(void)
{
    assert(stack_in_armed && completed_bytes + in_bytes <= sizeof(completed_data));
    memcpy(completed_data + completed_bytes, in_data, in_bytes); completed_bytes += in_bytes;
    uint16_t bytes = in_bytes; stack_in_armed = false;
    assert(audiod_xfer_isr(0, 0x84, XFER_RESULT_SUCCESS, bytes));
}

static void test_capture(void)
{
    stack_driver_init();
    assert(usb_audio_init() == ESP_OK); usb_audio_stack_started(ESP_OK); usb_audio_stack_event(true, false);
    select_interface(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 1);
    select_interface(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); service_io();
    ae_route_capabilities_t caps; assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 11, &caps.token) == AE_AGAIN);
    service_io(); assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 11, &caps.token) == AE_OK);
    uint8_t expected[UAC_QUEUE_FRAMES * UAC_FRAME_BYTES], pending[392];
    for (uint64_t i = 1; i <= UAC_QUEUE_FRAMES; ++i) {
        ae_frame_t frame = make_frame(caps.token, i);
        memcpy(expected + (i - 1) * UAC_FRAME_BYTES, frame.data, UAC_FRAME_BYTES);
        assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    }
    /* Keep the original comparison at exactly twenty FIFO write attempts,
     * independently of how many bounded attempts the worker makes per wake. */
    for (unsigned i = 0; i < 20 && usb_audio_snapshot().capture_write_calls < 20; ++i) {
        service_io(); now_us += 1000;
    }
    usb_audio_snapshot_t snapshot = usb_audio_snapshot();
    uint16_t queued = tu_fifo_count(tud_audio_n_get_ep_in_ff(0));
    if (snapshot.capture_write_bytes != sizeof(pending))
        fprintf(stderr, "FAIL: actual IN FIFO accepted %llu bytes without completions; retained=%u\n",
            (unsigned long long)snapshot.capture_write_bytes, queued);
    assert(snapshot.capture_write_bytes == sizeof(pending) && queued == sizeof(pending));
    assert(tu_fifo_peek_n(tud_audio_n_get_ep_in_ff(0), pending, sizeof(pending)) == sizeof(pending));
    assert(!memcmp(pending, expected, sizeof(pending))); /* Oldest unread PCM survives. */
    assert(snapshot.in_callbacks == 0 && snapshot.capture_write_calls == 20 &&
        snapshot.queued_capture_frames == UAC_QUEUE_FRAMES && s_memory->tx[s_tx_head].offset == 392 &&
        !snapshot.playback_bound);
    ae_frame_t next = make_frame(caps.token, 5);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_AGAIN);
    assert(usb_audio_snapshot().rejected_capture_frames == 1);
    /* Real completion copies the pending DCD packet to the simulated host,
     * then the actual class consumes its FIFO and arms the following packet. */
    for (unsigned i = 0; i < 60; ++i) {
        complete_in();
        /* Include a four-millisecond wake gap at the real class's blackout
         * boundary. Batch admission otherwise keeps the FIFO above its
         * low-fill threshold, leaving the 94-byte branch unexercised. */
        bool wake = i % 4 != 3 || i == 7;
        if (i >= 8 && i <= 10) wake = false;
        if (wake) service_io();
        now_us += 1000;
        snapshot = usb_audio_snapshot(); queued = tu_fifo_count(tud_audio_n_get_ep_in_ff(0));
        assert(snapshot.capture_write_bytes == completed_bytes + in_bytes + queued);
        assert(completed_bytes <= sizeof(expected) && !memcmp(completed_data, expected, completed_bytes));
        assert(!memcmp(in_data, expected + completed_bytes, in_bytes));
        assert(tu_fifo_peek_n(tud_audio_n_get_ep_in_ff(0), pending, queued) == queued);
        assert(!memcmp(pending, expected + completed_bytes + in_bytes, queued));
    }
    fprintf(stderr, "IN resume: queued=%u accepted=%llu completed=%zu callbacks=%llu callback_bytes=%llu rejected=%llu pending=%u fifo=%u packet_sizes=%u\n",
        s_tx_count, (unsigned long long)snapshot.capture_write_bytes, completed_bytes,
        (unsigned long long)snapshot.in_callbacks, (unsigned long long)snapshot.in_callback_bytes,
        (unsigned long long)snapshot.rejected_capture_frames, in_bytes, queued, in_packet_sizes);
    assert(!s_tx_count && snapshot.capture_write_bytes == sizeof(expected) &&
        snapshot.in_callback_bytes == completed_bytes && snapshot.in_callbacks == 60 &&
        completed_bytes > sizeof(expected) - 94 && snapshot.rejected_capture_frames == 1 && in_packet_sizes == 7U);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_OK); /* Same rejected sequence can retry. */
    service_io(); uint16_t partial = s_memory->tx[s_tx_head].offset;
    assert(partial && partial < UAC_FRAME_BYTES);
    uint64_t accepted = usb_audio_snapshot().capture_write_bytes;
    now_us += UAC_FRAME_AGE_US; service_io(); snapshot = usb_audio_snapshot();
    assert(!s_tx_count && snapshot.rejected_capture_frames == 2 && snapshot.capture_write_bytes == accepted);
    assert(stop(NULL, AE_ROUTE_USB_UAC, 11, &caps.token) == AE_AGAIN); service_io();
    assert(stop(NULL, AE_ROUTE_USB_UAC, 11, &caps.token) == AE_OK && !tu_fifo_count(tud_audio_n_get_ep_in_ff(0)));
    stack_driver_reset(); assert(!stack_in_overwritable());
    stack_driver_init(); assert(!stack_in_overwritable());
    puts("PASS: real IN FIFO retains oldest PCM under stalled host, bounded backpressure and exact completion/resume conservation");
    puts("PASS: partial write, unchanged expiry/rejection, stop flush and reset/reinit non-overwrite policy");
}

static void test_single_capture(void)
{
    stack_driver_init(); ae_route_token_t token = start(AE_MICROPHONE);
    ae_frame_t frame = make_frame(token, 1);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &frame) == AE_OK);
    for (unsigned i = 0; i < 4; ++i) service_io();
    for (unsigned i = 0; i < 20; ++i) {
        complete_in(); service_io(); now_us += 1000;
        usb_audio_snapshot_t snapshot = usb_audio_snapshot();
        unsigned queued = tu_fifo_count(tud_audio_n_get_ep_in_ff(0));
        if (snapshot.capture_write_bytes != completed_bytes + in_bytes + queued)
            fprintf(stderr, "FAIL: actual IN scheduled %u bytes after only %llu bytes remained\n",
                in_bytes, (unsigned long long)(snapshot.capture_write_bytes - completed_bytes - queued));
        assert(snapshot.capture_write_bytes == completed_bytes + in_bytes + queued);
        assert(completed_bytes + in_bytes <= frame.bytes && !memcmp(completed_data, frame.data, completed_bytes) &&
            !memcmp(in_data, frame.data + completed_bytes, in_bytes));
    }
    usb_audio_snapshot_t snapshot = usb_audio_snapshot();
    assert(!s_tx_count && !tu_fifo_count(tud_audio_n_get_ep_in_ff(0)) && !in_bytes &&
        snapshot.capture_write_bytes == 960 && completed_bytes == 960 && snapshot.in_callback_bytes == 960 &&
        snapshot.in_callbacks == 20 && snapshot.in_zero_callbacks == 10 &&
        in_packet_sizes == 7U && !snapshot.rejected_capture_frames);
    puts("PASS: finite single-frame IN sends exact960 PCM bytes, including short94 tail, then empty packets only");
}

static uint8_t capture_byte(size_t offset)
{
    return (uint8_t)(offset % UAC_FRAME_BYTES + offset / UAC_FRAME_BYTES + 1U);
}

static void test_capture_consumer_restart(void)
{
    stack_driver_init(); ae_route_token_t token = start(AE_MICROPHONE | AE_SPEAKER);
    ae_frame_t old = make_frame(token, 7), next = make_frame(token, 1);
    next.consumer_generation = 18;
    assert(capture(NULL, AE_ROUTE_USB_UAC, &old) == AE_OK); service_io();
    assert(tu_fifo_count(tud_audio_n_get_ep_in_ff(0)) == 392 && s_memory->tx[s_tx_head].offset == 392);
    uint8_t packet[96]; memset(packet, 0xa5, sizeof(packet));
    assert(stack_complete_out(packet, sizeof(packet))); service_io();
    assert(s_input_bytes == 96); assert(stack_complete_out(packet, sizeof(packet)));
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_AGAIN && !s_tx_count);
    old.sequence = UINT64_MAX; assert(capture(NULL, AE_ROUTE_USB_UAC, &old) == AE_STALE);
    service_io();
    assert(!tu_fifo_count(tud_audio_n_get_ep_in_ff(0)) && tud_audio_available() == 96 && s_input_bytes == 96 && s_consumer_generation == 17);
    assert(capture(NULL, AE_ROUTE_USB_UAC, &next) == AE_OK); service_io();
    assert(s_input_bytes == 192 && !s_status.dropped_playback_frames);
    for (unsigned i = 0; i < s_input_bytes; ++i) assert(s_memory->input[i] == 0xa5);
    uint8_t fifo[392]; assert(tu_fifo_peek_n(tud_audio_n_get_ep_in_ff(0), fifo, sizeof(fifo)) == sizeof(fifo));
    assert(!memcmp(fifo, next.data, sizeof(fifo)));
    for (unsigned i = 0; i < 20; ++i) { complete_in(); service_io(); now_us += 1000; }
    assert(completed_bytes == next.bytes && !memcmp(completed_data, next.data, next.bytes));
    assert(s_consumer_generation == 17 && s_input_bytes == 192 && !s_status.dropped_playback_frames);
    assert(s_status.capture_write_bytes == 392 + next.bytes); /* Retired FIFO prefix was cleared, never padded/delivered. */
    assert(capture(NULL, AE_ROUTE_USB_UAC, &old) == AE_STALE);
    puts("PASS: real IN FIFO replacement purges retired partial capture, rejects late high-sequence frames and preserves OUT FIFO/partial Speaker data");
}

static void test_duplex_capture_jitter(void)
{
    stack_driver_init(); ae_route_token_t token = start(AE_MICROPHONE | AE_SPEAKER);
    size_t produced = 0, completed = 0, playback_bytes = 0;
    unsigned peak_queued = 0;
    uint8_t packet[96], fifo[392];
    for (unsigned ms = 0; ms < 10000; ++ms) {
        if (ms % 10U == 0) {
            ae_frame_t frame = make_frame(token, produced / UAC_FRAME_BYTES + 1U);
            ae_result_t result = capture(NULL, AE_ROUTE_USB_UAC, &frame);
            if (result != AE_OK) {
                usb_audio_snapshot_t state = usb_audio_snapshot();
                fprintf(stderr, "FAIL: continuous capture rejected at ms=%u result=%d produced=%zu accepted=%llu completed=%zu queued=%u\n",
                    ms, result, produced, (unsigned long long)state.capture_write_bytes, completed, state.queued_capture_frames);
            }
            assert(result == AE_OK); produced += frame.bytes;
        }
        /* One real IN completion and one 96-byte OUT packet per millisecond.
         * Varied PCM is checked in place; the test retains no stream history. */
        assert(stack_in_armed);
        for (uint16_t i = 0; i < in_bytes; ++i) assert(in_data[i] == capture_byte(completed + i));
        completed += in_bytes;
        uint16_t bytes = in_bytes; stack_in_armed = false;
        assert(audiod_xfer_isr(0, 0x84, XFER_RESULT_SUCCESS, bytes));
        for (unsigned i = 0; i < sizeof(packet); ++i) packet[i] = (uint8_t)((size_t)ms * sizeof(packet) + i);
        assert(stack_complete_out(packet, sizeof(packet)));
        /* Bounded scheduling jitter: three wakes, then one missed wake. */
        if (ms % 4U != 3U) {
            usb_audio_snapshot_t before = usb_audio_snapshot();
            service_io(); service_feedback();
            usb_audio_snapshot_t after = usb_audio_snapshot();
            assert(after.capture_write_calls - before.capture_write_calls <= 4);
            assert(after.capture_write_bytes - before.capture_write_bytes <= 392);
        }
        ae_frame_t playback;
        while (usb_audio_receive(&playback) == AE_OK) {
            assert(playback.sample_offset * 2U == playback_bytes);
            for (unsigned i = 0; i < playback.bytes; ++i) assert(playback.data[i] == (uint8_t)(playback_bytes + i));
            playback_bytes += playback.bytes;
            playback_consumed(NULL, AE_ROUTE_USB_UAC, &playback);
        }
        usb_audio_snapshot_t state = usb_audio_snapshot();
        uint16_t queued = tu_fifo_count(tud_audio_n_get_ep_in_ff(0));
        assert(state.capture_write_bytes == completed + in_bytes + queued);
        assert(tu_fifo_peek_n(tud_audio_n_get_ep_in_ff(0), fifo, queued) == queued);
        for (uint16_t i = 0; i < in_bytes; ++i) assert(in_data[i] == capture_byte(completed + i));
        for (uint16_t i = 0; i < queued; ++i) assert(fifo[i] == capture_byte(completed + in_bytes + i));
        size_t application_bytes = 0;
        for (unsigned i = 0; i < s_tx_count; ++i)
            application_bytes += UAC_FRAME_BYTES - s_memory->tx[(s_tx_head + i) % UAC_QUEUE_FRAMES].offset;
        assert(produced == state.capture_write_bytes + application_bytes);
        assert(!state.rejected_capture_frames && !state.dropped_playback_frames && state.prepared);
        if (s_tx_count > peak_queued) peak_queued = s_tx_count;
        now_us += 1000;
    }
    usb_audio_snapshot_t state = usb_audio_snapshot();
    assert(produced == 960000 && state.capture_write_bytes == produced && completed >= produced - UAC_FRAME_BYTES);
    assert(playback_bytes >= produced - UAC_FRAME_BYTES && peak_queued < UAC_QUEUE_FRAMES);
    printf("PASS: 10s real-class %s duplex with one missed wake per4ms: produced=%zu accepted=%llu completed=%zu pending=%u fifo=%u peak_queue=%u playback=%zu; exact PCM and finite-FIFO conservation\n",
        high_speed ? "HS" : "FS", produced, (unsigned long long)state.capture_write_bytes,
        completed, in_bytes, tu_fifo_count(tud_audio_n_get_ep_in_ff(0)), peak_queued, playback_bytes);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "out")) test_out();
    else if (!strcmp(argv[1], "restart_fs") || !strcmp(argv[1], "restart_hs")) {
        high_speed = !strcmp(argv[1], "restart_hs"); test_capture_consumer_restart();
    }
    else if (!strcmp(argv[1], "duplex_fs") || !strcmp(argv[1], "duplex_hs")) {
        high_speed = !strcmp(argv[1], "duplex_hs"); test_duplex_capture_jitter();
    }
    else {
        bool single = !strcmp(argv[1], "single_fs") || !strcmp(argv[1], "single_hs");
        assert(single || !strcmp(argv[1], "capture_fs") || !strcmp(argv[1], "capture_hs"));
        high_speed = !strcmp(argv[1], "capture_hs") || !strcmp(argv[1], "single_hs");
        if (single) test_single_capture(); else test_capture();
    }
    return 0;
}
