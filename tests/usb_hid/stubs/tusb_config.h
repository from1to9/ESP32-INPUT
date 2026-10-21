// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../firmware/main/usb_audio_config.h"
/* Real TinyUSB headers/macros, OS-independent host doubles below their API. */
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_CDC 1
#define CFG_TUD_HID 1
#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENDPOINT0_SIZE 64
