// SPDX-License-Identifier: Apache-2.0
/* The complete pinned DCD executes unchanged. This boundary supplies registers,
 * physical reset/FIFO handshakes, IRQ dispatch and cache operations, not USB logic. */
#include "tusb_option.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "portable/synopsys/dwc2/dwc2_type.h"
#include "dwc2_harness.h"
#include <assert.h>
#include <string.h>

#define TUSB_DWC2_COMMON_H
#define DWC2_EP_MAX 16
static dwc2_regs_t registers;
static const dwc2_controller_t _dwc2_controller[] = {
    {.reg_base = (uintptr_t)&registers, .ep_count = 16, .ep_in_count = 8, .otg_dfifo_depth = 1024}
};
enum { DWC2_CONTROLLER_COUNT = 1 };
static dwc2_regs_t *DWC2_REG(uint8_t rhport) { assert(!rhport); return &registers; }
static void dwc2_clock_init(uint8_t rhport, tusb_role_t role) { (void)rhport; (void)role; }
static bool dwc2_core_is_highspeed_phy(dwc2_regs_t *reg, bool prefer) { (void)reg; return prefer; }
static bool dwc2_core_init(uint8_t rhport, bool hs, bool dma) { (void)rhport; (void)hs; (void)dma; return true; }
static void dwc2_core_deinit(uint8_t rhport) { (void)rhport; }
static void dwc2_dcd_int_enable(uint8_t rhport) { (void)rhport; }
static void dwc2_dcd_int_disable(uint8_t rhport) { (void)rhport; }
static void dwc2_remote_wakeup_delay(void) {}
static void dfifo_flush_tx(dwc2_regs_t *reg, uint8_t ep) { (void)reg; (void)ep; }
static void dfifo_flush_rx(dwc2_regs_t *reg) { (void)reg; }

#include USB_AUDIO_DCD_SOURCE

typedef struct {
    bool target_odd;
    uint32_t submitted, completed, failed, bytes;
    uint8_t *data;
    uint16_t length;
} endpoint_model_t;
static endpoint_model_t model[16][2];
static uint32_t current_frame;
static bool high_speed;
static bool (*completion_callback)(uint8_t, uint8_t, uint32_t);
static bool reject_out_rearm;

void usbd_spin_lock(bool is_isr) { (void)is_isr; }
void usbd_spin_unlock(bool is_isr) { (void)is_isr; }
bool dcd_dcache_clean(const void *data, uint32_t bytes) { (void)data; (void)bytes; return true; }
bool dcd_dcache_invalidate(const void *data, uint32_t bytes) { (void)data; (void)bytes; return true; }
bool dcd_dcache_clean_invalidate(const void *data, uint32_t bytes) { (void)data; (void)bytes; return true; }

static void latch_command(uint8_t ep)
{
    unsigned n = ep & 15U, dir = ep >> 7;
    dwc2_dep_t *dep = dir ? &registers.epin[n] : &registers.epout[n];
    dwc2_depctl_t ctl = {.value = dep->ctl};
    /* Hardware command bits read as zero; DPID reports the selected parity. */
    if (ctl.set_data0_iso_even) model[n][dir].target_odd = false;
    if (ctl.set_data1_iso_odd) model[n][dir].target_odd = true;
    ctl.set_data0_iso_even = ctl.set_data1_iso_odd = 0;
    ctl.dpid_iso_odd = model[n][dir].target_odd;
    dep->ctl = ctl.value;
}
void dwc2_test_frame(uint32_t frame)
{
    current_frame = frame & 0x3fffU;
    dwc2_dsts_t dsts = {.value = 0};
    dsts.frame_number = current_frame; dsts.enum_speed = high_speed ? DCFG_SPEED_HIGH : DCFG_SPEED_FULL_30_60MHZ;
    registers.dsts = dsts.value;
}
void dwc2_test_init(bool hs)
{
    memset(&registers, 0, sizeof(registers)); memset(model, 0, sizeof(model));
    memset(xfer_status, 0, sizeof(xfer_status)); memset(&_dcd_data, 0, sizeof(_dcd_data));
    high_speed = hs; dwc2_test_frame(0);
    completion_callback = NULL;
    reject_out_rearm = false;
    dwc2_ghwcfg2_t hw = {.value = 0}; hw.arch = GHWCFG2_ARCH_INTERNAL_DMA; hw.num_dev_ep = 15;
    registers.ghwcfg2 = hw.value;
    dwc2_ghwcfg4_t fifo = {.value = 0}; fifo.dedicated_fifos = 1; registers.ghwcfg4 = fifo.value;
    registers.gahbcfg = GAHBCFG_DMAEN;
    registers.dthrctl = DTHRCTL_NONISOTHREN | DTHRCTL_ISOTHREN | DTHRCTL_RXTHREN;
    registers.gintmsk = GINTMSK_OEPINT | GINTMSK_IEPINT;
    tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = hs ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL};
    assert(dcd_init(0, &init));
    handle_bus_reset(0);
    dwc2_test_frame(0);
}
bool dwc2_test_open(const void *descriptor)
{
    const tusb_desc_endpoint_t *ep = descriptor;
    registers.gintsts |= GINTSTS_BOUTNAKEFF;
    registers.epout[ep->bEndpointAddress & 15].intr = DOEPINT_EPDISD;
    registers.epin[ep->bEndpointAddress & 15].intr = DIEPINT_EPDISD | DIEPINT_INEPNE;
    bool ok = dcd_edpt_iso_activate(0, ep);
    latch_command(ep->bEndpointAddress); registers.gintsts = 0;
    return ok;
}
bool dwc2_test_submit(uint8_t ep, uint8_t *data, uint16_t bytes)
{
    bool ok = dcd_edpt_xfer(0, ep, data, bytes, false);
    if (ok) {
        endpoint_model_t *m = &model[ep & 15][ep >> 7];
        ++m->submitted; m->data = data; m->length = bytes; latch_command(ep);
    }
    return ok;
}
void dcd_event_handler(const dcd_event_t *event, bool in_isr)
{
    assert(in_isr);
    if (event->event_id != DCD_EVENT_XFER_COMPLETE) return;
    uint8_t ep = event->xfer_complete.ep_addr;
    endpoint_model_t *m = &model[ep & 15][ep >> 7];
    ++m->completed; m->bytes += event->xfer_complete.len;
    if (event->xfer_complete.result != XFER_RESULT_SUCCESS) ++m->failed;
    if (ep == 0x04 && reject_out_rearm) {
        reject_out_rearm = false; xfer_status[4][TUSB_DIR_OUT].max_size = 0;
    }
    if (completion_callback) assert(completion_callback(ep, event->xfer_complete.result, event->xfer_complete.len));
    else assert(dwc2_test_submit(ep, m->data, m->length));
}
static bool poll_packet(uint8_t ep, uint16_t bytes, unsigned irq_delay, bool drop)
{
    unsigned n = ep & 15U, dir = ep >> 7;
    dwc2_dep_t *dep = dir ? &registers.epin[n] : &registers.epout[n];
    endpoint_model_t *m = &model[n][dir];
    assert(bytes <= m->length);
    bool matches = (registers.dctl & DCTL_IGNRFRMNUM) || m->target_odd == ((current_frame & 1U) != 0);
    if (!(dep->ctl & EPCTL_EPENA) || !matches) {
        registers.gintsts |= GINTSTS_PXFR_INCOMPISOOUT;
        dcd_int_handler(0); /* Masked OUT-incomplete does not manufacture completion. */
        return false;
    }
    if (!dir) {
        for (uint16_t i = 0; i < bytes; ++i) m->data[i] = (uint8_t)(m->bytes + i);
    }
    dep->ctl &= ~EPCTL_EPENA; /* Buffer-DMA hardware ends this transfer. */
    dwc2_ep_tsize_t size = {.value = dep->tsiz}; size.xfer_size = m->length - bytes; dep->tsiz = size.value;
    dep->intr = 1U | (drop ? 1U << 11 : 0); registers.daint = 1U << (n + (dir ? 0 : 16));
    registers.gintsts = dir ? GINTSTS_IEPINT : GINTSTS_OEPINT;
    dwc2_test_frame(current_frame + irq_delay);
    dcd_int_handler(0);
    dep->intr = 0; registers.daint = 0; registers.gintsts = 0; /* W1C interrupt acknowledgement. */
    return true;
}
bool dwc2_test_poll(uint8_t ep, uint16_t bytes, unsigned irq_delay) { return poll_packet(ep, bytes, irq_delay, false); }
bool dwc2_test_out_drop(void) { return poll_packet(0x04, 0, 0, true); }
void dwc2_test_reject_out_rearm(void) { reject_out_rearm = true; }
void dwc2_test_failed_in(uint8_t ep)
{
    assert(ep == 0x84 || ep == 0x85);
    registers.epin[ep & 15].ctl &= ~EPCTL_EPENA;
    /* Explicit failed-completion injection at the DCD reporting boundary;
     * this does not pretend that PTI's masked incomplete-IN interrupt fires. */
    input_usb_dcd_complete(0, ep, 0, XFER_RESULT_FAILED, true);
}
uint16_t dwc2_test_copy_pending(uint8_t ep, uint8_t *out, uint16_t capacity)
{
    endpoint_model_t *m = &model[ep & 15][ep >> 7];
    assert(m->length <= capacity && (out || !m->length));
    if (m->length) memcpy(out, m->data, m->length);
    return m->length;
}
dwc2_test_snapshot_t dwc2_test_snapshot(uint8_t ep)
{
    unsigned n = ep & 15U, dir = ep >> 7;
    dwc2_dep_t *dep = dir ? &registers.epin[n] : &registers.epout[n];
    endpoint_model_t *m = &model[n][dir];
    return (dwc2_test_snapshot_t){dep->ctl, dep->tsiz, registers.dctl, registers.gintsts, registers.gintmsk,
        m->submitted, m->completed, m->failed, m->bytes, m->target_odd};
}
void dwc2_test_completion(bool (*callback)(uint8_t, uint8_t, uint32_t)) { completion_callback = callback; }
void dwc2_test_reset(void)
{
    /* Core reset deactivates endpoints before the real device reset handler. */
    for (unsigned n = 0; n < 16; ++n) registers.epin[n].ctl = registers.epout[n].ctl = 0;
    handle_bus_reset(0);
    registers.gintsts = registers.daint = 0;
}
uint32_t dwc2_test_mode_fault(uint32_t reject)
{
    dwc2_ghwcfg2_t hw = {.value = registers.ghwcfg2}; hw.arch = GHWCFG2_ARCH_INTERNAL_DMA;
    if (reject & USB_AUDIO_MODE_NOT_INTERNAL_DMA) hw.arch = GHWCFG2_ARCH_SLAVE_ONLY;
    registers.ghwcfg2 = hw.value;
    registers.gahbcfg = reject & USB_AUDIO_MODE_DMA_DISABLED ? 0 : GAHBCFG_DMAEN;
    dwc2_dcfg_t cfg = {.value = registers.dcfg}; cfg.dma_desc = (reject & USB_AUDIO_MODE_DESCRIPTOR_DMA) != 0;
    registers.dcfg = cfg.value;
    if (reject & USB_AUDIO_MODE_PTI_NOT_ACTIVE) registers.dctl &= ~DCTL_IGNRFRMNUM;
    else registers.dctl |= DCTL_IGNRFRMNUM;
    registers.dthrctl = reject & USB_AUDIO_MODE_THRESHOLD_ACTIVE ? DTHRCTL_RXTHREN : 0;
    input_usb_mode_end(0, true);
    return input_usb_mode_reject(&registers);
}
void dwc2_test_unknown_mode(void) { input_usb_mode_begin(true); }
