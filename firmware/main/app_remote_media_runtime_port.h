// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_remote_media.h"

/* Implemented by the single app_media_runtime owner. These are copy-only,
 * source-aware entry points; they must not consume local UI requests/results.
 * Remote tokens occupy a separate namespace from the local UI token counter.
 * Pending commands retain the actual authenticated identity and scopes.
 * START preserves request.start, including request/consent/session IDs and
 * immutable bounds. Runtime authority retains that same request and identity
 * for continuous app_control_runtime_capture_authority/session revalidation.
 * Live local-route jobs also pin the actual WSS channel generation at
 * admission. Channel loss cancels that generation's pending/active live jobs;
 * it does not revoke HTTP management, file recording or BT/USB ownership.
 * Reads/deletes retain caller/object/operation scope until I/O and payload
 * publication both complete. Cancelling token N must never cancel N+1. */
typedef struct {
    void *context;
    bool (*session_alive)(void *, const cp_identity_t *);
    cp_error_t (*capture_authority)(void *, const cp_capture_authority_request_t *, cp_capture_authority_t *);
} app_media_remote_authority_t;
/* Configure once with the actual control runtime's bounded authority lookups. */
ae_result_t app_media_runtime_set_remote_authority(const app_media_remote_authority_t *);
ae_result_t app_media_runtime_submit_remote(const app_remote_media_request_t *);
bool app_media_runtime_poll_remote(uint64_t token, app_audio_result_t *);
void app_media_runtime_release_remote_result(uint64_t token);
ae_result_t app_media_runtime_copy_remote_records(uint64_t token, ae_record_metadata_t *, size_t capacity);
ae_result_t app_media_runtime_copy_remote_data(uint64_t token, size_t offset, void *, size_t bytes);
void app_media_runtime_cancel_remote(uint64_t token, const cp_identity_t *, ae_stop_reason_t);
cp_error_t app_media_runtime_read_recordings(const cp_identity_t *, const char *resource,
    char *json, size_t capacity, size_t *length);
/* Bounded async download port for the authenticated HTTP owner. Retry the same
 * identity/object/revision/offset/length while AE_AGAIN. No borrowed bytes.
 * Metadata is owner-only; content additionally enforces owner_download and a
 * completed verified object. A changed revision or session never returns old
 * cached data. Disconnect cancellation matches identity and object exactly. */
ae_result_t app_media_runtime_record_metadata(const cp_identity_t *, const ae_id_t *, ae_record_metadata_t *);
ae_result_t app_media_runtime_content_read(const cp_identity_t *, const ae_id_t *, uint64_t revision,
    uint64_t offset, void *output, size_t capacity, size_t *actual);
void app_media_runtime_content_cancel(const cp_identity_t *, const ae_id_t *);
