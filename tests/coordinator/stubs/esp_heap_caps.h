// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define MALLOC_CAP_SPIRAM (1U << 10)
void *heap_caps_calloc(size_t, size_t, uint32_t);
void heap_caps_free(void *);
#define MALLOC_CAP_8BIT (1U << 2)
#define MALLOC_CAP_DMA (1U << 3)
#define MALLOC_CAP_INTERNAL (1U << 11)
typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char *function_name);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
size_t heap_caps_get_free_size(uint32_t capabilities);
size_t heap_caps_get_minimum_free_size(uint32_t capabilities);
size_t heap_caps_get_largest_free_block(uint32_t capabilities);
