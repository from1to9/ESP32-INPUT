// SPDX-License-Identifier: Apache-2.0
/* Real class open/control parsing; no private endpoint state is fabricated. */
#include USB_AUDIO_DRIVER_SOURCE
#include "usb_audio_descriptors.h"
#include <assert.h>
static const uint8_t full_descriptor[] = {INPUT_USB_AUDIO_DESCRIPTOR(1)};
static const uint8_t high_descriptor[] = {INPUT_USB_AUDIO_DESCRIPTOR(4)};
unsigned dcd_audio_interval(bool high_speed, uint8_t endpoint)
{
    const uint8_t *p = high_speed ? high_descriptor : full_descriptor;
    const uint8_t *end = p + sizeof(full_descriptor);
    while (p < end) {
        if (p[1] == TUSB_DESC_ENDPOINT && p[2] == endpoint)
            return high_speed ? 1U << (p[6] - 1) : p[6];
        p += p[0];
    }
    assert(false); return 0;
}
void dcd_audio_open(bool high_speed)
{
    const uint8_t *descriptor = high_speed ? high_descriptor : full_descriptor;
    audiod_init();
    uint16_t length = sizeof(full_descriptor) - TUD_AUDIO20_DESC_IAD_LEN;
    assert(audiod_open(0, (const void *)(descriptor + TUD_AUDIO20_DESC_IAD_LEN), length) == length);
}
void dcd_audio_alt(uint16_t interface, uint16_t alternate)
{
    tusb_control_request_t request = {.bmRequestType = 1, .bRequest = TUSB_REQ_SET_INTERFACE,
        .wIndex = interface, .wValue = alternate};
    assert(audiod_control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
}
bool dcd_audio_complete(uint8_t ep, uint8_t result, uint32_t bytes)
{
    return audiod_xfer_isr(0, ep, (xfer_result_t)result, bytes);
}
void dcd_audio_clock_request(void)
{
    tusb_control_request_t request = {.bmRequestType = 0xa1, .bRequest = AUDIO20_CS_REQ_CUR,
        .wIndex = (INPUT_USB_AUDIO_CLOCK_ID << 8) | INPUT_USB_AUDIO_CONTROL_INTERFACE,
        .wValue = AUDIO20_CS_CTRL_SAM_FREQ << 8, .wLength = 4};
    assert(audiod_control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
}
