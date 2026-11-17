#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
static inline void explicit_bzero(void *bytes, size_t length)
{ volatile unsigned char *p = bytes; while (length--) *p++ = 0; }
typedef int esp_err_t;
#define ESP_OK 0
bool usb_serial_jtag_is_connected(void);
bool usb_serial_jtag_is_driver_installed(void);
uint32_t esp_random(void);
int64_t esp_timer_get_time(void);
typedef struct { char version[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
