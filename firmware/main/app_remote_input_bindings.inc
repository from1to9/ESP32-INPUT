// SPDX-License-Identifier: Apache-2.0
/* Included once by main.c after native semantic helpers. This is the shared
 * coordinator composition: no second engine, event loop or transport sender. */
#include "app_control_access.h"
static bool remote_authorize(void *context, const cp_identity_t *identity)
{
    (void)context;
    cp_principal_t current;
    if (!identity || !app_control_access_principal(identity->principal_id, &current) || !current.enabled ||
        !(current.scopes & CP_SCOPE_INPUT_CONTROL) ||
        !app_control_runtime_session_alive(identity->session_id, identity->principal_id)) return false;
    return app_control_access_managed(identity->principal_id) ? app_control_access_input(identity) : s_remote_allowed;
}
static cp_device_state_t s_published_control_state;
static bool s_control_state_published;
static portMUX_TYPE s_command_lock = portMUX_INITIALIZER_UNLOCKED;
static cp_result_t s_remote_transport_result;
static bool s_remote_transport_pending;
static app_input_transport_t s_remote_transport_target;
static cp_identity_t s_remote_transport_identity;
static uint8_t s_remote_transport_lease[CP_ID_BYTES];
static uint64_t s_remote_transport_deadline, s_remote_transport_epoch, s_remote_transport_management;
static bool s_remote_transport_effect, s_remote_transport_cancelled;
static void remote_transport_cancel(cp_error_t error)
{
    if (!s_remote_transport_pending || s_remote_transport_effect || s_remote_transport_cancelled) return;
    s_remote_transport_cancelled = true;
    s_remote_transport_result.state = CP_RESULT_CANCELLED;
    s_remote_transport_result.error = error;
    s_transport_switch_pending = false;
    s_requested_transport = s_input_transport;
    ++s_ui_revision;
}
static bool remote_transport_admit(void)
{
    if (!s_remote_transport_pending || s_remote_transport_effect) return true;
    app_remote_input_view_t view = {0}; app_remote_input_snapshot(s_remote_input, &view);
    if (s_remote_transport_cancelled) return true; /* Receipt retry owns no local switch. */
    if (s_now_ms >= s_remote_transport_deadline || s_now_ms >= view.lease_expires_ms) {
        remote_transport_cancel(CP_EXPIRED); return false;
    }
    if (!remote_authorize(NULL, &s_remote_transport_identity) || s_engine.state.locked || s_engine.state.owner != INPUT_REMOTE ||
        s_engine.state.epoch != s_remote_transport_epoch || s_management_generation != s_remote_transport_management ||
        memcmp(view.lease_id, s_remote_transport_lease, CP_ID_BYTES) ||
        memcmp(view.owner_session, s_remote_transport_identity.session_id, CP_ID_BYTES)) {
        remote_transport_cancel(CP_STALE); return false;
    }
    return true;
}
static void remote_transport_applied(void)
{
    if (s_remote_transport_pending && !s_remote_transport_cancelled) s_remote_transport_effect = true;
}

static bool remote_observe(void *context, app_remote_input_state_t *state)
{
    (void)context;
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    input_link_t active = input_link_snapshot(&bluetooth);
    portENTER_CRITICAL(&s_control_lock);
    bool busy = s_touch_down || s_physical_down;
    bool awake = s_screen_accepts_input;
    portEXIT_CRITICAL(&s_control_lock);
    *state = (app_remote_input_state_t){.transport_generation = active.generation,
        .management_generation = s_management_generation, .ui_revision = s_ui_revision,
        .draft_revision = s_ui_revision, .selection_revision = s_ui_revision,
        .state_sequence = s_state_sequence,
        .remote_allowed = ((s_remote_allowed && s_remote_allowance_generation != UINT64_MAX) ||
            app_control_access_any_input()) && !s_engine.state.locked && !remote_update_maintenance() &&
            s_state_sequence != UINT64_MAX,
        .local_busy = busy, .screen_awake = awake,
        .overlay_open = s_overlay != APP_OVERLAY_NONE,
        .transition_busy = s_transport_switch_pending || s_pair_operation != APP_CONFIRM_NONE,
        .native_send_active = s_send_token != 0,
        .selection_visible = s_overlay == APP_OVERLAY_NONE && s_engine.state.mode == INPUT_KEYBOARD &&
            (s_keyboard_submode == APP_KEYBOARD_MOTION || s_keyboard_submode == APP_KEYBOARD_TOUCH),
        .shortcut_modifier = s_host_os == APP_HOST_COMMAND ? 0x08U : 0x01U};
    if (active.generation) (void)snprintf(state->target_id, sizeof(state->target_id), "%s:%llu",
        active.kind == APP_INPUT_USB ? "usb" : "bluetooth", (unsigned long long)active.generation);
    return true;
}
static bool same_control_state(const cp_device_state_t *a, const cp_device_state_t *b)
{
    return a->management_generation == b->management_generation && a->input_epoch == b->input_epoch &&
        a->transport_generation == b->transport_generation && a->ui_revision == b->ui_revision &&
        a->allowance_generation == b->allowance_generation && a->locked == b->locked &&
        a->input_ready == b->input_ready && a->release_pending == b->release_pending &&
        a->local_busy == b->local_busy && a->remote_allowed == b->remote_allowed &&
        a->remote_owner == b->remote_owner && !strcmp(a->target_id, b->target_id) && !strcmp(a->mode, b->mode) &&
        !memcmp(a->owner_session, b->owner_session, CP_ID_BYTES);
}
static bool remote_publish(void)
{
    app_remote_input_state_t observed;
    (void)remote_observe(NULL, &observed);
    app_remote_input_view_t remote = {0};
    app_remote_input_snapshot(s_remote_input, &remote);
    cp_device_state_t state = {.management_generation = s_management_generation,
        .input_epoch = s_engine.state.epoch, .transport_generation = observed.transport_generation,
        .ui_revision = s_ui_revision, .state_sequence = s_state_sequence,
        .allowance_generation = s_remote_allowance_generation,
        .locked = s_engine.state.locked, .input_ready = s_engine.ready,
        .release_pending = s_engine.state.release_pending, .local_busy = observed.local_busy,
        .remote_allowed = s_remote_allowed && observed.remote_allowed, .remote_owner = s_engine.state.owner == INPUT_REMOTE};
    memcpy(state.target_id, observed.target_id, sizeof(state.target_id));
    memcpy(state.owner_session, remote.owner_session, sizeof(state.owner_session));
    (void)snprintf(state.mode, sizeof(state.mode), "%s", s_engine.state.mode == INPUT_MOUSE ? "mouse" :
        s_engine.state.mode == INPUT_KEYBOARD ? "keyboard" : "audio");
    if (s_control_state_published && !same_control_state(&state, &s_published_control_state)) {
        if (s_state_sequence == UINT64_MAX) return false;
        state.state_sequence = s_state_sequence + 1;
    }
    if (app_control_runtime_publish_state(&state) != ESP_OK) return false;
    s_state_sequence = state.state_sequence;
    s_published_control_state = state; s_control_state_published = true;
    return true;
}
static bool remote_complete(void *context, const cp_result_t *result)
{
    (void)context;
    return remote_publish() && app_control_runtime_complete(result->command_key, result) == ESP_OK;
}
#include "app_remote_media_bindings.inc"
static void remote_changed(void *context, unsigned flags)
{
    (void)context;
    if ((flags & (APP_REMOTE_CHANGED_OWNER | APP_REMOTE_CHANGED_UI | APP_REMOTE_CHANGED_DRAFT |
                  APP_REMOTE_CHANGED_SELECTION)) && s_ui_revision != UINT64_MAX) ++s_ui_revision;
    sync_interaction();
}
static bool remote_delegate_available(void *context, cp_operation_id_t operation, char reason[CP_REASON_CAPACITY])
{
    (void)context; reason[0] = 0;
    if (operation == CP_OP_UI_NAVIGATE || operation == CP_OP_UI_CLOSE ||
        operation == CP_OP_UI_PRIMARY_ACTIVATE || operation == CP_OP_UI_MODE_PANEL_OPEN)
        return remote_device_available(operation, reason);
    return operation == CP_OP_MODE_SELECT || operation == CP_OP_INPUT_MOTION_SET ||
        operation == CP_OP_INPUT_MOTION_RECENTER ||
        operation == CP_OP_KEYBOARD_SUBMODE_SELECT || operation == CP_OP_INPUT_TRANSPORT_SELECT ||
        operation == CP_OP_INPUT_RECOVER;
}
static cp_error_t remote_delegate(void *context, const cp_command_t *command, cp_result_t *result)
{
    (void)context;
    char value[24]; bool enabled;
    ble_hid_snapshot_t bluetooth = ble_hid_snapshot();
    *result = (cp_result_t){.command_key = command->command_key, .state = CP_RESULT_APPLIED,
        .uncertain_index = -1};
    switch (command->operation) {
    case CP_OP_UI_NAVIGATE: case CP_OP_UI_CLOSE: case CP_OP_UI_PRIMARY_ACTIVATE:
    case CP_OP_UI_MODE_PANEL_OPEN:
        return remote_device_execute_ui(command, result);
    case CP_OP_MODE_SELECT:
        if (!cp_command_string(command, "mode", value, sizeof(value))) return CP_INVALID;
        if (!strcmp(value, "mouse")) return select_mode(INPUT_MOUSE) ? CP_OK : CP_BUSY;
        if (!strcmp(value, "keyboard")) return select_mode(INPUT_KEYBOARD) ? CP_OK : CP_BUSY;
        if (!strcmp(value, "audio")) return select_mode(INPUT_AUDIO) ? CP_OK : CP_BUSY;
        return CP_INVALID;
    case CP_OP_INPUT_MOTION_SET:
        if (!cp_command_boolean(command, "enabled", &enabled)) return CP_INVALID;
        if (enabled && !s_interaction.view.calibrated) return CP_UNAVAILABLE;
        if (enabled != s_engine.state.gyro_enabled) (void)set_motion(INPUT_REMOTE, enabled);
        return s_engine.state.gyro_enabled == enabled ? CP_OK : CP_BUSY;
    case CP_OP_INPUT_MOTION_RECENTER: {
        cp_error_t error = recenter_motion(INPUT_REMOTE);
        if (error == CP_OK) (void)snprintf(result->reason, sizeof result->reason, "motion_reference_reset");
        return error;
    }
    case CP_OP_KEYBOARD_SUBMODE_SELECT: {
        if (!cp_command_string(command, "submode", value, sizeof(value))) return CP_INVALID;
        static const char *const names[] = {"motion", "touch", "spoken", "shortcuts"};
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!strcmp(value, names[i])) return select_keyboard_submode((app_keyboard_submode_t)i) ? CP_OK : CP_BUSY;
        return CP_INVALID;
    }
    case CP_OP_INPUT_RECOVER:
        recover_input(&bluetooth);
        return CP_OK; /* The common release/recovery barrier is established, not a host receipt. */
    case CP_OP_INPUT_TRANSPORT_SELECT:
        if (s_remote_transport_pending) return CP_BUSY;
        if (!cp_command_string(command, "transport", value, sizeof(value))) return CP_INVALID;
        if (strcmp(value, "usb") && strcmp(value, "bluetooth")) return CP_INVALID;
        s_remote_transport_target = !strcmp(value, "usb") ? APP_INPUT_USB : APP_INPUT_BLUETOOTH;
        if (!request_input_transport(s_remote_transport_target, &bluetooth)) return CP_UNAVAILABLE;
        if (!s_transport_switch_pending) return CP_OK;
        result->state = CP_RESULT_ACCEPTED;
        s_remote_transport_result = *result; s_remote_transport_pending = true;
        s_remote_transport_identity = command->identity;
        memcpy(s_remote_transport_lease, command->lease_id, CP_ID_BYTES);
        s_remote_transport_deadline = command->deadline_ms;
        s_remote_transport_epoch = s_engine.state.epoch;
        s_remote_transport_management = s_management_generation;
        s_remote_transport_effect = s_remote_transport_cancelled = false;
        return CP_OK;
    default: return CP_NOT_IMPLEMENTED;
    }
}
static void command_lock(void *context) { (void)context; portENTER_CRITICAL(&s_command_lock); }
static void command_unlock(void *context) { (void)context; portEXIT_CRITICAL(&s_command_lock); }
static uint64_t command_clock(void *context) { (void)context; return (uint64_t)esp_timer_get_time() / 1000U; }
static bool command_available(void *context, cp_operation_id_t operation, char reason[CP_REASON_CAPACITY])
{
    (void)context;
    if (operation == CP_OP_DEVICE_LOCK) { reason[0] = 0; return true; }
    return app_remote_input_available(s_remote_input, operation, reason) ||
        remote_device_available(operation, reason) || app_remote_media_available(s_remote_media, operation, reason);
}
static cp_error_t command_execute(void *context, const cp_command_t *command, cp_result_t *result)
{
    (void)context;
    bool safety = false;
    for (size_t i = 0; i < cp_operation_count(); ++i) {
        const cp_operation_info_t *op = cp_operation_at(i);
        if (op && op->id == command->operation) { safety = op->safety; break; }
    }
    if (!safety && !app_control_runtime_session_alive(command->identity.session_id, command->identity.principal_id))
        return CP_UNAUTHENTICATED;
    if (!safety && remote_update_maintenance()) return CP_BUSY;
    if (command->operation == CP_OP_DEVICE_LOCK) {
        portENTER_CRITICAL(&s_control_lock); s_priority_requests |= LOCK_REQUEST; portEXIT_CRITICAL(&s_control_lock);
        s_applying_remote_safety = true; (void)process_priority(); s_applying_remote_safety = false;
        *result = (cp_result_t){.command_key = command->command_key, .state = CP_RESULT_APPLIED,
            .resource_revision = s_ui_revision, .uncertain_index = -1};
        return CP_OK; /* Locked model; release/capture cleanup remains separately observable. */
    }
    cp_error_t error = CP_NOT_IMPLEMENTED;
    if (app_remote_input_owns(command->operation))
        error = app_remote_input_execute(s_remote_input, command, s_now_ms, result);
    else if (app_remote_device_owns(command->operation)) error = remote_device_execute(command, result);
    else if (app_remote_media_owns(command->operation))
        error = app_remote_media_execute(s_remote_media, command, s_now_ms, result);
    (void)remote_publish(); return error;
}
static void command_safety(void *context, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    (void)context;
    if (!identity || (!memcmp(identity->session_id, s_remote_transport_identity.session_id, CP_ID_BYTES) &&
        !memcmp(identity->principal_id, s_remote_transport_identity.principal_id, CP_ID_BYTES)))
        remote_transport_cancel(reason == CP_SAFETY_LEASE_EXPIRED ? CP_EXPIRED : CP_STALE);
    app_remote_input_revoke(s_remote_input, reason, identity, s_now_ms);
    remote_device_revoke(reason, identity);
    app_remote_media_revoke(s_remote_media, reason, identity, s_now_ms);
    if (reason == CP_SAFETY_LOCKED) {
        app_media_runtime_stop(AE_STOP_PRIVACY);
        portENTER_CRITICAL(&s_control_lock);
        s_priority_requests |= LOCK_REQUEST;
        portEXIT_CRITICAL(&s_control_lock);
        s_applying_remote_safety = true;
        (void)process_priority();
        s_applying_remote_safety = false;
    }
}
static cp_error_t command_read(void *context, const cp_identity_t *identity, const char *resource,
    char *json, size_t capacity, size_t *length)
{
    (void)context;
    if (!strcmp(resource, "/api/v1/state")) {
        if (!identity || !(identity->scopes & CP_SCOPE_VIEW)) return CP_FORBIDDEN;
        if (!app_control_runtime_session_alive(identity->session_id, identity->principal_id)) return CP_UNAUTHENTICATED;
        cp_error_t state_error = s_remote_media ?
            app_remote_media_read(s_remote_media, identity, resource, json, capacity, length) : CP_UNAVAILABLE;
        if (state_error == CP_UNAVAILABLE) {
            static const char unavailable[] = "{\"media\":null,\"media_status\":\"unavailable\"}";
            if (capacity < sizeof(unavailable)) return CP_TOO_LARGE;
            memcpy(json, unavailable, sizeof(unavailable)); *length = sizeof(unavailable) - 1U;
        } else if (state_error != CP_OK) return state_error;
        static const char key[] = ",\"device_controls\":";
        if (!*length || json[*length - 1U] != '}' || *length >= capacity ||
            sizeof(key) + 1U > capacity - *length) return CP_TOO_LARGE;
        size_t used = *length - 1U;
        memcpy(json + used, key, sizeof(key) - 1U); used += sizeof(key) - 1U;
        size_t extra = 0;
        state_error = remote_device_state_json(identity, json + used, capacity - used - 1U, &extra);
        if (state_error != CP_OK) { json[0] = 0; *length = 0; return state_error; }
        used += extra; json[used++] = '}'; json[used] = 0; *length = used;
        return CP_OK;
    }
    cp_error_t error = remote_device_read(identity, resource, json, capacity, length);
    return error == CP_NOT_IMPLEMENTED ?
        app_remote_media_read(s_remote_media, identity, resource, json, capacity, length) : error;
}
static void remote_local_override(bool locked)
{
    /* Close before revocation/publication, including a local action that wins
     * just before a remote acquisition is dequeued. Never auto-reopen. */
    if (s_remote_allowed) {
        s_remote_allowed = false;
        if (s_remote_allowance_generation != UINT64_MAX) ++s_remote_allowance_generation;
    }
    if (!s_remote_input && !s_command_bridge) return;
    app_remote_input_revoke(s_remote_input, locked ? CP_SAFETY_LOCKED : CP_SAFETY_LOCAL_OVERRIDE, NULL, s_now_ms);
    if (s_command_bridge) app_command_bridge_safety(s_command_bridge,
        locked ? CP_SAFETY_LOCKED : CP_SAFETY_LOCAL_OVERRIDE, NULL);
    app_control_runtime_physical_override(locked);
}
static bool set_remote_allowance(bool allowed)
{
    if (allowed && (!s_remote_input || !s_command_bridge || remote_update_maintenance() || s_engine.state.locked ||
        s_idle.asleep || s_idle.wake_capture || s_remote_allowance_generation == UINT64_MAX)) return false;
    if (!allowed) { remote_local_override(false); return true; }
    if (!s_remote_allowed) {
        ++s_remote_allowance_generation;
        if (s_remote_allowance_generation == UINT64_MAX) return false;
        s_remote_allowed = true;
    }
    return true;
}
static void remote_control_service(void)
{
    if (!s_command_bridge) return;
    app_remote_input_service(s_remote_input, s_now_ms);
    remote_device_service();
    app_remote_media_service(s_remote_media, s_now_ms);
    app_command_bridge_service(s_command_bridge);
    (void)remote_transport_admit();
    if (s_remote_transport_pending && (s_remote_transport_cancelled || s_remote_transport_effect || !s_transport_switch_pending)) {
        cp_result_t result = s_remote_transport_result;
        if (!s_remote_transport_cancelled) {
            result.state = s_remote_transport_effect ? CP_RESULT_APPLIED : CP_RESULT_FAILED;
            result.error = result.state == CP_RESULT_APPLIED ? CP_OK : CP_UNAVAILABLE;
        }
        if (remote_complete(NULL, &result)) s_remote_transport_pending = false;
    }
    app_command_bridge_refresh(s_command_bridge);
    (void)remote_publish();
}
static void remote_control_init(bool usb_available)
{
    uint64_t generation;
    esp_fill_random(&generation, sizeof(generation));
    if (!generation) return;
    s_management_generation = generation;
    if (!remote_device_initialize()) ESP_LOGW(TAG, "Remote device controls unavailable");
    remote_media_init();
    void *input = heap_caps_aligned_calloc(app_remote_input_alignment(), 1,
        app_remote_input_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *bridge = heap_caps_aligned_calloc(app_command_bridge_alignment(), 1,
        app_command_bridge_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    app_remote_input_port_t input_port = {.engine = &s_engine, .interaction = &s_interaction,
        .observe = remote_observe, .authorize = remote_authorize, .changed = remote_changed, .complete = remote_complete,
        .delegate_available = remote_delegate_available, .delegate = remote_delegate};
    app_command_bridge_port_t bridge_port = {.lock = command_lock, .unlock = command_unlock,
        .now_ms = command_clock, .available = command_available, .execute = command_execute,
        .safety = command_safety, .read = command_read, .complete = remote_complete};
    if (!input || !bridge || app_remote_input_init(input, app_remote_input_bytes(), &input_port, &s_remote_input) != CP_OK ||
        app_command_bridge_init(bridge, app_command_bridge_bytes(), &bridge_port, &s_command_bridge) != CP_OK) {
        heap_caps_free(input); heap_caps_free(bridge); s_remote_input = NULL; s_command_bridge = NULL; return;
    }
    app_control_host_t host = {.abi_version = APP_CONTROL_RUNTIME_ABI, .user = s_command_bridge,
        .available = app_command_bridge_available, .submit = app_command_bridge_submit,
        .safety = app_command_bridge_safety, .read = app_command_bridge_read,
        .update_authority = remote_update_authority};
    esp_err_t error = app_control_runtime_init(&host);
    if (error == ESP_OK && usb_available) error = app_control_runtime_attach_usb_stdio();
    if (error != ESP_OK) ESP_LOGW(TAG, "Remote control unavailable: %s", esp_err_to_name(error));
}
