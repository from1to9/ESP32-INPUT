// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

// Owner observations, distinct from admission to its command queue.
typedef enum {
    PR_NONE, PR_PENDING, PR_APPLIED, PR_INVALID, PR_STALE, PR_LOCKED, PR_BUSY,
    PR_UNAVAILABLE, PR_STORAGE_UNCERTAIN, PR_CANCELLED, PR_UNKNOWN, PR_TIMEOUT,
    PR_RELEASE_REQUIRED,
} passport_result_t;
typedef struct {
    uint32_t id; // Zero leaves native commands and their existing UI unchanged.
    passport_result_t result;
    int32_t detail;
} passport_receipt_t;

const char *passport_result_name(passport_result_t result);
