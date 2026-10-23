// SPDX-License-Identifier: Apache-2.0
/* Compile the complete pinned/generated class source. Only the setup shim
 * initializes its private endpoint cache; FIFO and RX completion stay real. */
#include USB_AUDIO_DRIVER_SOURCE
#include <assert.h>
#include <string.h>

extern bool stack_out_armed, stack_in_armed;
void stack_driver_init(void)
{
    static const tusb_desc_interface_t control = {.bLength = sizeof(tusb_desc_interface_t),
        .bDescriptorType = TUSB_DESC_INTERFACE, .bInterfaceNumber = 3,
        .bInterfaceClass = TUSB_CLASS_AUDIO, .bInterfaceSubClass = AUDIO_SUBCLASS_CONTROL,
        .bInterfaceProtocol = AUDIO_INT_PROTOCOL_CODE_V2};
    audiod_init();
    audiod_function_t *audio = &_audiod_fct[0];
    audio->p_desc = (const uint8_t *)&control;
    audio->ep_out = 0x04; audio->ep_out_alt = 1; audio->ep_out_sz = 98;
    audio->ep_in = 0x84; audio->ep_in_alt = 1; audio->ep_in_sz = 98;
    audio->ep_fb = 0x85;
    audio->format_type_tx = AUDIO20_FORMAT_TYPE_I; audio->n_channels_tx = 1;
    audio->n_bytes_per_sample_tx = 2; audio->sample_rate_tx = 48000;
    audio->interval_tx = tud_speed_get() == TUSB_SPEED_HIGH ? 4 : 1;
    audio->ep_in_fifo_threshold = audio->ep_in_ff.depth / 2;
    assert(audiod_calc_tx_packet_sz(audio));
    assert(usbd_edpt_xfer(0, audio->ep_in, audio->lin_buf_in, 0, false));
    assert(usbd_edpt_xfer(0, audio->ep_out, audio->lin_buf_out, audio->ep_out_sz, false));
}
bool stack_complete_out(const uint8_t *data, uint16_t bytes)
{
    assert(bytes <= 98);
    if (!stack_out_armed) return false;
    stack_out_armed = false;
    if (bytes) memcpy(_audiod_fct[0].lin_buf_out, data, bytes);
    return audiod_xfer_isr(0, 0x04, XFER_RESULT_SUCCESS, bytes);
}
void stack_driver_reset(void)
{
    audiod_reset(0); stack_out_armed = stack_in_armed = false;
}
void stack_out_overwrite(bool enabled)
{
    tu_fifo_set_overwritable(&_audiod_fct[0].ep_out_ff, enabled);
}
bool stack_in_overwritable(void)
{
    return _audiod_fct[0].ep_in_ff.overwritable;
}
