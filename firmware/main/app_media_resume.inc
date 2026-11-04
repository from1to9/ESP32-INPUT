// SPDX-License-Identifier: Apache-2.0
/* Coordinator-owned boot choices. A persistent address alone never grants
 * radio or playback authority. The normal owners recheck their exact tokens. */
_Static_assert(AE_ROUTE_HFP_HF == 2 && AE_ROUTE_A2DP_SINK == 3 && AE_ROUTE_USB_UAC == 4, "resume_v1 routes");
static bool resume_has_peer(const uint8_t peer[6])
{
    static const uint8_t empty[6];
    return memcmp(peer, empty, 6) != 0;
}

static void resume_save(app_boot_audio_t next)
{
    if (!controls.resume.save || !memcmp(&next, &controls.resume.desired, sizeof(next))) return;
    controls.resume.desired = next;
    controls.resume.save(&next);
    increase(&controls.revision);
}

static void resume_cancel_link(void)
{
    if (controls.resume.connect_id) {
        bt_audio_cancel_request(controls.resume.connect_id);
        controls.resume.connect_id = 0;
    }
}

static void resume_cancel_start(void)
{
    if (controls.resume.automatic_token) {
        app_media_runtime_cancel_boot_speaker(controls.resume.automatic_token);
        controls.resume.automatic_token = 0;
    }
}

/* Separate from the HID epoch/panel cancellation hook. Only loss of audio
 * authority, not a BLE reconnect or foreground mode change, cancels boot work. */
void app_media_controls_pause_boot(void)
{
    resume_cancel_link();
    resume_cancel_start();
}

void app_media_controls_boot(const app_boot_audio_t *choices, void (*save)(const app_boot_audio_t *))
{
    if (!controls.initialized || !choices || !save) return;
    controls.resume.desired = *choices;
    controls.resume.save = save;
    controls.resume.route_pending = choices->route != AE_ROUTE_OFF;
    controls.resume.connect_left = choices->services;
    if (resume_has_peer(choices->peer)) {
        memcpy(controls.selected_peer, choices->peer, 6);
        controls.selected_peer_known = true;
    }
}

static void resume_stop_speaker(void)
{
    app_boot_audio_t next = controls.resume.desired;
    next.speaker = 0;
    controls.resume.submitted_speaker = 0;
    controls.resume.speaker_job = (ae_id_t){0};
    resume_save(next);
}

static void resume_select_peer(const uint8_t peer[6])
{
    if (!controls.resume.save || controls.context.local_access != APP_LOCAL_NORMAL ||
        !memcmp(controls.resume.desired.peer, peer, 6)) return;
    resume_cancel_link(); resume_cancel_start();
    controls.resume.connect_left = 0;
    controls.resume.attempted_stream = 0; controls.resume.notice = NULL;
    controls.resume.submitted_speaker = 0;
    controls.resume.speaker_job = (ae_id_t){0};
    app_boot_audio_t next = {.route = controls.resume.desired.route};
    memcpy(next.peer, peer, 6); resume_save(next);
}

static void resume_link_choice(const uint8_t peer[6], unsigned service, bool connect)
{
    if (!controls.resume.save) return;
    resume_select_peer(peer);
    resume_cancel_link();
    controls.resume.connect_left &= (uint8_t)~service;
    app_boot_audio_t next = controls.resume.desired;
    if (connect) {
        controls.resume.ignore_connected &= (uint8_t)~service;
        next.disconnected &= (uint8_t)~service;
    }
    else {
        next.disconnected |= (uint8_t)service;
        next.services &= (uint8_t)~service;
        if ((service == APP_RESUME_HFP && next.speaker == AE_ROUTE_HFP_HF) ||
            (service == APP_RESUME_A2DP && next.speaker == AE_ROUTE_A2DP_SINK)) {
            resume_cancel_start(); resume_stop_speaker(); next.speaker = 0;
        }
    }
    resume_save(next);
}

static void resume_result(ae_result_t result, const ae_job_snapshot_t *job)
{
    if (!controls.resume.save) return;
    if (controls.pending_action == APP_MEDIA_ROUTE && result != AE_OK)
        controls.resume.notice = "Audio route change failed. Check the connection.";
    if (controls.pending_action == APP_MEDIA_ROUTE && result == AE_OK) {
        controls.resume.notice = NULL;
        app_boot_audio_t next = controls.resume.desired;
        next.route = controls.resume.submitted_route;
        if (next.route != AE_ROUTE_HFP_HF && next.route != AE_ROUTE_A2DP_SINK) {
            resume_cancel_link(); controls.resume.connect_left = 0;
            resume_cancel_start(); resume_stop_speaker(); next.speaker = 0;
        }
        resume_save(next);
    }
    if (controls.resume.pending_playback && controls.resume.submitted_speaker) {
        if (result == AE_OK && job->kind == AE_PLAYBACK_STREAM && !ae_id_empty(&job->job_id))
            controls.resume.speaker_job = job->job_id;
        else { controls.resume.automatic_token = 0; controls.resume.submitted_speaker = 0; controls.resume.notice = "Speaker start failed. Check connection."; }
    }
}

/* Remember a service only after the BT owner sees the bonded connection.
 * Browsing a row may select its address, but cannot set its service bits. */
static void resume_observe_service(app_boot_audio_t *next, const uint8_t peer[6], uint8_t service)
{
    if ((controls.resume.ignore_connected & service) || (next->disconnected & service) || !resume_has_peer(peer) || saved_bond(peer) < 0) return;
    if (!resume_has_peer(next->peer)) memcpy(next->peer, peer, 6);
    if (!memcmp(next->peer, peer, 6)) {
        next->services |= service;
        controls.resume.connect_left &= (uint8_t)~service;
    }
}

static void poll_resume(void)
{
    if (!controls.resume.save) return;
    const bt_audio_snapshot_t *bt = &controls.bluetooth;
    if (bt->request_id != controls.resume.seen_link_request) {
        controls.resume.seen_link_request = bt->request_id;
        if ((bt->request_command == BT_AUDIO_DISCONNECT_HFP || bt->request_command == BT_AUDIO_DISCONNECT_A2DP) &&
            !memcmp(bt->request_peer, controls.resume.desired.peer, 6)) {
            resume_link_choice(bt->request_peer, bt->request_command == BT_AUDIO_DISCONNECT_A2DP ?
                APP_RESUME_A2DP : APP_RESUME_HFP, false);
        }
    }
    if (controls.context.locked || controls.context.local_access == APP_LOCAL_PAUSED) {
        resume_cancel_link();
        resume_cancel_start();
        if (controls.context.locked) resume_stop_speaker();
        return;
    }
    if (!owner_ready(true)) { resume_cancel_link(); return; }
    app_boot_audio_t next = controls.resume.desired;
    const ae_snapshot_t *audio = &controls.runtime.audio;
    if (controls.resume.route_pending && idle_for(true) && !any_jobs() && audio->hardware_off_confirmed) {
        if (audio->route == next.route) controls.resume.route_pending = false;
        else if (controls.routes[next.route].implemented) {
            controls.resume.route_pending = false;
            if (submit_runtime((app_media_local_request_t){.action = APP_MEDIA_ROUTE, .route = (ae_route_t)next.route}, true) != AE_OK)
                controls.resume.notice = "Audio route restore unavailable. Select a connection.";
            return;
        }
    }
    if (!bt->initialized || bt->failed || !controls.bonds.known || controls.bonds.refreshing ||
        controls.bonds.error != ESP_OK || controls.bonds.count > BT_AUDIO_CLASSIC_BONDS_MAX) return;
    if (resume_has_peer(next.peer) && saved_bond(next.peer) < 0) {
        resume_cancel_link(); resume_cancel_start(); controls.resume.connect_left = 0;
        controls.resume.speaker_job = (ae_id_t){0}; controls.resume.submitted_speaker = 0;
        controls.selected_peer_known = false;
        resume_save((app_boot_audio_t){.route = next.route});
        return;
    }
    if (!bt->a2dp_connected) controls.resume.ignore_connected &= (uint8_t)~APP_RESUME_A2DP;
    if (!bt->hfp_connected) controls.resume.ignore_connected &= (uint8_t)~APP_RESUME_HFP;
    if (bt->a2dp_connected && bt->a2dp_peer_known && bt->classic_a2dp_peer_bonded)
        resume_observe_service(&next, bt->a2dp_peer, APP_RESUME_A2DP);
    if (bt->hfp_connected && bt->hfp_peer_known && bt->classic_hfp_peer_bonded)
        resume_observe_service(&next, bt->hfp_peer, APP_RESUME_HFP);
    resume_save(next);
    const ae_job_snapshot_t *job = &audio->playback;
    if (!ae_id_empty(&controls.resume.speaker_job) && ae_id_equal(&controls.resume.speaker_job, &job->job_id)) {
        unsigned route = controls.resume.submitted_speaker;
        if (job->first_frame_seen && (route == AE_ROUTE_HFP_HF || route == AE_ROUTE_A2DP_SINK) &&
            audio->route == route && ae_route_token_equal(&audio->route_token, &controls.resume.submitted_token) &&
            saved_bond(audio->route_token.peer_id.bytes + 10) >= 0) {
            const uint8_t *peer = audio->route_token.peer_id.bytes + 10;
            if (memcmp(next.peer, peer, 6)) { next.services = next.disconnected = 0; memcpy(next.peer, peer, 6); }
            uint8_t service = route == AE_ROUTE_HFP_HF ? APP_RESUME_HFP : APP_RESUME_A2DP;
            next.disconnected &= (uint8_t)~service;
            next.services |= service; next.speaker = next.route = (uint8_t)route;
            controls.resume.notice = NULL; resume_save(next);
        }
        if (!job_active(job)) {
            controls.resume.automatic_token = 0;
            uint64_t link = route == AE_ROUTE_HFP_HF ? bt->hfp_link_generation : bt->a2dp_link_generation;
            if (job->stop_reason == AE_STOP_UNDERRUN || (job->stop_reason == AE_STOP_OWNER_LOST &&
                link != controls.resume.submitted_token.link_generation)) {
                controls.resume.listen_after_us = (uint64_t)esp_timer_get_time();
                controls.resume.attempted_stream = 0;
                controls.resume.speaker_job = (ae_id_t){0}; controls.resume.submitted_speaker = 0;
            } else resume_stop_speaker();
        }
    }
    next = controls.resume.desired;
    if (bt->pairing_busy || bt->pairing_requested || bt->pairing_handshake_pending) { resume_cancel_link(); return; }
    if (!idle_for(true) || any_jobs() || !audio->hardware_off_confirmed || audio->quarantined ||
        bt->control_inflight || bt->request_status == BT_AUDIO_JOB_PENDING || bt->request_status == BT_AUDIO_JOB_WAITING) return;
    if (controls.resume.connect_id) {
        if (bt->request_id != controls.resume.connect_id || bt->request_status != BT_AUDIO_JOB_SUCCEEDED)
            controls.resume.notice = "Reconnect unconfirmed. Use Manage devices.";
        else controls.resume.notice = NULL;
        controls.resume.connect_id = 0;
    }
    if (controls.resume.connect_left && next.route != AE_ROUTE_USB_UAC && resume_has_peer(next.peer)) {
        bool music = (controls.resume.connect_left & APP_RESUME_A2DP) != 0;
        uint8_t service = music ? APP_RESUME_A2DP : APP_RESUME_HFP;
        controls.resume.connect_left &= (uint8_t)~service; /* One attempt per saved service this boot. */
        if (!(music ? bt->a2dp_peer_known : bt->hfp_peer_known)) {
            esp_err_t accepted = bt_audio_saved_request(bt->generation, music ? bt->a2dp_link_generation : bt->hfp_link_generation,
                controls.bonds.revision, music ? BT_AUDIO_CONNECT_A2DP : BT_AUDIO_CONNECT_HFP, next.peer, &controls.resume.connect_id);
            controls.resume.notice = accepted == ESP_OK ? "Reconnecting saved audio host" : "Reconnect unavailable. Use Manage devices.";
        }
        return;
    }
    if (!next.speaker || live_start_block(AE_SPEAKER, true) != APP_MEDIA_START_ALLOWED ||
        live_route(AE_SPEAKER) != next.speaker) return;
    bool music = next.speaker == AE_ROUTE_A2DP_SINK;
    const ae_route_token_t *token = &controls.routes[next.speaker].token;
    uint64_t last = music ? bt->a2dp_pcm_us : bt->hfp_pcm_us;
    uint64_t link = music ? bt->a2dp_link_generation : bt->hfp_link_generation;
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (!last || last <= controls.resume.listen_after_us || last > now || now - last > BT_AUDIO_PCM_MAX_AGE_US ||
        token->link_generation != link || controls.resume.attempted_stream == link ||
        memcmp(token->peer_id.bytes + 10, next.peer, 6) ||
        memcmp(music ? bt->a2dp_peer : bt->hfp_peer, next.peer, 6)) return;
    controls.resume.attempted_stream = link;
    if (submit_start(AE_PLAYBACK_STREAM, NULL, true) == AE_OK) controls.resume.automatic_token = controls.pending_token;
    else controls.resume.notice = "Speaker start unavailable.";
}


void app_media_controls_clear_boot(void)
{
    resume_cancel_link(); resume_cancel_start();
    controls.resume.connect_left = 0;
    controls.resume.route_pending = false;
    controls.resume.submitted_speaker = 0;
    controls.resume.speaker_job = (ae_id_t){0};
    controls.resume.notice = NULL;
    controls.resume.ignore_connected = APP_RESUME_HFP | APP_RESUME_A2DP;
    controls.selected_peer_known = false;
    controls.resume.desired = (app_boot_audio_t){0};
}
