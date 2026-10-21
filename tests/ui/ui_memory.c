// SPDX-License-Identifier: Apache-2.0
#include "ui_memory.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void *ui_clib_malloc_core(size_t size);
void *ui_clib_realloc_core(void *pointer, size_t size);
void ui_clib_free_core(void *pointer);

typedef struct { void *pointer; size_t bytes; } allocation_t;
static allocation_t s_live[8192];
static ui_memory_stats_t s_stats;

static size_t find_slot(void *pointer)
{
    for (size_t i = 0; i < sizeof s_live / sizeof s_live[0]; ++i)
        if (s_live[i].pointer == pointer) return i;
    assert(!"Allocator observation capacity exceeded or unknown pointer");
    return SIZE_MAX;
}

static void remember(size_t slot, void *pointer, size_t bytes)
{
    assert(pointer && slot < sizeof s_live / sizeof s_live[0]);
    s_stats.live_bytes -= s_live[slot].bytes;
    s_stats.live_bytes += bytes;
    s_live[slot] = (allocation_t){pointer, bytes};
    if (s_stats.live_bytes > s_stats.peak_live_bytes) s_stats.peak_live_bytes = s_stats.live_bytes;
}

void *lv_malloc_core(size_t size)
{
    ++s_stats.allocations;
    void *pointer = ui_clib_malloc_core(size);
    if (pointer) {
        remember(find_slot(NULL), pointer, size);
        ++s_stats.live_blocks;
    } else ++s_stats.failures;
    return pointer;
}

void *lv_realloc_core(void *pointer, size_t size)
{
    /* The unchanged LVGL front end handles zero size through lv_free. */
    assert(size > 0);
    bool existing = pointer != NULL;
    size_t slot = find_slot(pointer);
    ++s_stats.reallocations;
    void *replacement = ui_clib_realloc_core(pointer, size);
    if (replacement) {
        remember(slot, replacement, size);
        if (!existing) ++s_stats.live_blocks;
    } else ++s_stats.failures;
    return replacement;
}

void lv_free_core(void *pointer)
{
    ++s_stats.frees;
    if (pointer) {
        size_t slot = find_slot(pointer);
        s_stats.live_bytes -= s_live[slot].bytes;
        --s_stats.live_blocks;
        s_live[slot] = (allocation_t){0};
    }
    ui_clib_free_core(pointer);
}

ui_memory_stats_t ui_memory_stats(void) { return s_stats; }

bool ui_memory_contains(const char *value)
{
    size_t length = strlen(value);
    assert(length);
    for (size_t i = 0; i < sizeof s_live / sizeof s_live[0]; ++i) {
        if (!s_live[i].pointer || s_live[i].bytes < length) continue;
        const unsigned char *bytes = s_live[i].pointer;
        for (size_t offset = 0; offset <= s_live[i].bytes - length; ++offset)
            if (memcmp(bytes + offset, value, length) == 0) return true;
    }
    return false;
}

void ui_memory_check_observer(void)
{
    assert(s_stats.live_blocks == 0);
    void *pointer = lv_malloc_core(23);
    assert(pointer && s_stats.live_bytes == 23 && s_stats.allocations == 1);
    memset(pointer, 0x5a, 23);
    pointer = lv_realloc_core(pointer, 61);
    assert(pointer && s_stats.live_bytes == 61 && s_stats.reallocations == 1);
    for (size_t i = 0; i < 23; ++i) assert(((unsigned char *)pointer)[i] == 0x5a);
    lv_free_core(pointer);
    assert(s_stats.live_blocks == 0 && s_stats.live_bytes == 0 && s_stats.frees == 1);
    assert(s_stats.peak_live_bytes == 61 && s_stats.failures == 0);
    s_stats = (ui_memory_stats_t){0};
}
