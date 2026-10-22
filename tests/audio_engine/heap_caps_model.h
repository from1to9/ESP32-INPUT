// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Model the target's four-byte generic heap boundary, rather than relying on
 * libc calloc's stronger alignment. Explicit alignment is honored exactly. */
static void *test_heap_allocate(size_t alignment, size_t count, size_t bytes)
{
    assert(alignment && !(alignment & (alignment - 1)));
    void *base = calloc(1, count * bytes + alignment + sizeof(base) + 8);
    if (!base) return NULL;
    uintptr_t address = ((uintptr_t)base + sizeof(base) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    if (alignment <= 4 && !(address & 7)) address += 4;
    void *data = (void *)address;
    memcpy((uint8_t *)data - sizeof(base), &base, sizeof(base));
    return data;
}

static void test_heap_free(void *data)
{
    if (!data) return;
    void *base;
    memcpy(&base, (uint8_t *)data - sizeof(base), sizeof(base));
    free(base);
}

void *heap_caps_aligned_calloc(size_t, size_t, size_t, unsigned);
