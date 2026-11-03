// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared port contract v1. No SDK/codec/filesystem types cross this boundary.
 * One manager worker serializes every callback. Drivers never call the manager
 * recursively, retain borrowed pointers, change format or open another codec.
 * Each callback must return within AE_IO_TIMEOUT_MS. AE_AGAIN is unfinished,
 * never an acknowledgement. Retry uses identical operation identity/arguments.
 * Hardware qualification is separate from implemented/available capabilities. */
#define AE_IO_TIMEOUT_MS 20U
/* Sequence 1 is the first complete frame; its sample offset is 0. */
#define AE_FIRST_SEQUENCE UINT64_C(1)
#define AE_FRAME_BYTES_MAX 1920U
#define AE_RING_FRAMES 16U
#define AE_CAPTURE_RING_FRAMES 8U
#define AE_CONSUMERS_MAX 3U
#define AE_FORMATS_MAX 16U
#define AE_OBJECTS_MAX 64U
#define AE_LIST_MAX 16U
#define AE_WAV_HEADER_BYTES 44U
#define AE_ID_BYTES 16U

typedef struct { uint8_t bytes[AE_ID_BYTES]; } ae_id_t;
/* Actual media-link identity from its transport owner, independent of BLE HID.
 * All-zero is reserved for the local codec. A remote token has two nonzero
 * opaque IDs and a nonzero, strictly increasing link generation. Peer change,
 * disconnect/reconnect and stream replacement invalidate the complete token. */
typedef struct {
    ae_id_t peer_id;
    ae_id_t stream_id;
    uint64_t link_generation;
} ae_route_token_t;

typedef enum {
    AE_OK, AE_AGAIN, AE_INVALID, AE_UNAVAILABLE, AE_NOT_SUPPORTED,
    AE_BUSY, AE_STALE, AE_DENIED, AE_PRIVACY, AE_QUOTA, AE_OVERFLOW,
    AE_IO_ERROR, AE_UNCERTAIN, AE_TIMEOUT, AE_NOT_FOUND, AE_TOO_LATE,
    AE_INTEGRITY, AE_TIME_UNKNOWN, AE_EXHAUSTED,
} ae_result_t;

typedef enum { AE_ROUTE_OFF, AE_ROUTE_LOCAL, AE_ROUTE_HFP_HF,
    AE_ROUTE_A2DP_SINK, AE_ROUTE_USB_UAC, AE_ROUTE_COUNT } ae_route_t;
typedef enum { AE_MICROPHONE = 1, AE_SPEAKER = 2 } ae_role_t;
typedef enum { AE_PCM_S16_LE = 1 } ae_encoding_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;             /* Actual media channels, not I2S slot count. */
    uint8_t frame_ms;             /* Exactly 10 or 20; integral samples/frame. */
    ae_encoding_t encoding;
} ae_format_t;

typedef struct {
    uint64_t audio_generation;
    uint64_t consumer_generation;
    uint64_t sequence;
    uint64_t sample_offset;       /* Interleaved sample frames, not bytes. */
    ae_route_token_t route_token;
    ae_format_t format;
    uint16_t bytes;
    uint8_t data[AE_FRAME_BYTES_MAX];
} ae_frame_t;

typedef struct {
    ae_format_t format;
    uint8_t roles;                /* AE_MICROPHONE | AE_SPEAKER. */
    bool qualified;
} ae_profile_t;

typedef struct {
    bool implemented;
    bool available;
    bool qualified;
    bool full_duplex;             /* Never inferred from two codec handles. */
    uint8_t format_count;
    uint8_t safe_volume_max;      /* Exact percentage ceiling, <= 100. */
    ae_profile_t formats[AE_FORMATS_MAX];
    ae_result_t reason;
} ae_codec_capabilities_t;

/* Exactly one physical codec owner. prepare succeeds only with both roles
 * muted and the exact requested media format. It must not return bus bytes
 * relabelled as another rate/channel tuple. read/write report actual bytes;
 * zero/short transfers are never fabricated as a full valid frame. stop
 * confirms microphone mute/off and speaker mute/flush, or returns uncertainty.
 * A stale generation must reject read/write/unmute before hardware access;
 * safety mute/stop always remains admissible. Volume/gain cannot unmute. */
typedef struct {
    void *context;
    ae_result_t (*capabilities)(void *, ae_codec_capabilities_t *);
    ae_result_t (*prepare)(void *, const ae_format_t *, uint8_t roles, uint64_t generation);
    ae_result_t (*mute)(void *, ae_role_t, bool muted, uint64_t generation);
    ae_result_t (*volume)(void *, uint8_t percent, uint64_t generation);
    ae_result_t (*gain)(void *, uint8_t db, uint64_t generation);
    ae_result_t (*read)(void *, uint64_t generation, void *data, size_t capacity, size_t *actual);
    ae_result_t (*write)(void *, uint64_t generation, const void *data, size_t bytes, size_t *actual);
    ae_result_t (*stop)(void *, uint64_t generation);
    /* Register a nonblocking wake callback for asynchronous completion. The
     * driver copies this pair and invokes it after releasing its own lock.
     * Passing NULL clears it before the owning worker is destroyed. */
    void (*completion_notify)(void *, void (*notify)(void *), void *notify_context);
} ae_codec_port_t;

/* The transport owner manages its link/profile; it never owns the local codec.
 * prepare is a readiness/format reservation, not a claim that a host connected.
 * Receive PCM enters app_audio's bounded copied-frame submit path. capture
 * borrows one complete frame only for this call; AE_AGAIN must not retain it.
 * HFP/A2DP/UAC have separate capabilities and must not inherit HID readiness. */
typedef struct {
    bool implemented;
    bool available;
    bool connected;
    bool qualified;
    bool capture;
    bool playback;
    bool full_duplex;
    ae_format_t format;
    ae_route_token_t token;
    ae_result_t reason;
} ae_route_capabilities_t;

typedef struct {
    void *context;
    ae_result_t (*capabilities)(void *, ae_route_t, ae_route_capabilities_t *);
    /* AE_BUSY rejects preparation without acquiring a reservation or I/O.
     * The engine ends that start without calling stop on an unowned route. */
    ae_result_t (*prepare)(void *, ae_route_t, const ae_format_t *, uint8_t roles,
                           uint64_t generation, const ae_route_token_t *expected);
    ae_result_t (*capture)(void *, ae_route_t, const ae_frame_t *);
    /* Optional receipt after a complete current frame is consumed by the
     * physical codec. Never on app enqueue, partial/failed write or stale link.
     * No manager lock is held; callback is nonblocking and performs no I/O. */
    void (*playback_consumed)(void *, ae_route_t, const ae_frame_t *);
    /* Explicit playback consumer authorization, separate from shared resource
     * preparation. Enabling flushes old input, starts sequence 1/offset 0, and
     * stamps this nonzero consumer_generation into every produced frame.
     * Old callbacks retain their original generation and cannot be rebound.
     * Disabling closes only this binding; it preserves opposite-direction
     * capture. AE_OK requires old queued/in-flight input is invalidated.
     * Repeating identical args is idempotent. Closing an already closed old
     * binding is AE_OK and must never close a replacement binding. */
    ae_result_t (*bind_playback)(void *, ae_route_t, uint64_t audio_generation,
        const ae_route_token_t *, uint64_t consumer_generation, bool enabled);
    /* Stop the named old stream only; a replacement link is never retargeted.
     * AE_OK also requires that no pending callback can emit its old frames. */
    ae_result_t (*stop)(void *, ae_route_t, uint64_t generation, const ae_route_token_t *expected);
} ae_route_port_t;

typedef enum { AE_RECORD_STARTING, AE_RECORD_RECORDING, AE_RECORD_STOPPING,
    AE_RECORD_FINALIZING, AE_RECORD_COMPLETED, AE_RECORD_PARTIAL,
    AE_RECORD_ERROR, AE_RECORD_DELETING } ae_record_state_t;
typedef enum { AE_EXPORT_NONE, AE_EXPORT_OWNER_DOWNLOAD } ae_export_policy_t;
typedef enum { AE_STORAGE_NONE, AE_STORAGE_RECORDS_PARTITION, AE_STORAGE_NAND } ae_storage_t;

/* Persist with explicit versioned/authenticated serialization, never raw C
 * struct bytes. IDs are manager-generated CSPRNG values, never client paths.
 * A store must retain terminal metadata across restart and recover unfinished
 * objects as partial/error; it must never resume capture or reset retention. */
typedef struct {
    ae_id_t object_id;
    ae_id_t principal_id;
    ae_id_t consent_id;
    uint64_t consent_revision;
    uint64_t revision;
    uint64_t created_unix_seconds;
    uint64_t expires_unix_seconds;
    uint64_t reserved_bytes;
    uint64_t payload_limit_bytes;
    uint64_t pcm_bytes;
    uint64_t stored_bytes;
    uint64_t verified_pcm_bytes;
    uint64_t synced_pcm_bytes;
    uint64_t sample_frames;
    ae_format_t format;
    ae_record_state_t state;
    ae_storage_t storage;
    ae_export_policy_t export_policy;
    ae_result_t reason;
    uint8_t sha256[32];            /* Plaintext WAV hash, only if actually computed. */
    bool hash_available;
    bool integrity_verified;
    bool sync_acknowledged;
    bool access_revoked;
} ae_record_metadata_t;

typedef struct {
    ae_storage_t storage;
    bool implemented;
    bool available;
    bool qualified;
    bool geometry_verified;
    bool owned_and_provisioned;
    bool encryption_ready;
    bool authenticated_metadata;
    bool recovery_complete;
    bool retention_time_known;
    uint64_t usable_bytes;
    uint64_t quota_bytes;
    uint64_t used_bytes;
    uint64_t reserved_bytes;
    uint32_t per_object_overhead_bytes; /* Encryption/journal/index, excluding WAV header. */
    uint32_t media_chunk_bytes; /* Zero with zero chunk overhead means linear storage. */
    uint32_t media_chunk_overhead_bytes;
    uint16_t object_count;
    uint16_t object_limit;
    ae_result_t reason;
} ae_store_capabilities_t;

/* Store owner provides real encryption, journal, verification and recovery.
 * reserve atomically reserves the whole bounded job and durably records its
 * immutable owner/expiry before capture. append takes PCM at an exact offset;
 * confirmed bytes are explicit even on error. checkpoint returns verified and
 * synced extents, not merely accepted writes. finish uses the supplied WAV
 * header and publishes only after exact-length/integrity/hash/manifest sync;
 * returned evidence is checked by the manager before reporting completed.
 * AE_AGAIN is idempotent by object ID/revision, and never permits a new writer.
 * remove revokes access before reclamation; uncertain cleanup stays visible.
 * No callback may initialize, format or claim ownership of unknown NAND. */
typedef struct {
    void *context;
    ae_result_t (*capabilities)(void *, ae_store_capabilities_t *);
    ae_result_t (*reserve)(void *, const ae_record_metadata_t *);
    ae_result_t (*append)(void *, const ae_id_t *, uint64_t pcm_offset,
                          const void *data, size_t bytes, size_t *actual);
    ae_result_t (*checkpoint)(void *, const ae_id_t *, ae_record_metadata_t *evidence);
    ae_result_t (*finish)(void *, const ae_record_metadata_t *,
                          const uint8_t wav_header[AE_WAV_HEADER_BYTES], ae_record_metadata_t *evidence);
    ae_result_t (*get)(void *, const ae_id_t *, ae_record_metadata_t *);
    ae_result_t (*list)(void *, const ae_id_t *principal, uint32_t cursor,
                        ae_record_metadata_t *items, size_t capacity, size_t *count, uint32_t *next);
    ae_result_t (*read)(void *, const ae_id_t *, uint64_t wav_offset,
                        void *data, size_t capacity, size_t *actual);
    ae_result_t (*remove)(void *, const ae_id_t *, uint64_t expected_revision,
                          bool *access_revoked, bool *cleanup_complete);
    /* Optional serialized background service and explicit provision command.
     * Provision never creates ownership without the enclosing local authority.
     * Both run only in the audio worker, never in the UI/transport callback. */
    ae_result_t (*service)(void *);
    ae_result_t (*provision)(void *, uint16_t object_limit, uint64_t quota_bytes);
    void (*cancel_read)(void *, const ae_id_t *); /* Nonblocking; retire any pending byte request before erasure. */
    ae_result_t (*recover)(void *); /* Explicit read/reconcile only; never format. */
    /* Cancel before the terminal page program is submitted. Retires pending
     * read/erase work and returns AE_OK only when publication is suppressed.
     * An already submitted terminal commit is AE_TOO_LATE, never cancelled. */
    ae_result_t (*cancel_finish)(void *, const ae_id_t *, uint64_t terminal_revision);
    void (*completion_notify)(void *, void (*notify)(void *), void *notify_context);
} ae_store_port_t;
