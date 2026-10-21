// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    size_t allocations, reallocations, frees, failures;
    size_t live_blocks, live_bytes, peak_live_bytes;
} ui_memory_stats_t;

/* Counts requested bytes at the LVGL CLIB boundary, not allocator overhead,
 * test stdout/framebuffers, target internal SRAM or physical DMA memory. */
ui_memory_stats_t ui_memory_stats(void);
void ui_memory_check_observer(void);
/* Search live allocator-owned bytes, including tails beyond C-string NULs. */
bool ui_memory_contains(const char *value);
