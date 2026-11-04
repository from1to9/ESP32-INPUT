// SPDX-License-Identifier: Apache-2.0
#include "app_media_controls.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define MEDIA_CONTROLS_TASK_BSS EXT_RAM_BSS_ATTR
#else
#define MEDIA_CONTROLS_TASK_BSS
#endif
#include "app_media_runtime.h"
#include "app_recording_store.h"
#include "board_audio_mosaico.h"
#include "board_recording_flash.h"
#include "audio_media.h"
#include "bt_audio.h"
#include "esp_timer.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CONTROL_PROFILE_MAX APP_MEDIA_PROFILES
#define RECORD_OBJECT_LIMIT 4U
#define RECORD_QUOTA_MAX (512U * 1024U)
#define LOCAL_RETENTION_SECONDS 3600U
#define LIVE_SECONDS 3600U
#define SPEECH_SECONDS 8U
#define CURSOR_HISTORY 16U
#define MIC_LEVEL_FRESH_MS 300U
_Static_assert(APP_MEDIA_USB_START_SETTLE_MS > 0, "USB start settling must be positive");

typedef struct {
    bool initialized, clock_known, clock_failed, runtime_known, store_known;
    bool list_requested, list_loaded, query_get, pending, prepare_review;
    app_confirmation_t confirmation;
    bool capture_obligation;
    bool message_clear_pending, audio_pairing_owned;
    esp_err_t audio_pairing_request_error;
    uint64_t capture_generation, confirmation_epoch;
    uint64_t now_ms, serial, pending_token, revision, store_revision, speech_revision;
    uint64_t old_audio_generation, old_audio_revision, old_speech_token;
    uint64_t consumed_speech_token;
    sl_state_t old_speech_state;
    char old_candidate;
    uint64_t prepare_generation, prepare_revision;
    app_media_action_t pending_action;
    app_media_controls_context_t context;
    app_media_snapshot_t runtime;
    app_recording_store_snapshot_t store;
    board_recording_flash_status_t physical;
    ae_codec_capabilities_t codec;
    bt_audio_snapshot_t bluetooth;
    bt_audio_bond_list_t bonds;
    uint8_t selected_peer[6];
    bool selected_peer_known;
    struct {
        app_boot_audio_t desired;
        void (*save)(const app_boot_audio_t *);
        bool route_pending, pending_playback;
        uint8_t ignore_connected;
        const char *notice;
        uint8_t connect_left, submitted_route, submitted_speaker;
        uint64_t connect_id, automatic_token, attempted_stream, listen_after_us, seen_link_request;
        ae_route_token_t submitted_token;
        ae_id_t speaker_job;
    } resume;
    unsigned device_page;
    uint64_t device_request_id;
    struct {
        bool active, opening_voice, device_management;
        uint64_t token, deadline_ms, epoch, bond_revision;
        uint8_t peer[6];
    } voice_link;
    ae_route_capabilities_t routes[AE_ROUTE_COUNT];
    bool usb_role_observed[2];
    uint64_t usb_role_since_ms[2];
    app_media_view_t view;
    app_media_review_t review;
    app_recording_storage_view_t storage_view;
    ae_format_t profiles[CONTROL_PROFILE_MAX];
    uint16_t profile_id;
    uint32_t duration, cursors[CURSOR_HISTORY], page, next_cursor;
    ae_record_metadata_t records[APP_MEDIA_ROWS], selected;
    size_t record_count;
    bool selected_known;
    app_media_id_t selected_request;
    uint64_t selected_revision;
    char message[97];
    app_media_mic_level_t microphone_level;
    board_audio_capture_snapshot_t microphone_level_source;
    bool microphone_level_cached;
    bool microphone_level_clock_known, microphone_level_clock_failed; /* Clock failure lasts until owner init. */
    uint64_t microphone_level_read_ms;
} controls_t;
/* Coordinator task only, after PSRAM initialization; no ISR/cache-disabled access. */
static MEDIA_CONTROLS_TASK_BSS controls_t controls;
/* Distinct from the retained controls: compare old and incoming observations
 * before replacement. Only the serialized coordinator polls this scratch;
 * no callback, nested request, ISR or cache-disabled path may reuse it. */
typedef struct {
    app_media_snapshot_t runtime;
    app_recording_store_snapshot_t store;
    bt_audio_bond_list_t bonds;
    ae_record_metadata_t records[APP_MEDIA_ROWS];
    board_audio_capture_snapshot_t capture_level;
} poll_scratch_t;
static MEDIA_CONTROLS_TASK_BSS poll_scratch_t poll_scratch;

static void clear_poll_scratch(void)
{
    volatile unsigned char *bytes = (volatile unsigned char *)&poll_scratch;
    for (size_t i = 0; i < sizeof(poll_scratch); ++i) bytes[i] = 0;
}

static void increase(uint64_t *value)
{
    if (*value != UINT64_MAX) ++*value;
    else controls.clock_failed = true;
}
static const char *result_text(ae_result_t result)
{
    switch (result) {
    case AE_OK: return "Operation completed.";
    case AE_AGAIN: return "Operation pending.";
    case AE_BUSY: return "Audio resources are busy.";
    case AE_STALE: return "State changed. Review the current controls.";
    case AE_PRIVACY: return "Unlock the device and confirm microphone access.";
    case AE_DENIED: return "This operation is not authorized.";
    case AE_QUOTA: return "Recording capacity or object limit reached.";
    case AE_NOT_FOUND: return "Recording no longer available.";
    case AE_UNCERTAIN: return "Outcome unconfirmed; reconcile before retrying.";
    case AE_TIME_UNKNOWN: return "Trusted time is unavailable for recording retention.";
    case AE_NOT_SUPPORTED: return "This operation is not supported by this route.";
    case AE_UNAVAILABLE: return "The required device or service is unavailable.";
    case AE_INTEGRITY: return "Integrity check failed; existing data is preserved.";
    case AE_TIMEOUT: return "Operation timed out; inspect current state.";
    case AE_EXHAUSTED: return "Resource limit reached.";
    default: return "Audio operation failed.";
    }
}
static void clear_message(void)
{
    controls.message[0] = '\0';
    controls.message_clear_pending = true;
}
static void emit_message_clear(app_media_controls_effect_t *effect)
{
    if (controls.message_clear_pending && effect) {
        effect->handled = true; effect->message[0] = '\0'; controls.message_clear_pending = false;
    }
}
static void effect_result(app_media_controls_effect_t *effect, ae_result_t result)
{
    /* Buttons and job state show progress. Only failures need a notice. */
    if (result == AE_OK || result == AE_AGAIN) {
        clear_message(); emit_message_clear(effect); return;
    }
    controls.message_clear_pending = false;
    snprintf(controls.message, sizeof(controls.message), "%s", result_text(result));
    if (effect) { effect->handled = true; snprintf(effect->message, sizeof(effect->message), "%s", controls.message); }
}
static app_media_id_t ui_id(ae_id_t id)
{
    app_media_id_t output; memcpy(output.bytes, id.bytes, sizeof(output.bytes)); return output;
}
static ae_id_t engine_id(app_media_id_t id)
{
    ae_id_t output; memcpy(output.bytes, id.bytes, sizeof(output.bytes)); return output;
}
static bool same_ui_id(app_media_id_t a, app_media_id_t b)
{ return !memcmp(a.bytes, b.bytes, sizeof(a.bytes)); }
static app_media_id_t peer_id(const uint8_t address[6])
{
    app_media_id_t id = {.bytes = {0xb7, 0xc1}};
    memcpy(id.bytes + 10, address, 6); return id;
}
static int cached_bond(const uint8_t peer[6])
{
    if (controls.bonds.count > BT_AUDIO_CLASSIC_BONDS_MAX) return -1;
    for (unsigned i = 0; i < controls.bonds.count; ++i)
        if (!memcmp(controls.bonds.bonds[i].address, peer, 6)) return (int)i;
    return -1;
}
static int saved_bond(const uint8_t peer[6])
{
    if (!controls.bonds.known || controls.bonds.refreshing || controls.bonds.error != ESP_OK) return -1;
    return cached_bond(peer);
}
static int selected_bond(void)
{ return controls.selected_peer_known ? saved_bond(controls.selected_peer) : -1; }
static bool voice_ready(const bt_audio_snapshot_t *bt)
{
    return bt->hfp_connected && bt->hfp_peer_known && bt->classic_hfp_peer_bonded &&
        bt->hfp_audio_connected && bt->hfp_format_known &&
        (bt->hfp_sample_rate == 8000 || bt->hfp_sample_rate == 16000);
}
static bool same_route(const ae_route_capabilities_t *a, const ae_route_capabilities_t *b)
{
    return a->implemented == b->implemented && a->available == b->available && a->connected == b->connected &&
        a->qualified == b->qualified && a->capture == b->capture && a->playback == b->playback &&
        a->full_duplex == b->full_duplex && a->reason == b->reason && ae_format_equal(&a->format, &b->format) &&
        ae_route_token_equal(&a->token, &b->token);
}
static bool same_codec(const ae_codec_capabilities_t *a, const ae_codec_capabilities_t *b)
{
    if (a->implemented != b->implemented || a->available != b->available || a->qualified != b->qualified ||
        a->full_duplex != b->full_duplex || a->format_count != b->format_count ||
        a->safe_volume_max != b->safe_volume_max || a->reason != b->reason || a->format_count > AE_FORMATS_MAX) return false;
    for (size_t i = 0; i < a->format_count; ++i)
        if (a->formats[i].roles != b->formats[i].roles || a->formats[i].qualified != b->formats[i].qualified ||
            !ae_format_equal(&a->formats[i].format, &b->formats[i].format)) return false;
    return true;
}
static bool format_supported(const ae_format_t *format, ae_role_t role)
{
    if (!controls.codec.available || controls.codec.format_count > AE_FORMATS_MAX) return false;
    for (size_t i = 0; i < controls.codec.format_count; ++i)
        if ((controls.codec.formats[i].roles & role) && ae_format_equal(&controls.codec.formats[i].format, format)) return true;
    return false;
}
static bool owner_ready(bool resume)
{
    return controls.initialized && controls.clock_known && !controls.clock_failed &&
        controls.context.local_access == APP_LOCAL_NORMAL && !controls.context.locked &&
        (!controls.context.screen_off || (resume && controls.context.display_asleep)) && controls.runtime_known &&
        controls.runtime.initialized && controls.runtime.audio_known;
}
static bool local_ready(void) { return owner_ready(false); }
static bool idle_for(bool resume)
{ return owner_ready(resume) && !controls.pending && !controls.voice_link.active && !controls.runtime.busy && !controls.runtime.result_ready; }
static bool idle_controls(void) { return idle_for(false); }
static bool job_active(const ae_job_snapshot_t *job)
{
    return job->state == AE_JOB_STARTING || job->state == AE_JOB_RUNNING || job->state == AE_JOB_STOPPING ||
        job->state == AE_JOB_FINALIZING || job->cleanup_pending;
}
static bool any_jobs(void)
{
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) if (job_active(&controls.runtime.audio.capture[i])) return true;
    return job_active(&controls.runtime.audio.playback);
}
static void finish_voice_link(app_media_controls_effect_t *effect, ae_result_t result)
{
    /* Do not globally cancel a possibly replaced transport request. The BT
     * owner retains submitted commands and their timeout/quarantine. Closing
     * this local sequence prevents any subsequent OPEN_VOICE submission. */
    memset(&controls.voice_link, 0, sizeof(controls.voice_link));
    increase(&controls.revision); effect_result(effect, result);
}
static ae_result_t submit_voice_phase(bool opening_voice)
{
    if (controls.now_ms > UINT64_MAX - BT_AUDIO_COMMAND_TIMEOUT_US / 1000U) return AE_EXHAUSTED;
    uint64_t token = 0;
    esp_err_t error = bt_audio_saved_request(controls.bluetooth.generation, controls.bluetooth.hfp_link_generation,
        controls.voice_link.bond_revision, opening_voice ? BT_AUDIO_OPEN_VOICE : BT_AUDIO_CONNECT_HFP,
        controls.voice_link.peer, &token);
    if (error != ESP_OK) return error == ESP_ERR_INVALID_STATE || error == ESP_ERR_NOT_FOUND ? AE_STALE : AE_UNAVAILABLE;
    if (!token || token == controls.voice_link.token) return AE_UNCERTAIN;
    controls.voice_link.token = token;
    if (controls.voice_link.device_management) controls.device_request_id = token;
    controls.voice_link.opening_voice = opening_voice;
    controls.voice_link.deadline_ms = controls.now_ms + BT_AUDIO_COMMAND_TIMEOUT_US / 1000U;
    controls.voice_link.active = true;
    increase(&controls.revision);
    return AE_AGAIN;
}
static ae_result_t connect_voice(void)
{
    if (!idle_controls() || any_jobs()) return AE_BUSY;
    const bt_audio_snapshot_t *bt = &controls.bluetooth;
    const uint8_t *peer = bt->hfp_connected ? bt->hfp_peer : controls.selected_peer;
    if ((bt->hfp_connected && !bt->hfp_peer_known) || saved_bond(peer) < 0) return AE_STALE;
    controls.voice_link.epoch = controls.context.epoch;
    controls.voice_link.device_management = controls.context.bluetooth_audio_devices;
    controls.voice_link.bond_revision = controls.bonds.revision;
    memcpy(controls.voice_link.peer, peer, 6);
    ae_result_t result = submit_voice_phase(bt->hfp_connected);
    if (result != AE_AGAIN) memset(&controls.voice_link, 0, sizeof(controls.voice_link));
    return result;
}
static void poll_voice_link(app_media_controls_effect_t *effect)
{
    if (!controls.voice_link.active) return;
    const bt_audio_snapshot_t *bt = &controls.bluetooth;
    if (!controls.runtime_known || !controls.runtime.initialized || !controls.runtime.audio_known) {
        finish_voice_link(effect, AE_UNAVAILABLE); return;
    }
    bool manager = controls.voice_link.device_management;
    if (!local_ready() || controls.context.epoch != controls.voice_link.epoch ||
        (manager ? !controls.context.bluetooth_audio_devices : controls.context.mode != INPUT_AUDIO)) {
        finish_voice_link(effect, AE_PRIVACY); return;
    }
    if ((!manager && controls.runtime.audio.route != AE_ROUTE_HFP_HF) || controls.runtime.audio.quarantined ||
        controls.runtime.busy || controls.runtime.result_ready || any_jobs() ||
        !bt->initialized || bt->failed || bt->request_id != controls.voice_link.token ||
        (bt->hfp_peer_known && memcmp(bt->hfp_peer, controls.voice_link.peer, 6)) ||
        (controls.voice_link.opening_voice && (!bt->hfp_connected || !bt->hfp_peer_known))) {
        finish_voice_link(effect, AE_STALE); return;
    }
    if (controls.now_ms >= controls.voice_link.deadline_ms) { finish_voice_link(effect, AE_TIMEOUT); return; }
    if (controls.bonds.refreshing) return;
    if (controls.bonds.revision != controls.voice_link.bond_revision || saved_bond(controls.voice_link.peer) < 0) {
        finish_voice_link(effect, AE_STALE); return;
    }
    if (bt->request_status == BT_AUDIO_JOB_PENDING || bt->request_status == BT_AUDIO_JOB_WAITING) return;
    if (bt->request_status != BT_AUDIO_JOB_SUCCEEDED) {
        finish_voice_link(effect, bt->request_status == BT_AUDIO_JOB_FAILED ? AE_IO_ERROR :
            bt->request_status == BT_AUDIO_JOB_CANCELLED ? AE_STALE : AE_UNCERTAIN); return;
    }
    if (bt->control_inflight || !bt->hfp_connected || !bt->hfp_peer_known || !bt->classic_hfp_peer_bonded) {
        finish_voice_link(effect, AE_UNCERTAIN); return;
    }
    if (voice_ready(bt)) { finish_voice_link(effect, AE_OK); return; }
    if (controls.voice_link.opening_voice) { finish_voice_link(effect, AE_UNCERTAIN); return; }
    /* Exact successful SLC token and peer permit only the second connection
     * phase. This queues no codec, role, live consumer or microphone grant. */
    ae_result_t result = submit_voice_phase(true);
    if (result != AE_AGAIN) finish_voice_link(effect, result);
    else effect_result(effect, result);
}
static bool capture_jobs(void)
{
    for (unsigned i = 0; i < AE_CONSUMERS_MAX; ++i) if (job_active(&controls.runtime.audio.capture[i])) return true;
    return false;
}
static bool route_ready(ae_route_t route, ae_role_t role)
{
    const ae_route_capabilities_t *caps = &controls.routes[route];
    return caps->implemented && caps->available && caps->connected &&
        (role == AE_MICROPHONE ? caps->capture : caps->playback);
}
static void observe_usb_start_readiness(const ae_route_capabilities_t *next)
{
    const ae_route_capabilities_t *old = &controls.routes[AE_ROUTE_USB_UAC];
    bool same = ae_route_token_equal(&old->token, &next->token) && ae_format_equal(&old->format, &next->format);
    size_t bytes; uint32_t samples;
    bool valid = controls.clock_known && !controls.clock_failed && controls.runtime_known &&
        controls.runtime.initialized && controls.runtime.audio_known && next->implemented && next->available &&
        next->connected && ae_route_token_valid(&next->token, false) &&
        ae_format_size(&next->format, &bytes, &samples) == AE_OK;
    for (unsigned i = 0; i < 2; ++i) {
        bool role_valid = valid && (i == 0 ? next->capture : next->playback);
        if (!role_valid) {
            controls.usb_role_observed[i] = false;
            controls.usb_role_since_ms[i] = 0;
        } else if (!same || !controls.usb_role_observed[i]) {
            controls.usb_role_observed[i] = true;
            controls.usb_role_since_ms[i] = controls.now_ms;
        }
    }
}
static bool usb_start_settled(ae_role_t role)
{
    unsigned i = role == AE_MICROPHONE ? 0U : 1U;
    return controls.usb_role_observed[i] && controls.clock_known && !controls.clock_failed &&
        controls.now_ms >= controls.usb_role_since_ms[i] &&
        controls.now_ms - controls.usb_role_since_ms[i] >= APP_MEDIA_USB_START_SETTLE_MS;
}
static ae_route_t connection_route(app_media_connection_t connection)
{
    if (connection == APP_MEDIA_CONNECTION_USB) return AE_ROUTE_USB_UAC;
    if (connection == APP_MEDIA_CONNECTION_BLUETOOTH) {
        ae_route_t current = controls.runtime.audio.route;
        if ((current == AE_ROUTE_HFP_HF || current == AE_ROUTE_A2DP_SINK) &&
            (route_ready(current, AE_MICROPHONE) || route_ready(current, AE_SPEAKER))) return current;
        if (route_ready(AE_ROUTE_HFP_HF, AE_MICROPHONE) || route_ready(AE_ROUTE_HFP_HF, AE_SPEAKER)) return AE_ROUTE_HFP_HF;
        if (route_ready(AE_ROUTE_A2DP_SINK, AE_SPEAKER)) return AE_ROUTE_A2DP_SINK;
        /* A supported profile can be selected for explicit link setup. It
         * does not imply a negotiated connection or admit either live role. */
        if (controls.routes[AE_ROUTE_HFP_HF].implemented) return AE_ROUTE_HFP_HF;
        if (controls.routes[AE_ROUTE_A2DP_SINK].implemented) return AE_ROUTE_A2DP_SINK;
    }
    return AE_ROUTE_OFF;
}
static ae_route_t live_route(ae_role_t role)
{
    ae_route_t current = controls.runtime.audio.route;
    if (current != AE_ROUTE_HFP_HF && current != AE_ROUTE_A2DP_SINK) return current;
    if (role == AE_MICROPHONE) return AE_ROUTE_HFP_HF;
    if (route_ready(current, role)) return current;
    return route_ready(AE_ROUTE_A2DP_SINK, role) ? AE_ROUTE_A2DP_SINK : AE_ROUTE_HFP_HF;
}
static app_media_start_block_t live_start_block(ae_role_t role, bool resume)
{
    if (!controls.initialized || !controls.runtime_known || !controls.runtime.initialized || !controls.runtime.audio_known)
        return APP_MEDIA_START_UNKNOWN;
    if (!controls.clock_known || controls.clock_failed) return APP_MEDIA_START_CLOCK_UNAVAILABLE;
    if (controls.context.locked) return APP_MEDIA_START_LOCKED;
    if (controls.context.screen_off && !(resume && role == AE_SPEAKER && controls.context.display_asleep))
        return APP_MEDIA_START_SCREEN_UNAVAILABLE;
    if (controls.pending || controls.voice_link.active || controls.runtime.busy || controls.runtime.result_ready)
        return APP_MEDIA_START_PENDING;
    if (controls.runtime.audio.quarantined) return APP_MEDIA_START_QUARANTINED;
    if ((controls.runtime.audio.route == AE_ROUTE_HFP_HF || controls.runtime.audio.route == AE_ROUTE_A2DP_SINK) &&
        (controls.bluetooth.pairing_busy || controls.bluetooth.pairing_requested || controls.bluetooth.pairing_handshake_pending))
        return APP_MEDIA_START_PENDING;
    ae_route_t selected = live_route(role);
    if (selected <= AE_ROUTE_LOCAL || selected >= AE_ROUTE_COUNT) return APP_MEDIA_START_CONNECTION_REQUIRED;
    const ae_route_capabilities_t *next = &controls.routes[selected];
    if (!next->implemented || next->reason == AE_NOT_SUPPORTED) return APP_MEDIA_START_UNSUPPORTED;
    if (!next->available) return APP_MEDIA_START_ROUTE_UNAVAILABLE;
    if ((selected == AE_ROUTE_HFP_HF || selected == AE_ROUTE_A2DP_SINK) && next->reason == AE_BUSY)
        return APP_MEDIA_START_PENDING;
    if (!next->connected) return APP_MEDIA_START_HOST_UNAVAILABLE;
    if (!(role == AE_MICROPHONE ? next->capture : next->playback))
        return selected == AE_ROUTE_USB_UAC ? APP_MEDIA_START_HOST_STREAM_REQUIRED : APP_MEDIA_START_UNSUPPORTED;
    if (!controls.codec.implemented || !controls.codec.available)
        return controls.codec.reason == AE_AGAIN ? APP_MEDIA_START_PENDING : APP_MEDIA_START_CODEC_UNAVAILABLE;
    if (!format_supported(&next->format, role)) return APP_MEDIA_START_FORMAT_UNSUPPORTED;
    if (!ae_route_token_valid(&next->token, false)) return APP_MEDIA_START_IDENTITY_UNAVAILABLE;
    for (unsigned i = 0; i <= AE_CONSUMERS_MAX; ++i) {
        const ae_job_snapshot_t *job = i == AE_CONSUMERS_MAX ? &controls.runtime.audio.playback : &controls.runtime.audio.capture[i];
        if (job->cleanup_pending || job->state == AE_JOB_STOPPING || job->state == AE_JOB_FINALIZING)
            return APP_MEDIA_START_CLEANUP;
    }
    /* The runtime may select an already negotiated Bluetooth profile before
     * start. Never replace an active/prepared owner or connect another peer. */
    if (selected != controls.runtime.audio.route) {
        const ae_route_capabilities_t *current = &controls.routes[controls.runtime.audio.route];
        if (any_jobs() || !controls.runtime.audio.hardware_off_confirmed) return APP_MEDIA_START_STOP_REQUIRED;
        const ae_id_t *previous_peer = &controls.runtime.audio.route_token.peer_id;
        if (!ae_id_empty(previous_peer) && !ae_id_equal(previous_peer, &next->token.peer_id)) return APP_MEDIA_START_PEER_MISMATCH;
        if (!current->connected && ae_id_empty(&current->token.peer_id)) return APP_MEDIA_START_ALLOWED;
        if (ae_id_empty(&current->token.peer_id)) return APP_MEDIA_START_IDENTITY_UNAVAILABLE;
        return ae_id_equal(&current->token.peer_id, &next->token.peer_id) ? APP_MEDIA_START_ALLOWED : APP_MEDIA_START_PEER_MISMATCH;
    }
    bool duplex = controls.routes[selected].full_duplex && controls.codec.full_duplex;
    bool admitted = role == AE_MICROPHONE ? !capture_jobs() && (!job_active(&controls.runtime.audio.playback) || duplex) :
        !job_active(&controls.runtime.audio.playback) && (!capture_jobs() || duplex);
    if (!admitted) return APP_MEDIA_START_OCCUPIED;
    if (selected == AE_ROUTE_USB_UAC && !usb_start_settled(role)) return APP_MEDIA_START_HOST_PREPARING;
    return APP_MEDIA_START_ALLOWED;
}
static bool speech_context(void)
{
    return controls.context.mode == INPUT_KEYBOARD && controls.context.keyboard_submode == APP_KEYBOARD_SPOKEN &&
        controls.context.overlay == APP_OVERLAY_NONE;
}
static bool valid_identity(const app_media_request_t *request)
{
    return local_ready() && request->management_generation == controls.view.management_generation &&
        request->audio_generation == controls.view.audio_generation && request->resource_revision == controls.view.resource_revision &&
        same_ui_id(request->peer_id, controls.view.peer_id);
}
static app_media_job_state_t job_state(ae_job_state_t state)
{
    switch (state) {
    case AE_JOB_STARTING: return APP_MEDIA_JOB_STARTING;
    case AE_JOB_RUNNING: return APP_MEDIA_JOB_RUNNING;
    case AE_JOB_STOPPING: return APP_MEDIA_JOB_STOPPING;
    case AE_JOB_FINALIZING: return APP_MEDIA_JOB_FINALIZING;
    case AE_JOB_COMPLETED: return APP_MEDIA_JOB_COMPLETED;
    case AE_JOB_PARTIAL: return APP_MEDIA_JOB_PARTIAL;
    case AE_JOB_ERROR: return APP_MEDIA_JOB_ERROR;
    default: return APP_MEDIA_JOB_UNKNOWN;
    }
}
static app_media_job_state_t record_state(ae_record_state_t state)
{
    switch (state) {
    case AE_RECORD_STARTING: return APP_MEDIA_JOB_STARTING;
    case AE_RECORD_RECORDING: return APP_MEDIA_JOB_RUNNING;
    case AE_RECORD_STOPPING: return APP_MEDIA_JOB_STOPPING;
    case AE_RECORD_FINALIZING: return APP_MEDIA_JOB_FINALIZING;
    case AE_RECORD_COMPLETED: return APP_MEDIA_JOB_COMPLETED;
    case AE_RECORD_PARTIAL: return APP_MEDIA_JOB_PARTIAL;
    case AE_RECORD_DELETING: return APP_MEDIA_JOB_DELETING;
    default: return APP_MEDIA_JOB_ERROR;
    }
}
static void map_job(app_media_job_view_t *view, const ae_job_snapshot_t *job)
{
    *view = (app_media_job_view_t){.known = job->state != AE_JOB_EMPTY, .id = ui_id(job->job_id),
        .object_id = ui_id(job->recording_id), .consumer_generation = job->consumer_generation,
        .state = job_state(job->state), .cleanup_pending = job->cleanup_pending};
    bool running = job->state == AE_JOB_STARTING || job->state == AE_JOB_RUNNING;
    view->can_stop = view->can_cancel_keep = running && idle_controls();
    view->can_cancel_discard = view->can_stop && job->kind == AE_CAPTURE_RECORDING;
    if (job->first_frame_seen && controls.now_ms >= job->first_frame_ms) view->elapsed_ms = controls.now_ms - job->first_frame_ms;
    /* Transport delivery is not verified durable storage. */
    if (controls.selected_known && ae_id_equal(&controls.selected.object_id, &job->recording_id)) {
        view->stored_bytes = controls.selected.stored_bytes;
        view->verified_bytes = controls.selected.verified_pcm_bytes;
        view->stored_bytes_known = true;
    }
    if (job->error != AE_OK) snprintf(view->reason, sizeof(view->reason), "%s", result_text(job->error));
}
static void map_record(app_media_record_view_t *view, const ae_record_metadata_t *record)
{
    bool valid = !ae_id_empty(&record->object_id) && !record->access_revoked;
    *view = (app_media_record_view_t){.valid = valid, .id = ui_id(record->object_id), .revision = record->revision,
        .state = record_state(record->state), .stored_bytes = record->stored_bytes, .bytes_known = valid,
        .verified = valid && record->integrity_verified && record->sync_acknowledged && record->hash_available,
        .can_delete = valid && idle_controls() && !any_jobs() && controls.store.capabilities.available &&
            controls.store.capabilities.retention_time_known};
    snprintf(view->label, sizeof(view->label), "Recording %02X%02X%02X%02X", record->object_id.bytes[0], record->object_id.bytes[1],
        record->object_id.bytes[2], record->object_id.bytes[3]);
    if (record->format.sample_rate_hz && record->sample_frames <= UINT64_MAX / 1000U) {
        view->duration_known = valid;
        view->duration_ms = record->sample_frames * 1000U / record->format.sample_rate_hz;
    }
    view->can_play = valid && record->state == AE_RECORD_COMPLETED && view->verified && idle_controls() && !any_jobs() && format_supported(&record->format, AE_SPEAKER);
    if (record->reason != AE_OK) snprintf(view->reason, sizeof(view->reason), "%s", result_text(record->reason));
}
static uint64_t free_quota(const ae_store_capabilities_t *caps)
{
    return caps->used_bytes <= caps->quota_bytes && caps->reserved_bytes <= caps->quota_bytes - caps->used_bytes ?
        caps->quota_bytes - caps->used_bytes - caps->reserved_bytes : 0;
}
static uint32_t recording_seconds(const ae_store_capabilities_t *caps, const ae_format_t *format, uint64_t budget)
{
    size_t frame_bytes; uint32_t frames;
    if (ae_format_size(format, &frame_bytes, &frames) != AE_OK || !frame_bytes ||
        (!caps->media_chunk_bytes && caps->media_chunk_overhead_bytes)) return 0;
    uint64_t overhead = (uint64_t)caps->per_object_overhead_bytes + AE_WAV_HEADER_BYTES;
    if (budget <= overhead) return 0;
    uint64_t payload = budget - overhead;
    if (caps->media_chunk_bytes) {
        uint64_t cost = (uint64_t)caps->media_chunk_bytes + caps->media_chunk_overhead_bytes;
        payload = payload / cost * (caps->media_chunk_bytes - caps->media_chunk_bytes % frame_bytes);
    }
    payload -= payload % frame_bytes;
    uint64_t seconds = payload / ((uint64_t)format->sample_rate_hz * format->channels * 2U);
    return seconds > LIVE_SECONDS ? LIVE_SECONDS : (uint32_t)seconds;
}
static void map_storage(void)
{
    const app_recording_store_snapshot_t *store = &controls.store;
    const ae_store_capabilities_t *caps = &store->capabilities;
    app_recording_storage_view_t *view = &controls.storage_view;
    *view = (app_recording_storage_view_t){.generation = 1, .revision = controls.store_revision,
        .backend = APP_RECORD_BACKEND_INTERNAL_RESERVED};
    if (!controls.store_known || !store->initialized) { snprintf(view->reason, sizeof(view->reason), "Recording storage status unavailable."); return; }
    view->reserved_region_verified = controls.physical.geometry_verified &&
        (controls.physical.state == BOARD_RECORDING_FLASH_ERASED || controls.physical.state == BOARD_RECORDING_FLASH_OWNED);
    view->can_prepare = idle_controls() && !any_jobs() && view->reserved_region_verified &&
        controls.physical.state == BOARD_RECORDING_FLASH_ERASED && !store->preparing;
    view->state = store->preparing || store->store.provisioning ? APP_RECORD_STORE_PREPARING :
        store->store.storage_uncertain || store->error == AE_UNCERTAIN ? APP_RECORD_STORE_UNCERTAIN :
        caps->available && caps->owned_and_provisioned && caps->encryption_ready && caps->authenticated_metadata && caps->recovery_complete ?
            (free_quota(caps) >= 65536 ? APP_RECORD_STORE_READY : APP_RECORD_STORE_FULL) :
        controls.physical.state == BOARD_RECORDING_FLASH_ERASED ? APP_RECORD_STORE_UNPREPARED :
        store->error == AE_INTEGRITY || store->error == AE_IO_ERROR ? APP_RECORD_STORE_ERROR : APP_RECORD_STORE_UNKNOWN;
    view->capacity_known = caps->available && caps->used_bytes <= caps->quota_bytes && caps->reserved_bytes <= caps->quota_bytes - caps->used_bytes;
    if (view->capacity_known) { view->free_bytes = free_quota(caps); view->quota_bytes = caps->quota_bytes; }
    snprintf(view->reason, sizeof(view->reason), "%s", view->can_prepare ?
        "Prepare the verified erased internal region. Existing unknown contents are preserved." : result_text(store->error));
}
static void map_options(void)
{
    app_media_record_options_t *options = &controls.view.recording_options;
    *options = (app_media_record_options_t){.retention_seconds = LOCAL_RETENTION_SECONDS};
    size_t count = controls.codec.format_count <= AE_FORMATS_MAX ? controls.codec.format_count : 0;
    size_t added = 0;
    for (size_t i = 0; i < count && added < CONTROL_PROFILE_MAX; ++i) {
        const ae_profile_t *profile = &controls.codec.formats[i];
        size_t bytes; uint32_t frames;
        if (!(profile->roles & AE_MICROPHONE) || ae_format_size(&profile->format, &bytes, &frames) != AE_OK) continue;
        controls.profiles[added] = profile->format;
        app_media_profile_view_t *out = &options->profiles[added];
        out->id = (uint16_t)(i + 1); out->available = controls.codec.available;
        snprintf(out->label, sizeof(out->label), "PCM16 %u Hz / %u ch", (unsigned)profile->format.sample_rate_hz, profile->format.channels);
        if (!controls.profile_id) controls.profile_id = out->id;
        ++added;
    }
    options->profile_count = added; options->profile_id = controls.profile_id;
    size_t selected = 0;
    while (selected < added && options->profiles[selected].id != controls.profile_id) ++selected;
    bool profile = selected < added && options->profiles[selected].available;
    options->known = profile && controls.storage_view.capacity_known;
    options->byte_limit = controls.storage_view.free_bytes < RECORD_QUOTA_MAX ? controls.storage_view.free_bytes : RECORD_QUOTA_MAX;
    options->byte_limit -= options->byte_limit % 4096U;
    options->minimum_duration_seconds = 1;
    options->maximum_duration_seconds = profile ? recording_seconds(&controls.store.capabilities, &controls.profiles[selected], options->byte_limit) : 0;
    if (options->maximum_duration_seconds && controls.duration > options->maximum_duration_seconds)
        controls.duration = options->maximum_duration_seconds;
    options->duration_seconds = controls.duration;
    bool ready = options->known && options->maximum_duration_seconds && options->byte_limit >= 65536 &&
        controls.storage_view.state == APP_RECORD_STORE_READY && controls.store.capabilities.retention_time_known &&
        controls.store.capabilities.object_count < controls.store.capabilities.object_limit;
    options->can_start = ready && idle_controls() && !any_jobs();
    options->can_edit = options->known && idle_controls() && !any_jobs();
    snprintf(options->reason, sizeof(options->reason), "%s", !controls.store.capabilities.retention_time_known ? result_text(AE_TIME_UNKNOWN) :
        ready ? "Local recording; review microphone access and limits before starting." : controls.storage_view.reason);
    if (profile) snprintf(controls.storage_view.profile, sizeof(controls.storage_view.profile), "%s", options->profiles[selected].label);
    controls.storage_view.remaining_time_known = options->known;
    controls.storage_view.remaining_seconds = options->maximum_duration_seconds;
}
static void update_microphone_level(const board_audio_capture_snapshot_t *source)
{
    const ae_snapshot_t *audio = &controls.runtime.audio;
    app_media_mic_level_t level = {0};
    if (!controls.runtime_known || !controls.runtime.initialized || !controls.runtime.audio_known ||
        !controls.clock_known || controls.clock_failed || !controls.codec.available ||
        audio->quarantined || audio->error != AE_OK) goto unavailable;
    if (!audio->capture_gate_open) {
        if (audio->hardware_off_confirmed ||
            (audio->microphone_mute_confirmed && audio->microphone_mute_applied)) level.state = APP_MEDIA_LEVEL_OFF;
        else if (audio->capture_indicator_required && !audio->microphone_muted) level.state = APP_MEDIA_LEVEL_WAITING;
        goto unavailable;
    }
    if (!audio->microphone_enabled || audio->microphone_muted || controls.microphone_level_clock_failed) goto unavailable;
    if (!source->known) {
        if (source->result == AE_OK || source->result == AE_AGAIN) level.state = APP_MEDIA_LEVEL_WAITING;
        goto unavailable;
    }
    /* The board worker may publish after the coordinator sampled now_ms.
     * Order the snapshot copy before this clock read instead of treating
     * ordinary concurrent progress as a future-dated sample. */
    int64_t read_us = esp_timer_get_time();
    if (read_us < 0) { controls.microphone_level_clock_failed = true; goto unavailable; }
    uint64_t read_ms = (uint64_t)read_us / 1000U;
    if (controls.microphone_level_clock_failed || read_ms < controls.now_ms ||
        (controls.microphone_level_clock_known && read_ms < controls.microphone_level_read_ms)) {
        controls.microphone_level_clock_failed = true; goto unavailable;
    }
    controls.microphone_level_clock_known = true; controls.microphone_level_read_ms = read_ms;
    if (!source->current || source->result != AE_OK || source->generation != audio->audio_generation ||
        source->observed_ms > read_ms ||
        !source->samples || source->peak_abs > 32768U ||
        source->sum_squares > UINT64_C(1073741824) * source->samples ||
        ((source->sum_squares == 0) != (source->peak_abs == 0))) goto unavailable;
    if (read_ms - source->observed_ms > MIC_LEVEL_FRESH_MS) {
        level.state = APP_MEDIA_LEVEL_WAITING; goto unavailable;
    }
    const board_audio_capture_snapshot_t *cached = &controls.microphone_level_source;
    if (controls.microphone_level_cached && source->generation == cached->generation &&
        source->observed_ms == cached->observed_ms && source->sum_squares == cached->sum_squares &&
        source->samples == cached->samples && source->peak_abs == cached->peak_abs) return;
    level.state = APP_MEDIA_LEVEL_LIVE; level.silent = source->sum_squares == 0;
    level.clipping = source->peak_abs >= 32767U;
    level.dbfs = -96;
    if (!level.silent) {
        /* Convert only a new <=10 Hz snapshot, outside every owner lock.
         * Digital RMS is relative to the PCM16 full-scale magnitude32768. */
        double dbfs = 10.0 * log10((double)source->sum_squares / ((double)source->samples * 1073741824.0));
        if (dbfs < -96.0) dbfs = -96.0;
        if (dbfs > 0.0) dbfs = 0.0;
        level.dbfs = (int16_t)(dbfs - 0.5);
        if (level.dbfs > -60) level.percent = (uint8_t)((level.dbfs + 60) * 100 / 60);
    }
    controls.microphone_level_source = *source; controls.microphone_level_cached = true;
    controls.microphone_level = level; return;
unavailable:
    controls.microphone_level_cached = false; controls.microphone_level = level;
}

static void rebuild_devices(void)
{
    app_media_devices_view_t *view = &controls.view.devices;
    const bt_audio_snapshot_t *bt = &controls.bluetooth;
    view->known = controls.bonds.known && !controls.bonds.refreshing && controls.bonds.error == ESP_OK &&
        controls.bonds.count <= BT_AUDIO_CLASSIC_BONDS_MAX;
    view->refreshing = controls.bonds.refreshing;
    view->revision = controls.bonds.revision;
    /* The owner retains the last successful catalogue during refresh/failure.
     * It is display identity only until saved_bond() verifies current state. */
    view->count = controls.bonds.count <= BT_AUDIO_CLASSIC_BONDS_MAX ? controls.bonds.count : 0;
    view->pages = (view->count + APP_MEDIA_ROWS - 1U) / APP_MEDIA_ROWS;
    if (!view->pages) view->pages = 1;
    if (controls.device_page >= view->pages) controls.device_page = view->pages - 1U;
    view->page = controls.device_page;
    view->pending = controls.voice_link.active || bt->control_inflight ||
        bt->request_status == BT_AUDIO_JOB_PENDING || bt->request_status == BT_AUDIO_JOB_WAITING;
    view->request_failed = bt->request_status == BT_AUDIO_JOB_FAILED || bt->request_status == BT_AUDIO_JOB_UNCERTAIN;
    view->can_refresh = bt->initialized && !bt->failed && !view->refreshing && !view->pending &&
        !any_jobs() && controls.runtime.audio.hardware_off_confirmed && !bt->pairing_busy && !bt->pairing_requested;
    int selected = selected_bond();
    bool idle = idle_controls() && !any_jobs() && controls.runtime.audio.hardware_off_confirmed &&
        bt->initialized && !bt->failed && !view->pending && !bt->pairing_busy && !bt->pairing_requested;
    for (unsigned i = 0; i < APP_MEDIA_ROWS; ++i) {
        unsigned index = view->page * APP_MEDIA_ROWS + i;
        if (index >= view->count) break;
        const bt_audio_bond_t *bond = &controls.bonds.bonds[index];
        app_media_device_row_t *row = &view->rows[i]; row->id = peer_id(bond->address);
        snprintf(row->identity, sizeof(row->identity), "%02X:%02X:%02X:%02X:%02X:%02X", bond->address[0], bond->address[1], bond->address[2], bond->address[3], bond->address[4], bond->address[5]);
        row->connected = bt->initialized && !bt->failed &&
            ((bt->hfp_connected && bt->hfp_peer_known && !memcmp(bt->hfp_peer, bond->address, 6)) ||
             (bt->a2dp_connected && bt->a2dp_peer_known && !memcmp(bt->a2dp_peer, bond->address, 6)));
        row->selected = controls.selected_peer_known && !memcmp(controls.selected_peer, bond->address, 6);
    }
    view->selected_saved = selected >= 0;
    if (controls.selected_peer_known) {
        const uint8_t *peer = controls.selected_peer; view->selected = peer_id(peer);
        memcpy(view->address, peer, sizeof(view->address));
        snprintf(view->identity, sizeof(view->identity), "%02X:%02X:%02X:%02X:%02X:%02X", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
        for (unsigned i = 0; i < 2; ++i) {
            bool connected = i ? bt->a2dp_connected : bt->hfp_connected;
            bool known = i ? bt->a2dp_peer_known : bt->hfp_peer_known;
            const uint8_t *current = i ? bt->a2dp_peer : bt->hfp_peer;
            view->connection_known[i] = bt->initialized && !bt->failed && (!connected || known);
            view->connected[i] = connected && known && !memcmp(current, peer, 6);
            view->can_disconnect[i] = idle && selected >= 0 && view->connected[i];
            view->can_connect[i] = idle && selected >= 0 &&
                (i ? !connected && !known : (view->connected[i] ? !bt->hfp_audio_connected : !connected && !known)) &&
                (!bt->hfp_connected || (bt->hfp_peer_known && !memcmp(bt->hfp_peer, peer, 6))) &&
                (!bt->a2dp_connected || (bt->a2dp_peer_known && !memcmp(bt->a2dp_peer, peer, 6)));
        }
        view->voice_connected = view->connected[0] && bt->hfp_audio_connected;
    }
    snprintf(view->reason, sizeof(view->reason), "%s", view->refreshing ? "Refreshing saved audio devices." :
        !view->known ? "Saved state unknown. Back to devices, then Refresh." : controls.selected_peer_known && !view->selected_saved ? "Device is no longer saved." :
        any_jobs() ? "Stop Mic and Speaker before changing devices." : view->pending ? "Request pending; waiting for Bluetooth." :
        view->request_failed ? "Connection request failed or uncertain. Check host Bluetooth settings." :
        "Connect from this device or your host. Connecting does not start audio.");
}

static void rebuild_audio_pairing(void)
{
    app_media_pairing_view_t *view = &controls.view.pairing;
    const bt_audio_snapshot_t *bt = &controls.bluetooth;
    bool selected = controls.context.bluetooth_audio_devices || (controls.view.connection_known && controls.view.connection == APP_MEDIA_CONNECTION_BLUETOOTH);
    view->name_known = bt->device_name_known;
    if (view->name_known) snprintf(view->device_name, sizeof(view->device_name), "%s", bt->device_name);
    view->remaining_seconds = (bt->pairing_remaining_ms + 999U) / 1000U;
    view->can_open = selected && idle_controls() && !any_jobs() && controls.runtime.audio.hardware_off_confirmed &&
        bt->supported && bt->initialized && !bt->failed && bt->classic_bonds_known && !bt->pairing_requested &&
        !bt->pairing_busy && !bt->pairing_handshake_pending && !bt->control_inflight && bt->request_status != BT_AUDIO_JOB_PENDING &&
        bt->request_status != BT_AUDIO_JOB_WAITING;
    view->can_close = selected && local_ready() && bt->pairing_requested;
    bool current_failure = bt->failed || bt->pairing_error != ESP_OK;
    esp_err_t error = current_failure ? (bt->pairing_error != ESP_OK ? bt->pairing_error : bt->error) : controls.audio_pairing_request_error;
    const char *reason;
    if (!bt->supported) { view->state = APP_MEDIA_PAIR_UNAVAILABLE; reason = "Bluetooth audio is unavailable."; }
    else if (bt->failed) { view->state = APP_MEDIA_PAIR_ERROR; reason = "Bluetooth audio stopped. Restart the device to recover."; }
    else if (bt->pairing_handshake_pending && bt->pairing_error == ESP_ERR_TIMEOUT) {
        view->state = APP_MEDIA_PAIR_ERROR; reason = "Timed out. Waiting for the host's pairing result.";
    }
    else if (error != ESP_OK) {
        view->state = APP_MEDIA_PAIR_ERROR;
        if (!current_failure && error == ESP_ERR_NO_MEM) reason = "Bluetooth pairings are full; existing hosts are preserved.";
        else if (bt->pairing_busy) reason = "Pairing failed. Waiting for discovery to close.";
        else if (!bt->classic_bonds_known) reason = "Saved audio devices are unavailable.";
        else if (error == ESP_ERR_INVALID_STATE) reason = "Pairing is busy. Wait for the current change.";
        else reason = view->can_open ? "Audio pairing failed. Try again." : "Audio pairing failed.";
    }
    else if (!bt->initialized) { view->state = APP_MEDIA_PAIR_UNAVAILABLE; reason = "Bluetooth audio is starting."; }
    else if (bt->pairing_handshake_pending) { view->state = APP_MEDIA_PAIR_AUTHENTICATING; reason = "Finishing audio pairing; audio has not started."; }
    else if (bt->pairing_requested) { view->state = bt->discovery_known && bt->discoverable ? APP_MEDIA_PAIR_DISCOVERABLE : APP_MEDIA_PAIR_PREPARING; reason = view->state == APP_MEDIA_PAIR_DISCOVERABLE ? "Choose this device in your host's Bluetooth settings." : "Preparing audio discoverability..."; }
    else if (bt->pairing_busy) { view->state = APP_MEDIA_PAIR_CLOSING; reason = "Closing audio discoverability. Wait before pairing again."; }
    else if (!bt->classic_bonds_known) { view->state = APP_MEDIA_PAIR_UNAVAILABLE; reason = "Checking saved audio devices."; }
    else if (bt->control_inflight && bt->request_status == BT_AUDIO_JOB_UNCERTAIN) {
        view->state = APP_MEDIA_PAIR_UNAVAILABLE; reason = "Waiting for the host to finish the last audio link request.";
    }
    else if (bt->pairing_expired) { view->state = APP_MEDIA_PAIR_EXPIRED; reason = "Audio pairing expired."; }
    else {
        view->state = bt->pairing_completed ? APP_MEDIA_PAIR_COMPLETED : APP_MEDIA_PAIR_CLOSED;
        bool hfp = controls.view.route == APP_MEDIA_ROUTE_HFP;
        if (hfp && bt->hfp_connected) {
            if (!bt->classic_hfp_peer_bonded) reason = "Voice link is not paired. Disconnect before pairing.";
            else if (voice_ready(bt)) reason = "Voice audio is ready. Choose Mic or Speaker.";
            else if (bt->hfp_audio_connected) reason = "Voice format unavailable. Disconnect and reconnect.";
            else reason = "Voice link connected. Choose Connect to open voice audio.";
        } else if (!hfp && bt->a2dp_connected) {
            if (!bt->classic_a2dp_peer_bonded) reason = "Media link is not paired. Disconnect before pairing.";
            else if (bt->a2dp_streaming && bt->a2dp_format_known) reason = "Host audio is ready. Choose Speaker.";
            else reason = "Media link connected. Start playback on the host.";
        } else if (hfp ? bt->hfp_peer_known : bt->a2dp_peer_known) reason = "Waiting for the audio link to finish connecting or closing.";
        else reason = bt->classic_bond_count ? "Choose a saved audio device and connect." : "Pair audio first; input pairing is separate.";
    }
    snprintf(view->reason, sizeof(view->reason), "%s", reason);
}

static void rebuild(void)
{
    app_media_record_list_view_t list = controls.view.recordings;
    controls.view = (app_media_view_t){.known = controls.runtime_known && controls.runtime.initialized && controls.runtime.audio_known,
        .management_generation = 1, .resource_revision = controls.revision, .recordings = list};
    app_media_view_t *view = &controls.view;
    view->microphone_level = controls.microphone_level;
    const ae_snapshot_t *audio = &controls.runtime.audio;
    view->command_pending = controls.pending || controls.voice_link.active || controls.runtime.busy || controls.runtime.result_ready;
    if (view->known) {
        view->audio_generation = audio->audio_generation;
        view->state = (app_media_state_t)((unsigned)audio->state + 1U);
        view->route = (app_media_route_t)audio->route;
        view->peer_id = ui_id(audio->route_token.peer_id);
        view->microphone = (app_media_role_view_t){.known = true, .enabled = audio->microphone_enabled,
            .muted = audio->microphone_mute_applied, .mute_confirmed = audio->microphone_mute_confirmed};
        view->speaker = (app_media_role_view_t){.known = true, .enabled = audio->speaker_enabled,
            .muted = audio->speaker_mute_applied, .mute_confirmed = audio->speaker_mute_confirmed};
        bool editable = idle_controls() && !audio->quarantined;
        view->microphone.can_disable = editable && audio->microphone_enabled;
        view->speaker.can_disable = editable && audio->speaker_enabled;
        view->microphone.can_mute = editable && audio->microphone_enabled;
        view->speaker.can_mute = editable && audio->speaker_enabled;
        /* Unmute is only a role operation; creation of a bounded consumer is
         * separately admitted below. It never synthesizes recording consent. */
        view->microphone.can_unmute = editable && audio->microphone_enabled && job_active(&audio->capture[AE_CAPTURE_LIVE]);
        view->speaker.can_unmute = editable && audio->speaker_enabled && job_active(&audio->playback);
        const ae_route_capabilities_t *route = (unsigned)audio->route < AE_ROUTE_COUNT ? &controls.routes[audio->route] : NULL;
        view->microphone.can_enable = editable && !any_jobs() && route && route->available && route->connected && route->capture;
        view->speaker.can_enable = editable && !any_jobs() && route && route->available && route->connected && route->playback;
        /* Live routes have an actual host sink/source. Local capture belongs
         * to separately bounded recording and recognition flows. */
        view->microphone.start_block = live_start_block(AE_MICROPHONE, false);
        view->speaker.start_block = live_start_block(AE_SPEAKER, false);
        view->microphone.can_start = view->microphone.start_block == APP_MEDIA_START_ALLOWED;
        view->speaker.can_start = view->speaker.start_block == APP_MEDIA_START_ALLOWED;
        view->volume_known = view->gain_known = true;
        view->volume_percent = audio->volume_percent; view->microphone_gain_db = audio->microphone_gain_db;
        view->safe_volume_max = controls.codec.safe_volume_max;
        view->can_set_volume = editable && controls.codec.available;
        view->can_set_gain = editable && controls.codec.available && !any_jobs();
        if (audio->error != AE_OK) snprintf(view->reason, sizeof(view->reason), "%s", result_text(audio->error));
        map_job(&view->recording, &audio->capture[AE_CAPTURE_RECORDING]); map_job(&view->playback, &audio->playback);
        map_job(&view->live_capture, &audio->capture[AE_CAPTURE_LIVE]);
    }
    for (unsigned i = 1; i < AE_ROUTE_COUNT; ++i) {
        const ae_route_capabilities_t *route = &controls.routes[i];
        view->routes[i] = (app_media_route_view_t){.available = route->implemented,
            .connected = route->connected, .can_select = idle_controls() && !any_jobs() && route->implemented &&
                controls.runtime.audio.hardware_off_confirmed};
        snprintf(view->routes[i].reason, sizeof(view->routes[i].reason), "%.64s", result_text(route->reason));
    }
    if (view->known) {
        if (audio->route == AE_ROUTE_USB_UAC) {
            view->connection_known = true;
            view->connection = APP_MEDIA_CONNECTION_USB;
        } else if (audio->route == AE_ROUTE_HFP_HF || audio->route == AE_ROUTE_A2DP_SINK) {
            view->connection_known = true;
            view->connection = APP_MEDIA_CONNECTION_BLUETOOTH;
        }
        for (unsigned i = 0; i < APP_MEDIA_CONNECTION_COUNT; ++i) {
            ae_route_t route = connection_route((app_media_connection_t)i);
            view->can_select_connection[i] = route != AE_ROUTE_OFF && route != audio->route &&
                view->routes[route].can_select && !audio->quarantined;
        }
    }
    if (view->route == APP_MEDIA_ROUTE_HFP || view->route == APP_MEDIA_ROUTE_A2DP) {
        bool hfp = view->route == APP_MEDIA_ROUTE_HFP;
        const bt_audio_snapshot_t *bt = &controls.bluetooth;
        bool connected = hfp ? bt->hfp_connected : bt->a2dp_connected;
        int selected = selected_bond();
        bool current_known = hfp ? bt->hfp_peer_known : bt->a2dp_peer_known;
        bool known_peer = connected ? current_known : selected >= 0;
        const uint8_t *peer = connected ? (hfp ? bt->hfp_peer : bt->a2dp_peer) : controls.selected_peer;
        if (known_peer) {
            /* Same opaque Classic peer namespace as the transport owner's
             * token. MAC bytes are copied from its cache, never a HID label. */
            view->peer_id = peer_id(peer);
            snprintf(view->peer_label, sizeof(view->peer_label), "%02X:%02X:%02X:%02X:%02X:%02X", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
        }
        app_media_route_view_t *route = &view->routes[view->route];
        route->connected = connected; /* Profile link, not negotiated voice readiness. */
        bool link_idle = idle_controls() && !any_jobs() && bt->initialized && !bt->failed && !bt->control_inflight &&
            !bt->pairing_busy && !bt->pairing_requested && !bt->pairing_handshake_pending &&
            bt->request_status != BT_AUDIO_JOB_PENDING && bt->request_status != BT_AUDIO_JOB_WAITING;
        route->can_connect = link_idle && (hfp && connected ? current_known && saved_bond(peer) >= 0 && !bt->hfp_audio_connected :
            selected >= 0 && !current_known && !connected);
        route->can_disconnect = link_idle && connected && known_peer;
        bool catalog = controls.bonds.known && !controls.bonds.refreshing && controls.bonds.error == ESP_OK &&
            controls.bonds.count > 0 && controls.bonds.count <= BT_AUDIO_CLASSIC_BONDS_MAX;
        if (catalog) {
            unsigned next = selected < 0 ? 0U : ((unsigned)selected + 1U) % controls.bonds.count;
            view->next_peer_id = peer_id(controls.bonds.bonds[next].address);
            view->can_select_peer = link_idle && !current_known && !connected &&
                (selected < 0 || controls.bonds.count > 1);
        }
    }
    rebuild_devices();
    map_storage(); map_options();
    if (controls.selected_known) map_record(&view->selected_recording, &controls.selected);
    if (controls.list_loaded) {
        view->recordings.known = true; view->recordings.row_count = controls.record_count;
        view->recordings.pending = controls.pending && controls.pending_action == APP_MEDIA_QUERY_LIST;
        view->recordings.page = controls.page; view->recordings.has_previous = controls.page > 0;
        view->recordings.has_next = controls.next_cursor != 0 && controls.page + 1 < CURSOR_HISTORY;
        for (size_t i = 0; i < controls.record_count; ++i) map_record(&view->recordings.rows[i], &controls.records[i]);
    }
    const sl_view_t *speech = &controls.runtime.speech;
    app_media_speech_view_t *spoken = &view->speech;
    spoken->generation = speech->token ? speech->token : 1; spoken->revision = controls.speech_revision;
    spoken->state = !controls.runtime.speech_known || speech->state == SL_UNAVAILABLE ? APP_SPEECH_UNAVAILABLE :
        speech->state == SL_LISTENING ? APP_SPEECH_LISTENING : speech->state == SL_CANDIDATE ? APP_SPEECH_CANDIDATE :
        speech->state == SL_FAULT ? APP_SPEECH_ERROR : APP_SPEECH_IDLE;
    spoken->window_seconds = SPEECH_SECONDS;
    const ae_job_snapshot_t *speech_job = &audio->capture[AE_CAPTURE_RECOGNITION];
    if (spoken->state == APP_SPEECH_LISTENING && speech_job->deadline_ms && controls.now_ms <= speech_job->deadline_ms) {
        uint64_t remaining = (speech_job->deadline_ms - controls.now_ms + 999U) / 1000U;
        spoken->remaining_known = remaining <= SPEECH_SECONDS;
        spoken->remaining_seconds = spoken->remaining_known ? (uint32_t)remaining : 0;
    }
    snprintf(spoken->profile, sizeof(spoken->profile), "Alpha-Zulu / zero-nine");
    spoken->can_listen = idle_controls() && !any_jobs() && spoken->state == APP_SPEECH_IDLE &&
        controls.codec.available && controls.routes[AE_ROUTE_LOCAL].available && speech_context();
    spoken->can_stop = idle_controls() && job_active(&audio->capture[AE_CAPTURE_RECOGNITION]);
    spoken->candidate = spoken->state == APP_SPEECH_CANDIDATE ? speech->candidate : 0;
    if (spoken->candidate && speech->token == controls.consumed_speech_token) {
        spoken->candidate = 0;
        spoken->state = APP_SPEECH_STOPPING;
    }
    spoken->candidate_id = spoken->candidate ? controls.speech_revision : 0;
    spoken->can_reject = local_ready() && speech_context() && spoken->candidate && !speech->cleanup_pending;
    spoken->can_confirm = spoken->can_reject && controls.context.preview_characters < APP_UI_DRAFT_CAPACITY &&
        speech->epoch == controls.context.epoch && speech->ui_revision == controls.context.ui_revision;
    if (speech->result != AE_OK) snprintf(spoken->reason, sizeof(spoken->reason), "%s", result_text(speech->result));
    view->speaker_waiting = controls.resume.desired.speaker && !job_active(&audio->playback);
    view->resume_notice = controls.resume.notice;
    if (view->speaker_waiting && !view->resume_notice) {
        bool music = controls.resume.desired.speaker == AE_ROUTE_A2DP_SINK;
        bool connected = music ? controls.bluetooth.a2dp_connected : controls.bluetooth.hfp_connected;
        const uint8_t *peer = music ? controls.bluetooth.a2dp_peer : controls.bluetooth.hfp_peer;
        if (!connected || memcmp(peer, controls.resume.desired.peer, 6))
            view->resume_notice = "Waiting for saved audio host";
    }
    rebuild_audio_pairing();
    bool host_preparing = view->microphone.start_block == APP_MEDIA_START_HOST_PREPARING ||
        view->speaker.start_block == APP_MEDIA_START_HOST_PREPARING;
    bool primary_audio = controls.context.mode == INPUT_AUDIO &&
        (controls.context.overlay == APP_OVERLAY_NONE || controls.context.overlay == APP_OVERLAY_AUDIO);
    /* Keep old command outcomes and terminal jobs, but do not let a retained
     * command message mask current host preparation. Current faults win. */
    if (*controls.message && !(primary_audio && host_preparing))
        snprintf(view->reason, sizeof(view->reason), "%s", controls.message);
    if (controls.review.valid && !valid_identity(&controls.review.request)) controls.review.valid = false;
}

bool app_media_controls_init(void)
{
    if (controls.initialized) return false;
    controls = (controls_t){.initialized = true, .revision = 1, .store_revision = 1, .speech_revision = 1, .duration = 5,
        .next_cursor = 0};
    return true;
}
void app_media_controls_review_opened(app_confirmation_t confirmation, uint64_t epoch)
{
    /* The coordinator has just completed this review's own input release. */
    controls.confirmation_epoch = epoch;
    controls.confirmation = controls.review.valid || controls.prepare_review ? confirmation : APP_CONFIRM_NONE;
}
void app_media_controls_revoke_devices(void)
{
    if (controls.device_request_id) {
        bt_audio_cancel_request(controls.device_request_id); controls.device_request_id = 0;
    }
    if (controls.voice_link.active && controls.voice_link.device_management) finish_voice_link(NULL, AE_PRIVACY);
}

void app_media_controls_context(const app_media_controls_context_t *context)
{
    if (!context || !controls.initialized) return;
    if (context->locked || context->local_access != APP_LOCAL_NORMAL) app_media_controls_pause_boot();
    if (controls.device_request_id && (context->locked || context->screen_off || context->local_access != APP_LOCAL_NORMAL ||
        !context->bluetooth_audio_devices || context->epoch != controls.context.epoch)) {
        app_media_controls_revoke_devices();
    }
    if (controls.audio_pairing_owned && (context->locked || context->screen_off || (!context->bluetooth_audio_devices && (context->mode != INPUT_AUDIO || controls.context.bluetooth_audio_devices)))) {
        bt_audio_pairing_close(); controls.audio_pairing_owned = false;
    }
    if (controls.voice_link.active && (context->epoch != controls.voice_link.epoch || context->locked ||
        context->screen_off || (controls.voice_link.device_management ? !context->bluetooth_audio_devices :
        context->mode != INPUT_AUDIO))) finish_voice_link(NULL, AE_PRIVACY);
    if (context->overlay != APP_OVERLAY_CONFIRM) controls.confirmation = APP_CONFIRM_NONE;
    if (controls.confirmation == APP_CONFIRM_NONE || context->epoch != controls.confirmation_epoch ||
        context->locked || context->screen_off || (context->mode != INPUT_AUDIO && !context->bluetooth_audio_devices)) {
        controls.review.valid = false; controls.prepare_review = false;
    }
    if (context->overlay == APP_OVERLAY_RECORDINGS && controls.context.overlay != APP_OVERLAY_RECORDINGS) controls.list_requested = true;
    controls.context = *context;
}
static ae_result_t submit_runtime(app_media_local_request_t request, bool resume)
{
    if (!idle_for(resume)) return AE_BUSY;
    if (controls.serial == UINT64_MAX) return AE_EXHAUSTED;
    request.token = ++controls.serial;
    request.epoch = controls.context.epoch; request.ui_revision = controls.context.ui_revision;
    request.expected_audio_generation = controls.runtime.audio.audio_generation;
    request.expected_audio_revision = controls.runtime.audio.revision;
    ae_result_t result = app_media_runtime_submit_local(&request);
    if (result == AE_OK) {
        controls.pending = true; controls.pending_token = request.token; controls.pending_action = request.action;
        if (request.action == APP_MEDIA_ROUTE) {
            controls.resume.route_pending = false;
            controls.resume.submitted_route = (uint8_t)request.route;
        }
        controls.resume.pending_playback = request.action == APP_MEDIA_START && request.kind == AE_PLAYBACK_STREAM;
        if (controls.resume.pending_playback) {
            controls.resume.submitted_speaker = (uint8_t)request.route;
            controls.resume.submitted_token = request.expected_route_token;
        }
        increase(&controls.revision);
    }
    return result;
}
static void open_panel(app_media_controls_effect_t *effect, app_overlay_t overlay, app_confirmation_t confirmation)
{
    effect->handled = effect->open_overlay = true; effect->overlay = overlay; effect->confirmation = confirmation;
}

static const ae_format_t *selected_format(void)
{
    for (size_t i = 0; i < controls.view.recording_options.profile_count; ++i)
        if (controls.view.recording_options.profiles[i].id == controls.profile_id) return &controls.profiles[i];
    return NULL;
}
static ae_result_t submit_start(ae_job_kind_t kind, const ae_record_metadata_t *record, bool resume)
{
    const ae_format_t *format = selected_format();
    ae_route_t route = kind == AE_CAPTURE_RECORDING || kind == AE_CAPTURE_RECOGNITION || kind == AE_PLAYBACK_RECORDING ?
        AE_ROUTE_LOCAL : live_route(kind == AE_CAPTURE_LIVE ? AE_MICROPHONE : AE_SPEAKER);
    if (kind == AE_CAPTURE_RECOGNITION) {
        static const ae_format_t speech_format = {16000, 1, 10, AE_PCM_S16_LE};
        format = &speech_format;
    } else if (record) format = &record->format;
    else if (route != AE_ROUTE_LOCAL && (unsigned)route < AE_ROUTE_COUNT) format = &controls.routes[route].format;
    if (!format) return AE_UNAVAILABLE;
    uint32_t duration = kind == AE_CAPTURE_RECORDING ? controls.view.recording_options.duration_seconds :
        kind == AE_CAPTURE_RECOGNITION ? SPEECH_SECONDS : LIVE_SECONDS;
    uint64_t byte_limit = (uint64_t)format->sample_rate_hz * format->channels * 2U * duration;
    if (kind == AE_CAPTURE_RECORDING) byte_limit = controls.view.recording_options.byte_limit;
    if (record) {
        if (!record->format.sample_rate_hz || !record->sample_frames || record->sample_frames / record->format.sample_rate_hz >= UINT32_MAX) return AE_INVALID;
        duration = (uint32_t)(record->sample_frames / record->format.sample_rate_hz) + 1U;
        byte_limit = record->pcm_bytes;
    }
    app_media_local_request_t request = {.action = APP_MEDIA_START, .kind = kind, .route = route, .format = *format,
        .duration_seconds = duration, .byte_limit = byte_limit, .retention_seconds = kind == AE_CAPTURE_RECORDING ? LOCAL_RETENTION_SECONDS : 0,
        .storage = kind == AE_CAPTURE_RECORDING ? AE_STORAGE_RECORDS_PARTITION : AE_STORAGE_NONE,
        .export_policy = AE_EXPORT_NONE};
    if (route != AE_ROUTE_LOCAL) request.expected_route_token = controls.routes[route].token;
    if (record) request.recording_id = record->object_id;
    return submit_runtime(request, resume);
}
#include "app_media_resume.inc"

static bool object_request_valid(const app_media_request_t *request, const app_media_record_view_t *record)
{
    return record->valid && same_ui_id(request->object_id, record->id) && request->object_revision == record->revision;
}
static bool job_request_valid(const app_media_request_t *request, const app_media_job_view_t *job)
{
    return job->known && same_ui_id(request->job_id, job->id) && request->consumer_generation == job->consumer_generation;
}
static bool allowed_panel(app_media_operation_t operation)
{
    app_overlay_t panel = controls.context.overlay;
    if (operation >= APP_MEDIA_SPEECH_START && operation <= APP_MEDIA_SPEECH_CONFIRM) return speech_context();
    if (controls.context.bluetooth_audio_devices &&
        (panel == APP_OVERLAY_BLUETOOTH_DEVICES || panel == APP_OVERLAY_AUDIO_DEVICE))
        return operation == APP_MEDIA_PAIR_AUDIO_OPEN || operation == APP_MEDIA_PAIR_AUDIO_CLOSE ||
            operation == APP_MEDIA_DEVICE_DETAILS || operation == APP_MEDIA_DEVICE_PAGE || operation == APP_MEDIA_DEVICE_LINK ||
            operation == APP_MEDIA_DEVICE_REFRESH;
    if (controls.context.mode != INPUT_AUDIO) return false;
    if (operation == APP_MEDIA_LIVE_START || operation == APP_MEDIA_LIVE_STOP) return panel == APP_OVERLAY_NONE;
    if (operation == APP_MEDIA_SELECT_CONNECTION) return panel == APP_OVERLAY_NONE || panel == APP_OVERLAY_MEDIA_OPTIONS;
    if (operation == APP_MEDIA_PAIR_AUDIO_OPEN || operation == APP_MEDIA_PAIR_AUDIO_CLOSE)
        return panel == APP_OVERLAY_MEDIA_OPTIONS;
    if (operation == APP_MEDIA_SELECT_PEER) return panel == APP_OVERLAY_MEDIA_OPTIONS;
    if (operation <= APP_MEDIA_SET_GAIN) return panel == APP_OVERLAY_MEDIA_OPTIONS || panel == APP_OVERLAY_NONE;
    switch (operation) {
    case APP_MEDIA_RECORD_SET_PROFILE: case APP_MEDIA_RECORD_SET_DURATION:
    case APP_MEDIA_RECORD_START_REQUEST: case APP_MEDIA_RECORD_STOP:
    case APP_MEDIA_RECORD_CANCEL_KEEP: case APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST:
        return panel == APP_OVERLAY_RECORDING_START;
    case APP_MEDIA_RECORD_SELECT: case APP_MEDIA_RECORD_LIST_PAGE:
        return panel == APP_OVERLAY_RECORDINGS;
    case APP_MEDIA_RECORD_PLAY: case APP_MEDIA_RECORD_DELETE_REQUEST: case APP_MEDIA_PLAYBACK_STOP:
        return panel == APP_OVERLAY_RECORDING_DETAIL;
    default: return false;
    }
}
static ae_result_t request_list(uint32_t page)
{
    if (page >= CURSOR_HISTORY) return AE_INVALID;
    ae_result_t result = submit_runtime((app_media_local_request_t){.action = APP_MEDIA_QUERY_LIST,
        .capacity = APP_MEDIA_ROWS, .cursor = controls.cursors[page]}, false);
    if (result == AE_OK) { controls.page = page; controls.view.recordings.pending = true; }
    return result;
}
static ae_result_t stop_job(const app_media_request_t *request, ae_stop_reason_t reason)
{
    return submit_runtime((app_media_local_request_t){.action = APP_MEDIA_STOP_JOB,
        .job_id = engine_id(request->job_id), .consumer_generation = request->consumer_generation, .stop_reason = reason}, false);
}
static bool pairing_resources_current(void)
{
    return app_media_runtime_pairing_current(controls.runtime.audio.audio_generation, controls.runtime.audio.revision);
}
static ae_result_t confirm_review(const app_intent_t *intent)
{
    if (controls.confirmation == APP_CONFIRM_RECORDING_STORAGE_PREPARE) {
        bool valid = controls.prepare_review && controls.storage_view.can_prepare &&
            intent->recording_storage_generation == controls.prepare_generation &&
            intent->recording_storage_revision == controls.prepare_revision &&
            controls.prepare_revision == controls.storage_view.revision;
        controls.prepare_review = false;
        if (!valid) return AE_STALE;
        uint64_t reserve = controls.physical.capacity_bytes / 10U;
        if (reserve < 32768U) reserve = 32768U;
        uint64_t minimum = reserve + (2U + 2U * RECORD_OBJECT_LIMIT) * 4096U;
        if (controls.physical.capacity_bytes <= minimum) return AE_QUOTA;
        uint64_t quota = controls.physical.capacity_bytes - reserve;
        if (quota > RECORD_QUOTA_MAX) quota = RECORD_QUOTA_MAX;
        quota -= quota % 4096U;
        return submit_runtime((app_media_local_request_t){.action = APP_MEDIA_PREPARE_STORE,
            .object_limit = RECORD_OBJECT_LIMIT, .quota_bytes = quota}, false);
    }
    app_media_review_t review = controls.review;
    controls.review.valid = false;
    if (!review.valid || !valid_identity(&intent->media) ||
        !same_ui_id(intent->media.peer_id, review.request.peer_id) || intent->media.operation != review.request.operation ||
        intent->media.management_generation != review.request.management_generation ||
        intent->media.audio_generation != review.request.audio_generation ||
        intent->media.resource_revision != review.request.resource_revision ||
        !same_ui_id(intent->media.object_id, review.request.object_id) ||
        intent->media.object_revision != review.request.object_revision ||
        !same_ui_id(intent->media.job_id, review.request.job_id) ||
        intent->media.consumer_generation != review.request.consumer_generation) return AE_STALE;
    switch (review.request.operation) {
    case APP_MEDIA_PAIR_AUDIO_OPEN:
        if (!controls.view.pairing.can_open) return AE_STALE;
        esp_err_t opened = bt_audio_pairing_open_reviewed(BT_AUDIO_PAIRING_DEFAULT_MS, &controls.bluetooth,
            controls.bonds.revision, pairing_resources_current);
        if (opened == ESP_ERR_INVALID_VERSION) return AE_STALE;
        controls.audio_pairing_request_error = opened;
        controls.bluetooth = bt_audio_snapshot();
        controls.audio_pairing_owned = controls.audio_pairing_request_error == ESP_OK;
        increase(&controls.revision);
        return controls.audio_pairing_owned ? AE_OK : AE_UNAVAILABLE;
    case APP_MEDIA_PAIR_AUDIO_CLOSE:
        if (!controls.view.pairing.can_close) return AE_STALE;
        bt_audio_pairing_close(); controls.audio_pairing_owned = false;
        controls.audio_pairing_request_error = ESP_OK; increase(&controls.revision); return AE_OK;
    case APP_MEDIA_RECORD_START_REQUEST:
        if (!controls.view.recording_options.can_start || review.options.profile_id != controls.profile_id ||
            review.options.duration_seconds != controls.view.recording_options.duration_seconds ||
            review.options.byte_limit != controls.view.recording_options.byte_limit) return AE_STALE;
        return submit_start(AE_CAPTURE_RECORDING, NULL, false);
    case APP_MEDIA_RECORD_DELETE_REQUEST:
        if (!object_request_valid(&review.request, &controls.view.selected_recording) || !controls.view.selected_recording.can_delete) return AE_STALE;
        return submit_runtime((app_media_local_request_t){.action = APP_MEDIA_QUERY_DELETE,
            .recording_id = engine_id(review.request.object_id), .object_revision = review.request.object_revision}, false);
    case APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST:
        if (!job_request_valid(&review.request, &controls.view.recording) || !controls.view.recording.can_cancel_discard) return AE_STALE;
        return stop_job(&review.request, AE_STOP_CANCEL_DISCARD);
    default: return AE_INVALID;
    }
}

bool app_media_controls_handle(const app_intent_t *intent, app_media_controls_effect_t *effect)
{
    if (!intent || !effect || !controls.initialized) return false;
    memset(effect, 0, sizeof(*effect));
    bool confirmation = intent->action == APP_CONFIRM && controls.confirmation != APP_CONFIRM_NONE;
    if (intent->action != APP_MEDIA_CONTROL && intent->action != APP_RECORDING_STORAGE_PREPARE_REQUEST && !confirmation) return false;
    effect->handled = true;
    bool navigation = intent->action == APP_MEDIA_CONTROL && app_media_device_navigation(intent->media.operation);
    bool available = navigation ? controls.context.local_access != APP_LOCAL_PAUSED &&
        !controls.context.locked && !controls.context.screen_off : local_ready();
    if (!available || intent->epoch != controls.context.epoch || intent->ui_revision != controls.context.ui_revision) {
        effect_result(effect, AE_STALE); return true;
    }
    if (confirmation) {
        if (controls.context.overlay != APP_OVERLAY_CONFIRM || (intent->x != 0 && intent->x != 1)) {
            effect_result(effect, AE_STALE); return true;
        }
        bool pairing = controls.confirmation == APP_CONFIRM_AUDIO_PAIR;
        app_overlay_t pairing_return = controls.context.bluetooth_audio_devices ? APP_OVERLAY_BLUETOOTH_DEVICES : APP_OVERLAY_MEDIA_OPTIONS;
        bool storage = controls.confirmation == APP_CONFIRM_RECORDING_STORAGE_PREPARE;
        if (!intent->x) {
            controls.review.valid = controls.prepare_review = false;
            open_panel(effect, storage ? APP_OVERLAY_RECORDING_STORAGE : pairing ? pairing_return : APP_OVERLAY_AUDIO, APP_CONFIRM_NONE);
            return true;
        }
        ae_result_t result = confirm_review(intent);
        effect_result(effect, result);
        if (storage || pairing) open_panel(effect, storage ? APP_OVERLAY_RECORDING_STORAGE : pairing_return, APP_CONFIRM_NONE);
        else if (result == AE_OK) open_panel(effect, controls.pending_action == APP_MEDIA_QUERY_DELETE ? APP_OVERLAY_RECORDING_DETAIL :
            APP_OVERLAY_RECORDING_START, APP_CONFIRM_NONE);
        rebuild(); return true;
    }
    if (intent->action == APP_RECORDING_STORAGE_PREPARE_REQUEST) {
        if (controls.context.mode != INPUT_AUDIO || controls.context.overlay != APP_OVERLAY_RECORDING_STORAGE ||
            !controls.storage_view.can_prepare || intent->recording_storage_generation != controls.storage_view.generation ||
            intent->recording_storage_revision != controls.storage_view.revision) { effect_result(effect, AE_STALE); return true; }
        controls.prepare_review = true; controls.prepare_generation = controls.storage_view.generation;
        controls.prepare_revision = controls.storage_view.revision;
        open_panel(effect, APP_OVERLAY_CONFIRM, APP_CONFIRM_RECORDING_STORAGE_PREPARE);
        return true;
    }
    const app_media_request_t *request = &intent->media;
    if ((!navigation && !valid_identity(request)) || !allowed_panel(request->operation)) { effect_result(effect, AE_STALE); return true; }
    ae_result_t result = AE_DENIED;
    switch (request->operation) {
    case APP_MEDIA_PAIR_AUDIO_OPEN: case APP_MEDIA_PAIR_AUDIO_CLOSE:
        if (request->operation == APP_MEDIA_PAIR_AUDIO_OPEN ? controls.view.pairing.can_open : controls.view.pairing.can_close) {
            controls.review = (app_media_review_t){.valid = true, .request = *request};
            open_panel(effect, APP_OVERLAY_CONFIRM, APP_CONFIRM_AUDIO_PAIR); result = AE_OK;
        }
        break;
    case APP_MEDIA_DEVICE_PAGE:
        if (request->object_revision == controls.bonds.revision && request->value.page_delta >= 0 &&
            (unsigned)request->value.page_delta < controls.view.devices.pages) {
            controls.device_page = (unsigned)request->value.page_delta; increase(&controls.revision); result = AE_OK;
        }
        break;
    case APP_MEDIA_DEVICE_DETAILS:
        if (request->object_revision == controls.bonds.revision &&
            cached_bond(request->value.peer_id.bytes + 10) >= 0 &&
            same_ui_id(request->value.peer_id, peer_id(request->value.peer_id.bytes + 10))) {
            memcpy(controls.selected_peer, request->value.peer_id.bytes + 10, 6); controls.selected_peer_known = true;
            increase(&controls.revision); open_panel(effect, APP_OVERLAY_AUDIO_DEVICE, APP_CONFIRM_NONE); result = AE_OK;
        }
        break;
    case APP_MEDIA_DEVICE_REFRESH:
        if (controls.view.devices.can_refresh) {
            result = bt_audio_refresh_bonds() == ESP_OK ? AE_AGAIN : AE_UNAVAILABLE;
            bt_audio_copy_bond_list(&controls.bonds);
        }
        break;
    case APP_MEDIA_DEVICE_LINK: {
        int action = request->value.page_delta;
        if (action < 0 || action > 3 || request->object_revision != controls.bonds.revision ||
            !same_ui_id(request->object_id, controls.view.devices.selected) || selected_bond() < 0) { result = AE_STALE; break; }
        unsigned profile = (unsigned)action / 2U; bool disconnect = (action & 1) != 0;
        if (!(disconnect ? controls.view.devices.can_disconnect[profile] : controls.view.devices.can_connect[profile])) { result = AE_BUSY; break; }
        resume_link_choice(controls.selected_peer, profile ? APP_RESUME_A2DP : APP_RESUME_HFP, !disconnect);
        if (!profile && !disconnect) { result = connect_voice(); break; }
        bt_audio_command_t command = profile ? (disconnect ? BT_AUDIO_DISCONNECT_A2DP : BT_AUDIO_CONNECT_A2DP) :
            (disconnect ? BT_AUDIO_DISCONNECT_HFP : BT_AUDIO_CONNECT_HFP);
        esp_err_t error = bt_audio_saved_request(controls.bluetooth.generation, profile ? controls.bluetooth.a2dp_link_generation :
            controls.bluetooth.hfp_link_generation, controls.bonds.revision, command, controls.selected_peer, &controls.device_request_id);
        controls.bluetooth = bt_audio_snapshot();
        increase(&controls.revision); result = error == ESP_OK ? AE_AGAIN : AE_UNAVAILABLE;
        break;
    }
    case APP_MEDIA_SELECT_CONNECTION:
        if ((unsigned)request->value.connection < APP_MEDIA_CONNECTION_COUNT &&
            controls.view.can_select_connection[request->value.connection]) {
            if (request->value.connection == APP_MEDIA_CONNECTION_USB && controls.audio_pairing_owned) {
                bt_audio_pairing_close(); controls.audio_pairing_owned = false;
            }
            ae_route_t route = connection_route(request->value.connection);
            result = submit_runtime((app_media_local_request_t){.action = APP_MEDIA_ROUTE, .route = route}, false);
        }
        break;
    case APP_MEDIA_SELECT_PEER:
        if (controls.view.can_select_peer && same_ui_id(request->value.peer_id, controls.view.next_peer_id)) {
            resume_select_peer(request->value.peer_id.bytes + 10);
            memcpy(controls.selected_peer, request->value.peer_id.bytes + 10, 6);
            controls.selected_peer_known = true; increase(&controls.revision); result = AE_OK;
        }
        break;
    case APP_MEDIA_LIVE_START: case APP_MEDIA_LIVE_STOP: {
        bool microphone = request->value.role.role == APP_MEDIA_MICROPHONE;
        if ((unsigned)request->value.role.role > APP_MEDIA_SPEAKER) break;
        const app_media_role_view_t *role = microphone ? &controls.view.microphone : &controls.view.speaker;
        const app_media_job_view_t *job = microphone ? &controls.view.live_capture : &controls.view.playback;
        if (request->operation == APP_MEDIA_LIVE_START && role->can_start)
            result = submit_start(microphone ? AE_CAPTURE_LIVE : AE_PLAYBACK_STREAM, NULL, false);
        else if (request->operation == APP_MEDIA_LIVE_STOP && job->can_stop && job_request_valid(request, job)) {
            if (!microphone) resume_stop_speaker();
            result = stop_job(request, AE_STOP_REQUESTED);
        } else if (!microphone && request->operation == APP_MEDIA_LIVE_STOP && controls.view.speaker_waiting) {
            resume_cancel_start(); resume_stop_speaker(); result = AE_OK;
        }
        break;
    }
    case APP_MEDIA_SELECT_ROUTE:
        if ((unsigned)request->value.route < APP_MEDIA_ROUTE_COUNT && controls.view.routes[request->value.route].can_select)
            result = submit_runtime((app_media_local_request_t){.action = APP_MEDIA_ROUTE, .route = (ae_route_t)request->value.route}, false);
        break;
    case APP_MEDIA_LINK_CONNECT: case APP_MEDIA_LINK_DISCONNECT: {
        app_media_route_t route = controls.view.route;
        if (route != APP_MEDIA_ROUTE_HFP && route != APP_MEDIA_ROUTE_A2DP) { result = AE_NOT_SUPPORTED; break; }
        bool connect = request->operation == APP_MEDIA_LINK_CONNECT;
        bool allowed = connect ? controls.view.routes[route].can_connect : controls.view.routes[route].can_disconnect;
        if (allowed) {
            resume_link_choice(connect ? controls.selected_peer : route == APP_MEDIA_ROUTE_HFP ?
                controls.bluetooth.hfp_peer : controls.bluetooth.a2dp_peer,
                route == APP_MEDIA_ROUTE_HFP ? APP_RESUME_HFP : APP_RESUME_A2DP, connect);
            if (connect && route == APP_MEDIA_ROUTE_HFP) { result = connect_voice(); break; }
            app_media_local_request_t command = {.action = APP_MEDIA_LINK_CONTROL, .route = (ae_route_t)route,
                .value = connect, .bluetooth_generation = controls.bluetooth.generation,
                .bluetooth_link_generation = route == APP_MEDIA_ROUTE_HFP ?
                    controls.bluetooth.hfp_link_generation : controls.bluetooth.a2dp_link_generation,
                .bluetooth_bond_revision = connect ? controls.bonds.revision : 0};
            memcpy(command.bluetooth_peer, connect ? controls.selected_peer :
                route == APP_MEDIA_ROUTE_HFP ? controls.bluetooth.hfp_peer : controls.bluetooth.a2dp_peer, 6);
            result = submit_runtime(command, false);
        }
        break;
    }
    case APP_MEDIA_SET_ENABLED: case APP_MEDIA_SET_MUTED: {
        if ((unsigned)request->value.role.role > APP_MEDIA_SPEAKER) break;
        const app_media_role_view_t *role = request->value.role.role == APP_MEDIA_MICROPHONE ? &controls.view.microphone : &controls.view.speaker;
        bool allowed = request->operation == APP_MEDIA_SET_ENABLED ?
            (request->value.role.value ? role->can_enable : role->can_disable) :
            (request->value.role.value ? role->can_mute : role->can_unmute);
        const ae_format_t *format = (unsigned)controls.runtime.audio.route < AE_ROUTE_COUNT && controls.runtime.audio.route != AE_ROUTE_LOCAL ?
            &controls.routes[controls.runtime.audio.route].format : selected_format();
        if (allowed && format) result = submit_runtime((app_media_local_request_t){
            .action = request->operation == APP_MEDIA_SET_ENABLED ? APP_MEDIA_ENABLE_ROLE : APP_MEDIA_MUTE_ROLE,
            .role = request->value.role.role == APP_MEDIA_MICROPHONE ? AE_MICROPHONE : AE_SPEAKER,
            .value = request->value.role.value, .format = *format}, false);
        break;
    }
    case APP_MEDIA_SET_VOLUME:
        if (controls.view.can_set_volume && request->value.level <= controls.view.safe_volume_max)
            result = submit_runtime((app_media_local_request_t){.action = APP_MEDIA_VOLUME, .level = request->value.level}, false);
        break;
    case APP_MEDIA_SET_GAIN:
        if (controls.view.can_set_gain && request->value.level <= 42 && request->value.level % 6 == 0)
            result = submit_runtime((app_media_local_request_t){.action = APP_MEDIA_GAIN, .level = request->value.level}, false);
        break;
    case APP_MEDIA_RECORD_SET_PROFILE:
        if (controls.view.recording_options.can_edit) {
            for (size_t i = 0; i < controls.view.recording_options.profile_count; ++i)
                if (controls.view.recording_options.profiles[i].id == request->value.profile_id && controls.view.recording_options.profiles[i].available) {
                    controls.profile_id = request->value.profile_id; increase(&controls.revision); result = AE_OK; break;
                }
        }
        break;
    case APP_MEDIA_RECORD_SET_DURATION:
        if (controls.view.recording_options.can_edit && request->value.duration_seconds >= controls.view.recording_options.minimum_duration_seconds &&
            request->value.duration_seconds <= controls.view.recording_options.maximum_duration_seconds) {
            controls.duration = request->value.duration_seconds; increase(&controls.revision); result = AE_OK;
        }
        break;
    case APP_MEDIA_RECORD_START_REQUEST: case APP_MEDIA_RECORD_DELETE_REQUEST: case APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST: {
        bool valid = request->operation == APP_MEDIA_RECORD_START_REQUEST ? controls.view.recording_options.can_start :
            request->operation == APP_MEDIA_RECORD_DELETE_REQUEST ? controls.view.selected_recording.can_delete && object_request_valid(request, &controls.view.selected_recording) :
            controls.view.recording.can_cancel_discard && job_request_valid(request, &controls.view.recording);
        if (valid) {
            controls.review = (app_media_review_t){.valid = true, .request = *request, .options = controls.view.recording_options,
                .recording = controls.view.selected_recording, .job = controls.view.recording};
            open_panel(effect, APP_OVERLAY_CONFIRM, request->operation == APP_MEDIA_RECORD_START_REQUEST ? APP_CONFIRM_RECORDING_START :
                request->operation == APP_MEDIA_RECORD_DELETE_REQUEST ? APP_CONFIRM_RECORDING_DELETE : APP_CONFIRM_RECORDING_CANCEL_DISCARD);
            result = AE_OK;
        }
        break;
    }
    case APP_MEDIA_RECORD_STOP: case APP_MEDIA_RECORD_CANCEL_KEEP:
        if (job_request_valid(request, &controls.view.recording) && (request->operation == APP_MEDIA_RECORD_STOP ?
            controls.view.recording.can_stop : controls.view.recording.can_cancel_keep))
            result = stop_job(request, request->operation == APP_MEDIA_RECORD_STOP ? AE_STOP_REQUESTED : AE_STOP_CANCEL_KEEP);
        break;
    case APP_MEDIA_RECORD_LIST_PAGE:
        if (request->list_generation != controls.view.recordings.generation) { result = AE_STALE; break; }
        if (request->value.page_delta == -1 && controls.view.recordings.has_previous) result = request_list(controls.page - 1);
        else if (request->value.page_delta == 1 && controls.view.recordings.has_next) {
            controls.cursors[controls.page + 1] = controls.next_cursor; result = request_list(controls.page + 1);
        }
        break;
    case APP_MEDIA_RECORD_SELECT:
        if (request->list_generation != controls.view.recordings.generation) { result = AE_STALE; break; }
        for (size_t i = 0; i < controls.view.recordings.row_count; ++i) {
            if (!object_request_valid(request, &controls.view.recordings.rows[i])) continue;
            result = submit_runtime((app_media_local_request_t){.action = APP_MEDIA_QUERY_GET, .recording_id = engine_id(request->object_id)}, false);
            if (result == AE_OK) {
                controls.selected_known = false; controls.selected_request = request->object_id;
                controls.selected_revision = request->object_revision;
                open_panel(effect, APP_OVERLAY_RECORDING_DETAIL, APP_CONFIRM_NONE);
            }
            break;
        }
        break;
    case APP_MEDIA_RECORD_PLAY:
        if (object_request_valid(request, &controls.view.selected_recording) && controls.view.selected_recording.can_play)
            result = submit_start(AE_PLAYBACK_RECORDING, &controls.selected, false);
        break;
    case APP_MEDIA_PLAYBACK_STOP:
        if (job_request_valid(request, &controls.view.playback) && controls.view.playback.can_stop) result = stop_job(request, AE_STOP_REQUESTED);
        break;
    case APP_MEDIA_SPEECH_START:
        if (request->speech_generation != controls.view.speech.generation || request->speech_revision != controls.view.speech.revision) { result = AE_STALE; break; }
        if (controls.view.speech.can_listen) result = submit_start(AE_CAPTURE_RECOGNITION, NULL, false);
        break;
    case APP_MEDIA_SPEECH_STOP: {
        const ae_job_snapshot_t *job = &controls.runtime.audio.capture[AE_CAPTURE_RECOGNITION];
        if (request->speech_generation != controls.view.speech.generation || request->speech_revision != controls.view.speech.revision) { result = AE_STALE; break; }
        if (controls.view.speech.can_stop) {
            app_media_request_t stop = {.job_id = ui_id(job->job_id), .consumer_generation = job->consumer_generation};
            result = stop_job(&stop, AE_STOP_REQUESTED);
        }
        break;
    }
    case APP_MEDIA_SPEECH_REJECT: case APP_MEDIA_SPEECH_CONFIRM:
        if (request->speech_generation != controls.view.speech.generation || request->speech_revision != controls.view.speech.revision ||
            request->candidate_id != controls.view.speech.candidate_id || request->value.candidate != controls.view.speech.candidate) { result = AE_STALE; break; }
        if (request->operation == APP_MEDIA_SPEECH_REJECT && controls.view.speech.can_reject)
            result = app_media_runtime_reject(controls.runtime.speech.token, controls.context.epoch, controls.context.ui_revision);
        else if (request->operation == APP_MEDIA_SPEECH_CONFIRM && controls.view.speech.can_confirm) {
            char candidate = 0;
            result = app_media_runtime_confirm(controls.runtime.speech.token, controls.context.epoch, controls.context.ui_revision, &candidate);
            if (result == AE_OK && candidate == request->value.candidate) effect->preview_character = candidate;
            else if (result == AE_OK) result = AE_INTEGRITY;
        }
        if (result == AE_OK) { controls.consumed_speech_token = controls.runtime.speech.token; increase(&controls.speech_revision); }
        break;
    default: result = AE_NOT_SUPPORTED; break;
    }
    effect_result(effect, result); rebuild(); return true;
}

void app_media_controls_poll(uint64_t now_ms, app_media_controls_effect_t *effect)
{
    if (effect) memset(effect, 0, sizeof(*effect));
    if (!controls.initialized) return;
    if (controls.clock_known && now_ms < controls.now_ms) {
        resume_cancel_link();
        controls.clock_failed = true; controls.review.valid = controls.prepare_review = false;
        controls.microphone_level = (app_media_mic_level_t){0}; controls.microphone_level_cached = false;
        if (controls.voice_link.active) finish_voice_link(effect, AE_TIME_UNKNOWN);
        effect_result(effect, AE_TIME_UNKNOWN); return;
    }
    controls.clock_known = true; controls.now_ms = now_ms;
    emit_message_clear(effect);
    memset(&poll_scratch, 0, sizeof(poll_scratch));
    app_media_snapshot_t *runtime = &poll_scratch.runtime;
    app_recording_store_snapshot_t *store = &poll_scratch.store;
    controls.runtime_known = app_media_runtime_snapshot(runtime);
    if (controls.runtime_known && runtime->audio_known) {
        if (runtime->audio.capture_indicator_required) {
            controls.capture_obligation = true;
            controls.capture_generation = runtime->audio.audio_generation;
        } else if (runtime->audio.hardware_off_confirmed) controls.capture_obligation = false;
    }
    controls.store_known = app_recording_store_snapshot(store);
    board_recording_flash_status_t physical = board_recording_flash_status();
    bool storage_changed = store->initialized != controls.store.initialized || store->preparing != controls.store.preparing ||
        store->error != controls.store.error || store->store.mounted != controls.store.store.mounted ||
        store->store.storage_uncertain != controls.store.store.storage_uncertain ||
        store->capabilities.quota_bytes != controls.store.capabilities.quota_bytes ||
        store->capabilities.used_bytes != controls.store.capabilities.used_bytes ||
        store->capabilities.reserved_bytes != controls.store.capabilities.reserved_bytes ||
        store->capabilities.retention_time_known != controls.store.capabilities.retention_time_known ||
        physical.state != controls.physical.state || physical.geometry_verified != controls.physical.geometry_verified;
    if (storage_changed) { increase(&controls.store_revision); increase(&controls.revision); }
    if (runtime->audio.audio_generation != controls.old_audio_generation || runtime->audio.revision != controls.old_audio_revision ||
        runtime->busy != controls.runtime.busy || runtime->audio_known != controls.runtime.audio_known) increase(&controls.revision);
    if (runtime->speech.token != controls.old_speech_token || runtime->speech.state != controls.old_speech_state || runtime->speech.candidate != controls.old_candidate) {
        increase(&controls.speech_revision);
        controls.old_speech_token = runtime->speech.token; controls.old_speech_state = runtime->speech.state; controls.old_candidate = runtime->speech.candidate;
    }
    controls.old_audio_generation = runtime->audio.audio_generation; controls.old_audio_revision = runtime->audio.revision;
    controls.runtime = *runtime; controls.store = *store; controls.physical = physical;
    ae_codec_port_t codec = board_audio_mosaico_port();
    ae_codec_capabilities_t caps = {0};
    if (!codec.capabilities || codec.capabilities(codec.context, &caps) != AE_OK) caps = (ae_codec_capabilities_t){0};
    if (!same_codec(&caps, &controls.codec)) increase(&controls.revision);
    controls.codec = caps;
    board_audio_capture_snapshot(&poll_scratch.capture_level);
    update_microphone_level(&poll_scratch.capture_level);
    bt_audio_snapshot_t bluetooth = bt_audio_snapshot();
    if (bluetooth.generation != controls.bluetooth.generation ||
        bluetooth.hfp_link_generation != controls.bluetooth.hfp_link_generation ||
        bluetooth.a2dp_link_generation != controls.bluetooth.a2dp_link_generation ||
        bluetooth.control_inflight != controls.bluetooth.control_inflight ||
        bluetooth.request_status != controls.bluetooth.request_status) increase(&controls.revision);
    controls.bluetooth = bluetooth;
    bt_audio_bond_list_t *bonds = &poll_scratch.bonds;
    bt_audio_copy_bond_list(bonds);
    if (bonds->revision != controls.bonds.revision || bonds->known != controls.bonds.known ||
        bonds->refreshing != controls.bonds.refreshing || bonds->error != controls.bonds.error) increase(&controls.revision);
    controls.bonds = *bonds;
    poll_voice_link(effect);
    for (unsigned i = 1; i < AE_ROUTE_COUNT; ++i) {
        ae_route_capabilities_t route = {0};
        ae_result_t result = app_media_runtime_route((ae_route_t)i, &route);
        if (result != AE_OK) route = (ae_route_capabilities_t){.reason = result};
        if (i == AE_ROUTE_USB_UAC) observe_usb_start_readiness(&route);
        if (!same_route(&route, &controls.routes[i])) increase(&controls.revision);
        controls.routes[i] = route;
    }
    if (controls.pending && runtime->result_ready && runtime->request_token == controls.pending_token) {
        ae_result_t result = runtime->result;
        if (result == AE_OK && controls.pending_action == APP_MEDIA_QUERY_LIST) {
            if (runtime->result_detail.count > APP_MEDIA_ROWS) result = AE_INTEGRITY;
            else {
                ae_record_metadata_t *records = poll_scratch.records;
                result = app_media_runtime_copy_records(controls.pending_token, records, APP_MEDIA_ROWS);
                if (result == AE_OK) {
                    memcpy(controls.records, records, sizeof(controls.records)); controls.record_count = runtime->result_detail.count;
                    controls.next_cursor = runtime->result_detail.next_cursor; controls.list_loaded = true;
                    increase(&controls.view.recordings.generation);
                }
            }
        } else if (result == AE_OK && controls.pending_action == APP_MEDIA_QUERY_GET) {
            const ae_record_metadata_t *record = &runtime->result_detail.metadata;
            if (!same_ui_id(ui_id(record->object_id), controls.selected_request) || record->revision != controls.selected_revision) result = AE_STALE;
            else { controls.selected = *record; controls.selected_known = true; }
        } else if (result == AE_OK && controls.pending_action == APP_MEDIA_QUERY_DELETE) {
            controls.selected.access_revoked = runtime->result_detail.access_revoked;
            controls.selected.state = AE_RECORD_DELETING;
            if (runtime->result_detail.cleanup_complete) controls.selected_known = false;
            controls.list_loaded = false;
            controls.view.recordings.known = false;
            controls.view.recordings.row_count = 0;
            increase(&controls.view.recordings.generation);
        }
        uint64_t token; ae_result_t taken;
        if (!app_media_runtime_take_result(&token, &taken) || token != controls.pending_token || taken != runtime->result) result = AE_UNCERTAIN;
        resume_result(result, &runtime->result_detail.job);
        controls.pending = false; controls.runtime.result_ready = false;
        controls.view.recordings.pending = false;
        if (result != AE_OK && controls.pending_action == APP_MEDIA_QUERY_LIST) {
            controls.list_loaded = false; controls.view.recordings.known = false;
            snprintf(controls.view.recordings.reason, sizeof(controls.view.recordings.reason), "%s", result_text(result));
        }
        increase(&controls.revision); effect_result(effect, result);
    }
    rebuild();
    poll_resume();
    if (controls.list_requested && controls.context.overlay == APP_OVERLAY_RECORDINGS && idle_controls()) {
        ae_result_t result = request_list(controls.page);
        controls.list_requested = false;
        if (result != AE_OK) effect_result(effect, result);
        rebuild();
    }
    clear_poll_scratch();
}

void app_media_controls_fill(app_view_t *view)
{
    if (!view || !controls.initialized) return;
    rebuild();
    view->media = controls.view; view->media_review = controls.review; view->recording_storage = controls.storage_view;
    const ae_snapshot_t *audio = &controls.runtime.audio;
    bool known = controls.runtime_known && controls.runtime.audio_known;
    view->audio_available = known && controls.codec.available;
    view->audio_active = known && (audio->capture_gate_open || audio->playback_gate_open);
    view->microphone_muted = known && audio->microphone_mute_applied && audio->microphone_mute_confirmed;
    view->volume = known ? audio->volume_percent : 0;
    view->recognition_available = controls.runtime.speech_known && controls.runtime.speech.state != SL_UNAVAILABLE;
    view->listening = controls.runtime.speech_known && controls.runtime.speech.state == SL_LISTENING;
    view->voice_candidate = controls.view.speech.candidate;
    view->capture_indicator_required = controls.capture_obligation;
    view->capture_indicator_generation = controls.capture_generation;
    view->capture_state = !known ? (controls.capture_obligation ? APP_CAPTURE_UNCERTAIN : APP_CAPTURE_UNKNOWN) : audio->quarantined || audio->state == AE_FAULT ? APP_CAPTURE_UNCERTAIN :
        audio->state == AE_STOPPING ? APP_CAPTURE_STOPPING : audio->capture_gate_open ? APP_CAPTURE_ACTIVE :
        audio->capture_indicator_required ? APP_CAPTURE_STARTING : audio->hardware_off_confirmed ? APP_CAPTURE_OFF_CONFIRMED : APP_CAPTURE_UNKNOWN;
}
