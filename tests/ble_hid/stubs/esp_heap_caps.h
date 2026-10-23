// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1U << 2)
#define MALLOC_CAP_DMA (1U << 3)
#define MALLOC_CAP_INTERNAL (1U << 11)
size_t heap_caps_get_free_size(uint32_t capabilities);
size_t heap_caps_get_largest_free_block(uint32_t capabilities);
