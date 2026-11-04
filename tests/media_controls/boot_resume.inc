// SPDX-License-Identifier: Apache-2.0
static const uint8_t resume_peer[6] = {4,3,2,1,0,7};
static unsigned boot_writes;
static app_boot_audio_t boot_value;
static void boot_save(const app_boot_audio_t *v) { ++boot_writes; boot_value = *v; }
static void boot_setup(unsigned services, unsigned speaker)
{
    setup(); context.overlay = APP_OVERLAY_NONE;
    bluetooth = (bt_audio_snapshot_t){.initialized=true, .supported=true, .a2dp_link_generation=11, .hfp_link_generation=8};
    bonds = (bt_audio_bond_list_t){.known=true, .revision=7, .count=1};
    memcpy(bonds.bonds[0].address, resume_peer, 6);
    distinct_routes = true;
    route.capture = false; route.connected = false;
    route.token.peer_id = engine_id(peer_id(resume_peer)); route.token.link_generation=11;
    routes_by_id[AE_ROUTE_A2DP_SINK] = route;
    boot_value = (app_boot_audio_t){.route=AE_ROUTE_A2DP_SINK, .services=(uint8_t)services, .speaker=(uint8_t)speaker};
    memcpy(boot_value.peer,resume_peer,6);
    app_media_controls_boot(&boot_value, boot_save);
}
static void boot_connected(void)
{
    bluetooth.a2dp_connected = bluetooth.a2dp_peer_known = bluetooth.classic_a2dp_peer_bonded = true;
    memcpy(bluetooth.a2dp_peer, resume_peer, 6);
    bluetooth.request_status = BT_AUDIO_JOB_SUCCEEDED;
}
static void test_restore_speaker(void)
{
    boot_setup(APP_RESUME_A2DP, AE_ROUTE_A2DP_SINK);
    context.local_access = APP_LOCAL_BROWSE;
    poll_at(1100); assert(!submissions && !link_count && !boot_writes);
    context.local_access = APP_LOCAL_NORMAL;
    poll_at(1200); assert(submissions == 1 && submitted.action == APP_MEDIA_ROUTE && !link_count);
    owner.audio.route = AE_ROUTE_A2DP_SINK; complete(AE_OK);
    assert(link_count == 1 && links[0].command == BT_AUDIO_CONNECT_A2DP && links[0].bond_revision == 7 &&
        !memcmp(links[0].peer,resume_peer,6) && !pairing_opens);
    poll_at(1250); assert(!view.media.playback.can_stop && view.media.speaker_waiting && submissions == 1);
    boot_connected(); poll_at(1300); assert(submissions == 1);
    routes_by_id[AE_ROUTE_A2DP_SINK].connected = true;
    poll_at(1350); assert(submissions == 1); /* Profile and stream without decoded music. */
    context.screen_off = true;
    bluetooth.a2dp_pcm_us = 1390000; poll_at(1390); assert(submissions == 1); /* Unknown display state. */
    context.display_asleep = true; /* Idle display sleep does not revoke a saved speaker-only intent. */
    bluetooth.a2dp_pcm_us = 1400000; poll_at(1400);
    context.display_asleep = context.screen_off = false;
    assert(submissions == 2 && submitted.action == APP_MEDIA_START && submitted.kind == AE_PLAYBACK_STREAM &&
        submitted.route == AE_ROUTE_A2DP_SINK && ae_route_token_equal(&submitted.expected_route_token, &route.token));
    owner.audio.playback = (ae_job_snapshot_t){.kind=AE_PLAYBACK_STREAM,.state=AE_JOB_RUNNING,.job_id={{42}},
        .consumer_generation=9,.first_frame_seen=true};
    owner.result_detail.job = owner.audio.playback; owner.audio.route_token = route.token;
    complete(AE_OK); assert(!view.media.speaker_waiting && !boot_writes);
    owner.audio.playback.state=AE_JOB_ERROR; owner.audio.playback.stop_reason=AE_STOP_UNDERRUN;
    poll_at(2000); assert(submissions == 2 && view.media.speaker_waiting);
    bluetooth.a2dp_pcm_us=2050000; poll_at(2050);
    assert(submissions == 3 && submitted.kind == AE_PLAYBACK_STREAM);
    owner.result_detail.job = (ae_job_snapshot_t){0}; complete(AE_UNAVAILABLE);
    poll_at(2100); assert(submissions == 3); /* Failed submission cannot spin on the same link. */
    app_intent_t stop = intent(APP_MEDIA_LIVE_STOP); stop.media.value.role.role=APP_MEDIA_SPEAKER;
    handle(stop); poll_at(2200);
    assert(!boot_value.speaker && !view.media.speaker_waiting && !pairing_opens);
}
static void test_restore_host_boundaries(void)
{
    boot_setup(APP_RESUME_A2DP|APP_RESUME_HFP, AE_ROUTE_A2DP_SINK);
    owner.audio.route=AE_ROUTE_A2DP_SINK; bluetooth.pairing_requested=true;
    poll_at(1100); assert(!link_count);
    bluetooth.pairing_requested=false; bonds.refreshing=true; poll_at(1150); assert(!link_count && !boot_writes);
    bonds.refreshing=false; poll_at(1200); assert(link_count==1);
    bluetooth.request_status=BT_AUDIO_JOB_UNCERTAIN; bluetooth.control_inflight=true;
    poll_at(25000); assert(link_count==1 && !submissions);
    bluetooth.control_inflight=false; poll_at(25100);
    assert(link_count==2 && links[1].command==BT_AUDIO_CONNECT_HFP);
    bluetooth.request_status=BT_AUDIO_JOB_FAILED; poll_at(51000); poll_at(52000);
    assert(link_count==2 && !submissions);
    boot_connected(); bluetooth.a2dp_peer[0] ^= 1U; bluetooth.a2dp_pcm_us=52100000;
    routes_by_id[AE_ROUTE_A2DP_SINK].connected=true; poll_at(52100); assert(!submissions && view.media.speaker_waiting);
    context.locked=true; poll_at(52200); assert(!boot_value.speaker);
    context.locked=false; bonds.count=0; poll_at(52300);
    assert(!resume_has_peer(boot_value.peer) && !boot_value.services && !boot_value.speaker && link_count==2);
}
static void test_remember_local_speaker(void)
{
    boot_setup(0,0);
    owner.audio.route=AE_ROUTE_A2DP_SINK; boot_connected();
    routes_by_id[AE_ROUTE_A2DP_SINK].connected=true; poll_at(1050);
    assert(boot_value.services==APP_RESUME_A2DP && !boot_value.speaker && !submissions);
    app_intent_t start=intent(APP_MEDIA_LIVE_START); start.media.value.role.role=APP_MEDIA_SPEAKER;
    handle(start); assert(submissions==1 && !boot_value.speaker);
    owner.audio.route_token=route.token;
    owner.result_detail.job=(ae_job_snapshot_t){.kind=AE_PLAYBACK_STREAM,.state=AE_JOB_STARTING,.job_id={{42}},.consumer_generation=11};
    owner.audio.playback=owner.result_detail.job; complete(AE_OK); assert(!boot_value.speaker);
    owner.audio.playback.first_frame_seen=true; owner.audio.playback.state=AE_JOB_RUNNING; poll_at(1100);
    assert(boot_value.speaker==AE_ROUTE_A2DP_SINK);
    app_intent_t stop=intent(APP_MEDIA_LIVE_STOP); stop.media.value.role.role=APP_MEDIA_SPEAKER;
    stop.media.job_id=view.media.playback.id; stop.media.consumer_generation=view.media.playback.consumer_generation;
    handle(stop); assert(submitted.action==APP_MEDIA_STOP_JOB && !boot_value.speaker);
    owner.audio.playback=(ae_job_snapshot_t){0}; complete(AE_OK);
    context.overlay=APP_OVERLAY_MEDIA_OPTIONS; poll_at(1150);
    app_intent_t disconnect=intent(APP_MEDIA_LINK_DISCONNECT);
    handle(disconnect); assert(submitted.action==APP_MEDIA_LINK_CONTROL && !submitted.value);
    assert(!boot_value.services && boot_value.disconnected==APP_RESUME_A2DP);
    poll_at(1200); assert(!boot_value.services);
    app_boot_audio_t restart=boot_value;
    memset(&controls,0,sizeof(controls)); assert(app_media_controls_init()); app_media_controls_boot(&restart,boot_save);
    bluetooth.a2dp_connected=bluetooth.a2dp_peer_known=false; poll_at(1300);
    assert(!link_count && !boot_value.speaker);
}

static void test_restore_read_only_and_disconnect(void)
{
    boot_setup(APP_RESUME_A2DP, AE_ROUTE_A2DP_SINK);
    owner.audio.route = AE_ROUTE_A2DP_SINK;
    boot_connected(); poll_at(1100);
    bonds.count = 2; bonds.bonds[1].address[5] = 9;
    context.overlay = APP_OVERLAY_BLUETOOTH_DEVICES; context.bluetooth_audio_devices = true;
    poll_at(1110);
    app_intent_t details = intent(APP_MEDIA_DEVICE_DETAILS);
    details.media.object_revision = view.media.devices.revision;
    details.media.value.peer_id = view.media.devices.rows[1].id;
    apply_effect(handle(details)); poll_at(1120);
    assert(!memcmp(boot_value.peer, resume_peer, 6) && boot_value.speaker && !boot_writes);
    /* The shared radio owner can also receive an authenticated API disconnect. */
    bluetooth.request_id = 201; bluetooth.request_command = BT_AUDIO_DISCONNECT_A2DP;
    memcpy(bluetooth.request_peer, resume_peer, 6); poll_at(1130);
    assert(!boot_value.speaker && !boot_value.services && boot_value.disconnected == APP_RESUME_A2DP);
}
