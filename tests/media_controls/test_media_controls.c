// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_media_controls.c"

static app_media_snapshot_t owner;
static app_recording_store_snapshot_t disk;
static board_recording_flash_status_t physical;
static ae_codec_capabilities_t hardware;
static bt_audio_snapshot_t bluetooth;
static bt_audio_bond_list_t bonds;
static ae_route_capabilities_t route;
static app_media_local_request_t submitted;
static unsigned submissions, confirmations, rejections;
static struct { bt_audio_command_t command; uint64_t generation, bond_revision, token; uint8_t peer[6]; } links[8];
static unsigned link_count;
static esp_err_t link_result = ESP_OK;
static uint64_t next_link_token = 100;
static ae_result_t submit_result = AE_OK;
static bool snapshot_available = true;
static bool store_snapshot_available = true;
static ae_result_t records_copy_result = AE_OK;
static ae_record_metadata_t listed[APP_MEDIA_ROWS];
static app_media_controls_context_t context;
static app_view_t view;

bool app_media_runtime_snapshot(app_media_snapshot_t *out) { if (snapshot_available) *out = owner; return snapshot_available; }
bool app_recording_store_snapshot(app_recording_store_snapshot_t *out)
{ if (store_snapshot_available) *out = disk; return store_snapshot_available; }
board_recording_flash_status_t board_recording_flash_status(void) { return physical; }
bt_audio_snapshot_t bt_audio_snapshot(void) { return bluetooth; }
void bt_audio_copy_bond_list(bt_audio_bond_list_t *out) { assert(out); *out = bonds; }
esp_err_t bt_audio_saved_request(uint64_t generation, uint64_t revision, bt_audio_command_t command,
                               const uint8_t peer[6], uint64_t *token)
{
    assert(link_count < 8 && peer && token);
    links[link_count].command = command; links[link_count].generation = generation;
    links[link_count].bond_revision = revision; memcpy(links[link_count].peer, peer, 6);
    if (link_result == ESP_OK) {
        *token = ++next_link_token; links[link_count].token = *token;
        bluetooth.request_id = *token; bluetooth.request_status = BT_AUDIO_JOB_PENDING; ++bluetooth.generation;
    }
    ++link_count; return link_result;
}
static ae_result_t codec_caps(void *unused, ae_codec_capabilities_t *out) { (void)unused; *out = hardware; return AE_OK; }
ae_codec_port_t board_audio_mosaico_port(void) { return (ae_codec_port_t){.capabilities = codec_caps}; }
ae_result_t app_media_runtime_route(ae_route_t selected, ae_route_capabilities_t *out)
{ (void)selected; *out = route; return AE_OK; }
ae_result_t app_media_runtime_submit_local(const app_media_local_request_t *request)
{ ++submissions; submitted = *request; return submit_result; }
ae_result_t app_media_runtime_copy_records(uint64_t token, ae_record_metadata_t *out, size_t capacity)
{ assert(token == owner.request_token && owner.result_ready && capacity == APP_MEDIA_ROWS); memcpy(out, listed, sizeof(listed)); return records_copy_result; }
bool app_media_runtime_take_result(uint64_t *token, ae_result_t *result)
{ if (!owner.result_ready) return false; *token = owner.request_token; *result = owner.result; owner.result_ready = false; return true; }
ae_result_t app_media_runtime_confirm(uint64_t token, uint64_t epoch, uint64_t revision, char *candidate)
{ assert(token == owner.speech.token && epoch == context.epoch && revision == context.ui_revision); ++confirmations; *candidate = owner.speech.candidate; owner.speech.state = SL_IDLE; return AE_OK; }
ae_result_t app_media_runtime_reject(uint64_t token, uint64_t epoch, uint64_t revision)
{ assert(token == owner.speech.token && epoch == context.epoch && revision == context.ui_revision); ++rejections; owner.speech.state = SL_REJECTED; return AE_OK; }

static void poll_at(uint64_t now)
{
    app_media_controls_effect_t effect; app_media_controls_context(&context); app_media_controls_poll(now, &effect);
    const unsigned char *bytes = (const unsigned char *)&poll_scratch;
    for (size_t i = 0; i < sizeof(poll_scratch); ++i) assert(bytes[i] == 0);
    app_media_controls_fill(&view);
}
static void setup(void)
{
    owner = (app_media_snapshot_t){.initialized = true, .audio_known = true, .speech_known = true,
        .audio = {.audio_generation = 3, .revision = 5, .state = AE_OFF, .route = AE_ROUTE_LOCAL,
            .hardware_off_confirmed = true, .microphone_muted = true, .speaker_muted = true},
        .speech = {.state = SL_IDLE}};
    disk = (app_recording_store_snapshot_t){.initialized = true, .byte_region_owned = true,
        .store = {.initialized = true, .mounted = true},
        .capabilities = {.storage = AE_STORAGE_RECORDS_PARTITION, .implemented = true, .available = true,
            .geometry_verified = true, .owned_and_provisioned = true, .encryption_ready = true,
            .authenticated_metadata = true, .recovery_complete = true, .retention_time_known = true,
            .usable_bytes = 827392, .quota_bytes = 524288, .per_object_overhead_bytes = 8192,
            .media_chunk_bytes = 4048, .media_chunk_overhead_bytes = 48, .object_limit = 4}};
    physical = (board_recording_flash_status_t){.state = BOARD_RECORDING_FLASH_OWNED, .geometry_verified = true,
        .partition_bytes = 851968, .capacity_bytes = 847872};
    hardware = (ae_codec_capabilities_t){.implemented = true, .available = true, .safe_volume_max = 60, .format_count = 1,
        .formats = {{{16000, 1, 10, AE_PCM_S16_LE}, AE_MICROPHONE | AE_SPEAKER, false}}};
    route = (ae_route_capabilities_t){.implemented = true, .available = true, .connected = true,
        .capture = true, .playback = true, .format = {16000, 1, 10, AE_PCM_S16_LE}};
    context = (app_media_controls_context_t){.epoch = 11, .ui_revision = 13, .mode = INPUT_AUDIO,
        .overlay = APP_OVERLAY_RECORDING_START};
    assert(app_media_controls_init()); assert(!app_media_controls_init()); poll_at(1000);
}
static app_intent_t intent(app_media_operation_t operation)
{
    return (app_intent_t){.action = APP_MEDIA_CONTROL, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .media = {.operation = operation, .management_generation = view.media.management_generation,
            .audio_generation = view.media.audio_generation, .resource_revision = view.media.resource_revision, .peer_id = view.media.peer_id}};
}
static app_media_controls_effect_t handle(app_intent_t request)
{
    app_media_controls_effect_t effect;
    assert(app_media_controls_handle(&request, &effect)); assert(effect.handled); return effect;
}
static void apply_effect(app_media_controls_effect_t effect)
{
    assert(effect.open_overlay); context.overlay = effect.overlay; ++context.ui_revision;
    app_media_controls_context(&context); app_media_controls_fill(&view);
}
static void complete(ae_result_t result)
{
    owner.request_token = submitted.token; owner.result = result; owner.result_ready = true; owner.busy = false;
    poll_at(controls.now_ms + 10);
}
static void test_review_and_quota(void)
{
    setup(); assert(view.recording_storage.capacity_known && view.recording_storage.free_bytes == 524288);
    assert(view.media.recording_options.maximum_duration_seconds == 15 && view.media.recording_options.can_start);
    app_media_controls_effect_t effect = handle(intent(APP_MEDIA_RECORD_START_REQUEST));
    assert(!submissions && effect.confirmation == APP_CONFIRM_RECORDING_START); apply_effect(effect);
    assert(view.media_review.valid && view.media_review.options.duration_seconds == 5);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .x = 1, .media = view.media_review.request};
    effect = handle(confirm);
    assert(submissions == 1 && submitted.action == APP_MEDIA_START && submitted.kind == AE_CAPTURE_RECORDING);
    assert(submitted.expected_audio_generation == 3 && submitted.expected_audio_revision == 5);
    assert(submitted.duration_seconds == 5 && submitted.byte_limit == 524288 && submitted.retention_seconds == 3600);
    assert(submitted.storage == AE_STORAGE_RECORDS_PARTITION && submitted.export_policy == AE_EXPORT_NONE);
    assert(!effect.preview_character);
    complete(AE_OK); assert(!controls.pending);
    handle(confirm); assert(submissions == 1); /* Owned expired review never falls through. */
}
static void test_stale_and_failure(void)
{
    setup(); app_intent_t old = intent(APP_MEDIA_RECORD_START_REQUEST);
    ++owner.audio.revision; poll_at(1010); handle(old); assert(!submissions && !controls.review.valid);
    app_media_controls_effect_t effect = handle(intent(APP_MEDIA_RECORD_START_REQUEST)); apply_effect(effect);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1, .media = view.media_review.request};
    disk.capabilities.reserved_bytes = 262144; poll_at(1020); handle(confirm); assert(!submissions && !controls.review.valid);
    context.overlay = APP_OVERLAY_RECORDING_START; ++context.ui_revision; poll_at(1030);
    effect = handle(intent(APP_MEDIA_RECORD_START_REQUEST)); apply_effect(effect);
    confirm = (app_intent_t){.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1, .media = view.media_review.request};
    submit_result = AE_BUSY; handle(confirm); assert(submissions == 1 && !controls.pending);
    poll_at(1040); assert(submissions == 1); /* No automatic mutation retry. */
    poll_at(999); assert(controls.clock_failed);
    handle(intent(APP_MEDIA_RECORD_START_REQUEST)); assert(submissions == 1);
}
static void test_list_object_identity(void)
{
    setup(); context.overlay = APP_OVERLAY_RECORDINGS; ++context.ui_revision; poll_at(1010);
    assert(submissions == 1 && submitted.action == APP_MEDIA_QUERY_LIST && submitted.capacity == 4);
    listed[0] = (ae_record_metadata_t){.object_id = {{31}}, .revision = 41, .state = AE_RECORD_COMPLETED,
        .format = {16000, 1, 10, AE_PCM_S16_LE}, .sample_frames = 16000, .pcm_bytes = 32000,
        .stored_bytes = 40000, .integrity_verified = true, .sync_acknowledged = true, .hash_available = true};
    owner.result_detail.count = 1; owner.result_detail.next_cursor = 0; complete(AE_OK);
    assert(view.media.recordings.known && view.media.recordings.row_count == 1 && !view.media.recordings.has_next);
    app_intent_t select = intent(APP_MEDIA_RECORD_SELECT); select.media.list_generation = view.media.recordings.generation;
    select.media.object_id = view.media.recordings.rows[0].id; select.media.object_revision = 40;
    handle(select); assert(submissions == 1);
    select.media.object_revision = 41; apply_effect(handle(select)); assert(submissions == 2 && submitted.recording_id.bytes[0] == 31);
    owner.result_detail.metadata = listed[0]; complete(AE_OK);
    assert(view.media.selected_recording.valid && view.media.selected_recording.can_play);
    app_intent_t remove = intent(APP_MEDIA_RECORD_DELETE_REQUEST); remove.media.object_id = view.media.selected_recording.id; remove.media.object_revision = 41;
    apply_effect(handle(remove)); assert(submissions == 2);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1, .media = view.media_review.request};
    handle(confirm); assert(submitted.action == APP_MEDIA_QUERY_DELETE && submitted.object_revision == 41);
    owner.result_detail.access_revoked = true; owner.result_detail.cleanup_complete = false; complete(AE_OK);
    assert(!view.media.selected_recording.can_play && !view.media.selected_recording.valid);
}
static void test_storage_prepare(void)
{
    setup(); disk.capabilities.available = disk.capabilities.owned_and_provisioned = false;
    disk.store.mounted = false; disk.byte_region_owned = false; disk.error = AE_UNAVAILABLE;
    physical.state = BOARD_RECORDING_FLASH_ERASED;
    context.overlay = APP_OVERLAY_RECORDING_STORAGE; ++context.ui_revision; poll_at(1010);
    assert(view.recording_storage.can_prepare && view.recording_storage.state == APP_RECORD_STORE_UNPREPARED);
    app_intent_t prepare = {.action = APP_RECORDING_STORAGE_PREPARE_REQUEST, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .recording_storage_generation = view.recording_storage.generation, .recording_storage_revision = view.recording_storage.revision};
    apply_effect(handle(prepare)); assert(!submissions);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1,
        .recording_storage_generation = view.recording_storage.generation, .recording_storage_revision = view.recording_storage.revision};
    handle(confirm); assert(submissions == 1 && submitted.action == APP_MEDIA_PREPARE_STORE && submitted.quota_bytes == 524288 && submitted.object_limit == 4);
    complete(AE_UNCERTAIN); assert(view.recording_storage.state != APP_RECORD_STORE_READY);
    physical.state = BOARD_RECORDING_FLASH_UNKNOWN; poll_at(1030); assert(!view.recording_storage.can_prepare);
}
static void test_speech_once(void)
{
    setup(); context.mode = INPUT_KEYBOARD; context.keyboard_submode = APP_KEYBOARD_SPOKEN; context.overlay = APP_OVERLAY_NONE;
    context.preview_characters = APP_UI_DRAFT_CAPACITY; ++context.ui_revision;
    owner.speech = (sl_view_t){.state = SL_CANDIDATE, .token = 51, .epoch = context.epoch, .ui_revision = context.ui_revision, .candidate = 'G'};
    poll_at(1010); assert(!view.media.speech.can_confirm && view.media.speech.can_reject);
    context.preview_characters = 2; poll_at(1020); assert(view.media.speech.can_confirm);
    app_intent_t candidate = intent(APP_MEDIA_SPEECH_CONFIRM);
    candidate.media.speech_generation = view.media.speech.generation; candidate.media.speech_revision = view.media.speech.revision;
    candidate.media.candidate_id = view.media.speech.candidate_id; candidate.media.value.candidate = 'G';
    app_media_controls_effect_t effect = handle(candidate); assert(effect.preview_character == 'G' && confirmations == 1 && !submissions);
    app_media_controls_fill(&view);
    assert(!view.media.speech.candidate && !view.media.speech.can_confirm && !view.media.speech.can_reject);
    effect = handle(candidate); assert(!effect.preview_character && confirmations == 1);
    owner.speech.state = SL_CANDIDATE; owner.speech.token = 53; owner.speech.candidate = '9'; poll_at(1030);
    candidate = intent(APP_MEDIA_SPEECH_REJECT); candidate.media.speech_generation = view.media.speech.generation;
    candidate.media.speech_revision = view.media.speech.revision; candidate.media.candidate_id = view.media.speech.candidate_id; candidate.media.value.candidate = '9';
    effect = handle(candidate); assert(rejections == 1 && !effect.preview_character && !submissions);
}
static void test_targeted_stop_and_idle(void)
{
    setup(); owner.audio.capture[AE_CAPTURE_RECORDING] = (ae_job_snapshot_t){.kind = AE_CAPTURE_RECORDING, .state = AE_JOB_RUNNING,
        .job_id = {{61}}, .consumer_generation = 71};
    owner.audio.capture_gate_open = owner.audio.capture_indicator_required = true; owner.audio.hardware_off_confirmed = false;
    ++owner.audio.revision; poll_at(1010);
    assert(view.capture_indicator_required && view.capture_indicator_generation == 3 && view.capture_state == APP_CAPTURE_ACTIVE);
    uint64_t revision = view.media.resource_revision;
    for (uint64_t i = 1011; i < 1100; ++i) { ++owner.audio.capture[AE_CAPTURE_RECORDING].sample_frames; poll_at(i); assert(view.media.resource_revision == revision); }
    app_intent_t stop = intent(APP_MEDIA_RECORD_STOP); stop.media.job_id = view.media.recording.id; stop.media.consumer_generation = 70;
    handle(stop); assert(!submissions);
    stop.media.consumer_generation = 71; handle(stop);
    assert(submissions == 1 && submitted.action == APP_MEDIA_STOP_JOB && submitted.job_id.bytes[0] == 61 && submitted.consumer_generation == 71 && submitted.stop_reason == AE_STOP_REQUESTED);
}
static void test_live_and_saved_peer(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    ++owner.audio.revision; poll_at(1010); assert(view.media.microphone.can_start);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(submissions == 1 && submitted.action == APP_MEDIA_START && submitted.kind == AE_CAPTURE_LIVE &&
        submitted.duration_seconds == 3600 && submitted.route == AE_ROUTE_USB_UAC && submitted.storage == AE_STORAGE_NONE);
    complete(AE_OK);
    owner.audio.route = AE_ROUTE_HFP_HF; ++owner.audio.revision;
    bluetooth = (bt_audio_snapshot_t){.initialized = true, .generation = 19};
    bonds = (bt_audio_bond_list_t){.known = true, .revision = 29, .count = 2};
    bonds.bonds[0].address[5] = 1; bonds.bonds[1].address[5] = 2;
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; poll_at(1030);
    assert(view.media.can_select_peer && !view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect);
    app_intent_t select = intent(APP_MEDIA_SELECT_PEER); select.media.value.peer_id = view.media.next_peer_id;
    handle(select); app_media_controls_fill(&view);
    assert(submissions == 1 && view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect && view.media.peer_id.bytes[15] == 1);
    app_intent_t stale = intent(APP_MEDIA_LINK_CONNECT); ++bonds.revision; poll_at(1040);
    handle(stale); assert(submissions == 1);
    handle(intent(APP_MEDIA_LINK_CONNECT));
    assert(submissions == 1 && link_count == 1 && links[0].command == BT_AUDIO_CONNECT_HFP && links[0].peer[5] == 1 &&
        links[0].generation == 19 && links[0].bond_revision == 30);
    bonds.known = false; poll_at(1060); assert(!controls.voice_link.active);
    assert(!view.media.can_select_peer && !view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect);
}
static void setup_voice(bool slc)
{
    setup(); owner.audio.route = AE_ROUTE_HFP_HF; ++owner.audio.revision;
    route.available = route.connected = false;
    bluetooth = (bt_audio_snapshot_t){.initialized = true, .generation = 19, .hfp_connected = slc,
        .hfp_peer_known = slc, .classic_hfp_peer_bonded = slc, .microphone_muted = true, .speaker_muted = true};
    if (slc) bluetooth.hfp_peer[5] = 1;
    bonds = (bt_audio_bond_list_t){.known = true, .revision = 29, .count = 2};
    bonds.bonds[0].address[5] = 1; bonds.bonds[1].address[5] = 2;
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; poll_at(1010);
    if (!slc) {
        app_intent_t select = intent(APP_MEDIA_SELECT_PEER); select.media.value.peer_id = view.media.next_peer_id;
        handle(select); app_media_controls_fill(&view);
    }
    assert(view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect && !view.media.microphone.can_start);
}
static void slc_succeeded(void)
{
    bluetooth.request_status = BT_AUDIO_JOB_SUCCEEDED; bluetooth.control_inflight = false;
    bluetooth.hfp_connected = bluetooth.hfp_peer_known = bluetooth.classic_hfp_peer_bonded = true;
    bluetooth.hfp_peer[5] = 1; ++bluetooth.generation;
    poll_at(controls.now_ms + 10);
}
static void test_voice_connect_sequence(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT));
    assert(link_count == 1 && links[0].command == BT_AUDIO_CONNECT_HFP);
    app_media_controls_fill(&view); assert(view.media.command_pending && !view.media.microphone.can_start);
    bluetooth.request_status = BT_AUDIO_JOB_WAITING; bluetooth.control_inflight = true; poll_at(1020);
    assert(link_count == 1 && !submissions);
    slc_succeeded();
    assert(link_count == 2 && links[1].command == BT_AUDIO_OPEN_VOICE && links[1].peer[5] == 1 &&
        links[1].bond_revision == 29 && links[1].token != links[0].token && controls.voice_link.active);
    assert(view.media.command_pending && !view.media.microphone.can_start && !submissions);
    bluetooth.request_status = BT_AUDIO_JOB_SUCCEEDED; bluetooth.hfp_audio_connected = bluetooth.hfp_format_known = true;
    bluetooth.hfp_sample_rate = 16000; ++bluetooth.generation; route.available = route.connected = true;
    poll_at(1040);
    assert(!view.media.command_pending && !view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect &&
        view.media.routes[APP_MEDIA_ROUTE_HFP].can_disconnect && view.media.microphone.can_start);
    assert(!submissions && !owner.audio.microphone_enabled && !owner.audio.capture_gate_open && bluetooth.microphone_muted);
    poll_at(1050); assert(link_count == 2); /* Connection never creates a live consumer. */
}
static void test_voice_existing_slc(void)
{
    setup_voice(true); handle(intent(APP_MEDIA_LINK_CONNECT)); app_media_controls_fill(&view);
    assert(link_count == 1 && links[0].command == BT_AUDIO_OPEN_VOICE && links[0].peer[5] == 1);
    handle(intent(APP_MEDIA_LINK_CONNECT)); assert(link_count == 1);
    bluetooth.request_status = BT_AUDIO_JOB_SUCCEEDED; /* A success token without actual SCO is insufficient. */
    poll_at(1020); assert(!controls.voice_link.active && !view.media.microphone.can_start);
    assert(strstr(view.media.reason, "unconfirmed") && !submissions);
    poll_at(1030); assert(link_count == 1);
}
static void test_voice_async_failure(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT));
    bluetooth.request_status = BT_AUDIO_JOB_FAILED; poll_at(1020);
    assert(!controls.voice_link.active && link_count == 1 && !submissions);
    poll_at(1030); assert(link_count == 1);
    handle(intent(APP_MEDIA_LINK_CONNECT)); slc_succeeded();
    assert(link_count == 3 && links[2].command == BT_AUDIO_OPEN_VOICE);
    bluetooth.request_status = BT_AUDIO_JOB_FAILED; poll_at(1050);
    assert(!controls.voice_link.active && view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect &&
        view.media.routes[APP_MEDIA_ROUTE_HFP].can_disconnect && !view.media.microphone.can_start);
    poll_at(1060); assert(link_count == 3 && !submissions);
}
static void test_voice_replaced_token(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT)); ++bluetooth.request_id; slc_succeeded();
    assert(!controls.voice_link.active && link_count == 1 && !submissions);
    poll_at(1040); assert(link_count == 1);
}
static void test_voice_replaced_peer(void)
{
    setup_voice(true); handle(intent(APP_MEDIA_LINK_CONNECT)); bluetooth.hfp_peer[5] = 2;
    bluetooth.request_status = BT_AUDIO_JOB_SUCCEEDED; bluetooth.hfp_audio_connected = bluetooth.hfp_format_known = true;
    bluetooth.hfp_sample_rate = 8000; poll_at(1020);
    assert(!controls.voice_link.active && link_count == 1 && strstr(view.media.reason, "State changed"));
    bluetooth.hfp_peer[5] = 1; poll_at(1030); assert(link_count == 1 && !submissions);
}
static void test_voice_catalogue_changed(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT)); ++bonds.revision; slc_succeeded();
    assert(!controls.voice_link.active && link_count == 1 && !submissions);
}
static void test_voice_locked(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT)); context.locked = true; poll_at(1020);
    context.locked = false; slc_succeeded();
    assert(!controls.voice_link.active && link_count == 1 && !submissions);
}
static void test_voice_timeout(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT)); poll_at(16010);
    assert(!controls.voice_link.active && link_count == 1 && strstr(view.media.reason, "timed out"));
    slc_succeeded(); assert(link_count == 1 && !submissions);
}
static void test_voice_snapshot_unavailable(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT)); snapshot_available = false; poll_at(1020);
    assert(!controls.voice_link.active && link_count == 1 && strstr(view.media.reason, "unavailable"));
    snapshot_available = true; slc_succeeded(); assert(link_count == 1 && !submissions);
}
static void test_voice_submit_rejected(void)
{
    setup_voice(false); handle(intent(APP_MEDIA_LINK_CONNECT)); link_result = ESP_ERR_INVALID_STATE; slc_succeeded();
    assert(!controls.voice_link.active && link_count == 2 && !submissions);
    link_result = ESP_OK; poll_at(1040); assert(link_count == 2);
}
static void test_capture_and_mute_evidence(void)
{
    setup(); assert(!view.media.microphone.mute_confirmed && !view.media.speaker.mute_confirmed && !view.microphone_muted);
    owner.audio.microphone_mute_confirmed = owner.audio.microphone_mute_applied = true; ++owner.audio.revision; poll_at(1010);
    assert(view.microphone_muted && view.media.microphone.mute_confirmed && !view.media.speaker.mute_confirmed);
    owner.audio.speaker_mute_confirmed = owner.audio.speaker_mute_applied = true; ++owner.audio.revision; poll_at(1020);
    assert(view.media.speaker.mute_confirmed);
    owner.audio.capture_indicator_required = owner.audio.capture_gate_open = true;
    owner.audio.hardware_off_confirmed = false; ++owner.audio.revision; poll_at(1030);
    assert(view.capture_indicator_required);
    snapshot_available = false; poll_at(1040);
    assert(view.capture_indicator_required && view.capture_indicator_generation == 3 && view.capture_state == APP_CAPTURE_UNCERTAIN);
    snapshot_available = true; owner.audio.capture_indicator_required = owner.audio.capture_gate_open = false;
    ++owner.audio.revision; poll_at(1050); assert(view.capture_indicator_required);
    owner.audio.hardware_off_confirmed = true; ++owner.audio.revision; poll_at(1060);
    assert(!view.capture_indicator_required && view.capture_state == APP_CAPTURE_OFF_CONFIRMED);
}
static void test_snapshot_reuse(void)
{
    setup();
    uint64_t revision = view.media.resource_revision;
    uint64_t storage_revision = view.recording_storage.revision;
    poll_at(1010); assert(view.media.resource_revision == revision && view.recording_storage.revision == storage_revision);
    ++disk.capabilities.reserved_bytes; ++owner.audio.revision;
    owner.speech.token = 91; owner.speech.state = SL_CANDIDATE; owner.speech.candidate = 'Q';
    bonds.known = true; bonds.revision = 92; bonds.count = BT_AUDIO_CLASSIC_BONDS_MAX;
    for (unsigned i = 0; i < bonds.count; ++i) bonds.bonds[i].address[5] = (uint8_t)(i + 1);
    poll_at(1020);
    assert(view.media.resource_revision > revision && view.recording_storage.revision > storage_revision);
    assert(controls.runtime.speech.candidate == 'Q' && controls.bonds.bonds[BT_AUDIO_CLASSIC_BONDS_MAX - 1].address[5] == BT_AUDIO_CLASSIC_BONDS_MAX);
    revision = view.media.resource_revision; storage_revision = view.recording_storage.revision;
    uint64_t speech_revision = view.media.speech.revision;
    poll_at(1030); assert(view.media.resource_revision == revision && view.recording_storage.revision == storage_revision && view.media.speech.revision == speech_revision);
    snapshot_available = store_snapshot_available = false; poll_at(1040);
    assert(!controls.runtime_known && !controls.store_known && !controls.runtime.speech.candidate && !controls.store.capabilities.quota_bytes);
    assert(!view.audio_available && !view.recording_storage.capacity_known);
    snapshot_available = store_snapshot_available = true; poll_at(1050);
    assert(controls.runtime.speech.candidate == 'Q' && view.recording_storage.capacity_known && view.media.speech.revision > speech_revision);
}
static void test_record_copy_failure(void)
{
    setup(); context.overlay = APP_OVERLAY_RECORDINGS; ++context.ui_revision; poll_at(1010);
    listed[0] = (ae_record_metadata_t){.object_id = {{31}}, .revision = 41};
    owner.result_detail.count = 1; owner.result_detail.next_cursor = 0;
    records_copy_result = AE_UNCERTAIN; complete(AE_OK);
    assert(!controls.pending && !view.media.recordings.known && !controls.records[0].revision);
    assert(!owner.result_ready && strstr(view.media.recordings.reason, "unconfirmed"));
    unsigned prior = submissions; poll_at(controls.now_ms + 10); assert(submissions == prior);
    records_copy_result = AE_OK; controls.list_requested = true; poll_at(controls.now_ms + 10);
    complete(AE_OK); assert(view.media.recordings.known && view.media.recordings.row_count == 1 && controls.records[0].revision == 41);
}
int main(int argc, char **argv)
{
    const char *names[] = {"review_and_quota", "stale_and_failure", "list_object_identity", "storage_prepare", "speech_once", "targeted_stop_and_idle", "live_and_saved_peer", "capture_and_mute_evidence", "voice_connect_sequence", "voice_existing_slc", "voice_async_failure", "voice_replaced_token", "voice_replaced_peer", "voice_catalogue_changed", "voice_locked", "voice_timeout", "voice_snapshot_unavailable", "voice_submit_rejected", "snapshot_reuse", "record_copy_failure"};
    void (*cases[])(void) = {test_review_and_quota, test_stale_and_failure, test_list_object_identity, test_storage_prepare, test_speech_once, test_targeted_stop_and_idle, test_live_and_saved_peer, test_capture_and_mute_evidence, test_voice_connect_sequence, test_voice_existing_slc, test_voice_async_failure, test_voice_replaced_token, test_voice_replaced_peer, test_voice_catalogue_changed, test_voice_locked, test_voice_timeout, test_voice_snapshot_unavailable, test_voice_submit_rejected, test_snapshot_reuse, test_record_copy_failure};
    assert(argc == 2);
    if (!strcmp(argv[1], "--list")) { for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) puts(names[i]); return 0; }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(argv[1], names[i])) { cases[i](); printf("PASS: media controls %s\n", names[i]); return 0; }
    return 2;
}
