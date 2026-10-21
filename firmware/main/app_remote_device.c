// SPDX-License-Identifier: Apache-2.0
#include "app_remote_device.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../components/control_protocol/control_registry_internal.h"

typedef struct {
    bool used;
    char id[CP_ID_HEX_CAPACITY];
    cp_identity_t owner;
    cp_scope_mask_t scopes;
    uint64_t management_generation, revision, expires_ms;
    device_settings_t candidate;
    uint32_t fields;
    bool durable;
} transaction_t;
typedef struct {
    bool used, finished, publication_pending, submitted, waiting_approval, cancelling;
    uint8_t fingerprint[32];
    app_remote_device_request_t request;
    cp_result_t result;
} device_job_t;
struct app_remote_device {
    app_remote_device_port_t port;
    uint64_t next_token, last_ms;
    bool clock_failed;
    transaction_t transactions[APP_REMOTE_DEVICE_TRANSACTIONS];
    device_job_t jobs[APP_REMOTE_DEVICE_JOBS];
    cj_document_t document;
};

static void wipe(void *memory, size_t bytes)
{
    volatile unsigned char *p = memory;
    while (bytes--) *p++ = 0;
}
static size_t bounded_length(const char *s, size_t capacity)
{
    size_t n = 0;
    if (s) while (n < capacity && s[n]) ++n;
    return n;
}
static bool equal(const void *a, const void *b, size_t bytes)
{
    const uint8_t *x = a, *y = b; unsigned difference = 0;
    for (size_t i = 0; i < bytes; ++i) difference |= (unsigned)(x[i] ^ y[i]);
    return difference == 0;
}
static bool same_owner(const cp_identity_t *a, const cp_identity_t *b)
{
    return equal(a->principal_id, b->principal_id, CP_ID_BYTES) && equal(a->session_id, b->session_id, CP_ID_BYTES);
}
static void reason(cp_result_t *result, const char *text)
{
    (void)snprintf(result->reason, sizeof(result->reason), "%s", text);
}
static cp_error_t rejected(cp_result_t *result, cp_error_t error)
{
    result->state = CP_RESULT_REJECTED; result->error = error;
    reason(result, cp_error_name(error));
    return error;
}
static const cp_operation_info_t *operation_info(cp_operation_id_t operation)
{
    for (size_t i = 0; i < cp_operation_count(); ++i) {
        const cp_operation_info_t *info = cp_operation_at(i);
        if (info && info->id == operation) return info;
    }
    return NULL;
}
static bool ui_operation(cp_operation_id_t operation)
{
    return operation >= CP_OP_UI_NAVIGATE && operation <= CP_OP_UI_MODE_PANEL_OPEN;
}
static bool configuration(cp_operation_id_t operation)
{
    return operation >= CP_OP_CONFIG_STAGE && operation <= CP_OP_CONFIG_RESET;
}
static bool owned_operation(cp_operation_id_t operation)
{
    return configuration(operation) || ui_operation(operation) || operation == CP_OP_IMU_CALIBRATE ||
        (operation >= CP_OP_BLUETOOTH_PAIRING_OPEN && operation <= CP_OP_OPERATION_CANCEL) ||
        (operation >= CP_OP_HAPTICS_TEST && operation <= CP_OP_DISPLAY_SLEEP) ||
        (operation >= CP_OP_SYSTEM_RESTART && operation <= CP_OP_DEVICE_FACTORY_RESET_REQUEST);
}
bool app_remote_device_owns(cp_operation_id_t operation) { return owned_operation(operation); }
static bool destructive(cp_operation_id_t operation)
{
    return operation == CP_OP_BLUETOOTH_PAIRING_OPEN || operation == CP_OP_BLUETOOTH_BONDS_FORGET_ALL ||
        operation == CP_OP_BLUETOOTH_BOND_FORGET || operation == CP_OP_SYSTEM_RESTART ||
        operation == CP_OP_PREFERENCES_RESET || operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST;
}
static cp_error_t reset_candidate(app_remote_device_request_t *request, const device_settings_t *current)
{
    device_settings_t defaults; device_settings_defaults(&defaults);
    request->settings = *current; request->durable = true;
    bool all = request->operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST || !strcmp(request->group, "all");
    bool display = all || !strcmp(request->group, "display");
    bool input = all || !strcmp(request->group, "input");
    bool haptics = all || !strcmp(request->group, "haptics");
    bool bluetooth = all || !strcmp(request->group, "bluetooth");
    if (!display && !input && !haptics && !bluetooth) return CP_INVALID;
    if (display) {
        request->settings.theme = defaults.theme; request->settings.brightness_percent = defaults.brightness_percent;
        request->settings.idle_timeout = defaults.idle_timeout;
        request->settings_fields |= APP_REMOTE_SETTING_THEME | APP_REMOTE_SETTING_BRIGHTNESS | APP_REMOTE_SETTING_IDLE;
    }
    if (input) {
        request->settings.host_modifier = defaults.host_modifier; request->settings.tap_to_click = defaults.tap_to_click;
        request->settings.preferred_transport = defaults.preferred_transport;
        request->settings.motion_gain_x_centi = defaults.motion_gain_x_centi; request->settings.motion_gain_y_centi = defaults.motion_gain_y_centi;
        request->settings.motion_deadzone_mdps = defaults.motion_deadzone_mdps; request->settings.motion_filter_ms = defaults.motion_filter_ms;
        request->settings.motion_freeze_ms = defaults.motion_freeze_ms;
        request->settings.motion_invert_x = defaults.motion_invert_x; request->settings.motion_invert_y = defaults.motion_invert_y;
        request->settings_fields |= APP_REMOTE_SETTING_SHORTCUT | APP_REMOTE_SETTING_TAP | APP_REMOTE_SETTING_TRANSPORT |
            APP_REMOTE_SETTING_MOTION;
    }
    if (haptics) { request->settings.haptics_enabled = defaults.haptics_enabled; request->settings_fields |= APP_REMOTE_SETTING_HAPTICS; }
    if (bluetooth) {
        memcpy(request->settings.bluetooth_name, defaults.bluetooth_name, sizeof(defaults.bluetooth_name));
        request->settings_fields |= APP_REMOTE_SETTING_BLUETOOTH_NAME;
    }
    return CP_OK;
}
static unsigned domains(const app_remote_device_request_t *request)
{
    unsigned value = request->settings_fields ? 1U : 0U;
    if (request->settings_fields & APP_REMOTE_SETTING_BRIGHTNESS) value |= 2U;
    if (request->settings_fields & (APP_REMOTE_SETTING_BLUETOOTH_NAME | APP_REMOTE_SETTING_TRANSPORT)) value |= 4U;
    if (request->settings_fields & APP_REMOTE_SETTING_HAPTICS) value |= 8U;
    if (request->settings_fields & (APP_REMOTE_SETTING_MOTION | APP_REMOTE_SETTING_TAP | APP_REMOTE_SETTING_SHORTCUT)) value |= 16U;
    if (request->operation >= CP_OP_BLUETOOTH_PAIRING_OPEN && request->operation <= CP_OP_BLUETOOTH_TARGET_REQUEST) value |= 4U;
    if (request->operation == CP_OP_DISPLAY_WAKE || request->operation == CP_OP_DISPLAY_SLEEP) value |= 2U;
    if (request->operation == CP_OP_HAPTICS_TEST || request->operation == CP_OP_HAPTICS_RECOVER) value |= 8U;
    if (request->operation == CP_OP_IMU_CALIBRATE || ui_operation(request->operation)) value |= 16U;
    if (request->operation >= CP_OP_SYSTEM_RESTART && request->operation <= CP_OP_DEVICE_FACTORY_RESET_REQUEST) value = UINT32_MAX;
    return value;
}
static bool clock_valid(app_remote_device_t *d, uint64_t now)
{
    if (d->clock_failed || now < d->last_ms) { d->clock_failed = true; return false; }
    d->last_ms = now;
    return true;
}
static bool string(cj_document_t *doc, uint16_t object, const char *key, char *out, size_t bytes)
{
    const char *value = cj_string(doc, cj_get(doc, object, key));
    if (!value || strlen(value) >= bytes) return false;
    memcpy(out, value, strlen(value) + 1);
    return true;
}
static void optional_string(cj_document_t *doc, const char *key, char *out, size_t bytes)
{
    (void)string(doc, 0, key, out, bytes);
}
static bool hex_decode(const char *text, uint8_t *bytes, size_t count)
{
    if (!text || strlen(text) != count * 2U) return false;
    for (size_t i = 0; i < count; ++i) {
        unsigned value = 0;
        for (size_t n = 0; n < 2; ++n) {
            char c = text[i * 2U + n];
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16U;
            if (digit == 16) return false;
            value = value * 16U + digit;
        }
        bytes[i] = (uint8_t)value;
    }
    return true;
}
static void hex_encode(const uint8_t *bytes, size_t count, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) { out[2U*i] = hex[bytes[i] >> 4]; out[2U*i+1] = hex[bytes[i] & 15U]; }
    out[2U*count] = 0;
}
static cp_error_t parse(app_remote_device_t *d, const cp_command_t *command)
{
    size_t bytes = bounded_length(command->args_json, sizeof(command->args_json));
    const cs_node_t *schema = cp_argument_schema(command->operation);
    if (!schema || bytes > CP_MAX_ARGS_BYTES || !cj_parse(&d->document, command->args_json, bytes) ||
        d->document.nodes[0].type != CJ_OBJECT || !cs_validate(&d->document, 0, schema)) return CP_INVALID;
    return CP_OK;
}

static void append64(uint8_t *buffer, size_t *length, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) buffer[(*length)++] = (uint8_t)(value >> (56U - i * 8U));
}
static void append_bytes(uint8_t *buffer, size_t *length, const void *data, size_t bytes)
{
    memcpy(buffer + *length, data, bytes); *length += bytes;
}
static void action_digest(app_remote_device_request_t *request)
{
    uint8_t buffer[640] = {0}, record[DEVICE_SETTINGS_RECORD_SIZE] = {0}; size_t length = 0;
    append64(buffer, &length, request->operation);
    append_bytes(buffer, &length, request->identity.principal_id, CP_ID_BYTES);
    append_bytes(buffer, &length, request->identity.session_id, CP_ID_BYTES);
    append64(buffer, &length, request->management_generation); append64(buffer, &length, request->resource_revision);
    append64(buffer, &length, request->settings_revision); append64(buffer, &length, request->bluetooth_generation);
    append64(buffer, &length, request->bond_revision); append64(buffer, &length, request->settings_fields);
    append64(buffer, &length, request->durable); append64(buffer, &length, request->timeout_ms);
    append64(buffer, &length, request->window_seconds); append64(buffer, &length, request->duration_ms);
    append64(buffer, &length, request->strength_percent);
    if (request->settings_fields) (void)device_settings_encode(&request->settings, record, sizeof(record));
    append_bytes(buffer, &length, record, sizeof(record));
    append_bytes(buffer, &length, request->peer_id, sizeof(request->peer_id));
    append_bytes(buffer, &length, request->pattern, sizeof(request->pattern));
    append_bytes(buffer, &length, request->overlay, sizeof(request->overlay));
    append_bytes(buffer, &length, request->page, sizeof(request->page));
    append_bytes(buffer, &length, request->group, sizeof(request->group));
    append_bytes(buffer, &length, request->retained_manifest_digest, sizeof(request->retained_manifest_digest));
    cp_sha256(buffer, length, request->action_digest); wipe(buffer, sizeof(buffer)); wipe(record, sizeof(record));
}
static void fingerprint(const cp_command_t *command, uint8_t digest[32])
{
    uint8_t buffer[CP_MAX_ARGS_BYTES + 320U]; size_t length = 0;
    append64(buffer, &length, command->version); append64(buffer, &length, command->operation);
    append64(buffer, &length, command->command_key); append64(buffer, &length, command->deadline_ms);
    append64(buffer, &length, command->management_generation); append64(buffer, &length, command->resource_revision);
    append64(buffer, &length, command->input_epoch); append64(buffer, &length, command->transport_generation);
    append64(buffer, &length, command->ui_revision); append64(buffer, &length, command->identity.scopes);
    append_bytes(buffer, &length, command->identity.principal_id, CP_ID_BYTES);
    append_bytes(buffer, &length, command->identity.session_id, CP_ID_BYTES);
    append_bytes(buffer, &length, command->lease_id, CP_ID_BYTES);
    append_bytes(buffer, &length, command->target_id, sizeof(command->target_id));
    append_bytes(buffer, &length, command->args_json, strlen(command->args_json));
    cp_sha256(buffer, length, digest); wipe(buffer, sizeof(buffer));
}

static bool exact_scaled(cj_document_t *doc, uint16_t node, double scale, unsigned minimum, unsigned maximum, uint16_t *out)
{
    if (node == CJ_NONE) return false;
    double value = doc->nodes[node].type == CJ_INTEGER ? (double)doc->nodes[node].value.integer : doc->nodes[node].value.number;
    value *= scale;
    double rounded = floor(value + 0.5);
    if (!isfinite(value) || fabs(value - rounded) > 0.0000001 || rounded < minimum || rounded > maximum) return false;
    *out = (uint16_t)rounded; return true;
}
static cp_error_t setting(app_remote_device_t *d, const char *key, uint16_t node, bool reset,
    device_settings_t *candidate, uint32_t *fields, cp_scope_mask_t *scopes)
{
    const cp_config_info_t *metadata = cp_config_find(key);
    if (!metadata) return CP_INVALID;
    if (!reset && !cs_validate(&d->document, node, metadata->schema)) return CP_INVALID;
    device_settings_t defaults; device_settings_defaults(&defaults);
    cj_node_t *value = node == CJ_NONE ? NULL : &d->document.nodes[node];
    const char *text = reset ? NULL : cj_string(&d->document, node);
    uint32_t field = 0;
    if (!strcmp(key, "display.theme")) {
        field = APP_REMOTE_SETTING_THEME; candidate->theme = reset ? defaults.theme :
            !strcmp(text, "dark") ? DEVICE_THEME_DARK : DEVICE_THEME_LIGHT;
    } else if (!strcmp(key, "display.brightness_percent")) {
        field = APP_REMOTE_SETTING_BRIGHTNESS; candidate->brightness_percent = reset ? defaults.brightness_percent : (uint8_t)value->value.integer;
    } else if (!strcmp(key, "display.idle_timeout_seconds")) {
        field = APP_REMOTE_SETTING_IDLE; candidate->idle_timeout = reset ? defaults.idle_timeout : (device_idle_timeout_t)(value->value.integer / 60);
    } else if (!strcmp(key, "haptics.enabled")) {
        field = APP_REMOTE_SETTING_HAPTICS; candidate->haptics_enabled = reset ? defaults.haptics_enabled : value->value.boolean;
    } else if (!strcmp(key, "keyboard.shortcut_modifier")) {
        field = APP_REMOTE_SETTING_SHORTCUT; candidate->host_modifier = reset ? defaults.host_modifier :
            !strcmp(text, "command") ? DEVICE_HOST_COMMAND : DEVICE_HOST_CTRL;
    } else if (!strcmp(key, "input.preferred_transport")) {
        field = APP_REMOTE_SETTING_TRANSPORT; candidate->preferred_transport = reset ? defaults.preferred_transport :
            !strcmp(text, "usb") ? DEVICE_TRANSPORT_USB : DEVICE_TRANSPORT_BLUETOOTH;
    } else if (!strcmp(key, "touch.tap_to_click")) {
        field = APP_REMOTE_SETTING_TAP; candidate->tap_to_click = reset ? defaults.tap_to_click : value->value.boolean;
    } else if (!strcmp(key, "bluetooth.device_name")) {
        field = APP_REMOTE_SETTING_BLUETOOTH_NAME;
        (void)snprintf(candidate->bluetooth_name, sizeof(candidate->bluetooth_name), "%s", reset ? defaults.bluetooth_name : text);
    } else if (!strcmp(key, "pointer.gain_hid_per_degree")) {
        field = APP_REMOTE_SETTING_POINTER_GAIN;
        uint16_t gain = defaults.motion_gain_x_centi;
        if (!reset && !exact_scaled(&d->document, node, 100, 25, 10000, &gain)) return CP_INVALID;
        candidate->motion_gain_x_centi = candidate->motion_gain_y_centi = gain;
    } else if (!strcmp(key, "pointer.deadzone_dps")) {
        field = APP_REMOTE_SETTING_DEADZONE; uint16_t value16 = defaults.motion_deadzone_mdps;
        if (!reset && !exact_scaled(&d->document, node, 1000, 0, 10000, &value16)) return CP_INVALID;
        candidate->motion_deadzone_mdps = value16;
    } else if (!strcmp(key, "pointer.filter_seconds")) {
        field = APP_REMOTE_SETTING_FILTER; uint16_t value16 = defaults.motion_filter_ms;
        if (!reset && !exact_scaled(&d->document, node, 1000, 0, 1000, &value16)) return CP_INVALID;
        candidate->motion_filter_ms = value16;
    } else if (!strcmp(key, "pointer.gain_x_hid_per_degree") || !strcmp(key, "pointer.gain_y_hid_per_degree")) {
        bool x = !strcmp(key, "pointer.gain_x_hid_per_degree");
        field = x ? APP_REMOTE_SETTING_GAIN_X : APP_REMOTE_SETTING_GAIN_Y;
        uint16_t gain = x ? defaults.motion_gain_x_centi : defaults.motion_gain_y_centi;
        if (!reset && !exact_scaled(&d->document, node, 100, 25, 10000, &gain)) return CP_INVALID;
        if (x) candidate->motion_gain_x_centi = gain; else candidate->motion_gain_y_centi = gain;
    } else if (!strcmp(key, "pointer.invert_x")) {
        field = APP_REMOTE_SETTING_INVERT_X; candidate->motion_invert_x = reset ? defaults.motion_invert_x : value->value.boolean;
    } else if (!strcmp(key, "pointer.invert_y")) {
        field = APP_REMOTE_SETTING_INVERT_Y; candidate->motion_invert_y = reset ? defaults.motion_invert_y : value->value.boolean;
    } else if (!strcmp(key, "pointer.freeze_ms")) {
        field = APP_REMOTE_SETTING_FREEZE; candidate->motion_freeze_ms = reset ? defaults.motion_freeze_ms : (uint16_t)value->value.integer;
    } else return CP_NOT_IMPLEMENTED;
    if (*fields & field) return CP_INVALID;
    if ((field == APP_REMOTE_SETTING_POINTER_GAIN && (*fields & (APP_REMOTE_SETTING_GAIN_X | APP_REMOTE_SETTING_GAIN_Y))) ||
        ((field == APP_REMOTE_SETTING_GAIN_X || field == APP_REMOTE_SETTING_GAIN_Y) && (*fields & APP_REMOTE_SETTING_POINTER_GAIN))) return CP_INVALID;
    *fields |= field; *scopes |= metadata->scope;
    return CP_OK;
}
static cp_error_t candidate(app_remote_device_t *d, const app_remote_device_state_t *state,
    bool reset, app_remote_device_request_t *request, cp_scope_mask_t *scopes)
{
    request->settings = state->settings;
    request->durable = true;
    uint16_t durable = cj_get(&d->document, 0, "durable");
    if (durable != CJ_NONE) request->durable = d->document.nodes[durable].value.boolean;
    uint16_t list = cj_get(&d->document, 0, reset ? "keys" : "patch");
    if (list == CJ_NONE || d->document.nodes[list].child == CJ_NONE) return CP_INVALID;
    for (uint16_t n = d->document.nodes[list].child; n != CJ_NONE; n = d->document.nodes[n].next) {
        const char *key = reset ? cj_string(&d->document, n) : d->document.text + d->document.nodes[n].key;
        cp_error_t error = setting(d, key, reset ? CJ_NONE : n, reset, &request->settings, &request->settings_fields, scopes);
        if (error != CP_OK) return error;
    }
    if (!device_settings_validate(&request->settings)) return CP_INVALID;
    if (!d->port.validate_settings) return CP_UNAVAILABLE;
    return d->port.validate_settings(d->port.user, &request->settings, request->settings_fields, request->durable);
}

static bool approval_matches(const app_remote_device_request_t *request, const app_remote_device_approval_t *approval, uint64_t now)
{
    const cp_operation_info_t *info = operation_info(request->operation);
    return info && !approval->cancelled && now < approval->expires_ms && approval->operation == request->operation &&
        same_owner(&approval->owner, &request->identity) &&
        (request->identity.scopes & approval->required_scopes) == approval->required_scopes &&
        (approval->required_scopes & info->scopes) == info->scopes &&
        approval->management_generation == request->management_generation &&
        approval->resource_revision == request->resource_revision &&
        equal(approval->action_digest, request->action_digest, 32);
}
static bool same_result(const cp_result_t *a, const cp_result_t *b)
{
    return a->state == b->state && a->error == b->error && a->resource_revision == b->resource_revision &&
        !strcmp(a->object_id, b->object_id) && !strcmp(a->reason, b->reason);
}
static void set_outcome(device_job_t *job, const app_remote_device_outcome_t *outcome)
{
    cp_result_t result = job->result;
    result.error = outcome->error; result.resource_revision = outcome->resource_revision;
    bool confirmed = outcome->boundary_confirmed;
    switch (outcome->phase) {
    case APP_REMOTE_DEVICE_COMPLETED: case APP_REMOTE_DEVICE_EFFECTIVE:
        if (!confirmed) { result.state = CP_RESULT_RUNNING; reason(&result, "awaiting_boundary"); }
        else if (job->request.settings_fields && job->request.durable && !outcome->durable_confirmed) {
            result.state = CP_RESULT_RUNNING; reason(&result, "applied_saving");
        } else {
            result.state = outcome->phase == APP_REMOTE_DEVICE_EFFECTIVE || configuration(job->request.operation) ||
                job->request.operation == CP_OP_PREFERENCES_RESET ? CP_RESULT_APPLIED : CP_RESULT_COMPLETED;
            reason(&result, job->request.settings_fields && job->request.durable ? "saved" : "boundary_confirmed"); job->finished = true;
        }
        break;
    case APP_REMOTE_DEVICE_CANCELLED:
        result.state = confirmed ? CP_RESULT_CANCELLED : CP_RESULT_CLEANUP_PENDING;
        reason(&result, confirmed ? "cancelled" : "cleanup_pending"); job->finished = confirmed; break;
    case APP_REMOTE_DEVICE_FAILED:
        result.state = CP_RESULT_FAILED; reason(&result, "owner_failed"); job->finished = true; break;
    case APP_REMOTE_DEVICE_UNCERTAIN:
        result.state = CP_RESULT_CLEANUP_PENDING; result.error = CP_STORAGE_UNCERTAIN; reason(&result, "boundary_uncertain"); break;
    case APP_REMOTE_DEVICE_CLEANUP_PENDING:
        result.state = CP_RESULT_CLEANUP_PENDING; reason(&result, "cleanup_pending"); break;
    case APP_REMOTE_DEVICE_RESTART_PENDING:
        result.state = CP_RESULT_RUNNING; reason(&result, "restart_pending"); break;
    default: result.state = CP_RESULT_RUNNING; reason(&result, "owner_pending"); break;
    }
    if (!same_result(&result, &job->result)) { job->result = result; job->publication_pending = true; }
}
static void submit_job(app_remote_device_t *d, device_job_t *job)
{
    job->submitted = true; job->waiting_approval = false;
    app_remote_device_outcome_t outcome = {.phase = APP_REMOTE_DEVICE_PENDING};
    cp_error_t error = d->port.submit(d->port.user, &job->request, &outcome);
    if (error != CP_OK) outcome = (app_remote_device_outcome_t){.phase = error == CP_STORAGE_UNCERTAIN ?
        APP_REMOTE_DEVICE_UNCERTAIN : APP_REMOTE_DEVICE_FAILED, .error = error};
    set_outcome(job, &outcome);
}
static cp_error_t start(app_remote_device_t *d, app_remote_device_request_t *request,
    const uint8_t command_fingerprint[32], uint64_t now, cp_result_t *result)
{
    device_job_t *job = NULL;
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i)
        if (d->jobs[i].used && !d->jobs[i].finished && (domains(request) & domains(&d->jobs[i].request))) return rejected(result, CP_BUSY);
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i)
        if (!d->jobs[i].used || (d->jobs[i].finished && !d->jobs[i].publication_pending)) { job = &d->jobs[i]; break; }
    if (!job || d->next_token == UINT64_MAX) return rejected(result, CP_BUSY);
    request->token = ++d->next_token;
    wipe(job, sizeof(*job)); job->used = true; job->request = *request;
    memcpy(job->fingerprint, command_fingerprint, 32);
    job->result = (cp_result_t){.state = CP_RESULT_ACCEPTED, .command_key = request->command_key,
        .job_id = request->token, .resource_revision = request->resource_revision, .uncertain_index = -1};
    reason(&job->result, "accepted");
    if (request->approval_required) {
        app_remote_device_approval_t approval = {0}; cp_error_t error = CP_OK;
        if (request->approval_id[0]) {
            if (!d->port.approval || !d->port.approval(d->port.user, request->approval_id, &approval)) error = CP_STALE;
        } else if (!d->port.approval_prepare) error = CP_UNAVAILABLE;
        else error = d->port.approval_prepare(d->port.user, request, &approval);
        if (error != CP_OK || !approval_matches(request, &approval, now) || approval.consumed) {
            wipe(job, sizeof(*job)); return rejected(result, error != CP_OK ? error : CP_STALE);
        }
        uint8_t id[CP_ID_BYTES];
        if (!hex_decode(approval.id, id, CP_ID_BYTES)) { wipe(job, sizeof(*job)); return rejected(result, CP_INTERNAL); }
        memcpy(job->request.approval_id, approval.id, sizeof(approval.id));
        memcpy(job->result.object_id, approval.id, sizeof(approval.id));
        job->waiting_approval = true;
        job->request.deadline_ms = approval.expires_ms;
        reason(&job->result, approval.physical_required && !approval.physical_satisfied ? "physical_approval_required" : "confirmation_required");
    } else submit_job(d, job);
    job->publication_pending = false; /* Returned to the original dispatch now. */
    *result = job->result;
    return result->state == CP_RESULT_FAILED ? result->error : CP_OK;
}

size_t app_remote_device_bytes(void) { return sizeof(app_remote_device_t); }
cp_error_t app_remote_device_init(void *memory, size_t bytes, const app_remote_device_port_t *port, app_remote_device_t **out)
{
    if (!memory || bytes < sizeof(app_remote_device_t) || !port || !out || (uintptr_t)memory % _Alignof(app_remote_device_t)) return CP_INVALID;
    app_remote_device_t *d = memory; memset(d, 0, sizeof(*d)); d->port = *port; *out = d; return CP_OK;
}
bool app_remote_device_available(app_remote_device_t *d, cp_operation_id_t operation, char message[CP_REASON_CAPACITY])
{
    if (message) message[0] = 0;
    const cp_operation_info_t *info = operation_info(operation);
    bool base = d && info && info->initial_v1_supported && owned_operation(operation) &&
        d->port.observe && d->port.available && d->port.submit && d->port.poll && d->port.complete;
    if (base && configuration(operation)) base = d->port.validate_settings && d->port.random;
    if (base && (destructive(operation) || operation == CP_OP_OPERATION_CONFIRM || operation == CP_OP_OPERATION_CANCEL))
        base = d->port.approval && d->port.approval_decide && d->port.approval_claim;
    if (!base) { if (message) (void)snprintf(message, CP_REASON_CAPACITY, "%s", "owner_unavailable"); return false; }
    char local[CP_REASON_CAPACITY] = {0};
    return d->port.available(d->port.user, operation, 0, message ? message : local);
}

static cp_error_t execute(app_remote_device_t *d, const cp_command_t *command, uint64_t now,
    cp_result_t *result, bool ui_authorized)
{
    if (!result) return CP_INVALID;
    *result = (cp_result_t){.state = CP_RESULT_REJECTED, .command_key = command ? command->command_key : 0, .uncertain_index = -1};
    if (!d || !command || command->version != CP_API_VERSION || !command->command_key || !owned_operation(command->operation)) return rejected(result, CP_INVALID);
    const uint8_t empty[CP_ID_BYTES] = {0};
    if (equal(command->identity.principal_id, empty, sizeof(empty)) || equal(command->identity.session_id, empty, sizeof(empty))) return rejected(result, CP_UNAUTHENTICATED);
    if (ui_operation(command->operation) && !ui_authorized) return rejected(result, CP_FORBIDDEN);
    if (parse(d, command) != CP_OK) return rejected(result, CP_INVALID);
    uint8_t command_fingerprint[32]; fingerprint(command, command_fingerprint);
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i) if (d->jobs[i].used && d->jobs[i].request.command_key == command->command_key) {
        if (!equal(command_fingerprint, d->jobs[i].fingerprint, 32)) return rejected(result, CP_REQUEST_CONFLICT);
        *result = d->jobs[i].result; return CP_OK;
    }
    if (!clock_valid(d, now) || command->deadline_ms <= now) return rejected(result, CP_EXPIRED);
    const cp_operation_info_t *info = operation_info(command->operation);
    if (!info || (command->identity.scopes & info->scopes) != info->scopes) return rejected(result, CP_FORBIDDEN);
    char unavailable[CP_REASON_CAPACITY];
    if (!app_remote_device_available(d, command->operation, unavailable)) return rejected(result, CP_UNAVAILABLE);
    app_remote_device_state_t state;
    if (!d->port.observe(d->port.user, command->operation, &state)) return rejected(result, CP_UNAVAILABLE);
    bool ui = ui_operation(command->operation);
    if (ui && (!ui_authorized || command->ui_revision != state.ui_revision)) return rejected(result, CP_FORBIDDEN);
    if (!ui && command->management_generation != state.management_generation) return rejected(result, CP_STALE);
    bool safety = command->operation == CP_OP_CONFIG_CANCEL || command->operation == CP_OP_OPERATION_CANCEL;
    if (!safety && state.locked) return rejected(result, CP_LOCKED);
    if (!safety && (state.local_busy || state.transition_busy)) return rejected(result, CP_LOCAL_BUSY);
    if (ui && !state.screen_awake) return rejected(result, CP_UNAVAILABLE);
    app_remote_device_request_t request = {.operation = command->operation, .command_key = command->command_key,
        .identity = command->identity, .management_generation = state.management_generation,
        .resource_revision = state.resource_revision, .settings_revision = state.settings_revision,
        .bluetooth_generation = state.bluetooth_generation, .bond_revision = state.bond_revision};
    if (now > UINT64_MAX - APP_REMOTE_DEVICE_STAGE_MS) return rejected(result, CP_INTERNAL);
    request.deadline_ms = now + APP_REMOTE_DEVICE_JOB_MS;
    if (configuration(command->operation)) {
        if (command->operation != CP_OP_CONFIG_CANCEL && command->resource_revision != state.settings_revision) return rejected(result, CP_STALE);
        request.resource_revision = state.settings_revision;
        if (command->operation == CP_OP_CONFIG_STAGE || command->operation == CP_OP_CONFIG_RESET) {
            cp_scope_mask_t scopes = CP_SCOPE_VIEW;
            cp_error_t error = candidate(d, &state, command->operation == CP_OP_CONFIG_RESET, &request, &scopes);
            if (error != CP_OK) return rejected(result, error);
            if ((command->identity.scopes & scopes) != scopes) return rejected(result, CP_FORBIDDEN);
            if (!d->port.available(d->port.user, command->operation, request.settings_fields, unavailable)) return rejected(result, CP_UNAVAILABLE);
            if (command->operation == CP_OP_CONFIG_STAGE) {
                transaction_t *transaction = NULL;
                for (size_t i = 0; i < APP_REMOTE_DEVICE_TRANSACTIONS; ++i)
                    if (!d->transactions[i].used || now >= d->transactions[i].expires_ms) { transaction = &d->transactions[i]; break; }
                if (!transaction) return rejected(result, CP_BUSY);
                uint8_t id[CP_ID_BYTES] = {0}; bool unique = false; char hex[CP_ID_HEX_CAPACITY];
                for (unsigned attempt = 0; attempt < 4 && !unique; ++attempt) {
                    if (!d->port.random(d->port.user, id, sizeof(id))) return rejected(result, CP_UNAVAILABLE);
                    hex_encode(id, sizeof(id), hex); unique = strcmp(hex, "00000000000000000000000000000000") != 0;
                    for (size_t i = 0; i < APP_REMOTE_DEVICE_TRANSACTIONS; ++i)
                        if (d->transactions[i].used && !strcmp(d->transactions[i].id, hex)) unique = false;
                }
                if (!unique) return rejected(result, CP_INTERNAL);
                *transaction = (transaction_t){.used = true, .owner = command->identity, .scopes = scopes,
                    .management_generation = state.management_generation, .revision = state.settings_revision,
                    .expires_ms = now + APP_REMOTE_DEVICE_STAGE_MS, .candidate = request.settings,
                    .fields = request.settings_fields, .durable = request.durable};
                memcpy(transaction->id, hex, sizeof(hex)); memcpy(result->object_id, hex, sizeof(hex));
                result->state = CP_RESULT_APPLIED; result->resource_revision = state.settings_revision; reason(result, "staged_inert"); return CP_OK;
            }
        } else {
            char id[CP_ID_HEX_CAPACITY];
            if (!string(&d->document, 0, "transaction_id", id, sizeof(id))) return rejected(result, CP_INVALID);
            transaction_t *transaction = NULL;
            for (size_t i = 0; i < APP_REMOTE_DEVICE_TRANSACTIONS; ++i)
                if (d->transactions[i].used && !strcmp(d->transactions[i].id, id)) { transaction = &d->transactions[i]; break; }
            if (!transaction || !same_owner(&transaction->owner, &command->identity)) return rejected(result, CP_STALE);
            if ((command->identity.scopes & transaction->scopes) != transaction->scopes) return rejected(result, CP_FORBIDDEN);
            if (command->operation == CP_OP_CONFIG_CANCEL) {
                wipe(transaction, sizeof(*transaction)); result->state = CP_RESULT_CANCELLED; reason(result, "stage_cancelled"); return CP_OK;
            }
            if (now >= transaction->expires_ms || state.management_generation != transaction->management_generation ||
                state.settings_revision != transaction->revision) return rejected(result, CP_STALE);
            request.settings = transaction->candidate; request.settings_fields = transaction->fields; request.durable = transaction->durable;
            cp_error_t error = d->port.validate_settings(d->port.user, &request.settings, request.settings_fields, request.durable);
            if (error != CP_OK) return rejected(result, error);
            if (!d->port.available(d->port.user, command->operation, request.settings_fields, unavailable)) return rejected(result, CP_UNAVAILABLE);
            wipe(transaction, sizeof(*transaction));
        }
    } else if (command->operation == CP_OP_OPERATION_CONFIRM || command->operation == CP_OP_OPERATION_CANCEL) {
        char id[CP_ID_HEX_CAPACITY], hex[65]; uint8_t digest[32]; uint64_t revision;
        if (!string(&d->document, 0, "operation_id", id, sizeof(id)) || !string(&d->document, 0, "digest", hex, sizeof(hex)) ||
            !hex_decode(hex, digest, sizeof(digest)) || !cj_u64(&d->document, cj_get(&d->document, 0, "revision"), &revision)) return rejected(result, CP_INVALID);
        app_remote_device_approval_t approval;
        if (!d->port.approval(d->port.user, id, &approval) || !same_owner(&approval.owner, &command->identity) ||
            !equal(digest, approval.action_digest, sizeof(digest)) || revision != approval.resource_revision ||
            command->resource_revision != revision || approval.management_generation != state.management_generation ||
            (command->identity.scopes & approval.required_scopes) != approval.required_scopes) return rejected(result, CP_FORBIDDEN);
        if (approval.cancelled || approval.consumed || now >= approval.expires_ms) return rejected(result, CP_STALE);
        bool confirm = command->operation == CP_OP_OPERATION_CONFIRM;
        if (confirm && approval.physical_required && !approval.physical_satisfied) return rejected(result, CP_FORBIDDEN);
        app_remote_device_state_t original;
        if (confirm && (!d->port.observe(d->port.user, approval.operation, &original) ||
            original.resource_revision != approval.resource_revision || original.locked || original.transition_busy)) return rejected(result, CP_STALE);
        cp_error_t error = d->port.approval_decide(d->port.user, &approval, confirm);
        if (error != CP_OK) return rejected(result, error);
        result->state = confirm ? CP_RESULT_APPLIED : CP_RESULT_CANCELLED;
        result->resource_revision = approval.resource_revision; memcpy(result->object_id, id, sizeof(id));
        reason(result, confirm ? "approval_confirmed" : "approval_cancelled"); return CP_OK;
    } else {
        if (!ui && command->resource_revision != state.resource_revision) return rejected(result, CP_STALE);
        optional_string(&d->document, "peer_id", request.peer_id, sizeof(request.peer_id));
        optional_string(&d->document, "pattern", request.pattern, sizeof(request.pattern));
        optional_string(&d->document, "overlay", request.overlay, sizeof(request.overlay));
        optional_string(&d->document, "page", request.page, sizeof(request.page));
        optional_string(&d->document, "group", request.group, sizeof(request.group));
        optional_string(&d->document, "confirmation_id", request.approval_id, sizeof(request.approval_id));
        if (command->operation == CP_OP_SYSTEM_RESTART) optional_string(&d->document, "approval_id", request.approval_id, sizeof(request.approval_id));
        optional_string(&d->document, "retained_manifest_digest", request.retained_manifest_digest, sizeof(request.retained_manifest_digest));
        int64_t integer;
        request.window_seconds = cj_integer(&d->document, cj_get(&d->document, 0, "window_seconds"), &integer) ? (uint32_t)integer : 120;
        request.timeout_ms = cj_integer(&d->document, cj_get(&d->document, 0, "timeout_ms"), &integer) ? (uint32_t)integer : 10000;
        request.duration_ms = cj_integer(&d->document, cj_get(&d->document, 0, "duration_ms"), &integer) ? (uint32_t)integer : 60;
        request.strength_percent = cj_integer(&d->document, cj_get(&d->document, 0, "strength_percent"), &integer) ? (uint8_t)integer : 100;
        request.duration_provided = cj_get(&d->document, 0, "duration_ms") != CJ_NONE;
        request.strength_provided = cj_get(&d->document, 0, "strength_percent") != CJ_NONE;
        uint64_t generation;
        uint16_t node = cj_get(&d->document, 0, "generation");
        if (node != CJ_NONE && (!cj_u64(&d->document, node, &generation) || generation != state.bluetooth_generation)) return rejected(result, CP_STALE);
        if (command->operation == CP_OP_IMU_CALIBRATE) request.deadline_ms = now + request.timeout_ms;
        request.approval_required = destructive(command->operation);
        if (command->operation == CP_OP_PREFERENCES_RESET || command->operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST) {
            cp_error_t error = reset_candidate(&request, &state.settings);
            if (error != CP_OK) return rejected(result, error);
            error = d->port.validate_settings(d->port.user, &request.settings, request.settings_fields, true);
            if (error != CP_OK) return rejected(result, error);
        }
    }
    action_digest(&request);
    return start(d, &request, command_fingerprint, now, result);
}

cp_error_t app_remote_device_execute(app_remote_device_t *d, const cp_command_t *command, uint64_t now, cp_result_t *result)
{
    cp_error_t error = execute(d, command, now, result, false);
    if (d) wipe(&d->document, sizeof(d->document));
    return error;
}
cp_error_t app_remote_device_execute_ui_authorized(app_remote_device_t *d, const cp_command_t *command, uint64_t now, cp_result_t *result)
{
    if (!command || !ui_operation(command->operation)) return result ? rejected(result, CP_INVALID) : CP_INVALID;
    cp_error_t error = execute(d, command, now, result, true);
    if (d) wipe(&d->document, sizeof(d->document));
    return error;
}

static void cancel_job(app_remote_device_t *d, device_job_t *job)
{
    if (job->cancelling || job->finished) return;
    job->cancelling = true;
    if (!job->submitted) {
        app_remote_device_approval_t approval;
        if (job->waiting_approval && d->port.approval &&
            d->port.approval(d->port.user, job->request.approval_id, &approval) && !approval.cancelled &&
            (!d->port.approval_decide || d->port.approval_decide(d->port.user, &approval, false) != CP_OK)) {
            set_outcome(job, &(app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CLEANUP_PENDING, .error = CP_TOO_LATE});
            return;
        }
        set_outcome(job, &(app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CANCELLED, .boundary_confirmed = true});
        return;
    }
    cp_error_t error = d->port.cancel ? d->port.cancel(d->port.user, job->request.token) : CP_UNAVAILABLE;
    set_outcome(job, &(app_remote_device_outcome_t){.phase = APP_REMOTE_DEVICE_CLEANUP_PENDING, .error = error});
}
void app_remote_device_service(app_remote_device_t *d, uint64_t now)
{
    if (!d) return;
    bool valid_clock = clock_valid(d, now);
    for (size_t i = 0; i < APP_REMOTE_DEVICE_TRANSACTIONS; ++i)
        if (d->transactions[i].used && (!valid_clock || now >= d->transactions[i].expires_ms)) wipe(&d->transactions[i], sizeof(d->transactions[i]));
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i) {
        device_job_t *job = &d->jobs[i]; if (!job->used) continue;
        if (!job->finished) {
            app_remote_device_state_t state;
            bool current = valid_clock && d->port.observe(d->port.user, job->request.operation, &state) &&
                state.management_generation == job->request.management_generation && !state.locked;
            if (!current || now >= job->request.deadline_ms) cancel_job(d, job);
            if (!job->finished && job->waiting_approval && !job->cancelling) {
                app_remote_device_approval_t approval;
                if (!d->port.approval(d->port.user, job->request.approval_id, &approval) || !approval_matches(&job->request, &approval, now)) cancel_job(d, job);
                else if (approval.confirmed && (!approval.physical_required || approval.physical_satisfied)) {
                    char message[CP_REASON_CAPACITY] = {0};
                    if (state.resource_revision != job->request.resource_revision || state.transition_busy || state.local_busy ||
                        !d->port.available(d->port.user, job->request.operation, job->request.settings_fields, message) ||
                        !d->port.approval_claim || d->port.approval_claim(d->port.user, &approval) != CP_OK) cancel_job(d, job);
                    else {
                        job->request.deadline_ms = now + (job->request.operation == CP_OP_DEVICE_FACTORY_RESET_REQUEST ?
                            APP_REMOTE_DEVICE_STAGE_MS : APP_REMOTE_DEVICE_JOB_MS);
                        submit_job(d, job);
                    }
                }
            } else if (!job->finished && job->submitted) {
                app_remote_device_outcome_t outcome;
                if (d->port.poll(d->port.user, job->request.token, &outcome)) set_outcome(job, &outcome);
            }
        }
        if (job->publication_pending && d->port.complete && d->port.complete(d->port.user, &job->result)) job->publication_pending = false;
    }
}
void app_remote_device_revoke(app_remote_device_t *d, cp_safety_reason_t why, const cp_identity_t *identity, uint64_t now)
{
    if (!d) return;
    (void)clock_valid(d, now);
    bool input_only = why == CP_SAFETY_LOCAL_OVERRIDE || why == CP_SAFETY_LEASE_EXPIRED ||
        why == CP_SAFETY_STATE_CHANGED || why == CP_SAFETY_CLIENT_STOP;
    for (size_t i = 0; i < APP_REMOTE_DEVICE_TRANSACTIONS; ++i)
        if (!input_only && d->transactions[i].used && (!identity || same_owner(identity, &d->transactions[i].owner))) wipe(&d->transactions[i], sizeof(d->transactions[i]));
    for (size_t i = 0; i < APP_REMOTE_DEVICE_JOBS; ++i)
        if (d->jobs[i].used && (!input_only || ui_operation(d->jobs[i].request.operation)) &&
            (!identity || same_owner(identity, &d->jobs[i].request.identity))) cancel_job(d, &d->jobs[i]);
}

cp_error_t app_remote_device_read(app_remote_device_t *d, const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!d || !identity || !resource || !json || !length || !capacity) return CP_INVALID;
    if (!(identity->scopes & CP_SCOPE_VIEW)) return CP_FORBIDDEN;
    if (strcmp(resource, "/api/v1/config")) return d->port.read_cached ?
        d->port.read_cached(d->port.user, identity, resource, json, capacity, length) : CP_NOT_IMPLEMENTED;
    app_remote_device_state_t state;
    if (!d->port.observe || !d->port.observe(d->port.user, CP_OP_CONFIG_GET, &state) || !device_settings_validate(&state.settings)) return CP_UNAVAILABLE;
    const device_settings_t *s = &state.settings;
    char name[6U * DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES + 3U];
    char scalar_gain[32];
    if (!cj_escape(s->bluetooth_name, name, sizeof(name))) return CP_INTERNAL;
    bool equal_gains = s->motion_gain_x_centi == s->motion_gain_y_centi;
    if (equal_gains) (void)snprintf(scalar_gain, sizeof(scalar_gain), "%.2f", (double)s->motion_gain_x_centi / 100);
    else (void)snprintf(scalar_gain, sizeof(scalar_gain), "%s", "null");
    int used = snprintf(json, capacity,
        "{\"revision\":\"%" PRIu64 "\",\"values\":{\"display.theme\":\"%s\",\"display.brightness_percent\":%u,"
        "\"display.idle_timeout_seconds\":%u,\"haptics.enabled\":%s,\"keyboard.shortcut_modifier\":\"%s\","
        "\"input.preferred_transport\":\"%s\",\"touch.tap_to_click\":%s,\"bluetooth.device_name\":%s,"
        "\"pointer.gain_hid_per_degree\":%s,\"pointer.deadzone_dps\":%.3f,\"pointer.filter_seconds\":%.3f,"
        "\"pointer.gain_x_hid_per_degree\":%.2f,\"pointer.gain_y_hid_per_degree\":%.2f,\"pointer.invert_x\":%s,"
        "\"pointer.invert_y\":%s,\"pointer.freeze_ms\":%u},"
        "\"pointer_gain_axes_equal\":%s,\"pointer_gain_axes\":{\"x\":%.2f,\"y\":%.2f},\"durability\":\"owner_status_required\"}",
        state.settings_revision, s->theme == DEVICE_THEME_DARK ? "dark" : "light", s->brightness_percent,
        (unsigned)s->idle_timeout * 60U, s->haptics_enabled ? "true" : "false",
        s->host_modifier == DEVICE_HOST_COMMAND ? "command" : "ctrl",
        s->preferred_transport == DEVICE_TRANSPORT_USB ? "usb" : "bluetooth", s->tap_to_click ? "true" : "false", name,
        scalar_gain, (double)s->motion_deadzone_mdps / 1000, (double)s->motion_filter_ms / 1000,
        (double)s->motion_gain_x_centi / 100, (double)s->motion_gain_y_centi / 100,
        s->motion_invert_x ? "true":"false", s->motion_invert_y ? "true":"false", s->motion_freeze_ms,
        equal_gains ? "true" : "false", (double)s->motion_gain_x_centi / 100, (double)s->motion_gain_y_centi / 100);
    if (used < 0 || (size_t)used >= capacity) { json[0] = 0; return CP_TOO_LARGE; }
    *length = (size_t)used; return CP_OK;
}
