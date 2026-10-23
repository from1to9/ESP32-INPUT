// SPDX-License-Identifier: Apache-2.0
#define USB_AUDIO_REAL_STACK 1
#define main fifo_fixture_main
#include "test_usb_audio.c"
#undef main
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "dwc2_harness.h"
void dcd_audio_open(bool);
void dcd_audio_alt(uint16_t, uint16_t);
bool dcd_audio_complete(uint8_t, uint8_t, uint32_t);
unsigned dcd_audio_interval(bool, uint8_t);
void dcd_audio_clock_request(void);
uint8_t *usbd_get_ctrl_buf(void) { static uint8_t buffer[64]; return buffer; }
bool usbd_edpt_xfer(uint8_t port, uint8_t ep, uint8_t *data, uint16_t bytes, bool in_isr)
{ (void)in_isr; assert(!port && !lock_depth); return dwc2_test_submit(ep, data, bytes); }
bool usbd_edpt_iso_alloc(uint8_t port, uint8_t ep, uint16_t bytes)
{ assert(!port); return dcd_edpt_iso_alloc(port, ep, bytes); }
bool usbd_edpt_iso_activate(uint8_t port, const tusb_desc_endpoint_t *ep)
{ assert(!port); return dwc2_test_open(ep); }
void usbd_edpt_clear_stall(uint8_t port, uint8_t ep) { (void)ep; assert(!port); }
void usbd_sof_enable(uint8_t port, sof_consumer_t consumer, bool enabled)
{ (void)consumer; assert(!port && !enabled); }
bool usbd_edpt_claim(uint8_t port, uint8_t ep) { (void)ep; assert(!port); return true; }
bool usbd_edpt_release(uint8_t port, uint8_t ep) { (void)ep; assert(!port); return true; }
bool usbd_edpt_busy(uint8_t port, uint8_t ep) { (void)ep; assert(!port); return false; }
bool tud_control_status(uint8_t port, const tusb_control_request_t *request)
{ (void)request; assert(!port); return true; }
bool tud_control_xfer(uint8_t port, const tusb_control_request_t *request, void *data, uint16_t bytes)
{ assert(!port && request && data && bytes == 4 && tu_unaligned_read32(data) == 48000); return true; }
int main(int argc, char **argv)
{
    assert(argc == 4);
    high_speed = !strcmp(argv[1], "hs"); unsigned phase = (unsigned)strtoul(argv[2], NULL, 10);
    unsigned delay = (unsigned)strtoul(argv[3], NULL, 10);
    unsigned interval = dcd_audio_interval(high_speed, 0x04);
    dwc2_test_init(high_speed); dwc2_test_frame((phase + 0x3fffU) & 0x3fffU);
    assert(!s_status.initialized && !s_status.link_generation && !allocations && !workers);
    dwc2_test_completion(dcd_audio_complete); dcd_audio_open(high_speed);
    assert(usb_audio_init() == ESP_OK); usb_audio_stack_started(ESP_OK); usb_audio_stack_event(true, false);
    dcd_audio_alt(INPUT_USB_AUDIO_PLAYBACK_INTERFACE, 1);
    dcd_audio_alt(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); service_io();
    ae_route_capabilities_t caps; assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK);
    assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &caps.token) == AE_AGAIN);
    service_io(); assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &caps.token) == AE_OK);
    assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, true) == AE_AGAIN);
    service_io(); assert(bind_playback(NULL, AE_ROUTE_USB_UAC, 11, &caps.token, 17, true) == AE_OK);
    for (unsigned i = 0; i < 32; ++i) {
        dwc2_test_frame(phase + i * interval); (void)dwc2_test_poll(0x04, 96, delay);
        service_io(); service_feedback(); now_us += 1000;
    }
    usb_audio_snapshot_t s = usb_audio_snapshot();
    if (s.out_callback_bytes != 3072)
        fprintf(stderr, "FAIL: actual DCD/class OUT stops at %llu bytes\n", (unsigned long long)s.out_callback_bytes);
    assert(s.out_callback_bytes == 3072);
    ae_frame_t frame;
    for (unsigned sequence = 1; sequence <= 3; ++sequence) {
        assert(usb_audio_receive(&frame) == AE_OK && frame.sequence == sequence && frame.bytes == 960 &&
            frame.sample_offset == (sequence - 1) * 480 && frame.consumer_generation == 17);
        for (unsigned i = 0; i < frame.bytes; ++i) assert(frame.data[i] == (uint8_t)((sequence - 1) * 960 + i));
    }
    usb_audio_transport_snapshot_t transport;
    dwc2_test_snapshot_t before = dwc2_test_snapshot(0x04);
    for (unsigned i = 0; i < 100; ++i) usb_audio_transport_snapshot(&transport);
    dwc2_test_snapshot_t after = dwc2_test_snapshot(0x04);
    assert(before.ctl == after.ctl && before.tsiz == after.tsiz && before.submitted == after.submitted &&
        before.completed == after.completed && transport.out.completed_ok == 32 && transport.out.successful_bytes == 3072 &&
        transport.class_out_entries == 32 && transport.class_out_rearm_calls == 32 && !transport.class_out_rearm_rejected);
    if (INPUT_USB_AUDIO_PTI_REQUIRED) {
        for (unsigned i = 0; i < 8; ++i) {
            dwc2_test_frame(phase + (32 + i) * interval);
            assert(dwc2_test_poll(0x85, 4, 0));
            assert(dwc2_test_poll(0x84, 0, 0));
        }
        usb_audio_transport_snapshot(&transport);
        uint32_t scale = high_speed ? 8 : 1;
        assert(transport.feedback.completed_ok == 8 && transport.feedback.successful_bytes == 32 &&
            transport.feedback_submitted_known && transport.feedback_completed_known &&
            transport.feedback_completed_length == 4 && transport.feedback_completed_epoch == transport.reset_epoch &&
            transport.feedback_completed >= (47U << 16) / scale && transport.feedback_completed <= (49U << 16) / scale &&
            transport.in.completed_ok == 8 && transport.in.successful_zero == 8);
        assert(!(s_roles & AE_MICROPHONE) && !s_status.capture_write_calls);
        dwc2_test_frame(phase + 40 * interval); assert(dwc2_test_poll(0x04, 0, 0));
        dwc2_test_frame(phase + 41 * interval); assert(dwc2_test_out_drop());
        dwc2_test_failed_in(0x84); dwc2_test_failed_in(0x85);
        usb_audio_transport_snapshot(&transport);
        assert(transport.out.completed_ok == 33 && transport.out.successful_zero == 1 && transport.out.completed_failed == 1 &&
            transport.out.successful_bytes == 3072 && transport.in.completed_failed == 1 && transport.feedback.completed_failed == 1 &&
            transport.in.successful_zero == 8 && transport.feedback.successful_bytes == 32 &&
            s_status.in_zero_callbacks == 9 && s_input_bytes == 192 && usb_audio_receive(&frame) == AE_AGAIN);
        /* Existing capture interrupts provide active sampling contexts while
         * OUT remains pending. No diagnostic timer/interrupt is introduced. */
        for (unsigned i = 0; i < 128; ++i) {
            dwc2_test_frame(phase + (42 + i) * interval);
            assert(dwc2_test_poll(0x84, 0, 0)); now_us += 1000;
        }
        usb_audio_transport_snapshot(&transport);
        assert(transport.pending_registers_known && transport.pending_registers.point == USB_AUDIO_REG_PENDING_IRQ &&
            transport.pending_registers.epoch_before == transport.reset_epoch &&
            transport.pending_registers.epoch_before == transport.pending_registers.epoch_after);
        dwc2_test_reject_out_rearm(); dwc2_test_frame(phase + 170 * interval);
        assert(dwc2_test_poll(0x04, 96, 0)); usb_audio_transport_snapshot(&transport);
        assert(transport.out.rejected == 1 && transport.class_out_rearm_rejected == 1 &&
            transport.class_out_entries == transport.class_out_rearm_calls &&
            transport.class_out_entries == s_status.out_callbacks + 1);
        uint32_t submits = dwc2_test_snapshot(0x04).submitted;
        for (unsigned i = 0; i < 10; ++i) service_io();
        assert(dwc2_test_snapshot(0x04).submitted == submits && !s_rx_count && s_status.dropped_playback_frames == 1);
        ae_route_token_t old = caps.token;
        dwc2_test_unknown_mode(); usb_audio_transport_snapshot(&transport);
        assert(!transport.mode_known && !transport.mode_valid && !live_locked() && !s_consumer_generation && !s_status.prepared);
        assert(!dwc2_test_mode_fault(0));
        assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &old) == AE_STALE);
        service_io(); assert(!s_rx_count && !s_input_bytes);
        for (uint32_t reject = 1; reject <= USB_AUDIO_MODE_THRESHOLD_ACTIVE; reject <<= 1) {
            assert(dwc2_test_mode_fault(reject) == reject); usb_audio_transport_snapshot(&transport);
            assert(transport.mode_known && !transport.mode_valid && transport.mode_reject == reject && !live_locked());
            assert(!dwc2_test_mode_fault(0));
        }
        uint64_t completed = transport.out.completed_ok;
        dwc2_test_reset(); usb_audio_stack_event(false, false); usb_audio_stack_event(true, false); service_io();
        usb_audio_transport_snapshot(&transport);
        assert(transport.out.completed_ok == completed && transport.first_mode_reject == 1 &&
            !s_status.capture_selected && !s_status.playback_selected && !s_status.prepared && !s_consumer_generation);
        assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_SPEAKER, 11, &old) != AE_OK);
        audiod_reset(0); dcd_audio_open(high_speed);
        dcd_audio_alt(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); dcd_audio_clock_request(); service_io();
        assert(capabilities(NULL, AE_ROUTE_USB_UAC, &caps) == AE_OK && caps.capture && !caps.playback);
        assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_AGAIN);
        service_io(); assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_OK);
        ae_frame_t captured = make_frame(caps.token, 1); captured.audio_generation = 12;
        assert(capture(NULL, AE_ROUTE_USB_UAC, &captured) == AE_OK);
        usb_audio_transport_snapshot(&transport); uint64_t prior_in = transport.in.completed_ok;
        uint64_t prior_bytes = transport.in.successful_bytes;
        for (unsigned i = 0; i < 20; ++i) { service_io(); now_us += 1000; }
        assert(tu_fifo_count(tud_audio_n_get_ep_in_ff(0)) == 392 && s_tx_count == 1 && s_memory->tx[s_tx_head].offset == 392);
        usb_audio_transport_snapshot(&transport); assert(transport.in.completed_ok == prior_in);
        uint8_t received[960], pending[98]; unsigned received_bytes = 0;
        for (unsigned i = 0; i < 20; ++i) {
            uint16_t bytes = dwc2_test_copy_pending(0x84, pending, sizeof(pending));
            assert(received_bytes + bytes <= sizeof(received));
            memcpy(received + received_bytes, pending, bytes); received_bytes += bytes;
            dwc2_test_frame(phase + (100 + i) * interval); assert(dwc2_test_poll(0x84, bytes, 0));
            service_io(); now_us += 1000;
        }
        usb_audio_transport_snapshot(&transport);
        assert(received_bytes == 960 && !memcmp(received, captured.data, sizeof(received)) &&
            transport.in.successful_bytes == prior_bytes + 960 && !s_tx_count && !tu_fifo_count(tud_audio_n_get_ep_in_ff(0)));
        assert(stop(NULL, AE_ROUTE_USB_UAC, 12, &caps.token) == AE_AGAIN); service_io();
        assert(stop(NULL, AE_ROUTE_USB_UAC, 12, &caps.token) == AE_OK);
        dcd_audio_alt(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 0);
        assert(!s_status.capture_selected && !s_roles && !s_consumer_generation);
        dcd_audio_alt(INPUT_USB_AUDIO_CAPTURE_INTERFACE, 1); service_io();
        assert(prepare(NULL, AE_ROUTE_USB_UAC, &s_format, AE_MICROPHONE, 12, &caps.token) == AE_STALE);
        uint32_t calls = dwc2_test_snapshot(0x84).submitted;
        s_transport.in.submitted = s_transport.in.accepted = s_transport.in.rejected = UINT64_MAX;
        s_transport.in.completed_ok = s_transport.in.completed_failed = s_transport.in.successful_bytes = s_transport.in.successful_zero = UINT64_MAX;
        input_usb_audio_dcd_transfer(transport.reset_epoch, 0x84, false, XFER_RESULT_SUCCESS, 2, 0, 0, true);
        input_usb_audio_dcd_transfer(transport.reset_epoch, 0x84, true, XFER_RESULT_SUCCESS, 2, 0, 0, true);
        input_usb_audio_dcd_transfer(transport.reset_epoch, 0x84, true, XFER_RESULT_FAILED, 0, 0, 0, true);
        input_usb_audio_dcd_transfer(transport.reset_epoch, 0x81, true, XFER_RESULT_SUCCESS, 64, 0, 0, true);
        usb_audio_transport_snapshot(&transport);
        assert(transport.in.submitted == UINT64_MAX && transport.in.completed_ok == UINT64_MAX &&
            transport.in.completed_failed == UINT64_MAX && transport.in.successful_bytes == UINT64_MAX &&
            dwc2_test_snapshot(0x84).submitted == calls && !s_roles && !s_consumer_generation);
    }
    printf("PASS: actual DCD/class/FIFO/production SET_INTERFACE PTI%d received%llu enqueued%llu\n",
        CFG_TUD_DWC2_PTI_ENABLE, (unsigned long long)s.playback_read_bytes, (unsigned long long)s.playback_frames_enqueued);
    return 0;
}
