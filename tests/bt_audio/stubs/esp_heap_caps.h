// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_calloc_prefer(size_t count, size_t bytes, size_t alternatives, ...);
