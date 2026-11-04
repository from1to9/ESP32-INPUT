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
static ae_route_capabilities_t routes_by_id[AE_ROUTE_COUNT];
static bool distinct_routes;
static app_media_local_request_t submitted;
static unsigned submissions, confirmations, rejections;
static struct { bt_audio_command_t command; uint64_t generation, bond_revision, token; uint8_t peer[6]; } links[8];
static unsigned link_count;
static esp_err_t link_result = ESP_OK, pairing_result = ESP_OK;
static unsigned pairing_opens, pairing_closes, le_pairing_closes;
static uint64_t next_link_token = 100;
static ae_result_t submit_result = AE_OK;
static bool snapshot_available = true;
static bool store_snapshot_available = true;
static ae_result_t records_copy_result = AE_OK;
static ae_record_metadata_t listed[APP_MEDIA_ROWS];
static app_media_controls_context_t context;
static app_view_t view;
static board_audio_capture_snapshot_t capture_level;
static bool capture_clock_override;
static int64_t capture_clock_us;
int64_t esp_timer_get_time(void)
{
    if (capture_clock_override) return capture_clock_us;
    return controls.now_ms <= INT64_MAX / 1000U ? (int64_t)(controls.now_ms * 1000U) : INT64_MAX;
}

bool app_media_runtime_snapshot(app_media_snapshot_t *out) { if (snapshot_available) *out = owner; return snapshot_available; }
bool app_recording_store_snapshot(app_recording_store_snapshot_t *out)
{ if (store_snapshot_available) *out = disk; return store_snapshot_available; }
board_recording_flash_status_t board_recording_flash_status(void) { return physical; }
bool app_media_runtime_pairing_current(uint64_t generation, uint64_t revision)
{
    return snapshot_available && owner.initialized && !owner.busy && !owner.result_ready && owner.audio_known &&
        owner.audio.hardware_off_confirmed && owner.audio.audio_generation == generation && owner.audio.revision == revision;
}
#ifndef TEST_MEDIA_REAL_BLUETOOTH
esp_err_t bt_audio_pairing_open_reviewed(uint32_t duration, const bt_audio_snapshot_t *expected, uint64_t revision,
                                        bool (*resources_current)(void))
{
    assert(duration == BT_AUDIO_PAIRING_DEFAULT_MS);
    if (expected->generation != bluetooth.generation || revision != bonds.revision || !resources_current())
        return ESP_ERR_INVALID_VERSION;
    if (pairing_result == ESP_OK) ++le_pairing_closes;
    ++pairing_opens;
    bluetooth.pairing_error = ESP_OK;
    if (pairing_result == ESP_OK) { bluetooth.pairing_busy = bluetooth.pairing_requested = true; bluetooth.pairing_remaining_ms = duration; bluetooth.pairing_completed = bluetooth.pairing_expired = false; }
    return pairing_result;
}
void bt_audio_pairing_close(void) { ++pairing_closes; bluetooth.pairing_requested = false; }
void bt_audio_cancel_request(uint64_t token)
{
    if (token && bluetooth.request_id == token && bluetooth.request_status == BT_AUDIO_JOB_PENDING)
        bluetooth.request_status = BT_AUDIO_JOB_CANCELLED;
}
bt_audio_snapshot_t bt_audio_snapshot(void) { return bluetooth; }
void bt_audio_copy_bond_list(bt_audio_bond_list_t *out) { assert(out); *out = bonds; }
esp_err_t bt_audio_saved_request(uint64_t generation, uint64_t link_generation, uint64_t revision, bt_audio_command_t command,
                               const uint8_t peer[6], uint64_t *token)
{
    assert(link_count < 8 && peer && token && link_generation == (command == BT_AUDIO_CONNECT_A2DP || command == BT_AUDIO_DISCONNECT_A2DP ? bluetooth.a2dp_link_generation : bluetooth.hfp_link_generation));
    links[link_count].command = command; links[link_count].generation = generation;
    links[link_count].bond_revision = revision; memcpy(links[link_count].peer, peer, 6);
    if (link_result == ESP_OK) {
        *token = ++next_link_token; links[link_count].token = *token;
        bluetooth.request_id = *token; bluetooth.request_status = BT_AUDIO_JOB_PENDING; ++bluetooth.generation;
    }
    ++link_count; return link_result;
}
#endif
static ae_result_t codec_caps(void *unused, ae_codec_capabilities_t *out) { (void)unused; *out = hardware; return AE_OK; }
ae_codec_port_t board_audio_mosaico_port(void) { return (ae_codec_port_t){.capabilities = codec_caps}; }
void board_audio_capture_snapshot(board_audio_capture_snapshot_t *out) { *out = capture_level; }
ae_result_t app_media_runtime_route(ae_route_t selected, ae_route_capabilities_t *out)
{ assert((unsigned)selected < AE_ROUTE_COUNT); *out = distinct_routes ? routes_by_id[selected] : route; return AE_OK; }
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
        .capture = true, .playback = true, .format = {16000, 1, 10, AE_PCM_S16_LE},
        .token = {.peer_id = {{1}}, .stream_id = {{2}}, .link_generation = 3}};
    context = (app_media_controls_context_t){.epoch = 11, .ui_revision = 13, .mode = INPUT_AUDIO,
        .overlay = APP_OVERLAY_RECORDING_START};
    assert(app_media_controls_init()); assert(!app_media_controls_init());
    /* Timing-independent cases begin with an explicitly observed stable host. */
    poll_at(0); poll_at(1000);
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
    assert(effect.open_overlay); context.overlay = effect.overlay; ++context.epoch; ++context.ui_revision;
    if (effect.overlay == APP_OVERLAY_CONFIRM) app_media_controls_review_opened(effect.confirmation, context.epoch);
    app_media_controls_context(&context); app_media_controls_fill(&view);
}
static void complete(ae_result_t result)
{
    owner.request_token = submitted.token; owner.result = result; owner.result_ready = true; owner.busy = false;
    poll_at(controls.now_ms + 10);
}
static void pairing_setup(void)
{
    setup(); context.overlay = APP_OVERLAY_MEDIA_OPTIONS; owner.audio.route = AE_ROUTE_HFP_HF;
    bluetooth = (bt_audio_snapshot_t){.supported = true, .initialized = true, .classic_bonds_known = true,
        .device_name_known = true};
    strcpy(bluetooth.device_name, "Classic Audio"); poll_at(1010);
    assert(view.media.pairing.can_open && !strcmp(view.media.pairing.device_name, "Classic Audio"));
}
static app_media_controls_effect_t pair_confirm(app_media_operation_t operation)
{
    app_intent_t request = intent(operation); app_media_controls_effect_t effect = handle(request);
    assert(effect.open_overlay && effect.confirmation == APP_CONFIRM_AUDIO_PAIR);
    apply_effect(effect);
    request = intent(operation); request.action = APP_CONFIRM; request.x = 1;
    effect = handle(request); apply_effect(effect); poll_at(controls.now_ms + 1U); return effect;
}
static void test_pairing_window(void)
{
    pairing_setup(); pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    assert(pairing_opens == 1 && !submissions && !link_count);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_PREPARING && view.media.pairing.can_close);
    assert(view.media.pairing.remaining_seconds == 120);
    bluetooth.discovery_known = true; bluetooth.discoverable = true; poll_at(1020);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_DISCOVERABLE && !view.media.pairing.can_open);
    assert(!view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect);
    context.overlay = APP_OVERLAY_NONE; poll_at(1021);
    assert(!view.media.microphone.can_start && !view.media.speaker.can_start);
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; poll_at(1022);
    app_media_controls_effect_t effect = handle(intent(APP_MEDIA_PAIR_AUDIO_CLOSE)); apply_effect(effect);
    uint64_t revision = view.media.resource_revision;
    bluetooth.pairing_remaining_ms = 110999; poll_at(1030);
    assert(view.media.resource_revision == revision && view.media.pairing.remaining_seconds == 111);
    app_intent_t close = intent(APP_MEDIA_PAIR_AUDIO_CLOSE); close.action = APP_CONFIRM; close.x = 1;
    effect = handle(close); apply_effect(effect); poll_at(1031);
    assert(pairing_closes == 1 && view.media.pairing.state == APP_MEDIA_PAIR_CLOSING && !view.media.pairing.can_open);
    bluetooth.pairing_busy = bluetooth.discoverable = false; poll_at(1032);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_CLOSED && view.media.pairing.can_open);
    assert(!submissions && !link_count);
}
static void test_pairing_outcomes(void)
{
    pairing_setup(); bluetooth.device_name_known = false; poll_at(1020);
    assert(!view.media.pairing.name_known && !*view.media.pairing.device_name && view.media.pairing.can_open);
    pairing_result = ESP_ERR_NO_MEM; pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_ERROR && strstr(view.media.pairing.reason, "full"));
    pairing_result = ESP_OK; pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    bluetooth.pairing_handshake_pending = true; poll_at(1030);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_AUTHENTICATING);
    bluetooth.pairing_busy = bluetooth.pairing_handshake_pending = bluetooth.pairing_requested = false;
    bluetooth.pairing_completed = true; poll_at(1031);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_COMPLETED && !submissions);
    bluetooth.pairing_completed = false; bluetooth.pairing_expired = true; poll_at(1032);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_EXPIRED && view.media.pairing.can_open);
}
static void test_pairing_recovery_and_link(void)
{
    pairing_setup();
    bluetooth.pairing_handshake_pending = bluetooth.pairing_busy = true;
    bluetooth.pairing_error = ESP_ERR_TIMEOUT; poll_at(1030);
    assert(!view.media.pairing.can_open && strstr(view.media.pairing.reason, "Waiting") &&
        !strstr(view.media.pairing.reason, "Try again"));
    bluetooth.pairing_handshake_pending = false; bluetooth.pairing_error = ESP_FAIL; poll_at(1040);
    assert(!view.media.pairing.can_open && strstr(view.media.pairing.reason, "close"));
    bluetooth.pairing_busy = false; poll_at(1050);
    assert(view.media.pairing.can_open && strstr(view.media.pairing.reason, "Try again"));
    bluetooth.pairing_error = ESP_OK; bluetooth.classic_bond_count = 1; poll_at(1060);
    assert(strstr(view.media.pairing.reason, "saved"));
    bluetooth.hfp_peer_known = bluetooth.hfp_connected = true; poll_at(1070);
    assert(strstr(view.media.pairing.reason, "not paired"));
    bluetooth.classic_hfp_peer_bonded = true; poll_at(1080);
    assert(strstr(view.media.pairing.reason, "open voice"));
    bluetooth.hfp_audio_connected = true; poll_at(1090);
    assert(!view.media.routes[APP_MEDIA_ROUTE_HFP].can_connect && strstr(view.media.pairing.reason, "format unavailable"));
    owner.audio.route = AE_ROUTE_A2DP_SINK; ++owner.audio.revision;
    bluetooth.a2dp_peer_known = true; poll_at(1100);
    assert(!view.media.can_select_peer && !view.media.routes[APP_MEDIA_ROUTE_A2DP].can_connect);
    bluetooth.a2dp_connected = bluetooth.classic_a2dp_peer_bonded = true; poll_at(1110);
    assert(strstr(view.media.pairing.reason, "Start playback"));
}
static void test_pairing_context_and_stale(void)
{
    pairing_setup(); app_intent_t request = intent(APP_MEDIA_PAIR_AUDIO_OPEN);
    request.media.resource_revision++; handle(request); assert(!le_pairing_closes && !pairing_opens);
    bluetooth.control_inflight = true; poll_at(1011); assert(!view.media.pairing.can_open);
    bluetooth.control_inflight = false; poll_at(1012);
    owner.audio.hardware_off_confirmed = false; poll_at(1020); assert(!view.media.pairing.can_open);
    owner.audio.hardware_off_confirmed = true; poll_at(1021);
    app_media_controls_effect_t effect = handle(intent(APP_MEDIA_PAIR_AUDIO_OPEN)); apply_effect(effect);
    owner.audio.revision++; poll_at(1022);
    request = intent(APP_MEDIA_PAIR_AUDIO_OPEN); request.action = APP_CONFIRM; request.x = 1;
    effect = handle(request); apply_effect(effect); assert(!pairing_opens && !le_pairing_closes);
    pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    context.locked = true; poll_at(1030); assert(pairing_closes == 1);
    context.locked = false; context.mode = INPUT_MOUSE; poll_at(1031); assert(pairing_closes == 1);
    context.mode = INPUT_AUDIO; owner.audio.route = AE_ROUTE_USB_UAC; poll_at(1032);
    assert(!view.media.pairing.can_open && !view.media.pairing.can_close);
    handle(intent(APP_MEDIA_PAIR_AUDIO_OPEN)); assert(pairing_opens == 1);
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
static void test_discard_review(void)
{
    setup(); context.overlay = APP_OVERLAY_RECORDING_START;
    owner.audio.capture[AE_CAPTURE_RECORDING] = (ae_job_snapshot_t){.kind = AE_CAPTURE_RECORDING,
        .state = AE_JOB_RUNNING, .job_id = {{61}}, .consumer_generation = 71};
    ++owner.audio.revision; poll_at(1010);
    app_intent_t request = intent(APP_MEDIA_RECORD_CANCEL_DISCARD_REQUEST);
    request.media.job_id = view.media.recording.id; request.media.consumer_generation = 71;
    apply_effect(handle(request)); assert(view.media_review.valid && !submissions);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .x = 0, .media = view.media_review.request};
    apply_effect(handle(confirm)); assert(!submissions && !view.media_review.valid);
    context.overlay = APP_OVERLAY_RECORDING_START; ++context.ui_revision; poll_at(1020);
    request.epoch = context.epoch; request.ui_revision = context.ui_revision;
    apply_effect(handle(request)); assert(view.media_review.valid);
    confirm.epoch = context.epoch; confirm.ui_revision = context.ui_revision; confirm.x = 1;
    handle(confirm); assert(submissions == 1 && submitted.action == APP_MEDIA_STOP_JOB &&
        submitted.stop_reason == AE_STOP_CANCEL_DISCARD && submitted.consumer_generation == 71);
    handle(confirm); assert(submissions == 1);
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
    pairing_setup();
    apply_effect(handle(intent(APP_MEDIA_PAIR_AUDIO_OPEN)));
    app_intent_t cancel_pair = intent(APP_MEDIA_PAIR_AUDIO_OPEN); cancel_pair.action = APP_CONFIRM;
    apply_effect(handle(cancel_pair)); assert(!pairing_opens);
    disk.capabilities.available = disk.capabilities.owned_and_provisioned = false;
    disk.store.mounted = false; disk.byte_region_owned = false; disk.error = AE_UNAVAILABLE;
    physical.state = BOARD_RECORDING_FLASH_ERASED;
    context.overlay = APP_OVERLAY_RECORDING_STORAGE; ++context.ui_revision; poll_at(1010);
    assert(view.recording_storage.can_prepare && view.recording_storage.state == APP_RECORD_STORE_UNPREPARED);
    app_intent_t prepare = {.action = APP_RECORDING_STORAGE_PREPARE_REQUEST, .epoch = context.epoch, .ui_revision = context.ui_revision,
        .recording_storage_generation = view.recording_storage.generation, .recording_storage_revision = view.recording_storage.revision};
    apply_effect(handle(prepare)); assert(!submissions);
    app_intent_t confirm = {.action = APP_CONFIRM, .epoch = context.epoch, .ui_revision = context.ui_revision, .x = 1,
        .recording_storage_generation = view.recording_storage.generation, .recording_storage_revision = view.recording_storage.revision};
    app_intent_t cancel = confirm; cancel.x = 0;
    apply_effect(handle(cancel));
    assert(context.overlay == APP_OVERLAY_RECORDING_STORAGE && !submissions);
    prepare.epoch = context.epoch; prepare.ui_revision = context.ui_revision;
    apply_effect(handle(prepare)); confirm.epoch = context.epoch; confirm.ui_revision = context.ui_revision;
    apply_effect(handle(confirm)); assert(context.overlay == APP_OVERLAY_RECORDING_STORAGE);
    assert(submissions == 1 && submitted.action == APP_MEDIA_PREPARE_STORE && submitted.quota_bytes == 524288 && submitted.object_limit == 4);
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
static void test_connection_observed_completion(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; poll_at(1010);
    assert(!view.media.connection_known && view.media.can_select_connection[APP_MEDIA_CONNECTION_USB] &&
        view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH]);
    app_intent_t select = intent(APP_MEDIA_SELECT_CONNECTION);
    select.media.value.connection = APP_MEDIA_CONNECTION_USB;
    handle(select); app_media_controls_fill(&view);
    assert(submissions == 1 && submitted.action == APP_MEDIA_ROUTE && submitted.route == AE_ROUTE_USB_UAC &&
        submitted.expected_audio_generation == 3 && submitted.expected_audio_revision == 5);
    assert(view.media.command_pending && !view.media.connection_known &&
        !view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH]);
    handle(select); assert(submissions == 1);
    complete(AE_OK); /* A command result alone does not invent an observed route. */
    assert(!view.media.connection_known);
    owner.audio.route = AE_ROUTE_USB_UAC; ++owner.audio.audio_generation; ++owner.audio.revision;
    poll_at(1030);
    assert(view.media.connection_known && view.media.connection == APP_MEDIA_CONNECTION_USB &&
        !view.media.can_select_connection[APP_MEDIA_CONNECTION_USB]);
    select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_BLUETOOTH;
    handle(select); app_media_controls_fill(&view);
    assert(submissions == 2 && submitted.route == AE_ROUTE_HFP_HF &&
        view.media.connection_known && view.media.connection == APP_MEDIA_CONNECTION_USB);
    complete(AE_UNCERTAIN); poll_at(1050);
    assert(submissions == 2 && view.media.connection == APP_MEDIA_CONNECTION_USB);
    snapshot_available = false; poll_at(1060);
    assert(!view.media.connection_known && !view.media.can_select_connection[APP_MEDIA_CONNECTION_USB] &&
        !view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH]);
}
static void test_bluetooth_stream_after_idle_selection(void)
{
    setup(); distinct_routes = true;
    owner.audio.route = AE_ROUTE_HFP_HF;
    routes_by_id[AE_ROUTE_HFP_HF] = (ae_route_capabilities_t){.implemented = true, .available = true, .reason = AE_UNAVAILABLE};
    routes_by_id[AE_ROUTE_A2DP_SINK] = route; routes_by_id[AE_ROUTE_A2DP_SINK].capture = false;
    context.overlay = APP_OVERLAY_NONE; poll_at(1010);
    assert(view.media.speaker.can_start);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_SPEAKER;
    handle(start); assert(submissions == 1 && submitted.action == APP_MEDIA_START && submitted.route == AE_ROUTE_A2DP_SINK);
}
static void test_bluetooth_microphone_after_idle_selection(void)
{
    setup(); distinct_routes = true; owner.audio.route = AE_ROUTE_A2DP_SINK;
    routes_by_id[AE_ROUTE_A2DP_SINK] = (ae_route_capabilities_t){.implemented = true, .reason = AE_UNAVAILABLE};
    routes_by_id[AE_ROUTE_HFP_HF] = route; context.overlay = APP_OVERLAY_NONE; poll_at(1010);
    assert(view.media.microphone.can_start);
    routes_by_id[AE_ROUTE_A2DP_SINK].token.peer_id.bytes[0] = 99; poll_at(1011);
    assert(view.media.microphone.start_block == APP_MEDIA_START_PEER_MISMATCH);
    routes_by_id[AE_ROUTE_A2DP_SINK].token.peer_id = (ae_id_t){0};
    owner.audio.hardware_off_confirmed = false; poll_at(1012);
    assert(view.media.microphone.start_block == APP_MEDIA_START_STOP_REQUIRED);
    owner.audio.hardware_off_confirmed = true; poll_at(1013);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(submissions == 1 && submitted.route == AE_ROUTE_HFP_HF && submitted.kind == AE_CAPTURE_LIVE);
}
static void test_bluetooth_retained_owner_peer(void)
{
    setup(); distinct_routes = true; owner.audio.route = AE_ROUTE_A2DP_SINK;
    owner.audio.route_token.peer_id.bytes[0] = 77;
    routes_by_id[AE_ROUTE_A2DP_SINK] = (ae_route_capabilities_t){.implemented = true, .reason = AE_UNAVAILABLE};
    routes_by_id[AE_ROUTE_HFP_HF] = route; context.overlay = APP_OVERLAY_NONE; poll_at(1010);
    assert(view.media.microphone.start_block == APP_MEDIA_START_PEER_MISMATCH);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(!submissions);
    owner.audio.route_token.peer_id = route.token.peer_id; poll_at(1011);
    assert(view.media.microphone.can_start);
}
static void test_pairing_close_pending(void)
{
    pairing_setup(); pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    bluetooth.discovery_known = bluetooth.discoverable = true; poll_at(1020);
    pair_confirm(APP_MEDIA_PAIR_AUDIO_CLOSE);
    assert(!view.media.pairing.can_open && view.media.pairing.state != APP_MEDIA_PAIR_CLOSED);
}
static void test_pairing_old_capacity_error(void)
{
    pairing_setup(); pairing_result = ESP_ERR_NO_MEM; pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    assert(strstr(view.media.pairing.reason, "full"));
    bluetooth.failed = true; bluetooth.pairing_error = ESP_FAIL; poll_at(1050);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_ERROR && !strstr(view.media.pairing.reason, "full"));
}
static void test_pairing_new_capacity_error(void)
{
    pairing_setup(); bluetooth.pairing_error = ESP_FAIL; poll_at(1020);
    pairing_result = ESP_ERR_NO_MEM; pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_ERROR && strstr(view.media.pairing.reason, "full"));
}
static void test_pairing_error_origin(void)
{
    pairing_setup(); bluetooth.failed = true; bluetooth.pairing_error = ESP_ERR_NO_MEM; poll_at(1020);
    assert(view.media.pairing.state == APP_MEDIA_PAIR_ERROR && !strstr(view.media.pairing.reason, "full"));
}
static void test_connection_profile_capabilities(void)
{
    setup(); distinct_routes = true;
    route.token.peer_id.bytes[0] = 17;
    routes_by_id[AE_ROUTE_USB_UAC] = (ae_route_capabilities_t){.implemented = true, .reason = AE_UNAVAILABLE};
    routes_by_id[AE_ROUTE_HFP_HF] = (ae_route_capabilities_t){.implemented = true, .full_duplex = true, .reason = AE_UNAVAILABLE};
    routes_by_id[AE_ROUTE_A2DP_SINK] = route;
    routes_by_id[AE_ROUTE_A2DP_SINK].capture = false;
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; poll_at(1010);
    assert(view.media.can_select_connection[APP_MEDIA_CONNECTION_USB] &&
        view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH]);
    app_intent_t select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_BLUETOOTH;
    handle(select); assert(submissions == 1 && submitted.route == AE_ROUTE_A2DP_SINK);
    owner.audio.route = AE_ROUTE_A2DP_SINK; ++owner.audio.revision; complete(AE_OK);
    assert(view.media.connection_known && view.media.connection == APP_MEDIA_CONNECTION_BLUETOOTH &&
        !view.media.microphone.can_start && view.media.speaker.can_start);
    poll_at(1030);
    assert(!view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH] && !view.media.microphone.can_start && view.media.speaker.can_start);
    select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_BLUETOOTH;
    handle(select); assert(submissions == 1); /* Preserve the usable speaker profile. */
    routes_by_id[AE_ROUTE_HFP_HF] = route; /* Both profiles really negotiated. */
    context.overlay = APP_OVERLAY_NONE; ++context.ui_revision; poll_at(1040);
    assert(!view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH] && view.media.microphone.can_start);
    routes_by_id[AE_ROUTE_HFP_HF].token.peer_id.bytes[0] = 18; poll_at(1041);
    assert(!view.media.microphone.can_start && view.media.microphone.start_block == APP_MEDIA_START_PEER_MISMATCH);
    routes_by_id[AE_ROUTE_HFP_HF].token.peer_id.bytes[0] = 17; poll_at(1042);
    owner.audio.playback = (ae_job_snapshot_t){.kind = AE_PLAYBACK_STREAM, .state = AE_JOB_RUNNING};
    owner.audio.hardware_off_confirmed = false; ++owner.audio.revision; poll_at(1050);
    assert(!view.media.microphone.can_start && view.media.microphone.start_block == APP_MEDIA_START_STOP_REQUIRED);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(submissions == 1 && !link_count);
    owner.audio.playback = (ae_job_snapshot_t){0}; owner.audio.hardware_off_confirmed = true;
    ++owner.audio.revision; poll_at(1060); handle(start); assert(submissions == 1); /* Old admission is stale. */
    start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(submissions == 2 && submitted.action == APP_MEDIA_START &&
        submitted.kind == AE_CAPTURE_LIVE && submitted.route == AE_ROUTE_HFP_HF && !link_count &&
        ae_route_token_equal(&submitted.expected_route_token, &routes_by_id[AE_ROUTE_HFP_HF].token));
    owner.audio.route = AE_ROUTE_HFP_HF; ++owner.audio.revision; complete(AE_OK);
    assert(view.media.connection_known && view.media.microphone.can_start && view.media.speaker.can_start);
    select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_USB;
    handle(select); owner.audio.route = AE_ROUTE_USB_UAC; ++owner.audio.revision; complete(AE_OK);
    assert(submissions == 3 && view.media.connection == APP_MEDIA_CONNECTION_USB &&
        !view.media.microphone.can_start && !view.media.speaker.can_start);
    routes_by_id[AE_ROUTE_HFP_HF].implemented = routes_by_id[AE_ROUTE_A2DP_SINK].implemented = false;
    poll_at(1090); assert(!view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH]);
    select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_BLUETOOTH;
    handle(select); assert(submissions == 3);
}
static void test_connection_stop_and_privacy_barriers(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE;
    for (unsigned gate = 0; gate < 8; ++gate) {
        owner.audio.capture[AE_CAPTURE_LIVE] = (ae_job_snapshot_t){0};
        owner.audio.playback = (ae_job_snapshot_t){0};
        owner.audio.hardware_off_confirmed = true; owner.audio.quarantined = false; owner.busy = false;
        context.locked = context.screen_off = false; owner.audio_known = true;
        if (gate == 0) owner.audio.capture[AE_CAPTURE_LIVE].state = AE_JOB_RUNNING;
        if (gate == 1) owner.audio.playback.cleanup_pending = true;
        if (gate == 2) owner.audio.hardware_off_confirmed = false;
        if (gate == 3) owner.audio.quarantined = true;
        if (gate == 4) owner.busy = true;
        if (gate == 5) context.locked = true;
        if (gate == 6) context.screen_off = true;
        if (gate == 7) owner.audio_known = false;
        ++owner.audio.revision; poll_at(1010 + gate * 10U);
        assert(!view.media.can_select_connection[APP_MEDIA_CONNECTION_USB] &&
            !view.media.can_select_connection[APP_MEDIA_CONNECTION_BLUETOOTH]);
        app_intent_t select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_USB;
        handle(select); assert(!submissions);
    }
}
static void test_connection_context_and_stale_requests(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; poll_at(1010);
    app_intent_t select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_USB;
    app_intent_t changed = select; ++changed.epoch; handle(changed);
    changed = select; ++changed.ui_revision; handle(changed);
    changed = select; ++changed.media.audio_generation; handle(changed);
    changed = select; ++changed.media.resource_revision; handle(changed);
    changed = select; changed.media.value.connection = (app_media_connection_t)-1; handle(changed);
    changed.media.value.connection = APP_MEDIA_CONNECTION_COUNT; handle(changed);
    assert(!submissions);
    const app_overlay_t panels[] = {APP_OVERLAY_AUDIO, APP_OVERLAY_RECORDING_START, APP_OVERLAY_CONFIRM, APP_OVERLAY_MODES};
    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); ++i) {
        context.overlay = panels[i]; ++context.ui_revision; poll_at(1020 + i * 10U);
        select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_USB;
        handle(select); assert(!submissions);
    }
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; context.mode = INPUT_MOUSE; ++context.ui_revision; poll_at(1060);
    select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_USB;
    handle(select); assert(!submissions);
    context.mode = INPUT_AUDIO; ++context.ui_revision; poll_at(1070);
    select = intent(APP_MEDIA_SELECT_CONNECTION); select.media.value.connection = APP_MEDIA_CONNECTION_USB;
    submit_result = AE_BUSY; handle(select); assert(submissions == 1 && !controls.pending);
    poll_at(1080); assert(submissions == 1 && !view.media.connection_known);
}
static void test_live_start_block_matrix(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    app_media_snapshot_t baseline = owner;
    ae_route_capabilities_t baseline_route = route;
    ae_codec_capabilities_t baseline_codec = hardware;
    const app_media_start_block_t expected[] = {APP_MEDIA_START_ALLOWED, APP_MEDIA_START_LOCKED,
        APP_MEDIA_START_SCREEN_UNAVAILABLE, APP_MEDIA_START_UNKNOWN, APP_MEDIA_START_PENDING,
        APP_MEDIA_START_QUARANTINED, APP_MEDIA_START_CONNECTION_REQUIRED, APP_MEDIA_START_UNSUPPORTED,
        APP_MEDIA_START_ROUTE_UNAVAILABLE, APP_MEDIA_START_HOST_UNAVAILABLE, APP_MEDIA_START_HOST_STREAM_REQUIRED,
        APP_MEDIA_START_CODEC_UNAVAILABLE, APP_MEDIA_START_FORMAT_UNSUPPORTED, APP_MEDIA_START_IDENTITY_UNAVAILABLE,
        APP_MEDIA_START_CLEANUP, APP_MEDIA_START_OCCUPIED, APP_MEDIA_START_OCCUPIED, APP_MEDIA_START_PENDING,
        APP_MEDIA_START_UNKNOWN, APP_MEDIA_START_PENDING, APP_MEDIA_START_UNKNOWN};
    for (unsigned gate = 0; gate < sizeof(expected) / sizeof(expected[0]); ++gate) {
        owner = baseline; route = baseline_route; hardware = baseline_codec;
        context.locked = context.screen_off = false; snapshot_available = true;
        owner.audio.revision += gate;
        switch (gate) {
        case 1: context.locked = true; break;
        case 2: context.screen_off = true; break;
        case 3: owner.audio_known = false; break;
        case 4: owner.busy = true; break;
        case 5: owner.audio.quarantined = true; break;
        case 6: owner.audio.route = AE_ROUTE_LOCAL; break;
        case 7: route.implemented = false; break;
        case 8: route.available = false; break;
        case 9: route.connected = false; break;
        case 10: route.playback = false; break;
        case 11: hardware.available = false; break;
        case 12: hardware.format_count = 0; break;
        case 13: route.token.stream_id = (ae_id_t){0}; break;
        case 14: owner.audio.playback.cleanup_pending = true; break;
        case 15: owner.audio.playback.state = AE_JOB_RUNNING; break;
        case 16: owner.audio.capture[AE_CAPTURE_LIVE].state = AE_JOB_RUNNING; break;
        case 17: hardware.available = false; hardware.reason = AE_AGAIN; break;
        case 18: owner.initialized = false; break;
        case 19: owner.result_ready = true; break;
        case 20: snapshot_available = false; break;
        default: break;
        }
        poll_at(1010 + gate * 10U);
        assert(view.media.speaker.start_block == expected[gate]);
        assert(view.media.speaker.can_start == (expected[gate] == APP_MEDIA_START_ALLOWED));
        if (expected[gate] != APP_MEDIA_START_ALLOWED) {
            app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_SPEAKER;
            handle(start); assert(!submissions);
        }
    }
    owner = baseline; route = baseline_route; hardware = baseline_codec; snapshot_available = true;
    poll_at(1300); poll_at(1299);
    assert(view.media.speaker.start_block == APP_MEDIA_START_CLOCK_UNAVAILABLE && !view.media.speaker.can_start);
}
static void test_live_start_independent_host_roles(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    route.capture = false; ++owner.audio.revision; poll_at(1010);
    assert(!view.media.microphone.can_start && view.media.microphone.start_block == APP_MEDIA_START_HOST_STREAM_REQUIRED);
    assert(view.media.speaker.can_start && view.media.speaker.start_block == APP_MEDIA_START_ALLOWED &&
        !view.media.speaker.enabled && !view.media.speaker.mute_confirmed);
    route.capture = true; route.playback = false; poll_at(1020);
    assert(view.media.microphone.start_block == APP_MEDIA_START_HOST_PREPARING &&
        view.media.speaker.start_block == APP_MEDIA_START_HOST_STREAM_REQUIRED);
    poll_at(2020); assert(view.media.microphone.can_start);
    route.playback = true; hardware.formats[0].roles = AE_SPEAKER; poll_at(2030);
    assert(view.media.speaker.start_block == APP_MEDIA_START_HOST_PREPARING);
    poll_at(3030);
    assert(view.media.microphone.start_block == APP_MEDIA_START_FORMAT_UNSUPPORTED && view.media.speaker.can_start);
    hardware.formats[0].roles = AE_MICROPHONE | AE_SPEAKER;
    route.token.link_generation = 0; poll_at(3040);
    assert(view.media.microphone.start_block == APP_MEDIA_START_IDENTITY_UNAVAILABLE &&
        view.media.speaker.start_block == APP_MEDIA_START_IDENTITY_UNAVAILABLE);
}
static void test_live_start_pending_and_cleanup_reasons(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    route.full_duplex = hardware.full_duplex = true;
    owner.audio.capture[AE_CAPTURE_LIVE].state = AE_JOB_RUNNING;
    ++owner.audio.revision; poll_at(1010);
    assert(view.media.microphone.start_block == APP_MEDIA_START_OCCUPIED && view.media.speaker.can_start);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_SPEAKER;
    handle(start); app_media_controls_fill(&view);
    assert(submissions == 1 && view.media.microphone.start_block == APP_MEDIA_START_PENDING &&
        view.media.speaker.start_block == APP_MEDIA_START_PENDING);
    complete(AE_OK); assert(view.media.speaker.can_start);
    owner.audio.playback.cleanup_pending = true; ++owner.audio.revision; poll_at(1030);
    assert(view.media.microphone.start_block == APP_MEDIA_START_CLEANUP && view.media.speaker.start_block == APP_MEDIA_START_CLEANUP);
    context.locked = true; poll_at(1040);
    assert(view.media.speaker.start_block == APP_MEDIA_START_LOCKED && submissions == 1);
}
static void test_link_target_at_touch(void)
{
    setup(); context.overlay = APP_OVERLAY_MEDIA_OPTIONS; owner.audio.route = AE_ROUTE_A2DP_SINK;
    bluetooth = (bt_audio_snapshot_t){.initialized = true, .a2dp_connected = true,
        .a2dp_peer_known = true, .a2dp_peer = {1}, .generation = 10, .a2dp_link_generation = 3};
    poll_at(1010);
    assert(view.media.routes[APP_MEDIA_ROUTE_A2DP].can_disconnect);
    app_intent_t old = intent(APP_MEDIA_LINK_DISCONNECT);
    ++bluetooth.a2dp_link_generation; poll_at(1020);
    handle(old); assert(!submissions);
    handle(intent(APP_MEDIA_LINK_DISCONNECT));
    assert(submissions == 1 && submitted.action == APP_MEDIA_LINK_CONTROL && !submitted.value &&
        submitted.bluetooth_generation == 10 && submitted.bluetooth_link_generation == 4 &&
        !memcmp(submitted.bluetooth_peer, bluetooth.a2dp_peer, 6));
}

static void test_quarantined_profile_start(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE;
    owner.audio.route = AE_ROUTE_HFP_HF;
    distinct_routes = true;
    routes_by_id[AE_ROUTE_HFP_HF] = route;
    routes_by_id[AE_ROUTE_A2DP_SINK] = (ae_route_capabilities_t){.implemented = true, .available = true,
        .reason = AE_BUSY};
    bluetooth.control_inflight = true; bluetooth.request_status = BT_AUDIO_JOB_UNCERTAIN;
    poll_at(1010);
    assert(view.media.microphone.can_start && view.media.microphone.start_block == APP_MEDIA_START_ALLOWED);
    app_intent_t start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(submissions == 1 && submitted.kind == AE_CAPTURE_LIVE && submitted.route == AE_ROUTE_HFP_HF);
    complete(AE_OK);

    /* HFP is now the quarantined profile. Only the negotiated A2DP speaker can start. */
    owner.audio.route = AE_ROUTE_A2DP_SINK;
    routes_by_id[AE_ROUTE_A2DP_SINK] = route; routes_by_id[AE_ROUTE_A2DP_SINK].capture = false;
    routes_by_id[AE_ROUTE_HFP_HF].reason = AE_BUSY;
    ++owner.audio.revision; poll_at(1030);
    assert(view.media.microphone.start_block == APP_MEDIA_START_PENDING && view.media.speaker.can_start);
    start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_MICROPHONE;
    handle(start); assert(submissions == 1);
    start = intent(APP_MEDIA_LIVE_START); start.media.value.role.role = APP_MEDIA_SPEAKER;
    handle(start); assert(submissions == 2 && submitted.kind == AE_PLAYBACK_STREAM && submitted.route == AE_ROUTE_A2DP_SINK);
    complete(AE_OK);

    bluetooth.control_inflight = false; routes_by_id[AE_ROUTE_HFP_HF].reason = AE_OK;
    poll_at(1050);
    assert(view.media.microphone.can_start && view.media.speaker.can_start);
    bluetooth.pairing_busy = true; poll_at(1060);
    assert(view.media.microphone.start_block == APP_MEDIA_START_PENDING && view.media.speaker.start_block == APP_MEDIA_START_PENDING);
    context.locked = true; poll_at(1070);
    assert(view.media.microphone.start_block == APP_MEDIA_START_LOCKED && view.media.speaker.start_block == APP_MEDIA_START_LOCKED);
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
    bonds.refreshing = true; slc_succeeded();
    assert(link_count == 1 && controls.voice_link.active);
    bonds.refreshing = false; poll_at(controls.now_ms + 1);
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
static void test_operation_message_lifecycle(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC; poll_at(1010);
    app_intent_t volume = intent(APP_MEDIA_SET_VOLUME); volume.media.value.level = 45;
    app_media_controls_effect_t effect = handle(volume);
    assert(submissions == 1 && controls.pending && effect.handled && !*effect.message);
    owner.busy = true; poll_at(5010);
    assert(view.media.command_pending && !*view.media.reason);
    complete(AE_OK);
    assert(!view.media.command_pending && !*view.media.reason);
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; poll_at(5020);
    context.overlay = APP_OVERLAY_NONE; poll_at(5030); assert(!*view.media.reason);

    volume = intent(APP_MEDIA_SET_VOLUME); volume.media.value.level = 46;
    handle(volume); complete(AE_IO_ERROR); poll_at(9030);
    assert(!strcmp(view.media.reason, result_text(AE_IO_ERROR)));
    context.mode = INPUT_MOUSE; poll_at(9040);
    assert(!strcmp(controls.message, result_text(AE_IO_ERROR)));
    context.mode = INPUT_AUDIO; poll_at(9050);
    volume = intent(APP_MEDIA_SET_VOLUME); volume.media.value.level = 47; effect = handle(volume);
    assert(controls.pending && effect.handled && !*effect.message && !*controls.message);
    complete(AE_OK); assert(!view.media.command_pending && !*view.media.reason);

    effect_result(NULL, AE_IO_ERROR); effect_result(NULL, AE_OK);
    app_media_controls_poll(9060, &effect);
    assert(effect.handled && !*effect.message);
}

static void test_late_success_notice_scope(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC; poll_at(1010);
    app_intent_t volume = intent(APP_MEDIA_SET_VOLUME); volume.media.value.level = 45;
    handle(volume); assert(controls.pending);
    context.overlay = APP_OVERLAY_MEDIA_OPTIONS; poll_at(1020);
    context.overlay = APP_OVERLAY_NONE; poll_at(1030);
    owner.audio.volume_percent = 45; ++owner.audio.revision;
    owner.request_token = submitted.token; owner.result = AE_OK; owner.result_ready = true;
    app_media_controls_effect_t effect;
    app_media_controls_poll(1040, &effect); app_media_controls_fill(&view);
    assert(!owner.result_ready && !controls.pending && view.volume == 45);
    assert(effect.handled && !*effect.message && !*view.media.reason);
    poll_at(1050); assert(!*view.media.reason);

    volume = intent(APP_MEDIA_SET_VOLUME); volume.media.value.level = 46; handle(volume);
    ++context.epoch; context.mode = INPUT_MOUSE; poll_at(1060);
    complete(AE_IO_ERROR);
    assert(!controls.pending && !strcmp(view.media.reason, result_text(AE_IO_ERROR)));
}

static void setup_microphone_level(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    owner.audio.hardware_off_confirmed = false; owner.audio.microphone_enabled = true;
    owner.audio.microphone_muted = false; owner.audio.microphone_mute_applied = false;
    owner.audio.microphone_mute_confirmed = true; owner.audio.capture_gate_open = true;
    owner.audio.capture_indicator_required = true;
    owner.audio.capture[0] = (ae_job_snapshot_t){.kind = AE_CAPTURE_LIVE, .state = AE_JOB_RUNNING,
        .job_id = {{51}}, .consumer_generation = 61};
    capture_level = (board_audio_capture_snapshot_t){.known = true, .current = true, .generation = 3,
        .observed_ms = 1020, .samples = 8, .sum_squares = UINT64_C(8) * 268435456U, .peak_abs = 16384};
    ++owner.audio.revision; poll_at(1020);
}
static void test_microphone_level_values(void)
{
    setup_microphone_level(); app_media_mic_level_t level = view.media.microphone_level;
    assert(level.state == APP_MEDIA_LEVEL_LIVE && level.dbfs == -6 && level.percent == 90 && !level.silent && !level.clipping);
    capture_level.observed_ms += 100; capture_level.sum_squares = UINT64_C(8) * 67108864U; capture_level.peak_abs = 8192;
    poll_at(1120); level = view.media.microphone_level;
    assert(level.state == APP_MEDIA_LEVEL_LIVE && level.dbfs == -12 && level.percent == 80);
    capture_level.observed_ms += 100; capture_level.sum_squares = 0; capture_level.peak_abs = 0;
    poll_at(1220); level = view.media.microphone_level;
    assert(level.state == APP_MEDIA_LEVEL_LIVE && level.silent && !level.percent && !level.clipping);
    capture_level.observed_ms += 100; capture_level.sum_squares = UINT64_C(8) * 1073741824U; capture_level.peak_abs = 32768;
    poll_at(1320); level = view.media.microphone_level;
    assert(level.state == APP_MEDIA_LEVEL_LIVE && level.dbfs == 0 && level.percent == 100 && level.clipping);
    capture_level.observed_ms += 100; capture_level.sum_squares = 1; capture_level.peak_abs = 1; capture_level.samples = 100000;
    poll_at(1420); level = view.media.microphone_level;
    assert(level.state == APP_MEDIA_LEVEL_LIVE && level.dbfs == -96 && !level.silent && !level.percent);
}
static void test_microphone_level_validity(void)
{
    setup_microphone_level(); poll_at(1320); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_LIVE);
    poll_at(1321); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_WAITING && !view.media.microphone_level.percent);
    capture_level.observed_ms = 1400; poll_at(1330); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    capture_level.observed_ms = 1330; ++capture_level.generation; poll_at(1340);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    capture_level.generation = 3; capture_level.current = false; poll_at(1350);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    capture_level.current = true; capture_level.known = false; poll_at(1360);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_WAITING);
    capture_level.result = AE_TIMEOUT; poll_at(1370); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    capture_level.result = AE_OK; capture_level.known = true; capture_level.samples = 0; poll_at(1380);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    owner.audio.capture_gate_open = false; owner.audio.microphone_muted = true;
    owner.audio.microphone_mute_applied = true; poll_at(1390);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_OFF && !view.media.microphone_level.percent);
    owner.audio.microphone_mute_confirmed = false; poll_at(1400);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    owner.audio.microphone_muted = false; poll_at(1410); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_WAITING);
    owner.audio.hardware_off_confirmed = true; poll_at(1420); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_OFF);
    owner.audio.error = AE_IO_ERROR; poll_at(1430); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    owner.audio.error = AE_OK; owner.audio.capture_gate_open = true; owner.audio.hardware_off_confirmed = false;
    capture_level.samples = 8; capture_level.observed_ms = 1440; poll_at(1440);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_LIVE);
    snapshot_available = false; poll_at(1450); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
    snapshot_available = true; poll_at(1460); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_LIVE);
    poll_at(1459); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN);
}
static void test_microphone_level_preserves_controls(void)
{
    setup_microphone_level(); uint64_t revision = view.media.resource_revision;
    app_intent_t held = intent(APP_MEDIA_SET_VOLUME); held.media.value.level = 45;
    for (unsigned i = 1; i <= 10; ++i) {
        capture_level.observed_ms = 1020 + i * 100U;
        capture_level.sum_squares = UINT64_C(8) * i * i; capture_level.peak_abs = (uint16_t)i;
        ++owner.audio.capture[0].sample_frames; poll_at(capture_level.observed_ms);
        assert(view.media.resource_revision == revision && view.media.microphone_level.state == APP_MEDIA_LEVEL_LIVE);
    }
    handle(held); assert(submissions == 1 && controls.pending && submitted.action == APP_MEDIA_VOLUME);
}
static void test_microphone_level_clock_order(void)
{
    setup_microphone_level(); uint64_t revision = view.media.resource_revision;
    capture_clock_override = true; capture_clock_us = 1121000; capture_level.observed_ms = 1121;
    poll_at(1120); assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_LIVE);
    assert(view.media.resource_revision == revision && controls.now_ms == 1120);
    capture_level.observed_ms = 1122; poll_at(1121);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN); /* Future even after the copy. */
    capture_level.observed_ms = 1121; capture_clock_us = 1120500; poll_at(1121);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN); /* Clock itself went backward. */
    capture_clock_us = 1221000; capture_level.observed_ms = 1221; poll_at(1221);
    assert(view.media.microphone_level.state == APP_MEDIA_LEVEL_UNKNOWN); /* Clock failure stays explicit. */
}

static void test_usb_start_settling(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    ++route.token.link_generation; poll_at(1010);
    app_intent_t stale = intent(APP_MEDIA_LIVE_START); stale.media.value.role.role = APP_MEDIA_SPEAKER;
    for (unsigned i = 0; i < 6; ++i) {
        route.capture = route.playback = (i & 1U) == 0; ++route.token.link_generation;
        poll_at(1020 + i * 100U);
        assert(!view.media.microphone.can_start && !view.media.speaker.can_start);
        app_intent_t start = intent(APP_MEDIA_LIVE_START);
        start.media.value.role.role = (i & 1U) ? APP_MEDIA_MICROPHONE : APP_MEDIA_SPEAKER;
        handle(start); assert(!submissions);
    }
    route.capture = route.playback = true; ++route.token.link_generation; poll_at(2000);
    assert(view.media.speaker.start_block == APP_MEDIA_START_HOST_PREPARING);
    uint64_t revision = view.media.resource_revision;
    poll_at(2000 + APP_MEDIA_USB_START_SETTLE_MS - 1U); assert(!view.media.speaker.can_start && !submissions);
    poll_at(2000 + APP_MEDIA_USB_START_SETTLE_MS);
    assert(view.media.speaker.can_start && view.media.microphone.can_start && !submissions && view.media.resource_revision == revision);
    handle(stale); assert(!submissions); /* A replacement never retargets the old press. */
    app_intent_t fresh = intent(APP_MEDIA_LIVE_START); fresh.media.value.role.role = APP_MEDIA_SPEAKER;
    handle(fresh); assert(submissions == 1 && submitted.kind == AE_PLAYBACK_STREAM && submitted.action == APP_MEDIA_START);
}
static void test_usb_settle_revoke_and_hint(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    route.capture = false; poll_at(1010); assert(view.media.speaker.can_start);
    route.capture = true; poll_at(1020);
    assert(view.media.microphone.start_block == APP_MEDIA_START_HOST_PREPARING && view.media.speaker.can_start);
    uint64_t now = 1020 + APP_MEDIA_USB_START_SETTLE_MS; poll_at(now); assert(view.media.microphone.can_start);
    ++now; route.available = false; poll_at(now); assert(!view.media.speaker.can_start && !controls.usb_role_observed[1]);
    ++now; route.available = true; poll_at(now); assert(view.media.speaker.start_block == APP_MEDIA_START_HOST_PREPARING);
    effect_result(NULL, AE_STALE);
    owner.audio.playback = (ae_job_snapshot_t){.job_id={{42}}, .consumer_generation=9, .kind=AE_PLAYBACK_STREAM,
        .state=AE_JOB_PARTIAL, .stop_reason=AE_STOP_OWNER_LOST, .error=AE_STALE};
    poll_at(++now);
    assert(view.media.playback.known && *view.media.playback.reason && !*view.media.reason && *controls.message);
    owner.audio.error = AE_IO_ERROR; poll_at(++now);
    assert(!strcmp(view.media.reason, result_text(AE_IO_ERROR)) && *controls.message);
    owner.audio.error = AE_OK;
    now += APP_MEDIA_USB_START_SETTLE_MS; poll_at(now); assert(view.media.speaker.can_start);
    route.token.stream_id.bytes[0]++; poll_at(++now); assert(view.media.speaker.start_block == APP_MEDIA_START_HOST_PREPARING);
    route.token.stream_id = (ae_id_t){0}; poll_at(++now); assert(view.media.speaker.start_block == APP_MEDIA_START_IDENTITY_UNAVAILABLE);
    route.token.stream_id = (ae_id_t){{7}}; poll_at(++now);
    snapshot_available = false; poll_at(++now); assert(!controls.usb_role_observed[0] && !controls.usb_role_observed[1]);
    snapshot_available = true; poll_at(++now); assert(view.media.speaker.start_block == APP_MEDIA_START_HOST_PREPARING);
    now += APP_MEDIA_USB_START_SETTLE_MS; poll_at(now); assert(view.media.speaker.can_start && !submissions);
    owner.audio.route = AE_ROUTE_LOCAL; route.capture = route.playback = route.connected = false; poll_at(++now);
    assert(view.media.can_select_connection[APP_MEDIA_CONNECTION_USB]);
}
static void test_usb_settle_stop_and_clock(void)
{
    setup(); context.overlay = APP_OVERLAY_NONE; owner.audio.route = AE_ROUTE_USB_UAC;
    route.full_duplex = hardware.full_duplex = true;
    owner.audio.hardware_off_confirmed = false;
    owner.audio.microphone_enabled = owner.audio.speaker_enabled = true;
    owner.audio.microphone_muted = owner.audio.speaker_muted = false;
    owner.audio.microphone_mute_confirmed = owner.audio.speaker_mute_confirmed = true;
    owner.audio.playback = (ae_job_snapshot_t){.job_id={{51}}, .consumer_generation=61, .kind=AE_PLAYBACK_STREAM, .state=AE_JOB_RUNNING};
    owner.audio.capture[0] = (ae_job_snapshot_t){.job_id={{52}}, .consumer_generation=62, .kind=AE_CAPTURE_LIVE, .state=AE_JOB_RUNNING};
    ++route.token.link_generation; poll_at(1010);
    assert(view.media.playback.can_stop && view.media.microphone.can_mute);
    app_intent_t stop = intent(APP_MEDIA_LIVE_STOP); stop.media.value.role.role = APP_MEDIA_SPEAKER;
    stop.media.job_id=view.media.playback.id; stop.media.consumer_generation=view.media.playback.consumer_generation;
    uint64_t revision=view.media.resource_revision;poll_at(1010 + APP_MEDIA_USB_START_SETTLE_MS);
    assert(view.media.resource_revision==revision && view.media.playback.can_stop && view.media.microphone.can_mute);
    handle(stop); assert(submissions==1 && submitted.action==APP_MEDIA_STOP_JOB && submitted.job_id.bytes[0]==51);
    complete(AE_OK);
    app_intent_t mute=intent(APP_MEDIA_SET_MUTED);mute.media.value.role.role=APP_MEDIA_MICROPHONE;mute.media.value.role.value=true;
    handle(mute);assert(submissions==2 && submitted.action==APP_MEDIA_MUTE_ROLE && submitted.value);
    complete(AE_OK);owner.audio.playback=(ae_job_snapshot_t){0};owner.audio.capture[0]=(ae_job_snapshot_t){0};
    owner.audio.hardware_off_confirmed=true;poll_at(UINT64_MAX-500U);
    ++route.token.link_generation;poll_at(UINT64_MAX-499U);poll_at(UINT64_MAX);
    assert(view.media.speaker.start_block==APP_MEDIA_START_HOST_PREPARING);
    poll_at(0);assert(view.media.speaker.start_block==APP_MEDIA_START_CLOCK_UNAVAILABLE && !view.media.speaker.can_start);
}

static void test_shared_devices(void)
{
    setup(); context.mode = INPUT_MOUSE; context.overlay = APP_OVERLAY_BLUETOOTH_DEVICES;
    context.bluetooth_audio_devices = true; owner.audio.route = AE_ROUTE_USB_UAC;
    bluetooth = (bt_audio_snapshot_t){.supported=true,.initialized=true,.generation=31,.classic_bonds_known=true,
        .device_name_known=true,.hfp_link_generation=41,.a2dp_link_generation=43};
    strcpy(bluetooth.device_name,"Actual Classic search name");
    bonds=(bt_audio_bond_list_t){.known=true,.revision=29,.count=2};
    bonds.bonds[0].address[5]=1; bonds.bonds[1].address[5]=2; poll_at(1010);
    assert(view.media.devices.known && view.media.devices.count==2 && view.media.pairing.can_open);
    assert(!submissions && !link_count && owner.audio.route==AE_ROUTE_USB_UAC);
    app_intent_t choose=intent(APP_MEDIA_DEVICE_DETAILS);
    choose.media.object_revision=bonds.revision; choose.media.value.peer_id=view.media.devices.rows[1].id;
    apply_effect(handle(choose)); poll_at(1020);
    assert(context.overlay==APP_OVERLAY_AUDIO_DEVICE && view.media.devices.selected.bytes[15]==2);
    app_intent_t connect=intent(APP_MEDIA_DEVICE_LINK); connect.media.object_revision=bonds.revision;
    connect.media.object_id=view.media.devices.selected; connect.media.value.page_delta=2;
    handle(connect); assert(link_count==1 && links[0].command==BT_AUDIO_CONNECT_A2DP && links[0].peer[5]==2);
    assert(!submissions && owner.audio.route==AE_ROUTE_USB_UAC);
    handle(connect); assert(link_count==1); /* The accepted request revokes its old revision. */
    bluetooth.request_status=BT_AUDIO_JOB_SUCCEEDED; bluetooth.a2dp_connected=bluetooth.a2dp_peer_known=true;
    bluetooth.a2dp_peer[5]=2; bonds.bonds[1].a2dp_current=true; poll_at(1030);
    assert(view.media.devices.connected[1] && view.media.devices.can_disconnect[1]);
    app_intent_t disconnect=intent(APP_MEDIA_DEVICE_LINK); disconnect.media.object_revision=bonds.revision;
    disconnect.media.object_id=view.media.devices.selected; disconnect.media.value.page_delta=3;
    bluetooth.a2dp_peer[5]=1; ++bluetooth.a2dp_link_generation; poll_at(1040);
    handle(disconnect); assert(link_count==1); /* Actual replacement survives a stale action. */
    context.overlay = APP_OVERLAY_BLUETOOTH_DEVICES;
    bonds.count = 6; ++bonds.revision;
    for (unsigned i = 0; i < bonds.count; ++i) bonds.bonds[i].address[5] = (uint8_t)(i + 1);
    poll_at(1050);
    assert(view.media.devices.pages == 2 && view.media.devices.page == 0);
    app_intent_t page = intent(APP_MEDIA_DEVICE_PAGE);
    page.media.object_revision = bonds.revision; page.media.value.page_delta = 1;
    handle(page); poll_at(1060);
    assert(view.media.devices.page == 1 && view.media.devices.rows[0].id.bytes[15] == 5 &&
        view.media.devices.rows[1].id.bytes[15] == 6);
    choose = intent(APP_MEDIA_DEVICE_DETAILS); choose.media.object_revision = bonds.revision;
    choose.media.value.peer_id = view.media.devices.rows[1].id;
    ++bonds.revision; bonds.count = 1; poll_at(1070);
    handle(choose);
    assert(context.overlay == APP_OVERLAY_BLUETOOTH_DEVICES && view.media.devices.selected.bytes[15] == 2 &&
        !view.media.devices.selected_saved);
    assert(view.media.devices.page == 0 && view.media.devices.pages == 1 && !submissions && link_count == 1);
}
static void test_shared_devices_pairing(void)
{
    setup(); context.mode=INPUT_KEYBOARD; context.overlay=APP_OVERLAY_BLUETOOTH_DEVICES;
    context.bluetooth_audio_devices=true; owner.audio.route=AE_ROUTE_USB_UAC;
    bluetooth=(bt_audio_snapshot_t){.supported=true,.initialized=true,.classic_bonds_known=true}; poll_at(1010);
    assert(view.media.pairing.can_open);
    pair_confirm(APP_MEDIA_PAIR_AUDIO_OPEN);
    assert(context.overlay==APP_OVERLAY_BLUETOOTH_DEVICES && pairing_opens==1 && !submissions && !link_count);
    context.bluetooth_audio_devices=false; context.overlay=APP_OVERLAY_NONE; app_media_controls_context(&context);
    assert(pairing_closes==1);
}

int main(int argc, char **argv)
{
    const char *names[] = {"shared_devices", "shared_devices_pairing", "review_and_quota", "discard_review", "stale_and_failure", "list_object_identity", "storage_prepare", "speech_once", "targeted_stop_and_idle", "live_and_saved_peer", "connection_observed_completion", "connection_profile_capabilities", "bluetooth_idle_stream", "bluetooth_idle_microphone", "bluetooth_retained_peer", "pairing_close_pending", "pairing_error_origin", "pairing_old_capacity_error", "pairing_new_capacity_error", "pairing_window", "pairing_recovery_and_link", "pairing_outcomes", "pairing_context_and_stale", "connection_stop_and_privacy_barriers", "connection_context_and_stale_requests", "live_start_block_matrix", "live_start_independent_host_roles", "live_start_pending_and_cleanup_reasons", "quarantined_profile_start", "link_target_at_touch", "capture_and_mute_evidence", "voice_connect_sequence", "voice_existing_slc", "voice_async_failure", "voice_replaced_token", "voice_replaced_peer", "voice_catalogue_changed", "voice_locked", "voice_timeout", "voice_snapshot_unavailable", "voice_submit_rejected", "snapshot_reuse", "record_copy_failure", "usb_start_settling", "usb_settle_revoke_and_hint", "usb_settle_stop_and_clock", "operation_message_lifecycle", "late_success_notice_scope", "microphone_level_values", "microphone_level_validity", "microphone_level_preserves_controls", "microphone_level_clock_order"};
    void (*cases[])(void) = {test_shared_devices, test_shared_devices_pairing, test_review_and_quota, test_discard_review, test_stale_and_failure, test_list_object_identity, test_storage_prepare, test_speech_once, test_targeted_stop_and_idle, test_live_and_saved_peer, test_connection_observed_completion, test_connection_profile_capabilities, test_bluetooth_stream_after_idle_selection, test_bluetooth_microphone_after_idle_selection, test_bluetooth_retained_owner_peer, test_pairing_close_pending, test_pairing_error_origin, test_pairing_old_capacity_error, test_pairing_new_capacity_error, test_pairing_window, test_pairing_recovery_and_link, test_pairing_outcomes, test_pairing_context_and_stale, test_connection_stop_and_privacy_barriers, test_connection_context_and_stale_requests, test_live_start_block_matrix, test_live_start_independent_host_roles, test_live_start_pending_and_cleanup_reasons, test_quarantined_profile_start, test_link_target_at_touch, test_capture_and_mute_evidence, test_voice_connect_sequence, test_voice_existing_slc, test_voice_async_failure, test_voice_replaced_token, test_voice_replaced_peer, test_voice_catalogue_changed, test_voice_locked, test_voice_timeout, test_voice_snapshot_unavailable, test_voice_submit_rejected, test_snapshot_reuse, test_record_copy_failure, test_usb_start_settling, test_usb_settle_revoke_and_hint, test_usb_settle_stop_and_clock, test_operation_message_lifecycle, test_late_success_notice_scope, test_microphone_level_values, test_microphone_level_validity, test_microphone_level_preserves_controls, test_microphone_level_clock_order};
    assert(argc == 2);
    if (!strcmp(argv[1], "--list")) { for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) puts(names[i]); return 0; }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(argv[1], names[i])) { cases[i](); printf("PASS: media controls %s\n", names[i]); return 0; }
    return 2;
}
