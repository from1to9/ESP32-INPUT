// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* One copied coordinator-owned proposal; no authority, secrets or SDK handles.
 * Strings must be complete and NUL terminated. Overlong/visually clipped content
 * disables confirmation. details names the exact reset/retention consequences.
 * now_ms/expires_ms use the same monotonic clock. The coordinator must recheck
 * expiry and every request identity on the validated physical confirmation path. */
typedef struct {
    uint64_t management_generation, resource_revision, expires_ms;
    char operation[49], approval_id[33], action_digest[65];
} app_remote_review_request_t;
typedef struct {
    bool pending, complete, can_confirm, clock_known;
    app_remote_review_request_t request;
    uint64_t now_ms;
    char reset_group[33], retained_manifest_digest[65], details[257];
} app_remote_review_view_t;
