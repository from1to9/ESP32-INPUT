// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    USB_AUDIO_MODE_NOT_INTERNAL_DMA = 1U,
    USB_AUDIO_MODE_DMA_DISABLED = 2U,
    USB_AUDIO_MODE_DESCRIPTOR_DMA = 4U,
    USB_AUDIO_MODE_PTI_NOT_ACTIVE = 8U,
    USB_AUDIO_MODE_THRESHOLD_ACTIVE = 16U,
    USB_AUDIO_MODE_EPOCH_EXHAUSTED = 32U,
};
enum { USB_AUDIO_REG_MODE = 1, USB_AUDIO_REG_OUT_SUBMIT = 2, USB_AUDIO_REG_PENDING_IRQ = 3 };
typedef struct {
    /* Cached, non-atomic active-context reads. frame_* are raw DSTS words;
     * matching epoch/frame pairs do not establish an atomic hardware instant. */
    uint32_t epoch_before, epoch_after, frame_before, frame_after, out_activation;
    uint32_t core_id, hwcfg2, gahbcfg, dcfg, dctl, dthrctl;
    uint32_t gintsts, gintmsk, daint, out_ctl, out_int, out_size;
    uint8_t point;
} usb_audio_dcd_registers_t;
typedef struct {
    /* Lifetime DCD call/return totals. submitted counts attempts; successful
     * completion bytes/zero packets exclude every failed completion. */
    uint64_t submitted, accepted, rejected;
    uint64_t completed_ok, completed_failed, successful_bytes, successful_zero;
    bool first_failure_known;
    uint32_t first_failure_epoch;
    uint8_t first_failure_result;
} usb_audio_endpoint_observation_t;
typedef struct {
    bool observed, mode_required, mode_known, mode_valid;
    uint32_t reset_epoch, mode_reject, first_mode_reject, first_mode_reject_epoch;
    uint64_t class_out_entries, class_out_rearm_calls, class_out_rearm_rejected;
    usb_audio_endpoint_observation_t out, in, feedback;
    bool feedback_calculated_known, feedback_submitted_known, feedback_completed_known;
    uint32_t feedback_calculated, feedback_submitted, feedback_completed;
    uint32_t feedback_calculated_epoch, feedback_submitted_epoch, feedback_completed_epoch;
    uint8_t feedback_submitted_length, feedback_completed_length;
    /* Samples can be stale after their tagged epoch/activation. No getter
     * accesses MMIO or claims a current power/endpoint state. */
    bool registers_known, pending_registers_known;
    usb_audio_dcd_registers_t registers, pending_registers;
} usb_audio_transport_snapshot_t;

/* Explicit hardware lifecycle entry, independent of diagnostics collection.
 * Unknown/new reset epochs revoke prior audio authority. No codec/USB calls. */
void input_usb_audio_dcd_mode(uint32_t epoch, bool known, uint32_t reject, bool in_isr);
/* Observation hooks count existing operations/results and copy metadata only. */
void input_usb_audio_dcd_transfer(uint32_t epoch, uint8_t endpoint, bool completion,
    uint8_t result, uint16_t bytes, uint32_t feedback_value, uint8_t feedback_length, bool in_isr);
void input_usb_audio_dcd_registers(const usb_audio_dcd_registers_t *registers, bool in_isr);
void input_usb_audio_class_out(bool rearm, bool accepted, uint8_t result, uint16_t bytes);
/* Copy only cached metadata; never read MMIO, configure hardware or change policy. */
void usb_audio_transport_snapshot(usb_audio_transport_snapshot_t *out);
