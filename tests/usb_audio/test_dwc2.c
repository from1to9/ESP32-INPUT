// SPDX-License-Identifier: Apache-2.0
#include "tusb.h"
#include "usb_audio_descriptors.h"
#include "dwc2_harness.h"
#include <assert.h>
#include <stdio.h>
static const uint8_t full_speed[] = {INPUT_USB_AUDIO_DESCRIPTOR(1)};
static const uint8_t high_speed[] = {INPUT_USB_AUDIO_DESCRIPTOR(4)};
static const tusb_desc_endpoint_t *endpoint(bool hs, uint8_t address)
{
    const uint8_t *p = hs ? high_speed : full_speed;
    const uint8_t *end = p + sizeof(full_speed);
    while (p < end) {
        if (p[1] == TUSB_DESC_ENDPOINT && p[2] == address) return (const void *)p;
        p += p[0];
    }
    assert(false); return NULL;
}
int main(void)
{
    uint8_t data[98] = {0};
    bool progress = true;
    for (unsigned speed = 0; speed < 2; ++speed) {
        bool hs = speed != 0;
        const tusb_desc_endpoint_t *descriptor = endpoint(hs, 0x04);
        unsigned interval = hs ? 1U << (descriptor->bInterval - 1) : descriptor->bInterval;
        for (unsigned phase = 0; phase < interval; ++phase) {
            dwc2_test_init(hs); dwc2_test_frame((phase + 0x3fffU) & 0x3fffU);
            assert(dwc2_test_open(descriptor));
            assert(dwc2_test_submit(0x04, data, 98));
            for (unsigned i = 0; i < 32; ++i) {
                dwc2_test_frame(phase + i * interval);
                (void)dwc2_test_poll(0x04, 96, 0);
            }
            dwc2_test_snapshot_t s = dwc2_test_snapshot(0x04);
            if (s.completed != 32 || s.bytes != 3072 || s.failed) {
                fprintf(stderr, "FAIL: actual DCD periodic OUT progress %u/32 at phase%u interval%u\n",
                    s.completed, phase, interval);
                progress = false;
            }
            assert(s.submitted == s.completed + 1);
            printf("PASS: actual DCD %s phase%u PTI%d OUT completed%u bytes%u\n", hs ? "HS" : "FS",
                phase, CFG_TUD_DWC2_PTI_ENABLE, s.completed, s.bytes);
        }
    }
    assert(progress);
    return 0;
}
