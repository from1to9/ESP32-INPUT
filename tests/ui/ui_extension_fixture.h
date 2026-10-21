// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_ui.h"
#include <stdio.h>
#include <string.h>

/* Synthetic public metadata; never a real network or saved credential. */
static void ui_fixture_native_networks(app_view_t *view)
{
    view->wifi_scan = (app_wifi_scan_view_t){.state = APP_UI_SCAN_READY,
        .generation = 47, .count = 32, .row_count = 4, .can_start = true,
        .seconds_left = 120, .capacity_reached = true};
    const uint16_t identities[] = {0, 3, 17, 31};
    for (unsigned i = 0; i < 4; ++i) {
        app_wifi_row_view_t *row = &view->wifi_scan.rows[i];
        row->record_index = identities[i];
        row->security = APP_UI_SECURITY_WPA2;
        row->availability = APP_UI_JOIN_AVAILABLE;
        row->rssi_valid = true;
        row->rssi_dbm = -40 - (int)i * 12;
        snprintf(row->label, sizeof(row->label), "Synthetic network %u", i);
    }
    strcpy(view->wifi_scan.rows[0].label, "Duplicate synthetic network");
    strcpy(view->wifi_scan.rows[2].label, "Duplicate synthetic network");
    memset(view->wifi_scan.rows[1].label, 'W', 128);
    view->wifi_scan.rows[1].label[128] = '\0';
    for (unsigned i = 0; i < 32; ++i) memcpy(view->wifi_scan.rows[3].label + i * 4, "\\xFF", 4);
    view->wifi_scan.rows[3].label[128] = '\0';
    view->wifi_scan.rows[0].current = true;
    view->wifi_scan.rows[2].remembered = true;
    view->wifi_scan.rows[3].security = APP_UI_SECURITY_ENTERPRISE;
    view->wifi_scan.rows[3].availability = APP_UI_JOIN_UNSUPPORTED_SECURITY;
    view->wifi_scan.rows[3].rssi_valid = false;
    view->wifi_selection = (app_wifi_selection_view_t){.valid = true, .generation = 47,
        .network = view->wifi_scan.rows[2], .can_join_once = true, .can_remember = true};
    view->wifi_stored_known = view->wifi_stored_present = view->wifi_stored_valid = true;
    strcpy(view->wifi_stored_label, "Different saved network");
    view->wifi_lan_ready = true;
    view->wifi_lan_service = APP_UI_LAN_SERVICE_UNAVAILABLE;
    view->bluetooth_name = (app_bluetooth_name_view_t){.available = true,
        .configured_valid = true, .effective_valid = true, .save_state = APP_SAVE_SAVED};
    strcpy(view->bluetooth_name.configured, "Configured name");
    strcpy(view->bluetooth_name.effective, "Effective name");
    view->tap_to_click_available = view->tap_to_click_enabled = true;
    view->motion_reason = APP_MOTION_READY;
}

/* Synthetic owner evidence for native media rendering; never hardware proof. */
static inline void ui_fixture_media(app_view_t *state)
{
    app_media_view_t *media = &state->media;
    *media = (app_media_view_t){.known = true, .management_generation = 31,
        .audio_generation = 47, .resource_revision = 59, .state = APP_MEDIA_READY_MUTED,
        .route = APP_MEDIA_ROUTE_LOCAL, .volume_known = true, .can_set_volume = true,
        .volume_percent = 35, .safe_volume_max = 60, .gain_known = true, .can_set_gain = true,
        .microphone_gain_db = 18};
    media->peer_id.bytes[0] = 5;
    strcpy(media->peer_label, "Synthetic audio peer");
    for (size_t i = 1; i < APP_MEDIA_ROUTE_COUNT; ++i)
        media->routes[i] = (app_media_route_view_t){.available = true, .can_select = true, .can_connect = true};
    media->microphone = media->speaker = (app_media_role_view_t){.known = true, .enabled = true,
        .muted = true, .mute_confirmed = true, .can_enable = true, .can_disable = true,
        .can_mute = true, .can_unmute = true, .can_start = true};
    media->recording_options = (app_media_record_options_t){.known = true, .can_start = true,
        .can_edit = true, .profile_id = 7, .profile_count = 2, .duration_seconds = 5,
        .minimum_duration_seconds = 1, .maximum_duration_seconds = 7,
        .byte_limit = 480000, .retention_seconds = 3600};
    media->recording_options.profiles[0] = (app_media_profile_view_t){.id = 7, .available = true, .label = "PCM16 mono 48 kHz"};
    media->recording_options.profiles[1] = (app_media_profile_view_t){.id = 19, .available = true, .label = "Synthetic second profile"};
    strcpy(media->recording_options.reason, "Limited internal recording capacity.");
    media->recordings = (app_media_record_list_view_t){.known = true, .generation = 71,
        .has_next = true, .row_count = APP_MEDIA_ROWS};
    for (size_t i = 0; i < APP_MEDIA_ROWS; ++i) {
        app_media_record_view_t *row = &media->recordings.rows[i];
        *row = (app_media_record_view_t){.valid = true, .revision = 83 + i, .state = APP_MEDIA_JOB_COMPLETED,
            .duration_ms = 5000, .stored_bytes = 480000, .duration_known = true, .bytes_known = true,
            .verified = true, .can_play = true, .can_delete = true};
        row->id.bytes[0] = (uint8_t)(91 + i);
        snprintf(row->label, sizeof(row->label), "Synthetic recording %u", (unsigned)i + 1);
    }
    media->selected_recording = media->recordings.rows[0];
    media->speech = (app_media_speech_view_t){.state = APP_SPEECH_IDLE, .generation = 101,
        .revision = 103, .can_listen = true, .window_seconds = 10, .profile = "Synthetic letter recognizer"};
}

static inline void ui_fixture_connections(app_view_t *state)
{
    state->api = (app_api_view_t){.known = true, .revision = 701, .can_open = true, .remote_input_generation = 997,
        .review_pending = true, .review_complete = true, .can_approve = true, .review_page_count = 2,
        .review_host = "synthetic-device.example", .reason = "No trust package has been approved."};
    memset(state->api.review_digest, 'a', 64); state->api.review_digest[64] = 0;
    strcpy(state->api.review_pages[0], "Principal: synthetic owner\nInput control and device status\nNo capture authority in this proposal.");
    strcpy(state->api.review_pages[1], "Principal: synthetic observer\nRead-only device status\nNo microphone or recording permission.");
    state->saved_wifi = (app_saved_wifi_view_t){.known = true, .revision = 709, .count = 8};
    for (size_t i = 0; i < APP_SAVED_WIFI_ROWS; ++i) {
        app_saved_wifi_row_t *row = &state->saved_wifi.rows[i];
        snprintf(row->id, sizeof(row->id), "immutable-profile-%u", (unsigned)i);
        snprintf(row->label, sizeof(row->label), "Synthetic network %u", (unsigned)i + 1);
        row->enabled = row->can_join = row->can_prefer = row->can_forget = true; row->priority = (unsigned)i + 1;
    }
    state->bluetooth_target = (app_bluetooth_target_view_t){.known = true, .can_select = true, .can_prefer = true,
        .bond_revision = 719, .preferences_revision = 727, .name = "Synthetic saved peer", .identity = "AA:BB:CC:DD:EE:01"};
    state->bluetooth_target.target.address[0] = 0xaa;
    state->bluetooth_target.target.bond_id_known = true; state->bluetooth_target.target.bond_id[31] = 0x75;
}

static inline void ui_fixture_remote_review(app_view_t *state)
{
    state->remote_review = (app_remote_review_view_t){.pending = true, .complete = true,
        .can_confirm = true, .clock_known = true, .now_ms = lv_tick_get(),
        .request = {.management_generation = 37, .resource_revision = 41,
            .expires_ms = (uint64_t)lv_tick_get() + 60000U, .operation = "system.restart",
            .approval_id = "0123456789abcdef0123456789abcdef",
            .action_digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        .details = "Restart the device.\nSettings, recordings, trust and firmware are retained."};
}
