// SPDX-License-Identifier: Apache-2.0
#include "usb_console.h"
#include "usb_hid.h"
#include "usb_audio.h"
#include "hid_engine.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "usb_audio_descriptors.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>

enum {
    USB_INTERFACE_CDC = 0,
    USB_INTERFACE_HID = 2,
    USB_INTERFACE_COUNT = 6,
    USB_CONFIGURATION_LENGTH = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + INPUT_USB_AUDIO_DESC_LENGTH,
    USB_HID_LENGTH_OFFSET = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + 9 + 7,
};

/* CDC notification 0x81, CDC data OUT 0x02/IN 0x82, HID input 0x83.
 * Keyboard LED output uses standard EP0 SET_REPORT, not another OUT endpoint.
 * Report-only interface: keyboard/mouse IDs live in the canonical report map.
 * bInterval 1 at FS and 4 at HS both request one-millisecond HID polling. */
static uint8_t s_full_speed_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_INTERFACE_COUNT, 0, USB_CONFIGURATION_LENGTH, 0, 100),
    TUD_CDC_DESCRIPTOR(USB_INTERFACE_CDC, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(USB_INTERFACE_HID, 0, HID_ITF_PROTOCOL_NONE, 0, 0x83, 16, 1),
    INPUT_USB_AUDIO_DESCRIPTOR(1),
};
#if TUD_OPT_HIGH_SPEED
static uint8_t s_high_speed_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_INTERFACE_COUNT, 0, USB_CONFIGURATION_LENGTH, 0, 100),
    TUD_CDC_DESCRIPTOR(USB_INTERFACE_CDC, 4, 0x81, 8, 0x02, 0x82, 512),
    TUD_HID_DESCRIPTOR(USB_INTERFACE_HID, 0, HID_ITF_PROTOCOL_NONE, 0, 0x83, 16, 4),
    INPUT_USB_AUDIO_DESCRIPTOR(4),
};
#endif

_Static_assert(sizeof(s_full_speed_configuration) == USB_CONFIGURATION_LENGTH,
               "Composite USB descriptor length must match its configuration");
static tinyusb_config_t s_driver;
static portMUX_TYPE s_start_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_start_attempted;
static esp_err_t s_start_result = ESP_ERR_INVALID_STATE;

static void usb_device_event(tinyusb_event_t *event, void *argument)
{
    (void)argument;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        usb_hid_stack_event(true, false);
        usb_audio_stack_event(true, false);
        break;
    case TINYUSB_EVENT_DETACHED:
        usb_hid_stack_event(false, false);
        usb_audio_stack_event(false, false);
        break;
    case TINYUSB_EVENT_SUSPENDED:
        usb_hid_stack_event(tud_mounted(), true);
        usb_audio_stack_event(tud_mounted(), true);
        break;
    case TINYUSB_EVENT_RESUMED:
        usb_hid_stack_event(tud_mounted(), false);
        usb_audio_stack_event(tud_mounted(), false);
        break;
    default:
        break;
    }
}

/* Initialization follows Espressif's pinned tusb_console example (CC0-1.0).
 * The managed esp_tinyusb 2.4 VFS is nonblocking when its TX buffer is full:
 * unread diagnostic logs may be dropped, but cannot stall HID on a USB reader.
 * stdout is not a command interface and no remote-reset request is accepted. */
esp_err_t usb_console_start(void)
{
    portENTER_CRITICAL(&s_start_lock);
    if (s_start_attempted) {
        const esp_err_t result = s_start_result;
        portEXIT_CRITICAL(&s_start_lock);
        return result;
    }
    s_start_attempted = true;
    portEXIT_CRITICAL(&s_start_lock);
    esp_err_t error = usb_hid_prepare();
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_start_lock);
        s_start_result = error;
        portEXIT_CRITICAL(&s_start_lock);
        return error;
    }
    if (!hid_engine_report_descriptor_length || hid_engine_report_descriptor_length > UINT16_MAX) {
        error = ESP_ERR_INVALID_SIZE;
        goto finished;
    }
    /* Audio allocation failure is retained by its capability provider. It must
     * not remove the independently usable HID/diagnostic interfaces. */
    (void)usb_audio_init();
    s_full_speed_configuration[USB_HID_LENGTH_OFFSET] = (uint8_t)hid_engine_report_descriptor_length;
    s_full_speed_configuration[USB_HID_LENGTH_OFFSET + 1] = (uint8_t)(hid_engine_report_descriptor_length >> 8);
    s_driver = TINYUSB_DEFAULT_CONFIG(usb_device_event);
    s_driver.descriptor.full_speed_config = s_full_speed_configuration;
#if TUD_OPT_HIGH_SPEED
    s_high_speed_configuration[USB_HID_LENGTH_OFFSET] = (uint8_t)hid_engine_report_descriptor_length;
    s_high_speed_configuration[USB_HID_LENGTH_OFFSET + 1] = (uint8_t)(hid_engine_report_descriptor_length >> 8);
    s_driver.descriptor.high_speed_config = s_high_speed_configuration;
#endif
    /* Keep the SDK's matching IAD device/qualifier and chip-unique serial. */
    error = tinyusb_driver_install(&s_driver);
    if (error != ESP_OK) {
        goto finished;
    }
    const tinyusb_config_cdcacm_t cdc = {0};
    error = tinyusb_cdcacm_init(&cdc);
    if (error != ESP_OK) {
        goto finished;
    }
    error = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (error == ESP_OK) {
        /* Picolibc freopen() resets these streams to full buffering. Restore
         * newline flushing so a short final log tail is not held until the
         * next burst. The underlying CDC VFS remains nonblocking when full. */
        if (setvbuf(stdout, NULL, _IOLBF, BUFSIZ) != 0 ||
                setvbuf(stderr, NULL, _IOLBF, BUFSIZ) != 0) {
            error = ESP_FAIL;
        }
    }
finished:
    /* A partially redirected console may still reference the CDC VFS. Keep
     * its stack/resources intact on failure rather than freeing live handles.
     * HID remains failed and no second stack is installed by a retry. */
    usb_hid_stack_started(error);
    usb_audio_stack_started(error);
    portENTER_CRITICAL(&s_start_lock);
    s_start_result = error;
    portEXIT_CRITICAL(&s_start_lock);
    return error;
}
