// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded coordinator/UI boundary. No SDK, audio-engine, wire permission or
 * borrowed pointers. Owners supply observed values and action admission. */
#define APP_MEDIA_ROWS 4U
#define APP_MEDIA_PROFILES 8U
/* Build-time local-start policy; host negotiation must keep one stream/role
 * identity continuously valid for this positive interval before new consent. */
#ifndef APP_MEDIA_USB_START_SETTLE_MS
#define APP_MEDIA_USB_START_SETTLE_MS 1000U
#endif
typedef struct { uint8_t bytes[16]; } app_media_id_t;
typedef enum { APP_MEDIA_ROUTE_OFF, APP_MEDIA_ROUTE_LOCAL, APP_MEDIA_ROUTE_HFP,
    APP_MEDIA_ROUTE_A2DP, APP_MEDIA_ROUTE_UAC, APP_MEDIA_ROUTE_COUNT } app_media_route_t;
typedef enum { APP_MEDIA_MICROPHONE, APP_MEDIA_SPEAKER } app_media_role_t;
typedef enum {
    APP_MEDIA_LEVEL_UNKNOWN, APP_MEDIA_LEVEL_OFF,
    APP_MEDIA_LEVEL_WAITING, APP_MEDIA_LEVEL_LIVE
} app_media_level_state_t;
typedef struct {
    app_media_level_state_t state;
    int16_t dbfs; /* RMS dBFS, -96..0 when LIVE and not silent. */
    uint8_t percent; /* Bounded visual scale: -60..0 dBFS maps to 0..100. */
    bool silent, clipping;
} app_media_mic_level_t;
typedef enum { APP_MEDIA_CONNECTION_USB, APP_MEDIA_CONNECTION_BLUETOOTH,
    APP_MEDIA_CONNECTION_COUNT } app_media_connection_t;
typedef enum {
    APP_MEDIA_START_UNKNOWN = 0, APP_MEDIA_START_ALLOWED,
    APP_MEDIA_START_LOCKED, APP_MEDIA_START_SCREEN_UNAVAILABLE,
    APP_MEDIA_START_CLOCK_UNAVAILABLE, APP_MEDIA_START_PENDING,
    APP_MEDIA_START_QUARANTINED, APP_MEDIA_START_CONNECTION_REQUIRED,
    APP_MEDIA_START_UNSUPPORTED, APP_MEDIA_START_ROUTE_UNAVAILABLE,
    APP_MEDIA_START_HOST_UNAVAILABLE, APP_MEDIA_START_HOST_STREAM_REQUIRED,
    APP_MEDIA_START_IDENTITY_UNAVAILABLE, APP_MEDIA_START_PEER_MISMATCH,
    APP_MEDIA_START_CLEANUP, APP_MEDIA_START_OCCUPIED,
    APP_MEDIA_START_STOP_REQUIRED, APP_MEDIA_START_CODEC_UNAVAILABLE,
    APP_MEDIA_START_FORMAT_UNSUPPORTED, APP_MEDIA_START_HOST_PREPARING
} app_media_start_block_t;
typedef enum { APP_MEDIA_UNKNOWN, APP_MEDIA_OFF, APP_MEDIA_PREPARING,
    APP_MEDIA_READY_MUTED, APP_MEDIA_RUNNING, APP_MEDIA_STOPPING, APP_MEDIA_FAULT } app_media_state_t;
typedef enum { APP_MEDIA_JOB_UNKNOWN, APP_MEDIA_JOB_STARTING, APP_MEDIA_JOB_RUNNING,
    APP_MEDIA_JOB_STOPPING, APP_MEDIA_JOB_FINALIZING, APP_MEDIA_JOB_COMPLETED,
    APP_MEDIA_JOB_PARTIAL, APP_MEDIA_JOB_ERROR, APP_MEDIA_JOB_DELETING } app_media_job_state_t;
typedef enum { APP_SPEECH_UNKNOWN, APP_SPEECH_UNAVAILABLE, APP_SPEECH_IDLE,
    APP_SPEECH_STARTING, APP_SPEECH_LISTENING, APP_SPEECH_CANDIDATE,
    APP_SPEECH_STOPPING, APP_SPEECH_ERROR } app_speech_state_t;

typedef enum {
    APP_MEDIA_SELECT_ROUTE, APP_MEDIA_LINK_CONNECT, APP_MEDIA_LINK_DISCONNECT,
    APP_MEDIA_SET_ENABLED, APP_MEDIA_SET_MUTED, APP_MEDIA_SET_VOLUME, APP_MEDIA_SET_GAIN,
    APP_MEDIA_RECORD_SET_PROFILE, APP_MEDIA_RECORD_SET_DURATION,
    APP_MEDIA_RECORD_START_REQUEST, APP_MEDIA_RECORD_STOP, APP_MEDIA_RECORD_CANCEL_KEEP,
    APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST, APP_MEDIA_RECORD_LIST_PAGE,
    APP_MEDIA_RECORD_SELECT, APP_MEDIA_RECORD_PLAY, APP_MEDIA_RECORD_DELETE_REQUEST,
    APP_MEDIA_PLAYBACK_STOP, APP_MEDIA_SPEECH_START, APP_MEDIA_SPEECH_STOP,
    APP_MEDIA_SPEECH_REJECT, APP_MEDIA_SPEECH_CONFIRM,
    APP_MEDIA_LIVE_START, APP_MEDIA_LIVE_STOP, APP_MEDIA_SELECT_PEER,
    APP_MEDIA_SELECT_CONNECTION, APP_MEDIA_PAIR_AUDIO_OPEN, APP_MEDIA_PAIR_AUDIO_CLOSE,
    APP_MEDIA_DEVICE_DETAILS, APP_MEDIA_DEVICE_PAGE, APP_MEDIA_DEVICE_LINK, APP_MEDIA_DEVICE_REFRESH,
    APP_MEDIA_DEVICE_FORGET
} app_media_operation_t;

static inline bool app_media_device_navigation(app_media_operation_t operation)
{
    /* These operations read saved state; none grants link or pairing authority. */
    return operation == APP_MEDIA_DEVICE_DETAILS || operation == APP_MEDIA_DEVICE_PAGE || operation == APP_MEDIA_DEVICE_REFRESH;
}

typedef struct {
    app_media_operation_t operation;
    uint64_t management_generation, audio_generation, resource_revision;
    uint64_t list_generation;
    app_media_id_t object_id, job_id, peer_id;
    uint64_t object_revision, consumer_generation;
    uint64_t speech_generation, speech_revision, candidate_id;
    union {
        app_media_route_t route;
        app_media_connection_t connection;
        struct { app_media_role_t role; bool value; } role;
        uint8_t level; /* Applied volume percent or exact discrete microphone dB. */
        uint16_t profile_id; /* Immutable owner-advertised profile ID. */
        uint32_t duration_seconds;
        int page_delta;
        char candidate;
        app_media_id_t peer_id; /* Explicit next saved Classic peer, never HID. */
    } value;
} app_media_request_t;

typedef struct {
    bool available, can_select, can_connect, can_disconnect, connected;
    char reason[65];
} app_media_route_view_t;
typedef struct {
    bool known, enabled, muted, mute_confirmed;
    bool can_enable, can_disable, can_mute, can_unmute, can_start;
    /* Owner-produced result of the same admission predicate as can_start. */
    app_media_start_block_t start_block;
} app_media_role_view_t;
typedef struct {
    uint16_t id;
    bool available;
    char label[33];
} app_media_profile_view_t;
typedef struct {
    bool known, can_start, can_edit;
    uint16_t profile_id;
    size_t profile_count;
    app_media_profile_view_t profiles[APP_MEDIA_PROFILES];
    uint32_t duration_seconds, minimum_duration_seconds, maximum_duration_seconds;
    uint64_t byte_limit;
    uint32_t retention_seconds;
    bool owner_download;
    bool continue_locally; /* Only with separately admitted background authority. */
    char reason[97];
} app_media_record_options_t;
typedef struct {
    bool known;
    app_media_id_t id, object_id;
    uint64_t consumer_generation;
    app_media_job_state_t state;
    bool can_stop, can_cancel_keep, can_cancel_discard;
    uint64_t elapsed_ms, captured_ms, stored_bytes, verified_bytes;
    uint32_t limit_seconds;
    uint64_t byte_limit;
    bool cleanup_pending, stored_bytes_known;
    char reason[97];
} app_media_job_view_t;
typedef struct {
    bool valid;
    app_media_id_t id;
    uint64_t revision;
    app_media_job_state_t state;
    char label[33]; /* Safe authorized label, never a path. */
    uint64_t duration_ms, stored_bytes;
    bool duration_known, bytes_known, verified;
    bool can_play, can_delete;
    char reason[65];
} app_media_record_view_t;
typedef struct {
    bool known, pending, has_previous, has_next;
    uint64_t generation;
    unsigned page;
    size_t row_count;
    app_media_record_view_t rows[APP_MEDIA_ROWS];
    char reason[97];
} app_media_record_list_view_t;
typedef struct {
    app_speech_state_t state;
    uint64_t generation, revision, candidate_id;
    char candidate; /* Actual recognized letter only; zero means unavailable. */
    bool can_listen, can_stop, can_reject, can_confirm;
    bool remaining_known;
    uint32_t window_seconds, remaining_seconds;
    char profile[33];
    char reason[97];
} app_media_speech_view_t;
typedef enum {
    APP_MEDIA_PAIR_UNAVAILABLE, APP_MEDIA_PAIR_CLOSED, APP_MEDIA_PAIR_PREPARING,
    APP_MEDIA_PAIR_DISCOVERABLE, APP_MEDIA_PAIR_AUTHENTICATING,
    APP_MEDIA_PAIR_COMPLETED, APP_MEDIA_PAIR_EXPIRED, APP_MEDIA_PAIR_ERROR, APP_MEDIA_PAIR_CLOSING
} app_media_pair_state_t;
typedef struct {
    app_media_pair_state_t state;
    bool can_open, can_close, name_known;
    uint32_t remaining_seconds;
    char device_name[65];
    char reason[97];
} app_media_pairing_view_t;
typedef struct {
    app_media_id_t id;
    char identity[24];
    bool connected, selected;
} app_media_device_row_t;
typedef struct {
    bool known, pending, refreshing, selected_saved, voice_connected, can_refresh, request_failed, can_forget;
    unsigned page, pages, count;
    uint64_t revision;
    app_media_device_row_t rows[APP_MEDIA_ROWS];
    app_media_id_t selected;
    char identity[24], reason[97];
    uint8_t address[6]; /* Display-only transport address, never a merged identity. */
    bool can_connect[2], can_disconnect[2], connected[2], connection_known[2];
} app_media_devices_view_t;
typedef struct {
    bool known, command_pending;
    const char *resume_notice; /* Static literals only. */
    bool boot_storage_error;
    bool speaker_waiting; /* Saved intent, no active playback job. */
    uint64_t management_generation, audio_generation, resource_revision;
    app_media_state_t state;
    app_media_route_t route;
    app_media_route_view_t routes[APP_MEDIA_ROUTE_COUNT];
    app_media_id_t peer_id;
    app_media_id_t next_peer_id;
    bool can_select_peer;
    char peer_label[49]; /* Actual audio peer, never inferred from HID target. */
    app_media_role_view_t microphone, speaker;
    bool volume_known, can_set_volume, gain_known, can_set_gain;
    uint8_t volume_percent, safe_volume_max, microphone_gain_db;
    char reason[97];
    app_media_record_options_t recording_options;
    app_media_job_view_t recording, playback, live_capture;
    app_media_record_list_view_t recordings;
    app_media_record_view_t selected_recording;
    app_media_speech_view_t speech;
    /* Owner-applied selection, not evidence of a link, stream or profile.
     * The owner resolves Bluetooth profiles and completes release barriers. */
    bool connection_known;
    app_media_connection_t connection;
    bool can_select_connection[APP_MEDIA_CONNECTION_COUNT];
    app_media_pairing_view_t pairing;
    app_media_devices_view_t devices;
    /* Read-only fresh local PCM telemetry, never an action revision or proof
     * of host recording. Unknown/stale observations carry no displayed level. */
    app_media_mic_level_t microphone_level;
} app_media_view_t;

typedef struct {
    bool valid; /* Immutable coordinator-owned review, invalidated on replacement. */
    app_media_request_t request;
    app_media_record_options_t options;
    app_media_record_view_t recording;
    app_media_job_view_t job;
} app_media_review_t;

/* *_REQUEST only opens an immutable coordinator-owned review/confirmation.
 * RECORD_START confirmation shows the exact profile/time/bytes/retention and
 * destination, then creates only the exact local consent needed for that job.
 * Delete/discard confirmation retains the original object/job and revision.
 * Unknown results reconcile; no optimistic ready/muted/saved state or replay.
 * Speech confirm also checks the enclosing UI input epoch/revision, appends to
 * the draft once, and never sends automatically. Audio authority is independent
 * of HID ownership; page changes cannot cancel independent media jobs. */
