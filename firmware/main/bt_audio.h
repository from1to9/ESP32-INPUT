// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_io.h"
#include "bt_audio_btc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_AUDIO_PCM_FRAMES 480U
#define BT_AUDIO_PCM_MAX_AGE_US UINT64_C(150000)
#define BT_AUDIO_INIT_TIMEOUT_US UINT64_C(5000000)
#define BT_AUDIO_COMMAND_TIMEOUT_US UINT64_C(15000000)
#define BT_AUDIO_PAIRING_DEFAULT_MS 120000U
#define BT_AUDIO_PAIRING_MAX_MS 300000U
#define BT_AUDIO_CLASSIC_BONDS_MAX 32U
#define BT_AUDIO_PEER_NAME_MAX_BYTES 64U
#define BT_AUDIO_DEVICE_NAME_MAX_BYTES 64U

typedef struct {
    uint8_t address[6];           /* Actual saved BR/EDR address, never a HID key. */
    bool name_known;             /* SDK bond-list export has no saved names. */
    char name[BT_AUDIO_PEER_NAME_MAX_BYTES + 1U];
    bool hfp_current;
    bool a2dp_current;
} bt_audio_bond_t;
typedef struct {
    bool known;
    bool refreshing;
    esp_err_t error;
    uint64_t revision;
    uint8_t count;
    uint8_t capacity;            /* Shared Classic/LE configured storage limit. */
    bt_audio_bond_t bonds[BT_AUDIO_CLASSIC_BONDS_MAX];
} bt_audio_bond_list_t;

typedef enum { BT_AUDIO_OFF, BT_AUDIO_HFP, BT_AUDIO_A2DP } bt_audio_route_t;
typedef enum {
    BT_AUDIO_JOB_IDLE, BT_AUDIO_JOB_PENDING, BT_AUDIO_JOB_WAITING,
    BT_AUDIO_JOB_SUCCEEDED, BT_AUDIO_JOB_FAILED, BT_AUDIO_JOB_CANCELLED,
    BT_AUDIO_JOB_UNCERTAIN
} bt_audio_job_status_t;

/* Signed mono PCM16 samples. The SDK boundary uses PCM16LE. Samples are not
 * encoded SBC/mSBC frames. timestamp_us is monotonic capture/arrival time. */
typedef struct {
    uint64_t generation;
    uint64_t consumer_generation; /* Immutable playback admission; zero for raw unbound PCM. */
    uint64_t timestamp_us;
    bt_audio_route_t source;
    uint32_t sample_rate;
    uint16_t frames;
    int16_t samples[BT_AUDIO_PCM_FRAMES];
} bt_audio_pcm_t;

typedef struct {
    bool supported;              /* Compiled S31/Classic + HCI/internal-codec path. */
    bool initialization_attempted;
    bool initialized;            /* Both actual profile-init callbacks received. */
    bool failed;
    esp_err_t error;             /* First historical failure, not current readiness. */
    const char *first_failure_stage; /* Static literal, no peer/sample content. */
    uint64_t first_failure_us;
    bool hfp_initialized;
    bool a2dp_initialized;
    bool hfp_connected;          /* HFP service-level connection, not just RFCOMM. */
    bool hfp_audio_connected;
    bool a2dp_connected;
    bool a2dp_streaming;
    bool hfp_format_known;
    uint32_t hfp_sample_rate;     /* CVSD 8000 or mSBC 16000; LC3 unavailable. */
    bool a2dp_format_known;
    uint32_t a2dp_sample_rate;    /* Negotiated SBC 16000/32000/44100/48000. */
    uint8_t a2dp_source_channels; /* Actual decoded input channels, 1 or 2. */
    bool hfp_peer_known;
    uint8_t hfp_peer[6];
    bool a2dp_peer_known;
    uint8_t a2dp_peer[6];
    bt_audio_route_t route;
    bool microphone_muted;       /* Egress gate only, not physical codec mute. */
    bool speaker_muted;          /* Read/queue gate only, not hardware mute. */
    bool microphone_write_ready;
    bool speaker_read_ready;
    uint64_t generation;
    uint64_t hfp_link_generation, a2dp_link_generation; /* Version of each profile, including while disconnected. */
    uint64_t request_id;
    bt_audio_job_status_t request_status;
    esp_err_t request_error;
    bool control_inflight;      /* May remain true after local cancellation. */
    bool classic_bonds_known;
    uint8_t classic_bond_count;
    uint8_t classic_bond_capacity;
    bool classic_hfp_peer_bonded;
    bool classic_a2dp_peer_bonded;
    bool device_name_known;       /* Actual Classic GAP cache readback, not the HID name. */
    char device_name[BT_AUDIO_DEVICE_NAME_MAX_BYTES + 1U];
    esp_err_t device_name_error;
    bool pairing_completed;      /* Admitted authentication and verified saved peer. */
    bool pairing_expired;
    bool pairing_busy;           /* Held until Classic releases shared admission after closure/auth. */
    bool pairing_requested;
    uint32_t pairing_remaining_ms;
    bool discovery_known;       /* SDK GAP cache evidence, not radio readback. */
    bool discoverable;
    bool pairing_handshake_pending;
    esp_err_t pairing_error;
    uint16_t microphone_queued_frames;
    uint16_t speaker_queued_frames;
    uint32_t receive_overruns;
    uint32_t transmit_overruns;
    uint32_t expired_packets;
    uint32_t malformed_packets;
    uint32_t transmit_underflows;
} bt_audio_snapshot_t;

/* Boot-lifetime A2DP receive totals, independent of the current playback grant.
 * Sample counts are per-channel PCM frames. No audio payload or peer address. */
typedef struct {
    uint64_t callback_bytes, enqueued_samples, emitted_samples, link_generation;
    uint32_t callbacks, gated_callbacks, largest_callback_bytes;
    uint32_t starts, stops, overruns, expired, malformed;
    uint32_t partial_expiries; /* Subset of expired that originated in the frame assembler. */
    uint64_t gap_discarded_samples; /* Samples discarded from the A2DP ring and assembler on expiry or overrun. */
    uint64_t max_callback_gap_us; /* Timings restart on a new A2DP grant. */
    uint32_t callback_intervals;
    uint16_t queued_samples;
    uint8_t peak_slots;
    bool known, connected, streaming, bound;
} bt_audio_a2dp_rx_t;
void bt_audio_a2dp_rx_snapshot(bt_audio_a2dp_rx_t *out);

/* Call once from a task AFTER the shared owner has enabled a Classic-capable
 * BTDM controller and Bluedroid. This module never initializes/enables/disables
 * either singleton, automatically pairs,
 * opens a codec/I2S device, records, or starts microphone capture.
 *
 * Requires Classic A2DP Sink and HFP HF over HCI, using pinned SDK built-in
 * SBC/mSBC codecs (external-codec and LC3 modes unsupported). A BLE-only image
 * returns ESP_ERR_NOT_SUPPORTED truthfully. ESP_OK accepts asynchronous profile
 * initialization; poll initialized/failed. Partial initialization is not retried
 * or torn down behind the shared owner's back; recovery requires stack reset/
 * reboot and reinitialization of the complete shared lifecycle.
 *
 * Resources: sixteen bounded 480-frame mono packet slots allocated once,
 * preferring PSRAM, plus one state-only watchdog. No transport task is created.
 * Audio starts OFF with both gates muted. Profile connection/format changes
 * invalidate queued PCM; reconnection never authorizes microphone egress. */
esp_err_t bt_audio_init(void);
/* A matching terminal receipt covers the BR/EDR local-name readback and
 * complete-name EIR configuration. Fresh discovery is a separate observation. */
typedef struct { uint64_t request_id; bool available, busy, uncertain; esp_err_t error; } bt_audio_name_change_t;
esp_err_t bt_audio_name_request(const char *target, uint64_t *id);
bt_audio_name_change_t bt_audio_name_change(void);

/* One manager/control task calls at least every 10 ms. Posts at most one
 * control item to the BTC owner and a bounded microphone-ready notification.
 * SDK calls are outside state locks and can wait on the SDK's own queue;
 * never call from audio, Bluetooth, UI or interrupt callbacks. */
void bt_audio_service(void);

/* Task-only shared-manager policy. Enabling a route requires its negotiated
 * stream format/connection. OFF requires both mute flags. A2DP cannot unmute
 * microphone. This is not consent: caller owns lock/privacy/indicator/resource
 * checks and physical codec control. Changing policy flushes all queued PCM and
 * returns a fresh generation (output may be NULL). */
esp_err_t bt_audio_set_policy(uint64_t generation, bt_audio_route_t route,
                              bool microphone_muted, bool speaker_muted,
                              uint64_t *new_generation);

/* Immediate local gate/cancellation, no Bluetooth or codec call. Cancels queued
 * control intent; an already submitted request remains quarantined until its
 * actual completion, including after a timeout. Previously copied SDK frames cannot be
 * recalled. Does not claim physical microphone-off or peer disconnection. */
uint64_t bt_audio_cancel(void);
/* Cancel only this queued/in-flight profile command. A submitted SDK request
 * stays reserved until its matching terminal callback. */
void bt_audio_cancel_request(uint64_t request_id);

/* Explicit profile/voice operation on a manager-authorized Classic peer.
 * One bounded request; ESP_OK is submission acceptance, not connection success.
 * Does not grant pairing, dial/answer a call or unmute capture. peer may be NULL
 * only for voice/disconnect operations, which use the verified current peer.
 * Pass the shared and target profile generations from the same observation.
 * Both are checked atomically; acceptance mutes and flushes PCM
 * on the affected route. Poll request_id/status and actual connection fields. */
esp_err_t bt_audio_request(uint64_t generation, uint64_t link_generation, bt_audio_command_t command,
                           const uint8_t peer[6], uint64_t *request_id);
/* UI/control saved-peer operations additionally require this exact fresh
 * Classic bond-list revision and an address still present in that list.
 * Same bounded command queue/result semantics as bt_audio_request. */
esp_err_t bt_audio_saved_request(uint64_t generation, uint64_t link_generation, uint64_t bond_revision,
                                 bt_audio_command_t command, const uint8_t peer[6], uint64_t *request_id);
/* Cached bounded copy, no SDK calls. Unknown/read failure retains old rows
 * labelled unknown. Names stay explicitly unavailable; no identity inference. */
/* Same-lock observation directly into caller-owned storage, without a large
 * return-value temporary. NULL is ignored. */
void bt_audio_copy_bond_list(bt_audio_bond_list_t *out);
/* Compatibility wrapper; new stack-sensitive callers should use copy-out. */
bt_audio_bond_list_t bt_audio_bond_list(void);
/* Nonblocking refresh request; service performs the SDK read and updates the
 * revision. Acceptance is not a completed/fresh read. */
esp_err_t bt_audio_refresh_bonds(void);

/* One audio-manager producer/consumer. Writes require current HFP generation,
 * negotiated rate, explicit unmute and fresh capture timestamp. All-or-none;
 * no resampling or fabricated microphone samples. Reads yield mono HFP or
 * arithmetic-mean downmixed SBC PCM at its actual rate; the manager owns board
 * format conversion. Stale generations and aged packets never replay.
 * Callback storage is never returned to the caller. */
esp_err_t bt_audio_write_microphone(const bt_audio_pcm_t *packet);
bool bt_audio_read_speaker(uint64_t generation, bt_audio_pcm_t *packet);

/* Thread-safe cached copy, no SDK/codec calls or identity logging. Transport
 * readiness is not board-audio readiness, AEC, consent or host qualification. */
bt_audio_snapshot_t bt_audio_snapshot(void);

/* Shared Classic GAP/security owner: explicit SSP Just Works window, no MITM
 * or legacy PIN/passkey-entry claim. Close LE pairing before opening; a shared
 * reservation prevents simultaneous Classic/LE admission and bond eviction.
 * ESP_OK is acceptance only; service applies/polls bounded discovery state.
 * Boot is non-discoverable; close/expiry deny new SSP immediately. An admitted
 * handshake retains the guard until actual auth completion or uncertain fault.
 * Existing Classic bonds may reconnect outside the window. No bonds removed. */
esp_err_t bt_audio_pairing_open(uint32_t duration_ms);
/* Only a confirmed local handoff may close LE pairing. The resource predicate
 * reads owner revisions under their locks and must not block or call back into BT. */
esp_err_t bt_audio_pairing_open_reviewed(uint32_t duration_ms, const bt_audio_snapshot_t *expected,
                                        uint64_t bonds, bool (*resources_current)(void));
void bt_audio_pairing_close(void);

/* Audio-manager port. Capabilities identify the actual Classic media peer and
 * a separate stream generation, never the BLE HID host. The manager must use
 * the exact token on prepare/capture/stop. HFP is mono 8/16 kHz, A2DP is mono at
 * its negotiated SBC rate; no resampling. Physical qualification stays false.
 * Stop confirms local queue/producer gating, not peer buffers or codec mute. */
ae_route_port_t bt_audio_route_port(void);
/* Called by the serialized manager task. Reframes copied PCM into complete
 * 10 ms frames, preserving the prepared audio generation and route token;
 * consumer_generation is the explicitly bound playback job. No
 * manager, codec or Bluetooth API is invoked. AE_AGAIN retains partial PCM. */
ae_result_t bt_audio_receive(ae_frame_t *frame);

#ifdef __cplusplus
}
#endif
