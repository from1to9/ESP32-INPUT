// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "tinyusb.h"
#define TINYUSB_DEFAULT_CONFIG(callback) ((tinyusb_config_t){.event_cb = (callback)})
