// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Model an 8-bit heap that guarantees four-byte, but not eight-byte, alignment.
 * Explicit aligned requests satisfy only the requested contract. Track the raw
 * allocation separately so shifted payloads can be legitimately freed. */
static struct {
    void *raw, *payload;
    size_t bytes;
} test_heap_blocks[128];
static unsigned test_heap_live, test_heap_fail_next, test_heap_external_failures;
static bool test_heap_fail_external;
static size_t test_heap_fail_bytes;

static void *test_heap_calloc(size_t alignment, size_t count, size_t bytes, unsigned caps)
{
    if (test_heap_fail_next) {
        --test_heap_fail_next;
        return NULL;
    }
    if (test_heap_fail_external && (caps & MALLOC_CAP_SPIRAM)) {
        ++test_heap_external_failures;
        return NULL;
    }
    assert(!count || bytes <= SIZE_MAX / count);
    bytes *= count;
    if (test_heap_fail_bytes && bytes == test_heap_fail_bytes) return NULL;
    assert(alignment && !(alignment & (alignment - 1U)));
    if (alignment < 4) alignment = 4;
    assert(bytes <= SIZE_MAX - alignment - 8);
    void *raw = malloc(bytes + alignment + 8);
    if (!raw) return NULL;
    uintptr_t address = ((uintptr_t)raw + 8U + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
    if (alignment == 4 && !(address % 8U)) address += 4;
    void *payload = (void *)address;
    memset(payload, 0, bytes);
    unsigned slot = 0;
    while (slot < 128 && test_heap_blocks[slot].raw) ++slot;
    assert(slot < 128);
    test_heap_blocks[slot].raw = raw;
    test_heap_blocks[slot].payload = payload;
    test_heap_blocks[slot].bytes = bytes;
    ++test_heap_live;
    return payload;
}

static void test_heap_free(void *pointer)
{
    if (!pointer) return;
    unsigned slot = 0;
    while (slot < 128 && test_heap_blocks[slot].payload != pointer) ++slot;
    assert(slot < 128 && test_heap_blocks[slot].raw && test_heap_live);
    free(test_heap_blocks[slot].raw);
    memset(&test_heap_blocks[slot], 0, sizeof(test_heap_blocks[slot]));
    --test_heap_live;
}
