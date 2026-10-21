// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../firmware/main/app_remote_device.c"

static struct {
    app_remote_device_state_t state;
    unsigned submits, polls, cancels, completions, approvals, claims, validations;
    uint64_t random_sequence;
    bool present, available, complete_accept, physical_required;
    cp_error_t submit_error, cancel_error;
    app_remote_device_outcome_t outcome;
    app_remote_device_request_t request;
    app_remote_device_approval_t approval;
    cp_result_t last_completion;
} model;
static app_remote_device_t *device;
static void *memory;
static uint64_t now = 1000, command_key;

static bool observe(void *user, cp_operation_id_t operation, app_remote_device_state_t *out)
{
    (void)user; *out = model.state;
    if (configuration(operation) || operation == CP_OP_CONFIG_GET) out->resource_revision = out->settings_revision;
    return model.present;
}
static bool available(void *user, cp_operation_id_t operation, uint32_t fields, char text[CP_REASON_CAPACITY])
{
    (void)user; (void)operation; (void)fields; if (text) text[0] = 0; return model.available;
}
static cp_error_t validate(void *user, const device_settings_t *settings, uint32_t fields, bool durable)
{
    (void)user; (void)durable; assert(fields && device_settings_validate(settings)); ++model.validations; return CP_OK;
}
static bool random_bytes(void *user, uint8_t *data, size_t bytes)
{
    (void)user; assert(bytes == 16); memset(data, 0, bytes); uint64_t value = ++model.random_sequence;
    memcpy(data, &value, sizeof(value)); return true;
}
static cp_error_t submit(void *user, const app_remote_device_request_t *request, app_remote_device_outcome_t *out)
{
    (void)user; ++model.submits; model.request = *request; *out = model.outcome; return model.submit_error;
}
static bool poll(void *user, uint64_t token, app_remote_device_outcome_t *out)
{
    (void)user; assert(token == model.request.token); ++model.polls; *out = model.outcome; return true;
}
static cp_error_t cancel(void *user, uint64_t token)
{
    (void)user; assert(token == model.request.token); ++model.cancels; return model.cancel_error;
}
static bool complete(void *user, const cp_result_t *result)
{
    (void)user; ++model.completions; model.last_completion = *result; return model.complete_accept;
}
static bool approval(void *user, const char *id, app_remote_device_approval_t *out)
{
    (void)user; if (strcmp(id, model.approval.id)) return false; *out = model.approval; return true;
}
static cp_error_t prepare_approval(void *user, const app_remote_device_request_t *request, app_remote_device_approval_t *out)
{
    (void)user; ++model.approvals;
    model.approval = (app_remote_device_approval_t){.operation = request->operation, .owner = request->identity,
        .required_scopes = operation_info(request->operation)->scopes, .management_generation = request->management_generation,
        .resource_revision = request->resource_revision, .expires_ms = now + APP_REMOTE_DEVICE_STAGE_MS,
        .physical_required = model.physical_required};
    (void)snprintf(model.approval.id, sizeof(model.approval.id), "%032u", model.approvals);
    memcpy(model.approval.action_digest, request->action_digest, 32); *out = model.approval; return CP_OK;
}
static cp_error_t decide(void *user, const app_remote_device_approval_t *review, bool confirm)
{
    (void)user; assert(!strcmp(review->id, model.approval.id) && equal(review->action_digest, model.approval.action_digest, 32));
    if (model.approval.consumed) return CP_TOO_LATE;
    if (confirm) model.approval.confirmed = true; else model.approval.cancelled = true;
    return CP_OK;
}
static cp_error_t claim(void *user, const app_remote_device_approval_t *review)
{
    (void)user; assert(!strcmp(review->id, model.approval.id));
    if (!model.approval.confirmed || model.approval.consumed || model.approval.cancelled) return CP_STALE;
    model.approval.consumed = true; ++model.claims; return CP_OK;
}
static void setup(void)
{
    free(memory); memory = calloc(1, app_remote_device_bytes()); assert(memory);
    memset(&model, 0, sizeof(model)); now = 1000; command_key = 0;
    model.present = model.available = model.complete_accept = true;
    model.state = (app_remote_device_state_t){.management_generation = 3, .resource_revision = 7,
        .settings_revision = 5, .ui_revision = 9, .bluetooth_generation = 11, .bond_revision = 13, .screen_awake = true};
    device_settings_defaults(&model.state.settings);
    app_remote_device_port_t port = {.observe = observe, .available = available, .validate_settings = validate,
        .random = random_bytes, .submit = submit, .poll = poll, .cancel = cancel, .complete = complete,
        .approval = approval, .approval_prepare = prepare_approval, .approval_decide = decide, .approval_claim = claim};
    assert(app_remote_device_init(memory, app_remote_device_bytes(), &port, &device) == CP_OK);
}
static cp_command_t command(cp_operation_id_t operation, const char *args)
{
    cp_command_t value = {.version = 1, .operation = operation, .command_key = ++command_key,
        .identity = {.principal_id = {1}, .session_id = {2}, .scopes = CP_SCOPE_ALL},
        .management_generation = model.state.management_generation, .resource_revision =
            configuration(operation) ? model.state.settings_revision : model.state.resource_revision,
        .ui_revision = model.state.ui_revision, .deadline_ms = now + 2000};
    assert(strlen(args) <= CP_MAX_ARGS_BYTES); memcpy(value.args_json, args, strlen(args) + 1); return value;
}
static cp_result_t dispatch(cp_command_t *value, cp_error_t expected)
{
    cp_result_t result; assert(app_remote_device_execute(device, value, now, &result) == expected);
    assert(result.command_key == value->command_key && result.uncertain_index == -1);
    return result;
}
static cp_result_t stage(const char *patch)
{
    char args[CP_MAX_ARGS_BYTES + 1]; int n = snprintf(args, sizeof(args), "{\"patch\":%s}", patch);
    assert(n > 0 && (size_t)n < sizeof(args)); cp_command_t value = command(CP_OP_CONFIG_STAGE, args);
    cp_result_t result = dispatch(&value, CP_OK);
    assert(result.state == CP_RESULT_APPLIED && result.object_id[0] && !strcmp(result.reason, "staged_inert"));
    return result;
}
static cp_command_t transaction_command(cp_operation_id_t operation, const char *id)
{
    char args[96]; (void)snprintf(args, sizeof(args), "{\"transaction_id\":\"%s\"}", id); return command(operation, args);
}
static cp_command_t confirmation(cp_operation_id_t operation)
{
    char digest[65], args[256]; hex_encode(model.approval.action_digest, 32, digest);
    (void)snprintf(args, sizeof(args), "{\"operation_id\":\"%s\",\"digest\":\"%s\",\"revision\":\"%" PRIu64 "\"}",
        model.approval.id, digest, model.approval.resource_revision);
    return command(operation, args);
}

static void configuration_transactions(void)
{
    setup();
    cp_result_t staged = stage("{\"display.theme\":\"light\",\"display.brightness_percent\":70,\"haptics.enabled\":false}");
    assert(!model.submits && model.state.settings.theme == DEVICE_THEME_DARK && model.state.settings.brightness_percent == 50);
    cp_command_t apply = transaction_command(CP_OP_CONFIG_APPLY, staged.object_id);
    apply.identity.session_id[0] = 9; (void)dispatch(&apply, CP_STALE); assert(!model.submits);
    apply.identity.session_id[0] = 2; apply.identity.scopes = CP_SCOPE_VIEW; (void)dispatch(&apply, CP_FORBIDDEN);
    apply.identity.scopes = CP_SCOPE_ALL;
    cp_result_t result = dispatch(&apply, CP_OK); assert(result.state == CP_RESULT_RUNNING && model.submits == 1);
    assert(model.request.settings.theme == DEVICE_THEME_LIGHT && model.request.settings.brightness_percent == 70 && !model.request.settings.haptics_enabled);
    assert(model.request.settings.preferred_transport == model.state.settings.preferred_transport);
    model.outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_EFFECTIVE, .boundary_confirmed = true, .resource_revision = 6};
    model.complete_accept = false; now += 1; app_remote_device_service(device, now);
    assert(model.last_completion.state == CP_RESULT_RUNNING && !strcmp(model.last_completion.reason, "applied_saving"));
    unsigned completions = model.completions; app_remote_device_service(device, ++now); assert(model.completions == completions + 1);
    model.outcome.durable_confirmed = true; app_remote_device_service(device, ++now);
    assert(model.last_completion.state == CP_RESULT_APPLIED && !strcmp(model.last_completion.reason, "saved") && model.submits == 1);
    model.complete_accept = true; app_remote_device_service(device, ++now);
    (void)dispatch(&apply, CP_OK); assert(model.submits == 1);
    apply.args_json[1] = 'x'; (void)dispatch(&apply, CP_INVALID); assert(model.submits == 1);
    setup(); staged = stage("{\"display.theme\":\"light\"}");
    apply = transaction_command(CP_OP_CONFIG_APPLY, staged.object_id); ++model.state.settings_revision;
    apply.resource_revision = model.state.settings_revision; (void)dispatch(&apply, CP_STALE);
    cp_command_t cancel_stage = transaction_command(CP_OP_CONFIG_CANCEL, staged.object_id);
    cancel_stage.resource_revision = 5; (void)dispatch(&cancel_stage, CP_OK); assert(!model.submits);
    setup(); staged = stage("{\"display.theme\":\"light\"}"); now += APP_REMOTE_DEVICE_STAGE_MS;
    apply = transaction_command(CP_OP_CONFIG_APPLY, staged.object_id); (void)dispatch(&apply, CP_STALE); assert(!model.submits);
    setup();
    for (unsigned i = 0; i < APP_REMOTE_DEVICE_TRANSACTIONS; ++i) (void)stage("{\"haptics.enabled\":false}");
    cp_command_t full = command(CP_OP_CONFIG_STAGE, "{\"patch\":{\"haptics.enabled\":false}}"); (void)dispatch(&full, CP_BUSY);
    assert(!model.submits);
}
static void exact_values_and_secret_rejection(void)
{
    setup();
    const char *bad[] = {"{\"patch\":{\"pointer.gain_hid_per_degree\":0}}",
        "{\"patch\":{\"pointer.gain_hid_per_degree\":0.333}}", "{\"patch\":{\"display.brightness_percent\":55}}",
        "{\"patch\":{}}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) { cp_command_t value = command(CP_OP_CONFIG_STAGE, bad[i]); (void)dispatch(&value, CP_INVALID); }
    cp_command_t value = command(CP_OP_CONFIG_STAGE, "{\"patch\":{\"wifi.profiles\":[{\"profile_id\":\"11111111111111111111111111111111\",\"ssid\":\"Example\",\"password\":\"private-fixture-password\",\"enabled\":true,\"security\":\"wpa2_minimum\"}]}}");
    cp_result_t result = dispatch(&value, CP_NOT_IMPLEMENTED);
    assert(!strstr(result.reason, "private-fixture-password") && !model.submits);
    const unsigned char *scratch = (const unsigned char *)&device->document;
    for (size_t i = 0; i < sizeof(device->document); ++i) assert(!scratch[i]);
    cp_result_t staged = stage("{\"pointer.gain_hid_per_degree\":4.25,\"pointer.deadzone_dps\":0.125,\"pointer.filter_seconds\":0.045}");
    cp_command_t apply = transaction_command(CP_OP_CONFIG_APPLY, staged.object_id); (void)dispatch(&apply, CP_OK);
    assert(model.request.settings.motion_gain_x_centi == 425 && model.request.settings.motion_gain_y_centi == 425);
    assert(model.request.settings.motion_deadzone_mdps == 125 && model.request.settings.motion_filter_ms == 45);
    setup(); model.state.settings.brightness_percent = 90; model.state.settings.haptics_enabled = false;
    value = command(CP_OP_CONFIG_RESET, "{\"keys\":[\"display.brightness_percent\"]}"); (void)dispatch(&value, CP_OK);
    assert(model.request.settings.brightness_percent == 50 && !model.request.settings.haptics_enabled && model.request.settings_fields == APP_REMOTE_SETTING_BRIGHTNESS);
}
static void immutable_approvals(void)
{
    setup(); cp_command_t forget = command(CP_OP_BLUETOOTH_BONDS_FORGET_ALL, "{}");
    cp_result_t pending = dispatch(&forget, CP_OK);
    assert(pending.state == CP_RESULT_ACCEPTED && pending.object_id[0] && !model.submits && model.approvals == 1);
    cp_command_t confirm = confirmation(CP_OP_OPERATION_CONFIRM);
    confirm.identity.session_id[0] = 8; (void)dispatch(&confirm, CP_FORBIDDEN);
    confirm.identity.session_id[0] = 2; confirm.identity.scopes = CP_SCOPE_VIEW; (void)dispatch(&confirm, CP_FORBIDDEN);
    confirm.identity.scopes = CP_SCOPE_ALL; confirm.args_json[65] ^= 1; (void)dispatch(&confirm, CP_FORBIDDEN);
    confirm = confirmation(CP_OP_OPERATION_CONFIRM); cp_result_t result = dispatch(&confirm, CP_OK);
    assert(result.state == CP_RESULT_APPLIED && !model.submits); /* Approval is not the destructive result. */
    app_remote_device_service(device, ++now); assert(model.submits == 1 && model.claims == 1);
    model.outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_COMPLETED, .boundary_confirmed = true, .resource_revision = 8};
    app_remote_device_service(device, ++now);
    assert(model.last_completion.command_key == forget.command_key && model.last_completion.state == CP_RESULT_COMPLETED);
    confirm = confirmation(CP_OP_OPERATION_CONFIRM); (void)dispatch(&confirm, CP_STALE); assert(model.submits == 1);
    setup(); model.physical_required = true;
    forget = command(CP_OP_BLUETOOTH_PAIRING_OPEN, "{\"window_seconds\":45}"); (void)dispatch(&forget, CP_OK);
    confirm = confirmation(CP_OP_OPERATION_CONFIRM); (void)dispatch(&confirm, CP_FORBIDDEN);
    app_remote_device_service(device, ++now); assert(!model.submits);
    model.approval.physical_satisfied = true; confirm = confirmation(CP_OP_OPERATION_CONFIRM); (void)dispatch(&confirm, CP_OK);
    app_remote_device_service(device, ++now); assert(model.submits == 1 && model.request.window_seconds == 45);
    setup(); forget = command(CP_OP_BLUETOOTH_BONDS_FORGET_ALL, "{}"); (void)dispatch(&forget, CP_OK);
    cp_command_t cancel_approval = confirmation(CP_OP_OPERATION_CANCEL); (void)dispatch(&cancel_approval, CP_OK);
    app_remote_device_service(device, ++now); assert(!model.submits && model.last_completion.state == CP_RESULT_CANCELLED);
    setup(); forget = command(CP_OP_BLUETOOTH_BONDS_FORGET_ALL, "{}"); (void)dispatch(&forget, CP_OK);
    app_remote_device_revoke(device, CP_SAFETY_REVOKED, &forget.identity, ++now);
    assert(model.approval.cancelled && !model.submits);
    confirm = confirmation(CP_OP_OPERATION_CONFIRM); (void)dispatch(&confirm, CP_STALE);
}
static void jobs_and_ui_gates(void)
{
    setup(); cp_command_t wake = command(CP_OP_DISPLAY_WAKE, "{}");
    (void)dispatch(&wake, CP_OK); assert(model.submits == 1);
    cp_command_t other = command(CP_OP_DISPLAY_SLEEP, "{}"); (void)dispatch(&other, CP_BUSY);
    model.outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_COMPLETED, .resource_revision = 8};
    app_remote_device_service(device, ++now); assert(model.last_completion.state == CP_RESULT_RUNNING && !strcmp(model.last_completion.reason, "awaiting_boundary"));
    model.outcome.boundary_confirmed = true; app_remote_device_service(device, ++now);
    assert(model.last_completion.state == CP_RESULT_COMPLETED);
    (void)dispatch(&wake, CP_OK); assert(model.submits == 1);
    wake.deadline_ms += 1; (void)dispatch(&wake, CP_REQUEST_CONFLICT);
    setup(); wake = command(CP_OP_DISPLAY_WAKE, "{}"); (void)dispatch(&wake, CP_OK);
    now += APP_REMOTE_DEVICE_JOB_MS; app_remote_device_service(device, now); assert(model.cancels == 1);
    app_remote_device_service(device, ++now); assert(model.cancels == 1);
    model.outcome = (app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true};
    app_remote_device_service(device, ++now); assert(model.last_completion.state == CP_RESULT_CANCELLED);
    setup(); cp_command_t ui = command(CP_OP_UI_NAVIGATE, "{\"overlay\":\"settings\",\"page\":\"input\"}");
    (void)dispatch(&ui, CP_FORBIDDEN); assert(!model.submits);
    cp_result_t result; assert(app_remote_device_execute_ui_authorized(device, &ui, now, &result) == CP_OK && model.submits == 1);
    assert(!strcmp(model.request.overlay, "settings") && !strcmp(model.request.page, "input"));
    setup(); ui = command(CP_OP_UI_PRIMARY_ACTIVATE, "{}"); model.state.screen_awake = false;
    assert(app_remote_device_execute_ui_authorized(device, &ui, now, &result) == CP_UNAVAILABLE && !model.submits);
    setup(); wake = command(CP_OP_DISPLAY_WAKE, "{}"); model.state.locked = true; (void)dispatch(&wake, CP_LOCKED);
    model.state.locked = false; model.state.local_busy = true; (void)dispatch(&wake, CP_LOCAL_BUSY);
    model.state.local_busy = false; --wake.management_generation; (void)dispatch(&wake, CP_STALE);
}
static void read_snapshots_and_clock(void)
{
    setup(); char json[2048]; size_t length; cp_identity_t identity = {.scopes = CP_SCOPE_VIEW};
    (void)snprintf(model.state.settings.bluetooth_name, sizeof(model.state.settings.bluetooth_name), "%s", "A\"B");
    assert(app_remote_device_read(device, &identity, "/api/v1/config", json, sizeof(json), &length) == CP_OK);
    cj_document_t document; assert(cj_parse(&document, json, length));
    assert(strstr(json, "A\\\"B") && strstr(json, "\"revision\":\"5\"") && !model.submits);
    model.state.settings.motion_gain_y_centi = 800;
    assert(app_remote_device_read(device, &identity, "/api/v1/config", json, sizeof(json), &length) == CP_OK);
    assert(strstr(json, "\"pointer.gain_hid_per_degree\":null") && strstr(json, "\"pointer_gain_axes\":{\"x\":4.00,\"y\":8.00}"));
    assert(app_remote_device_read(device, &identity, "/api/v1/config", json, 4, &length) == CP_TOO_LARGE && !length && !json[0]);
    identity.scopes = 0; assert(app_remote_device_read(device, &identity, "/api/v1/config", json, sizeof(json), &length) == CP_FORBIDDEN);
    cp_command_t calibrate = command(CP_OP_IMU_CALIBRATE, "{\"timeout_ms\":1500}"); (void)dispatch(&calibrate, CP_OK);
    assert(model.request.deadline_ms == now + 1500);
    --now; app_remote_device_service(device, now); assert(model.cancels == 1 && device->clock_failed);
    cp_command_t wake = command(CP_OP_DISPLAY_WAKE, "{}"); (void)dispatch(&wake, CP_EXPIRED);
}
static void independent_native_motion_fields_and_alias_conflicts(void)
{
    setup();
    cp_result_t staged = stage("{\"pointer.gain_x_hid_per_degree\":2.25,\"pointer.gain_y_hid_per_degree\":8.75,"
        "\"pointer.invert_x\":true,\"pointer.invert_y\":true,\"pointer.freeze_ms\":620}");
    cp_command_t apply = transaction_command(CP_OP_CONFIG_APPLY, staged.object_id); (void)dispatch(&apply, CP_OK);
    assert(model.request.settings.motion_gain_x_centi == 225 && model.request.settings.motion_gain_y_centi == 875 &&
        model.request.settings.motion_invert_x && model.request.settings.motion_invert_y && model.request.settings.motion_freeze_ms == 620);
    assert((model.request.settings_fields & (APP_REMOTE_SETTING_GAIN_X | APP_REMOTE_SETTING_GAIN_Y | APP_REMOTE_SETTING_INVERT_X |
        APP_REMOTE_SETTING_INVERT_Y | APP_REMOTE_SETTING_FREEZE)) == (APP_REMOTE_SETTING_GAIN_X | APP_REMOTE_SETTING_GAIN_Y |
        APP_REMOTE_SETTING_INVERT_X | APP_REMOTE_SETTING_INVERT_Y | APP_REMOTE_SETTING_FREEZE));
    setup(); model.state.settings.motion_gain_y_centi = 875; model.state.settings.motion_invert_x = true;
    model.state.settings.motion_invert_y = true; model.state.settings.motion_freeze_ms = 620;
    cp_command_t reset = command(CP_OP_CONFIG_RESET,"{\"keys\":[\"pointer.gain_x_hid_per_degree\",\"pointer.invert_y\",\"pointer.freeze_ms\"]}");
    (void)dispatch(&reset,CP_OK);
    assert(model.request.settings.motion_gain_x_centi == 400 && model.request.settings.motion_gain_y_centi == 875 &&
        model.request.settings.motion_invert_x && !model.request.settings.motion_invert_y && model.request.settings.motion_freeze_ms == 150);
    setup();
    const char *conflicts[] = {
        "{\"patch\":{\"pointer.gain_hid_per_degree\":4,\"pointer.gain_x_hid_per_degree\":5}}",
        "{\"patch\":{\"pointer.gain_y_hid_per_degree\":5,\"pointer.gain_hid_per_degree\":4}}",
        "{\"patch\":{\"pointer.gain_x_hid_per_degree\":0.251}}",
        "{\"patch\":{\"pointer.freeze_ms\":1001}}"};
    for (size_t i=0;i<sizeof(conflicts)/sizeof(conflicts[0]);++i) {
        cp_command_t bad = command(CP_OP_CONFIG_STAGE,conflicts[i]); (void)dispatch(&bad,CP_INVALID);
    }
    assert(!model.submits);
    model.state.settings.motion_gain_y_centi = 875; model.state.settings.motion_invert_x = true; model.state.settings.motion_freeze_ms = 620;
    cp_identity_t who = {.scopes=CP_SCOPE_VIEW}; char json[2048]; size_t length;
    assert(app_remote_device_read(device,&who,"/api/v1/config",json,sizeof(json),&length)==CP_OK);
    assert(strstr(json,"\"pointer.gain_x_hid_per_degree\":4.00") && strstr(json,"\"pointer.gain_y_hid_per_degree\":8.75") &&
        strstr(json,"\"pointer.invert_x\":true") && strstr(json,"\"pointer.invert_y\":false") && strstr(json,"\"pointer.freeze_ms\":620"));
}
int main(void)
{
    configuration_transactions(); exact_values_and_secret_rejection(); immutable_approvals(); jobs_and_ui_gates(); read_snapshots_and_clock();
    independent_native_motion_fields_and_alias_conflicts();
    free(memory);
    puts("Remote device: canonical inert transactions, shared settings requests, identity/scope/revision gates, immutable approvals, actual-boundary results, cancellation and secret-free reads passed.");
    return 0;
}
