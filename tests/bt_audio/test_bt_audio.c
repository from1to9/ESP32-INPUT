// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bt_audio.h"
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
static void (*clock_hook)(void);
static bool no_memory, stack_enabled = true;
static unsigned calls, kicks, fail_call;
static esp_err_t command_error, name_error;
static unsigned name_requests;
static esp_hf_client_cb_t hf;
static esp_a2d_cb_t a2;
static esp_bt_gap_cb_t gap;
static esp_bt_gap_profile_status_t gap_status;
static bool pairing_claim, deny_claim, security_accept, drop_scan;
static unsigned security_replies;
static int bond_count = 1, shared_count = 1;
static esp_err_t bond_list_error;
static void (*bond_count_hook)(void);
static void (*hf_rx)(const uint8_t *, uint32_t), (*a2_rx)(const uint8_t *, uint32_t);
static uint32_t (*hf_tx)(uint8_t *, uint32_t);
static void (*deadline)(TimerHandle_t);
static void *allocation;
static const uint8_t peer[6] = {2, 3, 4, 5, 6, 7};

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
{ assert(*count >= bond_count); *count = bond_count; if (bond_count == 1) memcpy(list[0], peer, 6); return bond_list_error; }
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t connection, esp_bt_discovery_mode_t discovery)
{ if (!drop_scan) gap_status = (esp_bt_gap_profile_status_t){.conn_mode = connection, .disc_mode = discovery}; return ESP_OK; }
esp_err_t esp_bt_gap_get_profile_status(esp_bt_gap_profile_status_t *status) { *status = gap_status; return ESP_OK; }
esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t address, bool accept, uint8_t bytes, esp_bt_pin_code_t pin)
{ (void)address; (void)pin; assert(!accept && bytes == 0); security_accept = accept; ++security_replies; return ESP_OK; }
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t address, bool accept)
{ assert(memcmp(address, peer, 6) == 0); security_accept = accept; ++security_replies; return ESP_OK; }
esp_err_t esp_bt_gap_ssp_passkey_reply(esp_bd_addr_t address, bool accept, uint32_t passkey)
{ (void)address; assert(!accept && passkey == 0); security_accept = accept; ++security_replies; return ESP_OK; }
bool ble_hid_pairing_claim_classic(void)
{ if (deny_claim || pairing_claim) return false; pairing_claim = true; return true; }
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
static esp_err_t control(const uint8_t *address) { assert(memcmp(address, peer, 6) == 0); ++calls; return command_error; }
esp_err_t esp_hf_client_connect(const uint8_t *address) { return control(address); }
esp_err_t esp_hf_client_disconnect(const uint8_t *address) { return control(address); }
esp_err_t esp_hf_client_connect_audio(const uint8_t *address) { return control(address); }
esp_err_t esp_hf_client_disconnect_audio(const uint8_t *address) { return control(address); }
esp_err_t esp_a2d_sink_connect(const uint8_t *address) { return control(address); }
esp_err_t esp_a2d_sink_disconnect(const uint8_t *address) { return control(address); }
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
    assert(bt_audio_saved_request(bt_audio_snapshot().generation, saved.revision + 1, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_ERR_INVALID_STATE);
    uint8_t absent[6] = {9};
    assert(bt_audio_saved_request(bt_audio_snapshot().generation, saved.revision, BT_AUDIO_CONNECT_HFP, absent, NULL) == ESP_ERR_NOT_FOUND);
    assert(bt_audio_saved_request(bt_audio_snapshot().generation, saved.revision, BT_AUDIO_CONNECT_HFP, peer, &request) == ESP_OK);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_PENDING);
    bt_audio_service(); assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_WAITING);
    bt_audio_cancel(); assert(bt_audio_snapshot().control_inflight);
    assert(bt_audio_request(bt_audio_snapshot().generation, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_ERR_INVALID_STATE);
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
    assert(bt_audio_receive(&frame) == AE_STALE); /* Expired receive packet revokes the reservation. */
    music_format(3, ESP_A2D_SBC_CIE_CH_MODE_MONO, 43);
    assert(!bt_audio_snapshot().a2dp_format_known);
    assert(bt_audio_refresh_bonds() == ESP_OK && observed_bonds().refreshing);
    assert(bt_audio_saved_request(bt_audio_snapshot().generation, observed_bonds().revision,
        BT_AUDIO_DISCONNECT_A2DP, peer, NULL) == ESP_ERR_INVALID_STATE);
    bond_list_error = ESP_FAIL; bt_audio_service();
    saved = observed_bonds(); assert(!saved.known && saved.error == ESP_FAIL && saved.count == 1);
    bond_list_error = ESP_OK; assert(bt_audio_refresh_bonds() == ESP_OK); bt_audio_service();
    saved = observed_bonds(); assert(saved.known && saved.bonds[0].a2dp_current);
    unsigned prior_calls = calls;
    assert(bt_audio_saved_request(bt_audio_snapshot().generation, saved.revision,
        BT_AUDIO_DISCONNECT_A2DP, peer, NULL) == ESP_OK);
    assert(bt_audio_refresh_bonds() == ESP_OK); bt_audio_service();
    assert(calls == prior_calls + 1 && bt_audio_snapshot().request_status == BT_AUDIO_JOB_WAITING);
    music_connection(ESP_A2D_CONNECTION_STATE_DISCONNECTED, 43);
    assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_SUCCEEDED);
    music_connection(ESP_A2D_CONNECTION_STATE_CONNECTED, 44);
    saved = observed_bonds(); prior_calls = calls;
    assert(bt_audio_saved_request(bt_audio_snapshot().generation, saved.revision,
        BT_AUDIO_DISCONNECT_A2DP, peer, NULL) == ESP_OK);
    esp_bt_gap_cb_param_t mutation = {.auth_cmpl.stat = 1};
    memcpy(mutation.auth_cmpl.bda, peer, 6); gap(ESP_BT_GAP_AUTH_CMPL_EVT, &mutation);
    bt_audio_service();
    assert(calls == prior_calls && bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED);
}

static ae_route_port_t interrupt_port;
static ae_route_token_t interrupt_token;
static void replace_playback_during_pcm(void) {
    assert(interrupt_port.bind_playback(NULL, AE_ROUTE_HFP_HF, 100, &interrupt_token, 40, true) == AE_OK);
}
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
    interrupt_port = port; interrupt_token = current.token; clock_hook = replace_playback_during_pcm;
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
        assert(bt_audio_snapshot().failed && pairing_claim && bt_audio_snapshot().pairing_handshake_pending);
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
        if (!strcmp(scenario, "retry-capacity")) pairing_retry_capacity();
        else if (!strcmp(scenario, "audio-exclusion")) pairing_audio_exclusion();
        else if (!strncmp(scenario, "name-", 5)) device_name_test(scenario);
        else if (strcmp(scenario, "normal") == 0) normal();
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
            assert(bt_audio_request(bt_audio_snapshot().generation, BT_AUDIO_CONNECT_HFP, peer, NULL) == ESP_OK);
            bt_audio_service();
            if (command_error) assert(bt_audio_snapshot().request_status == BT_AUDIO_JOB_FAILED && !bt_audio_snapshot().control_inflight);
            else if (strcmp(scenario, "command-timeout") == 0) {
                bt_audio_cancel(); clock_us += BT_AUDIO_COMMAND_TIMEOUT_US; deadline(NULL);
                assert(bt_audio_snapshot().failed && bt_audio_snapshot().request_status == BT_AUDIO_JOB_UNCERTAIN);
            }
        }
    }
    free(allocation);
    printf("Bluetooth audio: %s passed\n", scenario);
    return 0;
}
