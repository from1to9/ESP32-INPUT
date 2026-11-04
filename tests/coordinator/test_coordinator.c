// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "../audio_engine/heap_caps_model.h"
#include "freertos/FreeRTOS.h"
/* The SDK lock-acquisition boundary can schedule the other mailbox owner
 * before this test task obtains the lock. Production decisions are unchanged. */
static void coordinator_test_lock_entry(portMUX_TYPE *lock);
#undef portENTER_CRITICAL
#define portENTER_CRITICAL(lock) coordinator_test_lock_entry(lock)

/* Include production helpers unchanged, preserving their actual static state.
 * Hardware interfaces are substituted only through the included public headers. */
#include "../../firmware/main/main.c"

typedef struct {
    app_input_transport_t transport;
    uint8_t id;
    size_t length;
    uint8_t bytes[8];
} report_t;

static report_t trace[1024];
static size_t trace_count;
static ble_hid_snapshot_t test_link;
static ble_hid_advertising_diagnostics_t test_advertising_trace;
static ble_hid_admission_diagnostics_t test_admission_trace;
static bool test_haptics_enabled;
static bool test_haptic_active;
static unsigned test_sensor_buttons, test_sensor_contacts;
static bool test_sensor_public;
void app_sensor_api_init(void) {}
void app_sensor_api_touch(const board_touch_snapshot_t *sample, bool public_coordinates)
{ assert(sample); ++test_sensor_contacts; test_sensor_public = public_coordinates; }
void app_sensor_api_button(bool down, uint64_t timestamp_us)
{ (void)down; (void)timestamp_us; ++test_sensor_buttons; }
static bool test_send_failure;
static board_capabilities_t test_capabilities;
static board_imu_sample_t test_sample;
static esp_err_t test_imu_result;
static bool test_presenter_available;
static app_view_t test_view;
static char test_target[APP_PRESENTER_TARGET_BYTES];
static char test_message[APP_PRESENTER_MESSAGE_BYTES];
static size_t test_publish_count;
static const char *test_case;
static board_power_status_t test_power;
static esp_err_t test_power_error;
static board_brightness_status_t test_brightness;
static esp_err_t test_brightness_error;
static int test_brightness_requests[32];
static size_t test_brightness_count;
static settings_store_status_t test_storage;
static device_settings_t test_saved_request;
static esp_err_t test_storage_error;
static unsigned test_storage_requests;
static app_wifi_snapshot_t test_wifi;
static app_wifi_scan_diagnostics_t test_scan_diagnostics;
static app_control_runtime_snapshot_t test_control_runtime;
static bool test_media_snapshot_unavailable;
static bool test_media_uninitialized_snapshot_available;
static app_wifi_setup_info_t test_wifi_info;
static esp_err_t test_wifi_start_error, test_wifi_stop_error, test_wifi_forget_error;
static esp_err_t test_wifi_info_error;
static unsigned test_wifi_starts, test_wifi_stops, test_wifi_forgets, test_wifi_info_reads;
static bool test_pair_accept, test_disconnect_accept;
static unsigned test_pair_opens, test_pair_closes, test_pair_forgets, test_disconnects;
static uint64_t test_disconnect_generation, test_forget_generation;
static size_t test_disconnect_trace_count;
static unsigned test_native_management_starts, test_native_management_stops;
static bool test_native_management_active, test_native_start_release_pending;
static app_pairing_operation_t test_native_operation;
static uint64_t test_native_generation, test_native_bond_revision, test_native_preference_revision;
static ble_hid_bond_list_t test_bonds;
static usb_hid_snapshot_t test_usb;
static bool test_usb_send_failure, test_usb_recover_accept;
static unsigned test_usb_recovers, test_ble_send_attempts, test_usb_send_attempts;
static bool test_usb_enable_requests[32];
static size_t test_usb_enable_trace[32], test_usb_enable_count;
static input_link_t test_previous_link;
static void (*test_ble_after_accept)(void), (*test_usb_after_accept)(void);
static bool test_diagnostic_logs_enabled, test_heap_observation_enabled;
static char test_diagnostic_logs[16][2048];
static size_t test_diagnostic_log_count, test_heap_observation_count;
static size_t test_heap_free_internal, test_heap_largest_internal;
static size_t test_heap_free_dma, test_heap_largest_dma;
static app_wifi_scan_snapshot_t test_scan;
static app_wifi_scan_record_t test_scan_records[APP_WIFI_SCAN_CAPACITY];
static app_wifi_selection_t test_selection;
static esp_err_t test_scan_error, test_join_error, test_selection_error;
static unsigned test_scans, test_scan_cancels, test_selections, test_selection_releases, test_joins;
static uint32_t test_released_selection;
static app_wifi_join_request_t test_join_request;
static ble_hid_name_snapshot_t test_name;
static esp_err_t test_name_error;
static unsigned test_name_requests;
static bool test_clock_override;
static uint64_t test_clock_ms;
static void (*test_update_gate_entry_hook)(void);
static void coordinator_test_lock_entry(portMUX_TYPE *lock)
{
    if (lock == &s_update_gate_lock && test_update_gate_entry_hook) {
        void (*hook)(void) = test_update_gate_entry_hook;
        test_update_gate_entry_hook = NULL;
        hook();
    }
}
static uint32_t test_selection_lifetime_ms;
static bool test_boot_boundary_enabled, test_boot_lock_allowed, test_boot_locked;
static bool test_boot_active;
static lv_display_t *test_boot_display;
static unsigned test_boot_lock_calls, test_boot_create_calls, test_boot_update_calls;
static app_boot_ui_view_t test_boot_view;

/* Media boundary substitutes observe the real coordinator's decisions. Audio,
 * recognizer, persistence and UI adapters have separate production suites. */
static app_media_snapshot_t test_media_snapshot;
static app_presenter_indicator_status_t test_indicator;
static ae_environment_t test_media_environment;
static app_audio_capture_context_t test_capture_context;
static uint64_t test_capture_context_ms;
static app_media_controls_context_t test_media_context;
static unsigned test_media_stops;
static bool test_media_speech_context;
static app_media_controls_effect_t test_media_effect;
static bool test_media_handle;
static unsigned test_media_handles;
static bool test_control_session_alive;
static cp_device_state_t test_control_state;
static cp_result_t test_control_result;
static unsigned test_control_completions;
static bool test_control_completion_reject;
static app_update_snapshot_t test_update_snapshot;
static app_update_authority_t test_update_authority;
static cp_error_t test_update_authority_error = CP_UNAVAILABLE;
static bool test_update_busy, test_wifi_maintenance;
static unsigned test_update_health_requests;
static esp_err_t test_update_health_error;
static board_haptic_status_t test_haptic_status;
static board_imu_status_t test_update_imu;
static app_presenter_status_t test_update_presentation;
static ble_hid_target_snapshot_t test_update_target;
static bt_audio_bond_list_t test_classic_bonds;
static app_wifi_profiles_snapshot_t test_wifi_profiles;
static app_device_restart_status_t test_restart_status;
static esp_err_t test_restart_error;
static bool test_restart_authorized, test_restart_maintenance;
static unsigned test_restart_requests;
static bool test_remote_target_accept, test_remote_wifi_forget_accept, test_remote_haptic_accept, test_remote_bond_forget_accept;
static unsigned test_remote_target_calls, test_remote_wifi_forget_calls, test_remote_haptic_calls, test_remote_bond_forget_calls;
static ble_hid_target_preferences_request_t test_remote_target_request;
static uint64_t test_remote_wifi_forget_revision;
static bool test_remote_wifi_forget_acknowledge;
static ble_hid_bond_forget_snapshot_t test_remote_bond_forget;
static size_t test_heap_calls, test_heap_aligned_calls, test_heap_outstanding, test_heap_fail_at;
static bool test_media_authority_available;
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++test_heap_calls;
    if (test_heap_calls == test_heap_fail_at) return NULL;
    void *memory = test_heap_allocate(4, count, bytes);
    if (memory) ++test_heap_outstanding;
    return memory;
}
void *heap_caps_aligned_calloc(size_t alignment, size_t count, size_t bytes, uint32_t caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++test_heap_calls; ++test_heap_aligned_calls;
    if (test_heap_calls == test_heap_fail_at) return NULL;
    void *memory = test_heap_allocate(alignment, count, bytes);
    if (memory) ++test_heap_outstanding;
    return memory;
}
void heap_caps_free(void *memory)
{
    if (memory) { assert(test_heap_outstanding); --test_heap_outstanding; }
    test_heap_free(memory);
}
void esp_fill_random(void *memory, size_t bytes) { memset(memory, 0x17, bytes); }
esp_err_t app_control_runtime_init(const app_control_host_t *host) { (void)host; return ESP_OK; }
void app_control_runtime_snapshot(app_control_runtime_snapshot_t *out) { *out = test_control_runtime; }
esp_err_t app_control_runtime_attach_usb_stdio(void) { return ESP_OK; }
esp_err_t app_control_runtime_publish_state(const cp_device_state_t *state) { test_control_state = *state; return ESP_OK; }
esp_err_t app_control_runtime_complete(uint64_t key, const cp_result_t *result)
{ assert(key == result->command_key); if (test_control_completion_reject) return ESP_ERR_INVALID_STATE;
  test_control_result = *result; ++test_control_completions; return ESP_OK; }
void app_control_runtime_physical_override(bool locked) { (void)locked; }
bool app_control_runtime_session_alive(const uint8_t session[CP_ID_BYTES], const uint8_t principal[CP_ID_BYTES])
{ return test_control_session_alive && session[0] && principal[0]; }
bool app_control_runtime_wall_time(uint64_t *seconds) { (void)seconds; return false; }
cp_error_t app_control_runtime_update_authority(const cp_command_t *command, app_update_authority_t *out)
{ (void)command; *out = test_update_authority; return test_update_authority_error; }
void app_update_snapshot(app_update_snapshot_t *out) { *out = test_update_snapshot; }
bool app_update_flash_busy(void) { return test_update_busy; }
esp_err_t app_update_confirm_health(bool gates)
{ assert(gates); ++test_update_health_requests; return test_update_health_error; }
void app_wifi_set_maintenance(bool active) { test_wifi_maintenance = active; }
esp_err_t ble_hid_target_select(uint64_t revision, const ble_hid_target_t *target)
{
    ++test_remote_target_calls;
    test_remote_target_request.expected_bond_revision = revision;
    test_remote_target_request.target = *target;
    test_remote_target_request.selected = true;
    if (!test_remote_target_accept) return ESP_ERR_INVALID_STATE;
    ++test_update_target.request_id; test_update_target.target = *target;
    test_update_target.selected = true; test_update_target.request_status = BLE_HID_TARGET_PENDING;
    return ESP_OK;
}
ble_hid_target_snapshot_t ble_hid_target_snapshot(void) { return test_update_target; }
esp_err_t board_haptics_request(board_haptic_operation_t operation, board_feedback_t pattern, uint64_t *id)
{
    ++test_remote_haptic_calls;
    if (!test_remote_haptic_accept) return ESP_ERR_INVALID_STATE;
    *id = ++test_haptic_status.request_id;
    test_haptic_status.operation = operation; test_haptic_status.pattern = pattern;
    test_haptic_status.state = BOARD_HAPTIC_PENDING; test_haptic_status.worker_available = true;
    return ESP_OK;
}
board_haptic_status_t board_haptics_status(void) { return test_haptic_status; }
esp_err_t board_haptics_cancel(uint64_t id)
{
    if (!id || id != test_haptic_status.request_id) return ESP_ERR_INVALID_STATE;
    test_haptic_status.state = BOARD_HAPTIC_CANCELLED;
    test_haptic_status.off_confirmed = true;
    return ESP_OK;
}
bt_audio_bond_list_t bt_audio_bond_list(void) { return test_classic_bonds; }
void bt_audio_copy_bond_list(bt_audio_bond_list_t *out) { if (out) *out = test_classic_bonds; }
#ifndef TEST_COORDINATOR_MEDIA_REVIEW
void bt_audio_pairing_close(void) {}
#endif
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *out) { *out = test_wifi_profiles; }
esp_err_t app_wifi_profiles_forget_all(uint64_t revision, bool acknowledge)
{ (void)revision; (void)acknowledge; return ESP_ERR_INVALID_STATE; }
esp_err_t app_wifi_profiles_forget_all_request(uint64_t revision, bool acknowledge, uint64_t *id)
{
    ++test_remote_wifi_forget_calls; test_remote_wifi_forget_revision = revision;
    test_remote_wifi_forget_acknowledge = acknowledge;
    if (!test_remote_wifi_forget_accept) return ESP_ERR_INVALID_STATE;
    *id = ++test_wifi_profiles.request_id; test_wifi_profiles.operation_pending = true;
    return ESP_OK;
}
esp_err_t ble_hid_target_preferences_request(const ble_hid_target_preferences_request_t *request, uint64_t *id)
{
    ++test_remote_target_calls; test_remote_target_request = *request;
    if (!test_remote_target_accept) return ESP_ERR_INVALID_STATE;
    *id = ++test_update_target.request_id; test_update_target.target = request->target;
    test_update_target.selected = request->selected; test_update_target.request_status = BLE_HID_TARGET_PENDING;
    return ESP_OK;
}
bool ble_hid_forget_bonds_without_pairing(uint64_t generation)
{ ++test_pair_forgets; test_forget_generation = generation; return test_pair_accept; }
esp_err_t ble_hid_forget_bond_request(uint64_t generation, uint64_t revision, const ble_hid_target_t *target, uint64_t *id)
{
    ++test_remote_bond_forget_calls;
    if (!test_remote_bond_forget_accept) return ESP_ERR_INVALID_STATE;
    *id = ++test_remote_bond_forget.request_id;
    test_remote_bond_forget.generation = generation; test_remote_bond_forget.bond_revision = revision;
    test_remote_bond_forget.target = *target; test_remote_bond_forget.status = BLE_HID_BOND_FORGET_PENDING;
    test_remote_bond_forget.cancellable = true;
    return ESP_OK;
}
esp_err_t ble_hid_forget_bond_cancel(uint64_t id)
{
    if (id != test_remote_bond_forget.request_id || !test_remote_bond_forget.cancellable) return ESP_ERR_INVALID_STATE;
    test_remote_bond_forget.status = BLE_HID_BOND_FORGET_CANCELLED; return ESP_OK;
}
ble_hid_bond_forget_snapshot_t ble_hid_bond_forget_snapshot(void) { return test_remote_bond_forget; }
esp_err_t app_device_restart_init(void) { return test_restart_error; }
esp_err_t app_device_restart_request(uint64_t deadline_ms, uint64_t *id)
{
    ++test_restart_requests;
    if (test_restart_error != ESP_OK || deadline_ms <= s_now_ms) return ESP_ERR_INVALID_STATE;
    test_restart_status.request_id++; *id = test_restart_status.request_id;
    test_restart_status.state = APP_DEVICE_RESTART_PENDING;
    return ESP_OK;
}
void app_device_restart_authorize(uint64_t id, bool authorized, bool ready)
{ assert(id == test_restart_status.request_id); test_restart_authorized = authorized; test_restart_maintenance = ready; }
esp_err_t app_device_restart_cancel(uint64_t id)
{
    if (id != test_restart_status.request_id || test_restart_status.state != APP_DEVICE_RESTART_PENDING) return ESP_ERR_INVALID_STATE;
    test_restart_status.state = APP_DEVICE_RESTART_CANCELLED; return ESP_OK;
}
app_device_restart_status_t app_device_restart_status(void) { return test_restart_status; }
ae_result_t app_recording_store_init(const app_recording_store_config_t *config)
{ (void)config; return AE_UNAVAILABLE; }
ae_store_port_t app_recording_store_port(void) { return (ae_store_port_t){0}; }
bool app_recording_store_snapshot(app_recording_store_snapshot_t *out)
{ memset(out, 0, sizeof(*out)); return false; }
ae_codec_port_t board_audio_mosaico_port(void) { return (ae_codec_port_t){0}; }
void board_audio_capture_snapshot(board_audio_capture_snapshot_t *out)
{ *out = (board_audio_capture_snapshot_t){0}; }
cp_error_t app_control_runtime_capture_authority(const cp_capture_authority_request_t *request,
    cp_capture_authority_t *out)
{ (void)request; memset(out, 0, sizeof(*out)); return CP_FORBIDDEN; }
bool app_control_trust_grant(const uint8_t id[CP_ID_BYTES], cp_capture_grant_t *out)
{ (void)id; memset(out, 0, sizeof(*out)); return false; }
ae_result_t app_media_runtime_set_remote_authority(const app_media_remote_authority_t *port)
{ (void)port; return test_media_authority_available ? AE_OK : AE_UNAVAILABLE; }
ae_result_t app_media_runtime_submit_remote(const app_remote_media_request_t *request)
{ (void)request; return AE_UNAVAILABLE; }
bool app_media_runtime_poll_remote(uint64_t token, app_audio_result_t *out)
{ (void)token; (void)out; return false; }
void app_media_runtime_release_remote_result(uint64_t token) { (void)token; }
void app_media_runtime_cancel_remote(uint64_t token, const cp_identity_t *identity, ae_stop_reason_t reason)
{ (void)token; (void)identity; (void)reason; }
cp_error_t app_media_runtime_read_recordings(const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{ (void)identity; (void)resource; (void)json; (void)capacity; *length = 0; return CP_UNAVAILABLE; }
ae_result_t app_media_runtime_route(ae_route_t route, ae_route_capabilities_t *out)
{ (void)route; memset(out, 0, sizeof(*out)); return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_confirm(uint64_t token, uint64_t epoch, uint64_t revision, char *candidate)
{ (void)token; (void)epoch; (void)revision; (void)candidate; return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_reject(uint64_t token, uint64_t epoch, uint64_t revision)
{ (void)token; (void)epoch; (void)revision; return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_copy_records(uint64_t token, ae_record_metadata_t *records, size_t capacity)
{ (void)token; (void)records; (void)capacity; return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_copy_record_data(uint64_t token, size_t offset, void *data, size_t bytes)
{ (void)token; (void)offset; (void)data; (void)bytes; return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_copy_remote_records(uint64_t token, ae_record_metadata_t *records, size_t capacity)
{ (void)token; (void)records; (void)capacity; return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_copy_remote_data(uint64_t token, size_t offset, void *data, size_t bytes)
{ (void)token; (void)offset; (void)data; (void)bytes; return AE_UNAVAILABLE; }
ae_result_t app_media_runtime_init(const app_media_services_t *services)
{ (void)services; return AE_UNAVAILABLE; }
bool app_media_runtime_snapshot(app_media_snapshot_t *snapshot)
{
    if (test_media_snapshot_unavailable) return false;
    *snapshot = test_media_snapshot;
    return snapshot->initialized || test_media_uninitialized_snapshot_available;
}
/* The observer has a separate production-code suite; this boundary never
 * grants audio or input authority to coordinator tests. */
void app_audio_diagnostics_poll(uint64_t now_ms) { (void)now_ms; }
void app_audio_diagnostics_media_view(uint64_t observed_ms, const app_media_view_t *view)
{ (void)observed_ms; (void)view; }
void app_audio_diagnostics_capture_context(uint64_t observed_ms, const app_audio_capture_context_t *context)
{ assert(context); test_capture_context = *context; test_capture_context_ms = observed_ms; }
void app_media_runtime_environment(const ae_environment_t *environment, uint64_t epoch,
    uint64_t revision, bool speech)
{ (void)epoch; (void)revision; test_media_environment = *environment; test_media_speech_context = speech; }
void app_media_runtime_stop(ae_stop_reason_t reason)
{ assert(reason == AE_STOP_PRIVACY || reason == AE_STOP_MAINTENANCE); ++test_media_stops; }
esp_err_t app_presenter_bind_display(lv_display_t *display) { (void)display; return ESP_OK; }
app_presenter_indicator_status_t app_presenter_indicator_status(void) { return test_indicator; }
#ifndef TEST_COORDINATOR_MEDIA_REVIEW
bool app_media_controls_init(void) { return true; }
void app_media_controls_review_opened(app_confirmation_t confirmation, uint64_t epoch) { (void)confirmation; (void)epoch; }
void app_media_controls_context(const app_media_controls_context_t *context) { test_media_context = *context; }
bool app_media_controls_handle(const app_intent_t *intent, app_media_controls_effect_t *effect)
{
    (void)intent; ++test_media_handles;
    if (test_media_handle) *effect = test_media_effect;
    return test_media_handle;
}
void app_media_controls_poll(uint64_t now, app_media_controls_effect_t *effect)
{ (void)now; (void)effect; }
void app_media_controls_fill(app_view_t *view) { (void)view; }
#endif
bool app_connectivity_controls_init(void) { return true; }
static app_connectivity_controls_context_t test_connectivity_context;
void app_connectivity_controls_context(const app_connectivity_controls_context_t *context) { test_connectivity_context = *context; }
bool app_connectivity_controls_handle(const app_intent_t *intent, app_connectivity_controls_effect_t *effect)
{ (void)intent; (void)effect; return false; }
void app_connectivity_controls_poll(uint64_t now, app_connectivity_controls_effect_t *effect)
{ (void)now; (void)effect; }
void app_connectivity_controls_fill(app_view_t *view) { (void)view; }
bool app_connectivity_controls_bluetooth_revisions(uint64_t *bond, uint64_t *preference)
{
    if (!test_bonds.known || test_bonds.updating || !test_update_target.loaded) return false;
    *bond = test_bonds.revision; *preference = test_update_target.revision; return true;
}
bool app_connectivity_controls_bluetooth_busy(void) { return test_native_management_active; }
bool app_connectivity_controls_bluetooth_start(app_pairing_operation_t operation, uint64_t generation,
    uint64_t bond, uint64_t preference, app_connectivity_controls_effect_t *effect)
{
    ++test_native_management_starts; test_native_operation = operation; test_native_generation = generation;
    test_native_bond_revision = bond; test_native_preference_revision = preference;
    test_native_start_release_pending = s_engine.state.release_pending;
    bool valid = test_bonds.known && bond == test_bonds.revision && preference == test_update_target.revision;
    test_native_management_active = valid;
    *effect = (app_connectivity_controls_effect_t){.handled = true};
    strcpy(effect->message, valid ? "Bluetooth change admitted; wait for owner observations." : "Saved host revision changed.");
    return valid;
}
void app_connectivity_controls_bluetooth_stop(app_connectivity_controls_effect_t *effect)
{
    ++test_native_management_stops; test_native_management_active = false;
    *effect = (app_connectivity_controls_effect_t){.handled = true}; strcpy(effect->message, "Pairing stopped.");
}

usb_hid_snapshot_t usb_hid_snapshot(void) { return test_usb; }
void usb_hid_set_enabled(bool enabled)
{
    assert(test_usb_enable_count < 32);
    test_usb_enable_requests[test_usb_enable_count] = enabled;
    test_usb_enable_trace[test_usb_enable_count++] = trace_count;
    /* Documented adapter-boundary response: selection invalidates its session.
     * Endpoint readiness still requires an independent stack acknowledgement. */
    if (test_usb.enabled != enabled) {
        ++test_usb.generation;
        test_usb.ready = false;
    }
    test_usb.enabled = enabled;
    if (!enabled) test_usb.ready = false;
    /* Enabling alone never supplies a mounted/ready/completed acknowledgement. */
}
bool usb_hid_send_generation(uint64_t generation, uint8_t id,
                             const uint8_t *data, size_t length)
{
    ++test_usb_send_attempts;
    if (test_usb_send_failure || !test_usb.enabled || !test_usb.mounted || !test_usb.ready ||
        test_usb.failed || test_usb.suspended || test_usb.congested || generation != test_usb.generation ||
        !((id == 1 && length == 8) || (id == 2 && length == 4))) return false;
    assert(data && trace_count < sizeof trace / sizeof trace[0]);
    trace[trace_count] = (report_t){.transport = APP_INPUT_USB, .id = id, .length = length};
    memcpy(trace[trace_count].bytes, data, length);
    ++trace_count;
    if (test_usb_after_accept) {
        void (*hook)(void) = test_usb_after_accept;
        test_usb_after_accept = NULL;
        hook();
    }
    return true;
}
bool usb_hid_recover_generation(uint64_t generation)
{
    ++test_usb_recovers;
    return test_usb_recover_accept && generation == test_usb.generation;
}

esp_err_t board_power_read(board_power_status_t *status)
{
    assert(status);
    *status = test_power;
    return test_power_error;
}
esp_err_t board_brightness_set(int percent)
{
    assert(percent >= 0 && percent <= 100 && test_brightness_count < 32);
    test_brightness_requests[test_brightness_count++] = percent;
    /* Admission is deliberately not driver acknowledgement. */
    return test_brightness_error;
}
board_brightness_status_t board_brightness_status(void) { return test_brightness; }
esp_err_t settings_store_request(const device_settings_t *settings)
{
    assert(device_settings_validate(settings));
    ++test_storage_requests;
    test_saved_request = *settings;
    return test_storage_error;
}
settings_store_status_t settings_store_status(void) { return test_storage; }
void app_wifi_get_snapshot(app_wifi_snapshot_t *snapshot)
{
    assert(snapshot);
    *snapshot = test_wifi;
}
void app_wifi_get_scan_diagnostics(app_wifi_scan_diagnostics_t *out) { *out = test_scan_diagnostics; }
esp_err_t app_wifi_get_setup_info(app_wifi_setup_info_t *info)
{
    assert(info);
    ++test_wifi_info_reads;
    if (test_wifi_info_error == ESP_OK) *info = test_wifi_info;
    return test_wifi_info_error;
}
esp_err_t app_wifi_start_setup(void) { ++test_wifi_starts; return test_wifi_start_error; }
esp_err_t app_wifi_stop_setup(void) { ++test_wifi_stops; return test_wifi_stop_error; }
esp_err_t app_wifi_forget_network(void) { ++test_wifi_forgets; return test_wifi_forget_error; }
esp_err_t app_wifi_scan_start(void) { ++test_scans; return test_scan_error; }
esp_err_t app_wifi_scan_cancel(void) { ++test_scan_cancels; return test_scan_error; }
void app_wifi_get_scan_snapshot(app_wifi_scan_snapshot_t *snapshot) { assert(snapshot); *snapshot = test_scan; }
esp_err_t app_wifi_get_scan_record(uint32_t generation, uint16_t index, app_wifi_scan_record_t *record)
{
    assert(record);
    if (test_scan.state != APP_WIFI_SCAN_READY || generation != test_scan.generation ||
        index >= test_scan.count || index >= APP_WIFI_SCAN_CAPACITY) return ESP_ERR_INVALID_STATE;
    *record = test_scan_records[index];
    return ESP_OK;
}
esp_err_t app_wifi_select_scan_record(uint32_t generation, uint16_t index, app_wifi_selection_t *selection)
{
    assert(selection);
    ++test_selections;
    if (test_selection_error != ESP_OK) return test_selection_error;
    app_wifi_scan_record_t record;
    esp_err_t result = app_wifi_get_scan_record(generation, index, &record);
    if (result != ESP_OK) return result;
    test_selection = (app_wifi_selection_t){.id = 31, .scan_generation = generation,
        .record_index = index, .remaining_ms = test_selection_lifetime_ms, .record = record};
    *selection = test_selection;
    return ESP_OK;
}
esp_err_t app_wifi_release_selection(uint32_t id)
{
    assert(id);
    ++test_selection_releases;
    test_released_selection = id;
    return ESP_OK;
}
esp_err_t app_wifi_join(const app_wifi_join_request_t *request)
{
    assert(request);
    ++test_joins;
    test_join_request = *request;
    return test_join_error;
}
ble_hid_name_snapshot_t ble_hid_name_snapshot(void) { return test_name; }
esp_err_t ble_hid_name_request(const char *name, size_t length, uint64_t *request_id)
{
    assert(device_settings_bluetooth_name_valid(name, length));
    ++test_name_requests;
    if (test_name_error != ESP_OK) return test_name_error;
    ++test_name.request_id;
    test_name.status = BLE_HID_NAME_PENDING;
    memset(test_name.requested, 0, sizeof test_name.requested);
    memcpy(test_name.requested, name, length);
    if (request_id) *request_id = test_name.request_id;
    return ESP_OK;
}
bool ble_hid_pairing_open(uint32_t duration_ms)
{
    assert(duration_ms == BLE_HID_PAIRING_DEFAULT_MS);
    ++test_pair_opens;
    return test_pair_accept;
}
void ble_hid_pairing_close(void) { ++test_pair_closes; }
bool ble_hid_forget_bonds(uint64_t generation)
{
    ++test_pair_forgets;
    test_forget_generation = generation;
    return test_pair_accept;
}
ble_hid_bond_list_t ble_hid_bond_list(void) { return test_bonds; }
void ble_hid_copy_bond_list(ble_hid_bond_list_t *out) { if (out) *out = test_bonds; }

QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                uint8_t *bytes, StaticQueue_t *storage)
{
    assert(capacity > 0 && item_size > 0 && bytes && storage);
    *storage = (StaticQueue_t){
        .bytes = bytes, .capacity = capacity, .item_size = item_size
    };
    return storage;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    assert(queue && item && timeout == 0);
    if (queue->count == queue->capacity) return pdFALSE;
    const size_t tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->bytes + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    assert(queue && item && timeout == 0);
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->bytes + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue);
    queue->head = 0;
    queue->count = 0;
    return pdPASS;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    assert(queue && queue->count <= queue->capacity);
    return (UBaseType_t)queue->count;
}

bool ble_hid_send_generation(uint64_t generation, uint8_t id,
                             const uint8_t *data, size_t length)
{
    ++test_ble_send_attempts;
    if (test_send_failure || generation != test_link.generation || !test_link.connected ||
        !test_link.ready || test_link.congested ||
        !((id == 1 && length == 8) || (id == 2 && length == 4))) return false;
    assert(data && trace_count < sizeof trace / sizeof trace[0]);
    trace[trace_count] = (report_t){.id = id, .length = length};
    memcpy(trace[trace_count].bytes, data, length);
    ++trace_count;
    if (test_ble_after_accept) {
        void (*hook)(void) = test_ble_after_accept;
        test_ble_after_accept = NULL;
        hook();
    }
    return true;
}

void board_haptic_feedback(board_feedback_t event)
{
    assert(event >= BOARD_FEEDBACK_CONFIRM && event <= BOARD_FEEDBACK_ERROR);
}

void board_haptics_enable(bool enabled) { test_haptics_enabled = enabled; }
bool board_haptics_enabled(void) { return test_haptics_enabled; }
bool board_haptics_active(void) { return test_haptic_active; }
esp_err_t board_imu_read(board_imu_sample_t *sample)
{
    if (test_imu_result == ESP_OK) *sample = test_sample;
    return test_imu_result;
}

void coordinator_test_log(const char *tag, const char *format, ...)
{
    if (!test_diagnostic_logs_enabled) return;
    assert(strcmp(tag, "input") == 0);
    assert(test_diagnostic_log_count < sizeof test_diagnostic_logs / sizeof test_diagnostic_logs[0]);
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(test_diagnostic_logs[test_diagnostic_log_count],
                           sizeof test_diagnostic_logs[0], format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof test_diagnostic_logs[0]);
    ++test_diagnostic_log_count;
}

static _Noreturn void unexpected_boot_boundary(void)
{
    fputs("Unexpected boot/hardware operation in coordinator helper test\n", stderr);
    abort();
}

/* app_main is compiled, but not run. Boot-only boundaries must fail loudly
 * rather than supply success-shaped implementations for untested operations. */
esp_err_t usb_console_start(void) { unexpected_boot_boundary(); }
esp_err_t usb_hid_init(void) { unexpected_boot_boundary(); }
esp_err_t settings_store_init(device_settings_t *settings)
{
    (void)settings;
    unexpected_boot_boundary();
}
esp_err_t app_wifi_init(void) { unexpected_boot_boundary(); }
esp_err_t nvs_flash_init(void) { unexpected_boot_boundary(); }
const esp_app_desc_t *esp_app_get_description(void)
{
#if defined(CONFIG_APP_IMU_CAPTURE) && CONFIG_APP_IMU_CAPTURE
    static const esp_app_desc_t description = {.version = "observer-fixture", .app_elf_sha256 = {1, 2, 3}};
    return &description;
#else
    unexpected_boot_boundary();
#endif
}
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)mac;
    (void)type;
    unexpected_boot_boundary();
}
esp_err_t board_button_events_register(board_button_event_callback_t callback)
{
    (void)callback;
    unexpected_boot_boundary();
}
esp_err_t board_init(void (*callback)(void))
{
    (void)callback;
    unexpected_boot_boundary();
}
board_capabilities_t board_capabilities(void) { return test_capabilities; }
lv_display_t *board_display_get(void)
{
    if (!test_boot_boundary_enabled) unexpected_boot_boundary();
    return test_boot_display;
}
bool board_display_lock(uint32_t timeout)
{
    if (!test_boot_boundary_enabled) unexpected_boot_boundary();
    assert(timeout == 100 && !test_boot_locked);
    ++test_boot_lock_calls;
    test_boot_locked = test_boot_lock_allowed;
    return test_boot_locked;
}
void board_display_unlock(void)
{
    if (!test_boot_boundary_enabled) unexpected_boot_boundary();
    assert(test_boot_locked);
    test_boot_locked = false;
}
board_imu_status_t board_imu_status(void) { return test_update_imu; }
esp_err_t app_presenter_init(void) { unexpected_boot_boundary(); }
bool app_boot_ui_create(lv_display_t *display)
{
    if (!test_boot_boundary_enabled) unexpected_boot_boundary();
    assert(display && display == test_boot_display && test_boot_locked && !test_boot_active);
    ++test_boot_create_calls;
    test_boot_active = true;
    return true;
}
bool app_boot_ui_update(const app_boot_ui_view_t *view)
{
    if (!test_boot_boundary_enabled) unexpected_boot_boundary();
    assert(view && test_boot_locked && test_boot_active);
    test_boot_view = *view;
    ++test_boot_update_calls;
    return true;
}
void app_boot_ui_destroy(void) { unexpected_boot_boundary(); }
bool app_boot_ui_active(void)
{
    if (!test_boot_boundary_enabled) unexpected_boot_boundary();
    assert(test_boot_locked);
    return test_boot_active;
}
void app_ui_set_local_edit_submit(app_local_edit_submit_fn submit)
{
    (void)submit;
    unexpected_boot_boundary();
}
app_presenter_status_t app_presenter_status(void) { return test_update_presentation; }
void app_ui_create(lv_display_t *display, app_submit_fn submit)
{
    (void)display;
    (void)submit;
    unexpected_boot_boundary();
}
void app_ui_render(const app_view_t *view)
{
    (void)view;
    unexpected_boot_boundary(); /* Runtime coordinator must only publish. */
}
app_presenter_result_t app_presenter_publish(const app_view_t *view)
{
    assert(view);
    if (!test_presenter_available) return APP_PRESENTER_UNAVAILABLE;
    /* Capture publication only. Acceptance is not rendering or acknowledgement;
     * tests/presenter separately executes the actual handoff implementation. */
    test_view = *view;
    snprintf(test_target, sizeof test_target, "%s", view->target_name ? view->target_name : "");
    snprintf(test_message, sizeof test_message, "%s", view->message ? view->message : "");
    test_view.target_name = view->target_name ? test_target : NULL;
    test_view.message = view->message ? test_message : NULL;
    ++test_publish_count;
    return APP_PRESENTER_ACCEPTED;
}
esp_err_t ble_hid_init(void) { unexpected_boot_boundary(); }
ble_hid_snapshot_t ble_hid_snapshot(void) { return test_link; }
ble_hid_failure_t ble_hid_failure(void) { unexpected_boot_boundary(); }
void ble_hid_copy_advertising_diagnostics(ble_hid_advertising_diagnostics_t *out)
{ if (out) *out = test_advertising_trace; }
const char *ble_hid_advertising_event_name(ble_hid_advertising_event_t event)
{ (void)event; return "unknown"; }
void ble_hid_copy_admission_diagnostics(ble_hid_admission_diagnostics_t *out)
{ if (out) *out = test_admission_trace; }
const char *ble_hid_admission_event_name(ble_hid_admission_event_t event)
{ (void)event; return "unknown"; }
bool ble_hid_resume_reconnect(void)
{ if (test_link.connection_admitted || !test_link.reconnect_paused) return false; test_link.reconnect_paused = false; return true; }
bool ble_hid_disconnect_user_generation(uint64_t generation)
{ bool accepted = test_link.connection_admitted && ble_hid_disconnect_generation(generation); if (accepted) test_link.reconnect_paused = true; return accepted; }
bool ble_hid_disconnect_generation(uint64_t generation)
{
    ++test_disconnects;
    test_disconnect_generation = generation;
    test_disconnect_trace_count = trace_count;
    if (!test_disconnect_accept || !test_link.connected || generation != test_link.generation)
        return false;
    /* Production admission immediately revokes readiness and advances the
     * public generation; the asynchronous DISCONNECT event arrives later. */
    ++test_link.generation;
    if (!test_link.generation) ++test_link.generation;
    test_link.ready = false;
    return true;
}
const char *esp_err_to_name(esp_err_t error)
{
    if (error == ESP_OK) return "ESP_OK";
    if (error == ESP_FAIL) return "ESP_FAIL";
    if (error == ESP_ERR_TIMEOUT) return "ESP_ERR_TIMEOUT";
    return "TEST_UNKNOWN_ERROR";
}
int64_t esp_timer_get_time(void)
{
    return (int64_t)((test_clock_override ? test_clock_ms : s_now_ms) * 1000U);
}
esp_err_t esp_task_wdt_add(void *task)
{
    (void)task;
    unexpected_boot_boundary();
}
esp_err_t esp_task_wdt_reset(void) { unexpected_boot_boundary(); }
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    unexpected_boot_boundary();
}
UBaseType_t uxTaskGetStackHighWaterMark(void *task)
{
    (void)task;
    unexpected_boot_boundary();
}
size_t heap_caps_get_free_size(uint32_t capabilities)
{
    if (test_heap_observation_enabled) {
        ++test_heap_observation_count;
        if (capabilities == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return test_heap_free_internal;
        if (capabilities == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return test_heap_free_dma;
    }
    unexpected_boot_boundary();
}
size_t heap_caps_get_minimum_free_size(uint32_t capabilities)
{
    (void)capabilities;
    unexpected_boot_boundary();
}
size_t heap_caps_get_largest_free_block(uint32_t capabilities)
{
    if (test_heap_observation_enabled) {
        ++test_heap_observation_count;
        if (capabilities == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return test_heap_largest_internal;
        if (capabilities == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return test_heap_largest_dma;
    }
    unexpected_boot_boundary();
}
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    (void)callback;
    unexpected_boot_boundary();
}

static hid_engine_step_result_t step(void)
{
    const size_t before = trace_count;
    input_link_t active = input_link_snapshot(&test_link);
    const hid_engine_step_result_t result =
        hid_engine_step(&s_engine, send_report, &active.generation);
    assert(trace_count - before <= 1);
    return result;
}

static void expect_report(uint8_t id, const uint8_t *bytes, size_t length)
{
    const size_t before = trace_count;
    hid_engine_step_result_t result = step();
    if (result != HID_ENGINE_STEP_SENT || trace_count != before + 1)
        fprintf(stderr, "%s: expected one report; step=%d reports=%zu time=%llu motion=%u block=%d\n",
            test_case, (int)result, trace_count - before, (unsigned long long)s_now_ms,
            (unsigned)s_interaction.view.motion_status, (int)s_interaction.view.motion_block_reason);
    assert(result == HID_ENGINE_STEP_SENT && trace_count == before + 1);
    const report_t *report = &trace[before];
    assert(report->id == id && report->length == length);
    if (memcmp(report->bytes, bytes, length) != 0) {
        fprintf(stderr, "%s: report %zu mismatch; actual:", test_case, before);
        for (size_t i = 0; i < length; ++i) fprintf(stderr, " %02x", report->bytes[i]);
        fputs(" expected:", stderr);
        for (size_t i = 0; i < length; ++i) fprintf(stderr, " %02x", bytes[i]);
        fputc('\n', stderr);
    }
    assert(memcmp(report->bytes, bytes, length) == 0);
}

static void release_barrier(void)
{
    static const uint8_t zero[8] = {0};
    if (!s_engine.state.release_pending)
        fprintf(stderr, "%s: expected a pending neutral-release barrier\n", test_case);
    assert(s_engine.state.release_pending);
    expect_report(1, zero, 8);
    assert(s_engine.state.release_pending);
    expect_report(2, zero, 4);
    assert(!s_engine.state.release_pending);
}

static void expect_idle(void)
{
    const size_t before = trace_count;
    hid_engine_step_result_t result = step();
    if (result != HID_ENGINE_STEP_IDLE || trace_count != before)
        fprintf(stderr, "%s: expected idle at report %zu; step=%d reports=%zu\n",
            test_case, before, (int)result, trace_count);
    assert(result == HID_ENGINE_STEP_IDLE && trace_count == before);
}

static void expect_empty_intents(void)
{
    coordinator_event_t event;
    assert(xQueueReceive(s_intents, &event, 0) == pdFALSE);
}

static app_intent_t make_intent(app_action_t action, int x, int y, const char *text)
{
    app_intent_t intent = {
        .action = action, .epoch = s_engine.state.epoch,
        .ui_revision = s_ui_revision, .x = x, .y = y
    };
    if (text) {
        assert(strlen(text) < sizeof intent.text);
        memcpy(intent.text, text, strlen(text) + 1);
    }
    return intent;
}

static void consume_one(void)
{
    coordinator_event_t event;
    assert(xQueueReceive(s_intents, &event, 0) == pdTRUE);
    consume_coordinator_event(&event, &test_link);
}

static void deliver(app_intent_t intent)
{
    assert(submit_intent(&intent));
    consume_one();
}

static void setup(input_mode_t mode)
{
    heap_caps_free(s_remote_input); heap_caps_free(s_command_bridge);
    heap_caps_free(s_remote_device); s_remote_device = NULL;
    heap_caps_free(s_remote_media); s_remote_media = NULL;
    heap_caps_free(s_remote_media_preview); s_remote_media_preview = NULL;
    assert(!test_heap_outstanding);
    test_heap_calls = test_heap_aligned_calls = test_heap_fail_at = 0;
    test_media_authority_available = false;
    s_remote_media_immediate_ready = false;
    memset(&s_remote_media_immediate, 0, sizeof(s_remote_media_immediate));
    memset(s_remote_device_jobs, 0, sizeof(s_remote_device_jobs));
    memset(s_remote_device_reviews, 0, sizeof(s_remote_device_reviews));
    memset(s_remote_device_local_review, 0, sizeof(s_remote_device_local_review));
    memset(&s_update_gate, 0, sizeof(s_update_gate));
    test_update_gate_entry_hook = NULL;
    s_update_access = APP_LOCAL_NORMAL; s_update_health_since = 0;
    s_update_health_imu_recoveries = 0;
    memset(&s_update_health_trace, 0, sizeof s_update_health_trace);
    test_update_snapshot = (app_update_snapshot_t){.state = APP_UPDATE_UNAVAILABLE};
    test_update_authority = (app_update_authority_t){0};
    test_update_authority_error = CP_UNAVAILABLE;
    test_update_busy = test_wifi_maintenance = false;
    test_update_health_requests = 0;
    test_update_health_error = ESP_OK;
    test_haptic_status = (board_haptic_status_t){0};
    test_update_imu = (board_imu_status_t){0};
    test_update_presentation = (app_presenter_status_t){0};
    test_update_target = (ble_hid_target_snapshot_t){0};
    test_classic_bonds = (bt_audio_bond_list_t){0};
    test_wifi_profiles = (app_wifi_profiles_snapshot_t){0};
    test_restart_status = (app_device_restart_status_t){0};
    test_restart_error = ESP_ERR_INVALID_STATE;
    test_restart_authorized = test_restart_maintenance = false;
    test_restart_requests = 0;
    test_remote_target_accept = test_remote_wifi_forget_accept = test_remote_haptic_accept = test_remote_bond_forget_accept = false;
    test_remote_target_calls = test_remote_wifi_forget_calls = test_remote_haptic_calls = test_remote_bond_forget_calls = 0;
    test_remote_target_request = (ble_hid_target_preferences_request_t){0};
    test_remote_wifi_forget_revision = 0; test_remote_wifi_forget_acknowledge = false;
    test_remote_bond_forget = (ble_hid_bond_forget_snapshot_t){0};
    s_remote_device_maintenance_active = false;
    s_remote_device_maintenance_revision = 1;
    remote_device_restart_available = false;
    s_remote_input = NULL; s_command_bridge = NULL;
    s_remote_transport_pending = s_applying_remote_safety = false;
    s_control_state_published = false;
    s_management_generation = s_state_sequence = 1;
    s_remote_allowance_generation = 1;
    s_remote_allowed = false;
    test_control_session_alive = false; test_control_completion_reject = false;
    test_control_completions = 0;
    test_control_state = (cp_device_state_t){0};
    test_control_result = (cp_result_t){0};
    test_control_runtime = (app_control_runtime_snapshot_t){.error = ESP_ERR_INVALID_STATE};
    test_scan_diagnostics = (app_wifi_scan_diagnostics_t){0};
    memset(&s_motion_trace, 0, sizeof s_motion_trace);
    test_media_snapshot_unavailable = false;
    test_media_snapshot = (app_media_snapshot_t){0};
    test_media_uninitialized_snapshot_available = false;
    test_indicator = (app_presenter_indicator_status_t){0};
    test_media_environment = (ae_environment_t){0};
    test_capture_context = (app_audio_capture_context_t){0}; test_capture_context_ms = 0;
    test_media_context = (app_media_controls_context_t){0};
    test_media_stops = test_media_handles = 0;
    test_media_speech_context = test_media_handle = false;
    test_media_effect = (app_media_controls_effect_t){0};
    test_clock_override = false;
    test_clock_ms = 0;
    test_selection_lifetime_ms = 300000;
    test_diagnostic_logs_enabled = test_heap_observation_enabled = false;
    memset(&test_advertising_trace, 0, sizeof(test_advertising_trace));
    memset(&test_admission_trace, 0, sizeof(test_admission_trace));
    test_diagnostic_log_count = test_heap_observation_count = 0;
    memset(test_diagnostic_logs, 0, sizeof test_diagnostic_logs);
    test_heap_free_internal = test_heap_largest_internal = 0;
    test_heap_free_dma = test_heap_largest_dma = 0;
    atomic_store(&s_allocation_failure_state, 0);
    s_allocation_failure_size = 0;
    s_allocation_failure_caps = 0;
    s_allocation_failure_function = NULL;
    memset(s_boot_memory, 0, sizeof s_boot_memory);
    s_boot_memory_count = 0;
    s_intents = xQueueCreateStatic(INTENT_CAPACITY, sizeof(coordinator_event_t),
                                   s_intent_bytes, &s_intent_storage);
    s_priority_requests = 0;
    interaction_multitouch_init(&s_multitouch);
    s_multitouch_available = s_multitouch_consuming = s_multitouch_eligible = false;
    memset(s_touch_emergency, 0, sizeof s_touch_emergency);
    memset(s_touch_emergency_pending, 0, sizeof s_touch_emergency_pending);
    s_overlay = APP_OVERLAY_NONE;
    s_keyboard_submode = APP_KEYBOARD_TOUCH;
    s_host_os = APP_HOST_CTRL;
    s_audio_kind = APP_AUDIO_VOICE;
    s_dark_theme = true;
    s_ui_revision = 1;
    s_settings_revision = 1;
    s_now_ms = 1000;
    device_settings_defaults(&s_settings);
    s_durable_settings = s_settings;
    device_idle_init(&s_idle, s_now_ms);
    s_settings_page = APP_SETTINGS_HOME;
    s_bluetooth_page = 0;
    s_input_transport = s_requested_transport = APP_INPUT_BLUETOOTH;
    s_transport_switch_pending = false;
    s_transport_switch_persist = true;
    s_transport_switch_deadline_ms = 0;
    s_transport_origin_generation = 0;
    s_confirmation = APP_CONFIRM_NONE;
    s_confirmation_generation = 0;
    memset(s_confirmation_ssid, 0, sizeof s_confirmation_ssid);
    memset(s_confirmation_stored_ssid, 0, sizeof s_confirmation_stored_ssid);
    s_confirmation_storage_known = s_confirmation_storage_present = false;
    s_confirmation_storage_uncertain = false;
    s_wifi_page = 0;
    s_wifi_selection_generation = 0;
    s_wifi_selection_index = 0;
    memset(&s_wifi_reserved, 0, sizeof s_wifi_reserved);
    memset(&s_local_editor, 0, sizeof s_local_editor);
    memset(&s_local_submission, 0, sizeof s_local_submission);
    s_local_submission_pending = false;
    s_editor_sequence = s_editor_deadline_ms = 0;
    s_name_request_id = 0;
    s_name_should_persist = false;
    s_pair_operation = APP_CONFIRM_NONE;
    s_pair_deadline_ms = 0;
    s_pair_origin_generation = 0;
    s_pair_disconnect_requested = false;
    s_screen_accepts_input = true;
    s_brightness_requested = -1;
    s_touch_token = s_ai_token = 0;
    s_idle_touch_gesture = 0;
    s_user_activity_ms = s_seen_activity_ms = 0;
    memset(s_firmware_version, 0, sizeof s_firmware_version);
    memset(s_build_id, 0, sizeof s_build_id);
    memset(s_device_id, 0, sizeof s_device_id);
    s_send_token = 0;
    s_send_epoch = 0;
    s_send_base = 0;
    s_send_batch = false;
    s_drag_requested = false;
    s_touch_down = false;
    s_physical_down = false;
    s_touch_gesture = 0;
    s_consumed_gesture = 0;
    s_live_touch_gesture = 0;
    s_touch_transition_ms = 0;
    s_last_imu_us = 0;
    s_button_head = 0;
    s_button_count = 0;
    s_button_overflow = false;
    s_button_resync = false;
    s_button_down_us = s_button_access_since_us = 0;
    s_button_context_revision = 0;
    s_button_context_epoch = 0;
    memset(s_button_edges, 0, sizeof s_button_edges);
    hid_engine_init(&s_engine);
    interaction_init(&s_interaction);
    interaction_set_keyboard_motion(&s_interaction, s_keyboard_submode == APP_KEYBOARD_MOTION);
    interaction_reference_diagnostics_bind(&s_interaction, &s_reference_diagnostics);
    assert(configure_motion_preferences(&s_settings));
    test_link = (ble_hid_snapshot_t){
        .initialized = true, .connected = true, .connection_admitted = true, .ready = true,
        .encrypted = true, .generation = 17
    };
    test_haptics_enabled = true;
    test_haptic_active = false;
    test_send_failure = false;
    test_capabilities = (board_capabilities_t){
        .name = "Offline board boundary", .display = true, .touch = true,
        .lock_button = true, .imu = true, .haptics = true,
        .brightness = true, .battery = true
    };
    test_power = (board_power_status_t){0};
    test_power_error = ESP_FAIL;
    test_brightness = (board_brightness_status_t){.applied_known = true, .applied_percent = 50};
    test_brightness_error = ESP_OK;
    test_brightness_count = 0;
    test_storage = (settings_store_status_t){.available = true};
    test_storage_error = ESP_OK;
    test_storage_requests = 0;
    test_wifi = (app_wifi_snapshot_t){0};
    test_wifi_info = (app_wifi_setup_info_t){0};
    test_wifi_start_error = test_wifi_stop_error = test_wifi_forget_error = ESP_ERR_INVALID_STATE;
    test_wifi_info_error = ESP_ERR_INVALID_STATE;
    test_wifi_starts = test_wifi_stops = test_wifi_forgets = test_wifi_info_reads = 0;
    test_pair_accept = test_disconnect_accept = false;
    test_pair_opens = test_pair_closes = test_pair_forgets = test_disconnects = 0;
    test_native_management_starts = test_native_management_stops = 0;
    test_native_management_active = test_native_start_release_pending = false;
    test_disconnect_generation = test_forget_generation = 0;
    test_disconnect_trace_count = 0;
    test_bonds = (ble_hid_bond_list_t){0};
    test_usb = (usb_hid_snapshot_t){0};
    test_usb_send_failure = test_usb_recover_accept = false;
    test_usb_recovers = test_usb_send_attempts = test_ble_send_attempts = 0;
    test_usb_enable_count = 0;
    test_ble_after_accept = test_usb_after_accept = NULL;
    test_scan = (app_wifi_scan_snapshot_t){0};
    memset(test_scan_records, 0, sizeof test_scan_records);
    test_selection = (app_wifi_selection_t){0};
    test_scan_error = test_join_error = test_selection_error = ESP_OK;
    test_scans = test_scan_cancels = test_selections = test_selection_releases = test_joins = 0;
    test_released_selection = 0;
    memset(&test_join_request, 0, sizeof test_join_request);
    test_name = (ble_hid_name_snapshot_t){.active_known = true, .active = "ESP32-INPUT"};
    test_name_error = ESP_OK;
    test_name_requests = 0;
    test_sample = (board_imu_sample_t){0};
    test_imu_result = ESP_FAIL;
    test_presenter_available = true;
    test_publish_count = 0;
    trace_count = 0;
    hid_engine_set_link(&s_engine, true, true);
    assert(select_mode(mode));
    release_barrier();
    sync_interaction();
    trace_count = 0;
    expect_idle();
    assert(!process_priority());
    test_previous_link = input_link_snapshot(&test_link);
}

static void fill_intents(void)
{
    app_intent_t intent = make_intent(APP_KEY, 5, 0, NULL);
    /* Fill the boundary queue without requesting overflow cancellation, so
     * the explicit emergency request below is solely responsible for safety. */
    while (s_intents->count < s_intents->capacity) assert(submit_intent(&intent));
    const coordinator_event_t rejected = {.value.intent = intent};
    assert(xQueueSend(s_intents, &rejected, 0) == pdFALSE);
}

static void press(input_mode_t mode)
{
    if (mode == INPUT_KEYBOARD) {
        const uint8_t key[] = {2, 0, 4, 0, 0, 0, 0, 0};
        deliver(make_intent(APP_KEY, 4, 2, NULL));
        expect_report(1, key, sizeof key);
    } else {
        const uint8_t click[] = {1, 0, 0, 0};
        deliver(make_intent(APP_CLICK, 1, 0, NULL));
        expect_report(2, click, sizeof click);
    }
}

static void saturated_emergency_admission(void)
{
    const app_action_t emergencies[] = {APP_LOCK, APP_RELEASE};
    for (size_t i = 0; i < sizeof emergencies / sizeof emergencies[0]; ++i) {
        setup(INPUT_KEYBOARD);
        press(INPUT_KEYBOARD);
        deliver(make_intent(APP_KEY, 5, 0, NULL));
        fill_intents();
        app_intent_t emergency = make_intent(emergencies[i], 0, 0, NULL);
        emergency.epoch = 0; /* Emergency stop remains effective from an old UI. */
        assert(submit_intent(&emergency));
        assert(process_priority());
        assert(s_engine.state.locked == (emergencies[i] == APP_LOCK));
        expect_empty_intents();
        release_barrier();
        expect_idle();
        assert(trace_count == 3); /* Original press, then two releases only. */
        assert(!process_priority());
    }

    setup(INPUT_MOUSE);
    press(INPUT_MOUSE);
    fill_intents();
    app_intent_t overflow = make_intent(APP_MOVE, 99, 99, NULL);
    assert(!submit_intent(&overflow));
    assert(process_priority());
    assert(!s_engine.state.locked);
    expect_empty_intents();
    release_barrier();
    expect_idle();
    assert(trace_count == 3); /* Overflow causes release, never queued movement. */
}

static void stale_ui_epoch(void)
{
    setup(INPUT_MOUSE);
    app_intent_t stale_key = make_intent(APP_KEY, 5, 0, NULL);
    app_intent_t stale_mode = make_intent(APP_MODE, INPUT_AUDIO, 0, NULL);
    app_intent_t stale_recover = make_intent(APP_RECOVER, 0, 0, NULL);
    assert(submit_intent(&stale_key));
    assert(submit_intent(&stale_mode));
    assert(submit_intent(&stale_recover));
    const app_intent_t change = make_intent(APP_MODE, INPUT_KEYBOARD, 0, NULL);
    handle_intent(&change, &test_link);
    const uint64_t epoch = s_engine.state.epoch;
    assert(epoch != stale_key.epoch);
    release_barrier();
    consume_one();
    consume_one();
    consume_one();
    assert(s_engine.state.mode == INPUT_KEYBOARD && s_engine.state.epoch == epoch);
    expect_idle();
    const uint8_t key[] = {0, 0, 4, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    deliver(make_intent(APP_KEY, 4, 0, NULL));
    expect_report(1, key, sizeof key);
    expect_report(1, zero, sizeof zero);
    expect_idle();
}

static void touch_cannot_unlock(void)
{
    setup(INPUT_KEYBOARD);
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock) && process_priority());
    release_barrier();
    const app_action_t actions[] = {
        APP_LOCK, APP_RELEASE, APP_RECOVER, APP_MODE, APP_KEY,
        APP_MOVE, APP_CLICK, APP_SCROLL, APP_TEXT, APP_HAPTICS, (app_action_t)999
    };
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i) {
        app_intent_t touch = make_intent(actions[i], 1, 0, "a");
        assert(submit_intent(&touch));
        if (actions[i] == APP_LOCK || actions[i] == APP_RELEASE) {
            assert(process_priority());
        } else {
            consume_one();
        }
        assert(s_engine.state.locked && !s_engine.state.gyro_enabled);
        if (s_engine.state.release_pending) release_barrier();
        expect_idle();
    }
    for (size_t i = 0; i < trace_count; ++i)
        for (size_t byte = 0; byte < trace[i].length; ++byte)
            assert(trace[i].bytes[byte] == 0); /* Touch never emits a press while locked. */
}

static void physical_hold_toggle(void)
{
    setup(INPUT_MOUSE);
    assert(hid_engine_set_gyro(&s_engine, true));
    physical_lock();
    physical_lock(); /* Coalesced holds toggle once, not twice. */
    assert(process_priority() && s_engine.state.locked);
    assert(!s_engine.state.gyro_enabled);
    release_barrier();
    assert(!process_priority() && s_engine.state.locked);
    physical_lock();
    assert(process_priority() && !s_engine.state.locked);
    assert(s_engine.state.mode == INPUT_MOUSE && !s_engine.state.gyro_enabled);
    assert(s_engine.state.owner == INPUT_LOCAL);
    release_barrier();
    expect_idle();
    assert(!process_priority() && !s_engine.state.locked);
}

static void simultaneous_lock_wins(void)
{
    for (unsigned int initially_locked = 0; initially_locked < 2; ++initially_locked) {
        for (unsigned int physical_first = 0; physical_first < 2; ++physical_first) {
            setup(INPUT_MOUSE);
            if (initially_locked) {
                physical_lock();
                assert(process_priority());
                release_barrier();
            }
            app_intent_t touch_lock = make_intent(APP_LOCK, 0, 0, NULL);
            if (physical_first) physical_lock();
            assert(submit_intent(&touch_lock));
            if (!physical_first) physical_lock();
            assert(process_priority());
            assert(s_engine.state.locked);
            if (s_engine.state.release_pending) release_barrier();
            expect_idle();
            assert(!process_priority() && s_engine.state.locked);
            physical_lock();
            assert(process_priority() && !s_engine.state.locked);
            release_barrier();
        }
    }
}

static void lock_releases_before_later_work(void)
{
    const input_mode_t modes[] = {INPUT_MOUSE, INPUT_KEYBOARD};
    static const uint8_t zero[8] = {0};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        const input_mode_t mode = modes[i];
        setup(mode);
        press(mode);
        app_intent_t old = make_intent(mode == INPUT_MOUSE ? APP_CLICK : APP_KEY, 5, 0, NULL);
        deliver(old); /* Already admitted behind the currently held press. */
        app_intent_t waiting = make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 7, 3, NULL);
        assert(submit_intent(&waiting)); /* A separate UI action is also pending. */
        app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
        assert(submit_intent(&lock) && process_priority());
        expect_empty_intents();
        assert(s_engine.state.locked);
        release_barrier();
        expect_idle();
        assert(trace_count == 3);
        deliver(make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 6, 0, NULL));
        expect_idle();
        physical_lock();
        assert(process_priority() && !s_engine.state.locked);
        deliver(make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 6, 0, NULL));
        release_barrier(); /* Fresh requests before the barrier cannot bypass it. */
        expect_idle();
        deliver(old); /* Old accepted work is not revived by unlock. */
        expect_idle();
        deliver(make_intent(mode == INPUT_MOUSE ? APP_MOVE : APP_KEY, 6, 0, NULL));
        if (mode == INPUT_MOUSE) {
            const uint8_t move[] = {0, 6, 0, 0};
            expect_report(2, move, sizeof move);
        } else {
            const uint8_t key[] = {0, 0, 6, 0, 0, 0, 0, 0};
            expect_report(1, key, sizeof key);
            expect_report(1, zero, sizeof zero);
        }
        expect_idle();
    }
}

static void action(app_action_t kind, int x)
{
    deliver(make_intent(kind, x, 0, NULL));
}

static void touch(app_action_t kind, uint32_t gesture, int dx, int dy)
{
    app_intent_t intent = make_intent(kind, dx, dy, NULL);
    intent.gesture = gesture;
    intent.surface = APP_SURFACE_TOUCHPAD;
    deliver(intent);
}

static void buttons_at(uint64_t now_ms)
{
    s_now_ms = now_ms;
    process_buttons();
    (void)process_priority();
    sync_interaction();
}

static void button_edge(bool down, uint64_t now_ms)
{
    physical_button(down ? BOARD_BUTTON_DOWN : BOARD_BUTTON_UP, now_ms * 1000U);
    buttons_at(now_ms);
}

static void sample_after(uint64_t delay_ms, float x_dps, float y_dps)
{
    s_now_ms += delay_ms;
    test_sample = (board_imu_sample_t){
        /* Screen-up display frame: world-up yaw is +Z and pitch is +X.
         * Board mounting rotation is tested in the board adapter suite. */
        .timestamp_us = s_now_ms * 1000U, .x_dps = y_dps, .z_dps = x_dps, .z_g = 1
    };
    test_imu_result = ESP_OK;
    sync_interaction();
    process_motion();
}

static void calibrate(void)
{
    for (unsigned i = 0; i < 70; ++i) {
        sample_after(10, 0, 0);
        expect_idle();
    }
    assert(s_interaction.view.calibrated && !s_interaction.view.calibrating);
    interaction_config_t config = s_interaction.config;
    config.filter_seconds = 0;
    config.pointer_gain_x = 10;
    config.pointer_gain_y = 10;
    assert(interaction_configure(&s_interaction, &config));
}

static void settle_motion_reference(void)
{
    assert(s_engine.state.gyro_enabled && !s_interaction.gravity_ready);
    for (unsigned i = 0; i < 12; ++i) { sample_after(10, 0, 0); expect_idle(); }
    assert(s_interaction.gravity_ready && s_interaction.geometry_ready);
}

static void stale_context_and_modal_focus(void)
{
    setup(INPUT_KEYBOARD);
    app_intent_t stale = make_intent(APP_GRID, 1, 0, NULL);
    action(APP_GRID, 0); /* Change from groups to letters without changing HID epoch. */
    assert(s_interaction.view.group == 0 && s_interaction.view.preview_length == 0);
    assert(stale.epoch == s_engine.state.epoch && stale.ui_revision != s_ui_revision);
    deliver(stale);
    assert(s_interaction.view.preview_length == 0);
    action(APP_GRID, 0);
    assert(strcmp(s_interaction.view.preview, "A") == 0);
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    release_barrier();
    sync_interaction();
    action(APP_GRID, 0);
    action(APP_SPACE, 0);
    action(APP_SEND, 0);
    assert(strcmp(s_interaction.view.preview, "A") == 0 && !s_send_token);
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    buttons_at(start + 300);
    assert(s_overlay == APP_OVERLAY_SETTINGS);
    assert(strcmp(s_interaction.view.preview, "A") == 0);
    expect_idle();
    action(APP_CLOSE, 0);
    assert(s_overlay == APP_OVERLAY_NONE);
}

static void preview_editing_and_full_send(void)
{
    setup(INPUT_KEYBOARD);
    action(APP_GRID, 0);
    action(APP_GRID, 0);
    action(APP_CASE, 0);
    action(APP_GRID, 0);
    action(APP_GRID, 1);
    action(APP_SPACE, 0);
    assert(strcmp(s_interaction.view.preview, "Ab ") == 0);
    action(APP_DELETE, 0);
    assert(strcmp(s_interaction.view.preview, "Ab") == 0);
    action(APP_CLEAR, 0);
    assert(s_interaction.view.preview_length == 0);
    action(APP_CASE, 0);
    for (unsigned i = 0; i < 64; ++i) {
        action(APP_GRID, 0);
        action(APP_GRID, (int)(i % 2));
    }
    assert(s_interaction.view.preview_length == 64);
    action(APP_SPACE, 0);
    assert(s_interaction.view.preview_length == 64);
    expect_idle(); /* Selection/editing has emitted no HID reports. */
    assert(trace_count == 0);
    action(APP_SEND, 0);
    assert(s_send_token && s_engine.text_pending == 32);
    action(APP_CLEAR, 0);
    assert(s_interaction.view.preview_length == 64);
    for (size_t i = 0; i < 128; ++i) {
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        sync_interaction();
        const report_t *report = &trace[i];
        assert(report->id == 1 && report->length == 8);
        const uint8_t expected[8] = {
            i % 2 == 0 ? 2 : 0, 0,
            i % 2 == 0 ? (uint8_t)(4 + (i / 2) % 2) : 0, 0, 0, 0, 0, 0
        };
        assert(memcmp(report->bytes, expected, sizeof expected) == 0);
        if (i == 63) assert(s_engine.text_pending == 32); /* Second batch admitted. */
    }
    assert(!s_send_token && !s_interaction.view.sending);
    assert(s_interaction.view.preview_length == 0 && s_interaction.view.send_accepted == 64);
    assert(trace_count == 128);
    expect_idle(); /* No implicit Enter or replay. */
}

static void fill_preview(size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        action(APP_GRID, 0);
        action(APP_GRID, (int)(i % 2));
    }
}

static void interrupted_send_retains_only_unaccepted_suffix(void)
{
    const size_t steps[] = {0, 1, 2, 3, 63, 64, 65};
    for (size_t scenario = 0; scenario < sizeof steps / sizeof steps[0]; ++scenario) {
        setup(INPUT_KEYBOARD);
        fill_preview(64);
        action(APP_SEND, 0);
        for (size_t i = 0; i < steps[scenario]; ++i) {
            assert(step() == HID_ENGINE_STEP_SENT);
            update_send(false);
        }
        const size_t accepted = (steps[scenario] + 1) / 2;
        action(APP_STOP, 0);
        assert(!s_send_token && !s_interaction.view.sending);
        assert(s_interaction.view.preview_length == 64 - accepted);
        for (size_t i = 0; i < 64 - accepted; ++i)
            assert(s_interaction.view.preview[i] == ((i + accepted) % 2 ? 'B' : 'A'));
        release_barrier();
        sync_interaction();
        expect_idle();
        assert(trace_count == steps[scenario] + 2);
    }
    setup(INPUT_KEYBOARD);
    fill_preview(4);
    action(APP_SEND, 0);
    assert(step() == HID_ENGINE_STEP_SENT); /* A accepted; failed key-up cannot restore A. */
    update_send(false);
    test_send_failure = true;
    assert(step() == HID_ENGINE_STEP_SEND_FAILED);
    update_send(false);
    assert(!s_send_token && strcmp(s_interaction.view.preview, "BAB") == 0);
    test_send_failure = false;
    release_barrier();
    sync_interaction();
    expect_idle();
}

static void touch_precision_and_drag_boundaries(void)
{
    for (int divisor = 4; divisor <= 8; divisor *= 2) {
        setup(INPUT_MOUSE);
        action(APP_GAIN, divisor);
        touch(APP_TOUCH_BEGIN, 7, 100, 100);
        for (int i = 1; i <= divisor; ++i) {
            touch(APP_TOUCH_MOVE, 7, 1, -1);
            if (i < divisor) expect_idle();
        }
        const uint8_t delta[] = {0, 1, 0xff, 0};
        expect_report(2, delta, sizeof delta);
        touch(APP_TOUCH_MOVE, 99, 80, 80); /* Wrong gesture must not move. */
        expect_idle();
    }
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        setup(INPUT_MOUSE);
        action(APP_DRAG, 0);
        const uint8_t held[] = {1, 0, 0, 0};
        expect_report(2, held, sizeof held);
        touch(APP_TOUCH_BEGIN, 11, 10, 10);
        touch(APP_TOUCH_MOVE, 11, 3, -2);
        const uint8_t moved[] = {1, 3, 0xfe, 0};
        expect_report(2, moved, sizeof moved);
        touch(APP_TOUCH_END, 11, 13, 8);
        if (boundary == 0) action(APP_MENU, APP_OVERLAY_SETTINGS);
        else if (boundary == 1) action(APP_MODE, INPUT_KEYBOARD);
        else if (boundary == 2) {
            /* Invoke production cancellation and link setters at the real
             * transport boundary; full polling-loop scheduling is separate. */
            cancel_input();
            test_link.connection_admitted = test_link.connected = false;
            test_link.ready = false;
            ++test_link.generation;
            hid_engine_set_link(&s_engine, false, false);
            sync_interaction();
            assert(step() == HID_ENGINE_STEP_BLOCKED);
            test_link.connection_admitted = test_link.connected = test_link.ready = true;
            hid_engine_set_link(&s_engine, true, true);
        } else action(APP_DRAG, 0);
        assert(!s_drag_requested && s_engine.state.release_pending);
        release_barrier();
        sync_interaction();
        expect_idle();
        assert(trace_count == 4);
    }
}

static void raw_button_timing_and_takeover(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    buttons_at(start + 299);
    assert(!s_engine.state.gyro_enabled);
    buttons_at(start + 300);
    assert(s_engine.state.gyro_enabled);
    buttons_at(start + 500);
    assert(s_engine.state.gyro_enabled); /* Exactly one single-click. */

    setup(INPUT_MOUSE);
    calibrate();
    uint64_t time = s_now_ms + 10;
    button_edge(true, time);
    button_edge(false, time + 20);
    button_edge(true, time + 200);
    button_edge(false, time + 220);
    assert(s_overlay == APP_OVERLAY_MODES && !s_engine.state.gyro_enabled);
    release_barrier();
    buttons_at(time + 600);
    assert(!s_engine.state.gyro_enabled && s_overlay == APP_OVERLAY_MODES);

    setup(INPUT_MOUSE);
    time = s_now_ms + 10;
    button_edge(true, time);
    buttons_at(time + 899);
    assert(!s_engine.state.locked);
    buttons_at(time + 900);
    assert(s_engine.state.locked);
    release_barrier();
    buttons_at(time + 2000);
    assert(s_engine.state.locked); /* Holding cannot repeatedly toggle. */
    button_edge(false, time + 2010);
    buttons_at(time + 2300);
    assert(s_engine.state.locked);
    button_edge(true, time + 2500);
    buttons_at(time + 3400);
    assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
    release_barrier();

    setup(INPUT_MOUSE);
    calibrate();
    assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
    release_barrier();
    sync_interaction();
    time = s_now_ms + 10;
    button_edge(true, time);
    assert(s_engine.state.owner == INPUT_LOCAL);
    release_barrier();
    button_edge(false, time + 20);
    buttons_at(time + 300);
    assert(!s_engine.state.gyro_enabled && s_overlay == APP_OVERLAY_NONE);
    expect_idle();
}

static void motion_gates_and_local_keyboard_focus(void)
{
    setup(INPUT_MOUSE);
    sample_after(10, 0, 0);
    assert(s_interaction.view.calibrating && s_interaction.view.calibration_samples == 1);
    const interaction_t calibrating = s_interaction;
    const hid_engine_t engine = s_engine;
    const uint64_t accepted_timestamp = s_last_imu_us;
    /* A discarded late board batch can return the same still-fresh cache.
     * Repeated reads must not count it again or advance any motion state. */
    for (unsigned i = 0; i < 4; ++i) {
        s_now_ms += 10;
        process_motion();
        assert(s_last_imu_us == accepted_timestamp);
        assert(memcmp(&s_interaction, &calibrating, sizeof calibrating) == 0);
        assert(memcmp(&s_engine, &engine, sizeof engine) == 0);
        expect_idle();
    }
    setup(INPUT_MOUSE);
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    assert(!s_engine.state.gyro_enabled); /* Uncalibrated. */
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    assert(s_engine.state.gyro_enabled);
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
    process_motion(); /* Repeated board snapshot cannot duplicate movement. */
    expect_idle();
    test_haptic_active = true;
    sample_after(10, 100, 0);
    expect_idle();
    test_haptic_active = false;
    sample_after(10, 100, 0);
    expect_idle();
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 100, 0); expect_idle(); }
    sample_after(10, 0, 0);
    expect_idle(); /* Suppressed movement was discarded, not banked. */
    sample_after(10, 25, 0);
    expect_report(2, moved, sizeof moved);
    sample_after(100, 200, 0);
    expect_idle(); /* Long sampling gap produces no catch-up burst. */
    sample_after(10, 0, 0);
    expect_idle();
    sample_after(10, NAN, 0);
    expect_idle();
    sample_after(10, 25, 0);
    expect_idle(); /* First valid sample reestablishes time. */
    assert(!(s_interaction.view.motion_status & INTERACTION_MOTION_GRAVITY_READY));
    for (unsigned i = 0; i < 12; ++i) { sample_after(10, 0, 0); expect_idle(); }
    assert(s_interaction.view.motion_status & INTERACTION_MOTION_GRAVITY_READY);
    sample_after(10, 25, 0);
    expect_report(2, moved, sizeof moved);
    test_capabilities.imu = false;
    test_imu_result = ESP_FAIL;
    process_motion();
    assert(!s_engine.state.gyro_enabled && !s_interaction.view.motion_enabled);

    setup(INPUT_KEYBOARD);
    calibrate();
    const unsigned focus = s_interaction.view.focus;
    for (unsigned i = 0; i < 20; ++i) { sample_after(10, 50, 0); expect_idle(); }
    assert(s_interaction.view.focus == focus && s_interaction.view.preview_length == 0);
    action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_MOTION);
    release_barrier(); sync_interaction();
    trace_count = 0;
    for (unsigned i = 0; i < 20; ++i) { sample_after(10, 50, 0); expect_idle(); }
    assert(s_interaction.view.focus != 0 && s_interaction.view.preview_length == 0);
    assert(trace_count == 0); /* Selection motion never reaches host or confirms. */
    action(APP_MODE, INPUT_AUDIO);
    release_barrier();
    sync_interaction();
    for (unsigned i = 0; i < 20; ++i) { sample_after(10, 50, 0); expect_idle(); }
    assert(!s_engine.state.gyro_enabled && s_interaction.view.preview_length == 0);
}

static void fresh_sensor_after_consumer_gap_requires_new_reference(void)
{
    setup(INPUT_MOUSE);
    remote_control_init(false);
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    const uint8_t moved[] = {0, 2, 0, 0};
    /* Yaw about gravity is a physically coherent constant-1g trajectory. */
    for (unsigned i = 0; i < 200; ++i) {
        sample_after(10, 25, 0);
        const uint8_t fractional[] = {0, (uint8_t)(i % 2 ? 3 : 2), 0, 0};
        expect_report(2, fractional, sizeof fractional);
    }
    /* The board still supplies a new fresh sample; only the consumer missed
     * an interval. A healthy sampler does not repair the missing history. */
    sample_after(100, 25, 0);
    expect_idle();
    assert(s_interaction.view.calibrated && !s_interaction.view.calibration_samples);
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled && !s_interaction.gravity_ready);
    for (unsigned i = 0; i < 200; ++i) {
        sample_after(10, 25, 0);
        expect_idle();
    }
    assert(s_interaction.view.motion_block_reason == INTERACTION_MOTION_BLOCK_REFERENCE);
    for (unsigned i = 0; i < 15; ++i) { sample_after(10, 0, 0); expect_idle(); }
    assert(s_interaction.gravity_ready);
    sample_after(10, 25, 0);
    expect_report(2, moved, sizeof moved); /* No stale catch-up movement. */
}

static void rendered_state_is_truthful(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    render_view(&test_link, ESP_OK);
    assert(test_publish_count == 1);
    assert(strcmp(test_view.preview, "AB") == 0 && test_view.microphone_muted);
    assert(!test_view.audio_available && !test_view.audio_active &&
           !test_view.recognition_available && !test_view.listening);
    assert(test_view.keyboard_cell_count == 9 && !test_view.motion_available);
    test_link.peer_known = true;
    const uint8_t peer[] = {2, 3, 4, 5, 6, 7};
    memcpy(test_link.peer_address, peer, sizeof peer);
    render_view(&test_link, ESP_OK);
    assert(strcmp(test_target, "Host 02:03:04:05:06:07") == 0);
    test_link.connection_admitted = test_link.connected = false;
    test_link.ready = false;
    hid_engine_set_link(&s_engine, false, false);
    sync_interaction();
    render_view(&test_link, ESP_FAIL);
    assert(!test_view.connected && !test_view.ready && test_view.transport_error);
    assert(strcmp(test_target, "No Bluetooth host") == 0);
    test_presenter_available = false;
    render_view(&test_link, ESP_OK);
    assert(test_publish_count == 3);
}

static void motion_recovery_requires_fresh_samples_and_explicit_enable(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    assert(s_engine.state.gyro_enabled);
    const uint64_t accepted_timestamp = s_last_imu_us;
    test_capabilities.imu = false;
    test_imu_result = ESP_ERR_TIMEOUT;
    for (unsigned i = 0; i < 10; ++i) {
        s_now_ms += 10;
        process_motion();
        assert(s_last_imu_us == accepted_timestamp);
        assert(!s_engine.state.gyro_enabled && !s_interaction.view.motion_enabled);
        expect_idle();
    }
    render_view(&test_link, ESP_OK);
    assert(!test_view.motion_available && test_view.motion_reason == APP_MOTION_SENSOR_UNAVAILABLE);

    /* A fresh board sample restores availability, never the user's old ON
     * request. The gap invalidates the orientation reference without banking
     * missing movement; subsequent fresh samples reestablish that reference. */
    test_capabilities.imu = true;
    sample_after(10, 200, 0);
    expect_idle();
    for (unsigned i = 0; i < 20; ++i) {
        sample_after(10, 0, 0);
        expect_idle();
    }
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_available && test_view.motion_reason == APP_MOTION_READY);
    assert(!test_view.motion_enabled && !s_interaction.view.motion_enabled);
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
    process_motion();
    expect_idle();

    /* A gap during startup calibration cannot combine disjoint sample
     * windows into a successful calibration or count repeated cached data. */
    setup(INPUT_MOUSE);
    for (unsigned i = 0; i < 20; ++i) sample_after(10, 0, 0);
    assert(s_interaction.view.calibrating && s_interaction.view.calibration_samples == 20);
    test_capabilities.imu = false;
    test_imu_result = ESP_ERR_TIMEOUT;
    s_now_ms += 100;
    process_motion();
    test_capabilities.imu = true;
    sample_after(10, 0, 0);
    assert(s_interaction.view.calibration_samples == 0 && !s_interaction.view.calibrated);
    process_motion();
    assert(s_interaction.view.calibration_samples == 0);
    calibrate();
    assert(!s_engine.state.gyro_enabled);
    expect_idle();
}

static void touch_lifecycle_rejects_ended_gesture(void)
{
    setup(INPUT_MOUSE);
    /* This case exercises fine movement in the selectable movement-only mode. */
    s_settings.tap_to_click = false;
    interaction_touch_config_t config = s_interaction.touch_config;
    config.tap_to_click = false;
    assert(interaction_touch_configure(&s_interaction, &config));
    action(APP_GAIN, 4);
    touch(APP_TOUCH_BEGIN, 12, 0, 0);
    touch(APP_TOUCH_MOVE, 12, 3, 0);
    expect_idle();
    touch(APP_TOUCH_END, 12, 3, 0);
    touch(APP_TOUCH_MOVE, 12, 1, 0);
    expect_idle(); /* An ended gesture cannot emit during the gyro release freeze. */
    touch(APP_TOUCH_BEGIN, 13, 20, 20);
    touch(APP_TOUCH_MOVE, 13, 1, 0);
    expect_idle(); /* A new finger contact cannot inherit fractional movement. */
}

static void touch_release_has_one_freeze_interval(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    app_intent_t control = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
    control.gesture = 17;
    control.surface = APP_SURFACE_CONTROL;
    deliver(control);
    sample_after(10, 25, 0);
    expect_idle();
    control.action = APP_TOUCH_END;
    deliver(control);
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 25, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved); /* One 150 ms interval, not two stacked freezes. */
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
}

static void physical_down_freezes_before_classification(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    /* Snapshot gate acts immediately, even before queued down is classified. */
    physical_button(BOARD_BUTTON_DOWN, (s_now_ms + 10) * 1000U);
    sample_after(10, 25, 0);
    expect_idle();
    process_buttons();
    button_edge(false, s_now_ms + 10);
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 25, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
}

static void same_or_invalid_mode_preserves_active_send(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(4);
    action(APP_SEND, 0);
    assert(step() == HID_ENGINE_STEP_SENT);
    update_send(false);
    const uint64_t token = s_send_token;
    const uint64_t epoch = s_engine.state.epoch;
    assert(token && strcmp(s_interaction.view.preview, "BAB") == 0);
    action(APP_MODE, INPUT_KEYBOARD);
    action(APP_MODE, 999);
    assert(s_send_token == token && s_engine.state.epoch == epoch);
    assert(s_interaction.view.sending && s_engine.text_pending == 4);
    for (unsigned i = 0; i < 7; ++i) {
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        sync_interaction();
    }
    assert(!s_send_token && !s_interaction.view.sending);
    assert(s_interaction.view.send_accepted == 4 && s_interaction.view.preview_length == 0);
    assert(trace_count == 8);
    expect_idle();
}

static void short_congestion_preserves_preview_send(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(64);
    action(APP_SEND, 0);
    assert(step() == HID_ENGINE_STEP_SENT);
    update_send(false);
    const uint64_t token = s_send_token;
    test_link.congested = true;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, true));
    sync_interaction();
    update_send(false);
    assert(s_send_token == token && s_interaction.view.sending);
    assert(strcmp(s_interaction.view.preview + 61, "AB") == 0);
    assert(s_interaction.view.preview_length == 63);
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 1);
    s_now_ms += 20;
    test_link.congested = false;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, false));
    sync_interaction();
    for (unsigned i = 0; i < 127; ++i) {
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        sync_interaction();
    }
    assert(!s_send_token && s_interaction.view.preview_length == 0);
    assert(s_interaction.view.send_accepted == 64 && trace_count == 128);
    expect_idle();
}

static void short_congestion_preserves_motion_intent(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    test_link.congested = true;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, true));
    sync_interaction();
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
    sample_after(10, 100, 0);
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 0);
    test_link.congested = false;
    assert(!hid_engine_observe_transport(&s_engine, s_now_ms, false));
    sync_interaction();
    assert(s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
}

static void modal_button_press_is_not_retargeted_after_close(void)
{
    setup(INPUT_KEYBOARD);
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    release_barrier();
    sync_interaction();
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    buttons_at(start + 100);
    action(APP_CLOSE, 0);
    assert(s_interaction.view.group == -1);
    buttons_at(start + 300);
    assert(s_interaction.view.group == -1 && s_interaction.view.preview_length == 0);
    expect_idle();
}

static void local_touch_takeover_consumes_its_gesture(void)
{
    setup(INPUT_MOUSE);
    assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
    release_barrier();
    sync_interaction();
    touch(APP_TOUCH_BEGIN, 31, 0, 0);
    assert(s_engine.state.owner == INPUT_LOCAL && s_engine.state.release_pending);
    release_barrier();
    sync_interaction();
    app_intent_t click = make_intent(APP_CLICK, 1, 0, NULL);
    click.gesture = 31;
    deliver(click);
    touch(APP_TOUCH_MOVE, 31, 40, 30);
    expect_idle();
    touch(APP_TOUCH_END, 31, 0, 0);
    touch(APP_TOUCH_BEGIN, 32, 0, 0);
    click = make_intent(APP_CLICK, 1, 0, NULL);
    click.gesture = 32;
    deliver(click);
    const uint8_t down[] = {1, 0, 0, 0}, up[] = {0, 0, 0, 0};
    expect_report(2, down, sizeof down);
    expect_report(2, up, sizeof up);
    expect_idle();
}

static void preview_survives_mode_modal_and_link_cancellation(void)
{
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        setup(INPUT_KEYBOARD);
        fill_preview(4);
        action(APP_SEND, 0);
        assert(step() == HID_ENGINE_STEP_SENT);
        update_send(false);
        if (boundary == 0) action(APP_MODE, INPUT_MOUSE);
        else if (boundary == 1) action(APP_MENU, APP_OVERLAY_SETTINGS);
        else {
            cancel_input();
            test_link.ready = test_link.connection_admitted = test_link.connected = false;
            ++test_link.generation;
            hid_engine_set_link(&s_engine, false, false);
            sync_interaction();
            update_send(false);
            assert(step() == HID_ENGINE_STEP_BLOCKED);
            test_link.ready = test_link.connection_admitted = test_link.connected = true;
            hid_engine_set_link(&s_engine, true, true);
        }
        assert(!s_send_token && !s_interaction.view.sending);
        assert(strcmp(s_interaction.view.preview, "BAB") == 0);
        release_barrier();
        sync_interaction();
        expect_idle();
        assert(trace_count == 3); /* A down and two zero releases, with no suffix replay. */
    }
}

static void physical_confirm_uses_visible_keyboard_submode(void)
{
    for (int submode = APP_KEYBOARD_MOTION; submode <= APP_KEYBOARD_SHORTCUTS; ++submode) {
        setup(INPUT_KEYBOARD);
        action(APP_KEYBOARD_SUBMODE, submode);
        release_barrier();
        sync_interaction();
        uint64_t start = s_now_ms + 10;
        button_edge(true, start);
        button_edge(false, start + 20);
        buttons_at(start + 300);
        const bool selects_grid = submode == APP_KEYBOARD_MOTION || submode == APP_KEYBOARD_TOUCH;
        assert(s_interaction.view.group == (selects_grid ? 4 : -1));
        assert(s_interaction.view.preview_length == 0);
        start = s_now_ms + 10;
        button_edge(true, start);
        button_edge(false, start + 20);
        buttons_at(start + 300);
        assert(strcmp(s_interaction.view.preview, selects_grid ? "R" : "") == 0);
        expect_idle();
        assert(trace_count == 2); /* Only the submode-change release barrier. */
    }
}

static void shortcuts_use_selected_host_modifier_and_preserve_preview(void)
{
    for (int host = APP_HOST_CTRL; host <= APP_HOST_COMMAND; ++host) {
        setup(INPUT_KEYBOARD);
        fill_preview(2);
        action(APP_MENU, APP_OVERLAY_SETTINGS);
        action(APP_SETTINGS_PAGE, APP_SETTINGS_INPUT);
        action(APP_HOST_OS, host);
        release_barrier();
        action(APP_CLOSE, 0);
        sync_interaction();
        action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_SHORTCUTS);
        release_barrier();
        sync_interaction();
        trace_count = 0;
        action(APP_SHORTCUT, APP_SHORTCUT_COPY);
        const uint8_t key[] = {host == APP_HOST_COMMAND ? 8 : 1, 0, 6, 0, 0, 0, 0, 0};
        static const uint8_t zero[8] = {0};
        expect_report(1, key, sizeof key);
        expect_report(1, zero, sizeof zero);
        assert(strcmp(s_interaction.view.preview, "AB") == 0 && !s_send_token);
        expect_idle();
    }
}

static void lost_button_edges_cannot_unlock(void)
{
    for (unsigned final_down = 0; final_down < 2; ++final_down) {
        setup(INPUT_MOUSE);
        button_edge(true, 1010);
        for (unsigned i = 0; i < 8; ++i)
            physical_button(BOARD_BUTTON_DOWN, (1020U + i) * 1000U);
        physical_button(final_down ? BOARD_BUTTON_DOWN : BOARD_BUTTON_UP, 1030000);
        buttons_at(1040);
        assert(s_engine.state.locked && s_button_count == 0);
        release_barrier();
        buttons_at(3000);
        assert(s_engine.state.locked); /* The lost UP must never turn into a hold/unlock. */
        button_edge(false, 3010);
        buttons_at(3300);
        assert(s_engine.state.locked);
        button_edge(true, 3400);
        buttons_at(4299);
        assert(s_engine.state.locked);
        buttons_at(4300);
        assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
        release_barrier();
        expect_idle();
    }
}

static void theme_changes_are_settings_scoped(void)
{
    setup(INPUT_MOUSE);
    action(APP_THEME, 0);
    assert(s_dark_theme); /* Hidden settings controls cannot change state. */
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    action(APP_THEME, 0);
    assert(s_dark_theme); /* The Settings home is not the Display page. */
    action(APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
    release_barrier();
    sync_interaction();
    app_intent_t old_theme = make_intent(APP_THEME, 1, 0, NULL);
    action(APP_THEME, 0);
    assert(!s_dark_theme && s_engine.state.mode == INPUT_MOUSE);
    render_view(&test_link, ESP_OK);
    assert(!test_view.dark_theme && test_view.overlay == APP_OVERLAY_SETTINGS);
    deliver(old_theme);
    assert(!s_dark_theme); /* A change invalidates the previously rendered context. */
    action(APP_THEME, 2);
    assert(!s_dark_theme);
    action(APP_CLOSE, 0);
    action(APP_MENU, APP_OVERLAY_MODES);
    release_barrier();
    sync_interaction();
    action(APP_THEME, 1);
    assert(!s_dark_theme && s_overlay == APP_OVERLAY_MODES);
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock) && process_priority());
    release_barrier();
    action(APP_THEME, 1);
    assert(!s_dark_theme && s_engine.state.locked);
}

static void queued_double_click_then_hold_keeps_edge_time(void)
{
    setup(INPUT_MOUSE);
    physical_button(BOARD_BUTTON_DOWN, 1010000);
    physical_button(BOARD_BUTTON_UP, 1030000);
    physical_button(BOARD_BUTTON_DOWN, 1100000);
    physical_button(BOARD_BUTTON_UP, 1120000);
    physical_button(BOARD_BUTTON_DOWN, 1200000);
    buttons_at(2101);
    assert(s_now_ms == 2101 && s_engine.state.locked);
    assert(s_overlay == APP_OVERLAY_NONE && !s_engine.state.gyro_enabled);
    release_barrier();
    button_edge(false, 2110);
    buttons_at(2500);
    assert(s_engine.state.locked);
    button_edge(true, 2600);
    buttons_at(3500);
    assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
    release_barrier();
    expect_idle();
}

static void completed_quick_touch_still_freezes_motion(void)
{
    setup(INPUT_MOUSE);
    calibrate();
    action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
    begin.gesture = 41;
    app_intent_t end = begin;
    end.action = APP_TOUCH_END;
    s_now_ms += 10;
    assert(submit_intent(&begin));
    s_now_ms += 10;
    assert(submit_intent(&end));
    consume_one();
    consume_one(); /* Both callbacks happened before this coordinator poll. */
    for (unsigned i = 0; i < 14; ++i) { sample_after(10, 25, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
}

static void settings_page(app_settings_page_t page)
{
    action(APP_SETTINGS_PAGE, (int)page);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == page);
    release_barrier();
    sync_interaction();
}

static void display_at(uint64_t milliseconds)
{
    s_now_ms = milliseconds;
    service_display();
    sync_interaction();
}

static void brightness_applied(uint8_t percent)
{
    test_brightness = (board_brightness_status_t){
        .applied_known = true, .applied_percent = percent,
        .applied_at_us = s_now_ms * 1000U
    };
    service_display();
}

static void settings_pages_validate_and_persist_changes(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_HOME);
    const app_action_t hidden[] = {APP_THEME, APP_BRIGHTNESS, APP_IDLE_TIMEOUT,
        APP_HAPTICS, APP_HOST_OS, APP_CALIBRATE, APP_WIFI_SETUP, APP_PAIRING_REQUEST};
    for (size_t i = 0; i < sizeof hidden / sizeof hidden[0]; ++i) action(hidden[i], 0);
    assert(test_storage_requests == 0 && test_wifi_starts == 0 && test_pair_opens == 0);
    settings_page(APP_SETTINGS_DISPLAY);
    const uint64_t revision = s_ui_revision;
    action(APP_THEME, 1);
    action(APP_BRIGHTNESS, 50);
    action(APP_IDLE_TIMEOUT, 300);
    assert(test_storage_requests == 0 && s_ui_revision == revision);
    action(APP_BRIGHTNESS, 0);
    action(APP_BRIGHTNESS, 75);
    action(APP_BRIGHTNESS, 110);
    action(APP_IDLE_TIMEOUT, 120);
    action(APP_IDLE_TIMEOUT, -60);
    action(APP_IDLE_TIMEOUT, 61);
    assert(test_storage_requests == 0 && s_settings.brightness_percent == 50);
    app_intent_t stale = make_intent(APP_BRIGHTNESS, 20, 0, NULL);
    action(APP_BRIGHTNESS, 30);
    deliver(stale);
    assert(s_settings.brightness_percent == 30 && test_storage_requests == 1);
    assert(test_saved_request.brightness_percent == 30);
    action(APP_THEME, 0);
    assert(!s_dark_theme && test_storage_requests == 2);
    settings_page(APP_SETTINGS_INPUT);
    action(APP_THEME, 1);
    action(APP_HAPTICS, 0);
    assert(!s_dark_theme && !test_haptics_enabled && !s_settings.haptics_enabled);
    action(APP_HOST_OS, APP_HOST_COMMAND);
    release_barrier();
    assert(s_host_os == APP_HOST_COMMAND && test_storage_requests == 4);
    test_storage_error = ESP_FAIL;
    action(APP_HAPTICS, 1);
    assert(test_storage_requests == 5 && s_settings.haptics_enabled && test_haptics_enabled);
    assert(test_saved_request.host_modifier == DEVICE_HOST_COMMAND);
    settings_page(APP_SETTINGS_DISPLAY);
    test_capabilities.brightness = false;
    action(APP_BRIGHTNESS, 100);
    action(APP_IDLE_TIMEOUT, 0);
    assert(s_settings.brightness_percent == 30 && s_settings.idle_timeout == DEVICE_IDLE_5_MIN);
    assert(test_storage_requests == 5);
}

static void idle_deadlines_ignore_passive_status_updates(void)
{
    const int seconds[] = {60, 300, 600, 1800, 0};
    for (size_t choice = 0; choice < sizeof seconds / sizeof seconds[0]; ++choice) {
        setup(INPUT_MOUSE);
        assert(s_settings.idle_timeout == DEVICE_IDLE_5_MIN);
        settings_page(APP_SETTINGS_DISPLAY);
        const int brightness = choice % 2U ? 70 : 50;
        if (brightness == 70) action(APP_BRIGHTNESS, brightness); /* Explicit choice overrides the fallback. */
        action(APP_IDLE_TIMEOUT, seconds[choice]);
        action(APP_CLOSE, 0);
        service_display();
        assert(test_brightness_count == 1 && test_brightness_requests[0] == brightness);
        brightness_applied((uint8_t)brightness);
        const uint64_t epoch = s_engine.state.epoch;
        const unsigned writes = test_storage_requests;
        const uint64_t deadline = 1000U + (uint64_t)seconds[choice] * 1000U;
        const uint64_t before = seconds[choice] ? deadline - 1 : UINT64_C(5000000000);
        for (uint64_t now = 1000; now < 2000; now += 10) {
            display_at(now);
            test_wifi.revision++;
            test_link.rssi_dbm = -60;
            render_view(&test_link, ESP_OK);
        }
        display_at(before);
        assert(!s_idle.asleep && test_brightness_count == 1);
        display_at(seconds[choice] ? deadline : before + 1000000U);
        assert(s_idle.asleep == (seconds[choice] != 0));
        if (seconds[choice]) {
            assert(test_brightness_count == 2 && test_brightness_requests[1] == 0);
            brightness_applied(0);
            assert(!s_screen_accepts_input);
        }
        for (unsigned i = 0; i < 1000; ++i) {
            ++s_now_ms;
            service_display();
            render_view(&test_link, ESP_OK);
        }
        assert(test_brightness_count == (seconds[choice] ? 2U : 1U));
        assert(test_storage_requests == writes && s_engine.state.epoch == epoch);
        assert(test_disconnects == 0 && test_pair_opens == 0 && test_wifi_starts == 0);
        expect_idle();
    }
}

static void display_sleep_preserves_hid_work_and_draft(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(4);
    action(APP_SEND, 0);
    const uint64_t token = s_send_token, epoch = s_engine.state.epoch;
    assert(token);
    display_at(301000);
    assert(s_idle.asleep && s_send_token == token && s_engine.state.epoch == epoch);
    static const uint8_t zero[8] = {0};
    const uint8_t keys[] = {4, 5, 4, 5};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        const uint8_t key = keys[i];
        const uint8_t down[] = {2, 0, key, 0, 0, 0, 0, 0};
        expect_report(1, down, sizeof down);
        update_send(false);
        expect_report(1, zero, sizeof zero);
        update_send(false);
    }
    assert(!s_send_token && s_interaction.view.preview_length == 0 && s_idle.asleep);
    assert(test_storage_requests == 0 && test_disconnects == 0);
    expect_idle();

    setup(INPUT_MOUSE);
    action(APP_DRAG, 1);
    const uint8_t held[] = {1, 0, 0, 0};
    expect_report(2, held, sizeof held);
    display_at(301000);
    assert(s_idle.asleep && s_drag_requested);
    const size_t count = trace_count;
    expect_idle();
    assert(trace_count == count);
    /* A physical hold still requires a separate wake gesture before locking. */
    button_edge(true, 301010);
    buttons_at(302000);
    assert(!s_engine.state.locked && s_drag_requested);
    button_edge(false, 302010);
    brightness_applied(50);
    buttons_at(302290);
    button_edge(true, 302300);
    buttons_at(303200);
    assert(s_engine.state.locked && !s_drag_requested);
    release_barrier();
}

static void touch_wake_consumes_entire_gesture_until_driver_ack(void)
{
    for (unsigned locked = 0; locked < 2; ++locked) {
        setup(INPUT_KEYBOARD);
        fill_preview(2);
        if (locked) {
            physical_lock();
            assert(process_priority());
            release_barrier();
        } else {
            assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
            release_barrier();
            sync_interaction();
        }
        display_at(301000);
        brightness_applied(0);
        trace_count = 0;
        touch(APP_TOUCH_BEGIN, 501, 0, 0);
        assert(!s_idle.asleep && s_idle.wake_capture);
        service_display();
        assert(test_brightness_requests[test_brightness_count - 1] == 50);
        touch(APP_TOUCH_MOVE, 501, 20, 30);
        touch(APP_TOUCH_END, 501, 0, 0);
        touch(APP_KEY, 501, 4, 0);
        assert(strcmp(s_interaction.view.preview, "AB") == 0);
        assert(s_engine.state.locked == (locked != 0));
        assert(s_engine.state.owner == (locked ? INPUT_LOCAL : INPUT_REMOTE));
        expect_idle();
        /* Fresh contacts before illumination is acknowledged are also consumed. */
        touch(APP_TOUCH_BEGIN, 502, 0, 0);
        brightness_applied(50);
        touch(APP_TOUCH_END, 502, 0, 0);
        touch(APP_KEY, 502, 5, 0);
        expect_idle();
        touch(APP_TOUCH_BEGIN, 503, 0, 0);
        touch(APP_TOUCH_END, 503, 0, 0);
        touch(APP_KEY, 503, 6, 0);
        if (!locked) {
            assert(s_engine.state.owner == INPUT_LOCAL);
            release_barrier(); /* A later fresh gesture takes over, and is consumed too. */
        }
        expect_idle();
        assert(test_storage_requests == 0 && strcmp(s_interaction.view.preview, "AB") == 0);
    }
}

static void ai_wake_double_and_hold_are_consumed(void)
{
    setup(INPUT_MOUSE);
    physical_lock();
    assert(process_priority());
    release_barrier();
    display_at(301000);
    brightness_applied(0);
    button_edge(true, 301010);
    button_edge(false, 301030);
    brightness_applied(50);
    button_edge(true, 301100);
    buttons_at(302000);
    assert(s_engine.state.locked && s_overlay == APP_OVERLAY_NONE);
    button_edge(false, 302010);
    buttons_at(302290);
    display_at(302290);
    assert(s_engine.state.locked && !s_idle.wake_capture);
    button_edge(true, 302300);
    buttons_at(303199);
    assert(s_engine.state.locked);
    buttons_at(303200);
    assert(!s_engine.state.locked && !s_engine.state.gyro_enabled);
    release_barrier();
    assert(test_storage_requests == 0);
}

static void display_activity_clock_and_optional_driver(void)
{
    setup(INPUT_MOUSE);
    service_display();
    display_at(300999);
    /* Physical callbacks still renew idle if a safety queue reset drops their intents. */
    app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
    begin.gesture = 601;
    assert(submit_intent(&begin));
    app_intent_t end = begin;
    end.action = APP_TOUCH_END;
    assert(submit_intent(&end));
    xQueueReset(s_intents);
    display_at(301000);
    assert(!s_idle.asleep);
    display_at(600998);
    assert(!s_idle.asleep);
    display_at(600999);
    assert(s_idle.asleep);
    display_at(500000); /* Backward time wakes and rebases; no subtraction underflow. */
    assert(!s_idle.asleep);
    display_at(799999);
    assert(!s_idle.asleep);
    display_at(800000);
    assert(s_idle.asleep);

    setup(INPUT_MOUSE);
    test_capabilities.brightness = false;
    test_brightness = (board_brightness_status_t){.error = ESP_FAIL};
    display_at(UINT64_C(5000000000));
    assert(!s_idle.asleep && s_screen_accepts_input && test_brightness_count == 0);
    touch(APP_TOUCH_BEGIN, 701, 0, 0);
    touch(APP_TOUCH_END, 701, 0, 0);
    touch(APP_CLICK, 701, 1, 0);
    const uint8_t click[] = {1, 0, 0, 0};
    const uint8_t release[] = {0, 0, 0, 0};
    expect_report(2, click, sizeof click);
    expect_report(2, release, sizeof release);
}

static void timeout_changes_start_a_full_new_interval(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_DISPLAY);
    action(APP_IDLE_TIMEOUT, 1800);
    display_at(100000);
    action(APP_IDLE_TIMEOUT, 60);
    display_at(159999);
    assert(!s_idle.asleep);
    display_at(160000);
    assert(s_idle.asleep);
    brightness_applied(0);
    /* A hidden timeout request cannot change or wake a sleeping display. */
    action(APP_IDLE_TIMEOUT, 0);
    assert(s_idle.asleep && s_settings.idle_timeout == DEVICE_IDLE_1_MIN);
    touch(APP_TOUCH_BEGIN, 801, 0, 0);
    touch(APP_TOUCH_END, 801, 0, 0);
    brightness_applied(50);
    display_at(160010);
    action(APP_IDLE_TIMEOUT, 1800);
    assert(!s_idle.asleep);
    display_at(1960009);
    assert(!s_idle.asleep);
    display_at(1960010);
    assert(s_idle.asleep);
    touch(APP_TOUCH_BEGIN, 802, 0, 0);
    touch(APP_TOUCH_END, 802, 0, 0);
    brightness_applied(50);
    action(APP_IDLE_TIMEOUT, 0);
    display_at(UINT64_C(5000000000));
    assert(!s_idle.asleep && s_settings.idle_timeout == DEVICE_IDLE_OFF);
    action(APP_IDLE_TIMEOUT, 60);
    display_at(UINT64_C(5000059999));
    assert(!s_idle.asleep);
    display_at(UINT64_C(5000060000));
    assert(s_idle.asleep);
    assert(test_storage_requests == 5);
}

static void failed_wake_is_blocked_until_a_fresh_acknowledged_gesture(void)
{
    setup(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    test_brightness_error = ESP_FAIL;
    test_brightness.error = ESP_FAIL;
    touch(APP_TOUCH_BEGIN, 901, 0, 0);
    touch(APP_TOUCH_END, 901, 0, 0);
    service_display();
    const size_t attempts = test_brightness_count;
    for (unsigned i = 0; i < 1000; ++i) {
        ++s_now_ms;
        service_display();
    }
    assert(!s_screen_accepts_input && test_brightness_count == attempts);
    touch(APP_TOUCH_BEGIN, 902, 0, 0);
    touch(APP_TOUCH_END, 902, 0, 0);
    touch(APP_CLICK, 902, 1, 0);
    assert(test_brightness_count == attempts + 1 && !s_screen_accepts_input);
    expect_idle();
    test_brightness_error = ESP_OK;
    touch(APP_TOUCH_BEGIN, 903, 0, 0);
    assert(test_brightness_count == attempts + 2);
    brightness_applied(50);
    touch(APP_TOUCH_END, 903, 0, 0);
    touch(APP_CLICK, 903, 1, 0);
    expect_idle();
    touch(APP_TOUCH_BEGIN, 904, 0, 0);
    touch(APP_TOUCH_END, 904, 0, 0);
    touch(APP_CLICK, 904, 1, 0);
    const uint8_t click[] = {1, 0, 0, 0};
    const uint8_t release[] = {0, 0, 0, 0};
    expect_report(2, click, sizeof click);
    expect_report(2, release, sizeof release);
    assert(test_storage_requests == 0);
}

static void wifi_setup_lifecycle_and_secret_scope(void)
{
    setup(INPUT_MOUSE);
    action(APP_WIFI_SETUP, 1);
    assert(test_wifi_starts == 0);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SETUP, 1);
    assert(test_wifi_starts == 1 && s_overlay == APP_OVERLAY_SETTINGS);
    test_wifi_start_error = ESP_OK;
    action(APP_WIFI_SETUP, 1);
    assert(test_wifi_starts == 2 && s_overlay == APP_OVERLAY_WIFI_SETUP);
    release_barrier();
    render_view(&test_link, ESP_OK);
    assert(!test_view.wifi_setup_active && test_view.wifi_setup_passcode[0] == '\0');
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .setup_active = true,
        .state = APP_WIFI_IDLE, .setup_remaining_ms = 599001};
    test_wifi_info = (app_wifi_setup_info_t){.generation = 4,
        .ssid = "TEST-SETUP", .password = "synthetic-password", .ip = "192.0.2.1"};
    test_wifi_info_error = ESP_OK;
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_setup_active && strcmp(test_view.wifi_setup_passcode, "synthetic-password") == 0);
    assert(strcmp(test_view.wifi_setup_url, "http://192.0.2.1") == 0);
    assert(test_view.wifi_setup_seconds_left == 600);
    app_intent_t stale_start = make_intent(APP_WIFI_SETUP, 1, 0, NULL);
    action(APP_CLOSE, 0);
    assert(test_wifi_stops == 1 && s_overlay == APP_OVERLAY_NONE);
    const unsigned reads = test_wifi_info_reads;
    render_view(&test_link, ESP_OK);
    assert(test_wifi_info_reads == reads && test_view.wifi_setup_passcode[0] == '\0');
    assert(test_view.wifi_setup_ssid[0] == '\0' && test_view.wifi_setup_url[0] == '\0');
    deliver(stale_start);
    assert(test_wifi_starts == 2);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SETUP, 1); /* Existing AP opens its page without another start. */
    release_barrier();
    assert(test_wifi_starts == 2);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_HOME);
    release_barrier();
    assert(test_wifi_stops == 2);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SETUP, 1);
    release_barrier();
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    assert(submit_intent(&lock) && process_priority());
    release_barrier();
    assert(test_wifi_stops == 3 && s_engine.state.locked);
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_setup_passcode[0] == '\0');
    assert(test_storage_requests == 0 && test_pair_forgets == 0);
}

static void wifi_forget_requires_current_explicit_confirmation(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_FORGET_REQUEST, 0);
    assert(s_overlay == APP_OVERLAY_SETTINGS && test_wifi_forgets == 0);
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .configured = true,
        .ssid = "TEST-NETWORK-A", .state = APP_WIFI_IDLE};
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_WIFI_FORGET);
    app_intent_t old_confirm = make_intent(APP_CONFIRM, 1, 0, NULL);
    action(APP_THEME, 0);
    action(APP_KEY, 4);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
    assert(s_overlay == APP_OVERLAY_CONFIRM && test_storage_requests == 0);
    action(APP_CONFIRM, 0);
    release_barrier();
    deliver(old_confirm);
    assert(test_wifi_forgets == 0);
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    strcpy(test_wifi.ssid, "TEST-NETWORK-B");
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(test_wifi_forgets == 0);
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    test_wifi.operation_pending = true;
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(test_wifi_forgets == 0);
    test_wifi.operation_pending = false;
    action(APP_WIFI_FORGET_REQUEST, 0);
    release_barrier();
    test_wifi_forget_error = ESP_OK;
    app_intent_t confirm = make_intent(APP_CONFIRM, 1, 0, NULL);
    deliver(confirm);
    release_barrier();
    deliver(confirm);
    assert(test_wifi_forgets == 1 && test_storage_requests == 0 && test_pair_forgets == 0);
    assert(strcmp(s_interaction.view.preview, "AB") == 0);
    expect_idle();
}

static void pairing_confirmation_releases_before_disconnect_and_forget(void)
{
    const app_pairing_operation_t operations[] = {APP_PAIR_OPEN, APP_PAIR_FORGET_ALL};
    for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        setup(INPUT_KEYBOARD); fill_preview(2); press(INPUT_KEYBOARD);
        test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 51};
        test_update_target = (ble_hid_target_snapshot_t){.loaded = true, .durable = true, .revision = 61};
        action(APP_MENU, APP_OVERLAY_TARGET); release_barrier();
        action(APP_PAIRING_REQUEST, (int)operations[i]); release_barrier();
        assert(s_overlay == APP_OVERLAY_CONFIRM && !test_native_management_starts);
        app_intent_t approve = make_intent(APP_CONFIRM, 1, 0, NULL);
        deliver(approve);
        assert(test_native_management_starts == 1 && test_native_operation == operations[i]);
        assert(test_native_generation == test_link.generation && test_native_bond_revision == 51 && test_native_preference_revision == 61);
        assert(test_native_start_release_pending && !test_disconnects && !test_pair_opens && !test_pair_forgets);
        release_barrier();
        assert(s_overlay == (operations[i] == APP_PAIR_OPEN ? APP_OVERLAY_BLUETOOTH_PAIRING : APP_OVERLAY_SETTINGS));
        deliver(approve); assert(test_native_management_starts == 1);
        assert(!strcmp(s_interaction.view.preview, "AB"));
        if (operations[i] == APP_PAIR_OPEN) {
            action(APP_PAIRING_REQUEST, APP_PAIR_CLOSE);
            assert(test_native_management_stops == 1 && !test_native_management_active && s_overlay == APP_OVERLAY_BLUETOOTH_PAIRING);
        }
    }
    setup(INPUT_MOUSE); settings_page(APP_SETTINGS_BLUETOOTH);
    test_link.connection_admitted = test_link.connected = test_link.ready = false; hid_engine_set_link(&s_engine, false, false);
    test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 71};
    test_update_target = (ble_hid_target_snapshot_t){.loaded = true, .revision = 81};
    action(APP_PAIRING_REQUEST, APP_PAIR_OPEN);
    assert(s_overlay == APP_OVERLAY_BLUETOOTH_PAIRING && test_native_management_starts == 1 && test_native_generation == 0 && s_confirmation != APP_CONFIRM_PAIR_OPEN);
}

static void pairing_stale_generation_failure_and_lock_cancel(void)
{
    setup(INPUT_MOUSE);
    test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 51};
    test_update_target = (ble_hid_target_snapshot_t){.loaded = true, .revision = 61};
    action(APP_MENU, APP_OVERLAY_TARGET); release_barrier();
    action(APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL); release_barrier();
    ++test_link.generation; action(APP_CONFIRM, 1); release_barrier();
    assert(!test_native_management_starts && !test_pair_forgets);
    settings_page(APP_SETTINGS_BLUETOOTH); action(APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL); release_barrier();
    ++test_bonds.revision; action(APP_CONFIRM, 1); release_barrier();
    assert(test_native_management_starts == 1 && !test_native_management_active && test_native_bond_revision == 51);
    settings_page(APP_SETTINGS_BLUETOOTH); action(APP_PAIRING_REQUEST, APP_PAIR_OPEN); release_barrier();
    app_intent_t old = make_intent(APP_CONFIRM, 1, 0, NULL);
    physical_lock(); assert(process_priority()); deliver(old);
    assert(s_engine.state.locked && test_native_management_starts == 1 && !test_pair_opens && !test_pair_forgets);
}

static void extended_status_views_do_not_invent_evidence_or_writes(void)
{
    setup(INPUT_AUDIO);
    render_view(&test_link, ESP_OK);
    assert(!test_view.battery_percent_valid && test_view.battery_state == APP_BATTERY_UNKNOWN);
    assert(!test_view.wifi_rssi_valid);
    assert(test_view.wifi_state == APP_UI_WIFI_UNAVAILABLE && test_view.paired_host_count == -1);
    assert(test_view.paired_host_capacity == 0);
    assert(test_view.settings_save_state == APP_SAVE_DEFAULTS);
    assert(test_view.brightness_percent == 50 && test_view.idle_timeout_seconds == 300);
    assert(!test_view.audio_available && test_view.microphone_muted);
    test_power_error = ESP_OK;
    test_power = (board_power_status_t){.gauge_available = true,
        .battery_present = BOARD_TRUTH_YES, .percent_valid = true, .percent = 63,
        .charging = BOARD_TRUTH_YES, .current_valid = true, .current_ma = 120};
    test_link.rssi_valid = true;
    test_link.rssi_dbm = -82;
    test_link.bond_count_known = true;
    test_link.bond_count = 2;
    test_link.bond_capacity = 32;
    test_link.pairing_open = true;
    test_link.pairing_remaining_ms = 1001;
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .configured = true,
        .state = APP_WIFI_CONNECTED, .rssi_valid = true, .rssi_dbm = -41,
        .ssid = "TEST-NETWORK", .ip = "192.0.2.14"};
    test_storage.pending = true;
    test_storage.requested_revision = 2;
    test_storage.saved_revision = 1;
    render_view(&test_link, ESP_OK);
    assert(test_view.battery_percent_valid && test_view.battery_percent == 63);
    assert(test_view.battery_state == APP_BATTERY_CHARGING);
    assert(test_view.wifi_rssi_valid && test_view.wifi_rssi_dbm == -41);
    assert(strcmp(test_view.wifi_ssid, "TEST-NETWORK") == 0 && strcmp(test_view.wifi_ipv4, "192.0.2.14") == 0);
    assert(test_view.paired_host_count == 2 && test_view.pairing_state == APP_PAIRING_OPEN);
    assert(test_view.paired_host_capacity == 32);
    assert(test_view.pairing_seconds_left == 2 && test_view.settings_save_state == APP_SAVE_PENDING);
    test_power.charging = BOARD_TRUTH_NO;
    test_power.current_ma = -30;
    render_view(&test_link, ESP_OK);
    assert(test_view.battery_state == APP_BATTERY_DISCHARGING);
    test_power.full = BOARD_TRUTH_YES;
    test_power.percent = 100;
    render_view(&test_link, ESP_OK);
    assert(test_view.battery_state == APP_BATTERY_FULL && test_view.battery_percent == 100);
    /* Board read failures publish an invalidated cache, never an invented 0% battery. */
    test_power_error = ESP_FAIL;
    test_power = (board_power_status_t){.error = ESP_FAIL};
    test_link.connection_admitted = test_link.connected = false;
    test_wifi.state = APP_WIFI_BACKOFF;
    test_wifi.retry_in_ms = 1001;
    test_storage.pending = false;
    test_storage.error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(!test_view.battery_percent_valid && test_view.battery_state == APP_BATTERY_UNKNOWN);
    assert(!test_view.wifi_rssi_valid);
    assert(test_view.wifi_state == APP_UI_WIFI_RETRYING && test_view.settings_save_state == APP_SAVE_ERROR);
    test_storage.error = ESP_OK;
    test_storage.saved_revision = 2;
    render_view(&test_link, ESP_OK);
    assert(test_view.settings_save_state == APP_SAVE_SAVED);
    test_storage.available = false;
    test_link.bond_forget_state = BLE_HID_FORGET_FAILED;
    test_wifi.state = APP_WIFI_ERROR;
    test_wifi.last_error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(test_view.settings_save_state == APP_SAVE_UNAVAILABLE && test_view.pairing_state == APP_PAIRING_ERROR);
    assert(test_view.wifi_state == APP_UI_WIFI_ERROR && test_view.wifi_message[0]);
    const uint64_t revision = s_ui_revision, epoch = s_engine.state.epoch;
    for (unsigned i = 0; i < 1000; ++i) render_view(&test_link, ESP_OK);
    assert(s_ui_revision == revision && s_engine.state.epoch == epoch);
    assert(test_storage_requests == 0 && test_brightness_count == 0 && test_wifi_info_reads == 0);
    expect_idle();
}

static void all_settings_pages_own_focus_in_every_mode(void)
{
    const app_action_t background[] = {APP_GRID, APP_KEY, APP_CLICK, APP_MOVE,
        APP_SEND, APP_CLEAR, APP_SHORTCUT, APP_MODE, APP_MOTION, APP_DRAG, APP_VOICE_START};
    for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        for (int page = APP_SETTINGS_HOME; page <= APP_SETTINGS_WIFI; ++page) {
            setup((input_mode_t)mode);
            settings_page((app_settings_page_t)page);
            const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
            const size_t reports = trace_count;
            for (size_t i = 0; i < sizeof background / sizeof background[0]; ++i)
                action(background[i], 1);
            assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == (app_settings_page_t)page);
            assert(s_engine.state.mode == (input_mode_t)mode && s_engine.state.epoch == epoch);
            assert(s_ui_revision == revision && test_storage_requests == 0 && trace_count == reports);
            expect_idle();
        }
    }
}

static void physical_modal_exit_stops_wifi_setup(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_WIFI);
    test_wifi_start_error = ESP_OK;
    action(APP_WIFI_SETUP, 1);
    release_barrier();
    test_wifi.setup_active = true;
    button_edge(true, 1010);
    button_edge(false, 1030);
    button_edge(true, 1100);
    button_edge(false, 1120);
    assert(s_overlay == APP_OVERLAY_MODES);
    assert(test_wifi_stops == 1); /* Leaving setup must close its AP on every navigation path. */
    release_barrier();
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_setup_passcode[0] == '\0');
}

static void wake_gesture_cannot_enter_the_priority_lock_path(void)
{
    const app_action_t controls[] = {APP_LOCK, APP_RELEASE};
    for (size_t i = 0; i < sizeof controls / sizeof controls[0]; ++i) {
        setup(INPUT_MOUSE);
        display_at(301000);
        brightness_applied(0);
        touch(APP_TOUCH_BEGIN, 1001, 0, 0);
        brightness_applied(50);
        touch(APP_TOUCH_END, 1001, 0, 0);
        service_display();
        const uint64_t epoch = s_engine.state.epoch;
        app_intent_t old_control = make_intent(controls[i], 0, 0, NULL);
        old_control.gesture = 1001;
        assert(submit_intent(&old_control));
        if (!process_priority()) {
            consume_one();
            (void)process_priority();
        }
        assert(!s_engine.state.locked && s_engine.state.epoch == epoch);
        expect_idle(); /* Consumed wake gestures cannot trigger either priority control. */
    }
}

static void release_priority_stops_wifi_setup(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_WIFI);
    test_wifi_start_error = ESP_OK;
    action(APP_WIFI_SETUP, 1);
    release_barrier();
    app_intent_t release = make_intent(APP_RELEASE, 0, 0, NULL);
    assert(submit_intent(&release) && process_priority());
    assert(s_overlay == APP_OVERLAY_NONE && test_wifi_stops == 1);
    release_barrier();
}

static void priority_reset_reconciles_a_queued_touch_release(void)
{
    setup(INPUT_MOUSE);
    touch(APP_TOUCH_BEGIN, 1101, 0, 0);
    s_now_ms = 1010;
    app_intent_t end = make_intent(APP_TOUCH_END, 0, 0, NULL);
    end.gesture = 1101;
    assert(submit_intent(&end));
    assert(!s_touch_down); /* Callback evidence arrived; its queued END has not. */
    app_intent_t release = make_intent(APP_RELEASE, 0, 0, NULL);
    assert(submit_intent(&release) && process_priority());
    expect_empty_intents();
    release_barrier();
    display_at(301009);
    assert(!s_idle.asleep);
    display_at(301010);
    assert(s_idle.asleep); /* A discarded END must not keep the device awake forever. */
    assert(!s_idle.down[DEVICE_INPUT_TOUCH]);
}

static void overflow_reconciles_a_captured_wake_touch_release(void)
{
    setup(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    touch(APP_TOUCH_BEGIN, 1201, 0, 0);
    brightness_applied(50);
    assert(s_idle.wake_capture);
    fill_intents();
    s_now_ms = 301010;
    app_intent_t end = make_intent(APP_TOUCH_END, 0, 0, NULL);
    end.gesture = 1201;
    assert(!submit_intent(&end)); /* Full queue loses END but retains callback state. */
    assert(!s_touch_down && process_priority());
    release_barrier();
    service_display();
    assert(!s_idle.wake_capture && !s_idle.down[DEVICE_INPUT_TOUCH]);
    const uint64_t epoch = s_engine.state.epoch;
    touch(APP_CLICK, 1201, 1, 0);
    assert(s_engine.state.epoch == epoch);
    expect_idle(); /* Reconciliation must retain the consumed gesture watermark. */
    touch(APP_TOUCH_BEGIN, 1202, 0, 0);
    touch(APP_TOUCH_END, 1202, 0, 0);
    touch(APP_CLICK, 1202, 1, 0);
    const uint8_t click[] = {1, 0, 0, 0};
    const uint8_t release[] = {0, 0, 0, 0};
    expect_report(2, click, sizeof click);
    expect_report(2, release, sizeof release);
    display_at(601010);
    assert(s_idle.asleep);
}

static void fresh_touch_safety_during_ai_guard(app_action_t control)
{
    setup(INPUT_MOUSE);
    press(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    button_edge(true, 301010);
    button_edge(false, 301030);
    brightness_applied(50);
    assert(s_idle.wake_capture && !s_screen_accepts_input);
    s_now_ms = 301040;
    touch(APP_TOUCH_BEGIN, 1301, 0, 0);
    touch(APP_TOUCH_END, 1301, 0, 0);
    service_display();
    assert(s_idle.wake_capture); /* The AI-only double-click guard still exists. */
    app_intent_t safety = make_intent(control, 0, 0, NULL);
    safety.gesture = 1301;
    assert(submit_intent(&safety));
    if (!process_priority()) {
        consume_one();
        (void)process_priority();
    }
    assert(s_engine.state.release_pending);
    assert(s_engine.state.locked == (control == APP_LOCK));
    release_barrier();
    expect_idle();
}

static void fresh_touch_lock_survives_ai_wake_guard(void)
{
    fresh_touch_safety_during_ai_guard(APP_LOCK);
}

static void fresh_touch_release_survives_ai_wake_guard(void)
{
    fresh_touch_safety_during_ai_guard(APP_RELEASE);
}

static void bluetooth_bond_views_preserve_unknown_and_verified_identity(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_BLUETOOTH);
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_hosts_available && test_view.bluetooth_host_count == 0);
    test_bonds = (ble_hid_bond_list_t){.known = true, .revision = 1};
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_hosts_available && test_view.bluetooth_host_count == 0);
    test_bonds.count = 2;
    test_bonds.bonds[0] = (ble_hid_bond_t){.address = {2, 3, 4, 5, 6, 7},
        .address_type_known = true, .address_type = 1, .is_current = true};
    test_bonds.bonds[1] = (ble_hid_bond_t){.address = {2, 8, 9, 10, 11, 12},
        .address_type = 0};
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_host_count == 2);
    assert(strcmp(test_view.bluetooth_hosts[0].identity, "02:03:04:05:06:07/1") == 0);
    assert(strcmp(test_view.bluetooth_hosts[1].identity, "02:08:09:0A:0B:0C/?0") == 0);
    assert(test_view.bluetooth_hosts[0].name[0] == '\0' && test_view.bluetooth_hosts[1].name[0] == '\0');
    assert(!test_view.bluetooth_hosts[0].connected); /* A row flag alone cannot prove an active bond. */
    test_bonds.current_match_known = true;
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_hosts[0].connected && !test_view.bluetooth_hosts[1].connected);
    test_link.connection_admitted = test_link.connected = false;
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_hosts[0].connected);
    test_bonds.known = false;
    test_bonds.error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_hosts_available && test_view.bluetooth_host_count == 0);
    assert(test_view.bluetooth_hosts[0].identity[0] == '\0');
    assert(test_storage_requests == 0 && test_pair_forgets == 0 && test_disconnects == 0);
}

static void bluetooth_pagination_is_bounded_and_contextual(void)
{
    setup(INPUT_KEYBOARD);
    test_bonds = (ble_hid_bond_list_t){.known = true, .count = 32, .revision = 8};
    for (uint8_t i = 0; i < 32; ++i) {
        test_bonds.bonds[i].address[0] = 2;
        test_bonds.bonds[i].address[5] = i;
        test_bonds.bonds[i].address_type_known = true;
    }
    action(APP_BLUETOOTH_PAGE, 7);
    assert(s_bluetooth_page == 0); /* Hidden pagination has no effect. */
    settings_page(APP_SETTINGS_BLUETOOTH);
    app_intent_t stale = make_intent(APP_BLUETOOTH_PAGE, 0, 0, NULL);
    action(APP_BLUETOOTH_PAGE, 7);
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_page == 7 && test_view.bluetooth_host_count == 32);
    assert(strcmp(test_view.bluetooth_hosts[31].identity, "02:00:00:00:00:1F/0") == 0);
    deliver(stale);
    assert(s_bluetooth_page == 7);
    action(APP_BLUETOOTH_PAGE, -1);
    action(APP_BLUETOOTH_PAGE, 8);
    assert(s_bluetooth_page == 7);
    test_bonds.count = 1;
    ++test_bonds.revision;
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_page == 0 && test_view.bluetooth_host_count == 1);
    assert(test_view.bluetooth_hosts[1].identity[0] == '\0');
    action(APP_BLUETOOTH_PAGE, 1);
    assert(s_bluetooth_page == 0);
    action(APP_THEME, 0);
    action(APP_KEY, 4);
    action(APP_WIFI_SETUP, 1);
    assert(test_storage_requests == 0 && test_wifi_starts == 0);
    expect_idle();
}

static void disconnect_current_host_preserves_saved_pairings(void)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 4};
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    release_barrier();
    assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_DISCONNECT);
    action(APP_CONFIRM, 1);
    service_pair_operation(&test_link);
    assert(test_disconnects == 0);
    release_barrier();
    test_disconnect_accept = true;
    service_pair_operation(&test_link);
    assert(test_disconnects == 1 && test_pair_opens == 0 && test_pair_forgets == 0);
    service_pair_operation(&test_link);
    assert(test_disconnects == 1);
    test_link.connection_admitted = test_link.connected = test_link.ready = false;
    ++test_link.generation;
    hid_engine_set_link(&s_engine, false, false);
    service_pair_operation(&test_link);
    assert(s_pair_operation == APP_CONFIRM_NONE && test_pair_opens == 0 && test_pair_forgets == 0);
    assert(test_link.reconnect_paused && strstr(s_message, "automatic reconnect paused"));
    assert(test_storage_requests == 0 && test_wifi_forgets == 0);
    assert(strcmp(s_interaction.view.preview, "AB") == 0);
    render_view(&test_link, ESP_OK);
    assert(test_view.bluetooth_host_count == 2 && test_bonds.revision == 4);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    assert(s_overlay == APP_OVERLAY_SETTINGS && test_disconnects == 1);
}

static void saturated_queue_preserves_real_touch_lock(void)
{
    setup(INPUT_MOUSE);
    press(INPUT_MOUSE);
    touch(APP_TOUCH_BEGIN, 1401, 0, 0);
    touch(APP_TOUCH_END, 1401, 0, 0);
    fill_intents();
    app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
    lock.gesture = 1401; /* The real UI path carries its physical gesture identity. */
    (void)submit_intent(&lock);
    assert(process_priority());
    assert(s_engine.state.locked); /* Queue pressure cannot downgrade Lock to Release. */
    expect_empty_intents();
    release_barrier();
    expect_idle();
}

static void nine_cell_grid_reaches_every_group_and_rejects_empty_children(void)
{
    const char *groups[] = {"ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"};
    const char last[] = "DHLPTX159";
    for (int group = 0; group < 9; ++group) {
        setup(INPUT_KEYBOARD);
        render_view(&test_link, ESP_OK);
        assert(test_view.keyboard_cell_count == 9 && test_view.keyboard_focus == 4);
        for (size_t cell = 0; cell < 9; ++cell)
            assert(strcmp(test_view.keyboard_cells[cell], groups[cell]) == 0);
        action(APP_GRID, group);
        render_view(&test_link, ESP_OK);
        assert(test_view.keyboard_group == group && test_view.keyboard_cell_count == 4);
        assert(test_view.keyboard_focus == 1);
        const uint64_t revision = s_ui_revision;
        for (int cell = 4; cell < 9; ++cell) {
            assert(test_view.keyboard_cells[cell][0] == '\0');
            action(APP_GRID, cell);
        }
        assert(s_ui_revision == revision && s_interaction.view.preview_length == 0);
        action(APP_GRID, 3);
        assert(s_interaction.view.preview_length == 1 && s_interaction.view.preview[0] == last[group]);
        render_view(&test_link, ESP_OK);
        assert(test_view.keyboard_group == -1 && test_view.keyboard_focus == 4);
        action(APP_GRID, 9);
        action(APP_GRID, -1);
        assert(s_interaction.view.group == -1 && s_interaction.view.preview_length == 1);
        expect_idle();
    }
    setup(INPUT_KEYBOARD);
    action(APP_SYMBOLS, 0);
    action(APP_GRID, 8);
    render_view(&test_link, ESP_OK);
    assert(test_view.keyboard_cell_count == 3 && test_view.keyboard_focus == 1);
    for (int cell = 3; cell < 9; ++cell) action(APP_GRID, cell);
    assert(s_interaction.view.preview_length == 0);
    action(APP_GRID, 2);
    assert(strcmp(s_interaction.view.preview, "^") == 0);
    expect_idle();
}

static void saturated_queue_rejects_consumed_wake_emergencies(void)
{
    setup(INPUT_MOUSE);
    display_at(301000);
    brightness_applied(0);
    touch(APP_TOUCH_BEGIN, 1501, 0, 0);
    touch(APP_TOUCH_END, 1501, 0, 0);
    brightness_applied(50);
    fill_intents();
    const uint64_t epoch = s_engine.state.epoch;
    app_intent_t control = make_intent(APP_LOCK, 0, 0, NULL);
    control.gesture = 1501;
    assert(submit_intent(&control));
    control.action = APP_RELEASE;
    assert(submit_intent(&control));
    assert(!process_priority());
    assert(!s_engine.state.locked && s_engine.state.epoch == epoch);
    expect_idle();
    assert(!process_priority()); /* Rejected emergency slots must not replay later. */
}

static void saturated_emergency_waits_for_its_queued_begin(void)
{
    for (unsigned asleep = 0; asleep < 2; ++asleep) {
        setup(INPUT_MOUSE);
        if (asleep) {
            display_at(301000);
            brightness_applied(0);
        }
        app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
        begin.gesture = 1601;
        assert(submit_intent(&begin));
        app_intent_t end = begin;
        end.action = APP_TOUCH_END;
        assert(submit_intent(&end));
        fill_intents();
        app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
        lock.gesture = 1601;
        assert(submit_intent(&lock));
        assert(!process_priority());
        assert(!s_engine.state.locked); /* A not-yet-routed BEGIN proves neither wake nor admission. */
        consume_one();
        if (!asleep) {
            assert(process_priority() && s_engine.state.locked);
            expect_empty_intents();
            release_barrier();
            assert(!s_idle.down[DEVICE_INPUT_TOUCH]);
        } else {
            assert(!process_priority() && !s_engine.state.locked);
            while (s_intents->count) consume_one();
            brightness_applied(50);
            assert(!s_idle.wake_capture && !s_idle.down[DEVICE_INPUT_TOUCH]);
            assert(!process_priority());
        }
        expect_idle();
    }
}

static void transport_setup(input_mode_t mode, app_input_transport_t selected)
{
    setup(mode);
    test_usb = (usb_hid_snapshot_t){.initialized = true, .mounted = true,
        .enabled = selected == APP_INPUT_USB, .ready = selected == APP_INPUT_USB,
        .generation = 71};
    if (selected == APP_INPUT_USB) {
        s_input_transport = s_requested_transport = selected;
        s_settings.preferred_transport = DEVICE_TRANSPORT_USB;
        hid_engine_set_link(&s_engine, false, false);
        hid_engine_set_link(&s_engine, true, true);
        release_barrier();
    }
    trace_count = 0;
    test_ble_send_attempts = test_usb_send_attempts = 0;
    test_previous_link = input_link_snapshot(&test_link);
}

static void bluetooth_management_ignores_unrelated_usb_release(void)
{
    for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        for (int transport = APP_INPUT_BLUETOOTH; transport <= APP_INPUT_USB; ++transport) {
            transport_setup((input_mode_t)mode, (app_input_transport_t)transport);
            if (transport == APP_INPUT_USB) {
                test_usb.mounted = test_usb.ready = false;
                input_link_t active = input_link_snapshot(&test_link);
                observe_input_link(&active, &test_previous_link);
            }
            hid_engine_cancel(&s_engine);
            assert(s_engine.state.release_pending && test_link.connected);
            refresh_connectivity_context();
            assert(test_connectivity_context.release_pending == (transport == APP_INPUT_BLUETOOTH));
            s_pair_operation = APP_CONFIRM_DISCONNECT;
            s_pair_origin_generation = test_link.generation;
            s_pair_deadline_ms = s_now_ms + 5000;
            test_disconnect_accept = true;
            service_pair_operation(&test_link);
            assert(test_disconnects == (transport == APP_INPUT_USB ? 1U : 0U));
            assert(s_engine.state.release_pending && !test_ble_send_attempts && !test_usb_send_attempts);
        }
    }
}

static void acknowledge_selected_transport(void)
{
    if (s_input_transport == APP_INPUT_USB) {
        assert(test_usb.enabled);
        test_usb.ready = true; /* Explicit synthetic stack acknowledgement, not selection admission. */
    }
    input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    sync_interaction();
}

static void transport_switch_waits_for_both_releases_and_completion(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (int mode = INPUT_MOUSE; mode <= INPUT_KEYBOARD; ++mode) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup((input_mode_t)mode, from);
            if (mode == INPUT_MOUSE) {
                action(APP_DRAG, 1);
                const uint8_t held[] = {1, 0, 0, 0};
                expect_report(2, held, sizeof held);
            } else {
                fill_preview(2);
                press(INPUT_KEYBOARD);
            }
            assert(trace_count == 1 && trace[0].transport == from);
            action(APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
            action(APP_TRANSPORT_SELECT, (int)to);
            assert(s_transport_switch_pending && s_input_transport == from);
            assert(test_storage_requests == 0 && test_usb_enable_count == 0);
            service_transport_switch();
            assert(s_transport_switch_pending);
            static const uint8_t zero[8] = {0};
            expect_report(1, zero, 8);
            service_transport_switch();
            assert(s_transport_switch_pending && test_usb_enable_count == 0);
            expect_report(2, zero, 4);
            assert(trace[1].transport == from && trace[2].transport == from);
            if (from == APP_INPUT_USB) test_usb.congested = true;
            else test_link.congested = true;
            service_transport_switch();
            assert(s_transport_switch_pending && s_input_transport == from && test_storage_requests == 0);
            if (from == APP_INPUT_USB) test_usb.congested = false;
            else test_link.congested = false;
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == to);
            assert(test_usb_enable_count == 1 && test_usb_enable_requests[0] == (to == APP_INPUT_USB));
            assert(test_usb_enable_trace[0] == 3 && test_storage_requests == 1);
            assert(test_saved_request.preferred_transport ==
                (to == APP_INPUT_USB ? DEVICE_TRANSPORT_USB : DEVICE_TRANSPORT_BLUETOOTH));
            assert(s_engine.state.mode == (input_mode_t)mode && !s_engine.state.gyro_enabled && !s_drag_requested);
            acknowledge_selected_transport();
            release_barrier();
            assert(trace[3].transport == to && trace[4].transport == to);
            action(APP_CLOSE, 0);
            if (mode == INPUT_KEYBOARD) {
                assert(strcmp(s_interaction.view.preview, "AB") == 0);
                action(APP_KEY, 4);
                const uint8_t down[] = {0, 0, 4, 0, 0, 0, 0, 0};
                expect_report(1, down, sizeof down);
                expect_report(1, zero, 8);
            } else {
                action(APP_CLICK, 1);
                const uint8_t down[] = {1, 0, 0, 0};
                expect_report(2, down, sizeof down);
                expect_report(2, zero, 4);
            }
            assert(trace[5].transport == to && trace[6].transport == to);
            assert(test_pair_forgets == 0 && test_wifi_forgets == 0 && test_disconnects == 0);
        }
    }
}

static void transport_selection_is_page_scoped_and_failure_visible(void)
{
    transport_setup(INPUT_AUDIO, APP_INPUT_BLUETOOTH);
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending && test_usb_enable_count == 0);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending);
    settings_page(APP_SETTINGS_CONNECTIVITY);
    action(APP_TRANSPORT_SELECT, -1);
    action(APP_TRANSPORT_SELECT, 2);
    action(APP_TRANSPORT_SELECT, APP_INPUT_BLUETOOTH);
    assert(!s_transport_switch_pending && test_storage_requests == 0);
    test_usb.initialized = false;
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending);
    test_usb.initialized = true;
    test_usb.failed = true;
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(!s_transport_switch_pending);
    test_usb.failed = false;
    app_intent_t stale = make_intent(APP_TRANSPORT_SELECT, APP_INPUT_USB, 0, NULL);
    settings_page(APP_SETTINGS_DISPLAY);
    deliver(stale);
    assert(!s_transport_switch_pending);
    settings_page(APP_SETTINGS_CONNECTIVITY);
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    assert(s_transport_switch_pending && test_storage_requests == 0);
    test_usb.failed = true; /* Availability can disappear after admission. */
    service_transport_switch();
    assert(!s_transport_switch_pending && s_input_transport == APP_INPUT_BLUETOOTH);
    assert(s_requested_transport == APP_INPUT_BLUETOOTH && test_usb_enable_count == 0);
    assert(test_storage_requests == 0 && s_engine.state.mode == INPUT_AUDIO);
    test_usb.failed = false;
    service_transport_switch();
    assert(s_input_transport == APP_INPUT_BLUETOOTH); /* Late readiness cannot revive a cancelled switch. */
    release_barrier();
    action(APP_TRANSPORT_SELECT, APP_INPUT_USB);
    release_barrier();
    test_storage_error = ESP_FAIL;
    service_transport_switch();
    assert(s_input_transport == APP_INPUT_USB && test_storage_requests == 1);
    assert(s_engine.state.mode == INPUT_AUDIO && !s_engine.state.gyro_enabled);
    test_storage = (settings_store_status_t){.available = true, .error = ESP_FAIL};
    render_view(&test_link, ESP_OK);
    assert(test_view.input_transport == APP_INPUT_USB && test_view.settings_save_state == APP_SAVE_ERROR);
    assert(!test_view.audio_available && test_view.microphone_muted);
}

static void transport_blocked_old_host_times_out_without_persisting(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup(INPUT_MOUSE, from);
            settings_page(APP_SETTINGS_CONNECTIVITY);
            action(APP_TRANSPORT_SELECT, (int)to);
            release_barrier();
            if (from == APP_INPUT_USB) {
                test_usb.suspended = fault == 0;
                test_usb.failed = fault == 1;
                test_usb.congested = fault == 2;
                if (fault < 2) test_usb.ready = false;
            } else {
                test_link.suspended = fault == 0;
                test_link.failed = fault == 1;
                test_link.congested = fault == 2;
                if (fault < 2) test_link.ready = false;
            }
            s_now_ms = 5999;
            service_transport_switch();
            assert(s_transport_switch_pending && s_input_transport == from);
            s_now_ms = 6000;
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == from && s_requested_transport == from);
            assert(test_usb_enable_count == 0 && test_storage_requests == 0);
            test_usb.suspended = test_usb.failed = test_usb.congested = false;
            test_link.suspended = test_link.failed = test_link.congested = false;
            service_transport_switch();
            assert(s_input_transport == from && test_storage_requests == 0);
        }
    }
}

static void transport_lock_or_release_cancels_late_switch_completion(void)
{
    const app_action_t controls[] = {APP_LOCK, APP_RELEASE};
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (size_t i = 0; i < sizeof controls / sizeof controls[0]; ++i) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup(INPUT_KEYBOARD, from);
            fill_preview(2);
            settings_page(APP_SETTINGS_CONNECTIVITY);
            action(APP_TRANSPORT_SELECT, (int)to);
            app_intent_t stale = make_intent(APP_TRANSPORT_SELECT, (int)to, 0, NULL);
            app_intent_t control = make_intent(controls[i], 0, 0, NULL);
            assert(submit_intent(&control) && process_priority());
            release_barrier();
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == from);
            assert(s_engine.state.locked == (controls[i] == APP_LOCK));
            assert(test_usb_enable_count == 0 && test_storage_requests == 0);
            deliver(stale);
            service_transport_switch();
            assert(!s_transport_switch_pending && s_input_transport == from);
            assert(strcmp(s_interaction.view.preview, "AB") == 0);
        }
    }
}

static void disconnected_old_transport_allows_explicit_offline_selection(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_KEYBOARD, from);
        fill_preview(2);
        test_link.connection_admitted = test_link.connected = test_link.ready = false;
        hid_engine_set_link(&s_engine, false, false);
        test_usb.mounted = test_usb.ready = false;
        action(APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        const size_t before = trace_count;
        service_transport_switch();
        assert(s_input_transport == to && !s_transport_switch_pending);
        assert(trace_count == before && test_storage_requests == 1);
        assert(test_usb.enabled == (to == APP_INPUT_USB));
        input_link_t active = input_link_snapshot(&test_link);
        assert(!active.connected && !active.ready && active.kind == to);
        render_view(&test_link, ESP_OK);
        assert(!test_view.connected && !test_view.ready && test_view.input_transport == to);
        assert(test_view.usb_available && !test_view.usb_connected);
        assert(test_view.bluetooth_available && !test_view.bluetooth_connected);
        assert(strcmp(s_interaction.view.preview, "AB") == 0);
        action(APP_CLOSE, 0);
        action(APP_SEND, 0);
        assert(trace_count == before && !s_send_token);
    }
}

static void inactive_bluetooth_events_do_not_retarget_selected_usb(void)
{
    transport_setup(INPUT_KEYBOARD, APP_INPUT_USB);
    const input_link_t original = input_link_snapshot(&test_link);
    const uint64_t epoch = s_engine.state.epoch;
    action(APP_KEY, 4);
    test_link.generation += 10;
    test_link.connection_admitted = test_link.connected = test_link.ready = false;
    test_link.failed = test_link.congested = test_link.suspended = true;
    test_link.keyboard_leds = 7;
    test_usb.keyboard_leds = 2;
    const input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(active.kind == original.kind && active.generation == original.generation);
    assert(s_engine.state.epoch == epoch);
    assert(active.connected && active.ready && !active.failed && !active.congested && !active.suspended);
    render_view(&test_link, ESP_FAIL);
    assert(test_view.input_transport == APP_INPUT_USB && test_view.connected && test_view.ready);
    assert(!test_view.transport_error && test_view.keyboard_leds == 2);
    assert(!test_view.bluetooth_available && !test_view.bluetooth_connected);
    const unsigned ble_attempts = test_ble_send_attempts;
    const uint8_t down[] = {0, 0, 4, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    expect_report(1, down, sizeof down);
    expect_report(1, zero, sizeof zero);
    assert(trace[0].transport == APP_INPUT_USB && trace[1].transport == APP_INPUT_USB);
    assert(test_ble_send_attempts == ble_attempts && test_usb_send_attempts == 2);
    assert(test_storage_requests == 0 && test_usb_enable_count == 0);
}

static void transport_replaced_generation_and_refused_release_cannot_replay(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_KEYBOARD, from);
        fill_preview(4);
        action(APP_SEND, 0);
        const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
        expect_report(1, a, sizeof a);
        update_send(false);
        action(APP_SETTINGS_PAGE, APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        assert(strcmp(s_interaction.view.preview, "BAB") == 0 && !s_send_token);
        if (from == APP_INPUT_USB) test_usb_send_failure = true;
        else test_send_failure = true;
        assert(step() == HID_ENGINE_STEP_SEND_FAILED);
        service_transport_switch();
        assert(s_transport_switch_pending && s_input_transport == from && test_storage_requests == 0);
        test_usb_send_failure = test_send_failure = false;
        const input_link_t old_snapshot = input_link_snapshot(&test_link);
        if (from == APP_INPUT_USB) ++test_usb.generation;
        else ++test_link.generation;
        static const uint8_t zero[8] = {0};
        uint64_t obsolete = old_snapshot.generation;
        assert(!send_report(&obsolete, 1, zero, 8));
        assert(trace_count == 1);
        const input_link_t replacement = input_link_snapshot(&test_link);
        observe_input_link(&replacement, &test_previous_link);
        service_transport_switch();
        assert(!s_transport_switch_pending && s_input_transport == from && test_storage_requests == 0);
        assert(test_usb_enable_count == 0); /* Connected origin replacement requires fresh intent. */
        action(APP_TRANSPORT_SELECT, (int)to);
        assert(s_transport_switch_pending);
        release_barrier();
        service_transport_switch();
        assert(s_input_transport == to && test_storage_requests == 1);
        assert(strcmp(s_interaction.view.preview, "BAB") == 0 && !s_send_token);
        assert(trace_count == 3 && trace[1].transport == from && trace[2].transport == from);
        acknowledge_selected_transport();
        release_barrier();
        assert(trace_count == 5 && trace[3].transport == to && trace[4].transport == to);
        expect_idle(); /* No discarded suffix can leak to the replacement target. */
    }
}

static void transport_unavailable_targets_never_enable_or_persist(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            const app_input_transport_t from = (app_input_transport_t)old;
            const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
            transport_setup(INPUT_MOUSE, from);
            settings_page(APP_SETTINGS_CONNECTIVITY);
            if (to == APP_INPUT_USB) {
                test_usb.initialized = fault != 0;
                test_usb.failed = fault == 1;
            } else {
                test_link.initialized = fault != 0;
                test_link.failed = fault == 1;
            }
            action(APP_TRANSPORT_SELECT, (int)to);
            assert(s_transport_switch_pending == (fault == 2));
            if (fault == 2) {
                if (to == APP_INPUT_USB) test_usb.failed = true;
                else test_link.failed = true;
                service_transport_switch();
            }
            assert(!s_transport_switch_pending && s_input_transport == from);
            assert(test_usb_enable_count == 0 && test_storage_requests == 0);
        }
    }
}

static void wifi_unknown_storage_recovery_binds_complete_confirmation(void)
{
    for (unsigned changed = 0; changed < 6; ++changed) {
        setup(INPUT_MOUSE);
        settings_page(APP_SETTINGS_WIFI);
        test_wifi = (app_wifi_snapshot_t){.initialized = true, .state = APP_WIFI_ERROR,
            .stored_data_known = true, .stored_data_present = true,
            .storage_uncertain = true, .storage_error = ESP_FAIL};
        assert(!test_wifi.configured);
        action(APP_WIFI_FORGET_REQUEST, 0);
        release_barrier();
        assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == APP_CONFIRM_WIFI_FORGET);
        if (changed == 0) strcpy(test_wifi.ssid, "TEST-RUNTIME");
        if (changed == 1) strcpy(test_wifi.stored_ssid, "TEST-STORED");
        if (changed == 2) test_wifi.stored_data_known = false;
        if (changed == 3) test_wifi.stored_data_present = false;
        if (changed == 4) test_wifi.storage_uncertain = false;
        test_wifi_forget_error = ESP_OK;
        action(APP_CONFIRM, 1);
        release_barrier();
        assert(test_wifi_forgets == (changed == 5 ? 1U : 0U));
        assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_WIFI);
        assert(test_storage_requests == 0 && test_pair_forgets == 0);
    }
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_WIFI);
    test_wifi = (app_wifi_snapshot_t){.initialized = true, .state = APP_WIFI_IDLE,
        .storage_uncertain = true, .storage_error = ESP_FAIL};
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_saved_network && !test_view.wifi_operation_pending);
    assert(test_view.wifi_message[0] && !test_view.wifi_ssid[0]);
    action(APP_WIFI_FORGET_REQUEST, 0); /* Unknown presence also permits explicit recovery. */
    release_barrier();
    action(APP_CONFIRM, 1); /* Configurable service rejects the request. */
    release_barrier();
    assert(test_wifi_forgets == 1 && test_wifi.storage_uncertain);
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_saved_network && test_view.wifi_message[0]);
    test_wifi.cleanup_pending = true;
    test_wifi.cleanup_error = ESP_FAIL;
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_operation_pending && test_view.wifi_message[0]);
    assert(test_view.wifi_setup_passcode[0] == '\0');
}

static void usb_removal_never_falls_back_or_replays_to_bluetooth(void)
{
    transport_setup(INPUT_KEYBOARD, APP_INPUT_USB);
    fill_preview(2);
    action(APP_SEND, 0);
    const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
    expect_report(1, a, sizeof a);
    update_send(false);
    test_usb.mounted = test_usb.ready = false;
    ++test_usb.generation;
    input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    sync_interaction();
    service_transport_switch();
    render_view(&test_link, ESP_OK);
    assert(s_input_transport == APP_INPUT_USB && !s_transport_switch_pending);
    assert(!test_view.connected && test_view.bluetooth_connected && test_view.bluetooth_ready);
    assert(strcmp(s_interaction.view.preview, "B") == 0 && !s_send_token);
    assert(test_usb_enable_count == 0 && test_storage_requests == 0 && test_ble_send_attempts == 0);
    action(APP_SEND, 0);
    assert(trace_count == 1 && !s_send_token);
    test_usb.mounted = true;
    ++test_usb.generation;
    acknowledge_selected_transport();
    release_barrier();
    service_transport_switch();
    expect_idle();
    assert(trace_count == 3 && trace[1].transport == APP_INPUT_USB && trace[2].transport == APP_INPUT_USB);
    assert(strcmp(s_interaction.view.preview, "B") == 0 && test_ble_send_attempts == 0);
}

static void audio_mode_remains_unavailable_across_both_transport_choices(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_AUDIO, from);
        settings_page(APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        release_barrier();
        service_transport_switch();
        assert(s_input_transport == to && s_engine.state.mode == INPUT_AUDIO);
        acknowledge_selected_transport();
        release_barrier();
        render_view(&test_link, ESP_OK);
        assert(test_view.mode == INPUT_AUDIO && test_view.input_transport == to);
        assert(!test_view.audio_available && !test_view.audio_active && test_view.microphone_muted);
        action(APP_CLOSE, 0);
        action(APP_AUDIO_POWER, 1);
        action(APP_VOICE_START, 1);
        expect_idle();
        assert(test_storage_requests == 1 && !s_engine.state.gyro_enabled);
    }
}

static void usb_selection_rearms_only_a_reset_cleared_selected_adapter(void)
{
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    for (unsigned i = 0; i < 100; ++i) service_usb_selection();
    assert(test_usb_enable_count == 0 && !test_usb.enabled);
    transport_setup(INPUT_MOUSE, APP_INPUT_USB);
    test_usb.failed = true;
    test_usb.enabled = test_usb.ready = false;
    for (unsigned i = 0; i < 100; ++i) service_usb_selection();
    assert(test_usb_enable_count == 0 && !test_usb.enabled);
    test_usb.initialized = false;
    test_usb.failed = false;
    service_usb_selection();
    assert(test_usb_enable_count == 0);
    /* A synthetic real reset has cleared failure; the selected adapter is
     * initialized again, with a fresh session but no input enable/readiness. */
    test_usb.initialized = true;
    ++test_usb.generation;
    const uint64_t before_enable = test_usb.generation;
    service_usb_selection();
    assert(test_usb_enable_count == 1 && test_usb_enable_requests[0] && test_usb.enabled);
    assert(test_usb.generation > before_enable && !test_usb.ready);
    for (unsigned i = 0; i < 1000; ++i) service_usb_selection();
    assert(test_usb_enable_count == 1 && test_storage_requests == 0);
    assert(s_input_transport == APP_INPUT_USB && trace_count == 0);
}

static void usb_reset_rearm_discards_stale_work_before_fresh_reports(void)
{
    transport_setup(INPUT_KEYBOARD, APP_INPUT_USB);
    fill_preview(4);
    action(APP_SEND, 0);
    const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
    expect_report(1, a, sizeof a);
    update_send(false);
    fill_intents();
    const uint64_t first_epoch = s_engine.state.epoch;
    test_usb.failed = true;
    test_usb.enabled = test_usb.ready = false;
    ++test_usb.generation;
    service_usb_selection();
    input_link_t active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(test_usb_enable_count == 0 && s_engine.state.epoch != first_epoch);
    expect_empty_intents();
    assert(!s_send_token && strcmp(s_interaction.view.preview, "BAB") == 0);
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 1);
    fill_intents(); /* Old producer work also exists immediately before reset. */
    const uint64_t failed_epoch = s_engine.state.epoch;
    test_usb.failed = false;
    ++test_usb.generation;
    service_usb_selection();
    active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(test_usb_enable_count == 1 && test_usb.enabled && !test_usb.ready);
    assert(s_engine.state.epoch != failed_epoch && s_engine.state.release_pending);
    expect_empty_intents();
    assert(step() == HID_ENGINE_STEP_BLOCKED && trace_count == 1);
    test_usb.ready = true;
    active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    sync_interaction();
    release_barrier();
    assert(trace_count == 3 && trace[1].transport == APP_INPUT_USB && trace[2].transport == APP_INPUT_USB);
    expect_idle();
    service_usb_selection();
    active = input_link_snapshot(&test_link);
    observe_input_link(&active, &test_previous_link);
    assert(test_usb_enable_count == 1 && !s_send_token && test_ble_send_attempts == 0);
    assert(strcmp(s_interaction.view.preview, "BAB") == 0);
    action(APP_KEY, 5);
    const uint8_t fresh[] = {0, 0, 5, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    expect_report(1, fresh, sizeof fresh);
    expect_report(1, zero, sizeof zero);
    assert(trace_count == 5 && test_storage_requests == 0);
}

static void mark_ble_admission_in_flight(void) { test_link.congested = true; }
static void mark_usb_admission_in_flight(void) { test_usb.congested = true; }
static void fail_bluetooth_target_after_usb_admission(void)
{
    test_link.failed = true;
    test_link.ready = false;
}

static void final_release_admission_refreshes_transport_completion_evidence(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_KEYBOARD, from);
        settings_page(APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        static const uint8_t zero[8] = {0};
        expect_report(1, zero, 8);
        const input_link_t before_final_send = input_link_snapshot(&test_link);
        assert(!before_final_send.congested);
        if (from == APP_INPUT_USB) test_usb_after_accept = mark_usb_admission_in_flight;
        else test_ble_after_accept = mark_ble_admission_in_flight;
        expect_report(2, zero, 4);
        assert(!s_engine.state.release_pending);
        assert(from == APP_INPUT_USB ? test_usb.congested : test_link.congested);
        service_transport_switch();
        assert(s_transport_switch_pending && s_input_transport == from);
        assert(test_usb_enable_count == 0 && test_storage_requests == 0);
        if (from == APP_INPUT_USB) test_usb.congested = false;
        else test_link.congested = false;
        service_transport_switch();
        assert(!s_transport_switch_pending && s_input_transport == to);
        assert(test_usb_enable_count == 1 && test_storage_requests == 1);
    }
}

static void ble_target_failure_after_final_usb_release_aborts_selection(void)
{
    transport_setup(INPUT_MOUSE, APP_INPUT_USB);
    settings_page(APP_SETTINGS_CONNECTIVITY);
    action(APP_TRANSPORT_SELECT, APP_INPUT_BLUETOOTH);
    static const uint8_t zero[8] = {0};
    expect_report(1, zero, 8);
    service_transport_switch();
    assert(s_transport_switch_pending); /* Establish a valid origin before injecting target failure. */
    const ble_hid_snapshot_t before_final_send = test_link;
    assert(before_final_send.initialized && !before_final_send.failed);
    test_usb_after_accept = fail_bluetooth_target_after_usb_admission;
    expect_report(2, zero, 4);
    service_transport_switch();
    assert(!s_transport_switch_pending && s_input_transport == APP_INPUT_USB);
    assert(s_requested_transport == APP_INPUT_USB && test_usb.enabled);
    assert(test_usb_enable_count == 0 && test_storage_requests == 0);
}

static void pairing_disconnect_waits_for_fresh_final_release_completion(void)
{
    setup(INPUT_KEYBOARD);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    release_barrier();
    action(APP_CONFIRM, 1);
    static const uint8_t zero[8] = {0};
    expect_report(1, zero, 8);
    const ble_hid_snapshot_t before_final_send = test_link;
    assert(!before_final_send.congested);
    test_disconnect_accept = true;
    test_ble_after_accept = mark_ble_admission_in_flight;
    expect_report(2, zero, 4);
    assert(!s_engine.state.release_pending && test_link.congested);
    service_pair_operation(&before_final_send);
    assert(test_disconnects == 0 && s_pair_operation == APP_CONFIRM_DISCONNECT);
    test_link.congested = false;
    service_pair_operation(&before_final_send);
    assert(test_disconnects == 1 && test_disconnect_trace_count == trace_count);
    service_pair_operation(&before_final_send);
    assert(test_disconnects == 1 && test_pair_forgets == 0);
}

static void confirmed_disconnect_cannot_retarget_a_replacement_peer(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    release_barrier();
    action(APP_CONFIRM, 1);
    release_barrier();
    const ble_hid_snapshot_t original = test_link;
    ++test_link.generation;
    test_disconnect_accept = true;
    service_pair_operation(&original);
    assert(test_disconnects == 0); /* Confirmation authorized the original current peer only. */
    assert(s_pair_operation == APP_CONFIRM_NONE);
}

static void origin_disconnect_allows_explicit_switch_without_release_claim(void)
{
    for (int old = APP_INPUT_BLUETOOTH; old <= APP_INPUT_USB; ++old) {
        const app_input_transport_t from = (app_input_transport_t)old;
        const app_input_transport_t to = from == APP_INPUT_USB ? APP_INPUT_BLUETOOTH : APP_INPUT_USB;
        transport_setup(INPUT_MOUSE, from);
        settings_page(APP_SETTINGS_CONNECTIVITY);
        action(APP_TRANSPORT_SELECT, (int)to);
        service_transport_switch();
        assert(s_transport_switch_pending);
        const size_t before = trace_count;
        if (from == APP_INPUT_USB) {
            test_usb.mounted = test_usb.ready = false;
            ++test_usb.generation;
        } else {
            test_link.connection_admitted = test_link.connected = test_link.ready = false;
            ++test_link.generation;
        }
        service_transport_switch();
        assert(!s_transport_switch_pending && s_input_transport == to);
        assert(test_storage_requests == 1 && trace_count == before);
        assert(strstr(s_message, "disconnected") != NULL);
    }
}

static void start_confirmed_pair_change(app_pairing_operation_t operation)
{
    setup(INPUT_KEYBOARD);
    fill_preview(2);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_PAIRING_REQUEST, (int)operation);
    release_barrier();
    action(APP_CONFIRM, 1);
    release_barrier();
    assert(s_pair_operation != APP_CONFIRM_NONE && !s_pair_disconnect_requested);
    test_disconnect_accept = test_pair_accept = true;
}

static void assert_pair_change_completion(app_pairing_operation_t operation, bool completed)
{
    assert(s_pair_operation == APP_CONFIRM_NONE);
    assert(test_pair_opens == (completed && operation == APP_PAIR_OPEN ? 1U : 0U));
    assert(test_pair_forgets == (completed && operation == APP_PAIR_FORGET_ALL ? 1U : 0U));
    assert(test_disconnects == 1 && test_storage_requests == 0 && test_wifi_forgets == 0);
    assert(strcmp(s_interaction.view.preview, "AB") == 0);
}

static void pairing_admission_generation_change_waits_for_actual_link_down(void)
{
    /* Add/ForgetAll now use the native management owner. Their corresponding
     * generation/replacement cases live in ble_hid/native_management_cases.inc
     * with the actual BLE worker; this path still owns Disconnect. */
    const app_pairing_operation_t operations[] = {APP_PAIR_DISCONNECT};
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        start_confirmed_pair_change(operations[i]);
        const ble_hid_snapshot_t original = test_link;
        const app_confirmation_t pending = s_pair_operation;
        const size_t reports = trace_count;
        service_pair_operation(&original);
        assert(test_disconnects == 1 && test_disconnect_generation == original.generation);
        assert(test_link.generation == original.generation + 1 && test_link.connected && !test_link.ready);
        assert(s_pair_disconnect_requested && s_pair_operation == pending);
        input_link_t active = input_link_snapshot(&test_link);
        observe_input_link(&active, &test_previous_link);
        assert(!s_engine.ready);
        for (unsigned poll = 0; poll < 50; ++poll) {
            s_now_ms += 10;
            service_pair_operation(&original);
            assert(s_pair_operation == pending && test_disconnects == 1);
            assert(test_pair_opens == 0 && test_pair_forgets == 0);
        }
        assert(trace_count == reports);
        test_link.connection_admitted = test_link.connected = false;
        ++test_link.generation;
        active = input_link_snapshot(&test_link);
        observe_input_link(&active, &test_previous_link);
        service_pair_operation(&original);
        assert_pair_change_completion(operations[i], true);
        if (operations[i] == APP_PAIR_FORGET_ALL) assert(test_forget_generation == test_link.generation);
        service_pair_operation(&original);
        assert_pair_change_completion(operations[i], true);
    }
}

static void pairing_disconnect_replacement_before_acceptance_aborts(void)
{
    const app_pairing_operation_t operations[] = {APP_PAIR_DISCONNECT};
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        start_confirmed_pair_change(operations[i]);
        const ble_hid_snapshot_t original = test_link;
        ++test_link.generation;
        service_pair_operation(&original);
        assert(s_pair_operation == APP_CONFIRM_NONE && !s_pair_disconnect_requested);
        assert(test_disconnects == 0 && test_pair_opens == 0 && test_pair_forgets == 0);
        test_link.connection_admitted = test_link.connected = test_link.ready = false;
        ++test_link.generation;
        service_pair_operation(&original);
        assert(test_disconnects == 0 && test_pair_opens == 0 && test_pair_forgets == 0);
        assert(test_storage_requests == 0 && strcmp(s_interaction.view.preview, "AB") == 0);
    }
}

static void pairing_disconnect_replacement_after_acceptance_never_issues_another_disconnect(void)
{
    const app_pairing_operation_t operations[] = {APP_PAIR_DISCONNECT};
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        for (unsigned link_down = 0; link_down < 2; ++link_down) {
            start_confirmed_pair_change(operations[i]);
            const ble_hid_snapshot_t original = test_link;
            service_pair_operation(&original);
            assert(s_pair_disconnect_requested && test_disconnects == 1);
            ++test_link.generation; /* Replacement beyond our own admission-induced increment. */
            test_link.connection_admitted = test_link.ready = link_down != 0;
            test_link.peer_known = true;
            test_link.peer_address[0] = 2;
            test_link.peer_address[5] = 99;
            input_link_t active = input_link_snapshot(&test_link);
            observe_input_link(&active, &test_previous_link);
            s_now_ms = 5999;
            service_pair_operation(&original);
            assert(s_pair_operation == APP_CONFIRM_NONE && test_disconnects == 1);
            assert(test_pair_opens == 0 && test_pair_forgets == 0);
            if (link_down) {
                test_link.connection_admitted = test_link.connected = test_link.ready = false;
                ++test_link.generation;
            } else {
                s_now_ms = 6000;
            }
            service_pair_operation(&original);
            assert_pair_change_completion(operations[i], true);
            test_link.connection_admitted = test_link.connected = test_link.ready = false;
            ++test_link.generation;
            service_pair_operation(&original);
            assert_pair_change_completion(operations[i], true);
        }
    }
}

static void allocation_diagnostics_retain_only_first_complete_failure(void)
{
    setup(INPUT_MOUSE);
    test_diagnostic_logs_enabled = true;
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 0);
    record_allocation_failure(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, "synthetic_dma_alloc");
    for (unsigned i = 0; i < 1000; ++i)
        record_allocation_failure(8192, MALLOC_CAP_8BIT, "later_alloc");
    /* The allocator callback neither logs nor queries the allocator again. */
    assert(test_diagnostic_log_count == 0 && test_heap_observation_count == 0);
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 1);
    assert(strstr(test_diagnostic_logs[0], "size=4096 caps=0x00000808 function=synthetic_dma_alloc"));
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 2);
    assert(strcmp(test_diagnostic_logs[0], test_diagnostic_logs[1]) == 0);
    assert(test_heap_observation_count == 0);

    setup(INPUT_MOUSE);
    test_diagnostic_logs_enabled = true;
    record_allocation_failure(17, MALLOC_CAP_8BIT, NULL);
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 1);
    assert(strstr(test_diagnostic_logs[0], "size=17 caps=0x00000004 function=unknown"));
}

static void allocation_diagnostics_ignore_an_unpublished_record(void)
{
    setup(INPUT_MOUSE);
    test_diagnostic_logs_enabled = true;
    /* Model a writer paused after reservation, with only some fields written.
     * This exercises the reader gate, not real concurrent scheduling. */
    s_allocation_failure_size = 123;
    s_allocation_failure_function = "unpublished";
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 0);
    atomic_store_explicit(&s_allocation_failure_state, 1, memory_order_relaxed);
    record_allocation_failure(8192, MALLOC_CAP_DMA, "competing_alloc");
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 0);
    assert(atomic_load(&s_allocation_failure_state) == 1);
    assert(s_allocation_failure_size == 123 && s_allocation_failure_caps == 0);
    assert(strcmp(s_allocation_failure_function, "unpublished") == 0);
    assert(test_heap_observation_count == 0);
}

static void boot_memory_diagnostics_preserve_bounded_capability_history(void)
{
    static const char *const stages[] = {"entry", "usb", "board", "bluetooth", "wifi", "media", "ui", "remote"};
    setup(INPUT_MOUSE);
    test_heap_observation_enabled = true;
    for (size_t i = 0; i < 8; ++i) {
        test_heap_free_internal = 80000 - i * 1000;
        test_heap_largest_internal = 50000 - i * 1000;
        test_heap_free_dma = 30000 - i * 1000;
        test_heap_largest_dma = 10000 - i * 1000;
        record_boot_memory(stages[i]);
    }
    assert(test_heap_observation_count == 32);
    /* Extra stages must be discarded before even querying heap state. */
    test_heap_observation_enabled = false;
    for (unsigned i = 0; i < 1000; ++i) record_boot_memory("overflow");
    assert(s_boot_memory_count == 8 && test_heap_observation_count == 32);
    test_diagnostic_logs_enabled = true;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        test_diagnostic_log_count = 0;
        report_memory_diagnostics();
        assert(test_diagnostic_log_count == 8);
        for (size_t i = 0; i < 8; ++i) {
            char expected[160];
            int length = snprintf(expected, sizeof expected,
                "stage=%s free_internal=%zu largest_internal=%zu free_dma=%zu largest_dma=%zu",
                stages[i], 80000 - i * 1000, 50000 - i * 1000, 30000 - i * 1000, 10000 - i * 1000);
            assert(length > 0 && (size_t)length < sizeof expected);
            assert(strstr(test_diagnostic_logs[i], expected));
        }
        assert(s_boot_memory_count == 8 && test_heap_observation_count == 32);
    }
    record_allocation_failure(1024, MALLOC_CAP_DMA, "after_boot");
    test_diagnostic_log_count = 0;
    report_memory_diagnostics();
    assert(test_diagnostic_log_count == 9);
    assert(strstr(test_diagnostic_logs[8], "size=1024 caps=0x00000008 function=after_boot"));
}

static void runtime_diagnostics_keep_unknowns_private_and_observation_only(void)
{
    setup(INPUT_MOUSE);
    test_diagnostic_logs_enabled = true;
    test_wifi.initialized = test_wifi.lan_ready = true;
    test_wifi.backend_available = false;
    test_wifi.backend_error = ESP_FAIL;
    (void)snprintf(test_wifi.ssid, sizeof test_wifi.ssid, "%s", "PRIVATE_NETWORK");
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true,
        .speech_known = true, .busy = true, .result_ready = true, .result = AE_BUSY};
    test_media_snapshot.audio.error = AE_UNAVAILABLE;
    test_media_snapshot.speech.result = AE_UNAVAILABLE;
    test_control_runtime.initialized = true;
    test_control_runtime.error = ESP_OK;
    test_control_runtime.server.state = APP_CONTROL_ERROR;
    test_control_runtime.server.error = ESP_FAIL;
    test_control_runtime.trust.known = true;
    test_control_runtime.trust.present = false;
    test_control_runtime.trust.uncertain = true;
    (void)snprintf(test_control_runtime.server.address, sizeof test_control_runtime.server.address,
                   "%s", "PRIVATE_LISTENER");
    (void)snprintf(test_control_runtime.trust.active_digest, sizeof test_control_runtime.trust.active_digest,
                   "%s", "PRIVATE_IDENTITY");
    app_intent_t intent = {.action = APP_THEME, .x = 1};
    assert(submit_intent(&intent));
    const hid_engine_t engine = s_engine;
    const interaction_t interaction = s_interaction;
    const device_settings_t settings = s_settings;
    const unsigned queued = uxQueueMessagesWaiting(s_intents);
    const unsigned priority = s_priority_requests;
    const uint64_t revision = s_ui_revision;
    report_runtime_diagnostics();
    assert(test_diagnostic_log_count == 5);
    assert(strstr(test_diagnostic_logs[0], "initialized=1"));
    assert(strstr(test_diagnostic_logs[0], "lan_ready=1 backend_available=0"));
    assert(strstr(test_diagnostic_logs[1], "snapshot_available=1 initialized=1 audio_known=1"));
    assert(strstr(test_diagnostic_logs[1], "off_confirmed=0 speech_known=1"));
    char expected_error[48];
    (void)snprintf(expected_error, sizeof expected_error, "backend_error=%d", (int)ESP_FAIL);
    assert(strstr(test_diagnostic_logs[0], expected_error));
    (void)snprintf(expected_error, sizeof expected_error, "audio_error=%d", (int)AE_UNAVAILABLE);
    assert(strstr(test_diagnostic_logs[1], expected_error));
    (void)snprintf(expected_error, sizeof expected_error, "speech_result=%d", (int)AE_UNAVAILABLE);
    assert(strstr(test_diagnostic_logs[1], expected_error));
    (void)snprintf(expected_error, sizeof expected_error, "server_error=%d", (int)ESP_FAIL);
    assert(strstr(test_diagnostic_logs[2], expected_error));
    assert(strstr(test_diagnostic_logs[2], "trust_known=1 trust_present=0 trust_uncertain=1 clock_known=0"));
    for (size_t i = 0; i < 3; ++i) assert(!strstr(test_diagnostic_logs[i], "PRIVATE_"));
    for (size_t i = 0; i < sizeof s_runtime_diagnostics; ++i)
        assert(((const uint8_t *)&s_runtime_diagnostics)[i] == 0);

    /* A missing snapshot does not write its output. Previous ready flags and
     * result bytes must not leak into a later unavailable observation. */
    test_media_snapshot_unavailable = true;
    test_wifi.initialized = test_wifi.lan_ready = false;
    test_control_runtime = (app_control_runtime_snapshot_t){.error = ESP_ERR_INVALID_STATE};
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
        test_diagnostic_log_count = 0;
        report_runtime_diagnostics();
        assert(test_diagnostic_log_count == 5);
        assert(strstr(test_diagnostic_logs[0], "initialized=0 state=-1 lan_ready=0"));
        assert(strstr(test_diagnostic_logs[1], "snapshot_available=0 initialized=0 audio_known=0 audio_state=-1 audio_error=-1"));
        assert(strstr(test_diagnostic_logs[1], "off_confirmed=0 speech_known=0 speech_state=-1 speech_result=-1 busy=0 result_known=0 result=-1"));
        assert(strstr(test_diagnostic_logs[2], "initialized=0 server_state=-1"));
        for (size_t i = 0; i < sizeof s_runtime_diagnostics; ++i)
            assert(((const uint8_t *)&s_runtime_diagnostics)[i] == 0);
    }
    assert(memcmp(&s_engine, &engine, sizeof engine) == 0);
    assert(memcmp(&s_interaction, &interaction, sizeof interaction) == 0);
    assert(memcmp(&s_settings, &settings, sizeof settings) == 0);
    assert(uxQueueMessagesWaiting(s_intents) == queued && s_priority_requests == priority);
    assert(s_ui_revision == revision && test_heap_observation_count == 0);
}

static void motion_and_scan_diagnostics_are_bounded_observations(void)
{
    setup(INPUT_MOUSE); calibrate(); action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0}; expect_report(2, moved, sizeof moved);
    const uint32_t processed = s_motion_trace.processed;
    process_motion();
    assert(s_motion_trace.processed == processed && s_motion_trace.duplicates == 1);
    sample_after(100, 25, 0); expect_idle();
    assert(s_motion_trace.gaps == 1 && s_motion_trace.last_gap_us == 100000);
    assert(s_motion_trace.first_active_gap_known && s_motion_trace.first_active_gap_us == 100000);
    assert(s_motion_trace.max_dt_us == 100000 && s_motion_trace.sample_blocks == 1);
    assert(s_motion_trace.minimum[2] == 0 && s_motion_trace.maximum[2] == 25000);
    const hid_engine_t engine = s_engine;
    const interaction_t interaction = s_interaction;
    const uint64_t last_sample = s_last_imu_us;
    input_link_t link = input_link_snapshot(&test_link);
    test_diagnostic_logs_enabled = true;
    report_motion_diagnostics(&link);
    assert(test_diagnostic_log_count == 4);
    assert(strstr(test_diagnostic_logs[0], "engine_motion=1 model_motion=1"));
    assert(strstr(test_diagnostic_logs[0], "consumer_gaps=1"));
    assert(strstr(test_diagnostic_logs[0], "last_gap_us=100000"));
    assert(strstr(test_diagnostic_logs[1], "raw_known=1"));
    assert(strstr(test_diagnostic_logs[1], "gz_max_mdps=25000"));
    assert(strstr(test_diagnostic_logs[2], "first_known=1 reason=6 active_losses=1"));
    assert(strstr(test_diagnostic_logs[3], "kind=terminal sample_known=1"));
    assert(memcmp(&engine, &s_engine, sizeof engine) == 0);
    assert(memcmp(&interaction, &s_interaction, sizeof interaction) == 0 && s_last_imu_us == last_sample);
    assert(!s_motion_trace.range_known && !s_motion_trace.window_samples);
    assert(s_motion_trace.gaps == 1 && s_motion_trace.processed == processed + 1);

    test_diagnostic_log_count = 0;
    report_runtime_diagnostics();
    assert(test_diagnostic_log_count == 5 && strstr(test_diagnostic_logs[3], "current known=0"));
    assert(strstr(test_diagnostic_logs[4], "last_failure known=0"));
    test_scan_diagnostics.current = (app_wifi_scan_observation_t){.known = true, .generation = 4,
        .phase = APP_WIFI_SCAN_PHASE_COMPLETE, .error = ESP_OK, .heap_known = true,
        .free_internal = 30000, .largest_internal = 16000, .driver_admitted = true,
        .count_known = true, .driver_count = 3, .read_count = 3};
    test_scan_diagnostics.last_failure = (app_wifi_scan_observation_t){.known = true, .generation = 3,
        .phase = APP_WIFI_SCAN_PHASE_SDK_START, .error = ESP_FAIL};
    test_diagnostic_log_count = 0;
    report_runtime_diagnostics();
    assert(strstr(test_diagnostic_logs[3], "generation=4 phase=9 error=0"));
    assert(strstr(test_diagnostic_logs[4], "generation=3 phase=3 error=-1"));
    assert(strstr(test_diagnostic_logs[4], "heap_known=0"));
    assert(!test_scans && !test_selections && !test_joins);
    for (size_t i = 0; i < sizeof s_runtime_diagnostics; ++i)
        assert(((const uint8_t *)&s_runtime_diagnostics)[i] == 0);

    /* Emit the actual producer formatting for the separate parser-only proof;
     * these synthetic snapshots do not claim device or physical evidence. */
    for (unsigned i = 0; i < 3; ++i) {
        test_update_imu = (board_imu_status_t){.available = true, .error = ESP_OK,
            .sample_count = 1000U + 1000U * i, .sample_age_known = true, .sample_age_us = 1000,
            .phase = BOARD_IMU_ACTIVE, .worker_running = true};
        test_diagnostic_log_count = 0;
        report_motion_diagnostics(&link);
        puts(test_diagnostic_logs[0]); puts(test_diagnostic_logs[1]);
    }
}

static void reference_incident_logs_are_exact_bounded_and_observation_only(void)
{
    setup(INPUT_MOUSE); calibrate(); action(APP_MOTION, 0);
    if (s_engine.state.gyro_enabled && !s_interaction.gravity_ready) settle_motion_reference();
    for (unsigned i = 0; i < 26; ++i) {
        s_now_ms += 10;
        test_sample = (board_imu_sample_t){.timestamp_us = s_now_ms * 1000U, .z_g = 1.2f};
        sync_interaction(); process_motion(); expect_idle();
    }
    assert(s_reference_diagnostics.first_active.reason == INTERACTION_REFERENCE_TRUST_AGE);
    const interaction_reference_diagnostics_t diagnostics = s_reference_diagnostics;
    const interaction_t model = s_interaction;
    const hid_engine_t engine = s_engine;
    input_link_t link = input_link_snapshot(&test_link);
    test_diagnostic_logs_enabled = true;
    report_motion_diagnostics(&link);
    assert(test_diagnostic_log_count == 5);
    assert(strstr(test_diagnostic_logs[2], "first_known=1 reason=12 active_losses=1"));
    assert(strstr(test_diagnostic_logs[2], "rejected=26 near_one=26 gentle=1 innovation=0"));
    assert(strstr(test_diagnostic_logs[3], "kind=first_reject sample_known=1"));
    assert(strstr(test_diagnostic_logs[4], "kind=terminal sample_known=1"));
    unsigned bits[6];
    const char *sample_bits = strstr(test_diagnostic_logs[3], "sample_bits=");
    assert(sample_bits && sscanf(sample_bits, "sample_bits=%x,%x,%x,%x,%x,%x",
        &bits[0], &bits[1], &bits[2], &bits[3], &bits[4], &bits[5]) == 6);
    assert(bits[0] == 0 && bits[5] == reference_float_bits(1.2f));
    assert(memcmp(&diagnostics, &s_reference_diagnostics, sizeof diagnostics) == 0);
    assert(memcmp(&model, &s_interaction, sizeof model) == 0);
    assert(memcmp(&engine, &s_engine, sizeof engine) == 0);
    char frozen[3][1024];
    for (size_t i = 0; i < 3; ++i) {
        assert(strlen(test_diagnostic_logs[i + 2]) < sizeof frozen[i]);
        strcpy(frozen[i], test_diagnostic_logs[i + 2]);
    }
    test_diagnostic_log_count = 0;
    report_motion_diagnostics(&link);
    for (size_t i = 0; i < 3; ++i) assert(strcmp(frozen[i], test_diagnostic_logs[i + 2]) == 0);
    /* Worst-width timestamps and nonfinite bit patterns remain bounded data. */
    s_reference_diagnostics.first_active.terminal.sample.timestamp_us = UINT64_MAX;
    s_reference_diagnostics.first_active.terminal.previous.timestamp_us = UINT64_MAX;
    s_reference_diagnostics.first_active.terminal.trusted_us = UINT64_MAX;
    s_reference_diagnostics.first_active.terminal.now_ms = UINT64_MAX;
    s_reference_diagnostics.first_active.terminal.sample.gx_dps = NAN;
    test_diagnostic_log_count = 0;
    report_motion_diagnostics(&link);
    for (size_t i = 2; i < 5; ++i) assert(strlen(test_diagnostic_logs[i]) < 1024);
    interaction_reference_diagnostics_bind(&s_interaction, NULL);
    test_diagnostic_log_count = 0;
    report_motion_diagnostics(&link);
    assert(test_diagnostic_log_count == 3 && strcmp(test_diagnostic_logs[2], "MotionRef attached=0") == 0);
}

static void native_recenter_waits_for_current_grip_and_keeps_intent_stoppable(void)
{
    setup(INPUT_MOUSE); calibrate();
    action(APP_MOTION, 0);
    assert(s_engine.state.gyro_enabled && !s_interaction.gravity_ready);
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_enabled && test_view.motion_settings.reference == APP_REFERENCE_ACQUIRING);
    for (unsigned i = 0; i < 30; ++i) { sample_after(10, 25, 0); expect_idle(); }
    assert(!s_interaction.gravity_ready);
    action(APP_MOTION, 0);
    assert(!s_engine.state.gyro_enabled);
    action(APP_MOTION, 0); settle_motion_reference();
    const uint64_t epoch = s_engine.state.epoch;
    const interaction_config_t config = s_interaction.config;
    float bias[3]; memcpy(bias, s_interaction.bias, sizeof bias);
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_recenter_available && test_view.motion_settings.reference == APP_REFERENCE_READY);
    /* Production UI emits control BEGIN, END, then the press-bound action. */
    app_intent_t control_touch = make_intent(APP_TOUCH_BEGIN, 0, 0, NULL);
    control_touch.gesture = 901; control_touch.surface = APP_SURFACE_CONTROL;
    deliver(control_touch);
    control_touch.action = APP_TOUCH_END; deliver(control_touch);
    control_touch.action = APP_RECENTER; deliver(control_touch);
    assert(s_reference_diagnostics.recenter_resets == 1 && !s_reference_diagnostics.first_active.known);
    assert(s_engine.state.epoch == epoch && s_engine.state.gyro_enabled && s_interaction.view.motion_enabled);
    assert(memcmp(bias, s_interaction.bias, sizeof bias) == 0);
    assert(memcmp(&config, &s_interaction.config, sizeof config) == 0);
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_enabled && test_view.motion_settings.reference == APP_REFERENCE_ACQUIRING);
    settle_motion_reference();
    /* Stable acquisition must not bypass the control tap's release freeze. */
    for (unsigned i = 0; i < 3; ++i) { sample_after(10, 0, 0); expect_idle(); }
    assert(s_now_ms >= s_interaction.freeze_until_ms);
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0}; expect_report(2, moved, sizeof moved);
    app_intent_t stale = make_intent(APP_RECENTER, 0, 0, NULL);
    ++s_ui_revision; deliver(stale);
    assert(s_reference_diagnostics.recenter_resets == 1 && s_interaction.gravity_ready);
    action(APP_MENU, APP_OVERLAY_SETTINGS); action(APP_RECENTER, 0);
    assert(s_reference_diagnostics.recenter_resets == 1);
}

static void recenter_rechecks_busy_input_without_purging_or_false_availability(void)
{
    for (unsigned fault = 0; fault < 10; ++fault) {
        setup(INPUT_MOUSE); calibrate(); action(APP_MOTION, 0); settle_motion_reference();
        if (fault == 0) assert(hid_engine_enqueue_mouse_move(&s_engine, s_engine.state.epoch,
            INPUT_LOCAL, 1, 0) == HID_ENGINE_ENQUEUE_OK);
        if (fault == 1) s_engine.mouse_buttons = 1;
        if (fault == 2) s_drag_requested = true;
        if (fault == 3) s_interaction.touch.active = true;
        if (fault == 4) s_touch_down = true;
        if (fault == 5) s_physical_down = true;
        if (fault == 6) s_engine.state.release_pending = true;
        if (fault == 7) s_engine.congested = true;
        if (fault == 8) s_engine.ready = false;
        if (fault == 9) test_capabilities.imu = false;
        sync_interaction();
        const hid_engine_t engine = s_engine;
        const interaction_t model = s_interaction;
        const interaction_reference_diagnostics_t diagnostics = s_reference_diagnostics;
        if (fault == 0) {
            render_view(&test_link, ESP_OK);
            assert(test_view.motion_recenter_available); /* Queue is checked on admission. */
        }
        assert(recenter_motion(INPUT_LOCAL) != CP_OK);
        assert(memcmp(&engine, &s_engine, sizeof engine) == 0);
        assert(memcmp(&model, &s_interaction, sizeof model) == 0);
        assert(memcmp(&diagnostics, &s_reference_diagnostics, sizeof diagnostics) == 0);
        assert(!s_reference_diagnostics.recenter_resets);
    }
}

static void imu_state_observes_actual_grip_transitions_and_bounds_its_response(void)
{
    setup(INPUT_MOUSE); calibrate(); action(APP_MOTION, 0);
    test_update_imu = (board_imu_status_t){.available = true, .error = ESP_OK,
        .sample_count = 1000, .sample_age_known = true, .sample_age_us = 1000};
    char json[512]; size_t length = 0;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", json, sizeof json, &length) == CP_OK);
    assert(length == strlen(json) && strstr(json, "\"reference_state\":\"acquiring\""));
    assert(strstr(json, "\"motion_requested\":true,\"input_ready\":true,\"output_ready\":true"));
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_settings.reference == APP_REFERENCE_ACQUIRING);
    settle_motion_reference();
    const interaction_t ready_model = s_interaction;
    const hid_engine_t ready_engine = s_engine;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", json, sizeof json, &length) == CP_OK);
    assert(strstr(json, "\"reference_state\":\"ready\""));
    assert(memcmp(&ready_model, &s_interaction, sizeof ready_model) == 0);
    assert(memcmp(&ready_engine, &s_engine, sizeof ready_engine) == 0);
    s_engine.ready = false; sync_interaction();
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", json, sizeof json, &length) == CP_OK);
    assert(strstr(json, "\"input_ready\":false,\"output_ready\":false,\"reference_state\":\"ready\""));
    /* Existing orientation and route readiness are independently observed. */
    s_interaction.geometry_ready = false;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", json, sizeof json, &length) == CP_OK);
    assert(strstr(json, "\"reference_state\":\"unstable\""));
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_settings.reference == APP_REFERENCE_UNSTABLE);
    test_capabilities.imu = false; test_update_imu.available = false;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", json, sizeof json, &length) == CP_OK);
    assert(strstr(json, "\"available\":false") && strstr(json, "\"reference_state\":\"unavailable\""));
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_settings.reference == APP_REFERENCE_UNAVAILABLE);
    test_capabilities.imu = true; s_interaction.clock_seen = false;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", json, sizeof json, &length) == CP_OK);
    assert(strstr(json, "\"reference_state\":\"unknown\""));
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_settings.reference == APP_REFERENCE_UNKNOWN);
    char small[8] = "guard"; length = 99;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", small, sizeof small, &length) == CP_TOO_LARGE);
    assert(!length && !small[0]);
    small[0] = 'x'; length = 99;
    assert(remote_device_board_read(NULL, NULL, "/api/v1/imu", small, 0, &length) == CP_TOO_LARGE);
    assert(!length && small[0] == 'x');
}

static void expect_neutral_navigation_trace(void)
{
    for (size_t i = 0; i < trace_count; ++i)
        for (size_t byte = 0; byte < trace[i].length; ++byte)
            assert(trace[i].bytes[byte] == 0);
    assert(test_storage_requests == 0 && test_pair_opens == 0 &&
        test_pair_forgets == 0 && test_disconnects == 0 && test_wifi_forgets == 0);
}

static void navigation_with_draft(input_mode_t mode)
{
    setup(INPUT_KEYBOARD);
    action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_MOTION);
    release_barrier(); sync_interaction();
    trace_count = 0;
    fill_preview(4);
    assert(strcmp(s_interaction.view.preview, "ABAB") == 0);
    if (mode != INPUT_KEYBOARD) {
        action(APP_MODE, (int)mode);
        release_barrier();
        sync_interaction();
    }
    assert(s_engine.state.mode == mode);
}

static void physical_double_click(void)
{
    const uint64_t start = s_now_ms + 10;
    button_edge(true, start);
    button_edge(false, start + 20);
    button_edge(true, start + 100);
    button_edge(false, start + 120);
    buttons_at(start + 500); /* A later poll must not produce a delayed single. */
}

static void quick_status_settings_back_preserves_primary_context(void)
{
    navigation_with_draft(INPUT_KEYBOARD);
    action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_TOUCH);
    release_barrier();
    sync_interaction();
    action(APP_MENU, APP_OVERLAY_MODES);
    release_barrier();
    sync_interaction();
    assert(s_overlay == APP_OVERLAY_MODES && s_interaction.view.menu_open);
    action(APP_MENU, APP_OVERLAY_TARGET);
    release_barrier();
    sync_interaction();
    render_view(&test_link, ESP_OK);
    assert(test_view.overlay == APP_OVERLAY_TARGET);
    app_intent_t stale_back = make_intent(APP_MENU, APP_OVERLAY_MODES, 0, NULL);
    action(APP_MENU, APP_OVERLAY_MODES);
    release_barrier();
    sync_interaction();
    assert(s_overlay == APP_OVERLAY_MODES);
    action(APP_MENU, APP_OVERLAY_SETTINGS);
    release_barrier();
    sync_interaction();
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_HOME);
    deliver(stale_back);
    assert(s_overlay == APP_OVERLAY_SETTINGS);
    settings_page(APP_SETTINGS_INPUT);
    settings_page(APP_SETTINGS_HOME);
    action(APP_MENU, APP_OVERLAY_MODES);
    release_barrier();
    sync_interaction();
    assert(s_overlay == APP_OVERLAY_MODES);
    action(APP_CLOSE, 0);
    assert(s_overlay == APP_OVERLAY_NONE && !s_interaction.view.menu_open);
    assert(s_engine.state.mode == INPUT_KEYBOARD && s_keyboard_submode == APP_KEYBOARD_TOUCH);
    assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
    expect_idle();
    expect_neutral_navigation_trace();
}

static void chooser_entry_rejects_wrong_mode_and_invalid_payload_without_cancelling_send(void)
{
    const int invalid[] = {-1, APP_OVERLAY_NONE, APP_OVERLAY_WIFI_SETUP, APP_OVERLAY_CONFIRM, 999};
    for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        setup((input_mode_t)mode);
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
            action(APP_MENU, invalid[i]);
        if (mode != INPUT_KEYBOARD) action(APP_MENU, APP_OVERLAY_KEYBOARD);
        if (mode != INPUT_AUDIO) action(APP_MENU, APP_OVERLAY_AUDIO);
        assert(s_overlay == APP_OVERLAY_NONE && s_engine.state.mode == (input_mode_t)mode);
        assert(s_engine.state.epoch == epoch && s_ui_revision == revision);
        expect_idle();
        assert(trace_count == 0);
    }
    navigation_with_draft(INPUT_KEYBOARD);
    action(APP_SEND, 0);
    const uint64_t token = s_send_token, epoch = s_engine.state.epoch, revision = s_ui_revision;
    assert(token && s_engine.count != 0);
    action(APP_MENU, APP_OVERLAY_AUDIO);
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        action(APP_MENU, invalid[i]);
    assert(s_send_token == token && s_interaction.view.sending && s_engine.count != 0);
    assert(s_engine.state.epoch == epoch && s_ui_revision == revision && trace_count == 0);
    static const uint8_t a_down[8] = {2, 0, 4, 0, 0, 0, 0, 0};
    expect_report(1, a_down, sizeof a_down);
}

static void chooser_entry_cannot_nest_behind_another_panel(void)
{
    const input_mode_t modes[] = {INPUT_KEYBOARD, INPUT_AUDIO};
    for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        const app_overlay_t chooser = modes[mode] == INPUT_KEYBOARD ?
            APP_OVERLAY_KEYBOARD : APP_OVERLAY_AUDIO;
        const app_overlay_t panels[] = {APP_OVERLAY_MODES, APP_OVERLAY_TARGET,
            APP_OVERLAY_SETTINGS, chooser, APP_OVERLAY_WIFI_SETUP};
        for (size_t panel = 0; panel < sizeof panels / sizeof panels[0]; ++panel) {
            navigation_with_draft(modes[mode]);
            if (panels[panel] == APP_OVERLAY_WIFI_SETUP) {
                settings_page(APP_SETTINGS_WIFI);
                test_wifi.initialized = true;
                test_wifi_start_error = ESP_OK;
                action(APP_WIFI_SETUP, 1);
            } else {
                action(APP_MENU, (int)panels[panel]);
            }
            release_barrier();
            sync_interaction();
            const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
            action(APP_MENU, APP_OVERLAY_KEYBOARD);
            action(APP_MENU, APP_OVERLAY_AUDIO);
            assert(s_overlay == panels[panel] && s_engine.state.mode == modes[mode]);
            assert(s_engine.state.epoch == epoch && s_ui_revision == revision);
            assert(strcmp(s_interaction.view.preview, "ABAB") == 0);
            assert(test_wifi_stops == 0);
            expect_idle();
            expect_neutral_navigation_trace();
        }
    }
}

static void choice_panels_reject_hidden_actions_without_reports_or_writes(void)
{
    const input_mode_t modes[] = {INPUT_KEYBOARD, INPUT_AUDIO};
    const struct { app_action_t kind; int x; } hidden[] = {
        {APP_GRID, 0}, {APP_SPACE, 0}, {APP_SEND, 0}, {APP_CLEAR, 0},
        {APP_DELETE, 0}, {APP_KEY, 4}, {APP_CLICK, 1}, {APP_MOVE, 5},
        {APP_SCROLL, 1}, {APP_MODE, INPUT_MOUSE}, {APP_MOTION, 0}, {APP_DRAG, 0},
        {APP_THEME, 0}, {APP_HAPTICS, 0}, {APP_HOST_OS, APP_HOST_COMMAND},
        {APP_SETTINGS_PAGE, APP_SETTINGS_INPUT}, {APP_WIFI_SETUP, 1},
        {APP_WIFI_FORGET_REQUEST, 0}, {APP_PAIRING_REQUEST, APP_PAIR_OPEN},
        {APP_CONFIRM, 1}, {APP_SHORTCUT, APP_SHORTCUT_ENTER}, {APP_VOICE_START, 0},
        {APP_VOICE_CONFIRM, 0}, {APP_AUDIO_POWER, 1}, {APP_AUDIO_MUTE, 0},
        {APP_AUDIO_VOLUME, 100}, {APP_RECOVER, 0}, {APP_BRIGHTNESS, 10},
        {APP_IDLE_TIMEOUT, 0}, {APP_CALIBRATE, 0}, {APP_CASE, 0}, {APP_SYMBOLS, 0}
    };
    for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        navigation_with_draft(modes[mode]);
        const app_overlay_t chooser = modes[mode] == INPUT_KEYBOARD ?
            APP_OVERLAY_KEYBOARD : APP_OVERLAY_AUDIO;
        action(APP_MENU, (int)chooser);
        release_barrier();
        sync_interaction();
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        const size_t reports = trace_count;
        for (size_t i = 0; i < sizeof hidden / sizeof hidden[0]; ++i)
            action(hidden[i].kind, hidden[i].x);
        deliver(make_intent(APP_TEXT, 0, 0, "X"));
        if (modes[mode] == INPUT_KEYBOARD) action(APP_AUDIO_KIND, APP_AUDIO_SPEAKER);
        else action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_TOUCH);
        assert(s_overlay == chooser && s_engine.state.mode == modes[mode]);
        assert(s_engine.state.epoch == epoch && s_ui_revision == revision);
        assert(s_keyboard_submode == APP_KEYBOARD_MOTION && s_audio_kind == APP_AUDIO_VOICE);
        assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
        assert(s_dark_theme && s_settings.haptics_enabled && s_host_os == APP_HOST_CTRL);
        assert(test_wifi_starts == 0 && test_wifi_stops == 0 && trace_count == reports);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void keyboard_choice_closes_preserves_draft_and_acknowledges_context(void)
{
    for (int choice = APP_KEYBOARD_MOTION; choice <= APP_KEYBOARD_SHORTCUTS; ++choice) {
        navigation_with_draft(INPUT_KEYBOARD);
        action(APP_GRID, 0);
        action(APP_MENU, APP_OVERLAY_KEYBOARD);
        release_barrier();
        sync_interaction();
        assert(s_overlay == APP_OVERLAY_KEYBOARD && s_interaction.view.menu_open);
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        app_intent_t stale = make_intent(APP_KEYBOARD_SUBMODE,
            choice == APP_KEYBOARD_TOUCH ? APP_KEYBOARD_MOTION : APP_KEYBOARD_TOUCH, 0, NULL);
        action(APP_KEYBOARD_SUBMODE, -1);
        action(APP_KEYBOARD_SUBMODE, 999);
        assert(s_overlay == APP_OVERLAY_KEYBOARD && s_ui_revision == revision);
        action(APP_KEYBOARD_SUBMODE, choice);
        assert(s_overlay == APP_OVERLAY_NONE && s_keyboard_submode == (app_keyboard_submode_t)choice);
        assert(s_engine.state.epoch > epoch && s_ui_revision > revision);
        assert(!s_interaction.view.menu_open && s_interaction.view.group == -1);
        assert(s_interaction.view.keyboard_motion == (choice == APP_KEYBOARD_MOTION));
        assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
        release_barrier();
        sync_interaction();
        deliver(stale);
        assert(s_keyboard_submode == (app_keyboard_submode_t)choice && s_overlay == APP_OVERLAY_NONE);
        render_view(&test_link, ESP_OK);
        assert(test_view.overlay == APP_OVERLAY_NONE && test_view.keyboard_submode == s_keyboard_submode);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void audio_choice_closes_and_stale_revision_cannot_select_or_navigate(void)
{
    for (int choice = APP_AUDIO_VOICE; choice <= APP_AUDIO_SPEAKER; ++choice) {
        navigation_with_draft(INPUT_AUDIO);
        action(APP_MENU, APP_OVERLAY_AUDIO);
        release_barrier();
        sync_interaction();
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        const int other = choice == APP_AUDIO_VOICE ? APP_AUDIO_SPEAKER : APP_AUDIO_VOICE;
        app_intent_t stale_choice = make_intent(APP_AUDIO_KIND, other, 0, NULL);
        app_intent_t stale_navigation = make_intent(APP_MENU, APP_OVERLAY_SETTINGS, 0, NULL);
        action(APP_AUDIO_KIND, -1);
        action(APP_AUDIO_KIND, 999);
        assert(s_overlay == APP_OVERLAY_AUDIO && s_ui_revision == revision);
        action(APP_AUDIO_KIND, choice);
        assert(s_overlay == APP_OVERLAY_NONE && s_audio_kind == (app_audio_kind_t)choice);
        assert(s_engine.state.epoch == epoch && s_ui_revision > revision);
        assert(!s_interaction.view.menu_open && strcmp(s_interaction.view.preview, "ABAB") == 0);
        deliver(stale_choice); /* Same HID epoch isolates stale UI revision handling. */
        deliver(stale_navigation);
        assert(s_overlay == APP_OVERLAY_NONE && s_audio_kind == (app_audio_kind_t)choice);
        render_view(&test_link, ESP_OK);
        assert(test_view.overlay == APP_OVERLAY_NONE && test_view.audio_kind == s_audio_kind);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void choice_panels_allow_safe_quick_status_settings_and_close(void)
{
    const input_mode_t modes[] = {INPUT_KEYBOARD, INPUT_AUDIO};
    const app_overlay_t destinations[] = {APP_OVERLAY_MODES, APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS};
    for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        for (size_t destination = 0;
             destination < sizeof destinations / sizeof destinations[0]; ++destination) {
            navigation_with_draft(modes[mode]);
            action(APP_MENU, modes[mode] == INPUT_KEYBOARD ? APP_OVERLAY_KEYBOARD : APP_OVERLAY_AUDIO);
            release_barrier();
            sync_interaction();
            const uint64_t revision = s_ui_revision;
            action(APP_MENU, (int)destinations[destination]);
            release_barrier();
            sync_interaction();
            assert(s_overlay == destinations[destination] && s_ui_revision > revision);
            if (s_overlay == APP_OVERLAY_SETTINGS) assert(s_settings_page == APP_SETTINGS_HOME);
            assert(s_engine.state.mode == modes[mode] && s_keyboard_submode == APP_KEYBOARD_MOTION);
            assert(s_audio_kind == APP_AUDIO_VOICE && strcmp(s_interaction.view.preview, "ABAB") == 0);
            action(APP_CLOSE, 0);
            assert(s_overlay == APP_OVERLAY_NONE && !s_interaction.view.menu_open);
            expect_idle();
            expect_neutral_navigation_trace();
        }
        navigation_with_draft(modes[mode]);
        action(APP_MENU, modes[mode] == INPUT_KEYBOARD ? APP_OVERLAY_KEYBOARD : APP_OVERLAY_AUDIO);
        release_barrier();
        sync_interaction();
        action(APP_CLOSE, 0);
        assert(s_overlay == APP_OVERLAY_NONE && !s_interaction.view.menu_open);
        assert(strcmp(s_interaction.view.preview, "ABAB") == 0);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void physical_double_click_toggles_shared_quick_panel_in_every_mode(void)
{
    for (int mode = INPUT_MOUSE; mode <= INPUT_AUDIO; ++mode) {
        navigation_with_draft((input_mode_t)mode);
        physical_double_click();
        assert(s_overlay == APP_OVERLAY_MODES && !s_engine.state.gyro_enabled);
        release_barrier();
        sync_interaction();
        physical_double_click();
        assert(s_overlay == APP_OVERLAY_NONE && !s_interaction.view.menu_open);
        release_barrier();
        sync_interaction();
        assert(s_engine.state.mode == (input_mode_t)mode && !s_engine.state.locked);
        assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void physical_double_click_returns_status_settings_and_choices_to_quick(void)
{
    const input_mode_t modes[] = {INPUT_KEYBOARD, INPUT_AUDIO};
    for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        const app_overlay_t panels[] = {APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS,
            modes[mode] == INPUT_KEYBOARD ? APP_OVERLAY_KEYBOARD : APP_OVERLAY_AUDIO};
        for (size_t panel = 0; panel < sizeof panels / sizeof panels[0]; ++panel) {
            navigation_with_draft(modes[mode]);
            action(APP_MENU, (int)panels[panel]);
            release_barrier();
            sync_interaction();
            const uint64_t revision = s_ui_revision;
            physical_double_click();
            assert(s_overlay == APP_OVERLAY_MODES && s_ui_revision > revision);
            assert(s_engine.state.mode == modes[mode] && !s_engine.state.locked);
            assert(s_keyboard_submode == APP_KEYBOARD_MOTION && s_audio_kind == APP_AUDIO_VOICE);
            assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
            release_barrier();
            sync_interaction();
            expect_idle();
            expect_neutral_navigation_trace();
        }
    }
}

static void confirmation_owns_navigation_choices_and_physical_double_click(void)
{
    const app_overlay_t destinations[] = {APP_OVERLAY_MODES, APP_OVERLAY_TARGET,
        APP_OVERLAY_SETTINGS, APP_OVERLAY_KEYBOARD, APP_OVERLAY_AUDIO};
    for (unsigned network = 0; network < 2; ++network) {
        navigation_with_draft(INPUT_KEYBOARD);
        if (network) {
            settings_page(APP_SETTINGS_WIFI);
            test_wifi = (app_wifi_snapshot_t){.initialized = true, .configured = true,
                .state = APP_WIFI_IDLE, .ssid = "CONFIRM-NETWORK"};
            action(APP_WIFI_FORGET_REQUEST, 0);
        } else {
            action(APP_MENU, APP_OVERLAY_TARGET);
            release_barrier();
            sync_interaction();
            test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 51};
            test_update_target = (ble_hid_target_snapshot_t){.loaded = true, .durable = true, .revision = 61};
            action(APP_PAIRING_REQUEST, APP_PAIR_OPEN);
        }
        release_barrier();
        sync_interaction();
        const app_confirmation_t confirmation = s_confirmation;
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        app_intent_t stale_confirm = make_intent(APP_CONFIRM, 1, 0, NULL);
        assert(s_overlay == APP_OVERLAY_CONFIRM && confirmation != APP_CONFIRM_NONE);
        for (size_t i = 0; i < sizeof destinations / sizeof destinations[0]; ++i)
            action(APP_MENU, (int)destinations[i]);
        action(APP_CLOSE, 0);
        action(APP_SETTINGS_PAGE, APP_SETTINGS_DISPLAY);
        action(APP_MODE, INPUT_AUDIO);
        action(APP_KEYBOARD_SUBMODE, APP_KEYBOARD_TOUCH);
        action(APP_AUDIO_KIND, APP_AUDIO_SPEAKER);
        action(APP_GRID, 0);
        action(APP_SEND, 0);
        action(APP_KEY, 4);
        physical_double_click();
        assert(s_overlay == APP_OVERLAY_CONFIRM && s_confirmation == confirmation);
        assert(s_engine.state.epoch == epoch && s_ui_revision == revision);
        assert(s_engine.state.mode == INPUT_KEYBOARD && strcmp(s_interaction.view.preview, "ABAB") == 0);
        expect_idle();
        expect_neutral_navigation_trace();
        action(APP_CONFIRM, 0);
        assert(s_overlay == APP_OVERLAY_SETTINGS && s_confirmation == APP_CONFIRM_NONE);
        assert(s_settings_page == (network ? APP_SETTINGS_WIFI : APP_SETTINGS_BLUETOOTH));
        release_barrier();
        sync_interaction();
        deliver(stale_confirm);
        assert(s_pair_operation == APP_CONFIRM_NONE && !s_send_token);
        assert(test_wifi_starts == 0 && test_wifi_forgets == 0 && test_pair_opens == 0 && !test_native_management_starts);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void wifi_setup_safe_menu_exits_clear_credentials_and_reject_stale_restart(void)
{
    const app_overlay_t destinations[] = {APP_OVERLAY_MODES, APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS};
    for (size_t destination = 0;
         destination < sizeof destinations / sizeof destinations[0]; ++destination) {
        navigation_with_draft(INPUT_KEYBOARD);
        settings_page(APP_SETTINGS_WIFI);
        test_wifi_start_error = ESP_OK;
        action(APP_WIFI_SETUP, 1);
        release_barrier();
        sync_interaction();
        test_wifi = (app_wifi_snapshot_t){.initialized = true, .setup_active = true,
            .state = APP_WIFI_IDLE, .setup_remaining_ms = 600000};
        test_wifi_info = (app_wifi_setup_info_t){.generation = 5,
            .ssid = "NAV-SETUP", .password = "synthetic-password", .ip = "192.0.2.2"};
        test_wifi_info_error = ESP_OK;
        render_view(&test_link, ESP_OK);
        assert(test_view.wifi_setup_passcode[0] && test_wifi_starts == 1);
        app_intent_t stale_restart = make_intent(APP_WIFI_SETUP, 1, 0, NULL);
        action(APP_MENU, (int)destinations[destination]);
        assert(s_overlay == destinations[destination] && test_wifi_stops == 1);
        release_barrier();
        sync_interaction();
        const unsigned reads = test_wifi_info_reads;
        render_view(&test_link, ESP_OK);
        assert(test_wifi_info_reads == reads && !test_view.wifi_setup_passcode[0]);
        assert(!test_view.wifi_setup_ssid[0] && !test_view.wifi_setup_url[0]);
        deliver(stale_restart);
        assert(s_overlay == destinations[destination] && test_wifi_starts == 1 && test_wifi_stops == 1);
        assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

static void pad_at(app_action_t kind, uint32_t gesture, uint64_t time, int x, int y)
{
    s_now_ms = time;
    touch(kind, gesture, x, y);
}

static void pad_click_pair(void)
{
    static const uint8_t down[4] = {1,0,0,0}, up[4] = {0};
    expect_report(2, down, sizeof down);
    expect_report(2, up, sizeof up);
}

static void coordinator_touch_hold(void)
{
    setup(INPUT_MOUSE);
    assert(s_settings.tap_to_click);
    pad_at(APP_TOUCH_BEGIN, 2001, 1000, 100, 100);
    pad_at(APP_TOUCH_END, 2001, 1020, 100, 100);
    pad_click_pair();
    pad_at(APP_TOUCH_BEGIN, 2002, 1100, 100, 100);
    static const uint8_t down[4] = {1,0,0,0};
    expect_report(2, down, sizeof down);
    assert(interaction_touch_holding(&s_interaction) && s_engine.mouse_buttons == 1);
}

static void pad_single_double_and_drag_use_actual_hid_reports(void)
{
    for (unsigned drag = 0; drag < 2; ++drag) {
        coordinator_touch_hold();
        if (drag) {
            pad_at(APP_TOUCH_MOVE, 2002, 1110, 12, 3);
            const uint8_t moved[4] = {1,12,3,0};
            expect_report(2, moved, sizeof moved);
        }
        pad_at(APP_TOUCH_MOVE, 2002, 1120, 0, 0); /* Stationary physical heartbeat. */
        expect_idle();
        pad_at(APP_TOUCH_END, 2002, 1130, drag ? 112 : 100, drag ? 103 : 100);
        const uint8_t up[4] = {0};
        expect_report(2, up, sizeof up);
        assert(!interaction_touch_holding(&s_interaction) && !s_engine.mouse_buttons);
        assert(trace_count == (drag ? 5U : 4U));
        deliver(make_intent(APP_TOUCH_END, 100, 100, NULL));
        expect_idle();
    }
}

static void queued_pad_events_use_native_time_and_raw_out_and_back_slop(void)
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        setup(INPUT_MOUSE);
        const uint32_t gesture = 2100U + scenario;
        s_now_ms = 1000;
        app_intent_t begin = make_intent(APP_TOUCH_BEGIN, 100, 100, NULL);
        begin.gesture = gesture; begin.surface = APP_SURFACE_TOUCHPAD;
        assert(submit_intent(&begin));
        if (scenario == 2) {
            s_now_ms = 1010;
            app_intent_t move = make_intent(APP_TOUCH_MOVE, 9, 0, NULL);
            move.gesture = gesture; move.surface = APP_SURFACE_TOUCHPAD;
            assert(submit_intent(&move));
            s_now_ms = 1020; move.x = -9;
            assert(submit_intent(&move));
        }
        s_now_ms = scenario == 1 ? 1300 : 1030;
        app_intent_t end = begin; end.action = APP_TOUCH_END;
        assert(submit_intent(&end));
        s_now_ms = 1400;
        while (uxQueueMessagesWaiting(s_intents)) consume_one();
        if (scenario == 0) pad_click_pair();
        else assert(trace_count == 0); /* Long press or queued swipe is not a tap. */
        expect_idle();
        assert(!interaction_touch_holding(&s_interaction));
    }
}

static void queued_drag_with_lost_live_contact_is_neutralized(void)
{
    setup(INPUT_MOUSE);
    pad_at(APP_TOUCH_BEGIN, 2201, 1000, 100, 100);
    pad_at(APP_TOUCH_END, 2201, 1020, 100, 100);
    pad_click_pair();
    const size_t before = trace_count;
    s_now_ms = 1100;
    app_intent_t intent = make_intent(APP_TOUCH_BEGIN, 100, 100, NULL);
    intent.gesture = 2202; intent.surface = APP_SURFACE_TOUCHPAD;
    assert(submit_intent(&intent));
    s_now_ms = 1110; intent.action = APP_TOUCH_MOVE; intent.x = 20; intent.y = 0;
    assert(submit_intent(&intent));
    s_now_ms = 1120; intent.action = APP_TOUCH_END; intent.x = 120; intent.y = 100;
    assert(submit_intent(&intent));
    s_now_ms = 1140;
    while (uxQueueMessagesWaiting(s_intents)) consume_one();
    (void)apply_touch_result(interaction_touch_tick(&s_interaction, s_now_ms, s_ui_revision));
    assert(!interaction_touch_holding(&s_interaction) && s_engine.state.release_pending);
    release_barrier();
    for (size_t i = before; i < trace_count; ++i)
        for (size_t byte = 0; byte < trace[i].length; ++byte) assert(trace[i].bytes[byte] == 0);
    expect_idle();
}

static void tap_preference_preserves_movement_and_explicit_drag(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_INPUT);
    action(APP_TAP_TO_CLICK, 0);
    release_barrier(); sync_interaction();
    assert(!s_settings.tap_to_click && !s_interaction.touch_config.tap_to_click);
    assert(test_storage_requests == 1 && !test_saved_request.tap_to_click);
    action(APP_CLOSE, 0);
    pad_at(APP_TOUCH_BEGIN, 2301, 2000, 100, 100);
    pad_at(APP_TOUCH_MOVE, 2301, 2010, 6, -3);
    const uint8_t moved[4] = {0,6,253,0};
    expect_report(2, moved, sizeof moved);
    pad_at(APP_TOUCH_END, 2301, 2020, 106, 97);
    expect_idle();
    action(APP_DRAG, 0);
    const uint8_t down[4] = {1,0,0,0};
    expect_report(2, down, sizeof down);
    pad_at(APP_TOUCH_BEGIN, 2302, 2100, 100, 100);
    pad_at(APP_TOUCH_MOVE, 2302, 2110, 5, 2);
    const uint8_t dragged[4] = {1,5,2,0};
    expect_report(2, dragged, sizeof dragged);
    pad_at(APP_TOUCH_END, 2302, 2120, 105, 102);
    assert(s_drag_requested && s_engine.mouse_buttons == 1 && !interaction_touch_holding(&s_interaction));
    expect_idle();
    action(APP_DRAG, 0);
    release_barrier(); sync_interaction();
    assert(!s_drag_requested && !s_engine.mouse_buttons && test_storage_requests == 1);
}

static void touch_drag_boundaries_and_contact_deadman_release(void)
{
    for (unsigned boundary = 0; boundary < 6; ++boundary) {
        coordinator_touch_hold();
        const size_t before = trace_count;
        s_now_ms = 1110;
        if (boundary == 0) touch(APP_TOUCH_CANCEL, 2002, 100, 100);
        if (boundary == 1) {
            app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
            assert(submit_intent(&lock) && process_priority());
        }
        if (boundary == 2) action(APP_MENU, APP_OVERLAY_MODES);
        if (boundary == 3) action(APP_MODE, INPUT_KEYBOARD);
        if (boundary == 4) {
            test_link.connection_admitted = test_link.connected = test_link.ready = false;
            hid_engine_set_link(&s_engine, false, false);
        }
        if (boundary == 5) s_now_ms = 2100; /* Exact last-contact +1000 ms. */
        sync_interaction();
        (void)apply_touch_result(interaction_touch_tick(&s_interaction, s_now_ms, s_ui_revision));
        assert(!interaction_touch_holding(&s_interaction) && s_engine.state.release_pending);
        if (boundary == 4) {
            test_link.connection_admitted = test_link.connected = test_link.ready = true;
            ++test_link.generation;
            hid_engine_set_link(&s_engine, true, true);
        }
        release_barrier(); sync_interaction();
        for (size_t i = before; i < trace_count; ++i)
            for (size_t byte = 0; byte < trace[i].length; ++byte) assert(trace[i].bytes[byte] == 0);
        expect_idle();
    }
}

static void touch_release_survives_congestion_and_refused_down_never_drags(void)
{
    coordinator_touch_hold();
    test_link.congested = true;
    assert(!hid_engine_observe_transport(&s_engine, 1110, true));
    sync_interaction();
    pad_at(APP_TOUCH_END, 2002, 1120, 100, 100);
    assert(!interaction_touch_holding(&s_interaction) && s_engine.mouse_buttons == 1);
    assert(step() == HID_ENGINE_STEP_BLOCKED);
    test_link.congested = false;
    assert(!hid_engine_observe_transport(&s_engine, 1130, false));
    sync_interaction();
    const uint8_t up[4] = {0};
    expect_report(2, up, sizeof up);
    assert(!s_engine.mouse_buttons);
    setup(INPUT_MOUSE);
    pad_at(APP_TOUCH_BEGIN, 2401, 1000, 100, 100);
    pad_at(APP_TOUCH_END, 2401, 1020, 100, 100);
    pad_click_pair();
    for (unsigned i = 0; i < 31; ++i)
        assert(hid_engine_enqueue_mouse_move(&s_engine, s_engine.state.epoch, INPUT_LOCAL, 0, 0) == HID_ENGINE_ENQUEUE_OK);
    pad_at(APP_TOUCH_BEGIN, 2402, 1100, 100, 100);
    assert(!interaction_touch_holding(&s_interaction) && s_engine.count == 31);
    pad_at(APP_TOUCH_MOVE, 2402, 1110, 20, 0);
    pad_at(APP_TOUCH_END, 2402, 1120, 120, 100);
    assert(s_engine.count == 31 && trace_count == 2);
    cancel_input(); release_barrier(); expect_idle();
}

static void common_keys_are_explicit_and_preserve_preview(void)
{
    setup(INPUT_KEYBOARD); fill_preview(4);
    action(APP_MENU, APP_OVERLAY_COMMON_KEYS);
    release_barrier(); sync_interaction();
    const unsigned usages[] = {0x29,0x28,0x2a,0x2b,0x4a,0x4c,0x4d,0x4f,0x50,0x51,0x52};
    for (size_t i = 0; i < sizeof usages / sizeof usages[0]; ++i) {
        action(APP_KEY, (int)usages[i]);
        const uint8_t down[8] = {0,0,(uint8_t)usages[i],0,0,0,0,0}, up[8] = {0};
        expect_report(1, down, sizeof down); expect_report(1, up, sizeof up);
    }
    const int invalid[] = {0,1,2,3,4,0x4b,0x4e,0x66};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) action(APP_KEY, invalid[i]);
    deliver(make_intent(APP_KEY, 0x28, 1, NULL));
    action(APP_CLEAR, 0); action(APP_SEND, 0); action(APP_MODE, INPUT_MOUSE);
    assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
    assert(s_overlay == APP_OVERLAY_COMMON_KEYS && s_engine.state.mode == INPUT_KEYBOARD);
    app_intent_t stale = make_intent(APP_KEY, 0x28, 0, NULL);
    action(APP_CLOSE, 0); deliver(stale);
    expect_idle();
    assert(test_storage_requests == 0);
    setup(INPUT_MOUSE);
    const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
    action(APP_MENU, APP_OVERLAY_COMMON_KEYS);
    assert(s_overlay == APP_OVERLAY_NONE && s_engine.state.epoch == epoch && s_ui_revision == revision);
}

static void common_keys_include_host_edit_shortcuts_in_each_keyboard_submode(void)
{
    const uint8_t usages[] = {0x06, 0x19, 0x1d, 0x04}; /* Copy, Paste, Undo, Select all. */
    const int shortcuts[] = {APP_SHORTCUT_COPY, APP_SHORTCUT_PASTE, APP_SHORTCUT_UNDO, APP_SHORTCUT_SELECT_ALL};
    const uint8_t up[8] = {0};
    for (int host = APP_HOST_CTRL; host <= APP_HOST_COMMAND; ++host) {
        for (int submode = APP_KEYBOARD_MOTION; submode <= APP_KEYBOARD_SHORTCUTS; ++submode) {
            setup(INPUT_KEYBOARD);
            fill_preview(4);
            action(APP_MENU, APP_OVERLAY_SETTINGS);
            action(APP_SETTINGS_PAGE, APP_SETTINGS_INPUT);
            action(APP_HOST_OS, host);
            release_barrier(); action(APP_CLOSE, 0); sync_interaction();
            action(APP_KEYBOARD_SUBMODE, submode);
            release_barrier(); sync_interaction();
            action(APP_MENU, APP_OVERLAY_COMMON_KEYS);
            release_barrier(); sync_interaction();
            assert(s_keyboard_submode == (app_keyboard_submode_t)submode);
            for (size_t shortcut = 0; shortcut < sizeof shortcuts / sizeof shortcuts[0]; ++shortcut) {
                action(APP_SHORTCUT, shortcuts[shortcut]);
                const uint8_t down[8] = {host == APP_HOST_COMMAND ? 8 : 1, 0,
                    usages[shortcut], 0, 0, 0, 0, 0};
                expect_report(1, down, sizeof down);
                expect_report(1, up, sizeof up);
                assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
                expect_idle();
            }
            action(APP_SHORTCUT, -1); action(APP_SHORTCUT, APP_SHORTCUT_SELECT_ALL + 1);
            /* Non-edit shortcuts use the explicit unmodified APP_KEY path
             * in this panel; forged enum variants cannot bypass its contract. */
            for (int other = APP_SHORTCUT_TAB; other <= APP_SHORTCUT_ENTER; ++other)
                action(APP_SHORTCUT, other);
            s_engine.ready = false;
            action(APP_SHORTCUT, APP_SHORTCUT_SELECT_ALL);
            assert(!s_engine.count && !strcmp(s_interaction.view.preview, "ABAB"));
            s_engine.ready = true; sync_interaction();
            hid_engine_cancel(&s_engine); sync_interaction();
            action(APP_SHORTCUT, APP_SHORTCUT_SELECT_ALL);
            assert(!s_engine.count);
            release_barrier(); sync_interaction();
            app_intent_t stale = make_intent(APP_SHORTCUT, APP_SHORTCUT_SELECT_ALL, 0, NULL);
            action(APP_CLOSE, 0); deliver(stale);
            action(APP_SHORTCUT, APP_SHORTCUT_SELECT_ALL); /* No forged legacy-submode admission. */
            assert(s_overlay == APP_OVERLAY_NONE && s_keyboard_submode == (app_keyboard_submode_t)submode);
            expect_idle();
        }
    }
}

static bool memory_has_text(const void *memory, size_t bytes, const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; length <= bytes && i <= bytes - length; ++i)
        if (memcmp((const uint8_t *)memory + i, text, length) == 0) return true;
    return false;
}

static void expect_cleared_submission(void)
{
    const uint8_t *bytes = (const uint8_t *)&s_local_submission;
    for (size_t i = 0; i < sizeof s_local_submission; ++i) assert(bytes[i] == 0);
    assert(!s_local_submission_pending);
}

static void local_control_contact(uint32_t gesture)
{
    app_intent_t intent = make_intent(APP_TOUCH_BEGIN, 100, 100, NULL);
    intent.gesture = gesture; intent.surface = APP_SURFACE_CONTROL;
    deliver(intent);
    intent.action = APP_TOUCH_END;
    deliver(intent);
}

static app_local_edit_request_t editor_request(uint32_t gesture, const char *value)
{
    app_local_edit_request_t request = {.purpose = s_local_editor.purpose,
        .epoch = s_engine.state.epoch, .ui_revision = s_ui_revision,
        .editor_session = s_local_editor.session, .gesture = gesture};
    if (request.purpose == APP_LOCAL_EDIT_WIFI_PASSPHRASE) {
        request.value.wifi.scan_generation = s_local_editor.wifi_scan_generation;
        request.value.wifi.record_index = s_local_editor.wifi_record_index;
        request.value.wifi.remember = s_local_editor.wifi_remember;
        request.value.wifi.length = strlen(value);
        assert(request.value.wifi.length < sizeof request.value.wifi.password);
        memcpy(request.value.wifi.password, value, strlen(value) + 1);
    } else {
        request.value.bluetooth.length = strlen(value);
        assert(request.value.bluetooth.length < sizeof request.value.bluetooth.name);
        memcpy(request.value.bluetooth.name, value, strlen(value) + 1);
    }
    return request;
}

static void open_name_editor(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_BLUETOOTH);
    action(APP_BLUETOOTH_NAME_EDIT, 0);
    release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_LOCAL_EDITOR && s_local_editor.purpose == APP_LOCAL_EDIT_BLUETOOTH_NAME);
}

static void open_network_editor_for(bool remember, uint32_t lifetime_ms)
{
    setup(INPUT_MOUSE);
    test_selection_lifetime_ms = lifetime_ms;
    test_wifi.initialized = true; test_wifi.state = APP_WIFI_IDLE;
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SCAN, 1);
    release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_WIFI_NETWORKS && test_scans == 1);
    test_scan = (app_wifi_scan_snapshot_t){.state = APP_WIFI_SCAN_READY, .generation = 7,
        .count = 1, .remaining_ms = 30000};
    test_scan_records[0] = (app_wifi_scan_record_t){.ssid = "REAL-NET", .ssid_length = 8,
        .label = "Display-only label", .rssi_dbm = -55, .security = APP_WIFI_SECURITY_WPA2,
        .availability = APP_WIFI_JOIN_AVAILABLE};
    app_intent_t select = make_intent(APP_WIFI_NETWORK_SELECT, 0, 0, NULL);
    select.wifi_scan_generation = 7; select.wifi_record_index = 0;
    deliver(select); release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_WIFI_NETWORK);
    app_intent_t open = make_intent(APP_WIFI_CREDENTIAL_OPEN, remember ? 1 : 0, 0, NULL);
    open.wifi_scan_generation = 7; open.wifi_record_index = 0;
    deliver(open); release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_LOCAL_EDITOR && s_wifi_reserved.id == 31);
}

static void open_network_editor(bool remember)
{
    open_network_editor_for(remember, 300000);
}

static void local_name_persists_only_matching_verified_acknowledgement(void)
{
    open_name_editor(); local_control_contact(2501);
    app_local_edit_request_t request = editor_request(2501, "Desk Pad");
    assert(submit_local_edit(&request) == APP_LOCAL_EDIT_ACCEPTED);
    assert(submit_local_edit(&request) == APP_LOCAL_EDIT_BUSY);
    memset(request.value.bluetooth.name, 'X', request.value.bluetooth.length);
    service_local_editor();
    assert(test_name_requests == 1 && strcmp(test_name.requested, "Desk Pad") == 0);
    assert(s_name_should_persist && test_storage_requests == 0 && strcmp(s_settings.bluetooth_name, "ESP32-INPUT") == 0);
    expect_cleared_submission();
    const uint64_t id = test_name.request_id;
    test_name.status = BLE_HID_NAME_APPLIED; ++test_name.request_id;
    strcpy(test_name.active, "Desk Pad"); service_bluetooth_name();
    assert(test_storage_requests == 0);
    test_name.request_id = id; test_name.active_known = false; service_bluetooth_name();
    assert(test_storage_requests == 0);
    test_name.active_known = true; service_bluetooth_name();
    assert(test_storage_requests == 1 && !s_name_should_persist && strcmp(s_settings.bluetooth_name, "Desk Pad") == 0);
    service_bluetooth_name();
    assert(test_storage_requests == 1 && strcmp(test_saved_request.bluetooth_name, "Desk Pad") == 0);
    release_barrier(); expect_idle();
}

static void failed_or_stale_local_names_cannot_change_preferences(void)
{
    for (unsigned fault = 0; fault < 6; ++fault) {
        open_name_editor(); local_control_contact(2601);
        app_local_edit_request_t request = editor_request(2601, "Desk Pad");
        if (fault == 0) --request.epoch;
        if (fault == 1) --request.ui_revision;
        if (fault == 2) ++request.editor_session;
        assert(submit_local_edit(&request) == APP_LOCAL_EDIT_ACCEPTED);
        if (fault == 3) test_name_error = ESP_FAIL;
        service_local_editor();
        if (fault < 3) assert(test_name_requests == 0);
        if (fault >= 4) {
            test_name.status = fault == 4 ? BLE_HID_NAME_FAILED : BLE_HID_NAME_UNCERTAIN;
            service_bluetooth_name();
            test_name.status = BLE_HID_NAME_APPLIED;
            strcpy(test_name.active, "Desk Pad"); service_bluetooth_name();
        }
        assert(!s_name_should_persist && test_storage_requests == 0 && strcmp(s_settings.bluetooth_name, "ESP32-INPUT") == 0);
        expect_cleared_submission();
        release_barrier(); expect_idle();
    }
}

static void private_wifi_submission_waits_for_touch_fifo_and_cancel(void)
{
    const char *secret = "secret-local-9";
    for (unsigned cancel = 0; cancel < 2; ++cancel) {
        open_network_editor(true);
        app_local_edit_request_t request = editor_request(2701, secret);
        app_intent_t contact = make_intent(APP_TOUCH_BEGIN, 100, 100, NULL);
        contact.gesture = 2701; contact.surface = APP_SURFACE_CONTROL;
        assert(submit_intent(&contact));
        contact.action = cancel ? APP_TOUCH_CANCEL : APP_TOUCH_END;
        assert(submit_intent(&contact));
        assert(submit_local_edit(&request) == APP_LOCAL_EDIT_ACCEPTED);
        service_local_editor();
        assert(test_joins == 0 && s_local_submission_pending);
        render_view(&test_link, ESP_OK);
        assert(!memory_has_text(&test_view, sizeof test_view, secret));
        assert(!memory_has_text(s_intent_bytes, sizeof s_intent_bytes, secret));
        consume_one(); service_local_editor();
        assert(test_joins == 0 && s_local_submission_pending);
        consume_one(); service_local_editor();
        assert(test_joins == (cancel ? 0U : 1U));
        if (!cancel) {
            assert(strcmp(test_join_request.ssid, "REAL-NET") == 0 && strcmp(test_join_request.password, secret) == 0);
            assert(test_join_request.remember && test_join_request.selection_id == 31);
        }
        expect_cleared_submission();
        assert(!s_wifi_reserved.id && test_selection_releases == 1);
        assert(test_wifi.state == APP_WIFI_IDLE && !test_wifi.lan_ready && test_storage_requests == 0);
        release_barrier(); expect_idle();
    }
}

static void wifi_editor_uses_reserved_identity_after_scan_expiry(void)
{
    open_network_editor(false);
    test_scan.state = APP_WIFI_SCAN_EXPIRED; ++test_scan.generation;
    memcpy(test_scan_records[0].ssid, "REPLACED", 8);
    s_now_ms += 31000;
    local_control_contact(2801);
    app_local_edit_request_t request = editor_request(2801, "secret-local-9");
    assert(submit_local_edit(&request) == APP_LOCAL_EDIT_ACCEPTED);
    service_local_editor();
    assert(test_joins == 1 && !test_join_request.remember && test_join_request.scan_generation == 7);
    assert(test_join_request.selection_id == 31 && strcmp(test_join_request.ssid, "REAL-NET") == 0);
    assert(test_selection_releases == 1 && !s_wifi_reserved.id);
    expect_cleared_submission(); release_barrier(); expect_idle();
}

static void private_editor_deadline_and_lock_clear_secret_and_ticket(void)
{
    for (unsigned locked = 0; locked < 2; ++locked) {
        open_network_editor(true); local_control_contact(2901);
        app_local_edit_request_t request = editor_request(2901, "secret-local-9");
        assert(submit_local_edit(&request) == APP_LOCAL_EDIT_ACCEPTED);
        const size_t reports = trace_count;
        const uint64_t epoch = s_engine.state.epoch;
        if (locked) {
            app_intent_t lock = make_intent(APP_LOCK, 0, 0, NULL);
            assert(submit_intent(&lock) && process_priority());
        } else s_now_ms = s_editor_deadline_ms;
        service_local_editor();
        assert(test_joins == 0 && s_local_editor.purpose == APP_LOCAL_EDIT_NONE && !s_wifi_reserved.id);
        assert(test_selection_releases == 1 && test_released_selection == 31);
        expect_cleared_submission();
        render_view(&test_link, ESP_OK);
        assert(!memory_has_text(&test_view, sizeof test_view, "secret-local-9"));
        if (locked) release_barrier();
        else assert(!s_engine.state.release_pending && s_engine.state.epoch == epoch && trace_count == reports);
        expect_idle();
    }
}

static void mouse_pad_wake_is_consumed_before_fresh_takeover_and_click(void)
{
    for (unsigned remote = 0; remote < 2; ++remote) {
        setup(INPUT_MOUSE);
        if (remote) {
            assert(hid_engine_set_owner(&s_engine, INPUT_REMOTE));
            release_barrier(); sync_interaction();
        }
        const size_t before = trace_count;
        display_at(301000); brightness_applied(0);
        pad_at(APP_TOUCH_BEGIN, 3001, 301010, 100, 100);
        brightness_applied(50);
        pad_at(APP_TOUCH_END, 3001, 301030, 100, 100);
        assert(trace_count == before && !interaction_touch_holding(&s_interaction));
        assert(s_engine.state.owner == (remote ? INPUT_REMOTE : INPUT_LOCAL));
        expect_idle();
        pad_at(APP_TOUCH_BEGIN, 3002, 301100, 100, 100);
        if (remote) release_barrier();
        pad_at(APP_TOUCH_END, 3002, 301120, 100, 100);
        if (!remote) pad_click_pair();
        else expect_idle(); /* The fresh takeover press is consumed as well. */
        assert(s_engine.state.owner == INPUT_LOCAL);
        pad_at(APP_TOUCH_BEGIN, 3003, 302000, 100, 100);
        pad_at(APP_TOUCH_END, 3003, 302020, 100, 100);
        pad_click_pair(); expect_idle();
        assert(!interaction_touch_holding(&s_interaction));
    }
}

static void queued_expired_pad_and_real_clock_refresh_never_replay(void)
{
    setup(INPUT_MOUSE);
    app_intent_t intent = make_intent(APP_TOUCH_BEGIN, 100, 100, NULL);
    intent.gesture = 3101; intent.surface = APP_SURFACE_TOUCHPAD;
    assert(submit_intent(&intent));
    s_now_ms = 1020; intent.action = APP_TOUCH_END;
    assert(submit_intent(&intent));
    s_now_ms = 2020;
    consume_one(); consume_one();
    expect_idle();
    assert(trace_count == 0);
    coordinator_touch_hold();
    test_clock_override = true; test_clock_ms = 1110; /* Callback after the loop's 1100-ms sample. */
    touch(APP_TOUCH_MOVE, 2002, 0, 0);
    assert(s_now_ms == 1110 && s_interaction.touch.contact_ms == 1110 && interaction_touch_holding(&s_interaction));
    intent = make_intent(APP_TOUCH_MOVE, 0, 0, NULL);
    intent.gesture = 2002; intent.surface = APP_SURFACE_TOUCHPAD; intent.event_ms = 1111;
    handle_intent(&intent, &test_link); /* An actually future timestamp is still rejected. */
    test_clock_override = false;
    assert(s_engine.state.release_pending && !interaction_touch_holding(&s_interaction));
    release_barrier(); expect_idle();
}

static void short_selection_ticket_expires_even_while_scan_is_fresh(void)
{
    open_network_editor_for(true, 2000);
    assert(s_editor_deadline_ms == 3000 && test_scan.state == APP_WIFI_SCAN_READY);
    local_control_contact(3201);
    app_local_edit_request_t request = editor_request(3201, "secret-local-9");
    s_now_ms = 2999;
    assert(submit_local_edit(&request) == APP_LOCAL_EDIT_ACCEPTED);
    s_now_ms = 3000;
    service_local_editor();
    assert(test_joins == 0 && test_selection_releases == 1 && !s_wifi_reserved.id);
    assert(s_local_editor.purpose == APP_LOCAL_EDIT_NONE && test_scan.state == APP_WIFI_SCAN_READY);
    expect_cleared_submission(); expect_idle();
}

static void common_keys_route_from_keyboard_chooser_rejects_other_origins(void)
{
    navigation_with_draft(INPUT_KEYBOARD);
    action(APP_MENU, APP_OVERLAY_KEYBOARD); release_barrier(); sync_interaction();
    action(APP_MENU, APP_OVERLAY_COMMON_KEYS); release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_COMMON_KEYS && strcmp(s_interaction.view.preview, "ABAB") == 0);
    action(APP_KEY, 0x29);
    const uint8_t escape[8] = {0,0,0x29,0,0,0,0,0}, zero[8] = {0};
    expect_report(1, escape, sizeof escape); expect_report(1, zero, sizeof zero);
    const app_overlay_t origins[] = {APP_OVERLAY_MODES, APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS};
    for (size_t i = 0; i < sizeof origins / sizeof origins[0]; ++i) {
        setup(INPUT_KEYBOARD); action(APP_MENU, (int)origins[i]); release_barrier(); sync_interaction();
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        action(APP_MENU, APP_OVERLAY_COMMON_KEYS);
        assert(s_overlay == origins[i] && s_engine.state.epoch == epoch && s_ui_revision == revision);
        expect_idle();
    }
    setup(INPUT_AUDIO); action(APP_MENU, APP_OVERLAY_AUDIO); release_barrier(); sync_interaction();
    action(APP_MENU, APP_OVERLAY_COMMON_KEYS);
    assert(s_overlay == APP_OVERLAY_AUDIO); expect_idle();
}

static void network_detail_back_returns_to_list_only_from_valid_origin(void)
{
    open_network_editor(false);
    action(APP_CLOSE, 0); /* Leave the editor and release its ticket first. */
    assert(test_selection_releases == 1 && !s_wifi_reserved.id);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_WIFI_SCAN, 1); release_barrier(); sync_interaction();
    app_intent_t select = make_intent(APP_WIFI_NETWORK_SELECT, 0, 0, NULL);
    select.wifi_scan_generation = 7; select.wifi_record_index = 0;
    deliver(select); release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_WIFI_NETWORK);
    action(APP_MENU, APP_OVERLAY_WIFI_NETWORKS); release_barrier(); sync_interaction();
    assert(s_overlay == APP_OVERLAY_WIFI_NETWORKS && test_selections == 1 && test_joins == 0);
    render_view(&test_link, ESP_OK);
    assert(test_view.wifi_scan.row_count == 1 && test_view.wifi_scan.rows[0].record_index == 0);
    const app_overlay_t origins[] = {APP_OVERLAY_NONE, APP_OVERLAY_MODES, APP_OVERLAY_TARGET, APP_OVERLAY_SETTINGS};
    for (size_t i = 0; i < sizeof origins / sizeof origins[0]; ++i) {
        setup(INPUT_MOUSE);
        if (origins[i] != APP_OVERLAY_NONE) { action(APP_MENU, (int)origins[i]); release_barrier(); sync_interaction(); }
        const uint64_t epoch = s_engine.state.epoch, revision = s_ui_revision;
        action(APP_MENU, APP_OVERLAY_WIFI_NETWORKS);
        assert(s_overlay == origins[i] && s_engine.state.epoch == epoch && s_ui_revision == revision);
        assert(test_scans == 0 && test_selections == 0); expect_idle();
    }
}

static void boot_fault_uses_actual_display_after_partial_board_start(void)
{
    test_boot_boundary_enabled = true;
    test_boot_lock_allowed = true;
    test_boot_locked = test_boot_active = false;
    test_boot_lock_calls = test_boot_create_calls = test_boot_update_calls = 0;
    test_boot_display = NULL;
    present_boot(APP_BOOT_UI_FAULT, "board initialization", "ESP_FAIL");
    assert(test_boot_lock_calls == 0 && test_boot_update_calls == 0);
    /* An opaque retained display handle models a late board-init failure;
     * no overall success flag is supplied to the production helper. */
    test_boot_display = (lv_display_t *)(void *)&test_boot_view;
    test_boot_lock_allowed = false;
    present_boot(APP_BOOT_UI_FAULT, "board initialization", "ESP_FAIL");
    assert(test_boot_lock_calls == 1 && test_boot_update_calls == 0);
    test_boot_lock_allowed = true;
    present_boot(APP_BOOT_UI_FAULT, "board initialization", "ESP_FAIL");
    assert(test_boot_create_calls == 1 && test_boot_update_calls == 1 && !test_boot_locked);
    assert(test_boot_view.state == APP_BOOT_UI_FAULT && !test_boot_view.steps_known);
    assert(strcmp(test_boot_view.stage, "board initialization") == 0);
    assert(strcmp(test_boot_view.detail, "ESP_FAIL") == 0);
    present_boot(APP_BOOT_UI_FAULT, "physical button events", "ESP_ERR_TIMEOUT");
    assert(test_boot_create_calls == 1 && test_boot_update_calls == 2 && !test_boot_locked);
    assert(strcmp(test_boot_view.stage, "physical button events") == 0);
    test_boot_boundary_enabled = false;
}

static void motion_settings_are_independent_versioned_and_validated(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_INPUT);
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_settings.available && test_view.motion_settings.editable);
    assert(test_view.motion_settings.gain_x_centi == 400 && test_view.motion_settings.gain_y_centi == 400);
    const unsigned writes = test_storage_requests;
    app_intent_t intent = make_intent(APP_MOTION_CONFIG, 0, 0, NULL);
    intent.motion_patch = (app_motion_patch_t){.field = APP_MOTION_FIELD_GAIN_X,
        .expected_settings_revision = s_settings_revision, .value.gain_centi = 1200};
    deliver(intent);
    assert(s_settings.motion_gain_x_centi == 1200 && s_settings.motion_gain_y_centi == 400);
    assert(s_interaction.config.pointer_gain_x == 12.0f && s_interaction.config.pointer_gain_y == 4.0f);
    assert(test_storage_requests == writes + 1 && s_settings_revision == 2);
    intent = make_intent(APP_MOTION_CONFIG, 0, 0, NULL);
    intent.motion_patch = (app_motion_patch_t){.field = APP_MOTION_FIELD_GAIN_Y,
        .expected_settings_revision = 1, .value.gain_centi = 800};
    deliver(intent); /* Fresh UI action carrying a stale settings version. */
    assert(test_storage_requests == writes + 1 && s_settings.motion_gain_y_centi == 400);
    intent.motion_patch.expected_settings_revision = s_settings_revision;
    intent.motion_patch.value.gain_centi = 24;
    deliver(intent);
    assert(test_storage_requests == writes + 1 && s_settings_revision == 2);
    intent.motion_patch.field = APP_MOTION_FIELD_FREEZE;
    intent.motion_patch.value.milliseconds = 600;
    deliver(intent);
    assert(s_interaction.config.freeze_ms == 600 && s_settings.motion_freeze_ms == 600);
    assert(test_storage_requests == writes + 2 && s_settings_revision == 3);
    render_view(&test_link, ESP_OK);
    assert(test_view.motion_settings.freeze_ms == 600 && test_view.motion_settings.revision == 3);
    expect_idle();
}

static void new_settings_pages_reach_shared_preferences(void)
{
    setup(INPUT_MOUSE);
    settings_page(APP_SETTINGS_MOTION);
    assert(s_settings_page == APP_SETTINGS_MOTION);
    app_intent_t change = make_intent(APP_MOTION_CONFIG, 0, 0, NULL);
    change.motion_patch = (app_motion_patch_t){.field = APP_MOTION_FIELD_GAIN_Y,
        .expected_settings_revision = s_settings_revision, .value.gain_centi = 800};
    deliver(change);
    assert(s_settings.motion_gain_y_centi == 800 && s_interaction.config.pointer_gain_y == 8.0f);
    settings_page(APP_SETTINGS_MOTION_ADVANCED);
    change = make_intent(APP_MOTION_CONFIG, 0, 0, NULL);
    change.motion_patch = (app_motion_patch_t){.field = APP_MOTION_FIELD_INVERT_X,
        .expected_settings_revision = s_settings_revision, .value.inverted = true};
    deliver(change); assert(s_settings.motion_invert_x);
    settings_page(APP_SETTINGS_GESTURES);
    action(APP_TAP_TO_CLICK, 0); assert(!s_settings.tap_to_click);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_MENU, APP_OVERLAY_API_CONNECTION);
    assert(s_overlay == APP_OVERLAY_API_CONNECTION);
    action(APP_CLOSE, 0);
    settings_page(APP_SETTINGS_WIFI);
    action(APP_MENU, APP_OVERLAY_WIFI_PROFILES);
    assert(s_overlay == APP_OVERLAY_WIFI_PROFILES);
}

static void temporary_preferences_do_not_leak_into_unrelated_native_saves(void)
{
    setup(INPUT_MOUSE);
    device_settings_t temporary = s_settings;
    temporary.brightness_percent = 80;
    temporary.motion_gain_x_centi = 900;
    assert(update_settings_internal(&temporary, false));
    assert(test_storage_requests == 0 && s_settings.brightness_percent == 80);
    assert(s_durable_settings.brightness_percent == 50 && s_durable_settings.motion_gain_x_centi == 400);
    render_view(&test_link, ESP_OK);
    assert(test_view.settings_save_state == APP_SAVE_VOLATILE);
    assert(test_view.bluetooth_name.save_state == APP_SAVE_DEFAULTS);
    device_settings_t native = s_settings;
    native.theme = DEVICE_THEME_LIGHT;
    assert(update_settings(&native));
    assert(test_storage_requests == 1 && test_saved_request.theme == DEVICE_THEME_LIGHT);
    assert(test_saved_request.brightness_percent == 50 && test_saved_request.motion_gain_x_centi == 400);
    assert(s_settings.brightness_percent == 80 && s_settings.motion_gain_x_centi == 900);
    assert(update_settings_internal(&s_settings, true));
    assert(test_storage_requests == 2 && test_saved_request.brightness_percent == 80);
    assert(device_settings_equal(&s_durable_settings, &s_settings));
}

static void temporary_transport_switch_waits_without_persisting(void)
{
    transport_setup(INPUT_MOUSE, APP_INPUT_BLUETOOTH);
    assert(request_input_transport_internal(APP_INPUT_USB, &test_link, false));
    service_transport_switch(); assert(s_transport_switch_pending && test_storage_requests == 0);
    release_barrier(); service_transport_switch();
    assert(!s_transport_switch_pending && s_input_transport == APP_INPUT_USB);
    assert(s_settings.preferred_transport == DEVICE_TRANSPORT_USB && test_storage_requests == 0);
    assert(s_durable_settings.preferred_transport == DEVICE_TRANSPORT_BLUETOOTH);
}

static void contacts_at(uint64_t sequence, uint64_t time, unsigned contacts, unsigned y)
{
    s_now_ms = time;
    board_touch_snapshot_t sample = {.sequence = sequence, .timestamp_ms = time,
        .valid = true, .count = (uint8_t)contacts,
        .contacts = {{.id = 3, .x = 100, .y = (uint16_t)y},
                     {.id = 7, .x = 200, .y = (uint16_t)y}}};
    assert(submit_contacts(&sample)); consume_one();
}

static void wheel_slide_and_arrow_tap_reports(void)
{
    for (int transport = APP_INPUT_BLUETOOTH; transport <= APP_INPUT_USB; ++transport) {
        transport_setup(INPUT_MOUSE, (app_input_transport_t)transport);
        app_intent_t contact = make_intent(APP_TOUCH_BEGIN, 440, 210, NULL);
        contact.surface = APP_SURFACE_CONTROL;
        contact.gesture = 8001;
        deliver(contact);
        app_intent_t wheel = contact;
        wheel.action = APP_SCROLL;
        const int distance[] = {2, 1, -4};
        for (size_t i = 0; i < sizeof(distance) / sizeof(distance[0]); ++i) {
            wheel.x = distance[i];
            deliver(wheel);
            const uint8_t report[] = {0, 0, 0, (uint8_t)distance[i]};
            expect_report(2, report, sizeof(report));
        }
        contact.action = APP_TOUCH_END;
        deliver(contact);
        expect_idle();
        local_control_contact(8002);
        wheel.gesture = 8002;
        wheel.x = 1;
        deliver(wheel); deliver(wheel);
        const uint8_t up[] = {0, 0, 0, 1};
        expect_report(2, up, sizeof(up));
        expect_idle();
        assert(test_usb_send_attempts == (transport == APP_INPUT_USB ? 4U : 0U));
        assert(test_ble_send_attempts == (transport == APP_INPUT_BLUETOOTH ? 4U : 0U));
    }
}

static void full_contact_fifo_precedes_primary_and_drains_pair(void)
{
    setup(INPUT_MOUSE); s_multitouch_available = true;
    contacts_at(1, 1000, 1, 100);
    pad_at(APP_TOUCH_BEGIN, 5001, 1000, 100, 100);
    contacts_at(2, 1020, 2, 100);
    assert(s_multitouch_consuming);
    pad_at(APP_TOUCH_MOVE, 5001, 1020, 20, 0); expect_idle();
    contacts_at(3, 1040, 2, 148);
    const uint8_t wheel[4] = {0, 0, 0, 2}; expect_report(2, wheel, sizeof wheel);
    pad_at(APP_TOUCH_MOVE, 5001, 1040, 10, 20); expect_idle();
    contacts_at(4, 1050, 1, 148);
    pad_at(APP_TOUCH_MOVE, 5001, 1050, 10, 20); expect_idle();
    contacts_at(5, 1060, 0, 0);
    pad_at(APP_TOUCH_END, 5001, 1060, 110, 148); expect_idle();
    service_multitouch();
    contacts_at(6, 1100, 1, 100);
    pad_at(APP_TOUCH_BEGIN, 5002, 1100, 100, 100);
    contacts_at(7, 1120, 0, 0);
    pad_at(APP_TOUCH_END, 5002, 1120, 100, 100); pad_click_pair();
    expect_idle();
}

static void full_contact_right_click_and_cancelled_drag(void)
{
    setup(INPUT_MOUSE); s_multitouch_available = true;
    contacts_at(1, 1000, 2, 100);
    pad_at(APP_TOUCH_BEGIN, 5011, 1000, 100, 100);
    contacts_at(2, 1040, 0, 0);
    pad_at(APP_TOUCH_END, 5011, 1040, 100, 100);
    const uint8_t down[4] = {2,0,0,0}, up[4] = {0};
    expect_report(2, down, sizeof down); expect_report(2, up, sizeof up); expect_idle();
    coordinator_touch_hold(); s_multitouch_available = true;
    contacts_at(1, 1110, 1, 100);
    contacts_at(2, 1120, 2, 100);
    assert(s_engine.state.release_pending && !interaction_touch_holding(&s_interaction));
    release_barrier(); expect_idle();
    contacts_at(3, 1130, 1, 150);
    pad_at(APP_TOUCH_MOVE, 2002, 1130, 10, 0); expect_idle();
    contacts_at(4, 1140, 0, 0); expect_idle();
}

static void contact_overflow_stale_context_and_motion_keep_safe_defaults(void)
{
    setup(INPUT_MOUSE); s_multitouch_available = true;
    assert(s_settings.tap_to_click);
    contacts_at(1, 1000, 2, 100);
    s_now_ms = 1020;
    const board_touch_snapshot_t sample = {.sequence=2, .timestamp_ms=1020, .valid=true};
    assert(submit_contacts(&sample));
    action(APP_MENU, APP_OVERLAY_MODES); /* Older queued all-up cannot click in new focus. */
    while (uxQueueMessagesWaiting(s_intents)) consume_one();
    release_barrier(); expect_idle();
    setup(INPUT_MOUSE); s_multitouch_available = true;
    fill_intents(); assert(!submit_contacts(&sample));
    assert(process_priority()); release_barrier(); expect_empty_intents();
    setup(INPUT_MOUSE); s_multitouch_available = true;
    s_settings.tap_to_click = false;
    contacts_at(1, 1000, 2, 100);
    contacts_at(2, 1020, 0, 0); expect_idle();
    assert(!s_multitouch_consuming);
}

static void capture_display_permission_requires_actual_receipt_and_brightness(void)
{
    setup(INPUT_KEYBOARD);
    s_keyboard_submode = APP_KEYBOARD_SPOKEN;
    test_brightness = (board_brightness_status_t){.applied_known = true, .applied_percent = 50, .error = ESP_OK};
    test_indicator = (app_presenter_indicator_status_t){.indicator_armed = true,
        .capture_generation = 9, .owner_heartbeat_ms = 1000};
    refresh_media_context();
    assert(test_media_environment.screen_visible && !test_media_environment.indicator_visible);
    assert(test_capture_context.screen_visible && test_capture_context.armed && !test_capture_context.usable &&
        test_capture_context.capture_generation == 9 && test_capture_context_ms == s_now_ms);
    assert(test_media_speech_context && test_media_context.preview_characters == 0);
    test_indicator.usable = true;
    test_indicator.completed_us = 100; /* Old transfer remains evidence only with fresh owner heartbeat. */
    refresh_media_context();
    assert(test_media_environment.indicator_visible && test_media_environment.indicator_generation == 9);
    assert(test_media_environment.indicator_seen_ms == 1000);
    assert(test_capture_context.usable && test_capture_context.capture_generation == 9);
    test_brightness.applied_known = false;
    refresh_media_context();
    assert(!test_media_environment.screen_visible && !test_media_environment.indicator_visible);
    assert(!test_capture_context.screen_visible && test_capture_context.usable); /* Independent observations stay distinct. */
    test_brightness.applied_known = true;
    s_idle.wake_capture = true;
    refresh_media_context();
    assert(!test_media_environment.indicator_visible);
    s_idle.wake_capture = false;
    s_overlay = APP_OVERLAY_SETTINGS;
    refresh_media_context();
    assert(!test_media_speech_context); /* Native preview cannot be accepted behind a modal. */
}

static void capture_keeps_display_awake_until_confirmed_off_without_activity(void)
{
    setup(INPUT_AUDIO);
    test_capabilities.brightness = true;
    s_settings.idle_timeout = DEVICE_IDLE_1_MIN;
    assert(device_idle_configure(&s_idle, s_settings.idle_timeout, s_now_ms));
    uint64_t last_activity = s_idle.last_activity_ms;
    test_media_snapshot = (app_media_snapshot_t){.initialized = true, .audio_known = true};
    test_media_snapshot.audio.capture_indicator_required = true;
    s_now_ms += 60001;
    service_display();
    assert(!s_idle.asleep && s_idle.last_activity_ms == last_activity);
    test_media_snapshot.audio.capture_indicator_required = false;
    service_display();
    assert(s_idle.asleep); /* Normal saved timeout resumes without synthetic activity. */
}

static void physical_lock_stops_capture_but_release_only_does_not(void)
{
    setup(INPUT_MOUSE);
    s_priority_requests = RELEASE_REQUEST;
    assert(process_priority());
    assert(test_media_stops == 0);
    s_priority_requests = LOCK_REQUEST;
    assert(process_priority());
    assert(test_media_stops == 1 && s_engine.state.locked);
    refresh_media_context();
    assert(!test_media_environment.unlocked && test_media_environment.privacy_latched);
}

static void media_notice_clear_preserves_other_status_owners(void)
{
    setup(INPUT_AUDIO);
    app_media_controls_effect_t notice = {.handled = true};
    strcpy(notice.message, "Audio updated");
    apply_media_effect(&notice);
    assert(s_message == s_media_message && !strcmp(s_message, "Audio updated"));
    app_media_controls_effect_t unchanged = {0};
    apply_media_effect(&unchanged);
    assert(!strcmp(s_message, "Audio updated"));
    app_media_controls_effect_t clear = {.handled = true};
    apply_media_effect(&clear);
    assert(s_message == s_media_message && !*s_message);
    apply_media_effect(&notice);
    const char *other = "Wi-Fi connected";
    s_message = other;
    apply_media_effect(&clear);
    assert(s_message == other && !strcmp(s_message, "Wi-Fi connected") && !*s_media_message);
    apply_media_effect(&notice);
    assert(s_message == s_media_message && !strcmp(s_message, "Audio updated"));
}

static void media_preview_confirmation_uses_normal_input_context(void)
{
    setup(INPUT_KEYBOARD);
    s_keyboard_submode = APP_KEYBOARD_SPOKEN;
    test_media_handle = true;
    test_media_effect = (app_media_controls_effect_t){.handled = true, .preview_character = 'Q'};
    action(APP_VOICE_CONFIRM, 0);
    assert(strcmp(s_interaction.view.preview, "Q") == 0 && trace_count == 0);
    assert(!s_send_token && !s_interaction.view.sending);
    unsigned handled = test_media_handles;
    s_overlay = APP_OVERLAY_SETTINGS;
    action(APP_VOICE_CONFIRM, 0);
    assert(test_media_handles == handled && strcmp(s_interaction.view.preview, "Q") == 0);
    s_overlay = APP_OVERLAY_NONE;
    hid_engine_set_locked(&s_engine, true);
    action(APP_VOICE_CONFIRM, 0);
    assert(test_media_handles == handled);
}

static void remote_initializers_honor_alignment_and_clean_allocation_failures(void)
{
    setup(INPUT_MOUSE);
    void *weak = heap_caps_calloc(1, 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(weak && ((uintptr_t)weak & 7U) == 4U);
    heap_caps_free(weak);
    for (size_t failure = 0; failure <= 5; ++failure) {
        setup(INPUT_MOUSE);
        test_media_authority_available = true;
        test_heap_fail_at = failure;
        remote_control_init(false);
        assert(test_heap_calls == 5 && test_heap_aligned_calls == 5);
        if (failure == 0) {
            assert(s_remote_input && s_command_bridge && s_remote_device && s_remote_media && s_remote_media_preview);
            assert((uintptr_t)s_remote_input % app_remote_input_alignment() == 0);
            assert((uintptr_t)s_command_bridge % app_command_bridge_alignment() == 0);
            assert((uintptr_t)s_remote_device % app_remote_device_alignment() == 0);
            assert((uintptr_t)s_remote_media % app_remote_media_alignment() == 0);
            assert((uintptr_t)s_remote_media_preview % _Alignof(interaction_t) == 0);
            assert(test_heap_outstanding == 5);
        } else if (failure == 1) {
            assert(!s_remote_device && s_remote_input && s_command_bridge && s_remote_media && s_remote_media_preview);
            assert(test_heap_outstanding == 4);
        } else if (failure <= 3) {
            assert(!s_remote_media && !s_remote_media_preview && s_remote_device && s_remote_input && s_command_bridge);
            assert(test_heap_outstanding == 3);
        } else {
            assert(!s_remote_input && !s_command_bridge && s_remote_device && s_remote_media && s_remote_media_preview);
            assert(test_heap_outstanding == 3);
        }
        /* Independent owners retain valid contexts; rejected temporary input,
         * bridge or media/preview storage is already released. Teardown then
         * permits a complete fresh initialization without leaked allocations. */
        setup(INPUT_MOUSE);
        test_media_authority_available = true;
        remote_control_init(false);
        assert(test_heap_outstanding == 5 && s_remote_input && s_command_bridge && s_remote_device &&
               s_remote_media && s_remote_media_preview);
    }
    setup(INPUT_MOUSE);
    assert(!test_heap_outstanding);
}

static cp_command_t remote_test_command(cp_operation_id_t operation, uint64_t key, const char *arguments)
{
    app_remote_input_state_t observed; assert(remote_observe(NULL, &observed));
    cp_command_t command = {.version = CP_API_VERSION, .operation = operation, .command_key = key,
        .deadline_ms = s_now_ms + 1000, .input_epoch = s_engine.state.epoch,
        .transport_generation = observed.transport_generation, .ui_revision = s_ui_revision,
        .management_generation = s_management_generation, .lease_expires_ms = s_now_ms + 3000};
    command.identity.principal_id[0] = 11; command.identity.session_id[0] = 12;
    command.identity.scopes = CP_SCOPE_ALL; command.lease_id[0] = 13;
    memcpy(command.target_id, observed.target_id, sizeof(command.target_id));
    (void)snprintf(command.args_json, sizeof(command.args_json), "%s", arguments);
    return command;
}
static void remote_acquire_producer(const char *producer)
{
    remote_control_init(false); test_control_session_alive = true;
    assert(set_remote_allowance(true));
    remote_control_service();
    char args[128];
    if (producer) (void)snprintf(args, sizeof(args), "{\"state_sequence\":\"%llu\",\"producer\":\"%s\"}",
        (unsigned long long)s_state_sequence, producer);
    else (void)snprintf(args, sizeof(args), "{\"state_sequence\":\"%llu\"}",
        (unsigned long long)s_state_sequence);
    cp_command_t command = remote_test_command(CP_OP_CONTROL_ACQUIRE, 1, args);
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service();
    if (s_engine.state.owner != INPUT_REMOTE || !s_engine.state.release_pending)
        fprintf(stderr, "acquire owner=%d release=%d error=%d reason=%s sequence=%llu\n",
            s_engine.state.owner, s_engine.state.release_pending, test_control_result.error,
            test_control_result.reason, (unsigned long long)s_state_sequence);
    assert(s_engine.state.owner == INPUT_REMOTE && s_engine.state.release_pending);
    release_barrier();
    remote_control_service();
    app_remote_input_view_t remote; app_remote_input_snapshot(s_remote_input, &remote);
    assert(remote.active && !remote.pending && test_control_state.remote_owner);
    assert(test_control_result.command_key == 1 && test_control_result.state == CP_RESULT_APPLIED);
}
static void remote_acquire_current_host(void) { remote_acquire_producer(NULL); }

static void remote_imu_uses_the_single_board_sample_and_hid_queue(void)
{
    setup(INPUT_MOUSE); calibrate(); remote_acquire_producer("device_imu");
    cp_command_t motion = remote_test_command(CP_OP_INPUT_MOTION_SET, 2, "{\"enabled\":true}");
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &motion, &result) == CP_OK);
    remote_control_service(); sync_interaction();
    assert(!s_engine.state.release_pending);
    assert(s_engine.state.gyro_enabled && s_engine.state.owner == INPUT_REMOTE);
    for (unsigned i = 0; i < 15; ++i) { sample_after(10, 0, 0); expect_idle(); }
    sample_after(10, 25, 0);
    const uint8_t moved[] = {0, 2, 0, 0};
    expect_report(2, moved, sizeof moved);
    process_motion(); expect_idle(); /* The cached board sample is consumed once. */
    remote_local_override(false); release_barrier(); sync_interaction();
    sample_after(10, 25, 0); expect_idle();
    assert(!s_engine.state.gyro_enabled && !s_remote_allowed);
}
static void remote_recenter_uses_shared_owner_and_duplicate_never_restarts_acquisition(void)
{
    setup(INPUT_MOUSE); calibrate(); remote_acquire_producer("device_imu");
    cp_command_t command = remote_test_command(CP_OP_INPUT_MOTION_SET, 2, "{\"enabled\":true}");
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service(); settle_motion_reference();
    const uint64_t epoch = s_engine.state.epoch;
    command = remote_test_command(CP_OP_INPUT_MOTION_RECENTER, 3, "{}");
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service();
    assert(test_control_result.state == CP_RESULT_APPLIED && test_control_result.error == CP_OK);
    assert(strcmp(test_control_result.reason, "motion_reference_reset") == 0);
    assert(s_reference_diagnostics.recenter_resets == 1 && !s_interaction.gravity_ready);
    assert(s_engine.state.epoch == epoch && s_engine.state.gyro_enabled);
    sample_after(10, 0, 0); expect_idle();
    const size_t acquire_samples = s_interaction.gravity_acquire_samples;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service();
    assert(s_reference_diagnostics.recenter_resets == 1 && s_interaction.gravity_acquire_samples == acquire_samples);
    for (unsigned i = 0; i < 12; ++i) { sample_after(10, 0, 0); expect_idle(); }
    assert(s_interaction.gravity_ready && !s_reference_diagnostics.first_active.known);
    command = remote_test_command(CP_OP_INPUT_MOTION_RECENTER, 4, "{}");
    --command.input_epoch;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service();
    assert(s_reference_diagnostics.recenter_resets == 1 && s_interaction.gravity_ready);
}

static void remote_commands_share_actual_hid_queue_and_physical_takeover(void)
{
    setup(INPUT_MOUSE); remote_acquire_current_host();
    cp_command_t click = remote_test_command(CP_OP_MOUSE_CLICK, 2, "{\"button\":\"left\"}");
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &click, &result) == CP_OK);
    remote_control_service();
    const uint8_t down[] = {1, 0, 0, 0}, up[] = {0, 0, 0, 0};
    expect_report(2, down, sizeof(down));
    expect_report(2, up, sizeof(up));
    remote_control_service();
    assert(test_control_result.command_key == 2 && test_control_result.state == CP_RESULT_SENT);
    cp_command_t queued = remote_test_command(CP_OP_MOUSE_CLICK, 3, "{\"button\":\"left\"}");
    assert(app_command_bridge_submit(s_command_bridge, &queued, &result) == CP_OK);
    remote_local_override(false);
    remote_control_service();
    assert(s_engine.state.owner == INPUT_LOCAL && test_media_stops == 0);
    release_barrier(); expect_idle();
    assert(test_control_result.command_key == 3 && test_control_result.error == CP_STALE);
}

static void remote_allowance_requires_local_restore_after_takeover(void)
{
    setup(INPUT_MOUSE);
    remote_control_init(false); test_control_session_alive = true;
    remote_control_service();
    assert(!test_control_state.remote_allowed);
    const uint64_t initial = test_control_state.allowance_generation;
    assert(set_remote_allowance(true));
    remote_control_service();
    assert(test_control_state.remote_allowed && test_control_state.allowance_generation > initial);
    const uint64_t granted = test_control_state.allowance_generation;
    remote_local_override(false);
    remote_control_service();
    assert(!test_control_state.remote_allowed && test_control_state.allowance_generation > granted);
    const uint64_t revoked = test_control_state.allowance_generation;
    remote_control_service();
    assert(!test_control_state.remote_allowed && test_control_state.allowance_generation == revoked);
    assert(set_remote_allowance(true));
    remote_control_service();
    assert(test_control_state.remote_allowed && test_control_state.allowance_generation > revoked);
    remote_local_override(true);
    hid_engine_set_locked(&s_engine, true);
    assert(!set_remote_allowance(true));
    hid_engine_set_locked(&s_engine, false);
    s_idle.asleep = true; assert(!set_remote_allowance(true));
    s_idle.asleep = false; s_remote_allowance_generation = UINT64_MAX - 1;
    assert(!set_remote_allowance(true) && !s_remote_allowed);
    remote_control_service();
    assert(!test_control_state.remote_allowed && test_control_state.allowance_generation == UINT64_MAX);
}

static void remote_allowance_ui_checks_identity_and_returns_core(void)
{
    setup(INPUT_MOUSE); remote_control_init(false);
    s_overlay = APP_OVERLAY_API_CONNECTION;
    app_intent_t toggle = make_intent(APP_API_INPUT_ALLOW_SET, 1, 0, NULL);
    toggle.api.revision = s_remote_allowance_generation + 1;
    handle_intent(&toggle, &test_link);
    assert(!s_remote_allowed && s_overlay == APP_OVERLAY_API_CONNECTION);
    toggle.api.revision = s_remote_allowance_generation;
    handle_intent(&toggle, &test_link);
    assert(s_remote_allowed && s_overlay == APP_OVERLAY_NONE);
    assert(s_engine.state.release_pending);
    remote_local_override(false); release_barrier(); sync_interaction();
    toggle = make_intent(APP_API_INPUT_ALLOW_SET, 1, 0, NULL);
    toggle.api.revision = s_remote_allowance_generation;
    handle_intent(&toggle, &test_link);
    assert(!s_remote_allowed); /* Core cannot impersonate the local settings action. */
}

static void remote_transport_rechecks_authority_at_effect_boundary(void)
{
    for (unsigned fault = 0; fault < 4; ++fault) {
        setup(INPUT_MOUSE); remote_acquire_current_host();
        test_usb.initialized = true; test_usb.failed = false;
        cp_command_t command = remote_test_command(CP_OP_INPUT_TRANSPORT_SELECT, 2, "{\"transport\":\"usb\"}");
        cp_result_t result;
        assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
        remote_control_service();
        assert(s_remote_transport_pending && s_transport_switch_pending);
        if (fault == 1) remote_local_override(false);
        if (fault == 2) test_control_session_alive = false;
        if (fault == 3) s_now_ms = command.deadline_ms;
        release_barrier(); service_transport_switch(); remote_control_service();
        assert(!s_transport_switch_pending && !s_remote_transport_pending);
        assert(test_control_result.command_key == 2);
        if (!fault) {
            assert(s_input_transport == APP_INPUT_USB && test_control_result.state == CP_RESULT_APPLIED);
        } else {
            assert(s_input_transport == APP_INPUT_BLUETOOTH && test_control_result.state == CP_RESULT_CANCELLED);
            assert(test_control_result.error == (fault == 3 ? CP_EXPIRED : CP_STALE));
        }
    }
}
static void remote_device_routes_and_state_survive_missing_media(void)
{
    setup(INPUT_MOUSE); remote_control_init(false); test_control_session_alive = true; remote_control_service();
    app_remote_device_state_t state;
    assert(remote_device_observe(NULL, CP_OP_BLUETOOTH_PAIRING_OPEN, &state));
    cp_command_t command = remote_test_command(CP_OP_BLUETOOTH_PAIRING_OPEN, 10, "{\"window_seconds\":120}");
    command.resource_revision = state.resource_revision;
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service();
    assert(test_control_result.command_key == 10 && test_control_result.error == CP_OK);
    assert(s_overlay == APP_OVERLAY_CONFIRM && s_remote_device_local_review[0] && !test_pair_opens);
    char json[4096]; size_t length;
    assert(command_read(NULL, &command.identity, "/api/v1/state", json, sizeof(json), &length) == CP_OK);
    assert(length == strlen(json) && strstr(json, "\"device_controls\":{") &&
        strstr(json, "\"media_status\":\"unavailable\"") && strstr(json, "retained_manifest_digest"));
    command.identity.scopes = 0;
    assert(command_read(NULL, &command.identity, "/api/v1/state", json, sizeof(json), &length) == CP_FORBIDDEN);
}

static void native_bluetooth_resume_and_selection_release_effect(void)
{
    setup(INPUT_MOUSE); settings_page(APP_SETTINGS_BLUETOOTH);
    test_link.connection_admitted = test_link.connected = false; test_link.ready = false; test_link.reconnect_paused = true;
    action(APP_PAIRING_REQUEST, APP_PAIR_RESUME);
    assert(!test_link.reconnect_paused && !test_disconnects);
    test_link.connection_admitted = test_link.connected = true; test_link.reconnect_paused = true;
    action(APP_PAIRING_REQUEST, APP_PAIR_RESUME);
    assert(test_link.reconnect_paused && test_link.connected && !test_disconnects);
    setup(INPUT_MOUSE); cancel_input(); release_barrier(); assert(!s_engine.state.release_pending);
    app_connectivity_controls_effect_t effect = {.handled = true, .release_input = true};
    apply_connectivity_effect(&effect);
    assert(s_engine.state.release_pending); /* Accepted local selection cancels input, never acknowledges it. */
}

static void review_forget_all_survives_provisional_churn(void)
{
    for (unsigned changed = 0; changed < 3; ++changed) {
        setup(INPUT_MOUSE); settings_page(APP_SETTINGS_BLUETOOTH);
        test_bonds = (ble_hid_bond_list_t){.known = true, .count = 2, .revision = 51};
        test_update_target = (ble_hid_target_snapshot_t){.loaded = true, .durable = true, .revision = 61};
        test_link.connection_admitted = test_link.ready = false;
        hid_engine_set_link(&s_engine, false, false); sync_interaction();
        action(APP_PAIRING_REQUEST, APP_PAIR_FORGET_ALL);
        assert(s_confirmation == APP_CONFIRM_PAIR_FORGET_ALL && !test_native_management_starts);
        test_link.connected = false; ++test_link.generation;
        if (changed == 1) test_link.connection_admitted = test_link.connected = test_link.ready = true;
        if (changed == 2) ++test_bonds.revision;
        action(APP_CONFIRM, 1);
        if (changed == 1) assert(!test_native_management_starts); /* An admitted replacement revokes review. */
        else {
            assert(test_native_management_starts == 1 && test_native_management_active == (changed == 0));
            assert(test_native_operation == APP_PAIR_FORGET_ALL && test_native_generation == 0);
            assert(test_native_bond_revision == 51 && test_native_preference_revision == 61);
        }
        assert(!test_pair_forgets && test_bonds.count == 2);
    }
}

static void review_disconnect_host_leaves_before_pause(void)
{
    start_confirmed_pair_change(APP_PAIR_DISCONNECT);
    const ble_hid_snapshot_t original = test_link;
    test_link.connection_admitted = test_link.connected = test_link.ready = false;
    ++test_link.generation;
    service_pair_operation(&original);
    assert(!test_disconnects && !test_link.reconnect_paused && s_pair_operation == APP_CONFIRM_NONE);
    fprintf(stderr, "Unaccepted pause result: %s\n", s_message);
    assert(!strstr(s_message, "automatic reconnect paused") && !strstr(s_message, "Resume"));
    render_view(&test_link, ESP_OK);
    assert(!test_view.bluetooth_reconnect_paused && strstr(test_view.message, "reconnect policy is unchanged"));
}

static void provisional_bluetooth_retries_preserve_local_intents(void)
{
    setup(INPUT_MOUSE); settings_page(APP_SETTINGS_BLUETOOTH);
    test_link.connection_admitted = test_link.connected = test_link.ready = false;
    input_link_t projected = input_link_snapshot(&test_link);
    observe_input_link(&projected, &test_previous_link);
    const uint64_t epoch = s_engine.state.epoch;
    test_link.reconnect_paused = true;
    app_intent_t resume = make_intent(APP_PAIRING_REQUEST, APP_PAIR_RESUME, 0, NULL);
    assert(submit_intent(&resume));
    for (unsigned i = 0; i < 200; ++i) {
        test_link.connected = (i & 1) != 0; /* A raw candidate has no admitted-host authority. */
        ++test_link.generation;
        projected = input_link_snapshot(&test_link);
        observe_input_link(&projected, &test_previous_link);
        assert(!projected.connected && !projected.ready && projected.generation == 0);
        assert(s_engine.state.epoch == epoch && uxQueueMessagesWaiting(s_intents) == 1);
        render_view(&test_link, ESP_OK);
        assert(!test_view.bluetooth_connected && test_view.bluetooth_reconnect_paused);
    }
    consume_one();
    assert(!test_link.reconnect_paused && !test_disconnects && !s_pair_operation);
    test_link.connected = true;
    action(APP_PAIRING_REQUEST, APP_PAIR_DISCONNECT);
    assert(!s_pair_operation && s_overlay == APP_OVERLAY_SETTINGS && !test_disconnects);
    test_link.connection_admitted = test_link.ready = true;
    projected = input_link_snapshot(&test_link);
    observe_input_link(&projected, &test_previous_link);
    assert(projected.connected && projected.ready && projected.generation == test_link.generation);
    assert(s_engine.state.epoch != epoch);
}

static void native_connection_review_and_back_navigation_reach_owner(void)
{
    setup(INPUT_MOUSE); settings_page(APP_SETTINGS_WIFI);
    action(APP_MENU, APP_OVERLAY_API_CONNECTION);
    action(APP_MENU, APP_OVERLAY_API_REVIEW);
    assert(s_overlay == APP_OVERLAY_API_REVIEW);
    action(APP_CLOSE, 0); settings_page(APP_SETTINGS_WIFI);
    action(APP_MENU, APP_OVERLAY_API_CONNECTION);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_BLUETOOTH);
    assert(s_overlay == APP_OVERLAY_API_CONNECTION);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_WIFI);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_WIFI);
    open_overlay(APP_OVERLAY_BLUETOOTH_TARGET);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_WIFI);
    assert(s_overlay == APP_OVERLAY_BLUETOOTH_TARGET);
    action(APP_SETTINGS_PAGE, APP_SETTINGS_BLUETOOTH);
    assert(s_overlay == APP_OVERLAY_SETTINGS && s_settings_page == APP_SETTINGS_BLUETOOTH);
}

static void cancelled_remote_receipt_cannot_hold_a_new_local_switch(void)
{
    setup(INPUT_MOUSE); remote_acquire_current_host(); test_usb.initialized = true; test_usb.failed = false;
    cp_command_t command = remote_test_command(CP_OP_INPUT_TRANSPORT_SELECT, 2, "{\"transport\":\"usb\"}");
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service();
    test_control_completion_reject = true;
    remote_local_override(false); remote_control_service();
    assert(s_remote_transport_pending && s_remote_transport_cancelled && !s_transport_switch_pending);
    assert(request_input_transport(APP_INPUT_USB, &test_link));
    release_barrier(); service_transport_switch();
    assert(s_input_transport == APP_INPUT_USB && !s_transport_switch_pending);
    test_control_completion_reject = false; remote_control_service();
    assert(!s_remote_transport_pending && test_control_result.command_key == 2 &&
        test_control_result.state == CP_RESULT_CANCELLED && test_control_result.error == CP_STALE);
}

static void applied_remote_receipt_survives_later_local_selection(void)
{
    setup(INPUT_MOUSE); remote_acquire_current_host(); test_usb.initialized = true; test_usb.failed = false;
    cp_command_t command = remote_test_command(CP_OP_INPUT_TRANSPORT_SELECT, 2, "{\"transport\":\"usb\"}");
    cp_result_t result;
    assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
    remote_control_service(); test_control_completion_reject = true;
    release_barrier(); service_transport_switch(); remote_control_service();
    assert(s_remote_transport_pending && s_remote_transport_effect && s_input_transport == APP_INPUT_USB);
    remote_local_override(false);
    assert(request_input_transport(APP_INPUT_BLUETOOTH, &test_link));
    /* The selected USB target is disconnected; it cannot acknowledge release. */
    service_transport_switch();
    assert(s_input_transport == APP_INPUT_BLUETOOTH);
    test_control_completion_reject = false; remote_control_service();
    assert(!s_remote_transport_pending && test_control_result.command_key == 2 &&
        test_control_result.state == CP_RESULT_APPLIED && test_control_result.error == CP_OK);
}

#include "update_cases.inc"
#include "navigation_cases.inc"
#include "remote_device_cases.inc"

static void consumed_event_workspace_does_not_retain_private_payloads(void)
{
    setup(INPUT_KEYBOARD);
    coordinator_event_t event;
    memset(&event, 0xa5, sizeof(event));
    event.contacts = false;
    event.value.intent = make_intent(APP_TEXT, 0, 0, "AZ");
    consume_coordinator_event(&event, &test_link);
    const uint8_t *bytes = (const uint8_t *)&event;
    for (size_t i = 0; i < sizeof(event); ++i) assert(bytes[i] == 0);
    static const uint8_t a[] = {2, 0, 4, 0, 0, 0, 0, 0};
    static const uint8_t z[] = {2, 0, 29, 0, 0, 0, 0, 0};
    static const uint8_t zero[8] = {0};
    expect_report(1, a, sizeof(a)); expect_report(1, zero, sizeof(zero));
    expect_report(1, z, sizeof(z)); expect_report(1, zero, sizeof(zero));
    expect_idle();

    memset(&event, 0xa5, sizeof(event));
    event.contacts = true;
    event.value.touch.sample.epoch = s_engine.state.epoch - 1;
    consume_coordinator_event(&event, &test_link);
    for (size_t i = 0; i < sizeof(event); ++i) assert(bytes[i] == 0);
    expect_idle();

    event.value.intent = make_intent(APP_LOCK, 1, 0, "private-event-marker");
    consume_coordinator_event(&event, &test_link);
    assert(process_priority() && s_engine.state.locked);
    for (size_t i = 0; i < sizeof(event); ++i) assert(bytes[i] == 0);
}

static void build_identity_is_independent_of_link_and_artifact_version(void)
{
    transport_setup(INPUT_MOUSE, APP_INPUT_USB);
    strcpy(s_firmware_version, "fixture-version");
    strcpy(s_build_id, "012345abcdef");
    test_link.connection_admitted = test_link.connected = test_link.ready = false;
    test_usb.mounted = test_usb.ready = false;
    hid_engine_set_link(&s_engine, false, false);
    render_view(&test_link, ESP_OK);
    assert(strcmp(test_view.firmware_version, "fixture-version") == 0);
    assert(strcmp(test_view.build_id, "012345abcdef") == 0);
    assert(strcmp(test_view.developer, APP_BUILD_DEVELOPER) == 0);
    assert(strcmp(test_view.repository_url, APP_BUILD_REPOSITORY_URL) == 0);
    assert(strcmp(test_view.git_commit, APP_BUILD_GIT_COMMIT) == 0);
    assert(test_view.git_state == (!APP_BUILD_GIT_STATE_KNOWN ? APP_GIT_UNKNOWN :
        APP_BUILD_GIT_DIRTY ? APP_GIT_DIRTY : APP_GIT_CLEAN));
    assert(!test_view.connected && !test_view.ready);
    assert(test_storage_requests == 0 && trace_count == 0);
}

#if defined(CONFIG_APP_IMU_CAPTURE) && CONFIG_APP_IMU_CAPTURE
#include "motion_capture_cases.inc"
#endif

static void sensor_observers_preserve_input_queue_and_redact_non_pad_context(void)
{
    setup(INPUT_MOUSE);
    test_sensor_buttons = test_sensor_contacts = 0;
    board_touch_snapshot_t sample = {.sequence = 1, .timestamp_ms = 2000, .count = 1, .valid = true,
        .contacts = {{.id = 0, .x = 100, .y = 100}}};
    unsigned queued = uxQueueMessagesWaiting(s_intents);
    assert(submit_contacts(&sample) && test_sensor_public && test_sensor_contacts == 1);
    assert(uxQueueMessagesWaiting(s_intents) == queued + 1);
    s_overlay = APP_OVERLAY_SETTINGS;
    ++sample.sequence;
    assert(submit_contacts(&sample) && !test_sensor_public && test_sensor_contacts == 2);
    assert(uxQueueMessagesWaiting(s_intents) == queued + 2);
    s_overlay = APP_OVERLAY_NONE;
    sample.contacts[0].x = 600;
    assert(submit_contacts(&sample) && !test_sensor_public && test_sensor_contacts == 3);
    unsigned edges = s_button_count;
    physical_button(BOARD_BUTTON_DOWN, 3000000);
    physical_button(BOARD_BUTTON_UP, 3100000);
    assert(test_sensor_buttons == 2 && s_button_count == edges + 2 && !s_physical_down);
    assert(trace_count == 0);
}

#include "ble_advertising_diagnostics_cases.inc"
#include "ble_admission_diagnostics_cases.inc"

static void keyboard_test_select_mode(bool remote, input_mode_t mode, uint64_t key)
{
    if (remote) {
        const char *args = mode == INPUT_KEYBOARD ? "{\"mode\":\"keyboard\"}" :
            mode == INPUT_MOUSE ? "{\"mode\":\"mouse\"}" : "{\"mode\":\"audio\"}";
        cp_command_t command = remote_test_command(CP_OP_MODE_SELECT, key, args);
        cp_result_t result;
        assert(app_command_bridge_submit(s_command_bridge, &command, &result) == CP_OK);
        remote_control_service();
        assert(test_control_result.command_key == key && test_control_result.state == CP_RESULT_APPLIED);
    } else {
        action(APP_MENU, APP_OVERLAY_MODES);
        action(APP_MODE, (int)mode);
    }
    if (s_engine.state.release_pending) release_barrier();
    sync_interaction();
    assert(s_engine.state.mode == mode);
}

static void keyboard_entry_defaults_to_touch_for_local_and_remote_modes(void)
{
    const input_mode_t origins[] = {INPUT_MOUSE, INPUT_AUDIO};
    for (unsigned remote = 0; remote < 2; ++remote) {
        for (size_t origin = 0; origin < sizeof origins / sizeof origins[0]; ++origin) {
            for (int previous = APP_KEYBOARD_MOTION; previous <= APP_KEYBOARD_SHORTCUTS; ++previous) {
                setup(INPUT_KEYBOARD);
                fill_preview(4);
                action(APP_KEYBOARD_SUBMODE, previous);
                release_barrier(); sync_interaction();
                if (remote) remote_acquire_current_host();
                keyboard_test_select_mode(remote != 0, origins[origin], 2);
                keyboard_test_select_mode(remote != 0, INPUT_KEYBOARD, 3);
                assert(s_keyboard_submode == APP_KEYBOARD_TOUCH && !s_interaction.view.keyboard_motion);
                assert(s_interaction.view.group == -1 && !s_engine.state.gyro_enabled);
                assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
                render_view(&test_link, ESP_OK);
                assert(test_view.keyboard_submode == APP_KEYBOARD_TOUCH && test_view.keyboard_group == -1);
                if (!remote) {
                    action(APP_GRID, 0);
                    action(APP_GRID, 1);
                    assert(strcmp(s_interaction.view.preview, "ABABB") == 0);
                }
                expect_idle();
                expect_neutral_navigation_trace();
            }
        }
    }
}

static void keyboard_session_navigation_preserves_explicit_submode_and_draft(void)
{
    const app_overlay_t panels[] = {APP_OVERLAY_MODES, APP_OVERLAY_SETTINGS,
        APP_OVERLAY_COMMON_KEYS, APP_OVERLAY_KEYBOARD, APP_OVERLAY_TARGET};
    for (int submode = APP_KEYBOARD_MOTION; submode <= APP_KEYBOARD_SHORTCUTS; ++submode) {
        setup(INPUT_KEYBOARD);
        fill_preview(4);
        action(APP_KEYBOARD_SUBMODE, submode);
        release_barrier(); sync_interaction();
        for (size_t panel = 0; panel < sizeof panels / sizeof panels[0]; ++panel) {
            action(APP_MENU, (int)panels[panel]);
            assert(s_overlay == panels[panel]);
            action(APP_CLOSE, 0);
            release_barrier(); sync_interaction();
            assert(s_overlay == APP_OVERLAY_NONE && s_keyboard_submode == (app_keyboard_submode_t)submode);
            assert(s_interaction.view.keyboard_motion == (submode == APP_KEYBOARD_MOTION));
            assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
        }
        keyboard_test_select_mode(false, INPUT_KEYBOARD, 0);
        assert(s_keyboard_submode == (app_keyboard_submode_t)submode);
        assert(s_interaction.view.keyboard_motion == (submode == APP_KEYBOARD_MOTION));
        remote_acquire_current_host();
        keyboard_test_select_mode(true, INPUT_KEYBOARD, 2);
        assert(s_keyboard_submode == (app_keyboard_submode_t)submode);
        assert(s_interaction.view.keyboard_motion == (submode == APP_KEYBOARD_MOTION));
        assert(strcmp(s_interaction.view.preview, "ABAB") == 0 && !s_send_token);
        expect_idle();
        expect_neutral_navigation_trace();
    }
}

int main(int argc, char **argv)
{
    assert(argc <= 2);
    unsigned passed = 0;
#define RUN(test) do { if (argc == 1 || strcmp(argv[1], #test) == 0) { \
    test_case = #test; test(); ++passed; } } while (0)
    RUN(advertising_failure_replay_preserves_domains_and_input);
    RUN(admission_failure_replay_is_bounded_repeatable_and_observation_only);
    RUN(keyboard_entry_defaults_to_touch_for_local_and_remote_modes);
    RUN(keyboard_session_navigation_preserves_explicit_submode_and_draft);
    REMOTE_UPDATE_CASES(RUN);
    NAVIGATION_CASES(RUN);
#if defined(CONFIG_APP_IMU_CAPTURE) && CONFIG_APP_IMU_CAPTURE
    RUN(raw_capture_observes_without_mutating_production_input);
    RUN(raw_capture_full_and_dump_pause_do_not_stop_motion);
    RUN(raw_capture_delivery_failure_is_not_a_hardware_or_input_failure);
#endif
    RUN(build_identity_is_independent_of_link_and_artifact_version);
    RUN(applied_remote_receipt_survives_later_local_selection);
    RUN(cancelled_remote_receipt_cannot_hold_a_new_local_switch);
    REMOTE_DEVICE_CASES(RUN);
    RUN(native_bluetooth_resume_and_selection_release_effect);
    RUN(provisional_bluetooth_retries_preserve_local_intents);
    RUN(review_forget_all_survives_provisional_churn);
    RUN(review_disconnect_host_leaves_before_pause);
    RUN(native_connection_review_and_back_navigation_reach_owner);
    RUN(remote_transport_rechecks_authority_at_effect_boundary);
    RUN(remote_device_routes_and_state_survive_missing_media);
    RUN(remote_commands_share_actual_hid_queue_and_physical_takeover);
    RUN(remote_imu_uses_the_single_board_sample_and_hid_queue);
    RUN(remote_recenter_uses_shared_owner_and_duplicate_never_restarts_acquisition);
    RUN(remote_allowance_requires_local_restore_after_takeover);
    RUN(remote_allowance_ui_checks_identity_and_returns_core);
    RUN(capture_display_permission_requires_actual_receipt_and_brightness);
    RUN(capture_keeps_display_awake_until_confirmed_off_without_activity);
    RUN(physical_lock_stops_capture_but_release_only_does_not);
    RUN(media_preview_confirmation_uses_normal_input_context);
    RUN(media_notice_clear_preserves_other_status_owners);
    RUN(saturated_emergency_admission);
    RUN(stale_ui_epoch);
    RUN(touch_cannot_unlock);
    RUN(physical_hold_toggle);
    RUN(simultaneous_lock_wins);
    RUN(lock_releases_before_later_work);
    RUN(stale_context_and_modal_focus);
    RUN(preview_editing_and_full_send);
    RUN(interrupted_send_retains_only_unaccepted_suffix);
    RUN(touch_precision_and_drag_boundaries);
    RUN(raw_button_timing_and_takeover);
    RUN(motion_gates_and_local_keyboard_focus);
    RUN(fresh_sensor_after_consumer_gap_requires_new_reference);
    RUN(rendered_state_is_truthful);
    RUN(motion_recovery_requires_fresh_samples_and_explicit_enable);
    RUN(touch_lifecycle_rejects_ended_gesture);
    RUN(touch_release_has_one_freeze_interval);
    RUN(physical_down_freezes_before_classification);
    RUN(same_or_invalid_mode_preserves_active_send);
    RUN(short_congestion_preserves_preview_send);
    RUN(short_congestion_preserves_motion_intent);
    RUN(modal_button_press_is_not_retargeted_after_close);
    RUN(local_touch_takeover_consumes_its_gesture);
    RUN(preview_survives_mode_modal_and_link_cancellation);
    RUN(physical_confirm_uses_visible_keyboard_submode);
    RUN(shortcuts_use_selected_host_modifier_and_preserve_preview);
    RUN(lost_button_edges_cannot_unlock);
    RUN(theme_changes_are_settings_scoped);
    RUN(queued_double_click_then_hold_keeps_edge_time);
    RUN(completed_quick_touch_still_freezes_motion);
    RUN(settings_pages_validate_and_persist_changes);
    RUN(idle_deadlines_ignore_passive_status_updates);
    RUN(display_sleep_preserves_hid_work_and_draft);
    RUN(touch_wake_consumes_entire_gesture_until_driver_ack);
    RUN(ai_wake_double_and_hold_are_consumed);
    RUN(display_activity_clock_and_optional_driver);
    RUN(timeout_changes_start_a_full_new_interval);
    RUN(failed_wake_is_blocked_until_a_fresh_acknowledged_gesture);
    RUN(wifi_setup_lifecycle_and_secret_scope);
    RUN(wifi_forget_requires_current_explicit_confirmation);
    RUN(pairing_confirmation_releases_before_disconnect_and_forget);
    RUN(pairing_stale_generation_failure_and_lock_cancel);
    RUN(extended_status_views_do_not_invent_evidence_or_writes);
    RUN(all_settings_pages_own_focus_in_every_mode);
    RUN(physical_modal_exit_stops_wifi_setup);
    RUN(wake_gesture_cannot_enter_the_priority_lock_path);
    RUN(release_priority_stops_wifi_setup);
    RUN(priority_reset_reconciles_a_queued_touch_release);
    RUN(overflow_reconciles_a_captured_wake_touch_release);
    RUN(fresh_touch_lock_survives_ai_wake_guard);
    RUN(fresh_touch_release_survives_ai_wake_guard);
    RUN(bluetooth_bond_views_preserve_unknown_and_verified_identity);
    RUN(bluetooth_pagination_is_bounded_and_contextual);
    RUN(disconnect_current_host_preserves_saved_pairings);
    RUN(saturated_queue_preserves_real_touch_lock);
    RUN(nine_cell_grid_reaches_every_group_and_rejects_empty_children);
    RUN(saturated_queue_rejects_consumed_wake_emergencies);
    RUN(saturated_emergency_waits_for_its_queued_begin);
    RUN(transport_switch_waits_for_both_releases_and_completion);
    RUN(transport_selection_is_page_scoped_and_failure_visible);
    RUN(transport_blocked_old_host_times_out_without_persisting);
    RUN(transport_lock_or_release_cancels_late_switch_completion);
    RUN(disconnected_old_transport_allows_explicit_offline_selection);
    RUN(inactive_bluetooth_events_do_not_retarget_selected_usb);
    RUN(transport_replaced_generation_and_refused_release_cannot_replay);
    RUN(transport_unavailable_targets_never_enable_or_persist);
    RUN(wifi_unknown_storage_recovery_binds_complete_confirmation);
    RUN(usb_removal_never_falls_back_or_replays_to_bluetooth);
    RUN(audio_mode_remains_unavailable_across_both_transport_choices);
    RUN(usb_selection_rearms_only_a_reset_cleared_selected_adapter);
    RUN(usb_reset_rearm_discards_stale_work_before_fresh_reports);
    RUN(final_release_admission_refreshes_transport_completion_evidence);
    RUN(ble_target_failure_after_final_usb_release_aborts_selection);
    RUN(pairing_disconnect_waits_for_fresh_final_release_completion);
    RUN(bluetooth_management_ignores_unrelated_usb_release);
    RUN(confirmed_disconnect_cannot_retarget_a_replacement_peer);
    RUN(origin_disconnect_allows_explicit_switch_without_release_claim);
    RUN(pairing_admission_generation_change_waits_for_actual_link_down);
    RUN(pairing_disconnect_replacement_before_acceptance_aborts);
    RUN(pairing_disconnect_replacement_after_acceptance_never_issues_another_disconnect);
    RUN(allocation_diagnostics_retain_only_first_complete_failure);
    RUN(allocation_diagnostics_ignore_an_unpublished_record);
    RUN(boot_memory_diagnostics_preserve_bounded_capability_history);
    RUN(runtime_diagnostics_keep_unknowns_private_and_observation_only);
    RUN(motion_and_scan_diagnostics_are_bounded_observations);
    RUN(reference_incident_logs_are_exact_bounded_and_observation_only);
    RUN(native_recenter_waits_for_current_grip_and_keeps_intent_stoppable);
    RUN(recenter_rechecks_busy_input_without_purging_or_false_availability);
    RUN(imu_state_observes_actual_grip_transitions_and_bounds_its_response);
    RUN(remote_initializers_honor_alignment_and_clean_allocation_failures);
    RUN(quick_status_settings_back_preserves_primary_context);
    RUN(chooser_entry_rejects_wrong_mode_and_invalid_payload_without_cancelling_send);
    RUN(chooser_entry_cannot_nest_behind_another_panel);
    RUN(choice_panels_reject_hidden_actions_without_reports_or_writes);
    RUN(keyboard_choice_closes_preserves_draft_and_acknowledges_context);
    RUN(audio_choice_closes_and_stale_revision_cannot_select_or_navigate);
    RUN(choice_panels_allow_safe_quick_status_settings_and_close);
    RUN(physical_double_click_toggles_shared_quick_panel_in_every_mode);
    RUN(physical_double_click_returns_status_settings_and_choices_to_quick);
    RUN(confirmation_owns_navigation_choices_and_physical_double_click);
    RUN(wifi_setup_safe_menu_exits_clear_credentials_and_reject_stale_restart);
    RUN(pad_single_double_and_drag_use_actual_hid_reports);
    RUN(queued_pad_events_use_native_time_and_raw_out_and_back_slop);
    RUN(queued_drag_with_lost_live_contact_is_neutralized);
    RUN(tap_preference_preserves_movement_and_explicit_drag);
    RUN(touch_drag_boundaries_and_contact_deadman_release);
    RUN(touch_release_survives_congestion_and_refused_down_never_drags);
    RUN(common_keys_are_explicit_and_preserve_preview);
    RUN(common_keys_include_host_edit_shortcuts_in_each_keyboard_submode);
    RUN(local_name_persists_only_matching_verified_acknowledgement);
    RUN(failed_or_stale_local_names_cannot_change_preferences);
    RUN(private_wifi_submission_waits_for_touch_fifo_and_cancel);
    RUN(wifi_editor_uses_reserved_identity_after_scan_expiry);
    RUN(private_editor_deadline_and_lock_clear_secret_and_ticket);
    RUN(mouse_pad_wake_is_consumed_before_fresh_takeover_and_click);
    RUN(queued_expired_pad_and_real_clock_refresh_never_replay);
    RUN(short_selection_ticket_expires_even_while_scan_is_fresh);
    RUN(common_keys_route_from_keyboard_chooser_rejects_other_origins);
    RUN(network_detail_back_returns_to_list_only_from_valid_origin);
    RUN(boot_fault_uses_actual_display_after_partial_board_start);
    RUN(motion_settings_are_independent_versioned_and_validated);
    RUN(new_settings_pages_reach_shared_preferences);
    RUN(temporary_preferences_do_not_leak_into_unrelated_native_saves);
    RUN(temporary_transport_switch_waits_without_persisting);
    RUN(wheel_slide_and_arrow_tap_reports);
    RUN(full_contact_fifo_precedes_primary_and_drains_pair);
    RUN(full_contact_right_click_and_cancelled_drag);
    RUN(contact_overflow_stale_context_and_motion_keep_safe_defaults);
    RUN(consumed_event_workspace_does_not_retain_private_payloads);
    RUN(sensor_observers_preserve_input_queue_and_redact_non_pad_context);
#undef RUN
    assert(passed);
    if (argc == 2) printf("PASS: coordinator case %s\n", argv[1]);
    else printf("PASS: %u production coordinator cases covering safety, preview/send, touch/drag, "
                "physical timing, motion, settings, display/wake, connectivity, views and diagnostics\n", passed);
    return 0;
}

/* The coordinator suite substitutes only the board-reader boundary. Native UI
 * tests exercise visible geometry; board-touch tests exercise driver reads. */
void app_ui_observe_contact_count(uint8_t count, bool valid)
{ (void)count; (void)valid; }
app_ui_touch_context_t app_ui_touch_context(void)
{ return (app_ui_touch_context_t){.epoch = s_engine.state.epoch, .ui_revision = s_ui_revision,
    .eligible_pad = s_overlay == APP_OVERLAY_NONE, .x1 = 0, .y1 = 0, .x2 = 480, .y2 = 480}; }
esp_err_t board_touch_observe(board_touch_snapshot_fn observer)
{ (void)observer; return ESP_ERR_INVALID_STATE; }
