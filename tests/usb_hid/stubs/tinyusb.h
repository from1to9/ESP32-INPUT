// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include "tusb.h"
#define TINYUSB_ESPRESSIF_VID 0x303a
typedef enum {
    TINYUSB_EVENT_ATTACHED, TINYUSB_EVENT_DETACHED,
    TINYUSB_EVENT_SUSPENDED, TINYUSB_EVENT_RESUMED,
} tinyusb_event_id_t;
typedef struct { tinyusb_event_id_t id; } tinyusb_event_t;
typedef struct {
    const tusb_desc_device_t *device;
    const tusb_desc_device_qualifier_t *qualifier;
    const char **string;
    int string_count;
    const uint8_t *full_speed_config;
    const uint8_t *high_speed_config;
} tinyusb_desc_config_t;
typedef struct {
    tinyusb_desc_config_t descriptor;
    void (*event_cb)(tinyusb_event_t *event, void *argument);
    void *event_arg;
} tinyusb_config_t;
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);
