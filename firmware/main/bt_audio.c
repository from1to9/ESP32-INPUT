// SPDX-License-Identifier: Apache-2.0
#include "bt_audio.h"

#include <limits.h>
#include <string.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED
#include "esp_gap_bt_api.h"
#endif

#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_A2DP_ENABLE && \
    CONFIG_BT_HFP_ENABLE && CONFIG_BT_HFP_CLIENT_ENABLE && CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI && \
    !CONFIG_BT_A2DP_USE_EXTERNAL_CODEC && !CONFIG_BT_HFP_USE_EXTERNAL_CODEC && \
    ESP_BT_GAP_JUST_WORKS_CONFIRM_REQUIRED
#define AUDIO_PROFILES_SUPPORTED 1
#include "esp_a2dp_api.h"
#include "esp_hf_client_api.h"
#include "bt_bond_guard.h"
#include "ble_hid.h"
#else
#define AUDIO_PROFILES_SUPPORTED 0
#endif

#define A2DP_SLOTS 8U
#define VOICE_SLOTS 4U
#define SLOT_COUNT (A2DP_SLOTS + 2U * VOICE_SLOTS)
#define CALLBACK_BYTES_MAX 8192U
#define WATCHDOG_MS 20U
#define PAIRING_HANDSHAKE_US UINT64_C(30000000)

typedef struct {
    bt_audio_pcm_t *slots;
    uint16_t capacity, head, count, offset, frames;
} pcm_ring_t;

static portMUX_TYPE audio_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bt_audio_snapshot_t public;
    bool started, submitted, init_pending;
    uint64_t init_started_us;
    bt_audio_pcm_t *memory;
    pcm_ring_t music, voice, microphone;
    bool microphone_kick;
    bool hfp_rfcomm;
    uint16_t a2dp_handle;
    bt_audio_command_t command;
    uint8_t command_peer[6];
    bool command_pending;
    bool command_requires_saved;
    uint64_t command_bond_revision;
    uint64_t command_started_us;
    uint64_t hfp_link_generation, a2dp_link_generation;
    ae_id_t boot_id;
    uint8_t classic_bonds[BT_AUDIO_CLASSIC_BONDS_MAX][6];
    bool bond_refresh_pending, pairing_claimed, scan_pending, scan_submitted;
    bool scan_expected, security_pending;
    unsigned int security_kind; /* 1 confirmation, 2 legacy PIN, 3 passkey. */
    uint8_t security_peer[6], handshake_peer[6];
    uint64_t bond_revision, pairing_deadline_us, scan_started_us, handshake_started_us;
    uint64_t classic_list_revision;
    esp_err_t classic_list_error;
    struct {
        bool prepared, stopped, tx_seen;
        ae_route_t route;
        ae_format_t format;
        ae_route_token_t token;
        uint8_t roles;
        uint64_t generation, transport_generation, sequence, sample_offset, tx_sequence;
        uint64_t playback_consumer, playback_max_consumer, playback_epoch;
        bool playback_enabled;
        uint64_t partial_started_us;
        uint16_t partial_frames;
        int16_t partial[BT_AUDIO_PCM_FRAMES * 2U];
    } adapter;
} audio = {.public = {.supported = AUDIO_PROFILES_SUPPORTED,
    .microphone_muted = true, .speaker_muted = true, .generation = 1,
    .first_failure_stage = "none"}};

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
static void count_one(uint32_t *value) { if (*value != UINT32_MAX) ++*value; }

static void clear_ring(pcm_ring_t *ring)
{
    if (ring->slots) memset(ring->slots, 0, (size_t)ring->capacity * sizeof(*ring->slots));
    ring->head = ring->count = ring->offset = ring->frames = 0;
}

static void invalidate_locked(bool reset_policy)
{
    if (audio.public.generation == UINT64_MAX) {
        audio.public.failed = true;
        reset_policy = true;
        if (audio.public.error == ESP_OK) {
            audio.public.error = ESP_ERR_INVALID_STATE;
            audio.public.first_failure_stage = "Audio generation exhausted";
            audio.public.first_failure_us = now_us();
        }
    } else ++audio.public.generation;
    clear_ring(&audio.music); clear_ring(&audio.voice); clear_ring(&audio.microphone);
    audio.microphone_kick = false;
    audio.adapter.partial_frames = 0;
    audio.adapter.playback_enabled = false;
    memset(audio.adapter.partial, 0, sizeof(audio.adapter.partial));
    if (reset_policy) {
        audio.public.route = BT_AUDIO_OFF;
        audio.public.microphone_muted = true;
        audio.public.speaker_muted = true;
    }
}

static void refresh_locked(void)
{
    audio.public.initialized = audio.submitted && audio.public.hfp_initialized &&
        audio.public.a2dp_initialized && !audio.public.failed;
    audio.public.classic_hfp_peer_bonded = audio.public.classic_a2dp_peer_bonded = false;
    if (audio.public.classic_bonds_known) {
        for (unsigned int i = 0; i < audio.public.classic_bond_count; ++i) {
            audio.public.classic_hfp_peer_bonded |= audio.public.hfp_peer_known &&
                memcmp(audio.classic_bonds[i], audio.public.hfp_peer, 6) == 0;
            audio.public.classic_a2dp_peer_bonded |= audio.public.a2dp_peer_known &&
                memcmp(audio.classic_bonds[i], audio.public.a2dp_peer, 6) == 0;
        }
    }
    bool voice = audio.public.hfp_connected && audio.public.hfp_audio_connected && audio.public.hfp_format_known &&
        audio.public.classic_hfp_peer_bonded;
    bool music = audio.public.a2dp_connected && audio.public.a2dp_streaming && audio.public.a2dp_format_known &&
        audio.public.classic_a2dp_peer_bonded;
    audio.public.microphone_write_ready = audio.public.initialized && voice &&
        audio.public.route == BT_AUDIO_HFP && !audio.public.microphone_muted &&
        audio.microphone.count < audio.microphone.capacity;
    audio.public.speaker_read_ready = audio.public.initialized && !audio.public.speaker_muted &&
        ((audio.public.route == BT_AUDIO_HFP && voice) || (audio.public.route == BT_AUDIO_A2DP && music));
    audio.public.microphone_queued_frames = audio.microphone.frames;
    audio.public.speaker_queued_frames = audio.public.route == BT_AUDIO_HFP ? audio.voice.frames :
        (audio.public.route == BT_AUDIO_A2DP ? audio.music.frames : 0);
}

static void error_locked(const char *stage, esp_err_t error, bool fatal)
{
    if (audio.public.error == ESP_OK) {
        audio.public.error = error;
        audio.public.first_failure_stage = stage;
        audio.public.first_failure_us = now_us();
    }
    if (fatal) {
        audio.public.failed = true;
        audio.public.pairing_requested = false; audio.scan_pending = true;
        audio.init_pending = false;
        if (audio.command_pending || audio.public.control_inflight) {
            audio.public.request_status = BT_AUDIO_JOB_UNCERTAIN;
            audio.public.request_error = error;
        }
        audio.command_pending = false;
        invalidate_locked(true);
    }
    refresh_locked();
}

static void pop_ring(pcm_ring_t *ring)
{
    bt_audio_pcm_t *packet = &ring->slots[ring->head];
    ring->frames = (uint16_t)(ring->frames - (packet->frames - ring->offset));
    memset(packet, 0, sizeof(*packet));
    ring->head = (uint16_t)((ring->head + 1U) % ring->capacity);
    --ring->count; ring->offset = 0;
}

static void expire_ring_locked(pcm_ring_t *ring, uint64_t now)
{
    while (ring->count != 0) {
        const bt_audio_pcm_t *packet = &ring->slots[ring->head];
        if (packet->generation == audio.public.generation && packet->timestamp_us <= now &&
            now - packet->timestamp_us <= BT_AUDIO_PCM_MAX_AGE_US) break;
        pop_ring(ring); count_one(&audio.public.expired_packets);
        /* A receive gap must invalidate the manager's stream reservation;
         * never stitch later PCM onto an apparently contiguous sequence. */
        if (ring != &audio.microphone) { invalidate_locked(true); refresh_locked(); return; }
    }
}

static bool push_ring(pcm_ring_t *ring, const bt_audio_pcm_t *packet)
{
    if (ring->count == ring->capacity) return false;
    ring->slots[(ring->head + ring->count) % ring->capacity] = *packet;
    ++ring->count; ring->frames = (uint16_t)(ring->frames + packet->frames);
    return true;
}

static bool peer_matches(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

#if AUDIO_PROFILES_SUPPORTED
static void replace_stream_locked(bool hfp)
{
    uint64_t *generation = hfp ? &audio.hfp_link_generation : &audio.a2dp_link_generation;
    if (*generation == UINT64_MAX) {
        error_locked("Audio stream generation exhausted", ESP_ERR_INVALID_STATE, true);
        return;
    }
    ++*generation;
    invalidate_locked(true);
}

static void watchdog(TimerHandle_t timer)
{
    (void)timer;
    uint64_t now = now_us();
    portENTER_CRITICAL(&audio_lock);
    if (audio.public.pairing_requested && now >= audio.pairing_deadline_us) {
        audio.public.pairing_requested = false; audio.public.pairing_remaining_ms = 0;
        audio.scan_pending = true;
    } else if (audio.public.pairing_requested)
        audio.public.pairing_remaining_ms = (uint32_t)((audio.pairing_deadline_us - now + 999U) / 1000U);
    if (!audio.public.failed && audio.public.pairing_handshake_pending &&
        now - audio.handshake_started_us >= PAIRING_HANDSHAKE_US) {
        audio.public.pairing_error = ESP_ERR_TIMEOUT;
        audio.public.pairing_requested = false; audio.scan_pending = true;
        error_locked("Classic pairing completion timeout", ESP_ERR_TIMEOUT, true);
        /* Retain shared admission claim: late bond persistence is uncertain. */
    }
    if (!audio.public.failed && audio.init_pending && now >= audio.init_started_us &&
        now - audio.init_started_us >= BT_AUDIO_INIT_TIMEOUT_US) {
        error_locked("Audio profile initialization timeout", ESP_ERR_TIMEOUT, true);
    }
    if (!audio.public.failed && audio.public.control_inflight && now >= audio.command_started_us &&
        now - audio.command_started_us >= BT_AUDIO_COMMAND_TIMEOUT_US) {
        error_locked("Audio profile command timeout", ESP_ERR_TIMEOUT, true);
    }
    portEXIT_CRITICAL(&audio_lock);
}

static void classic_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *parameter)
{
    portENTER_CRITICAL(&audio_lock);
    if (event == ESP_BT_GAP_AUTH_CMPL_EVT || event == ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT) {
        invalidate_locked(true);
        ++audio.bond_revision;
        if (audio.classic_list_revision != UINT64_MAX) ++audio.classic_list_revision;
        else error_locked("Classic bond-list revision exhausted", ESP_ERR_INVALID_STATE, true);
        audio.bond_refresh_pending = true; audio.public.classic_bonds_known = false;
        if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
            if (audio.public.pairing_handshake_pending && peer_matches(audio.handshake_peer, parameter->auth_cmpl.bda)) {
                audio.public.pairing_handshake_pending = false;
                audio.public.pairing_error = parameter->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
            }
            if (parameter->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                audio.public.pairing_requested = false; audio.scan_pending = true;
            }
        }
    } else if (event == ESP_BT_GAP_CFM_REQ_EVT || event == ESP_BT_GAP_PIN_REQ_EVT || event == ESP_BT_GAP_KEY_REQ_EVT) {
        if (audio.security_pending) {
            audio.public.pairing_error = ESP_ERR_NO_MEM;
            audio.public.pairing_requested = false; audio.scan_pending = true;
            error_locked("Concurrent Classic security requests", ESP_ERR_NO_MEM, true);
        } else {
            audio.security_pending = true;
            audio.security_kind = event == ESP_BT_GAP_CFM_REQ_EVT ? 1U : (event == ESP_BT_GAP_PIN_REQ_EVT ? 2U : 3U);
            memcpy(audio.security_peer, event == ESP_BT_GAP_CFM_REQ_EVT ? parameter->cfm_req.bda :
                (event == ESP_BT_GAP_PIN_REQ_EVT ? parameter->pin_req.bda : parameter->key_req.bda), 6);
        }
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
}

static void service_classic_pairing(void)
{
    uint8_t bonds[BT_AUDIO_CLASSIC_BONDS_MAX][6];
    portENTER_CRITICAL(&audio_lock);
    bool refresh = audio.bond_refresh_pending;
    uint64_t revision = audio.bond_revision;
    portEXIT_CRITICAL(&audio_lock);
    if (refresh) {
        int count = esp_bt_gap_get_bond_device_num();
        esp_err_t error = count >= 0 && count <= (int)BT_AUDIO_CLASSIC_BONDS_MAX ? ESP_OK : ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) {
            count = (int)BT_AUDIO_CLASSIC_BONDS_MAX;
            error = esp_bt_gap_get_bond_device_list(&count, bonds);
            if (count < 0 || count > (int)BT_AUDIO_CLASSIC_BONDS_MAX || esp_bt_gap_get_bond_device_num() != count)
                error = ESP_ERR_INVALID_STATE;
        }
        portENTER_CRITICAL(&audio_lock);
        if (revision == audio.bond_revision) {
            audio.bond_refresh_pending = false;
            audio.public.classic_bonds_known = error == ESP_OK;
            audio.classic_list_error = error;
            if (audio.classic_list_revision != UINT64_MAX) ++audio.classic_list_revision;
            else error_locked("Classic bond-list revision exhausted", ESP_ERR_INVALID_STATE, true);
            if (error == ESP_OK) {
                memcpy(audio.classic_bonds, bonds, (size_t)count * 6U);
                audio.public.classic_bond_count = (uint8_t)count;
            } else audio.public.pairing_error = error;
            if (error != ESP_OK) invalidate_locked(true);
        }
        refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    }
    uint8_t peer[6]; unsigned int kind;
    portENTER_CRITICAL(&audio_lock);
    bool security = audio.security_pending;
    kind = audio.security_kind; memcpy(peer, audio.security_peer, 6);
    audio.security_pending = false;
    portEXIT_CRITICAL(&audio_lock);
    if (security) {
        int count = btc_storage_get_num_all_bond_devices();
        portENTER_CRITICAL(&audio_lock);
        bool accept = kind == 1 && audio.public.initialized && !audio.public.failed &&
            audio.public.pairing_requested && now_us() < audio.pairing_deadline_us && audio.pairing_claimed &&
            !audio.public.pairing_handshake_pending && audio.public.classic_bonds_known &&
            count >= 0 && count < CONFIG_BT_SMP_MAX_BONDS;
        if (accept) {
            audio.public.pairing_handshake_pending = true;
            memcpy(audio.handshake_peer, peer, 6); audio.handshake_started_us = now_us();
        }
        portEXIT_CRITICAL(&audio_lock);
        esp_err_t error;
        if (kind == 1) error = esp_bt_gap_ssp_confirm_reply(peer, accept);
        else if (kind == 2) { esp_bt_pin_code_t pin = {0}; error = esp_bt_gap_pin_reply(peer, false, 0, pin); }
        else error = esp_bt_gap_ssp_passkey_reply(peer, false, 0);
        if (error != ESP_OK) {
            portENTER_CRITICAL(&audio_lock);
            audio.public.pairing_error = error; audio.public.pairing_requested = false; audio.scan_pending = true;
            error_locked("Classic security reply submission", error, true);
            portEXIT_CRITICAL(&audio_lock);
        }
    }
    portENTER_CRITICAL(&audio_lock);
    bool submit_scan = audio.scan_pending && !audio.scan_submitted;
    bool discover = audio.public.pairing_requested && !audio.public.failed;
    if (submit_scan) {
        audio.scan_pending = false; audio.scan_submitted = true; audio.scan_expected = discover;
        audio.scan_started_us = now_us(); audio.public.discovery_known = false;
    }
    portEXIT_CRITICAL(&audio_lock);
    if (submit_scan) {
        esp_err_t error = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
            discover ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
        if (error != ESP_OK) {
            portENTER_CRITICAL(&audio_lock);
            audio.public.pairing_error = error; audio.public.pairing_requested = false;
            error_locked("Classic discovery submission", error, true);
            portEXIT_CRITICAL(&audio_lock);
        }
    }
    esp_bt_gap_profile_status_t status;
    esp_err_t observed = esp_bt_gap_get_profile_status(&status);
    portENTER_CRITICAL(&audio_lock);
    if (observed == ESP_OK) {
        audio.public.discovery_known = true;
        audio.public.discoverable = status.disc_mode != ESP_BT_NON_DISCOVERABLE;
        if (audio.scan_submitted && audio.public.discoverable == audio.scan_expected &&
            status.conn_mode == ESP_BT_CONNECTABLE) audio.scan_submitted = false;
    }
    if (audio.scan_submitted && now_us() - audio.scan_started_us >= BT_AUDIO_INIT_TIMEOUT_US) {
        audio.public.pairing_error = ESP_ERR_TIMEOUT; audio.public.pairing_requested = false;
        error_locked("Classic discovery verification timeout", ESP_ERR_TIMEOUT, true);
    }
    bool release = audio.pairing_claimed && !audio.public.pairing_requested && !audio.public.pairing_handshake_pending &&
        audio.public.discovery_known && !audio.public.discoverable && !audio.scan_pending && !audio.scan_submitted && !audio.public.failed;
    if (release) audio.pairing_claimed = false;
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    if (release) ble_hid_pairing_release_classic();
}

static void complete_command_locked(bt_audio_command_t command, const uint8_t peer[6], esp_err_t error)
{
    if (!audio.public.control_inflight || audio.command != command ||
        !peer_matches(audio.command_peer, peer)) return;
    audio.public.control_inflight = false;
    if (audio.public.request_status == BT_AUDIO_JOB_WAITING) {
        audio.public.request_status = error == ESP_OK ? BT_AUDIO_JOB_SUCCEEDED : BT_AUDIO_JOB_FAILED;
        audio.public.request_error = error;
    }
}

static void hfp_event(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *parameter)
{
    portENTER_CRITICAL(&audio_lock);
    if (!audio.started) { portEXIT_CRITICAL(&audio_lock); return; }
    if (event == ESP_HF_CLIENT_PROF_STATE_EVT) {
        if (parameter->prof_stat.state == ESP_HF_INIT_SUCCESS) audio.public.hfp_initialized = true;
        else error_locked("HFP profile initialization/state", ESP_FAIL, true);
    } else if (event == ESP_HF_CLIENT_CONNECTION_STATE_EVT) {
        const uint8_t *peer = parameter->conn_stat.remote_bda;
        bool current = !audio.public.hfp_peer_known || peer_matches(audio.public.hfp_peer, peer);
        if (current) {
            bool slc = parameter->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED;
            bool down = parameter->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
            if (audio.public.hfp_connected != slc || down || !audio.public.hfp_peer_known)
                replace_stream_locked(true);
            audio.public.hfp_connected = slc;
            audio.hfp_rfcomm = !down;
            audio.public.hfp_peer_known = !down;
            if (!down) memcpy(audio.public.hfp_peer, peer, 6);
            else {
                memset(audio.public.hfp_peer, 0, 6);
                audio.public.hfp_audio_connected = audio.public.hfp_format_known = false;
                audio.public.hfp_sample_rate = 0;
            }
            if (slc) complete_command_locked(BT_AUDIO_CONNECT_HFP, peer, ESP_OK);
            if (down) {
                complete_command_locked(BT_AUDIO_DISCONNECT_HFP, peer, ESP_OK);
                complete_command_locked(BT_AUDIO_CONNECT_HFP, peer, ESP_FAIL);
                complete_command_locked(BT_AUDIO_OPEN_VOICE, peer, ESP_FAIL);
                complete_command_locked(BT_AUDIO_CLOSE_VOICE, peer, ESP_OK);
            }
        }
    } else if (event == ESP_HF_CLIENT_AUDIO_STATE_EVT && audio.public.hfp_peer_known &&
        peer_matches(audio.public.hfp_peer, parameter->audio_stat.remote_bda)) {
        bool connected = parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
            parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ||
            parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_LC3;
        uint32_t rate = parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ? 8000U :
            (parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? 16000U : 0U);
        bool known = connected && rate != 0;
        if (audio.public.hfp_audio_connected != connected || audio.public.hfp_sample_rate != rate)
            replace_stream_locked(true);
        audio.public.hfp_audio_connected = connected;
        audio.public.hfp_format_known = known;
        audio.public.hfp_sample_rate = rate;
        if (connected) {
            if (!known) error_locked("Unsupported HFP voice codec", ESP_ERR_NOT_SUPPORTED, false);
            complete_command_locked(BT_AUDIO_OPEN_VOICE, parameter->audio_stat.remote_bda,
                known ? ESP_OK : ESP_ERR_NOT_SUPPORTED);
        } else if (parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            complete_command_locked(BT_AUDIO_CLOSE_VOICE, parameter->audio_stat.remote_bda, ESP_OK);
            complete_command_locked(BT_AUDIO_OPEN_VOICE, parameter->audio_stat.remote_bda, ESP_FAIL);
        }
    }
    if (audio.public.hfp_initialized && audio.public.a2dp_initialized) audio.init_pending = false;
    refresh_locked();
    portEXIT_CRITICAL(&audio_lock);
}

static void a2dp_event(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *parameter)
{
    portENTER_CRITICAL(&audio_lock);
    if (!audio.started) { portEXIT_CRITICAL(&audio_lock); return; }
    if (event == ESP_A2D_PROF_STATE_EVT) {
        if (parameter->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) audio.public.a2dp_initialized = true;
        else error_locked("A2DP profile initialization/state", ESP_FAIL, true);
    } else if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        const uint8_t *peer = parameter->conn_stat.remote_bda;
        bool current = !audio.public.a2dp_peer_known || peer_matches(audio.public.a2dp_peer, peer);
        if (current) {
            bool up = parameter->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            bool down = parameter->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED;
            if (audio.public.a2dp_connected != up || down || !audio.public.a2dp_peer_known ||
                (!down && audio.a2dp_handle != parameter->conn_stat.conn_hdl)) replace_stream_locked(false);
            audio.public.a2dp_connected = up;
            audio.public.a2dp_peer_known = !down;
            if (!down) { memcpy(audio.public.a2dp_peer, peer, 6); audio.a2dp_handle = parameter->conn_stat.conn_hdl; }
            else {
                memset(audio.public.a2dp_peer, 0, 6);
                audio.public.a2dp_streaming = audio.public.a2dp_format_known = false;
                audio.public.a2dp_sample_rate = 0; audio.public.a2dp_source_channels = 0;
            }
            if (up) complete_command_locked(BT_AUDIO_CONNECT_A2DP, peer, ESP_OK);
            if (down) {
                complete_command_locked(BT_AUDIO_DISCONNECT_A2DP, peer, ESP_OK);
                complete_command_locked(BT_AUDIO_CONNECT_A2DP, peer, ESP_FAIL);
            }
        }
    } else if (event == ESP_A2D_AUDIO_CFG_EVT && audio.public.a2dp_peer_known &&
        parameter->audio_cfg.conn_hdl == audio.a2dp_handle &&
        peer_matches(audio.public.a2dp_peer, parameter->audio_cfg.remote_bda)) {
        uint32_t rate = 0; uint8_t channels = 0;
        if (parameter->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            const esp_a2d_cie_sbc_t *sbc = &parameter->audio_cfg.mcc.cie.sbc_info;
            switch (sbc->samp_freq) {
            case ESP_A2D_SBC_CIE_SF_16K: rate = 16000; break;
            case ESP_A2D_SBC_CIE_SF_32K: rate = 32000; break;
            case ESP_A2D_SBC_CIE_SF_44K: rate = 44100; break;
            case ESP_A2D_SBC_CIE_SF_48K: rate = 48000; break;
            default: break;
            }
            if (sbc->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_MONO) channels = 1;
            else if (sbc->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL ||
                sbc->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_STEREO ||
                sbc->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO) channels = 2;
        }
        if (audio.public.a2dp_sample_rate != rate || audio.public.a2dp_source_channels != channels)
            replace_stream_locked(false);
        audio.public.a2dp_format_known = rate != 0 && channels != 0;
        audio.public.a2dp_sample_rate = rate; audio.public.a2dp_source_channels = channels;
        if (!audio.public.a2dp_format_known) error_locked("Unsupported SBC configuration", ESP_ERR_NOT_SUPPORTED, false);
    } else if (event == ESP_A2D_AUDIO_STATE_EVT && audio.public.a2dp_peer_known &&
        parameter->audio_stat.conn_hdl == audio.a2dp_handle &&
        peer_matches(audio.public.a2dp_peer, parameter->audio_stat.remote_bda)) {
        bool streaming = parameter->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        if (audio.public.a2dp_streaming != streaming) replace_stream_locked(false);
        audio.public.a2dp_streaming = streaming;
    }
    if (audio.public.hfp_initialized && audio.public.a2dp_initialized) audio.init_pending = false;
    refresh_locked();
    portEXIT_CRITICAL(&audio_lock);
}

static int16_t sample_le(const uint8_t *data)
{
    uint16_t value = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    return value <= INT16_MAX ? (int16_t)value : (int16_t)((int32_t)value - 65536);
}

static void receive_pcm(bt_audio_route_t source, const uint8_t *data, uint32_t bytes)
{
    bt_audio_pcm_t packet = {0};
    uint8_t channels;
    uint64_t playback_epoch;
    portENTER_CRITICAL(&audio_lock);
    bool allowed = audio.public.speaker_read_ready && audio.public.route == source &&
        (!audio.adapter.prepared || (audio.adapter.playback_enabled && audio.adapter.transport_generation == audio.public.generation));
    packet.generation = audio.public.generation;
    packet.consumer_generation = audio.adapter.playback_enabled ? audio.adapter.playback_consumer : 0;
    playback_epoch = audio.adapter.playback_epoch;
    packet.sample_rate = source == BT_AUDIO_HFP ? audio.public.hfp_sample_rate : audio.public.a2dp_sample_rate;
    channels = source == BT_AUDIO_HFP ? 1 : audio.public.a2dp_source_channels;
    portEXIT_CRITICAL(&audio_lock);
    if (!allowed) return;
    if (!data || bytes == 0 || bytes > CALLBACK_BYTES_MAX || channels == 0 ||
        bytes % (2U * channels) != 0) {
        portENTER_CRITICAL(&audio_lock); count_one(&audio.public.malformed_packets);
        error_locked("Malformed Bluetooth PCM", ESP_ERR_INVALID_SIZE, false);
        invalidate_locked(true); refresh_locked();
        portEXIT_CRITICAL(&audio_lock); return;
    }
    packet.timestamp_us = now_us(); packet.source = source;
    size_t total = bytes / (2U * channels);
    for (size_t offset = 0; offset < total;) {
        size_t frames = total - offset;
        if (frames > BT_AUDIO_PCM_FRAMES) frames = BT_AUDIO_PCM_FRAMES;
        packet.frames = (uint16_t)frames;
        for (size_t i = 0; i < frames; ++i) {
            const uint8_t *frame = data + (offset + i) * 2U * channels;
            int32_t sample = sample_le(frame);
            if (channels == 2) sample = (sample + sample_le(frame + 2)) / 2;
            packet.samples[i] = (int16_t)sample;
        }
        portENTER_CRITICAL(&audio_lock);
        bool current = packet.generation == audio.public.generation && audio.public.speaker_read_ready &&
            audio.public.route == source && playback_epoch == audio.adapter.playback_epoch &&
            packet.consumer_generation == (audio.adapter.playback_enabled ? audio.adapter.playback_consumer : 0) &&
            (!audio.adapter.prepared || audio.adapter.playback_enabled);
        if (current && !push_ring(source == BT_AUDIO_HFP ? &audio.voice : &audio.music, &packet)) {
            count_one(&audio.public.receive_overruns);
            error_locked("Bluetooth speaker queue overflow", ESP_ERR_NO_MEM, false);
            invalidate_locked(true);
            current = false;
        }
        refresh_locked(); portEXIT_CRITICAL(&audio_lock);
        if (!current) return;
        offset += frames;
    }
}

static void hfp_receive(const uint8_t *data, uint32_t bytes) { receive_pcm(BT_AUDIO_HFP, data, bytes); }
static void a2dp_receive(const uint8_t *data, uint32_t bytes) { receive_pcm(BT_AUDIO_A2DP, data, bytes); }

static uint32_t hfp_transmit(uint8_t *data, uint32_t bytes)
{
    uint64_t now = now_us();
    portENTER_CRITICAL(&audio_lock);
    if (!audio.public.initialized || audio.public.route != BT_AUDIO_HFP || audio.public.microphone_muted ||
        !audio.public.hfp_connected || !audio.public.hfp_audio_connected || !audio.public.hfp_format_known ||
        !audio.public.classic_hfp_peer_bonded) {
        portEXIT_CRITICAL(&audio_lock); return 0;
    }
    if (!data || bytes == 0 || (bytes & 1U) || bytes > BT_AUDIO_PCM_FRAMES * 2U) {
        count_one(&audio.public.malformed_packets); portEXIT_CRITICAL(&audio_lock); return 0;
    }
    expire_ring_locked(&audio.microphone, now);
    size_t frames = bytes / 2U;
    if (audio.microphone.frames < frames) {
        count_one(&audio.public.transmit_underflows); refresh_locked();
        portEXIT_CRITICAL(&audio_lock); return 0;
    }
    for (size_t i = 0; i < frames; ++i) {
        pcm_ring_t *ring = &audio.microphone;
        bt_audio_pcm_t *packet = &ring->slots[ring->head];
        uint16_t value = (uint16_t)packet->samples[ring->offset++];
        data[2U * i] = (uint8_t)value; data[2U * i + 1U] = (uint8_t)(value >> 8);
        --ring->frames;
        if (ring->offset == packet->frames) pop_ring(ring);
    }
    audio.microphone_kick = audio.microphone.frames != 0;
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return bytes;
}
#endif

esp_err_t bt_audio_init(void)
{
    portENTER_CRITICAL(&audio_lock);
    audio.public.initialization_attempted = true;
    bool started = audio.started;
    portEXIT_CRITICAL(&audio_lock);
    if (!AUDIO_PROFILES_SUPPORTED) {
        portENTER_CRITICAL(&audio_lock);
        error_locked("Classic audio build configuration unavailable", ESP_ERR_NOT_SUPPORTED, false);
        portEXIT_CRITICAL(&audio_lock); return ESP_ERR_NOT_SUPPORTED;
    }
    if (started) return ESP_ERR_INVALID_STATE;
#if AUDIO_PROFILES_SUPPORTED
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED ||
        esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&audio_lock);
    audio.started = true; audio.init_pending = true; audio.init_started_us = now_us();
    audio.bond_refresh_pending = true; audio.scan_pending = true;
    audio.public.classic_bond_capacity = CONFIG_BT_SMP_MAX_BONDS;
    invalidate_locked(true);
    portEXIT_CRITICAL(&audio_lock);
    esp_fill_random(audio.boot_id.bytes, sizeof(audio.boot_id.bytes));
    audio.memory = heap_caps_calloc_prefer(SLOT_COUNT, sizeof(*audio.memory), 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *stage = "Audio PCM queue allocation";
    esp_err_t error = audio.memory ? ESP_OK : ESP_ERR_NO_MEM;
    if (error == ESP_OK) {
        portENTER_CRITICAL(&audio_lock);
        audio.music = (pcm_ring_t){.slots = audio.memory, .capacity = A2DP_SLOTS};
        audio.voice = (pcm_ring_t){.slots = audio.memory + A2DP_SLOTS, .capacity = VOICE_SLOTS};
        audio.microphone = (pcm_ring_t){.slots = audio.memory + A2DP_SLOTS + VOICE_SLOTS, .capacity = VOICE_SLOTS};
        portEXIT_CRITICAL(&audio_lock);
        stage = "Audio watchdog allocation";
        TimerHandle_t timer = xTimerCreate("bt_audio_deadline", pdMS_TO_TICKS(WATCHDOG_MS), pdTRUE, NULL, watchdog);
        error = timer && xTimerStart(timer, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) { stage = "Classic GAP callback registration"; error = esp_bt_gap_register_callback(classic_gap_event); }
    if (error == ESP_OK) {
        uint8_t capability = ESP_BT_IO_CAP_NONE;
        stage = "Classic SSP capability configuration";
        error = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability));
    }
    if (error == ESP_OK) { stage = "HFP callback registration"; error = esp_hf_client_register_callback(hfp_event); }
    if (error == ESP_OK) { stage = "HFP profile initialization"; error = esp_hf_client_init(); }
    if (error == ESP_OK) { stage = "HFP PCM callback registration"; error = esp_hf_client_register_data_callback(hfp_receive, hfp_transmit); }
    if (error == ESP_OK) { stage = "A2DP callback registration"; error = esp_a2d_register_callback(a2dp_event); }
    if (error == ESP_OK) { stage = "A2DP PCM callback registration"; error = esp_a2d_sink_register_data_callback(a2dp_receive); }
    if (error == ESP_OK) { stage = "A2DP sink initialization"; error = esp_a2d_sink_init(); }
    portENTER_CRITICAL(&audio_lock);
    audio.submitted = error == ESP_OK;
    if (error != ESP_OK) error_locked(stage, error, true);
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return error;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bt_audio_snapshot_t bt_audio_snapshot(void)
{
    portENTER_CRITICAL(&audio_lock);
    bt_audio_snapshot_t snapshot = audio.public;
    portEXIT_CRITICAL(&audio_lock);
    return snapshot;
}

esp_err_t bt_audio_pairing_open(uint32_t duration_ms)
{
    if (!AUDIO_PROFILES_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (!duration_ms || duration_ms > BT_AUDIO_PAIRING_MAX_MS) return ESP_ERR_INVALID_ARG;
#if AUDIO_PROFILES_SUPPORTED
    portENTER_CRITICAL(&audio_lock);
    bool ready = audio.public.initialized && !audio.public.failed && audio.public.classic_bonds_known &&
        !audio.pairing_claimed && !audio.public.pairing_handshake_pending;
    portEXIT_CRITICAL(&audio_lock);
    if (!ready || !ble_hid_pairing_claim_classic()) return ESP_ERR_INVALID_STATE;
    int count = btc_storage_get_num_all_bond_devices();
    if (count < 0 || count >= CONFIG_BT_SMP_MAX_BONDS) {
        ble_hid_pairing_release_classic();
        return count < 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&audio_lock);
    audio.pairing_claimed = true;
    audio.public.pairing_requested = true; audio.public.pairing_remaining_ms = duration_ms;
    audio.public.pairing_error = ESP_OK;
    audio.pairing_deadline_us = now_us() + (uint64_t)duration_ms * 1000U;
    audio.scan_pending = true;
    portEXIT_CRITICAL(&audio_lock);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void bt_audio_pairing_close(void)
{
    portENTER_CRITICAL(&audio_lock);
    audio.public.pairing_requested = false; audio.public.pairing_remaining_ms = 0;
    audio.scan_pending = true;
    portEXIT_CRITICAL(&audio_lock);
}

uint64_t bt_audio_cancel(void)
{
    portENTER_CRITICAL(&audio_lock);
    invalidate_locked(true);
    if (audio.command_pending || audio.public.control_inflight) audio.public.request_status = BT_AUDIO_JOB_CANCELLED;
    audio.command_pending = false;
    uint64_t generation = audio.public.generation;
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return generation;
}

esp_err_t bt_audio_set_policy(uint64_t generation, bt_audio_route_t route,
                              bool microphone_muted, bool speaker_muted, uint64_t *new_generation)
{
    if (!AUDIO_PROFILES_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (route < BT_AUDIO_OFF || route > BT_AUDIO_A2DP ||
        (route != BT_AUDIO_HFP && !microphone_muted) ||
        (route == BT_AUDIO_OFF && !speaker_muted)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&audio_lock);
    bool ready = audio.public.initialized && !audio.public.failed && generation == audio.public.generation;
    if (route == BT_AUDIO_HFP) ready = ready && audio.public.hfp_connected &&
        audio.public.hfp_audio_connected && audio.public.hfp_format_known && audio.public.classic_hfp_peer_bonded;
    if (route == BT_AUDIO_A2DP) ready = ready && audio.public.a2dp_connected && audio.public.a2dp_format_known &&
        audio.public.classic_a2dp_peer_bonded;
    if (ready) {
        if (route != audio.public.route || microphone_muted != audio.public.microphone_muted ||
            speaker_muted != audio.public.speaker_muted) invalidate_locked(false);
        audio.public.route = route; audio.public.microphone_muted = microphone_muted;
        audio.public.speaker_muted = speaker_muted;
        if (new_generation) *new_generation = audio.public.generation;
        refresh_locked();
    }
    portEXIT_CRITICAL(&audio_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t submit_control_locked(uint64_t generation, bt_audio_command_t command,
                                       const uint8_t peer[6], uint64_t *request_id)
{
    bool hfp = command != BT_AUDIO_CONNECT_A2DP && command != BT_AUDIO_DISCONNECT_A2DP;
    bool known = hfp ? audio.public.hfp_peer_known : audio.public.a2dp_peer_known;
    const uint8_t *current = hfp ? audio.public.hfp_peer : audio.public.a2dp_peer;
    bool connecting = command == BT_AUDIO_CONNECT_HFP || command == BT_AUDIO_CONNECT_A2DP;
    bool accepted = audio.public.initialized && !audio.public.failed && generation == audio.public.generation &&
        !audio.command_pending && !audio.public.control_inflight &&
        (connecting ? !known : (known && (!peer || peer_matches(peer, current))));
    if (command == BT_AUDIO_OPEN_VOICE || command == BT_AUDIO_CLOSE_VOICE) accepted = accepted && audio.public.hfp_connected;
    if (accepted) {
        memcpy(audio.command_peer, peer ? peer : current, 6);
        audio.command = command; audio.command_pending = true;
        audio.command_requires_saved = false;
        ++audio.public.request_id; if (audio.public.request_id == 0) ++audio.public.request_id;
        audio.public.request_status = BT_AUDIO_JOB_PENDING; audio.public.request_error = ESP_OK;
        invalidate_locked(true);
        if (request_id) *request_id = audio.public.request_id;
        refresh_locked();
    }
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool valid_control(bt_audio_command_t command, const uint8_t *peer)
{
    return command >= BT_AUDIO_CONNECT_HFP && command <= BT_AUDIO_CLOSE_VOICE &&
        ((command != BT_AUDIO_CONNECT_HFP && command != BT_AUDIO_CONNECT_A2DP) || peer);
}

esp_err_t bt_audio_request(uint64_t generation, bt_audio_command_t command,
                           const uint8_t peer[6], uint64_t *request_id)
{
    if (!AUDIO_PROFILES_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (!valid_control(command, peer)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&audio_lock);
    esp_err_t error = submit_control_locked(generation, command, peer, request_id);
    portEXIT_CRITICAL(&audio_lock);
    return error;
}

esp_err_t bt_audio_saved_request(uint64_t generation, uint64_t bond_revision,
                                 bt_audio_command_t command, const uint8_t peer[6], uint64_t *request_id)
{
    if (!AUDIO_PROFILES_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (!peer || !valid_control(command, peer)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&audio_lock);
    bool saved = false;
    for (unsigned int i = 0; i < audio.public.classic_bond_count; ++i)
        saved |= peer_matches(peer, audio.classic_bonds[i]);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (audio.public.classic_bonds_known && !audio.bond_refresh_pending && bond_revision == audio.classic_list_revision) {
        error = saved ? submit_control_locked(generation, command, peer, request_id) : ESP_ERR_NOT_FOUND;
        if (error == ESP_OK) { audio.command_requires_saved = true; audio.command_bond_revision = audio.bond_revision; }
    }
    portEXIT_CRITICAL(&audio_lock);
    return error;
}

bt_audio_bond_list_t bt_audio_bond_list(void)
{
    portENTER_CRITICAL(&audio_lock);
    bt_audio_bond_list_t list = {.known = audio.public.classic_bonds_known,
        .refreshing = audio.bond_refresh_pending, .revision = audio.classic_list_revision,
        .error = !AUDIO_PROFILES_SUPPORTED ? ESP_ERR_NOT_SUPPORTED :
            (audio.public.classic_bonds_known ? ESP_OK : (audio.classic_list_error ? audio.classic_list_error : ESP_ERR_INVALID_STATE)),
        .count = audio.public.classic_bond_count, .capacity = audio.public.classic_bond_capacity};
    for (unsigned int i = 0; i < list.count; ++i) {
        memcpy(list.bonds[i].address, audio.classic_bonds[i], 6);
        list.bonds[i].hfp_current = list.known && audio.public.hfp_connected && audio.public.hfp_peer_known &&
            peer_matches(list.bonds[i].address, audio.public.hfp_peer);
        list.bonds[i].a2dp_current = list.known && audio.public.a2dp_connected && audio.public.a2dp_peer_known &&
            peer_matches(list.bonds[i].address, audio.public.a2dp_peer);
    }
    portEXIT_CRITICAL(&audio_lock);
    return list;
}

esp_err_t bt_audio_refresh_bonds(void)
{
    if (!AUDIO_PROFILES_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&audio_lock);
    bool ready = audio.public.initialized && !audio.public.failed;
    if (ready) audio.bond_refresh_pending = true;
    portEXIT_CRITICAL(&audio_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void bt_audio_service(void)
{
#if AUDIO_PROFILES_SUPPORTED
    watchdog(NULL);
    service_classic_pairing();
    uint64_t now = now_us();
    uint8_t peer[6]; bt_audio_command_t command; uint64_t request;
    portENTER_CRITICAL(&audio_lock);
    expire_ring_locked(&audio.music, now); expire_ring_locked(&audio.voice, now);
    expire_ring_locked(&audio.microphone, now);
    bool saved_peer_present = false;
    for (unsigned int i = 0; i < audio.public.classic_bond_count; ++i)
        saved_peer_present |= peer_matches(audio.command_peer, audio.classic_bonds[i]);
    if (audio.command_pending && audio.command_requires_saved &&
        (!audio.public.classic_bonds_known || audio.bond_refresh_pending || !saved_peer_present ||
         audio.command_bond_revision != audio.bond_revision)) {
        audio.command_pending = false;
        audio.public.request_status = BT_AUDIO_JOB_FAILED; audio.public.request_error = ESP_ERR_INVALID_STATE;
    }
    bool send_command = audio.command_pending && audio.public.initialized && !audio.public.failed;
    bool kick = audio.microphone_kick && audio.public.initialized && audio.public.route == BT_AUDIO_HFP &&
        !audio.public.microphone_muted && audio.public.hfp_audio_connected && audio.microphone.frames != 0;
    audio.microphone_kick = false;
    command = audio.command; request = audio.public.request_id; memcpy(peer, audio.command_peer, 6);
    if (send_command) {
        audio.command_pending = false; audio.public.control_inflight = true;
        audio.command_started_us = now; audio.public.request_status = BT_AUDIO_JOB_WAITING;
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    if (send_command) {
        esp_err_t error;
        switch (command) {
        case BT_AUDIO_CONNECT_HFP: error = esp_hf_client_connect(peer); break;
        case BT_AUDIO_CONNECT_A2DP: error = esp_a2d_sink_connect(peer); break;
        case BT_AUDIO_DISCONNECT_HFP: error = esp_hf_client_disconnect(peer); break;
        case BT_AUDIO_DISCONNECT_A2DP: error = esp_a2d_sink_disconnect(peer); break;
        case BT_AUDIO_OPEN_VOICE: error = esp_hf_client_connect_audio(peer); break;
        default: error = esp_hf_client_disconnect_audio(peer); break;
        }
        if (error != ESP_OK) {
            portENTER_CRITICAL(&audio_lock);
            if (audio.public.request_id == request && audio.public.control_inflight) {
                audio.public.control_inflight = false;
                if (audio.public.request_status == BT_AUDIO_JOB_WAITING) {
                    audio.public.request_status = BT_AUDIO_JOB_FAILED; audio.public.request_error = error;
                }
                error_locked("Audio profile command submission", error, false);
            }
            portEXIT_CRITICAL(&audio_lock);
        }
    }
    if (kick) esp_hf_client_outgoing_data_ready();
#endif
}

esp_err_t bt_audio_write_microphone(const bt_audio_pcm_t *packet)
{
    if (!AUDIO_PROFILES_SUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (!packet || packet->frames == 0 || packet->frames > BT_AUDIO_PCM_FRAMES ||
        packet->source != BT_AUDIO_HFP) return ESP_ERR_INVALID_ARG;
    uint64_t now = now_us();
    if (packet->timestamp_us > now || now - packet->timestamp_us > BT_AUDIO_PCM_MAX_AGE_US) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&audio_lock);
    expire_ring_locked(&audio.microphone, now);
    refresh_locked();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (packet->generation == audio.public.generation && audio.public.microphone_write_ready &&
        packet->sample_rate == audio.public.hfp_sample_rate) {
        bool accepted = push_ring(&audio.microphone, packet);
        error = accepted ? ESP_OK : ESP_ERR_NO_MEM;
        if (accepted) audio.microphone_kick = true;
        else count_one(&audio.public.transmit_overruns);
    } else if (packet->generation == audio.public.generation && audio.public.initialized &&
        !audio.public.microphone_muted && audio.microphone.count == audio.microphone.capacity) {
        error = ESP_ERR_NO_MEM; count_one(&audio.public.transmit_overruns);
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return error;
}

bool bt_audio_read_speaker(uint64_t generation, bt_audio_pcm_t *packet)
{
    if (!packet || !AUDIO_PROFILES_SUPPORTED) return false;
    uint64_t now = now_us();
    portENTER_CRITICAL(&audio_lock);
    pcm_ring_t *ring = audio.public.route == BT_AUDIO_HFP ? &audio.voice : &audio.music;
    expire_ring_locked(ring, now);
    bool ready = generation == audio.public.generation && audio.public.speaker_read_ready && ring->count != 0;
    if (ready) { *packet = ring->slots[ring->head]; pop_ring(ring); }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return ready;
}

static bool same_token(const ae_route_token_t *a, const ae_route_token_t *b)
{
    return a && b && a->link_generation == b->link_generation &&
        memcmp(a->peer_id.bytes, b->peer_id.bytes, AE_ID_BYTES) == 0 &&
        memcmp(a->stream_id.bytes, b->stream_id.bytes, AE_ID_BYTES) == 0;
}

static ae_route_token_t token_locked(ae_route_t route)
{
    bool hfp = route == AE_ROUTE_HFP_HF;
    bool known = hfp ? audio.public.hfp_peer_known : audio.public.a2dp_peer_known;
    uint64_t generation = hfp ? audio.hfp_link_generation : audio.a2dp_link_generation;
    ae_route_token_t token = {0};
    if (!known || !generation) return token;
    /* Namespaced actual BR/EDR address, not a BLE identity or inferred host.
     * These opaque comparison IDs are not credentials and must not be logged. */
    token.peer_id.bytes[0] = 0xb7;
    token.peer_id.bytes[1] = 0xc1;
    memcpy(token.peer_id.bytes + 10, hfp ? audio.public.hfp_peer : audio.public.a2dp_peer, 6);
    token.stream_id = audio.boot_id;
    token.stream_id.bytes[0] = hfp ? 1 : 2;
    for (unsigned i = 0; i < 8; ++i) token.stream_id.bytes[i + 8] ^= (uint8_t)(generation >> (8U * i));
    token.link_generation = generation;
    return token;
}

static ae_format_t format_locked(ae_route_t route)
{
    return (ae_format_t){.sample_rate_hz = route == AE_ROUTE_HFP_HF ?
        audio.public.hfp_sample_rate : audio.public.a2dp_sample_rate,
        .channels = 1, .frame_ms = 10, .encoding = AE_PCM_S16_LE};
}

static bool same_format(const ae_format_t *a, const ae_format_t *b)
{
    return a && b && a->sample_rate_hz == b->sample_rate_hz && a->channels == b->channels &&
        a->frame_ms == b->frame_ms && a->encoding == b->encoding;
}

static bool route_live_locked(ae_route_t route)
{
    if (!audio.public.initialized || audio.public.failed) return false;
    return route == AE_ROUTE_HFP_HF ? (audio.public.hfp_connected &&
        audio.public.hfp_audio_connected && audio.public.hfp_format_known && audio.public.classic_hfp_peer_bonded) :
        (audio.public.a2dp_connected && audio.public.a2dp_streaming && audio.public.a2dp_format_known &&
         audio.public.classic_a2dp_peer_bonded);
}

static bool valid_route(ae_route_t route) { return route == AE_ROUTE_HFP_HF || route == AE_ROUTE_A2DP_SINK; }

static ae_result_t port_capabilities(void *context, ae_route_t route, ae_route_capabilities_t *out)
{
    (void)context;
    if (!out || !valid_route(route)) return AE_INVALID;
    portENTER_CRITICAL(&audio_lock);
    bool live = route_live_locked(route);
    *out = (ae_route_capabilities_t){.implemented = AUDIO_PROFILES_SUPPORTED,
        .available = audio.public.initialized && !audio.public.failed, .connected = live,
        .qualified = false, .capture = live && route == AE_ROUTE_HFP_HF, .playback = live,
        .full_duplex = AUDIO_PROFILES_SUPPORTED && route == AE_ROUTE_HFP_HF,
        .format = format_locked(route), .token = token_locked(route),
        .reason = !AUDIO_PROFILES_SUPPORTED ? AE_NOT_SUPPORTED : (live ? AE_OK : AE_UNAVAILABLE)};
    portEXIT_CRITICAL(&audio_lock);
    return AE_OK;
}

static ae_result_t port_prepare(void *context, ae_route_t route, const ae_format_t *format,
                               uint8_t roles, uint64_t generation, const ae_route_token_t *expected)
{
    (void)context;
    if (!valid_route(route) || !format || !roles || (roles & ~(AE_MICROPHONE | AE_SPEAKER)) ||
        !generation || !expected) return AE_INVALID;
    if (!AUDIO_PROFILES_SUPPORTED) return AE_NOT_SUPPORTED;
    if (route == AE_ROUTE_A2DP_SINK && (roles & AE_MICROPHONE)) return AE_NOT_SUPPORTED;
    portENTER_CRITICAL(&audio_lock);
    ae_route_token_t token = token_locked(route);
    ae_format_t actual = format_locked(route);
    ae_result_t result = AE_OK;
    if (!same_token(expected, &token)) result = AE_STALE;
    else if (!route_live_locked(route)) result = AE_UNAVAILABLE;
    else if (!same_format(format, &actual)) result = AE_NOT_SUPPORTED;
    else if (audio.adapter.generation == generation &&
        (!audio.adapter.prepared || audio.adapter.stopped || audio.adapter.route != route ||
         audio.adapter.roles != roles || !same_token(expected, &audio.adapter.token) ||
         audio.adapter.transport_generation != audio.public.generation)) result = AE_STALE;
    else if (audio.adapter.generation != generation) {
        invalidate_locked(true);
        audio.public.route = route == AE_ROUTE_HFP_HF ? BT_AUDIO_HFP : BT_AUDIO_A2DP;
        audio.public.microphone_muted = !(roles & AE_MICROPHONE);
        audio.public.speaker_muted = !(roles & AE_SPEAKER);
        audio.adapter.prepared = true; audio.adapter.stopped = false; audio.adapter.tx_seen = false;
        audio.adapter.route = route; audio.adapter.roles = roles; audio.adapter.format = actual;
        audio.adapter.token = *expected; audio.adapter.generation = generation;
        audio.adapter.transport_generation = audio.public.generation;
        audio.adapter.sequence = audio.adapter.sample_offset = audio.adapter.tx_sequence = 0;
        refresh_locked();
    }
    portEXIT_CRITICAL(&audio_lock);
    return result;
}

static ae_result_t port_capture(void *context, ae_route_t route, const ae_frame_t *frame)
{
    (void)context;
    if (!frame || route != AE_ROUTE_HFP_HF || !frame->consumer_generation || !frame->sequence)
        return AE_INVALID;
    if (!AUDIO_PROFILES_SUPPORTED) return AE_NOT_SUPPORTED;
    portENTER_CRITICAL(&audio_lock);
    ae_route_token_t token = token_locked(route);
    uint32_t frames = audio.adapter.format.sample_rate_hz / 100U;
    ae_result_t result = AE_OK;
    uint64_t now = now_us();
    expire_ring_locked(&audio.microphone, now); refresh_locked();
    if (!audio.adapter.prepared || audio.adapter.stopped || !(audio.adapter.roles & AE_MICROPHONE)) result = AE_UNAVAILABLE;
    else if (audio.adapter.route != route || audio.adapter.generation != frame->audio_generation ||
        audio.adapter.transport_generation != audio.public.generation ||
        !same_token(&frame->route_token, &token) || !same_token(&frame->route_token, &audio.adapter.token) ||
        (audio.adapter.tx_seen && frame->sequence <= audio.adapter.tx_sequence)) result = AE_STALE;
    else if (!same_format(&frame->format, &audio.adapter.format) || frames > BT_AUDIO_PCM_FRAMES ||
        frame->bytes != frames * 2U) result = AE_INVALID;
    else if (!audio.public.microphone_write_ready) result = route_live_locked(route) &&
        !audio.public.microphone_muted && audio.microphone.count == audio.microphone.capacity ? AE_AGAIN : AE_UNAVAILABLE;
    else {
        bt_audio_pcm_t packet = {.generation = audio.public.generation, .timestamp_us = now,
            .source = BT_AUDIO_HFP, .sample_rate = frame->format.sample_rate_hz, .frames = (uint16_t)frames};
        for (uint32_t i = 0; i < frames; ++i) {
            uint16_t value = (uint16_t)((uint16_t)frame->data[i * 2U] | ((uint16_t)frame->data[i * 2U + 1U] << 8));
            packet.samples[i] = value <= INT16_MAX ? (int16_t)value : (int16_t)((int32_t)value - 65536);
        }
        if (!push_ring(&audio.microphone, &packet)) result = AE_AGAIN;
        else {
            audio.microphone_kick = true; audio.adapter.tx_seen = true;
            audio.adapter.tx_sequence = frame->sequence;
        }
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return result;
}

static ae_result_t port_bind_playback(void *context, ae_route_t route, uint64_t generation,
    const ae_route_token_t *expected, uint64_t consumer, bool enabled)
{
    (void)context;
    if (!valid_route(route) || !expected || !generation || !consumer) return AE_INVALID;
    if (!AUDIO_PROFILES_SUPPORTED) return AE_NOT_SUPPORTED;
    portENTER_CRITICAL(&audio_lock);
    ae_route_token_t current = token_locked(route);
    bool prepared = audio.adapter.prepared && !audio.adapter.stopped && audio.adapter.route == route &&
        audio.adapter.generation == generation && audio.adapter.transport_generation == audio.public.generation &&
        same_token(expected, &audio.adapter.token) && same_token(expected, &current);
    ae_result_t result = AE_OK;
    if (!enabled && (!prepared || consumer != audio.adapter.playback_consumer || !audio.adapter.playback_enabled)) {
        /* Any old binding is already invalidated. Never retarget its close to
         * the opposite direction or a replacement playback job/link. */
        result = AE_OK;
    } else if (!prepared) result = AE_STALE;
    else if (!(audio.adapter.roles & AE_SPEAKER) || !route_live_locked(route)) result = AE_UNAVAILABLE;
    else if (enabled && audio.adapter.playback_enabled && consumer == audio.adapter.playback_consumer) result = AE_OK;
    else if (enabled && consumer <= audio.adapter.playback_max_consumer) result = AE_STALE;
    else if (audio.adapter.playback_epoch == UINT64_MAX) {
        error_locked("Bluetooth playback binding exhausted", ESP_ERR_INVALID_STATE, true); result = AE_EXHAUSTED;
    } else {
        ++audio.adapter.playback_epoch;
        clear_ring(&audio.music); clear_ring(&audio.voice);
        audio.adapter.partial_frames = 0; memset(audio.adapter.partial, 0, sizeof(audio.adapter.partial));
        audio.adapter.sequence = audio.adapter.sample_offset = 0;
        audio.adapter.playback_enabled = enabled;
        audio.adapter.playback_consumer = consumer;
        if (enabled) audio.adapter.playback_max_consumer = consumer;
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return result;
}

static ae_result_t port_stop(void *context, ae_route_t route, uint64_t generation,
                            const ae_route_token_t *expected)
{
    (void)context;
    if (!valid_route(route) || !generation || !expected) return AE_INVALID;
    portENTER_CRITICAL(&audio_lock);
    ae_result_t result = AE_OK;
    if (audio.adapter.route != route || audio.adapter.generation != generation ||
        !same_token(expected, &audio.adapter.token)) result = AE_STALE;
    else {
        /* The old reservation cannot stop a replacement route. Every PCM
         * callback copies/commits under this lock and rechecks its generation. */
        if (audio.adapter.transport_generation == audio.public.generation) invalidate_locked(true);
        audio.adapter.prepared = false; audio.adapter.stopped = true;
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return result;
}

ae_route_port_t bt_audio_route_port(void)
{
    return (ae_route_port_t){.capabilities = port_capabilities, .prepare = port_prepare,
        .capture = port_capture, .stop = port_stop, .bind_playback = port_bind_playback};
}

ae_result_t bt_audio_receive(ae_frame_t *frame)
{
    if (!frame) return AE_INVALID;
    if (!AUDIO_PROFILES_SUPPORTED) return AE_NOT_SUPPORTED;
    portENTER_CRITICAL(&audio_lock);
    ae_result_t result = AE_AGAIN;
    ae_route_token_t token = token_locked(audio.adapter.route);
    uint64_t now = now_us();
    if (!audio.adapter.prepared || audio.adapter.stopped || !(audio.adapter.roles & AE_SPEAKER)) result = AE_UNAVAILABLE;
    else if (audio.adapter.transport_generation != audio.public.generation ||
        !same_token(&token, &audio.adapter.token)) result = AE_STALE;
    else if (!audio.adapter.playback_enabled || !audio.adapter.playback_consumer) result = AE_UNAVAILABLE;
    else if (!audio.public.speaker_read_ready) result = AE_UNAVAILABLE;
    else {
        if (audio.adapter.partial_frames && (audio.adapter.partial_started_us > now ||
            now - audio.adapter.partial_started_us > BT_AUDIO_PCM_MAX_AGE_US)) {
            count_one(&audio.public.expired_packets);
            invalidate_locked(true); refresh_locked();
            portEXIT_CRITICAL(&audio_lock); return AE_TIMEOUT;
        }
        pcm_ring_t *ring = audio.adapter.route == AE_ROUTE_HFP_HF ? &audio.voice : &audio.music;
        expire_ring_locked(ring, now);
        if (audio.adapter.transport_generation != audio.public.generation) {
            portEXIT_CRITICAL(&audio_lock); return AE_STALE;
        }
        uint16_t wanted = (uint16_t)(audio.adapter.format.sample_rate_hz / 100U);
        /* At most four queued HFP packets/eight SBC packets per call. */
        while (audio.adapter.partial_frames < wanted && ring->count) {
            const bt_audio_pcm_t *packet = &ring->slots[ring->head];
            if (!packet->consumer_generation || packet->consumer_generation != audio.adapter.playback_consumer) {
                clear_ring(ring); audio.adapter.partial_frames = 0; audio.adapter.playback_enabled = false;
                portEXIT_CRITICAL(&audio_lock); return AE_STALE;
            }
            if (!audio.adapter.partial_frames) audio.adapter.partial_started_us = packet->timestamp_us;
            memcpy(audio.adapter.partial + audio.adapter.partial_frames, packet->samples,
                packet->frames * sizeof(int16_t));
            audio.adapter.partial_frames = (uint16_t)(audio.adapter.partial_frames + packet->frames);
            pop_ring(ring);
        }
        if (wanted && audio.adapter.partial_frames >= wanted) {
            if (audio.adapter.sequence == UINT64_MAX || UINT64_MAX - audio.adapter.sample_offset < wanted) {
                invalidate_locked(true); result = AE_EXHAUSTED;
            } else {
                *frame = (ae_frame_t){.audio_generation = audio.adapter.generation,
                    .consumer_generation = audio.adapter.playback_consumer,
                    .sequence = ++audio.adapter.sequence, .sample_offset = audio.adapter.sample_offset,
                    .route_token = audio.adapter.token, .format = audio.adapter.format, .bytes = (uint16_t)(wanted * 2U)};
                for (uint16_t i = 0; i < wanted; ++i) {
                    uint16_t value = (uint16_t)audio.adapter.partial[i];
                    frame->data[i * 2U] = (uint8_t)value; frame->data[i * 2U + 1U] = (uint8_t)(value >> 8);
                }
                audio.adapter.sample_offset += wanted;
                audio.adapter.partial_frames = (uint16_t)(audio.adapter.partial_frames - wanted);
                memmove(audio.adapter.partial, audio.adapter.partial + wanted,
                    audio.adapter.partial_frames * sizeof(int16_t));
                result = AE_OK;
            }
        }
    }
    refresh_locked(); portEXIT_CRITICAL(&audio_lock);
    return result;
}
