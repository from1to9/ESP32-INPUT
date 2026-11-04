// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bt_audio.h"
#include "btc_av.h"
#include "btc_hf_client.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_hf_client_api.h"
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"
#include "freertos/timers.h"

/* Existing state-transition assertions exercise copy-out and legacy callers. */
static bt_audio_bond_list_t observed_bonds(void)
{
    bt_audio_bond_list_t copy, legacy;
    memset(&copy, 0xa5, sizeof(copy));
    bt_audio_copy_bond_list(NULL);
    bt_audio_copy_bond_list(&copy);
    legacy = bt_audio_bond_list();
    assert(copy.known == legacy.known && copy.refreshing == legacy.refreshing &&
        copy.error == legacy.error && copy.revision == legacy.revision &&
        copy.count == legacy.count && copy.capacity == legacy.capacity);
    assert(memcmp(copy.bonds, legacy.bonds, sizeof(copy.bonds)) == 0);
    /* The caller owns this buffer; changing it must not alter cached state. */
    copy.bonds[0].address[0] ^= 0xffU;
    bt_audio_copy_bond_list(&copy);
    assert(memcmp(copy.bonds, legacy.bonds, sizeof(copy.bonds)) == 0);
    return copy;
}

static uint64_t clock_us = 1000000;
static void (*clock_hook)(void), (*after_status)(void), (*before_lock)(void);
static bool no_memory, stack_enabled = true;
static unsigned calls, kicks, fail_call;
static esp_err_t command_error, name_error;
static unsigned name_requests;
static esp_hf_client_cb_t hf;
static esp_a2d_cb_t a2;
static esp_bt_gap_cb_t gap;
static esp_bt_gap_profile_status_t gap_status;
static bool pairing_claim, deny_claim, security_accept, drop_scan;
static unsigned security_replies, claim_calls, le_closes, discoverable_writes;
static bool le_window, resource_current = true;
static int bond_count = 1, shared_count = 1;
static esp_err_t bond_list_error;
static void (*bond_count_hook)(void);
static void (*hf_rx)(const uint8_t *, uint32_t), (*a2_rx)(const uint8_t *, uint32_t);
static uint32_t (*hf_tx)(uint8_t *, uint32_t);
static void (*deadline)(TimerHandle_t);
static void *allocation;
static const uint8_t peer[6] = {2, 3, 4, 5, 6, 7};

static const uint8_t other[6] = {8, 9, 10, 11, 12, 13};

int64_t esp_timer_get_time(void) { if (clock_hook) { void (*hook)(void) = clock_hook; clock_hook = NULL; hook(); } return (int64_t)clock_us; }
void esp_fill_random(void *buffer, size_t bytes) { memset(buffer, 0xa5, bytes); }
esp_bt_controller_status_t esp_bt_controller_get_status(void)
{ return stack_enabled ? ESP_BT_CONTROLLER_STATUS_ENABLED : ESP_BT_CONTROLLER_STATUS_IDLE; }
esp_bluedroid_status_t esp_bluedroid_get_status(void)
{ return stack_enabled ? ESP_BLUEDROID_STATUS_ENABLED : ESP_BLUEDROID_STATUS_UNINITIALIZED; }
void *heap_caps_calloc_prefer(size_t count, size_t bytes, size_t alternatives, ...)
{ assert(alternatives == 2); allocation = no_memory ? NULL : calloc(count, bytes); return allocation; }
TimerHandle_t xTimerCreate(const char *name, TickType_t period, int reload, void *id,
                          void (*callback)(TimerHandle_t))
{ (void)name; (void)id; assert(period == 20 && reload); deadline = callback; return &deadline; }
int xTimerStart(TimerHandle_t timer, TickType_t timeout)
{ assert(timer && timeout == 0); return pdPASS; }
static esp_err_t submit(void) { return ++calls == fail_call ? ESP_FAIL : ESP_OK; }
esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t callback) { gap = callback; return submit(); }
esp_err_t esp_bt_gap_set_security_param(unsigned int parameter, void *value, uint8_t bytes)
{ assert(parameter == ESP_BT_SP_IOCAP_MODE && bytes == 1 && *(uint8_t *)value == ESP_BT_IO_CAP_NONE); return submit(); }
esp_err_t esp_bt_gap_get_device_name(void) { ++name_requests; return name_error; }
int esp_bt_gap_get_bond_device_num(void) { return bond_count; }
int btc_storage_get_num_all_bond_devices(void)
{ if (bond_count_hook) { void (*hook)(void) = bond_count_hook; bond_count_hook = NULL; hook(); } return shared_count; }
esp_err_t esp_bt_gap_get_bond_device_list(int *count, esp_bd_addr_t *list)
{ assert(*count >= bond_count); *count = bond_count; if (bond_count >= 1) memcpy(list[0], peer, 6); if (bond_count == 2) memcpy(list[1], other, 6); return bond_list_error; }
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t connection, esp_bt_discovery_mode_t discovery)
{ if (discovery == ESP_BT_GENERAL_DISCOVERABLE) ++discoverable_writes; if (!drop_scan) gap_status = (esp_bt_gap_profile_status_t){.conn_mode = connection, .disc_mode = discovery}; return ESP_OK; }
esp_err_t esp_bt_gap_get_profile_status(esp_bt_gap_profile_status_t *status) { *status = gap_status; if (after_status) { clock_hook = after_status; after_status = NULL; } return ESP_OK; }
esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t address, bool accept, uint8_t bytes, esp_bt_pin_code_t pin)
{ (void)address; (void)pin; assert(!accept && bytes == 0); security_accept = accept; ++security_replies; return ESP_OK; }
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t address, bool accept)
{ assert(memcmp(address, peer, 6) == 0); security_accept = accept; ++security_replies; return ESP_OK; }
esp_err_t esp_bt_gap_ssp_passkey_reply(esp_bd_addr_t address, bool accept, uint32_t passkey)
{ (void)address; assert(!accept && passkey == 0); security_accept = accept; ++security_replies; return ESP_OK; }
void ble_hid_pairing_close(void) { assert(pairing_claim); ++le_closes; le_window = false; }
bool ble_hid_pairing_claim_classic(bool reviewed_handoff)
{ ++claim_calls; if (deny_claim || pairing_claim || (le_window && !reviewed_handoff)) return false; pairing_claim = true; return true; }
void ble_hid_pairing_release_classic(void) { assert(pairing_claim); pairing_claim = false; }
esp_err_t esp_hf_client_register_callback(esp_hf_client_cb_t callback) { hf = callback; return submit(); }
esp_err_t esp_hf_client_init(void) { return submit(); }
esp_err_t esp_hf_client_register_data_callback(void (*rx)(const uint8_t *, uint32_t),
                                              uint32_t (*tx)(uint8_t *, uint32_t))
{ hf_rx = rx; hf_tx = tx; return submit(); }
esp_err_t esp_a2d_register_callback(esp_a2d_cb_t callback) { a2 = callback; return submit(); }
esp_err_t esp_a2d_sink_register_data_callback(void (*rx)(const uint8_t *, uint32_t))
{ a2_rx = rx; return submit(); }
esp_err_t esp_a2d_sink_init(void) { return submit(); }
static bool audio_locked, on_btc, defer_btc;
static void (*queued_btc)(void *), (*before_post)(void), (*inside_handler)(void);
static unsigned dispatch_calls;
static uint8_t last_pid, last_action;
struct osi_thread { int queue; };
static osi_thread_t btc;
bool g_a2dp_on_deinit, g_a2dp_sink_ongoing_deinit;
void test_bt_lock(portMUX_TYPE *lock)
{
    (void)lock; assert(!audio_locked);
    if (before_lock) { void (*inject)(void) = before_lock; before_lock = NULL; inject(); }
    audio_locked = true;
}
void test_bt_unlock(portMUX_TYPE *lock) { (void)lock; assert(audio_locked); audio_locked = false; }
osi_thread_t *btc_get_current_thread(void) { return &btc; }
static void run_btc(void)
{
    assert(queued_btc && !on_btc);
    void (*run)(void *) = queued_btc; queued_btc = NULL;
    on_btc = true; run(NULL); on_btc = false;
}
bool osi_thread_post(osi_thread_t *owner, void (*run)(void *), void *context, int queue, uint32_t timeout)
{
    assert(!audio_locked && owner == &btc && !context && queue == 0 && timeout == 0);
    if (before_post) { void (*hook)(void) = before_post; before_post = NULL; hook(); }
    if (queued_btc || command_error) return false;
    queued_btc = run;
    if (!defer_btc) run_btc();
    return true;
}
int btc_inter_profile_call(btc_msg_t *message)
{
    assert(on_btc && !audio_locked && message->sig == BTC_SIG_API_CALL);
    assert(message->pid == BTC_PID_HF_CLIENT || message->pid == BTC_PID_A2DP);
    assert(!memcmp(message->arg, peer, sizeof(peer)));
    ++calls; ++dispatch_calls;
    last_pid = message->pid; last_action = message->act;
    if (inside_handler) { void (*hook)(void) = inside_handler; inside_handler = NULL; hook(); }
    return BT_STATUS_SUCCESS;
}
void esp_hf_client_outgoing_data_ready(void) { ++kicks; }

static void initialized(void)
{
    esp_hf_client_cb_param_t h = {.prof_stat.state = ESP_HF_INIT_SUCCESS};
    hf(ESP_HF_CLIENT_PROF_STATE_EVT, &h);
    assert(!bt_audio_snapshot().initialized);
    esp_a2d_cb_param_t a = {.a2d_prof_stat.init_state = ESP_A2D_INIT_SUCCESS};
    a2(ESP_A2D_PROF_STATE_EVT, &a);
    assert(bt_audio_snapshot().initialized);
}
static void voice(esp_hf_client_audio_state_t state)
{
    esp_hf_client_cb_param_t h = {.audio_stat.state = state};
    memcpy(h.audio_stat.remote_bda, peer, 6); hf(ESP_HF_CLIENT_AUDIO_STATE_EVT, &h);
}
static void hf_connection(esp_hf_client_connection_state_t state)
{
    esp_hf_client_cb_param_t h = {.conn_stat.state = state};
    memcpy(h.conn_stat.remote_bda, peer, 6); hf(ESP_HF_CLIENT_CONNECTION_STATE_EVT, &h);
}
static void music_connection(esp_a2d_connection_state_t state, uint16_t handle)
{
    esp_a2d_cb_param_t a = {.conn_stat = {.state = state, .conn_hdl = handle}};
    memcpy(a.conn_stat.remote_bda, peer, 6); a2(ESP_A2D_CONNECTION_STATE_EVT, &a);
}
static void music_format(uint8_t rate, uint8_t channels, uint16_t handle)
{
    esp_a2d_cb_param_t a = {.audio_cfg = {.conn_hdl = handle,
        .mcc = {.type = ESP_A2D_MCT_SBC, .cie.sbc_info = {.samp_freq = rate, .ch_mode = channels}}}};
    memcpy(a.audio_cfg.remote_bda, peer, 6); a2(ESP_A2D_AUDIO_CFG_EVT, &a);
}
static void music_state(esp_a2d_audio_state_t state, uint16_t handle)
{
    esp_a2d_cb_param_t a = {.audio_stat = {.state = state, .conn_hdl = handle}};
    memcpy(a.audio_stat.remote_bda, peer, 6); a2(ESP_A2D_AUDIO_STATE_EVT, &a);
}
static uint64_t policy(bt_audio_route_t route, bool mic_muted, bool speaker_muted)
{
    uint64_t generation;
    assert(bt_audio_set_policy(bt_audio_snapshot().generation, route, mic_muted, speaker_muted, &generation) == ESP_OK);
    return generation;
}
static ae_route_capabilities_t caps(ae_route_port_t port, ae_route_t route)
{
    ae_route_capabilities_t value;
    assert(port.capabilities(NULL, route, &value) == AE_OK); return value;
}
static void normal(void)
{
    initialized();
    assert(bt_audio_init() == ESP_ERR_INVALID_STATE);
    assert(!observed_bonds().known);
    bt_audio_service();
    bt_audio_bond_list_t saved = observed_bonds();
    assert(saved.known && saved.count == 1 && !saved.bonds[0].name_known && saved.bonds[0].name[0] == 0 &&
        memcmp(saved.bonds[0].address, peer, 6) == 0);
    uint64_t request;
    bt_audio_snapshot_t target = bt_audio_snapshot();
    assert(bt_audio_saved_request(target.generation, target.hfp_link_generation, saved.revision + 1, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_ERR_INVALID_STATE);
    uint8_t absent[6] = {9};
    assert(bt_audio_saved_request(target.generation, target.hfp_link_generation, saved.revision, BT_AUDIO_CONNECT_HFP, absent, NULL) == ESP_ERR_NOT_FOUND);
    assert(bt_audio_saved_request(target.generation, target.hfp_link_generation, saved.revision, BT_AUDIO_CONNECT_HFP, peer, &request) == ESP_OK);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
    bt_audio_service(); assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_WAITING);
    bt_audio_cancel(); assert(bt_audio_snapshot().control_inflight);
    target = bt_audio_snapshot();
    assert(bt_audio_request(target.generation, target.hfp_link_generation, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_ERR_INVALID_STATE);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    assert(observed_bonds().bonds[0].hfp_current && !observed_bonds().bonds[0].a2dp_current);
    assert(!bt_audio_snapshot().control_inflight && bt_audio_snapshot().request_status == BT_AUDIO_JOB_CANCELLED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    assert(bt_audio_snapshot().hfp_sample_rate == 16000 && bt_audio_snapshot().microphone_muted);
    uint64_t generation = policy(BT_AUDIO_HFP, false, false);
    bt_audio_pcm_t packet = {.generation = generation, .timestamp_us = clock_us,
        .source = BT_AUDIO_HFP, .sample_rate = 16000, .frames = 160};
    for (unsigned i = 0; i < packet.frames; ++i) packet.samples[i] = (int16_t)((int)i - 80);
    uint8_t pcm[960] = {0};
    assert(hf_tx(pcm, 320) == 0 && bt_audio_snapshot().transmit_underflows == 1);
    for (unsigned i = 0; i < 4; ++i) assert(bt_audio_write_microphone(&packet) == ESP_OK);
    assert(bt_audio_write_microphone(&packet) == ESP_ERR_NO_MEM);
    bt_audio_service(); assert(kicks == 1); /* Full queue still kicks. */
    assert(hf_tx(pcm, 3) == 0 && hf_tx(pcm, 320) == 320);
    assert(pcm[0] == 0xb0 && pcm[1] == 0xff && pcm[318] == 79 && pcm[319] == 0);
    assert(bt_audio_snapshot().microphone_queued_frames == 480);
    clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1;
    packet.timestamp_us = clock_us;
    assert(bt_audio_write_microphone(&packet) == ESP_OK); /* Expiry refreshes readiness. */
    assert(bt_audio_snapshot().expired_packets == 3);
    hf_rx(pcm, 320); assert(bt_audio_read_speaker(generation, &packet));
    assert(packet.frames == 160 && packet.samples[0] == -80);
    hf_rx(NULL, 2); assert(bt_audio_snapshot().malformed_packets == 2);
    generation = policy(BT_AUDIO_HFP, false, false);
    for (unsigned i = 0; i < 5; ++i) hf_rx(pcm, 320);
    assert(bt_audio_snapshot().receive_overruns == 1);
    uint64_t old = generation; generation = bt_audio_cancel();
    assert(!bt_audio_read_speaker(old, &packet) && hf_tx(pcm, 320) == 0);
    assert(bt_audio_snapshot().microphone_queued_frames == 0 && generation != old);

    ae_route_port_t port = bt_audio_route_port();
    ae_route_capabilities_t h = caps(port, AE_ROUTE_HFP_HF);
    assert(h.connected && !h.qualified && h.full_duplex && h.capture && h.playback && h.token.link_generation);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 100, &h.token) == AE_OK);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 100, &h.token) == AE_OK);
    ae_frame_t frame;
    hf_rx(pcm, 320); assert(bt_audio_receive(&frame) == AE_UNAVAILABLE);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &h.token, 1, true) == AE_OK);
    hf_rx(pcm, 240); assert(bt_audio_receive(&frame) == AE_AGAIN);
    hf_rx(pcm, 240); assert(bt_audio_receive(&frame) == AE_OK);
    assert(frame.bytes == 320 && frame.sequence == 1 && frame.sample_offset == 0 &&
        frame.audio_generation == 100 && frame.consumer_generation == 1 &&
        memcmp(&frame.route_token, &h.token, sizeof(h.token)) == 0);
    frame.consumer_generation = 1;
    assert(port.capture(NULL, AE_ROUTE_HFP_HF, &frame) == AE_OK);
    assert(port.capture(NULL, AE_ROUTE_HFP_HF, &frame) == AE_STALE);
    clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1;
    assert(bt_audio_receive(&frame) == AE_TIMEOUT); /* Partial chunks cannot bridge an aged gap. */
    assert(hf_tx(pcm, 320) == 0);
    assert(port.stop(NULL, AE_ROUTE_HFP_HF, 100, &h.token) == AE_OK);
    assert(port.stop(NULL, AE_ROUTE_HFP_HF, 100, &h.token) == AE_OK);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 100, &h.token) == AE_STALE);
    voice(ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED); voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 101, &h.token) == AE_STALE);
    assert(port.capture(NULL, AE_ROUTE_HFP_HF, &frame) != AE_OK);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_LC3);
    assert(!caps(port, AE_ROUTE_HFP_HF).connected);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED);
    assert(caps(port, AE_ROUTE_HFP_HF).format.sample_rate_hz == 8000);

    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 42);
    music_format(ESP_A2D_SBC_CIE_SF_48K, ESP_A2D_SBC_CIE_CH_MODE_STEREO, 42);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 42);
    ae_route_capabilities_t a = caps(port, AE_ROUTE_A2DP_SINK);
    assert(a.connected && a.format.sample_rate_hz == 48000 && !a.capture && !a.full_duplex);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &a.format, 1, 102, &a.token) == AE_NOT_SUPPORTED);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &a.format, 2, 102, &a.token) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 102, &a.token, 10, true) == AE_OK);
    uint8_t stereo[1920];
    for (unsigned i = 0; i < 480; ++i) {
        stereo[i * 4] = 0xff; stereo[i * 4 + 1] = 0x7f;
        stereo[i * 4 + 2] = 0; stereo[i * 4 + 3] = 0x80;
    }
    a2_rx(stereo, sizeof(stereo)); assert(bt_audio_receive(&frame) == AE_OK);
    assert(frame.bytes == 960 && frame.data[0] == 0 && frame.data[1] == 0);
    music_format(ESP_A2D_SBC_CIE_SF_16K, ESP_A2D_SBC_CIE_CH_MODE_MONO, 43); /* Foreign handle. */
    assert(caps(port, AE_ROUTE_A2DP_SINK).format.sample_rate_hz == 48000);
    music_state(ESP_A2D_AUDIO_STATE_SUSPEND, 42); music_state(ESP_A2D_AUDIO_STATE_STARTED, 42);
    assert(bt_audio_receive(&frame) == AE_STALE);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &a.format, 2, 103, &a.token) == AE_STALE);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 42);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 43);
    music_format(ESP_A2D_SBC_CIE_SF_44K, ESP_A2D_SBC_CIE_CH_MODE_MONO, 43);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 43);
    a = caps(port, AE_ROUTE_A2DP_SINK);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &a.format, 2, 104, &a.token) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 104, &a.token, 11, true) == AE_OK);
    a2_rx(pcm, 882); assert(bt_audio_receive(&frame) == AE_OK && frame.bytes == 882);
    assert(port.stop(NULL, AE_ROUTE_HFP_HF, 100, &h.token) == AE_STALE);
    assert(!bt_audio_snapshot().speaker_muted); /* Old stop cannot mute replacement. */
    a2_rx(pcm, 882); clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1; bt_audio_service();
    assert(bt_audio_receive(&frame) == AE_TIMEOUT); /* Report the gap once for this live A2DP grant. */
    assert(bt_audio_receive(&frame) == AE_AGAIN);
    music_format(3, ESP_A2D_SBC_CIE_CH_MODE_MONO, 43);
    assert(!bt_audio_snapshot().a2dp_format_known);
    assert(bt_audio_refresh_bonds() == ESP_OK && observed_bonds().refreshing);
    target = bt_audio_snapshot();
    assert(bt_audio_saved_request(target.generation, target.a2dp_link_generation, observed_bonds().revision,
        BT_AUDIO_DISCONNECT_A2DP, peer, NULL) == ESP_ERR_INVALID_STATE);
    bond_list_error = ESP_FAIL; bt_audio_service();
    saved = observed_bonds(); assert(!saved.known && saved.error == ESP_FAIL && saved.count == 1);
    bond_list_error = ESP_OK; assert(bt_audio_refresh_bonds() == ESP_OK); bt_audio_service();
    saved = observed_bonds(); assert(saved.known && saved.bonds[0].a2dp_current);
    unsigned prior_calls = calls;
    target = bt_audio_snapshot();
    assert(bt_audio_saved_request(target.generation, target.a2dp_link_generation, saved.revision,
        BT_AUDIO_DISCONNECT_A2DP, peer, NULL) == ESP_OK);
    assert(bt_audio_refresh_bonds() == ESP_OK); bt_audio_service();
    assert(calls == prior_calls + 1 && bt_audio_snapshot().request_status == BT_AUDIO_JOB_WAITING);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 43);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 44);
    saved = observed_bonds(); prior_calls = calls;
    target = bt_audio_snapshot();
    assert(bt_audio_saved_request(target.generation, target.a2dp_link_generation, saved.revision,
        BT_AUDIO_DISCONNECT_A2DP, peer, NULL) == ESP_OK);
    esp_bt_gap_cb_param_t mutation = {.auth_cmpl.stat = 1};
    memcpy(mutation.auth_cmpl.bda, peer, 6); gap(ESP_BT_GAP_AUTH_CMPL_EVT, &mutation);
    bt_audio_service();
    assert(calls == prior_calls && bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED);
}

static ae_route_port_t interrupt_port;
static ae_route_token_t interrupt_token;
static void publish_music_packet(void)
{
    uint8_t stereo[441 * 4] = {0};
    ++clock_us; a2_rx(stereo, sizeof(stereo));
}
static void schedule_music_packet(void) { before_lock = publish_music_packet; }
static void expiry_observation_race(void)
{
    initialized(); bt_audio_service();
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    music_format(ESP_A2D_SBC_CIE_SF_44K, ESP_A2D_SBC_CIE_CH_MODE_STEREO, 3);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    ae_route_port_t port = bt_audio_route_port();
    ae_route_capabilities_t route = caps(port, AE_ROUTE_A2DP_SINK);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &route.format, AE_SPEAKER, 11, &route.token) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 11, &route.token, 12, true) == AE_OK);
    /* Publish on the next lock acquisition after the service clock sample.
     * If that sample already owns the lock, publication follows its pass. */
    after_status = schedule_music_packet;
    bt_audio_service();
    ae_frame_t frame;
    assert(bt_audio_receive(&frame) == AE_OK && frame.bytes == 882 && !bt_audio_snapshot().expired_packets);
}

static ae_frame_t concurrent_capture;

static void capture_before_microphone_lock(void)
{
    ++clock_us;
    ae_route_port_t port = bt_audio_route_port();
    assert(port.capture(NULL, AE_ROUTE_HFP_HF, &concurrent_capture) == AE_OK);
}

static void hfp_queue_clock(bool writing)
{
    initialized(); bt_audio_service();
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    ae_route_port_t port = bt_audio_route_port();
    ae_route_capabilities_t route = caps(port, AE_ROUTE_HFP_HF);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &route.format, AE_MICROPHONE, 11, &route.token) == AE_OK);
    concurrent_capture = (ae_frame_t){.audio_generation = 11, .consumer_generation = 12,
        .sequence = 1, .route_token = route.token, .format = route.format, .bytes = 320};
    for (unsigned i = 0; i < concurrent_capture.bytes; ++i) concurrent_capture.data[i] = (uint8_t)i;
    bt_audio_pcm_t packet = {.source = BT_AUDIO_HFP, .frames = 160, .sample_rate = 16000,
        .generation = bt_audio_snapshot().generation, .timestamp_us = clock_us};
    for (unsigned i = 0; i < packet.frames; ++i) packet.samples[i] = (int16_t)(1000U + i);
    uint8_t pcm[320];
    before_lock = capture_before_microphone_lock;
    if (writing) assert(bt_audio_write_microphone(&packet) == ESP_OK);
    uint32_t bytes = hf_tx(pcm, sizeof(pcm));
    bt_audio_snapshot_t state = bt_audio_snapshot();
    printf("HFP %s: bytes=%u expired=%u underflows=%u\n", writing ? "write" : "transmit",
        (unsigned)bytes, (unsigned)state.expired_packets, (unsigned)state.transmit_underflows);
    fflush(stdout);
    assert(!before_lock && bytes == sizeof(pcm) && !memcmp(pcm, concurrent_capture.data, sizeof(pcm)));
    assert(!state.expired_packets && !state.transmit_underflows);
    if (writing) {
        assert(hf_tx(pcm, sizeof(pcm)) == sizeof(pcm));
        for (unsigned i = 0; i < packet.frames; ++i) {
            uint16_t sample = (uint16_t)(pcm[2U * i] | (uint16_t)pcm[2U * i + 1U] << 8);
            assert(sample == (uint16_t)packet.samples[i]);
        }
    }

    packet.timestamp_us = clock_us + 1;
    assert(bt_audio_write_microphone(&packet) == ESP_ERR_TIMEOUT);
    packet.timestamp_us = clock_us;
    assert(bt_audio_write_microphone(&packet) == ESP_OK);
    clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1;
    assert(bt_audio_write_microphone(&packet) == ESP_ERR_TIMEOUT);
    assert(hf_tx(pcm, sizeof(pcm)) == 0);
    state = bt_audio_snapshot();
    assert(state.expired_packets == 1 && state.transmit_underflows == 1 && !state.microphone_queued_frames);
}

static void sustained_sbc(void)
{
    initialized(); bt_audio_service();
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    music_format(ESP_A2D_SBC_CIE_SF_44K, ESP_A2D_SBC_CIE_CH_MODE_STEREO, 3);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    ae_route_port_t port = bt_audio_route_port();
    ae_route_capabilities_t route = caps(port, AE_ROUTE_A2DP_SINK);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &route.format, AE_SPEAKER, 11, &route.token) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 11, &route.token, 12, true) == AE_OK);

    /* The pinned sink delivers one callback per decoded media packet, and
     * drains queued packets back to back. Use eight 16-block/8-subband SBC
     * frames per packet. Two such callbacks fit within eight receive slots. */
    enum { samples_per_packet = 8 * 16 * 8, batch = 2, ticks = 200 };
    uint8_t stereo[samples_per_packet * 4];
    uint32_t produced = 0, consumed = 0;
    uint64_t start = clock_us;
    ae_frame_t frame;
    for (unsigned tick = 0; tick < ticks; ++tick) {
        clock_us = start + tick * 10000U;
        if (produced <= tick * 441U) {
            for (unsigned n = 0; n < batch; ++n) {
                for (unsigned i = 0; i < samples_per_packet; ++i) {
                    uint16_t sample = (uint16_t)(produced + i);
                    stereo[i*4] = stereo[i*4+2] = (uint8_t)sample;
                    stereo[i*4+1] = stereo[i*4+3] = (uint8_t)(sample >> 8);
                }
                a2_rx(stereo, sizeof(stereo)); produced += samples_per_packet;
            }
        }
        bt_audio_service();
        for (unsigned i = 0; i < 4; ++i) {
            ae_result_t result = bt_audio_receive(&frame);
            if (result == AE_AGAIN) break;
            if (result != AE_OK) {
                bt_audio_snapshot_t state = bt_audio_snapshot();
                fprintf(stderr, "SBC stream stopped at %u ms: result=%d samples=%u/%u expired=%u overruns=%u\n",
                    tick * 10, result, consumed, produced, state.expired_packets, state.receive_overruns);
            }
            assert(result == AE_OK && frame.bytes == 882 && frame.sample_offset == consumed &&
                frame.sequence == consumed / 441U + 1 && frame.consumer_generation == 12);
            for (unsigned j = 0; j < 441; ++j) {
                uint16_t sample = (uint16_t)(frame.data[j*2] | (uint16_t)frame.data[j*2+1] << 8);
                assert(sample == (uint16_t)consumed); ++consumed;
            }
        }
    }
    bt_audio_a2dp_rx_t rx;
    bt_audio_a2dp_rx_snapshot(&rx);
    assert(consumed >= (ticks - 1U) * 441U && rx.known && rx.connected && rx.bound && rx.starts == 1);
    assert(rx.callbacks == produced / samples_per_packet && rx.callback_bytes == produced * 4U &&
        rx.largest_callback_bytes == sizeof(stereo) && rx.enqueued_samples == produced && rx.emitted_samples == consumed &&
        !rx.gated_callbacks && !rx.expired && !rx.overruns && !rx.malformed);
    assert(produced > consumed && produced - consumed < 441);
    clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1;
    assert(bt_audio_receive(&frame) == AE_TIMEOUT); /* A genuinely old unconsumed suffix still expires. */
    uint32_t discarded = produced - consumed;
    memset(stereo, 0x35, sizeof(stereo));
    a2_rx(stereo, sizeof(stereo));
    assert(bt_audio_receive(&frame) == AE_OK && frame.consumer_generation == 12 &&
        frame.sample_offset == consumed && frame.sequence == consumed / 441U + 1U);
    for (unsigned j = 0; j < frame.bytes; ++j) assert(frame.data[j] == 0x35);
    consumed += 441;
    produced += samples_per_packet;
    music_state(ESP_A2D_AUDIO_STATE_SUSPEND, 3);
    a2_rx(stereo, sizeof(stereo));
    bt_audio_a2dp_rx_snapshot(&rx);
    assert(!rx.bound && !rx.streaming && rx.stops == 1 && rx.expired == 1 && rx.gated_callbacks == 1 &&
        rx.partial_expiries == 1 && rx.gap_discarded_samples == discarded &&
        rx.enqueued_samples == produced && rx.emitted_samples == consumed && rx.peak_slots <= 8);

    /* Three undrained 1024-sample callbacks require nine packet slots.
     * The actual eight-slot overflow must still revoke the new binding. */
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    route = caps(port, AE_ROUTE_A2DP_SINK);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &route.format, AE_SPEAKER, 12, &route.token) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 12, &route.token, 13, true) == AE_OK);
    for (unsigned i = 0; i < 3; ++i) a2_rx(stereo, sizeof(stereo));
    bt_audio_a2dp_rx_snapshot(&rx);
    assert(!rx.bound && rx.streaming && rx.peak_slots == 8 && rx.overruns == 1 && !rx.queued_samples &&
        rx.emitted_samples == consumed && bt_audio_receive(&frame) == AE_STALE);
}

static void a2dp_queued_gap(void)
{
    initialized(); bt_audio_service();
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    music_format(ESP_A2D_SBC_CIE_SF_44K, ESP_A2D_SBC_CIE_CH_MODE_MONO, 3);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    ae_route_port_t port = bt_audio_route_port();
    ae_route_capabilities_t route = caps(port, AE_ROUTE_A2DP_SINK);
    assert(port.prepare(NULL, AE_ROUTE_A2DP_SINK, &route.format, AE_SPEAKER, 11, &route.token) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 11, &route.token, 12, true) == AE_OK);
    ae_frame_t frame;
    uint8_t pcm[882] = {0};
    a2_rx(pcm, 128);
    assert(bt_audio_receive(&frame) == AE_AGAIN); /* 64 old samples in assembly. */
    a2_rx(pcm, sizeof(pcm));                    /* 441 old samples in the ring. */
    clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1;
    memset(pcm, 0x25, sizeof(pcm));
    a2_rx(pcm, sizeof(pcm));                    /* Fresh packet behind the aged head. */
    bt_audio_service();
    assert(bt_audio_receive(&frame) == AE_TIMEOUT);
    assert(bt_audio_receive(&frame) == AE_OK && frame.sequence == 1 && !frame.sample_offset &&
        frame.consumer_generation == 12 && frame.bytes == sizeof(pcm) && !memcmp(frame.data, pcm, sizeof(pcm)));
    bt_audio_a2dp_rx_t rx;
    bt_audio_a2dp_rx_snapshot(&rx);
    assert(rx.bound && rx.expired == 1 && !rx.partial_expiries && rx.gap_discarded_samples == 505 &&
        rx.callback_intervals == 2 && rx.max_callback_gap_us == BT_AUDIO_PCM_MAX_AGE_US + 1);

    a2_rx(pcm, sizeof(pcm));
    clock_us += BT_AUDIO_PCM_MAX_AGE_US + 1;
    bt_audio_service(); /* The consumer closes before observing this break. */
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 11, &route.token, 12, false) == AE_OK);
    a2_rx(pcm, sizeof(pcm));
    assert(bt_audio_receive(&frame) == AE_UNAVAILABLE);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 11, &route.token, 12, true) == AE_STALE);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 11, &route.token, 13, true) == AE_OK);
    assert(bt_audio_receive(&frame) == AE_AGAIN);
    a2_rx(pcm, sizeof(pcm)); --clock_us; /* A future stamp still revokes the binding. */
    assert(bt_audio_receive(&frame) == AE_STALE);
    bt_audio_a2dp_rx_snapshot(&rx);
    assert(!rx.bound && !rx.callback_intervals && !rx.max_callback_gap_us);
}

static void replace_playback_during_pcm(void) {
    assert(interrupt_port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &interrupt_token, 40, true) == AE_OK);
}
static void schedule_playback_replacement(void) { before_lock = replace_playback_during_pcm; }

static void playback_binding_test(void)
{
    initialized(); bt_audio_service(); hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    ae_route_port_t port = bt_audio_route_port(); ae_route_capabilities_t current = caps(port, AE_ROUTE_HFP_HF);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &current.format, 3, 100, &current.token) == AE_OK);
    ae_frame_t playback; uint8_t pcm[320] = {0};
    hf_rx(pcm, sizeof(pcm)); assert(bt_audio_receive(&playback) == AE_UNAVAILABLE);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 10, true) == AE_OK);
    ae_frame_t capture = {.audio_generation = 100, .consumer_generation = 99, .sequence = 1,
        .route_token = current.token, .format = current.format, .bytes = 320, .data = {1}};
    assert(port.capture(NULL, AE_ROUTE_HFP_HF, &capture) == AE_OK);
    hf_rx(pcm, sizeof(pcm)); assert(bt_audio_receive(&playback) == AE_OK && playback.consumer_generation == 10);
    ae_frame_t retained = playback;
    hf_rx(pcm, 240); assert(bt_audio_receive(&playback) == AE_AGAIN);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 20, true) == AE_OK);
    assert(bt_audio_snapshot().microphone_queued_frames == 160 && !bt_audio_snapshot().microphone_muted);
    assert(bt_audio_receive(&playback) == AE_AGAIN); /* Old partial/queued input flushed. */
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 10, false) == AE_OK);
    assert(hf_tx(pcm, sizeof(pcm)) == sizeof(pcm) && pcm[0] == 1); /* Capture survives binding changes. */
    hf_rx(pcm, sizeof(pcm));
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 20, true) == AE_OK);
    assert(bt_audio_receive(&playback) == AE_OK && playback.consumer_generation == 20 && playback.sequence == 1 && playback.sample_offset == 0);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 20, false) == AE_OK);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 20, false) == AE_OK);
    hf_rx(pcm, sizeof(pcm)); assert(bt_audio_receive(&playback) == AE_UNAVAILABLE);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 10, true) == AE_STALE);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 20, true) == AE_STALE);
    assert(port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &current.token, 30, true) == AE_OK);
    interrupt_port = port; interrupt_token = current.token; clock_hook = schedule_playback_replacement;
    hf_rx(pcm, sizeof(pcm)); /* Hook runs after callback captures consumer 30, before commit. */
    assert(!clock_hook && bt_audio_receive(&playback) == AE_AGAIN);
    assert(retained.consumer_generation == 10); /* Returned frames cannot be relabelled. */
    hf_rx(pcm, sizeof(pcm)); assert(bt_audio_receive(&playback) == AE_OK && playback.consumer_generation == 40 && playback.sequence == 1 && !playback.sample_offset);
    assert(port.bind_playback(NULL, AE_ROUTE_A2DP_SINK, 99, &current.token, 30, false) == AE_OK);
    hf_rx(pcm, sizeof(pcm)); assert(bt_audio_receive(&playback) == AE_OK && playback.consumer_generation == 40 && playback.sequence == 2 && playback.sample_offset == 160);
    capture.sequence = 2; capture.sample_offset = 160; assert(port.capture(NULL, AE_ROUTE_HFP_HF, &capture) == AE_OK);
}

static void security_event(esp_bt_gap_cb_event_t event)
{
    esp_bt_gap_cb_param_t parameter = {0};
    memcpy(parameter.cfm_req.bda, peer, 6);
    gap(event, &parameter);
}

static void auth_result(const uint8_t address[6], bool success)
{
    esp_bt_gap_cb_param_t p = {.auth_cmpl.stat = success ? ESP_BT_STATUS_SUCCESS : 1U};
    memcpy(p.auth_cmpl.bda, address, 6);
    gap(ESP_BT_GAP_AUTH_CMPL_EVT, &p);
}

static void remove_result(const uint8_t address[6])
{
    esp_bt_gap_cb_param_t p = {.remove_bond_dev_cmpl.status = ESP_BT_STATUS_SUCCESS};
    memcpy(p.remove_bond_dev_cmpl.bda, address, 6);
    gap(ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT, &p);
}

static void auth_during_capacity_read(void) { auth_result(peer, false); }

static void pairing_lifecycle(const char *scenario)
{
    initialized(); bt_audio_service();
    if (!strcmp(scenario, "pair-prewindow")) {
        security_event(ESP_BT_GAP_CFM_REQ_EVT);
        esp_err_t opened = bt_audio_pairing_open(1000);
        bt_audio_service();
        assert(!security_accept && !bt_audio_snapshot().pairing_handshake_pending);
        if (opened != ESP_OK) assert(bt_audio_pairing_open(1000) == ESP_OK);
        return;
    }
    assert(bt_audio_pairing_open(1000) == ESP_OK); bt_audio_service();
    security_event(ESP_BT_GAP_CFM_REQ_EVT);
    if (!strcmp(scenario, "pair-terminal")) {
        bond_count_hook = auth_during_capacity_read; bt_audio_service();
        assert(!bt_audio_snapshot().pairing_handshake_pending && !security_replies);
        return;
    }
    bt_audio_service(); assert(security_accept);
    if (!strcmp(scenario, "pair-foreign")) {
        auth_result(other, true); bt_audio_service();
        assert(pairing_claim && bt_audio_snapshot().pairing_requested && bt_audio_snapshot().pairing_handshake_pending);
    } else if (!strncmp(scenario, "pair-late", 9)) {
        clock_us += 30000000; deadline(NULL); bt_audio_service();
        auth_result(other, false); bt_audio_service();
        assert(pairing_claim && !bt_audio_snapshot().pairing_requested && bt_audio_snapshot().pairing_handshake_pending);
        assert(bt_audio_pairing_open(1000) != ESP_OK);
    }
    bool success = !strstr(scenario, "failure");
    if (!strcmp(scenario, "pair-unsaved")) bond_count = 0;
    auth_result(peer, success); bt_audio_service();
    assert(!pairing_claim && !bt_audio_snapshot().failed && !bt_audio_snapshot().pairing_requested);
    assert(bt_audio_snapshot().pairing_completed == (success && bond_count > 0));
    assert(bt_audio_pairing_open(1000) == ESP_OK);
}

static void profile_lifecycle(const char *scenario)
{
    bond_count = 2;
    initialized(); bt_audio_service();
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    uint64_t generation = policy(BT_AUDIO_HFP, false, false);
    bt_audio_pcm_t packet = {.generation = generation, .timestamp_us = clock_us,
        .source = BT_AUDIO_HFP, .sample_rate = 16000, .frames = 160};
    packet.samples[0] = 42;
    assert(bt_audio_write_microphone(&packet) == ESP_OK);
    if (!strcmp(scenario, "foreign-bonds")) {
        auth_result(other, false); remove_result(other);
    } else {
        esp_a2d_cb_param_t a = {.conn_stat = {.state = ESP_A2D_CONNECTION_STATE_CONNECTED, .conn_hdl = 7}};
        memcpy(a.conn_stat.remote_bda, other, 6); a2(ESP_A2D_CONNECTION_STATE_EVT, &a);
        a.conn_stat.state = ESP_A2D_CONNECTION_STATE_DISCONNECTED; a2(ESP_A2D_CONNECTION_STATE_EVT, &a);
    }
    uint8_t wire[320];
    assert(hf_tx(wire, sizeof(wire)) == sizeof(wire) && wire[0] == 42);
    bt_audio_service(); assert(bt_audio_snapshot().generation == generation);
    assert(bt_audio_write_microphone(&packet) == ESP_OK);
    remove_result(peer); bond_count = 0;
    assert(hf_tx(wire, sizeof(wire)) == 0);
    bt_audio_service(); assert(!bt_audio_snapshot().classic_hfp_peer_bonded && bt_audio_snapshot().microphone_muted);
    assert(bt_audio_write_microphone(&packet) != ESP_OK);
}

static void profile_peer_switch(void)
{
    initialized(); bt_audio_service();
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED); voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    uint64_t generation = policy(BT_AUDIO_HFP, false, false);
    bt_audio_pcm_t packet = {.generation = generation, .source = BT_AUDIO_HFP, .sample_rate = 16000,
        .timestamp_us = clock_us, .frames = 160};
    assert(bt_audio_write_microphone(&packet) == ESP_OK);
    esp_hf_client_cb_param_t h = {.conn_stat.state = ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED};
    memcpy(h.conn_stat.remote_bda, other, 6); hf(ESP_HF_CLIENT_CONNECTION_STATE_EVT, &h);
    uint8_t pcm[320]; assert(hf_tx(pcm, sizeof(pcm)) == 0 && bt_audio_snapshot().microphone_muted);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED);
    h.conn_stat.state = ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED; hf(ESP_HF_CLIENT_CONNECTION_STATE_EVT, &h);
    assert(!memcmp(bt_audio_snapshot().hfp_peer, other, 6) && !bt_audio_snapshot().hfp_audio_connected);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 1);
    music_format(ESP_A2D_SBC_CIE_SF_48K, ESP_A2D_SBC_CIE_CH_MODE_MONO, 1);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 1); generation = policy(BT_AUDIO_A2DP, true, false);
    esp_a2d_cb_param_t a = {.conn_stat = {.state = ESP_A2D_CONNECTION_STATE_CONNECTING, .conn_hdl = 1}};
    memcpy(a.conn_stat.remote_bda, other, 6); a2(ESP_A2D_CONNECTION_STATE_EVT, &a);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 1); /* Old address, reused handle. */
    a.conn_stat.state = ESP_A2D_CONNECTION_STATE_CONNECTED; a2(ESP_A2D_CONNECTION_STATE_EVT, &a);
    assert(!memcmp(bt_audio_snapshot().a2dp_peer, other, 6) && !bt_audio_snapshot().a2dp_format_known);
    a2_rx(pcm, sizeof(pcm)); assert(!bt_audio_read_speaker(generation, &packet));
}

static void connect_race(void)
{
    initialized(); bt_audio_service();
    uint64_t token;
    bt_audio_snapshot_t target = bt_audio_snapshot();
    assert(bt_audio_request(target.generation, target.a2dp_link_generation, BT_AUDIO_CONNECT_A2DP, peer, &token) == ESP_OK);
    bt_audio_service();
    esp_a2d_cb_param_t a = {.conn_stat = {.state = ESP_A2D_CONNECTION_STATE_CONNECTED, .conn_hdl = 4}};
    memcpy(a.conn_stat.remote_bda, other, 6); a2(ESP_A2D_CONNECTION_STATE_EVT, &a);
    /* The SDK emits this disconnected event for the rejected outgoing peer. */
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 0);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED && !bt_audio_snapshot().control_inflight);
    assert(bt_audio_snapshot().a2dp_connected && !memcmp(bt_audio_snapshot().a2dp_peer, other, 6));
    uint64_t current;
    target = bt_audio_snapshot();
    assert(bt_audio_request(target.generation, target.hfp_link_generation, BT_AUDIO_CONNECT_HFP, peer, &current) == ESP_OK);
    bt_audio_cancel_request(token);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING && current != token);
    bt_audio_cancel_request(current); bt_audio_service();
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_CANCELLED && !bt_audio_snapshot().control_inflight);
}

static void observed_disconnect(void)
{
    initialized(); bt_audio_service();
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    uint64_t generation = policy(BT_AUDIO_HFP, false, true);
    bt_audio_snapshot_t target = bt_audio_snapshot();
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 4);
    /* The same peer reconnects between media admission and BT queue admission. */
    assert(bt_audio_snapshot().generation == generation);
    unsigned before = calls;
    esp_err_t stale = bt_audio_request(target.generation, target.a2dp_link_generation, BT_AUDIO_DISCONNECT_A2DP, target.a2dp_peer, NULL);
    bt_audio_service();
    assert(calls == before && !bt_audio_snapshot().control_inflight && stale != ESP_OK);

    target = bt_audio_snapshot();
    assert(bt_audio_request(target.generation, target.a2dp_link_generation, BT_AUDIO_DISCONNECT_A2DP, target.a2dp_peer, NULL) == ESP_OK);
    /* A replacement after queue admission must also survive the service step. */
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 5);
    bt_audio_service();
    assert(calls == before && bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED);
    target = bt_audio_snapshot();
    assert(bt_audio_request(target.generation, target.a2dp_link_generation, BT_AUDIO_DISCONNECT_A2DP, target.a2dp_peer, NULL) == ESP_OK);
    bt_audio_service();
    assert(calls == before + 1 && bt_audio_snapshot().control_inflight);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 5);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED && !bt_audio_snapshot().a2dp_connected);
    assert(bt_audio_snapshot().generation == generation && bt_audio_snapshot().microphone_write_ready);
}

static uint64_t request_music_disconnect(void)
{
    bt_audio_snapshot_t target = bt_audio_snapshot();
    uint64_t token = 0;
    assert(bt_audio_request(target.generation, target.a2dp_link_generation, BT_AUDIO_DISCONNECT_A2DP,
        target.a2dp_peer, &token) == ESP_OK);
    return token;
}
static void replace_before_post(void)
{
    assert(!dispatch_calls && !bt_audio_snapshot().control_inflight);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 4);
    assert(!dispatch_calls && bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
}
static void disconnected_in_handler(void)
{
    assert(dispatch_calls == 1 && bt_audio_snapshot().control_inflight);
    assert(last_pid == BTC_PID_A2DP && last_action == BTC_AV_SINK_API_DISCONNECT_EVT);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED);
}
static void unrelated_work(void *context) { assert(on_btc && !context); }
static void dispatch_order(const char *scenario)
{
    initialized(); bt_audio_service();
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED); voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    uint64_t generation = policy(BT_AUDIO_HFP, false, true);
    uint64_t token = request_music_disconnect();
    defer_btc = true;
    if (!strcmp(scenario, "dispatch-replace")) before_post = replace_before_post;
    bool full = !strcmp(scenario, "dispatch-post-failure");
    if (full) queued_btc = unrelated_work;
    bt_audio_service();
    assert(!dispatch_calls && !bt_audio_snapshot().control_inflight);
    if (full) {
        bt_audio_snapshot_t failed = bt_audio_snapshot();
        assert(queued_btc == unrelated_work && failed.request_id == token &&
            failed.request_status == BT_AUDIO_JOB_FAILED && failed.request_error == ESP_FAIL);
        run_btc(); bt_audio_service();
        assert(!queued_btc && !dispatch_calls); /* Free space alone cannot replay the old disconnect. */
        assert(request_music_disconnect() != token);
        bt_audio_service(); run_btc();
        music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
        assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED && !bt_audio_snapshot().control_inflight);
    } else if (!strcmp(scenario, "dispatch-cancel")) {
        bt_audio_cancel_request(token);
        assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_CANCELLED);
        uint64_t next = request_music_disconnect();
        bt_audio_service(); run_btc(); /* Drain the cancelled ticket after its replacement was admitted. */
        assert(!dispatch_calls && bt_audio_snapshot().request_id == next && bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
        bt_audio_service(); run_btc();
    } else if (!strcmp(scenario, "dispatch-reentrant")) {
        inside_handler = disconnected_in_handler; run_btc();
        assert(!bt_audio_snapshot().control_inflight && bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED);
    } else {
        if (!strcmp(scenario, "dispatch-down")) music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
        assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
        run_btc();
        assert(!dispatch_calls && !bt_audio_snapshot().control_inflight &&
            bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED && bt_audio_snapshot().request_error == ESP_ERR_NOT_FOUND);
        if (!bt_audio_snapshot().a2dp_connected) music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 4);
        request_music_disconnect(); bt_audio_service(); run_btc();
    }
    assert(dispatch_calls == 1 && last_pid == BTC_PID_A2DP && last_action == BTC_AV_SINK_API_DISCONNECT_EVT);
    assert(bt_audio_snapshot().generation == generation && bt_audio_snapshot().microphone_write_ready);
}

static void expect_profile_dispatch(bt_audio_command_t command, uint8_t pid, uint8_t action)
{
    bt_audio_snapshot_t target = bt_audio_snapshot();
    uint64_t link = pid == BTC_PID_A2DP ? target.a2dp_link_generation : target.hfp_link_generation;
    unsigned before = dispatch_calls;
    assert(bt_audio_request(target.generation, link, command, peer, NULL) == ESP_OK);
    bt_audio_service();
    assert(dispatch_calls == before + 1 && last_pid == pid && last_action == action &&
        bt_audio_snapshot().request_status == BT_AUDIO_JOB_WAITING);
}
static void profile_actions(void)
{
    initialized(); bt_audio_service();
    expect_profile_dispatch(BT_AUDIO_CONNECT_HFP, BTC_PID_HF_CLIENT, BTC_HF_CLIENT_CONNECT_EVT);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    expect_profile_dispatch(BT_AUDIO_OPEN_VOICE, BTC_PID_HF_CLIENT, BTC_HF_CLIENT_CONNECT_AUDIO_EVT);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    expect_profile_dispatch(BT_AUDIO_CLOSE_VOICE, BTC_PID_HF_CLIENT, BTC_HF_CLIENT_DISCONNECT_AUDIO_EVT);
    voice(ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED);
    expect_profile_dispatch(BT_AUDIO_DISCONNECT_HFP, BTC_PID_HF_CLIENT, BTC_HF_CLIENT_DISCONNECT_EVT);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED);
    expect_profile_dispatch(BT_AUDIO_CONNECT_A2DP, BTC_PID_A2DP, BTC_AV_SINK_API_CONNECT_EVT);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    /* The SDK deinit barrier still applies at consumption of an already posted item. */
    defer_btc = true;
    request_music_disconnect(); bt_audio_service(); g_a2dp_sink_ongoing_deinit = true; run_btc();
    assert(dispatch_calls == 5 && !bt_audio_snapshot().control_inflight && bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED);
    defer_btc = g_a2dp_sink_ongoing_deinit = false;
    expect_profile_dispatch(BT_AUDIO_DISCONNECT_A2DP, BTC_PID_A2DP, BTC_AV_SINK_API_DISCONNECT_EVT);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
    assert(dispatch_calls == 6 && bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED);
}

static void timeout_profile(bool connect_hfp)
{
    initialized(); bt_audio_service();
    ae_route_port_t port = bt_audio_route_port();
    ae_route_t blocked = connect_hfp ? AE_ROUTE_HFP_HF : AE_ROUTE_A2DP_SINK;
    ae_route_t ready = connect_hfp ? AE_ROUTE_A2DP_SINK : AE_ROUTE_HFP_HF;
    if (connect_hfp) {
        music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
        music_format(ESP_A2D_SBC_CIE_SF_48K, ESP_A2D_SBC_CIE_CH_MODE_MONO, 3);
        music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    } else {
        hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
        voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    }
    bt_audio_snapshot_t target = bt_audio_snapshot();
    assert(bt_audio_request(target.generation, connect_hfp ? target.hfp_link_generation : target.a2dp_link_generation,
        connect_hfp ? BT_AUDIO_CONNECT_HFP : BT_AUDIO_CONNECT_A2DP, peer, NULL) == ESP_OK);
    assert(caps(port, blocked).reason == AE_BUSY && caps(port, ready).reason == AE_OK);
    bt_audio_service(); clock_us += BT_AUDIO_COMMAND_TIMEOUT_US; deadline(NULL);
    assert(bt_audio_snapshot().control_inflight && bt_audio_snapshot().request_status == BT_AUDIO_JOB_UNCERTAIN);
    ae_route_capabilities_t waiting = caps(port, blocked), usable = caps(port, ready);
    assert(waiting.reason == AE_BUSY && usable.reason == AE_OK);
    assert(port.prepare(NULL, blocked, &waiting.format, AE_SPEAKER, 12, &waiting.token) == AE_BUSY);
    assert(port.prepare(NULL, ready, &usable.format, connect_hfp ? AE_SPEAKER : AE_MICROPHONE, 13, &usable.token) == AE_OK);
    if (connect_hfp) hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    else music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 4);
    assert(!bt_audio_snapshot().control_inflight && bt_audio_snapshot().request_status == BT_AUDIO_JOB_UNCERTAIN);
    /* Late profile connection does not authorize a voice or media stream. */
    assert(caps(port, blocked).reason == AE_UNAVAILABLE && caps(port, ready).reason == AE_OK);
    assert(bt_audio_snapshot().route == (connect_hfp ? BT_AUDIO_A2DP : BT_AUDIO_HFP));
}

static void a2dp_reconnect(void)
{
    initialized(); bt_audio_service();
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    music_format(ESP_A2D_SBC_CIE_SF_48K, ESP_A2D_SBC_CIE_CH_MODE_STEREO, 3);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 3);
    ae_route_port_t port = bt_audio_route_port();
    assert(caps(port, AE_ROUTE_A2DP_SINK).connected);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 4);
    assert(!caps(port, AE_ROUTE_A2DP_SINK).connected);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 3);
    assert(bt_audio_snapshot().a2dp_connected);
    music_format(ESP_A2D_SBC_CIE_SF_48K, ESP_A2D_SBC_CIE_CH_MODE_STEREO, 4);
    music_state(ESP_A2D_AUDIO_STATE_STARTED, 4);
    uint64_t generation = policy(BT_AUDIO_A2DP, true, false);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
    hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED);
    uint8_t pcm[4] = {42, 0, 42, 0};
    a2_rx(pcm, sizeof(pcm)); bt_audio_pcm_t packet;
    assert(bt_audio_read_speaker(generation, &packet) && packet.samples[0] == 42);
}

static void prepare_during_pairing_capacity(void)
{
    ae_route_port_t port = bt_audio_route_port(); ae_route_capabilities_t h = caps(port, AE_ROUTE_HFP_HF);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 103, &h.token) == AE_OK);
}
static void pairing_audio_exclusion(void)
{
    initialized(); bt_audio_service(); hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
    voice(ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC); bt_audio_service();
    ae_route_port_t port = bt_audio_route_port(); ae_route_capabilities_t h = caps(port, AE_ROUTE_HFP_HF);
    assert(bt_audio_pairing_open(1000) == ESP_OK); bt_audio_service();
    assert(bt_audio_snapshot().discoverable && bt_audio_snapshot().pairing_busy);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 100, &h.token) == AE_BUSY);
    assert(bt_audio_set_policy(bt_audio_snapshot().generation, BT_AUDIO_HFP, false, false, NULL) == ESP_ERR_INVALID_STATE);
    security_event(ESP_BT_GAP_CFM_REQ_EVT); bt_audio_service();
    bt_audio_pairing_close(); bt_audio_service();
    assert(bt_audio_snapshot().pairing_handshake_pending && bt_audio_snapshot().pairing_busy && !bt_audio_snapshot().discoverable);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 101, &h.token) == AE_BUSY);
    esp_bt_gap_cb_param_t p = {.auth_cmpl.stat = ESP_BT_STATUS_SUCCESS};
    memcpy(p.auth_cmpl.bda, peer, 6); gap(ESP_BT_GAP_AUTH_CMPL_EVT, &p); bt_audio_service();
    assert(!bt_audio_snapshot().pairing_busy && !pairing_claim);
    h = caps(port, AE_ROUTE_HFP_HF);
    assert(port.prepare(NULL, AE_ROUTE_HFP_HF, &h.format, 3, 102, &h.token) == AE_OK);
    assert(bt_audio_pairing_open(1000) == ESP_ERR_INVALID_STATE);
    assert(port.stop(NULL, AE_ROUTE_HFP_HF, 102, &h.token) == AE_OK);
    assert(bt_audio_pairing_open(1000) == ESP_OK);
    bt_audio_pairing_close();
    assert(bt_audio_snapshot().pairing_busy && bt_audio_pairing_open(1000) == ESP_ERR_INVALID_STATE);
    bt_audio_service(); assert(!bt_audio_snapshot().pairing_busy && !pairing_claim);
    bond_count_hook = prepare_during_pairing_capacity;
    assert(bt_audio_pairing_open(1000) == ESP_ERR_INVALID_STATE);
    assert(!bond_count_hook && !pairing_claim && !bt_audio_snapshot().pairing_busy);
}

static void replace_during_pairing_capacity(void)
{
    assert(pairing_claim && le_window && !le_closes);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 4);
}
static void change_resource_during_capacity(void) { assert(pairing_claim && le_window); resource_current = false; }
static bool pairing_current(void) { return resource_current; }
static void reviewed_handoff(void)
{
    initialized(); bt_audio_service(); le_window = true;
    uint64_t revision = observed_bonds().revision;
    bt_audio_snapshot_t expected = bt_audio_snapshot();
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 3);
    assert(bt_audio_pairing_open_reviewed(1000, &expected, revision, pairing_current) == ESP_ERR_INVALID_VERSION);
    assert(!claim_calls && !le_closes && le_window);
    expected = bt_audio_snapshot();
    bond_count_hook = replace_during_pairing_capacity;
    assert(bt_audio_pairing_open_reviewed(1000, &expected, revision, pairing_current) == ESP_ERR_INVALID_VERSION);
    assert(!bond_count_hook && expected.a2dp_link_generation != bt_audio_snapshot().a2dp_link_generation);
    expected = bt_audio_snapshot();
    bond_count_hook = change_resource_during_capacity;
    assert(bt_audio_pairing_open_reviewed(1000, &expected, revision, pairing_current) == ESP_ERR_INVALID_VERSION);
    bt_audio_service();
    assert(!le_closes && le_window && !pairing_claim && !discoverable_writes && !bt_audio_snapshot().pairing_requested);
    resource_current = true;
    assert(bt_audio_pairing_open_reviewed(1000, &expected, revision, pairing_current) == ESP_OK);
    assert(le_closes == 1 && !le_window && pairing_claim);
    bt_audio_service(); assert(discoverable_writes == 1 && bt_audio_snapshot().discoverable);
    bt_audio_pairing_close(); bt_audio_service(); assert(!pairing_claim && !bt_audio_snapshot().discoverable);
}
static void device_name_test(const char *scenario)
{
    assert(name_requests == 1 && !bt_audio_snapshot().device_name_known);
    initialized(); bt_audio_service();
    if (!strcmp(scenario, "name-submit-failure")) {
        assert(bt_audio_snapshot().device_name_error == ESP_FAIL);
        name_error = ESP_OK;
        assert(bt_audio_pairing_open(100) == ESP_OK && name_requests == 2);
    } else if (!strcmp(scenario, "name-timeout")) {
        clock_us += BT_AUDIO_INIT_TIMEOUT_US; deadline(NULL);
        assert(bt_audio_snapshot().device_name_error == ESP_ERR_TIMEOUT);
        assert(!bt_audio_snapshot().failed && bt_audio_snapshot().initialized);
        assert(bt_audio_pairing_open(100) == ESP_OK && name_requests == 1);
    }
    char name[BT_AUDIO_DEVICE_NAME_MAX_BYTES + 2U];
    strcpy(name, "Actual Classic audio name");
    esp_bt_gap_cb_param_t p = {.get_dev_name_cmpl = {.status = ESP_BT_STATUS_SUCCESS, .name = name}};
    if (!strcmp(scenario, "name-invalid")) memset(name, 'x', sizeof(name) - 1U), name[sizeof(name) - 1U] = 0;
    if (!strcmp(scenario, "name-missing")) p.get_dev_name_cmpl.name = NULL;
    if (!strcmp(scenario, "name-event-failure")) p.get_dev_name_cmpl.status = 1;
    gap(ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT, &p);
    bool invalid = !strcmp(scenario, "name-invalid") || !strcmp(scenario, "name-missing") || !strcmp(scenario, "name-event-failure");
    assert(bt_audio_snapshot().device_name_known == !invalid);
    if (!invalid) assert(!strcmp(bt_audio_snapshot().device_name, name));
    else assert(bt_audio_snapshot().device_name_error != ESP_OK);
    assert(bt_audio_snapshot().initialized && !bt_audio_snapshot().failed);
    /* Name observation never owns the pairing guard or discovery closure. */
    bt_audio_pairing_close(); bt_audio_service();
    assert(!pairing_claim && !bt_audio_snapshot().discoverable);
}

static void pairing_retry_capacity(void)
{
    initialized(); bt_audio_service(); assert(bt_audio_pairing_open(1000) == ESP_OK);
    security_event(ESP_BT_GAP_CFM_REQ_EVT); bt_audio_service();
    esp_bt_gap_cb_param_t p = {.auth_cmpl.stat = 1};
    memcpy(p.auth_cmpl.bda, peer, 6); gap(ESP_BT_GAP_AUTH_CMPL_EVT, &p);
    bt_audio_pairing_close(); bt_audio_service();
    assert(bt_audio_snapshot().pairing_error == ESP_FAIL && !bt_audio_snapshot().failed);
    shared_count = 15;
    assert(bt_audio_pairing_open(1000) == ESP_ERR_NO_MEM);
    assert(bt_audio_snapshot().pairing_error == ESP_OK && !bt_audio_snapshot().pairing_busy);
    security_event(ESP_BT_GAP_CFM_REQ_EVT); security_event(ESP_BT_GAP_CFM_REQ_EVT);
    assert(bt_audio_snapshot().failed && bt_audio_snapshot().pairing_error == ESP_ERR_NO_MEM);
    assert(bt_audio_pairing_open(1000) == ESP_ERR_INVALID_STATE);
    assert(bt_audio_snapshot().pairing_error == ESP_ERR_NO_MEM);
}

static void pairing_test(const char *scenario)
{
    initialized(); bt_audio_service();
    assert(bt_audio_snapshot().classic_bonds_known && bt_audio_snapshot().classic_bond_count == 1);
    assert(bt_audio_snapshot().discovery_known && !bt_audio_snapshot().discoverable);
    assert(bt_audio_pairing_open(0) == ESP_ERR_INVALID_ARG);
    deny_claim = true; assert(bt_audio_pairing_open(100) == ESP_ERR_INVALID_STATE); deny_claim = false;
    shared_count = 15; assert(bt_audio_pairing_open(100) == ESP_ERR_NO_MEM && !pairing_claim); shared_count = 1;
    security_event(ESP_BT_GAP_CFM_REQ_EVT); bt_audio_service();
    assert(!security_accept && security_replies == 1 && !bt_audio_snapshot().pairing_handshake_pending);
    assert(bt_audio_pairing_open(100) == ESP_OK && pairing_claim);
    if (strcmp(scenario, "discovery-timeout") == 0) {
        drop_scan = true; bt_audio_service(); clock_us += BT_AUDIO_INIT_TIMEOUT_US; bt_audio_service();
        assert(bt_audio_snapshot().failed && pairing_claim && bt_audio_snapshot().pairing_error == ESP_ERR_TIMEOUT);
        return;
    }
    bt_audio_service(); assert(bt_audio_snapshot().discoverable);
    if (strcmp(scenario, "pairing-expiry") == 0) {
        clock_us += 100000; deadline(NULL); assert(!bt_audio_snapshot().pairing_requested && bt_audio_snapshot().pairing_expired);
        security_event(ESP_BT_GAP_CFM_REQ_EVT); bt_audio_service();
        assert(!security_accept && !pairing_claim && !bt_audio_snapshot().discoverable);
        return;
    }
    shared_count = 15; security_event(ESP_BT_GAP_CFM_REQ_EVT); bt_audio_service();
    assert(!security_accept && !bt_audio_snapshot().pairing_handshake_pending); shared_count = 1;
    security_event(ESP_BT_GAP_PIN_REQ_EVT); bt_audio_service(); assert(!security_accept);
    security_event(ESP_BT_GAP_KEY_REQ_EVT); bt_audio_service(); assert(!security_accept);
    security_event(ESP_BT_GAP_CFM_REQ_EVT); bt_audio_service();
    assert(security_accept && bt_audio_snapshot().pairing_handshake_pending);
    bt_audio_pairing_close(); bt_audio_service();
    assert(pairing_claim && !bt_audio_snapshot().discoverable);
    if (strcmp(scenario, "pairing-timeout") == 0) {
        clock_us += 30000000; deadline(NULL); bt_audio_service();
        assert(!bt_audio_snapshot().failed && pairing_claim && bt_audio_snapshot().pairing_handshake_pending &&
            bt_audio_snapshot().pairing_error == ESP_ERR_TIMEOUT);
    } else {
        esp_bt_gap_cb_param_t parameter = {.auth_cmpl.stat = ESP_BT_STATUS_SUCCESS};
        memcpy(parameter.auth_cmpl.bda, peer, 6); gap(ESP_BT_GAP_AUTH_CMPL_EVT, &parameter);
        assert(!bt_audio_snapshot().classic_bonds_known);
        bt_audio_service(); assert(!pairing_claim && bt_audio_snapshot().classic_bonds_known);
        assert(bt_audio_snapshot().pairing_completed && !bt_audio_snapshot().pairing_expired);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    if (strcmp(scenario, "unsupported") == 0) {
        assert(bt_audio_init() == ESP_ERR_NOT_SUPPORTED && calls == 0 && !allocation);
        assert(!bt_audio_snapshot().supported);
        ae_route_capabilities_t c = caps(bt_audio_route_port(), AE_ROUTE_HFP_HF);
        assert(!c.implemented && !c.connected && !c.full_duplex && c.reason == AE_NOT_SUPPORTED);
        return 0;
    }
    no_memory = strcmp(scenario, "allocation-failure") == 0;
    if (strcmp(scenario, "registration-failure") == 0) fail_call = 4;
    if (strcmp(scenario, "stack-unready") == 0) {
        stack_enabled = false;
        assert(bt_audio_init() == ESP_ERR_INVALID_STATE && calls == 0);
        stack_enabled = true;
    }
    if (!strcmp(scenario, "name-submit-failure")) name_error = ESP_FAIL;
    esp_err_t result = bt_audio_init();
    if (no_memory || fail_call) {
        assert(result != ESP_OK && bt_audio_snapshot().failed && !bt_audio_snapshot().initialized);
        assert(bt_audio_init() == ESP_ERR_INVALID_STATE);
    } else {
        assert(result == ESP_OK && !bt_audio_snapshot().initialized && !bt_audio_snapshot().failed);
        if (!strncmp(scenario, "pair-", 5)) pairing_lifecycle(scenario);
        else if (!strcmp(scenario, "foreign-bonds") || !strcmp(scenario, "other-profile")) profile_lifecycle(scenario);
        else if (!strcmp(scenario, "peer-switch")) profile_peer_switch();
        else if (!strcmp(scenario, "connect-race")) connect_race();
        else if (!strcmp(scenario, "timeout-hfp") || !strcmp(scenario, "timeout-a2dp")) timeout_profile(!strcmp(scenario, "timeout-hfp"));
        else if (!strcmp(scenario, "profile-actions")) profile_actions();
        else if (!strncmp(scenario, "dispatch-", 9)) dispatch_order(scenario);
        else if (!strcmp(scenario, "observed-disconnect")) observed_disconnect();
        else if (!strcmp(scenario, "a2dp-reconnect")) a2dp_reconnect();
        else if (!strcmp(scenario, "retry-capacity")) pairing_retry_capacity();
        else if (!strcmp(scenario, "reviewed-handoff")) reviewed_handoff();
        else if (!strcmp(scenario, "audio-exclusion")) pairing_audio_exclusion();
        else if (!strncmp(scenario, "name-", 5)) device_name_test(scenario);
        else if (strcmp(scenario, "normal") == 0) normal();
        else if (strcmp(scenario, "expiry-observation") == 0) expiry_observation_race();
        else if (!strcmp(scenario, "hfp-transmit-clock")) hfp_queue_clock(false);
        else if (!strcmp(scenario, "hfp-write-clock")) hfp_queue_clock(true);
        else if (!strcmp(scenario, "a2dp-queued-gap")) a2dp_queued_gap();
        else if (strcmp(scenario, "sustained-sbc") == 0) sustained_sbc();
        else if (strcmp(scenario, "playback-binding") == 0) playback_binding_test();
        else if (strncmp(scenario, "pairing", 7) == 0 || strcmp(scenario, "discovery-timeout") == 0) pairing_test(scenario);
        else if (strcmp(scenario, "init-timeout") == 0) {
            clock_us += BT_AUDIO_INIT_TIMEOUT_US; deadline(NULL);
            assert(bt_audio_snapshot().failed && bt_audio_snapshot().error == ESP_ERR_TIMEOUT);
        } else if (strcmp(scenario, "init-event-failure") == 0) {
            esp_hf_client_cb_param_t p = {.prof_stat.state = ESP_HF_INIT_FAIL};
            hf(ESP_HF_CLIENT_PROF_STATE_EVT, &p); assert(bt_audio_snapshot().failed);
        } else {
            initialized();
            if (strcmp(scenario, "command-failure") == 0) command_error = ESP_FAIL;
            bt_audio_snapshot_t target = bt_audio_snapshot();
            assert(bt_audio_request(target.generation, target.hfp_link_generation, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_OK);
            bt_audio_service();
            if (command_error) assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED && !bt_audio_snapshot().control_inflight);
            else if (strcmp(scenario, "command-timeout") == 0) {
                bt_audio_cancel(); clock_us += BT_AUDIO_COMMAND_TIMEOUT_US; deadline(NULL);
                assert(!bt_audio_snapshot().failed && bt_audio_snapshot().control_inflight &&
                    bt_audio_snapshot().request_status == BT_AUDIO_JOB_UNCERTAIN);
                assert(bt_audio_pairing_open(1000) == ESP_ERR_INVALID_STATE);
                hf_connection(ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED);
                assert(!bt_audio_snapshot().control_inflight && bt_audio_snapshot().request_status == BT_AUDIO_JOB_UNCERTAIN);
                target = bt_audio_snapshot();
                assert(bt_audio_request(target.generation, target.hfp_link_generation, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_OK);
            }
        }
    }
    free(allocation);
    printf("Bluetooth audio: %s passed\n", scenario);
    return 0;
}
