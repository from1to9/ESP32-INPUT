// SPDX-License-Identifier: Apache-2.0
#include "app_remote_input.h"
#include <stdio.h>
#include <string.h>

#define RI_RECENT 16U
#define RI_SAFETY_WAITERS CP_MAX_RESULTS
#define RI_VIRTUAL_GESTURES 32U

typedef enum { RI_JOB_NONE, RI_JOB_ACQUIRE, RI_JOB_RENEW, RI_JOB_HID, RI_JOB_TEXT } ri_job_kind_t;
typedef struct {
    ri_job_kind_t kind;
    cp_operation_id_t operation;
    cp_identity_t identity;
    cp_result_t result;
    uint64_t epoch, transport_generation, ui_revision, deadline_ms, proposed_expiry;
    char target_id[CP_TARGET_CAPACITY];
    bool publish, terminal, ending, preview;
    cp_result_state_t end_state;
    cp_error_t end_error;
    size_t total, batch, base_started, base_completed;
    uint64_t preview_token;
    char text[INTERACTION_PREVIEW_CAPACITY + 1U];
} ri_job_t;
typedef struct {
    bool used;
    cp_operation_id_t operation;
    cp_identity_t identity;
    uint64_t key, transport_generation;
    char target_id[CP_TARGET_CAPACITY];
} ri_safety_waiter_t;
typedef struct { cp_operation_id_t operation; cp_identity_t identity; cp_result_t result; } ri_recent_t;
struct app_remote_input {
    app_remote_input_port_t port;
    app_remote_input_view_t view;
    app_remote_input_state_t state;
    bool state_known, clock_seen, servicing;
    uint64_t now_ms, highest_key, lease_epoch, lease_transport;
    char lease_target[CP_TARGET_CAPACITY];
    uint64_t drag_started_ms, drag_expires_ms;
    bool drag_requested;
    bool virtual_active;
    uint64_t virtual_serial, virtual_epoch, virtual_ui, virtual_deadline, virtual_started;
    float virtual_x, virtual_y;
    uint8_t virtual_id[CP_ID_BYTES], gestures[RI_VIRTUAL_GESTURES][CP_ID_BYTES];
    size_t gesture_count;
    char virtual_surface[16];
    ri_job_t jobs[APP_REMOTE_INPUT_JOBS];
    ri_safety_waiter_t safety[RI_SAFETY_WAITERS];
    ri_recent_t recent[RI_RECENT];
    size_t recent_next;
};

static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static bool terminated(const char *s, size_t capacity)
{
    return memchr(s, 0, capacity) != NULL;
}

static bool nonzero_id(const uint8_t id[CP_ID_BYTES])
{
    for (size_t i = 0; i < CP_ID_BYTES; ++i) if (id[i]) return true;
    return false;
}

static bool identity_matches(const app_remote_input_t *m, const cp_identity_t *identity)
{
    return identity && !memcmp(m->view.owner_session, identity->session_id, CP_ID_BYTES) &&
        !memcmp(m->view.owner_principal, identity->principal_id, CP_ID_BYTES);
}

static bool same_identity(const cp_identity_t *a, const cp_identity_t *b)
{
    return !memcmp(a->session_id,b->session_id,CP_ID_BYTES) &&
        !memcmp(a->principal_id,b->principal_id,CP_ID_BYTES);
}

static const cp_operation_info_t *operation(cp_operation_id_t id)
{
    for (size_t i = 0; i < cp_operation_count(); ++i) {
        const cp_operation_info_t *o = cp_operation_at(i);
        if (o && o->id == id) return o;
    }
    return NULL;
}

static cp_result_t result_for(uint64_t key, cp_result_state_t state, cp_error_t error,
                               const char *reason)
{
    cp_result_t r = {.state = state, .error = error, .command_key = key,
        .job_id = key, .uncertain_index = -1};
    if (reason) (void)snprintf(r.reason, sizeof r.reason, "%s", reason);
    return r;
}

static bool observe(app_remote_input_t *m)
{
    app_remote_input_state_t state = {0};
    m->state_known = m->port.observe(m->port.user, &state) &&
        terminated(state.target_id, sizeof state.target_id) && state.transport_generation &&
        state.ui_revision && state.draft_revision && state.selection_revision;
    if (m->state_known) m->state = state;
    return m->state_known;
}

static bool ready(const app_remote_input_t *m)
{
    const hid_engine_t *e = m->port.engine;
    return e->state.connected && e->ready && !e->congested && !e->state.locked &&
        !e->state.release_pending && m->state_known && m->state.target_id[0] && !m->state.transition_busy;
}

static void sync_model(app_remote_input_t *m)
{
    hid_engine_t *e = m->port.engine;
    interaction_sync(m->port.interaction, &e->state,
        e->ready && !e->congested && !e->state.release_pending, m->now_ms);
}

static void changed(app_remote_input_t *m, unsigned flags)
{
    m->port.changed(m->port.user, flags);
    (void)observe(m);
}

static void remember(app_remote_input_t *m, cp_operation_id_t op, const cp_identity_t *identity, const cp_result_t *r)
{
    m->recent[m->recent_next] = (ri_recent_t){.operation = op, .identity = *identity, .result = *r};
    m->recent_next = (m->recent_next + 1) % RI_RECENT;
}

static ri_job_t *new_job(app_remote_input_t *m, const cp_command_t *cmd, ri_job_kind_t kind)
{
    for (size_t i = 0; i < APP_REMOTE_INPUT_JOBS; ++i) {
        ri_job_t *j = &m->jobs[i];
        if (j->kind != RI_JOB_NONE) continue;
        *j = (ri_job_t){.kind = kind, .operation = cmd->operation,
            .identity = cmd->identity,
            .result = result_for(cmd->command_key, CP_RESULT_ACCEPTED, CP_OK, "queued"),
            .epoch = m->port.engine->state.epoch, .transport_generation = m->state.transport_generation,
            .ui_revision = m->state.ui_revision, .deadline_ms = cmd->deadline_ms};
        memcpy(j->target_id, m->state.target_id, sizeof j->target_id);
        return j;
    }
    return NULL;
}

static bool hid_busy(const app_remote_input_t *m)
{
    for (size_t i = 0; i < APP_REMOTE_INPUT_JOBS; ++i)
        if (!m->jobs[i].terminal && (m->jobs[i].kind == RI_JOB_HID || m->jobs[i].kind == RI_JOB_TEXT))
            return true;
    return false;
}

static cp_error_t enqueue_error(hid_engine_enqueue_result_t r)
{
    switch (r) {
    case HID_ENGINE_ENQUEUE_OK: return CP_OK;
    case HID_ENGINE_ENQUEUE_INVALID: return CP_INVALID;
    case HID_ENGINE_ENQUEUE_STALE: return CP_STALE;
    case HID_ENGINE_ENQUEUE_FULL: return CP_BUSY;
    default: return CP_RELEASE_PENDING;
    }
}

static uint64_t minimum(uint64_t a, uint64_t b) { return a < b ? a : b; }

static uint64_t deadline(uint64_t now, uint64_t duration)
{
    return duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
}

static bool u64_argument(const cp_command_t *cmd, const char *name, uint64_t *out)
{
    char digits[21];
    if (!cp_command_string(cmd, name, digits, sizeof digits) || !digits[0]) return false;
    uint64_t value = 0;
    for (size_t i = 0; digits[i]; ++i) {
        if (digits[i] < '0' || digits[i] > '9' || value > (UINT64_MAX - (unsigned)(digits[i] - '0')) / 10)
            return false;
        value = value * 10 + (unsigned)(digits[i] - '0');
    }
    *out = value;
    return true;
}

static bool revisions_available(const app_remote_input_t *m)
{
    return m->state.ui_revision < UINT64_MAX && m->state.draft_revision < UINT64_MAX &&
        m->state.selection_revision < UINT64_MAX;
}

static void sample_text(app_remote_input_t *m, ri_job_t *j)
{
    hid_engine_t *e = m->port.engine;
    if (j->kind != RI_JOB_TEXT || j->ending || !j->batch || e->state.owner != INPUT_REMOTE) return;
    if (e->text_accepted > j->batch || e->text_completed > e->text_accepted) return;
    const size_t started = j->base_started + e->text_accepted;
    const size_t completed = j->base_completed + e->text_completed;
    if (started > j->total || completed > started) return;
    if (started != j->result.started_prefix || completed != j->result.completed_pairs) {
        j->result.started_prefix = (uint32_t)started;
        j->result.completed_pairs = (uint32_t)completed;
        j->result.uncertain_index = started > completed ? (int32_t)completed : -1;
        j->result.state = CP_RESULT_RUNNING; j->publish = true;
    }
}

static void end_job(app_remote_input_t *m, ri_job_t *j, cp_result_state_t state, cp_error_t error)
{
    if (j->kind == RI_JOB_NONE || j->terminal || j->ending) return;
    sample_text(m, j);
    j->ending = true; j->end_state = state; j->end_error = error;
    j->result.state = CP_RESULT_CLEANUP_PENDING; j->result.error = error;
    (void)snprintf(j->result.reason, sizeof j->result.reason, "%s", "neutral_release_pending");
    j->publish = true;
    if (j->preview && j->preview_token)
        (void)interaction_send_update(m->port.interaction, j->preview_token, j->result.started_prefix, false);
    secure_zero(j->text, sizeof j->text);
}

static void revoke(app_remote_input_t *m, cp_error_t error)
{
    const bool owned = m->view.active || m->view.pending;
    for (size_t i = 0; i < APP_REMOTE_INPUT_JOBS; ++i)
        end_job(m, &m->jobs[i], CP_RESULT_CANCELLED, error);
    m->view.active = false; m->view.pending = false; m->drag_requested = false;
    m->view.drag_active = false; m->view.virtual_touch_active = false;
    m->virtual_active = false;
    if (m->port.interaction->touch.source == INPUT_REMOTE)
        (void)interaction_touch_cancel(m->port.interaction);
    if (owned && m->port.engine->state.owner == INPUT_REMOTE)
        (void)hid_engine_set_owner(m->port.engine, INPUT_LOCAL);
    sync_model(m);
    const interaction_touch_result_t touch = m->port.interaction->touch.pending;
    if (touch.effect == INTERACTION_TOUCH_RELEASE_REQUIRED && m->port.engine->state.release_pending)
        (void)interaction_touch_admit(m->port.interaction, touch.token, true);
    if (owned) changed(m, APP_REMOTE_CHANGED_OWNER | APP_REMOTE_CHANGED_INPUT | APP_REMOTE_CHANGED_UI);
}

static cp_error_t same_target(const app_remote_input_t *m, const cp_command_t *cmd)
{
    return cmd->input_epoch == m->port.engine->state.epoch &&
        cmd->transport_generation == m->state.transport_generation &&
        terminated(cmd->target_id, sizeof cmd->target_id) && !strcmp(cmd->target_id, m->state.target_id)
        ? CP_OK : CP_STALE;
}

static cp_error_t lease_gate(const app_remote_input_t *m, const cp_command_t *cmd)
{
    const cp_operation_info_t *op = operation(cmd->operation);
    if (!m->view.active || !identity_matches(m, &cmd->identity) || !op ||
        (!op->safety && memcmp(cmd->lease_id, m->view.lease_id, CP_ID_BYTES))) return CP_FORBIDDEN;
    if (m->now_ms >= m->view.lease_expires_ms && !op->safety) return CP_EXPIRED;
    if (m->port.engine->state.owner != INPUT_REMOTE) return CP_OWNER_BUSY;
    if (op->safety) return CP_OK;
    return same_target(m, cmd);
}

size_t app_remote_input_bytes(void) { return sizeof(app_remote_input_t); }

cp_error_t app_remote_input_init(void *memory, size_t bytes, const app_remote_input_port_t *port,
                                app_remote_input_t **out)
{
    if (!memory || bytes < sizeof(app_remote_input_t) || !port || !out || !port->engine ||
        !port->interaction || !port->observe || !port->changed || !port->complete ||
        (uintptr_t)memory % _Alignof(app_remote_input_t)) return CP_INVALID;
    app_remote_input_t *m = memory;
    *m = (app_remote_input_t){.port = *port};
    *out = m;
    return CP_OK;
}

static bool delegated(cp_operation_id_t id)
{
    return id == CP_OP_MODE_SELECT || id == CP_OP_INPUT_MOTION_SET || id == CP_OP_INPUT_TRANSPORT_SELECT ||
        id == CP_OP_INPUT_RECOVER || id == CP_OP_KEYBOARD_SUBMODE_SELECT ||
        id == CP_OP_UI_NAVIGATE || id == CP_OP_UI_CLOSE || id == CP_OP_UI_PRIMARY_ACTIVATE ||
        id == CP_OP_UI_MODE_PANEL_OPEN;
}

bool app_remote_input_owns(cp_operation_id_t id)
{
    if (delegated(id)) return true;
    switch (id) {
    case CP_OP_CONTROL_ACQUIRE: case CP_OP_CONTROL_RENEW: case CP_OP_CONTROL_RELEASE:
    case CP_OP_INPUT_RELEASE_ALL: case CP_OP_INPUT_CANCEL:
    case CP_OP_MOUSE_MOVE_RELATIVE: case CP_OP_MOUSE_CLICK: case CP_OP_MOUSE_SCROLL:
    case CP_OP_MOUSE_DRAG_BEGIN: case CP_OP_MOUSE_DRAG_REFRESH: case CP_OP_MOUSE_DRAG_END:
    case CP_OP_KEYBOARD_TEXT_ENQUEUE: case CP_OP_KEYBOARD_KEY_TAP: case CP_OP_KEYBOARD_SHORTCUT:
    case CP_OP_SELECTION_FOCUS_GET: case CP_OP_SELECTION_BACK: case CP_OP_SELECTION_CONFIRM:
    case CP_OP_SELECTION_ACTIVATE: case CP_OP_SELECTION_FOCUS_SET: case CP_OP_SELECTION_CASE_SET:
    case CP_OP_SELECTION_PAGE_SET: case CP_OP_PREVIEW_CLEAR: case CP_OP_PREVIEW_DELETE:
    case CP_OP_PREVIEW_APPEND: case CP_OP_PREVIEW_SEND:
    case CP_OP_TOUCH_BEGIN: case CP_OP_TOUCH_MOVE: case CP_OP_TOUCH_END: case CP_OP_TOUCH_CANCEL:
        return true;
    default: return false;
    }
}

bool app_remote_input_available(app_remote_input_t *m, cp_operation_id_t id, char reason[CP_REASON_CAPACITY])
{
    if (!m || !reason) return false;
    reason[0] = 0;
    if (!app_remote_input_owns(id)) {
        (void)snprintf(reason, CP_REASON_CAPACITY, "%s", "input_operation_not_bound");
        return false;
    }
    if (delegated(id)) return m->port.delegate && m->port.delegate_available &&
        m->port.delegate_available(m->port.user, id, reason);
    return true;
}

void app_remote_input_snapshot(const app_remote_input_t *m, app_remote_input_view_t *view)
{
    if (!m || !view) return;
    *view = m->view; view->pending_jobs = 0; view->input_epoch = m->port.engine->state.epoch;
    view->drag_active = m->port.engine->mouse_buttons == 1;
    for (size_t i = 0; i < APP_REMOTE_INPUT_JOBS; ++i)
        if (m->jobs[i].kind != RI_JOB_NONE) ++view->pending_jobs;
    for (size_t i = 0; i < RI_SAFETY_WAITERS; ++i) if (m->safety[i].used) ++view->pending_jobs;
}

void app_remote_input_revoke(app_remote_input_t *m, cp_safety_reason_t reason,
                              const cp_identity_t *identity, uint64_t now_ms)
{
    if (!m || (identity && !identity_matches(m, identity))) return;
    m->now_ms = now_ms;
    (void)observe(m);
    revoke(m, reason == CP_SAFETY_LEASE_EXPIRED ? CP_EXPIRED :
        reason == CP_SAFETY_LOCKED ? CP_LOCKED : CP_STALE);
}

static bool target_changed(const app_remote_input_t *m, uint64_t transport, const char *target)
{
    return !m->state_known || transport != m->state.transport_generation || strcmp(target, m->state.target_id);
}

void app_remote_input_service(app_remote_input_t *m, uint64_t now_ms)
{
    if (!m || m->servicing) return;
    m->servicing = true;
    const bool regressed = m->clock_seen && now_ms < m->now_ms;
    m->clock_seen = true; m->now_ms = now_ms;
    const bool known = observe(m);
    for (size_t i = 0; i < APP_REMOTE_INPUT_JOBS; ++i) sample_text(m, &m->jobs[i]);
    if ((m->view.active || m->view.pending) &&
        (regressed || !known || now_ms >= m->view.lease_expires_ms || m->port.engine->state.locked ||
         !m->state.remote_allowed || m->state.local_busy || target_changed(m, m->lease_transport, m->lease_target) ||
         m->port.engine->state.owner != INPUT_REMOTE || m->port.engine->state.epoch != m->lease_epoch))
        revoke(m, now_ms >= m->view.lease_expires_ms ? CP_EXPIRED :
            m->port.engine->state.locked ? CP_LOCKED : CP_STALE);
    if (m->drag_requested && (now_ms >= m->drag_expires_ms ||
        now_ms < m->drag_started_ms || now_ms - m->drag_started_ms >= APP_REMOTE_INPUT_DRAG_MAX_MS))
        revoke(m, CP_EXPIRED);
    if (m->virtual_active && (now_ms >= m->virtual_deadline || now_ms < m->virtual_started ||
        now_ms - m->virtual_started >= APP_REMOTE_INPUT_DRAG_MAX_MS ||
        m->virtual_epoch != m->port.engine->state.epoch || !known || m->virtual_ui != m->state.ui_revision))
        revoke(m, CP_EXPIRED);
    for (size_t i = 0; i < APP_REMOTE_INPUT_JOBS; ++i) {
        ri_job_t *j = &m->jobs[i];
        if (j->kind == RI_JOB_NONE) continue;
        if (!j->terminal && !j->ending && j->preview && (!known || !revisions_available(m) ||
            j->ui_revision != m->state.ui_revision || !m->state.screen_awake || m->state.overlay_open)) {
            revoke(m,CP_STALE);
        }
        if (!j->terminal && !j->ending && (j->operation == CP_OP_MOUSE_CLICK ||
            j->operation == CP_OP_MOUSE_DRAG_BEGIN || j->operation == CP_OP_MOUSE_DRAG_END))
            interaction_freeze_after(m->port.interaction, now_ms);
        if (!j->terminal && !j->ending && now_ms >= j->deadline_ms) {
            end_job(m, j, CP_RESULT_CANCELLED, CP_EXPIRED);
            if (m->view.active || m->view.pending) revoke(m, CP_EXPIRED);
        }
        if (!j->terminal && j->ending &&
            (!m->port.engine->state.release_pending || target_changed(m, j->transport_generation, j->target_id))) {
            j->terminal = true; j->result.state = j->end_state; j->result.error = j->end_error;
            (void)snprintf(j->result.reason, sizeof j->result.reason, "%s",
                target_changed(m, j->transport_generation, j->target_id) ? "old_target_release_unverified" : "cancelled");
            j->publish = true;
        }
        if (!j->terminal && !j->ending && (j->kind == RI_JOB_ACQUIRE || j->kind == RI_JOB_RENEW) && ready(m)) {
            m->view.pending = false; m->view.active = true;
            if (j->kind == RI_JOB_RENEW) m->view.lease_expires_ms = j->proposed_expiry;
            j->terminal = true; j->result.state = CP_RESULT_APPLIED; j->publish = true;
            (void)snprintf(j->result.reason, sizeof j->result.reason, "%s", "lease_applied_after_neutral_release");
            if (j->kind == RI_JOB_ACQUIRE) changed(m, APP_REMOTE_CHANGED_OWNER);
        }
        if (!j->terminal && !j->ending && j->kind == RI_JOB_HID &&
            m->port.engine->state.epoch == j->epoch && ready(m) && !m->port.engine->count) {
            j->terminal = true; j->result.state = CP_RESULT_SENT; j->publish = true;
            (void)snprintf(j->result.reason, sizeof j->result.reason, "%s", "transport_accepted_reports");
        }
        if (!j->terminal && !j->ending && j->kind == RI_JOB_TEXT && ready(m) &&
            m->port.engine->state.epoch == j->epoch) {
            if (j->preview && j->preview_token) {
                const size_t before = m->port.interaction->view.preview_length;
                (void)interaction_send_update(m->port.interaction, j->preview_token, j->result.started_prefix, true);
                if (before != m->port.interaction->view.preview_length) {
                    changed(m, APP_REMOTE_CHANGED_DRAFT | APP_REMOTE_CHANGED_UI);
                    j->ui_revision = m->state.ui_revision;
                }
            }
            if (!m->port.engine->text_pending && j->result.completed_pairs == j->total) {
                if (j->preview) (void)interaction_send_update(m->port.interaction,
                    j->preview_token, j->result.started_prefix, false);
                j->terminal = true; j->result.state = CP_RESULT_SENT; j->result.error = CP_OK;
                j->result.uncertain_index = -1; j->publish = true;
                secure_zero(j->text, sizeof j->text);
                (void)snprintf(j->result.reason, sizeof j->result.reason, "%s", "all_key_pairs_transport_accepted");
            } else if (!m->port.engine->text_pending && j->result.started_prefix == j->base_started + j->batch) {
                j->base_started += j->batch; j->base_completed += j->batch;
                const size_t left = j->total - j->base_started;
                j->batch = left > HID_ENGINE_TEXT_MAX_LENGTH ? HID_ENGINE_TEXT_MAX_LENGTH : left;
                const hid_engine_enqueue_result_t admitted = hid_engine_enqueue_text(m->port.engine,
                    j->epoch, INPUT_REMOTE, j->text + j->base_started, j->batch);
                if (admitted != HID_ENGINE_ENQUEUE_OK) revoke(m, enqueue_error(admitted));
            }
        }
        if (j->publish && m->port.complete(m->port.user, &j->result)) {
            j->publish = false;
            if (j->terminal) { remember(m, j->operation, &j->identity, &j->result); secure_zero(j, sizeof *j); }
        }
    }
    for (size_t i = 0; i < RI_SAFETY_WAITERS; ++i) {
        ri_safety_waiter_t *w = &m->safety[i];
        if (!w->used) continue;
        const bool stale = target_changed(m, w->transport_generation, w->target_id);
        if (!stale && m->port.engine->state.release_pending) continue;
        cp_result_t r = result_for(w->key, stale ? CP_RESULT_FAILED : CP_RESULT_APPLIED,
            stale ? (known ? CP_STALE : CP_UNAVAILABLE) : CP_OK,
            stale ? "old_target_release_unverified" : "neutral_reports_accepted");
        if (m->port.complete(m->port.user, &r)) { remember(m, w->operation, &w->identity, &r); *w = (ri_safety_waiter_t){0}; }
    }
    m->servicing = false;
}

bool app_remote_input_motion_sample(app_remote_input_t *m, const interaction_imu_sample_t *sample, uint64_t now_ms)
{
    if (!m || !sample) return false;
    app_remote_input_service(m,now_ms);
    sync_model(m);
    input_owner_t source=INPUT_LOCAL;
    if (m->view.active && m->port.engine->state.owner == INPUT_REMOTE &&
        ((m->view.producer == APP_REMOTE_PRODUCER_DEVICE_IMU && m->port.engine->state.mode == INPUT_MOUSE) ||
         (m->view.producer == APP_REMOTE_PRODUCER_SELECTION && m->port.engine->state.mode == INPUT_KEYBOARD &&
          m->state.selection_visible && m->state.screen_awake && !m->state.overlay_open &&
          revisions_available(m)))) source=INPUT_REMOTE;
    const unsigned focus=m->port.interaction->view.focus;
    const input_delta_t delta=interaction_motion_sample_owned(m->port.interaction,source,sample);
    if (focus != m->port.interaction->view.focus && revisions_available(m))
        changed(m,APP_REMOTE_CHANGED_SELECTION | APP_REMOTE_CHANGED_UI);
    if (delta.x || delta.y)
        (void)hid_engine_enqueue_mouse_move(m->port.engine,m->port.engine->state.epoch,source,delta.x,delta.y);
    return true;
}

static cp_error_t remember_result(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *r,
                                  cp_result_state_t state, cp_error_t error, const char *reason)
{
    *r = result_for(cmd->command_key, state, error, reason);
    remember(m, cmd->operation, &cmd->identity, r);
    return error;
}

static cp_error_t safety_wait(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out, bool stop)
{
    ri_safety_waiter_t *waiter = NULL;
    for (size_t i = 0; i < RI_SAFETY_WAITERS; ++i)
        if (!m->safety[i].used) { waiter = &m->safety[i]; break; }
    if (!waiter) return CP_BUSY; /* Existing stop/release work already owns all reserved waiters. */
    *waiter = (ri_safety_waiter_t){.used = true, .key = cmd->command_key,
        .operation = cmd->operation, .identity = cmd->identity, .transport_generation = cmd->transport_generation};
    memcpy(waiter->target_id, cmd->target_id, sizeof waiter->target_id);
    if (!stop) {
        *out=result_for(cmd->command_key,CP_RESULT_CLEANUP_PENDING,CP_OK,"existing_neutral_release_pending");
        return CP_OK;
    }
    const bool remote = m->view.active || m->view.pending;
    revoke(m, CP_STALE);
    if (!remote) {
        if (m->port.engine->state.owner == INPUT_REMOTE)
            (void)hid_engine_set_owner(m->port.engine,INPUT_LOCAL);
        else hid_engine_cancel(m->port.engine);
        sync_model(m);
        changed(m, APP_REMOTE_CHANGED_OWNER | APP_REMOTE_CHANGED_INPUT | APP_REMOTE_CHANGED_UI);
    }
    *out = result_for(cmd->command_key, CP_RESULT_CLEANUP_PENDING, CP_OK, "neutral_release_pending");
    return CP_OK;
}

static cp_error_t safety_stop(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    /* Neutralization remains admissible after a deadline/provider loss, but an
     * old target/transport/epoch must never cancel new target work. A cached
     * target plus unchanged engine epoch is the owning continuity evidence. */
    if (!terminated(cmd->target_id,sizeof cmd->target_id) ||
        cmd->transport_generation != m->state.transport_generation || strcmp(cmd->target_id,m->state.target_id) ||
        !cmd->transport_generation || !cmd->target_id[0]) {
        *out=result_for(cmd->command_key,CP_RESULT_FAILED,m->state_known ? CP_STALE : CP_UNAVAILABLE,
            "old_target_release_unverified");
        return CP_OK;
    }
    if (cmd->input_epoch != m->port.engine->state.epoch) {
        if (m->port.engine->state.release_pending) return safety_wait(m,cmd,out,false);
        *out=result_for(cmd->command_key,CP_RESULT_FAILED,CP_STALE,"old_epoch_release_unverified");
        return CP_OK;
    }
    return safety_wait(m,cmd,out,true);
}

static bool expiry_valid(const app_remote_input_t *m, const cp_command_t *cmd)
{
    return cmd->lease_expires_ms > m->now_ms &&
        cmd->lease_expires_ms - m->now_ms <= APP_REMOTE_INPUT_LEASE_MAX_MS;
}

static cp_error_t acquire(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    uint64_t acknowledged;
    if (!u64_argument(cmd, "state_sequence", &acknowledged) || acknowledged != m->state.state_sequence)
        return CP_STALE;
    if (!expiry_valid(m, cmd) || !nonzero_id(cmd->lease_id)) return CP_INVALID;
    if (m->view.active || m->view.pending || m->port.engine->state.owner != INPUT_LOCAL) return CP_OWNER_BUSY;
    if (!m->state.remote_allowed) return CP_FORBIDDEN;
    if (m->state.local_busy || m->state.native_send_active || m->port.interaction->view.touch_active ||
        m->port.interaction->view.button_down || m->port.interaction->send_open ||
        m->port.engine->count || m->port.engine->mouse_buttons) return CP_LOCAL_BUSY;
    if (!ready(m)) return m->port.engine->state.release_pending ? CP_RELEASE_PENDING : CP_UNAVAILABLE;
    char producer[24] = "script";
    if (!cp_command_string(cmd, "producer", producer, sizeof producer))
        (void)snprintf(producer,sizeof producer,"%s","script");
    app_remote_input_producer_t selected;
    if (!strcmp(producer,"script")) selected = APP_REMOTE_PRODUCER_SCRIPT;
    else if (!strcmp(producer,"device_imu")) selected = APP_REMOTE_PRODUCER_DEVICE_IMU;
    else if (!strcmp(producer,"device_touch")) selected = APP_REMOTE_PRODUCER_DEVICE_TOUCH;
    else if (!strcmp(producer,"selection")) selected = APP_REMOTE_PRODUCER_SELECTION;
    else return CP_INVALID;
    if (selected == APP_REMOTE_PRODUCER_DEVICE_IMU && m->port.engine->state.mode != INPUT_MOUSE) return CP_STALE;
    if (selected == APP_REMOTE_PRODUCER_SELECTION &&
        (m->port.engine->state.mode != INPUT_KEYBOARD || !m->state.selection_visible ||
         !m->state.screen_awake || m->state.overlay_open)) return CP_STALE;
    if (selected == APP_REMOTE_PRODUCER_DEVICE_TOUCH && m->port.engine->state.mode != INPUT_MOUSE) {
        char reason[CP_REASON_CAPACITY];
        if (!m->port.delegate_available || !m->port.delegate_available(m->port.user,CP_OP_TOUCH_BEGIN,reason))
            return CP_UNAVAILABLE;
    }
    ri_job_t *j = new_job(m, cmd, RI_JOB_ACQUIRE);
    if (!j) return CP_BUSY;
    if (!hid_engine_set_owner(m->port.engine, INPUT_REMOTE)) { secure_zero(j,sizeof *j); return CP_UNAVAILABLE; }
    m->view.pending = true; m->view.active = false; m->view.producer = selected;
    m->view.lease_expires_ms = cmd->lease_expires_ms;
    memcpy(m->view.lease_id,cmd->lease_id,CP_ID_BYTES);
    memcpy(m->view.owner_session,cmd->identity.session_id,CP_ID_BYTES);
    memcpy(m->view.owner_principal,cmd->identity.principal_id,CP_ID_BYTES);
    m->lease_epoch = m->port.engine->state.epoch; j->epoch = m->lease_epoch;
    m->lease_transport = m->state.transport_generation;
    memcpy(m->lease_target,m->state.target_id,sizeof m->lease_target);
    m->gesture_count = 0; m->virtual_active = false; m->drag_requested = false;
    sync_model(m); changed(m,APP_REMOTE_CHANGED_OWNER | APP_REMOTE_CHANGED_INPUT | APP_REMOTE_CHANGED_UI);
    *out = j->result;
    return CP_OK;
}

static cp_error_t renew(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    uint64_t acknowledged;
    if (!u64_argument(cmd,"state_sequence",&acknowledged) || acknowledged != m->state.state_sequence)
        return CP_STALE;
    if (!expiry_valid(m,cmd)) return CP_INVALID;
    for (size_t i=0;i<APP_REMOTE_INPUT_JOBS;++i)
        if (m->jobs[i].kind == RI_JOB_RENEW && !m->jobs[i].terminal) return CP_BUSY;
    ri_job_t *j = new_job(m,cmd,RI_JOB_RENEW);
    if (!j) return CP_BUSY;
    j->proposed_expiry = cmd->lease_expires_ms; *out = j->result;
    return CP_OK;
}

static cp_error_t begin_text(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out, bool preview)
{
    if (hid_busy(m) || m->port.engine->count || m->state.native_send_active || m->port.interaction->send_open)
        return CP_BUSY;
    char text[INTERACTION_PREVIEW_CAPACITY + 1U] = {0};
    size_t length;
    if (preview) {
        uint64_t revision;
        if (!u64_argument(cmd,"draft_revision",&revision) || revision != m->state.draft_revision) return CP_STALE;
        length = m->port.interaction->view.preview_length;
        if (!length) return CP_INVALID;
        memcpy(text,m->port.interaction->view.preview,length);
    } else {
        if (!cp_command_string(cmd,"text",text,sizeof text)) return CP_INVALID;
        length = strlen(text);
        if (!length || length > HID_ENGINE_TEXT_MAX_LENGTH) { secure_zero(text,sizeof text); return CP_INVALID; }
    }
    ri_job_t *j = new_job(m,cmd,RI_JOB_TEXT);
    if (!j) { secure_zero(text,sizeof text); return CP_BUSY; }
    j->preview = preview; j->total = length; j->result.progress_known = true;
    memcpy(j->text,text,length); secure_zero(text,sizeof text);
    if (preview) {
        j->preview_token = interaction_send_begin_owned(m->port.interaction,INPUT_REMOTE);
        if (!j->preview_token) { secure_zero(j,sizeof *j); return CP_BUSY; }
    }
    j->batch = length > HID_ENGINE_TEXT_MAX_LENGTH ? HID_ENGINE_TEXT_MAX_LENGTH : length;
    const cp_error_t admitted = enqueue_error(hid_engine_enqueue_text(m->port.engine,
        m->port.engine->state.epoch,INPUT_REMOTE,j->text,j->batch));
    if (admitted != CP_OK) {
        if (preview) (void)interaction_send_update(m->port.interaction,j->preview_token,0,false);
        secure_zero(j,sizeof *j); return admitted;
    }
    *out = j->result;
    return CP_OK;
}

static cp_error_t simple_hid(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    if (hid_busy(m) || m->port.engine->count || m->state.native_send_active) return CP_BUSY;
    ri_job_t *j = new_job(m,cmd,RI_JOB_HID);
    if (!j) return CP_BUSY;
    hid_engine_enqueue_result_t admitted = HID_ENGINE_ENQUEUE_INVALID;
    int64_t a=0,b=0; char name[32];
    const uint64_t epoch = m->port.engine->state.epoch;
    switch (cmd->operation) {
    case CP_OP_MOUSE_MOVE_RELATIVE:
        if (cp_command_integer(cmd,"dx",&a) && cp_command_integer(cmd,"dy",&b) &&
            a >= -127 && a <= 127 && b >= -127 && b <= 127)
            admitted = hid_engine_enqueue_mouse_move(m->port.engine,epoch,INPUT_REMOTE,(int)a,(int)b);
        break;
    case CP_OP_MOUSE_SCROLL:
        if (cp_command_integer(cmd,"horizontal",&b) && b) { secure_zero(j,sizeof *j); return CP_NOT_IMPLEMENTED; }
        if (cp_command_integer(cmd,"wheel",&a) && a >= -127 && a <= 127)
            admitted = hid_engine_enqueue_mouse_scroll(m->port.engine,epoch,INPUT_REMOTE,(int)a);
        break;
    case CP_OP_MOUSE_CLICK:
        if (cp_command_string(cmd,"button",name,sizeof name)) {
            const unsigned button = !strcmp(name,"left") ? 1U : !strcmp(name,"right") ? 2U :
                !strcmp(name,"middle") ? 4U : 0U;
            admitted = hid_engine_enqueue_mouse_click(m->port.engine,epoch,INPUT_REMOTE,button);
        }
        break;
    case CP_OP_KEYBOARD_KEY_TAP:
        (void)cp_command_integer(cmd,"modifiers",&b);
        if (cp_command_integer(cmd,"usage",&a) && a >= 4 && a <= HID_ENGINE_KEY_USAGE_MAX && b >= 0 && b <= 255)
            admitted = hid_engine_enqueue_key(m->port.engine,epoch,INPUT_REMOTE,(unsigned)b,(unsigned)a);
        break;
    case CP_OP_KEYBOARD_SHORTCUT:
        if (cp_command_string(cmd,"shortcut",name,sizeof name)) {
            unsigned usage=0,modifier=0;
            if (!strcmp(name,"copy")) { usage=6; modifier=m->state.shortcut_modifier; }
            if (!strcmp(name,"paste")) { usage=25; modifier=m->state.shortcut_modifier; }
            if (!strcmp(name,"undo")) { usage=29; modifier=m->state.shortcut_modifier; }
            if (!strcmp(name,"tab")) usage=43;
            if (!strcmp(name,"escape")) usage=41;
            if (!strcmp(name,"enter")) usage=40;
            if ((usage == 6 || usage == 25 || usage == 29) && modifier != 1 && modifier != 8) {
                secure_zero(j,sizeof *j); return CP_UNAVAILABLE;
            }
            if (usage && (modifier == 0 || modifier == 1 || modifier == 8))
                admitted = hid_engine_enqueue_key(m->port.engine,epoch,INPUT_REMOTE,modifier,usage);
        }
        break;
    default: break;
    }
    const cp_error_t error = enqueue_error(admitted);
    if (error != CP_OK) { secure_zero(j,sizeof *j); return error; }
    if (cmd->operation == CP_OP_MOUSE_CLICK) interaction_freeze_after(m->port.interaction,m->now_ms);
    *out = j->result;
    return CP_OK;
}

static cp_error_t edit_model(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    if (m->state.native_send_active || m->port.interaction->send_open) return CP_BUSY;
    if (!revisions_available(m)) return CP_UNAVAILABLE;
    interaction_edit_t edit = {0};
    int64_t cell=0,count=0; uint64_t revision; char text[65] = {0}, value[24];
    switch (cmd->operation) {
    case CP_OP_SELECTION_BACK: edit.kind=INTERACTION_EDIT_BACK; break;
    case CP_OP_SELECTION_CONFIRM: edit.kind=INTERACTION_EDIT_CONFIRM; break;
    case CP_OP_SELECTION_ACTIVATE: case CP_OP_SELECTION_FOCUS_SET:
        if (!cp_command_integer(cmd,"cell",&cell) || cell < 0 || cell >= INTERACTION_GRID_CELLS ||
            !u64_argument(cmd,"selection_revision",&revision) || revision != m->state.selection_revision)
            return CP_STALE;
        edit.kind = cmd->operation == CP_OP_SELECTION_ACTIVATE ? INTERACTION_EDIT_ACTIVATE : INTERACTION_EDIT_FOCUS;
        edit.cell=(unsigned)cell; break;
    case CP_OP_SELECTION_CASE_SET:
        if (!cp_command_string(cmd,"case",value,sizeof value)) return CP_INVALID;
        if (strcmp(value,"lower") && strcmp(value,"upper")) return CP_INVALID;
        edit.kind=INTERACTION_EDIT_CASE; edit.lowercase=!strcmp(value,"lower"); break;
    case CP_OP_SELECTION_PAGE_SET:
        if (!cp_command_string(cmd,"page",value,sizeof value)) return CP_INVALID;
        if (strcmp(value,"letters") && strcmp(value,"symbols")) return CP_INVALID;
        edit.kind=INTERACTION_EDIT_PAGE;
        edit.page=!strcmp(value,"letters") ? INTERACTION_LETTERS : INTERACTION_SYMBOLS; break;
    case CP_OP_PREVIEW_CLEAR: edit.kind=INTERACTION_EDIT_CLEAR; break;
    case CP_OP_PREVIEW_DELETE:
        if (!cp_command_integer(cmd,"count",&count) || count < 1 || count > INTERACTION_PREVIEW_CAPACITY)
            return CP_INVALID;
        edit.kind=INTERACTION_EDIT_DELETE; edit.length=(size_t)count; break;
    case CP_OP_PREVIEW_APPEND:
        if (!cp_command_string(cmd,"text",text,sizeof text)) return CP_INVALID;
        edit.kind=INTERACTION_EDIT_APPEND; edit.text=text; edit.length=strlen(text); break;
    default: return CP_NOT_IMPLEMENTED;
    }
    if (cmd->operation == CP_OP_PREVIEW_DELETE ||
        cmd->operation == CP_OP_PREVIEW_APPEND) {
        if (!u64_argument(cmd,"draft_revision",&revision) || revision != m->state.draft_revision) {
            secure_zero(text,sizeof text); return CP_STALE;
        }
    }
    const interaction_view_t before = m->port.interaction->view;
    const bool applied = interaction_edit_owned(m->port.interaction,INPUT_REMOTE,&edit);
    secure_zero(text,sizeof text);
    if (!applied) return CP_INVALID;
    const interaction_view_t *after = &m->port.interaction->view;
    unsigned flags=0;
    if (before.preview_length != after->preview_length || strcmp(before.preview,after->preview))
        flags |= APP_REMOTE_CHANGED_DRAFT;
    if (before.page != after->page || before.group != after->group || before.focus != after->focus ||
        before.lowercase != after->lowercase) flags |= APP_REMOTE_CHANGED_SELECTION;
    if (flags) changed(m,flags | APP_REMOTE_CHANGED_UI);
    *out = result_for(cmd->command_key,CP_RESULT_APPLIED,CP_OK,"shared_model_applied");
    out->resource_revision = flags & APP_REMOTE_CHANGED_DRAFT ? m->state.draft_revision : m->state.selection_revision;
    return CP_OK;
}

static cp_error_t drag_command(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    char button[16]; int64_t deadman=1500;
    if (!cp_command_string(cmd,"button",button,sizeof button) || strcmp(button,"left")) return CP_INVALID;
    (void)cp_command_integer(cmd,"deadman_ms",&deadman);
    if (deadman < 100 || deadman > 1500) return CP_INVALID;
    if (cmd->operation == CP_OP_MOUSE_DRAG_REFRESH) {
        if (!m->drag_requested || m->now_ms >= m->drag_expires_ms) return CP_STALE;
        m->drag_expires_ms = minimum(deadline(m->now_ms,(uint64_t)deadman),m->view.lease_expires_ms);
        *out = result_for(cmd->command_key,CP_RESULT_APPLIED,CP_OK,"drag_deadman_updated"); return CP_OK;
    }
    const bool begin = cmd->operation == CP_OP_MOUSE_DRAG_BEGIN;
    if (begin && (m->drag_requested || hid_busy(m) || m->port.engine->count || interaction_touch_holding(m->port.interaction)))
        return CP_BUSY;
    if (!begin && !m->drag_requested) {
        if (interaction_touch_holding(m->port.interaction) || m->port.engine->mouse_buttons) return CP_STALE;
        *out = result_for(cmd->command_key,CP_RESULT_APPLIED,CP_OK,"drag_already_released"); return CP_OK;
    }
    ri_job_t *j = new_job(m,cmd,RI_JOB_HID);
    if (!j) return begin ? CP_BUSY : safety_stop(m,cmd,out);
    const cp_error_t admitted = enqueue_error(hid_engine_enqueue_mouse_buttons(m->port.engine,
        m->port.engine->state.epoch,INPUT_REMOTE,begin ? 1U : 0U));
    if (admitted != CP_OK) {
        secure_zero(j,sizeof *j);
        return begin ? admitted : safety_stop(m,cmd,out);
    }
    m->drag_requested = begin;
    if (begin) {
        m->drag_started_ms = m->now_ms;
        m->drag_expires_ms = minimum(deadline(m->now_ms,(uint64_t)deadman),m->view.lease_expires_ms);
    }
    interaction_freeze_after(m->port.interaction,m->now_ms);
    changed(m,APP_REMOTE_CHANGED_INPUT); *out = j->result;
    return CP_OK;
}

static bool gesture_id(const cp_command_t *cmd, uint8_t id[CP_ID_BYTES])
{
    char text[CP_ID_HEX_CAPACITY];
    if (!cp_command_string(cmd,"gesture_id",text,sizeof text) || strlen(text) != 32) return false;
    for (size_t i=0;i<CP_ID_BYTES;++i) {
        unsigned value=0;
        for (size_t n=0;n<2;++n) {
            const unsigned char c=(unsigned char)text[i*2+n];
            const unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c-'0') :
                c >= 'a' && c <= 'f' ? (unsigned)(c-'a')+10U :
                c >= 'A' && c <= 'F' ? (unsigned)(c-'A')+10U : 16U;
            if (digit > 15) return false;
            value=value*16+digit;
        }
        id[i]=(uint8_t)value;
    }
    return nonzero_id(id);
}

static cp_error_t touch_effect(app_remote_input_t *m, const cp_command_t *cmd,
                               interaction_touch_result_t effect, ri_job_t *j, cp_result_t *out)
{
    if (effect.effect == INTERACTION_TOUCH_NONE) return CP_OK;
    hid_engine_enqueue_result_t admitted = HID_ENGINE_ENQUEUE_FULL;
    if (j) {
        const uint64_t epoch=m->port.engine->state.epoch;
        if (effect.effect == INTERACTION_TOUCH_CLICK)
            admitted=hid_engine_enqueue_mouse_click(m->port.engine,epoch,INPUT_REMOTE,1);
        else if (effect.effect == INTERACTION_TOUCH_LEFT_DOWN || effect.effect == INTERACTION_TOUCH_LEFT_UP)
            admitted=hid_engine_enqueue_mouse_buttons(m->port.engine,epoch,INPUT_REMOTE,
                effect.effect == INTERACTION_TOUCH_LEFT_DOWN ? 1U : 0U);
    }
    const interaction_touch_result_t next = interaction_touch_admit(m->port.interaction,effect.token,
        admitted == HID_ENGINE_ENQUEUE_OK);
    if (next.effect == INTERACTION_TOUCH_RELEASE_REQUIRED || effect.effect == INTERACTION_TOUCH_RELEASE_REQUIRED) {
        if (j) secure_zero(j,sizeof *j);
        return safety_stop(m,cmd,out);
    }
    return enqueue_error(admitted);
}

static cp_error_t virtual_touch(app_remote_input_t *m, const cp_command_t *cmd, cp_result_t *out)
{
    uint8_t id[CP_ID_BYTES]; char surface[16] = {0};
    if (!gesture_id(cmd,id)) return CP_INVALID;
    const bool begin=cmd->operation == CP_OP_TOUCH_BEGIN;
    const bool finish=cmd->operation == CP_OP_TOUCH_END || cmd->operation == CP_OP_TOUCH_CANCEL;
    if (begin) {
        if (m->virtual_active) return CP_BUSY;
        for (size_t i=0;i<m->gesture_count;++i) if (!memcmp(m->gestures[i],id,sizeof id)) return CP_STALE;
        if (m->gesture_count == RI_VIRTUAL_GESTURES || m->virtual_serial == UINT64_MAX) return CP_BUSY;
        (void)snprintf(surface,sizeof surface,"%s","mouse_pad");
    } else {
        if (!m->virtual_active || memcmp(id,m->virtual_id,sizeof id)) return CP_STALE;
        memcpy(surface,m->virtual_surface,sizeof surface);
    }
    char specified[16] = {0};
    if (cp_command_string(cmd,"surface",specified,sizeof specified)) {
        if (!begin && strcmp(specified,surface)) return CP_STALE;
        memcpy(surface,specified,sizeof surface);
    }
    const bool pad=!strcmp(surface,"mouse_pad");
    if (pad && m->port.engine->state.mode != INPUT_MOUSE) return CP_STALE;
    if (!pad && strcmp(surface,"keyboard_grid") && strcmp(surface,"ui")) return CP_INVALID;
    if (!strcmp(surface,"keyboard_grid") && m->port.engine->state.mode != INPUT_KEYBOARD) return CP_STALE;
    if (!begin && (m->virtual_epoch != m->port.engine->state.epoch || m->virtual_ui != m->state.ui_revision))
        return CP_STALE;
    if (!finish && (hid_busy(m) || m->port.engine->count)) return CP_BUSY;
    int64_t deadman=1000,x=0,y=0,dx=0,dy=0;
    (void)cp_command_integer(cmd,"deadman_ms",&deadman);
    if (deadman < 100 || deadman > 1000) return CP_INVALID;
    const bool have_x=cp_command_integer(cmd,"x",&x), have_y=cp_command_integer(cmd,"y",&y);
    const bool have_dx=cp_command_integer(cmd,"dx",&dx), have_dy=cp_command_integer(cmd,"dy",&dy);
    if ((have_x && (x < 0 || x > 479)) || (have_y && (y < 0 || y > 479)) ||
        dx < -479 || dx > 479 || dy < -479 || dy > 479) return CP_INVALID;
    const float old_x=begin ? (have_x ? (float)x : 0) : m->virtual_x;
    const float old_y=begin ? (have_y ? (float)y : 0) : m->virtual_y;
    float next_x=have_x ? (float)x : old_x;
    float next_y=have_y ? (float)y : old_y;
    if (have_dx || have_dy) {
        if ((have_x && (float)x != old_x+(float)dx) || (have_y && (float)y != old_y+(float)dy)) return CP_INVALID;
        next_x=old_x+(float)dx; next_y=old_y+(float)dy;
    }
    if (next_x < 0 || next_x > 479 || next_y < 0 || next_y > 479) return CP_INVALID;
    if (!pad) {
        char reason[CP_REASON_CAPACITY];
        if (!m->port.delegate || !m->port.delegate_available ||
            !m->port.delegate_available(m->port.user,cmd->operation,reason)) return CP_UNAVAILABLE;
        const cp_error_t delegated_result=m->port.delegate(m->port.user,cmd,out);
        if (delegated_result != CP_OK) return delegated_result;
    } else {
        ri_job_t *j = new_job(m,cmd,RI_JOB_HID);
        if (!j && !finish) return CP_BUSY;
        if (begin) ++m->virtual_serial;
        interaction_touch_event_t event = {.gesture=m->virtual_serial,.epoch=m->port.engine->state.epoch,
            .ui_revision=m->state.ui_revision,.timestamp_ms=m->now_ms};
        if (begin) { event.phase=INTERACTION_TOUCH_BEGIN; event.x=next_x; event.y=next_y; }
        else if (cmd->operation == CP_OP_TOUCH_MOVE) {
            event.phase=INTERACTION_TOUCH_MOVE; event.x=next_x-old_x; event.y=next_y-old_y;
        } else { event.phase=cmd->operation == CP_OP_TOUCH_END ? INTERACTION_TOUCH_END : INTERACTION_TOUCH_CANCEL;
            event.x=next_x; event.y=next_y; }
        interaction_touch_result_t effect=interaction_touch_input_owned(m->port.interaction,INPUT_REMOTE,&event,
            m->now_ms,m->state.ui_revision,!m->drag_requested);
        const bool had_effect=effect.effect != INTERACTION_TOUCH_NONE;
        cp_error_t applied=touch_effect(m,cmd,effect,j,out);
        if (applied != CP_OK) {
            if (finish) {
                m->virtual_active=false;m->view.virtual_touch_active=false;
                interaction_freeze_after(m->port.interaction,m->now_ms);changed(m,APP_REMOTE_CHANGED_INPUT);
            }
            if (j) secure_zero(j,sizeof *j);
            return applied;
        }
        if (out->state == CP_RESULT_CLEANUP_PENDING) return CP_OK;
        if (begin && !m->port.interaction->touch.active) {
            if (j) secure_zero(j,sizeof *j);
            return m->port.interaction->touch.exhausted ? CP_UNAVAILABLE : CP_LOCAL_BUSY;
        }
        bool movement=false;
        if (effect.allow_motion && event.phase == INTERACTION_TOUCH_MOVE) {
            const input_delta_t delta=interaction_touch_move_owned(m->port.interaction,INPUT_REMOTE,event.x,event.y);
            movement=delta.x != 0 || delta.y != 0;
            if (movement) applied=enqueue_error(hid_engine_enqueue_mouse_move(m->port.engine,
                m->port.engine->state.epoch,INPUT_REMOTE,delta.x,delta.y));
        }
        if (applied != CP_OK) { if (j) secure_zero(j,sizeof *j); return applied; }
        if (had_effect || movement) { if (!j) return CP_INTERNAL; *out=j->result; }
        else { if (j) secure_zero(j,sizeof *j); *out=result_for(cmd->command_key,CP_RESULT_APPLIED,CP_OK,"virtual_contact_applied"); }
    }
    if (begin) {
        memcpy(m->gestures[m->gesture_count++],id,sizeof id); memcpy(m->virtual_id,id,sizeof id);
        memcpy(m->virtual_surface,surface,sizeof surface);
        m->virtual_active=true; m->virtual_epoch=m->port.engine->state.epoch; m->virtual_ui=m->state.ui_revision;
        m->virtual_started=m->now_ms;
    }
    m->virtual_x=next_x; m->virtual_y=next_y;
    m->virtual_deadline=minimum(deadline(m->now_ms,(uint64_t)deadman),m->view.lease_expires_ms);
    if (finish) m->virtual_active=false;
    m->view.virtual_touch_active=m->virtual_active;
    interaction_freeze_after(m->port.interaction,m->now_ms);
    changed(m,APP_REMOTE_CHANGED_INPUT);
    return CP_OK;
}

cp_error_t app_remote_input_execute(app_remote_input_t *m, const cp_command_t *cmd,
                                     uint64_t now_ms, cp_result_t *out)
{
    if (!m || !cmd || !out) return CP_INVALID;
    if (!app_remote_input_owns(cmd->operation)) {
        *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_NOT_IMPLEMENTED,"input_operation_not_bound");
        return CP_NOT_IMPLEMENTED;
    }
    *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_INVALID,"invalid_command");
    if (cmd->version != CP_API_VERSION || !cmd->command_key ||
        !nonzero_id(cmd->identity.principal_id) || !nonzero_id(cmd->identity.session_id) ||
        !terminated(cmd->args_json,sizeof cmd->args_json)) return CP_INVALID;
    const cp_operation_info_t *op=operation(cmd->operation);
    if (!op || op->read_only) return CP_INVALID;
    if ((cmd->identity.scopes & op->scopes) != op->scopes) {
        *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_FORBIDDEN,"forbidden");return CP_FORBIDDEN;
    }
    for (size_t i=0;i<APP_REMOTE_INPUT_JOBS;++i)
        if (m->jobs[i].kind != RI_JOB_NONE && m->jobs[i].result.command_key == cmd->command_key) {
            if (!same_identity(&m->jobs[i].identity,&cmd->identity)) {
                *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_FORBIDDEN,"foreign_command_key");return CP_FORBIDDEN;
            }
            if (m->jobs[i].operation != cmd->operation) {
                *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_REQUEST_CONFLICT,"command_key_conflict");
                return CP_REQUEST_CONFLICT;
            }
            *out=m->jobs[i].result; return CP_OK;
        }
    for (size_t i=0;i<RI_RECENT;++i)
        if (m->recent[i].result.command_key == cmd->command_key) {
            if (!same_identity(&m->recent[i].identity,&cmd->identity)) {
                *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_FORBIDDEN,"foreign_command_key");return CP_FORBIDDEN;
            }
            if (m->recent[i].operation != cmd->operation) {
                *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_REQUEST_CONFLICT,"command_key_conflict");
                return CP_REQUEST_CONFLICT;
            }
            *out=m->recent[i].result; return out->state == CP_RESULT_REJECTED ? out->error : CP_OK;
        }
    for (size_t i=0;i<RI_SAFETY_WAITERS;++i)
        if (m->safety[i].used && m->safety[i].key == cmd->command_key) {
            if (!same_identity(&m->safety[i].identity,&cmd->identity)) {
                *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_FORBIDDEN,"foreign_command_key");return CP_FORBIDDEN;
            }
            if (m->safety[i].operation != cmd->operation) {
                *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_REQUEST_CONFLICT,"command_key_conflict");
                return CP_REQUEST_CONFLICT;
            }
            *out=result_for(cmd->command_key,CP_RESULT_CLEANUP_PENDING,CP_OK,"neutral_release_pending"); return CP_OK;
        }
    if (cmd->command_key <= m->highest_key) {
        *out=result_for(cmd->command_key,CP_RESULT_REJECTED,CP_STALE,"old_command_key");return CP_STALE;
    }
    m->highest_key=cmd->command_key;
    app_remote_input_service(m,now_ms);
    cp_error_t error=CP_OK;
    char unavailable[CP_REASON_CAPACITY];
    if (!op->safety && !app_remote_input_available(m,cmd->operation,unavailable)) error=CP_UNAVAILABLE;
    else if (!op->safety && !m->state_known) error=CP_UNAVAILABLE;
    else if (!op->safety && now_ms >= cmd->deadline_ms) error=CP_EXPIRED;
    else if (!op->safety && op->input && same_target(m,cmd) != CP_OK) error=CP_STALE;
    else if (m->port.engine->state.locked && !op->safety) error=CP_LOCKED;
    else if (!op->safety && !revisions_available(m)) error=CP_UNAVAILABLE;
    if (error != CP_OK) return remember_result(m,cmd,out,CP_RESULT_REJECTED,error,cp_error_name(error));
    sync_model(m);
    if (cmd->operation == CP_OP_INPUT_CANCEL || cmd->operation == CP_OP_INPUT_RELEASE_ALL)
        error=safety_stop(m,cmd,out);
    else if (cmd->operation == CP_OP_CONTROL_RELEASE) {
        if ((m->view.pending || m->view.active) && !identity_matches(m,&cmd->identity)) error=CP_FORBIDDEN;
        else if (m->view.pending || m->view.active ||
            (identity_matches(m,&cmd->identity) && m->port.engine->state.release_pending)) error=safety_stop(m,cmd,out);
        else *out=result_for(cmd->command_key,CP_RESULT_APPLIED,CP_OK,"no_owned_lease");
    } else if (cmd->operation == CP_OP_CONTROL_ACQUIRE) error=acquire(m,cmd,out);
    else if (cmd->operation == CP_OP_INPUT_RECOVER) {
        if (cmd->management_generation != m->state.management_generation) error=CP_STALE;
        else error=m->port.delegate(m->port.user,cmd,out);
    } else if (op->safety && !m->view.active && !m->view.pending &&
        (cmd->operation == CP_OP_TOUCH_CANCEL || cmd->operation == CP_OP_MOUSE_DRAG_END)) {
        if (identity_matches(m,&cmd->identity) && m->port.engine->state.release_pending)
            error=safety_stop(m,cmd,out);
        else *out=result_for(cmd->command_key,CP_RESULT_APPLIED,CP_OK,"no_owned_gesture");
    } else {
        error=lease_gate(m,cmd);
        if (error == CP_OK && op->ui_context &&
            (cmd->ui_revision != m->state.ui_revision || !m->state.screen_awake)) error=CP_STALE;
        if (error == CP_OK && m->state.local_busy && !op->safety) error=CP_LOCAL_BUSY;
        if (error == CP_OK && cmd->operation != CP_OP_CONTROL_RENEW && !op->safety && !ready(m))
            error=m->port.engine->state.release_pending ? CP_RELEASE_PENDING : CP_UNAVAILABLE;
        const bool grid=cmd->operation >= CP_OP_SELECTION_BACK && cmd->operation <= CP_OP_SELECTION_PAGE_SET &&
            cmd->operation != CP_OP_PREVIEW_CLEAR;
        const bool keyboard=!strncmp(op->name,"keyboard.",9) || !strncmp(op->name,"preview.",8) ||
            !strncmp(op->name,"selection.",10);
        if (error == CP_OK && ((!strncmp(op->name,"mouse.",6) && m->port.engine->state.mode != INPUT_MOUSE) ||
            (keyboard && m->port.engine->state.mode != INPUT_KEYBOARD))) error=CP_STALE;
        if (error == CP_OK && (grid || !strncmp(op->name,"preview.",8)) &&
            (m->state.overlay_open || (grid && !m->state.selection_visible))) error=CP_STALE;
        if (error == CP_OK && cp_operation_flag(cmd->operation,"script_movement") &&
            m->view.producer == APP_REMOTE_PRODUCER_DEVICE_IMU) error=CP_BUSY;
        if (error == CP_OK && (cmd->operation == CP_OP_MOUSE_MOVE_RELATIVE || cmd->operation == CP_OP_MOUSE_SCROLL) &&
            m->view.producer != APP_REMOTE_PRODUCER_SCRIPT) error=CP_BUSY;
        if (error == CP_OK && m->virtual_active && (cmd->operation == CP_OP_MOUSE_MOVE_RELATIVE ||
            cmd->operation == CP_OP_MOUSE_CLICK || cmd->operation == CP_OP_MOUSE_SCROLL ||
            cmd->operation == CP_OP_MOUSE_DRAG_BEGIN)) error=CP_BUSY;
        if (error == CP_OK) {
            if (cmd->operation == CP_OP_CONTROL_RENEW) error=renew(m,cmd,out);
            else if (delegated(cmd->operation)) {
                bool enabled=false;
                if (cmd->operation == CP_OP_INPUT_MOTION_SET && cp_command_boolean(cmd,"enabled",&enabled) &&
                    enabled && m->view.producer != APP_REMOTE_PRODUCER_DEVICE_IMU) error=CP_BUSY;
                else {
                    error=m->port.delegate(m->port.user,cmd,out);
                    if (error == CP_OK && m->port.engine->state.owner == INPUT_REMOTE &&
                        m->port.engine->state.epoch != m->lease_epoch) {
                        for (size_t i=0;i<APP_REMOTE_INPUT_JOBS;++i)
                            if (m->jobs[i].kind != RI_JOB_NONE && !m->jobs[i].terminal &&
                                m->jobs[i].epoch != m->port.engine->state.epoch)
                                end_job(m,&m->jobs[i],CP_RESULT_CANCELLED,CP_STALE);
                        m->drag_requested=false; m->virtual_active=false; m->view.virtual_touch_active=false;
                        m->lease_epoch=m->port.engine->state.epoch;
                        sync_model(m);
                    }
                }
            } else if (cmd->operation == CP_OP_PREVIEW_SEND) error=begin_text(m,cmd,out,true);
            else if (cmd->operation == CP_OP_KEYBOARD_TEXT_ENQUEUE) error=begin_text(m,cmd,out,false);
            else if (!strncmp(op->name,"selection.",10) || !strncmp(op->name,"preview.",8))
                error=edit_model(m,cmd,out);
            else if (cmd->operation >= CP_OP_TOUCH_BEGIN && cmd->operation <= CP_OP_TOUCH_CANCEL)
                error=virtual_touch(m,cmd,out);
            else if (cmd->operation >= CP_OP_MOUSE_DRAG_BEGIN && cmd->operation <= CP_OP_MOUSE_DRAG_END)
                error=drag_command(m,cmd,out);
            else error=simple_hid(m,cmd,out);
        }
    }
    if (error != CP_OK) return remember_result(m,cmd,out,CP_RESULT_REJECTED,error,cp_error_name(error));
    out->command_key=cmd->command_key;
    if (out->state != CP_RESULT_ACCEPTED && out->state != CP_RESULT_RUNNING && out->state != CP_RESULT_CLEANUP_PENDING)
        remember(m,cmd->operation,&cmd->identity,out);
    return CP_OK;
}
