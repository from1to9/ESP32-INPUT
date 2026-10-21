// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"
#define CP_MAX_CAPTURE_GRANTS 8U
typedef enum {
    CP_CAPTURE_SPEECH,
    CP_CAPTURE_LIVE,
    CP_CAPTURE_RECORDING,
    CP_CAPTURE_BACKGROUND
} cp_capture_purpose_t;
typedef struct {
    uint8_t id[CP_ID_BYTES], principal_id[CP_ID_BYTES];
    uint64_t revision, expires_unix_seconds, maximum_bytes;
    uint32_t maximum_duration_seconds, maximum_retention_seconds;
    cp_capture_purpose_t purpose;
    bool live_destination, file_destination, owner_download;
} cp_capture_grant_t;
/* Read-only authority request. IDs identify locally reviewed standing grants;
 * they are not bearer tokens. The authenticated identity and immutable job
 * bounds must also match. No grant activates capture or grants input control. */
typedef struct {
    cp_identity_t identity;
    uint8_t grant_id[CP_ID_BYTES], background_grant_id[CP_ID_BYTES];
    uint64_t grant_revision, background_grant_revision, bytes;
    uint32_t duration_seconds, retention_seconds;
    cp_capture_purpose_t purpose;
    bool live_destination, file_destination, owner_download, continue_local;
} cp_capture_authority_request_t;
typedef struct {
    bool allowed, owner_session_alive, background_allowed;
    uint64_t source_valid_until_ms, background_valid_until_ms;
    cp_error_t error;
} cp_capture_authority_t;
