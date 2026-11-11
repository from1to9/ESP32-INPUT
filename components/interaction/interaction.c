// SPDX-License-Identifier: Apache-2.0
#include "interaction.h"
#include <math.h>
#include <string.h>

static void invalidate_reference(interaction_t *m, interaction_reference_reason_t reason);
static void reset_calibration(interaction_t *m);
static void recenter_reference(interaction_t *m);

static interaction_reference_diagnostics_t *reference_diagnostics(interaction_t *m)
{
    interaction_reference_diagnostics_t *d = m->reference_diagnostics;
    return d && d->owner == m ? d : NULL;
}

const interaction_reference_diagnostics_t *interaction_reference_diagnostics_get(const interaction_t *m)
{
    const interaction_reference_diagnostics_t *d = m->reference_diagnostics;
    return d && d->owner == m ? d : NULL;
}

void interaction_reference_diagnostics_reset(interaction_t *m)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->owner = m;
}

void interaction_reference_diagnostics_bind(interaction_t *m, interaction_reference_diagnostics_t *storage)
{
    m->reference_diagnostics = storage;
    if (storage) {
        storage->owner = m;
        interaction_reference_diagnostics_reset(m);
    }
}

static void reference_count(uint32_t *count, bool *saturated)
{
    if (*count == UINT32_MAX) *saturated = true;
    else ++*count;
}

static void __attribute__((noinline)) reference_begin(interaction_t *m, const interaction_imu_sample_t *sample)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (!d) return;
    interaction_reference_frame_t *f = &d->work;
    memset(f, 0, sizeof(*f));
    f->sample_known = sample != NULL;
    if (sample) f->sample = *sample;
    f->previous_known = d->previous_known && m->sample_seen && d->previous.timestamp_us == m->sample_us;
    if (f->previous_known) f->previous = d->previous;
    memcpy(f->bias, m->bias, sizeof f->bias);
    memcpy(f->gravity_before, m->gravity, sizeof f->gravity_before);
    memcpy(f->previous_accel, m->previous_accel, sizeof f->previous_accel);
    f->gravity_ready = m->gravity_ready;
    f->accel_seen = m->accel_seen;
    f->trusted_us = m->gravity_trusted_us;
    f->now_ms = m->now_ms;
    f->context = (m->view.motion_enabled ? INTERACTION_REFERENCE_ENABLED : 0U) |
        (m->output_ready ? INTERACTION_REFERENCE_OUTPUT_READY : 0U) |
        (m->view.mode == INPUT_MOUSE ? INTERACTION_REFERENCE_MOUSE : 0U) |
        (m->control.owner == m->motion_source ? INTERACTION_REFERENCE_OWNER : 0U) |
        (m->control.locked ? INTERACTION_REFERENCE_LOCKED : 0U) |
        (m->view.touch_active ? INTERACTION_REFERENCE_TOUCH : 0U) |
        (m->touch.active ? INTERACTION_REFERENCE_GESTURE : 0U) |
        (m->view.menu_open ? INTERACTION_REFERENCE_MENU : 0U) |
        (m->view.haptic_active ? INTERACTION_REFERENCE_HAPTIC : 0U) |
        (m->view.button_down ? INTERACTION_REFERENCE_BUTTON : 0U) |
        (m->now_ms < m->freeze_until_ms ? INTERACTION_REFERENCE_FREEZE : 0U);
    d->previous_known = sample != NULL;
    if (sample) d->previous = *sample;
}

static void reference_checks(interaction_t *m, uint32_t known, uint32_t pass)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (d) { d->work.checks_known |= known; d->work.checks_pass |= pass; }
}

static void __attribute__((noinline)) reference_rejected(interaction_t *m, bool haptic)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (!d || !m->gravity_ready) return;
    interaction_reference_episode_t *e = &d->episode;
    if (!e->first_known) { e->first = d->work; e->first_known = true; }
    reference_count(&e->rejected, &e->saturated);
    const uint32_t failed = d->work.checks_known & ~d->work.checks_pass;
    if (failed & INTERACTION_REFERENCE_NEAR_ONE) reference_count(&e->near_one, &e->saturated);
    if (failed & INTERACTION_REFERENCE_GENTLE) reference_count(&e->gentle, &e->saturated);
    if (failed & INTERACTION_REFERENCE_INNOVATION) reference_count(&e->innovation, &e->saturated);
    if (haptic) reference_count(&e->haptic, &e->saturated);
}

static void reference_accepted(interaction_t *m)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (d && d->episode.first_known) memset(&d->episode, 0, sizeof d->episode);
}

static const char *const letters[INTERACTION_GRID_CELLS] = {
    "ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"
};
static const char *const symbols[INTERACTION_GRID_CELLS] = {
    ".,!?", ";:-_", "+=/*", "()[]", "{}@#", "$%&", "\\|<", ">\"'", "`~^"
};

static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static void refresh_grid(interaction_t *m)
{
    const char *const *groups = m->view.page == INTERACTION_SYMBOLS ? symbols : letters;
    memset(m->view.cells, 0, sizeof m->view.cells);
    if (m->view.group < 0) {
        for (size_t cell = 0; cell < INTERACTION_GRID_CELLS; ++cell) {
            for (size_t i = 0; groups[cell][i]; ++i) {
                const unsigned char c = (unsigned char)groups[cell][i];
                m->view.cells[cell][i] = (char)(m->view.lowercase ? lower(c) : c);
            }
        }
    } else {
        const char *text = groups[m->view.group];
        for (size_t cell = 0; text[cell]; ++cell) {
            const unsigned char c = (unsigned char)text[cell];
            m->view.cells[cell][0] = (char)(m->view.lowercase ? lower(c) : c);
        }
    }
}

static void reset_pointer_output(interaction_t *m)
{
    m->aim_aligned = false;
    input_motion_reset(&m->pointer_motion);
    memset(m->filtered, 0, sizeof m->filtered);
}

static void reset_motion(interaction_t *m)
{
    m->view.motion_status |= INTERACTION_MOTION_OUTPUT_SUPPRESSED;
    m->view.motion_block_reason = INTERACTION_MOTION_BLOCK_CONTROL;
    reset_pointer_output(m);
    memset(m->touch_carry, 0, sizeof m->touch_carry);
    m->selector_cell[0] = (int)(m->view.focus % INTERACTION_GRID_COLUMNS);
    m->selector_cell[1] = (int)(m->view.focus / INTERACTION_GRID_COLUMNS);
    for (size_t axis = 0; axis < 2; ++axis)
        m->selector_angle[axis] = (float)m->selector_cell[axis] * m->config.selector_cell_degrees;
}

static void reset_focus(interaction_t *m)
{
    unsigned int best = INTERACTION_GRID_CENTER;
    unsigned int closest = INTERACTION_GRID_CELLS;
    for (unsigned int cell = 0; cell < INTERACTION_GRID_CELLS; ++cell) {
        if (!m->view.cells[cell][0]) continue;
        const int x = (int)(cell % INTERACTION_GRID_COLUMNS) - 1;
        const int y = (int)(cell / INTERACTION_GRID_COLUMNS) - 1;
        const unsigned int distance = (unsigned int)(x * x + y * y);
        if (distance < closest) {
            best = cell;
            closest = distance;
        }
    }
    m->view.focus = best;
    reset_motion(m);
}

static void reset_grid(interaction_t *m)
{
    m->view.group = -1;
    refresh_grid(m);
    reset_focus(m);
}

static uint64_t after_freeze(const interaction_t *m, uint64_t now_ms)
{
    return now_ms > UINT64_MAX - m->config.freeze_ms ? UINT64_MAX :
        now_ms + m->config.freeze_ms;
}

static bool observe_clock(interaction_t *m, uint64_t now_ms)
{
    const bool valid = !m->clock_seen || now_ms >= m->now_ms;
    m->clock_seen = true;
    m->now_ms = now_ms;
    if (!valid) {
        reference_begin(m, NULL);
        invalidate_reference(m, INTERACTION_REFERENCE_CLOCK);
        reset_calibration(m);
        m->sample_seen = false;
        m->view.motion_enabled = false;
        m->click_pending = false;
        m->second_click = false;
        m->press_consumed = true;
        m->hold_emitted = m->view.button_down;
        m->freeze_until_ms = after_freeze(m, now_ms);
        reset_motion(m);
    }
    return valid;
}

static bool valid_source(input_owner_t source)
{
    return source == INPUT_LOCAL || source == INPUT_REMOTE;
}

static bool editable_by(const interaction_t *m, input_owner_t source)
{
    return valid_source(source) && m->view.mode == INPUT_KEYBOARD && !m->control.locked &&
        m->control.owner == source && !m->send_open && !m->view.menu_open;
}

static void draft_edited(interaction_t *m)
{
    m->view.send_total = 0;
    m->view.send_accepted = 0;
}

void interaction_init(interaction_t *m)
{
    *m = (interaction_t){0};
    input_init(&m->control);
    m->config = (interaction_config_t){
        .pointer_gain_x = 4, .pointer_gain_y = 4, .freeze_ms = INTERACTION_FREEZE_MS,
        .deadzone_dps = .5f, .filter_seconds = .03f, .selector_cell_degrees = 6,
        .selector_hysteresis_degrees = 1, .selector_axis_x = 1, .selector_axis_y = 2
    };
    m->view.mode = INPUT_MOUSE;
    m->view.keyboard_motion = true;
    m->view.touch_divisor = 1;
    interaction_touch_defaults(&m->touch_config);
    reset_grid(m);
    interaction_calibrate(m);
}

void interaction_get_view(const interaction_t *m, interaction_view_t *view)
{
    *view = m->view;
    if (!m->view.motion_enabled || !m->output_ready || m->control.locked ||
        m->control.owner != m->motion_source || m->view.touch_active || m->view.menu_open ||
        m->view.haptic_active || m->view.button_down ||
        (m->touch.active && m->touch.source == m->control.owner) || m->now_ms < m->freeze_until_ms) {
        view->motion_status |= INTERACTION_MOTION_OUTPUT_SUPPRESSED;
        if (view->motion_block_reason == INTERACTION_MOTION_BLOCK_NONE)
            view->motion_block_reason = INTERACTION_MOTION_BLOCK_CONTROL;
    }
}

void interaction_sync(interaction_t *m, const input_state_t *state,
                      bool output_ready, uint64_t now_ms)
{
    (void)observe_clock(m, now_ms);
    const bool boundary = state->epoch != m->control.epoch ||
        state->mode != m->control.mode || state->owner != m->control.owner ||
        state->locked != m->control.locked || state->connected != m->control.connected;
    m->control = *state;
    m->view.mode = state->mode;
    const bool was_ready = m->output_ready;
    m->output_ready = output_ready && state->connected && !state->locked &&
        !state->release_pending;
    if (state->mode == INPUT_MOUSE && was_ready != m->output_ready)
        reset_pointer_output(m);
    m->touch_motion_ready = m->output_ready;
    if (boundary) {
        (void)interaction_touch_cancel(m);
        m->touch.latest_gesture = 0;
        m->view.motion_enabled = false;
        m->click_pending = false;
        m->second_click = false;
        m->press_consumed = m->view.button_down;
        m->freeze_until_ms = after_freeze(m, now_ms);
        if (m->send_open) {
            m->view.sending = false;
            m->view.send_settling = true;
        }
        reset_grid(m);
    }
}

void interaction_set_touch_motion_ready(interaction_t *m, bool ready)
{
    m->touch_motion_ready = ready && m->control.connected && !m->control.locked &&
        !m->control.release_pending && m->control.owner == INPUT_LOCAL;
}

bool interaction_configure(interaction_t *m, const interaction_config_t *c)
{
    if (!c || !isfinite(c->pointer_gain_x) || c->pointer_gain_x < .25f || c->pointer_gain_x > 100 ||
        !isfinite(c->pointer_gain_y) || c->pointer_gain_y < .25f || c->pointer_gain_y > 100 ||
        c->freeze_ms > 1000 ||
        c->selector_axis_x == 0 || c->selector_axis_x < -3 || c->selector_axis_x > 3 ||
        c->selector_axis_y == 0 || c->selector_axis_y < -3 || c->selector_axis_y > 3 ||
        c->selector_axis_x == c->selector_axis_y || c->selector_axis_x == -c->selector_axis_y ||
        !isfinite(c->deadzone_dps) || c->deadzone_dps < 0 || c->deadzone_dps > 10 ||
        !isfinite(c->filter_seconds) || c->filter_seconds < 0 || c->filter_seconds > 1 ||
        !isfinite(c->selector_cell_degrees) || c->selector_cell_degrees < 1 ||
        c->selector_cell_degrees > 45 || !isfinite(c->selector_hysteresis_degrees) ||
        c->selector_hysteresis_degrees < 0 ||
        c->selector_hysteresis_degrees >= c->selector_cell_degrees / 2) return false;
    m->config = *c;
    reset_motion(m);
    return true;
}

bool interaction_set_motion(interaction_t *m, bool enabled)
{
    return interaction_set_motion_owned(m, INPUT_LOCAL, enabled);
}

bool interaction_set_motion_owned(interaction_t *m, input_owner_t source, bool enabled)
{
    if (!valid_source(source)) return false;
    if (enabled && (m->view.mode != INPUT_MOUSE || !m->output_ready ||
        m->control.owner != source || m->view.menu_open)) return false;
    m->motion_source = source;
    m->view.motion_enabled = enabled;
    reset_motion(m);
    return true;
}

bool interaction_recenter_owned(interaction_t *m, input_owner_t source)
{
    if (!valid_source(source) || m->view.mode != INPUT_MOUSE || !m->view.motion_enabled ||
        !interaction_aim_reference_ready(m) || !m->output_ready || m->control.locked ||
        m->control.owner != source || m->motion_source != source || m->view.menu_open) return false;
    recenter_reference(m);
    return true;
}

bool interaction_reset_grip_owned(interaction_t *m, input_owner_t source)
{
    if (source != INPUT_LOCAL || m->view.mode != INPUT_MOUSE ||
        !interaction_aim_reference_ready(m) || !m->output_ready ||
        m->control.locked || m->control.owner != source) return false;
    recenter_reference(m);
    return true;
}

void interaction_set_keyboard_motion(interaction_t *m, bool enabled)
{
    m->view.keyboard_motion = enabled;
    reset_motion(m);
}

void interaction_set_suppressed(interaction_t *m, bool touch, bool menu,
                                bool haptic, uint64_t now_ms)
{
    (void)observe_clock(m, now_ms);
    if (m->view.touch_active != touch || m->view.menu_open != menu ||
        m->view.haptic_active != haptic) {
        m->freeze_until_ms = after_freeze(m, now_ms);
        reset_motion(m);
    }
    if (menu && !m->view.menu_open) {
        m->view.motion_enabled = false;
        m->click_pending = false;
        m->press_consumed = m->view.button_down;
    }
    m->view.touch_active = touch;
    m->view.menu_open = menu;
    m->view.haptic_active = haptic;
}

void interaction_freeze_until(interaction_t *m, uint64_t deadline_ms)
{
    if (deadline_ms <= m->freeze_until_ms) return;
    m->freeze_until_ms = deadline_ms;
    reset_motion(m);
}

void interaction_freeze_after(interaction_t *m, uint64_t event_ms)
{
    interaction_freeze_until(m, after_freeze(m, event_ms));
}

bool interaction_set_touch_divisor(interaction_t *m, unsigned int divisor)
{
    if (divisor != 1 && divisor != 4 && divisor != 8) return false;
    m->view.touch_divisor = divisor;
    memset(m->touch_carry, 0, sizeof m->touch_carry);
    return true;
}

static int8_t touch_axis(float *carry, float delta, unsigned int divisor)
{
    const float total = *carry + delta / (float)divisor;
    if (total >= 127) { *carry = 0; return 127; }
    if (total <= -127) { *carry = 0; return -127; }
    const int8_t whole = (int8_t)total;
    *carry = total - whole;
    return whole;
}

input_delta_t interaction_touch_move(interaction_t *m, float dx, float dy)
{
    return interaction_touch_move_owned(m, INPUT_LOCAL, dx, dy);
}

input_delta_t interaction_touch_move_owned(interaction_t *m, input_owner_t source, float dx, float dy)
{
    if (!isfinite(dx) || !isfinite(dy) || fabsf(dx) > 4096 || fabsf(dy) > 4096 ||
        !valid_source(source) ||
        !(source == INPUT_LOCAL ? m->view.touch_active : m->touch.active && m->touch.source == source) ||
        m->view.mode != INPUT_MOUSE || !m->output_ready ||
        m->control.owner != source || m->view.menu_open || m->view.button_down) {
        memset(m->touch_carry, 0, sizeof m->touch_carry);
        return (input_delta_t){0};
    }
    return (input_delta_t){touch_axis(&m->touch_carry[0], dx, m->view.touch_divisor),
                           touch_axis(&m->touch_carry[1], dy, m->view.touch_divisor)};
}

interaction_touch_delta_t interaction_touch_displacement_owned(interaction_t *m,
    input_owner_t source, float dx, float dy)
{
    const bool ready = source == INPUT_LOCAL ? m->touch_motion_ready : m->output_ready;
    if (!isfinite(dx) || !isfinite(dy) || fabsf(dx) > 4096 || fabsf(dy) > 4096 ||
        !valid_source(source) ||
        !(source == INPUT_LOCAL ? m->view.touch_active : m->touch.active && m->touch.source == source) ||
        m->view.mode != INPUT_MOUSE || !ready || m->control.owner != source ||
        m->view.menu_open || m->view.button_down) {
        memset(m->touch_carry, 0, sizeof m->touch_carry);
        return (interaction_touch_delta_t){0};
    }
    interaction_touch_delta_t result;
    const float x = m->touch_carry[0] + dx / (float)m->view.touch_divisor;
    const float y = m->touch_carry[1] + dy / (float)m->view.touch_divisor;
    result.x = (int16_t)x;
    result.y = (int16_t)y;
    m->touch_carry[0] = x - result.x;
    m->touch_carry[1] = y - result.y;
    return result;
}

void interaction_touch_defaults(interaction_touch_config_t *config)
{
    if (config) *config = (interaction_touch_config_t){
        .tap_to_click = true, .tap_max_ms = 250, .double_window_ms = 280,
        .contact_timeout_ms = 1000, .drag_max_ms = 30000,
        .tap_slop_px = 8, .double_slop_px = 16
    };
}

static interaction_touch_result_t touch_none(bool motion)
{
    return (interaction_touch_result_t){.allow_motion = motion};
}

static interaction_touch_result_t touch_effect(interaction_t *m, interaction_touch_effect_t effect)
{
    interaction_touch_state_t *t = &m->touch;
    if (t->serial >= UINT64_MAX - 1) {
        t->exhausted = true;
        if (effect != INTERACTION_TOUCH_RELEASE_REQUIRED) {
            t->active = t->candidate = t->second = t->have_tap = false;
            t->pending = touch_none(false);
            return t->held ? touch_effect(m, INTERACTION_TOUCH_RELEASE_REQUIRED) : touch_none(false);
        }
        t->serial = UINT64_MAX;
    } else ++t->serial;
    t->pending = (interaction_touch_result_t){.effect = effect, .token = t->serial};
    return t->pending;
}

interaction_touch_result_t interaction_touch_cancel(interaction_t *m)
{
    if (!m) return touch_none(false);
    memset(m->touch_carry, 0, sizeof m->touch_carry);
    interaction_touch_state_t *t = &m->touch;
    const bool release = t->held || t->pending.effect != INTERACTION_TOUCH_NONE;
    t->active = t->candidate = t->second = t->have_tap = false;
    if (t->pending.effect == INTERACTION_TOUCH_RELEASE_REQUIRED) return t->pending;
    t->pending = touch_none(false);
    return release ? touch_effect(m, INTERACTION_TOUCH_RELEASE_REQUIRED) : touch_none(false);
}

bool interaction_touch_configure(interaction_t *m, const interaction_touch_config_t *c)
{
    if (!m || !c || c->tap_max_ms < 50 || c->tap_max_ms > 500 ||
        c->double_window_ms < 100 || c->double_window_ms > 500 ||
        c->contact_timeout_ms < 100 || c->contact_timeout_ms > 2000 ||
        c->drag_max_ms < 1000 || c->drag_max_ms > 60000 ||
        !isfinite(c->tap_slop_px) || c->tap_slop_px < 2 || c->tap_slop_px > 24 ||
        !isfinite(c->double_slop_px) || c->double_slop_px < c->tap_slop_px ||
        c->double_slop_px > 48) return false;
    const interaction_touch_config_t *old = &m->touch_config;
    if (old->tap_to_click == c->tap_to_click && old->tap_max_ms == c->tap_max_ms &&
        old->double_window_ms == c->double_window_ms && old->contact_timeout_ms == c->contact_timeout_ms &&
        old->drag_max_ms == c->drag_max_ms && old->tap_slop_px == c->tap_slop_px &&
        old->double_slop_px == c->double_slop_px) return true;
    (void)interaction_touch_cancel(m);
    m->touch_config = *c;
    return true;
}

static bool touch_context(const interaction_t *m, uint64_t revision)
{
    return m->control.connected && !m->control.locked && valid_source(m->touch.source) &&
        m->control.owner == m->touch.source &&
        m->control.mode == INPUT_MOUSE && !m->view.menu_open && !m->view.button_down &&
        m->touch.epoch == m->control.epoch && m->touch.ui_revision == revision;
}

interaction_touch_result_t interaction_touch_tick(interaction_t *m,
    uint64_t now_ms, uint64_t current_ui_revision)
{
    if (!m) return touch_none(false);
    interaction_touch_state_t *t = &m->touch;
    const bool backwards = t->now_seen && now_ms < t->observed_ms;
    t->observed_ms = now_ms; t->now_seen = true;
    if (backwards) {
        t->clock_seen = false;
        return interaction_touch_cancel(m);
    }
    if ((t->active || t->have_tap || t->held) && !touch_context(m, current_ui_revision))
        return interaction_touch_cancel(m);
    if ((t->active || t->held) && (now_ms < t->contact_ms ||
        now_ms - t->contact_ms >= m->touch_config.contact_timeout_ms ||
        ((t->second || t->held) && (now_ms < t->started_ms ||
         now_ms - t->started_ms >= m->touch_config.drag_max_ms))))
        return interaction_touch_cancel(m);
    if (t->pending.effect == INTERACTION_TOUCH_CLICK &&
        (now_ms < t->tap_ms || now_ms - t->tap_ms >= m->touch_config.contact_timeout_ms))
        return interaction_touch_cancel(m);
    return t->pending.effect != INTERACTION_TOUCH_NONE ? t->pending : touch_none(false);
}

interaction_touch_result_t interaction_touch_admit(interaction_t *m, uint64_t token, bool accepted)
{
    if (!m || !token || m->touch.pending.effect == INTERACTION_TOUCH_NONE ||
        token != m->touch.pending.token) return touch_none(false);
    interaction_touch_state_t *t = &m->touch;
    const interaction_touch_effect_t effect = t->pending.effect;
    if (effect == INTERACTION_TOUCH_RELEASE_REQUIRED && !accepted) return t->pending;
    t->pending = touch_none(false);
    if (effect == INTERACTION_TOUCH_LEFT_UP && !accepted) return interaction_touch_cancel(m);
    if (effect == INTERACTION_TOUCH_LEFT_DOWN) {
        if (accepted) t->held = true;
        else { t->active = t->candidate = t->second = t->have_tap = false; }
    } else if (effect == INTERACTION_TOUCH_LEFT_UP || effect == INTERACTION_TOUCH_RELEASE_REQUIRED) {
        t->held = false;
    } else if (effect == INTERACTION_TOUCH_CLICK) t->have_tap = accepted;
    return touch_none(false);
}

void interaction_touch_disqualify_tap(interaction_t *m)
{
    m->touch.candidate = false;
}

bool interaction_touch_holding(const interaction_t *m)
{
    return m && (m->touch.held || m->touch.pending.effect == INTERACTION_TOUCH_LEFT_DOWN ||
        m->touch.pending.effect == INTERACTION_TOUCH_LEFT_UP ||
        m->touch.pending.effect == INTERACTION_TOUCH_RELEASE_REQUIRED);
}

interaction_touch_result_t interaction_touch_input(interaction_t *m,
    const interaction_touch_event_t *event, uint64_t now_ms,
    uint64_t current_ui_revision, bool tap_allowed)
{
    return interaction_touch_input_owned(m, INPUT_LOCAL, event, now_ms, current_ui_revision, tap_allowed);
}

interaction_touch_result_t interaction_touch_input_owned(interaction_t *m, input_owner_t source,
    const interaction_touch_event_t *event, uint64_t now_ms,
    uint64_t current_ui_revision, bool tap_allowed)
{
    if (!m || !event) return touch_none(false);
    interaction_touch_state_t *t = &m->touch;
    if (!valid_source(source)) return touch_none(false);
    if (m->control.owner != source)
        return t->source == source ? interaction_touch_cancel(m) : touch_none(false);
    if (!event->gesture || event->phase < INTERACTION_TOUCH_BEGIN || event->phase > INTERACTION_TOUCH_CANCEL)
        return interaction_touch_cancel(m);
    /* Old gestures cannot cancel or refresh a newer gesture, even with old time. */
    if (event->phase == INTERACTION_TOUCH_BEGIN) {
        if (event->gesture <= t->latest_gesture) return touch_none(false);
    } else if (!t->active || event->gesture != t->gesture) return touch_none(false);
    interaction_touch_result_t pending = interaction_touch_tick(m, now_ms, current_ui_revision);
    if (pending.effect != INTERACTION_TOUCH_NONE) return pending;
    if (!t->active && event->phase != INTERACTION_TOUCH_BEGIN) return touch_none(false);
    if (event->epoch != m->control.epoch || event->ui_revision != current_ui_revision ||
        event->timestamp_ms > now_ms || now_ms - event->timestamp_ms >= m->touch_config.contact_timeout_ms ||
        (t->clock_seen && event->timestamp_ms < t->last_ms) ||
        !isfinite(event->x) || !isfinite(event->y) || fabsf(event->x) > 4096 || fabsf(event->y) > 4096) {
        if (event->phase == INTERACTION_TOUCH_BEGIN) t->latest_gesture = event->gesture;
        return interaction_touch_cancel(m);
    }
    t->last_ms = event->timestamp_ms; t->clock_seen = true;
    if (event->phase == INTERACTION_TOUCH_CANCEL) return interaction_touch_cancel(m);
    if (event->phase == INTERACTION_TOUCH_BEGIN) {
        t->latest_gesture = event->gesture;
        if (t->active || t->exhausted) return interaction_touch_cancel(m);
        const bool second = m->output_ready && t->have_tap && tap_allowed && m->touch_config.tap_to_click &&
            event->timestamp_ms >= t->tap_ms &&
            event->timestamp_ms - t->tap_ms <= m->touch_config.double_window_ms &&
            hypotf(event->x - t->tap_x, event->y - t->tap_y) <= m->touch_config.double_slop_px;
        t->source = source; t->gesture = event->gesture; t->epoch = event->epoch; t->ui_revision = event->ui_revision;
        t->started_ms = t->contact_ms = event->timestamp_ms;
        t->start_x = t->x = event->x; t->start_y = t->y = event->y;
        t->active = true;
        t->candidate = m->output_ready && tap_allowed && m->touch_config.tap_to_click;
        t->second = second; t->have_tap = false;
        const bool motion_ready = source == INPUT_LOCAL ? m->touch_motion_ready : m->output_ready;
        if (!touch_context(m, current_ui_revision) || !motion_ready) return interaction_touch_cancel(m);
        if (second) return touch_effect(m, INTERACTION_TOUCH_LEFT_DOWN);
        return touch_none(false);
    }
    if (!touch_context(m, current_ui_revision)) return interaction_touch_cancel(m);
    if (!tap_allowed || !m->touch_config.tap_to_click) {
        if (t->held || t->second) return interaction_touch_cancel(m);
        t->candidate = t->have_tap = false;
    }
    if (event->phase == INTERACTION_TOUCH_MOVE) { t->x += event->x; t->y += event->y; }
    else { t->x = event->x; t->y = event->y; }
    if (!isfinite(t->x) || !isfinite(t->y) ||
        hypotf(t->x - t->start_x, t->y - t->start_y) > m->touch_config.tap_slop_px)
        t->candidate = false;
    t->contact_ms = event->timestamp_ms;
    if (event->phase == INTERACTION_TOUCH_END) {
        t->active = false;
        if (t->held) return touch_effect(m, INTERACTION_TOUCH_LEFT_UP);
        const bool click = t->candidate && m->output_ready &&
            event->timestamp_ms - t->started_ms <= m->touch_config.tap_max_ms;
        t->candidate = t->second = false;
        if (click) {
            t->tap_ms = event->timestamp_ms; t->tap_x = t->x; t->tap_y = t->y;
            return touch_effect(m, INTERACTION_TOUCH_CLICK);
        }
        return touch_none(false);
    }
    return touch_none(event->phase == INTERACTION_TOUCH_MOVE &&
        (source == INPUT_LOCAL ? m->touch_motion_ready : m->output_ready));
}

static bool append_character(interaction_t *m, unsigned char c)
{
    if (m->view.lowercase) c = lower(c);
    input_key_report_t report;
    if (m->view.preview_length >= INTERACTION_PREVIEW_CAPACITY || !input_ascii_report(c, &report)) return false;
    m->view.preview[m->view.preview_length++] = (char)c;
    m->view.preview[m->view.preview_length] = '\0';
    draft_edited(m); reset_grid(m);
    return true;
}

static bool confirm_cell(interaction_t *m)
{
    if (m->view.focus >= INTERACTION_GRID_CELLS || !m->view.cells[m->view.focus][0]) return false;
    if (m->view.group < 0) {
        m->view.group = (int)m->view.focus; refresh_grid(m); reset_focus(m); return true;
    }
    return append_character(m, (unsigned char)m->view.cells[m->view.focus][0]);
}

bool interaction_edit_owned(interaction_t *m, input_owner_t source, const interaction_edit_t *edit)
{
    if (!edit || !editable_by(m, source)) return false;
    switch (edit->kind) {
    case INTERACTION_EDIT_ACTIVATE:
    case INTERACTION_EDIT_FOCUS:
        if (edit->cell >= INTERACTION_GRID_CELLS || !m->view.cells[edit->cell][0]) return false;
        if (edit->kind == INTERACTION_EDIT_ACTIVATE && m->view.group >= 0 &&
            m->view.preview_length >= INTERACTION_PREVIEW_CAPACITY) return false;
        m->view.focus = edit->cell;
        if (edit->kind == INTERACTION_EDIT_ACTIVATE) return confirm_cell(m);
        reset_motion(m); return true;
    case INTERACTION_EDIT_CONFIRM: return confirm_cell(m);
    case INTERACTION_EDIT_BACK: reset_grid(m); return true;
    case INTERACTION_EDIT_CASE:
        m->view.lowercase = edit->lowercase; refresh_grid(m); return true;
    case INTERACTION_EDIT_PAGE:
        if (edit->page != INTERACTION_LETTERS && edit->page != INTERACTION_SYMBOLS) return false;
        m->view.page = edit->page; reset_grid(m); return true;
    case INTERACTION_EDIT_APPEND:
        if (!edit->text || !edit->length || edit->length > INTERACTION_PREVIEW_CAPACITY - m->view.preview_length)
            return false;
        for (size_t i = 0; i < edit->length; ++i) {
            input_key_report_t report;
            if (!input_ascii_report((unsigned char)edit->text[i], &report)) return false;
        }
        for (size_t i = 0; i < edit->length; ++i) (void)append_character(m, (unsigned char)edit->text[i]);
        return true;
    case INTERACTION_EDIT_DELETE:
        if (!edit->length || edit->length > m->view.preview_length) return false;
        m->view.preview_length -= edit->length; m->view.preview[m->view.preview_length] = '\0';
        draft_edited(m); return true;
    case INTERACTION_EDIT_CLEAR:
        m->view.preview_length = 0; m->view.preview[0] = '\0';
        draft_edited(m); reset_grid(m); return true;
    default: return false;
    }
}

bool interaction_append(interaction_t *m, unsigned char c)
{
    const char character = (char)c;
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_APPEND, .text = &character, .length = 1};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

bool interaction_confirm(interaction_t *m)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_CONFIRM};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

bool interaction_select_cell(interaction_t *m, unsigned int cell)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_ACTIVATE, .cell = cell};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

void interaction_back(interaction_t *m)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_BACK};
    (void)interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

bool interaction_set_page(interaction_t *m, interaction_page_t page)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_PAGE, .page = page};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

bool interaction_set_lowercase(interaction_t *m, bool lowercase)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_CASE, .lowercase = lowercase};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

bool interaction_delete(interaction_t *m)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_DELETE, .length = 1};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

bool interaction_clear(interaction_t *m)
{
    const interaction_edit_t edit = {.kind = INTERACTION_EDIT_CLEAR};
    return interaction_edit_owned(m, INPUT_LOCAL, &edit);
}

uint64_t interaction_send_begin(interaction_t *m)
{
    return interaction_send_begin_owned(m, INPUT_LOCAL);
}

uint64_t interaction_send_begin_owned(interaction_t *m, input_owner_t source)
{
    if (!editable_by(m, source) || !m->output_ready || !m->view.preview_length ||
        m->view.send_token == UINT64_MAX) return 0;
    ++m->view.send_token;
    m->send_epoch = m->control.epoch; m->send_open = true;
    m->view.sending = true; m->view.send_settling = false;
    m->view.send_total = m->view.preview_length; m->view.send_accepted = 0;
    reset_motion(m);
    return m->view.send_token;
}

bool interaction_send_update(interaction_t *m, uint64_t token, size_t accepted, bool active)
{
    if (!m->send_open || token != m->view.send_token || accepted < m->view.send_accepted ||
        accepted > m->view.send_total) return false;
    const size_t delta = accepted - m->view.send_accepted;
    if (delta > m->view.preview_length) return false;
    memmove(m->view.preview, m->view.preview + delta, m->view.preview_length - delta + 1);
    m->view.preview_length -= delta;
    m->view.send_accepted = accepted;
    if (!active || m->view.send_settling || m->control.epoch != m->send_epoch) {
        m->send_open = false;
        m->view.sending = false;
        m->view.send_settling = false;
    }
    return true;
}

unsigned int interaction_tick(interaction_t *m, uint64_t now_ms)
{
    if (!observe_clock(m, now_ms)) return INTERACTION_EVENT_NONE;
    if (m->view.button_down && !m->hold_emitted && now_ms - m->down_ms >= INTERACTION_HOLD_MS) {
        m->hold_emitted = true;
        m->press_consumed = true;
        m->click_pending = false;
        m->second_click = false;
        return INTERACTION_EVENT_HOLD;
    }
    if (!m->view.button_down && m->click_pending && now_ms - m->click_ms >= INTERACTION_CLICK_WINDOW_MS) {
        m->click_pending = false;
        return INTERACTION_EVENT_SINGLE;
    }
    return INTERACTION_EVENT_NONE;
}

unsigned int interaction_button_update(interaction_t *m, bool down, uint64_t now_ms)
{
    const bool regressed = m->clock_seen && now_ms < m->now_ms;
    unsigned int events = interaction_tick(m, now_ms);
    if (regressed) {
        m->view.button_down = down;
        m->hold_emitted = down;
        return INTERACTION_EVENT_NONE;
    }
    if (down == m->view.button_down) return events;
    m->view.button_down = down;
    m->freeze_until_ms = after_freeze(m, now_ms);
    reset_motion(m);
    if (down) {
        m->second_click = m->click_pending;
        m->click_pending = false;
        m->down_ms = now_ms;
        m->hold_emitted = false;
        m->press_consumed = false;
        events |= INTERACTION_EVENT_DOWN;
    } else if (!m->hold_emitted && !m->press_consumed) {
        if (m->second_click) {
            events |= INTERACTION_EVENT_DOUBLE;
            m->second_click = false;
        } else {
            m->click_pending = true;
            m->click_ms = now_ms;
        }
    }
    return events;
}

void interaction_consume_press(interaction_t *m)
{
    m->press_consumed = true;
    m->click_pending = false;
    m->second_click = false;
}

static void reset_calibration(interaction_t *m)
{
    m->view.calibration_samples = 0;
    memset(m->calibration_mean, 0, sizeof m->calibration_mean);
    memset(m->calibration_m2, 0, sizeof m->calibration_m2);
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool normalize3(float v[3])
{
    const float norm = sqrtf(dot3(v, v));
    if (!isfinite(norm) || norm < .0001f) return false;
    for (size_t i = 0; i < 3; ++i) v[i] /= norm;
    return true;
}

static void cross3(const float a[3], const float b[3], float v[3])
{
    v[0] = a[1] * b[2] - a[2] * b[1];
    v[1] = a[2] * b[0] - a[0] * b[2];
    v[2] = a[0] * b[1] - a[1] * b[0];
}

static interaction_quaternion_t inverse_rotation(interaction_quaternion_t q)
{
    return (interaction_quaternion_t){q.w, -q.x, -q.y, -q.z};
}

static void rotate3(interaction_quaternion_t q, const float v[3], float out[3])
{
    const float axis[] = {q.x, q.y, q.z};
    float first[3], second[3];
    cross3(axis, v, first);
    cross3(axis, first, second);
    for (size_t i = 0; i < 3; ++i) out[i] = v[i] + 2 * (q.w * first[i] + second[i]);
}

static bool advance_attitude(interaction_quaternion_t *pose, interaction_quaternion_t d)
{
    const interaction_quaternion_t p = *pose;
    const interaction_quaternion_t q = {
        p.w*d.w - p.x*d.x - p.y*d.y - p.z*d.z,
        p.w*d.x + p.x*d.w + p.y*d.z - p.z*d.y,
        p.w*d.y - p.x*d.z + p.y*d.w + p.z*d.x,
        p.w*d.z + p.x*d.y - p.y*d.x + p.z*d.w
    };
    const float norm = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (!isfinite(norm) || norm < .0001f) return false;
    *pose = (interaction_quaternion_t){q.w/norm, q.x/norm, q.y/norm, q.z/norm};
    return true;
}

static interaction_quaternion_t gyro_increment(const float gyro[3], float dt)
{
    const float speed = sqrtf(dot3(gyro, gyro));
    if (speed < .00001f || dt <= 0) return (interaction_quaternion_t){.w = 1};
    const float half_angle = speed * dt * .008726646259971648f;
    const float scale = sinf(half_angle) / speed;
    return (interaction_quaternion_t){cosf(half_angle), gyro[0]*scale, gyro[1]*scale, gyro[2]*scale};
}

static void set_aim_basis(interaction_t *m)
{
    float up[3];
    rotate3(inverse_rotation(m->attitude), m->world_up, up);
    const float body_right[] = {1, 0, 0}, body_top[] = {0, 1, 0};
    /* Capture a horizontal pointing ray in the current grip. When screen
     * right is nearly vertical, screen top defines the starting pitch axis. */
    cross3(up, fabsf(up[0]) > .988685997f ? body_top : body_right, m->aim_forward);
    (void)normalize3(m->aim_forward);
}

/* Restore tilt from measured gravity after a real gap. Preserve the last
 * available heading estimate. Rotation about gravity during the gap is unknown. */
static bool recover_attitude(interaction_t *m, const float measured[3])
{
    float expected[3], axis[3];
    rotate3(inverse_rotation(m->attitude), m->world_up, expected);
    float w = 1 + dot3(measured, expected);
    cross3(measured, expected, axis);
    if (w < .0001f && dot3(axis, axis) < .00000001f) {
        const float right[] = {1, 0, 0}, top[] = {0, 1, 0};
        cross3(measured, fabsf(measured[0]) < .9f ? right : top, axis);
        w = 0;
    }
    return advance_attitude(&m->attitude, (interaction_quaternion_t){w, axis[0], axis[1], axis[2]});
}

static void clear_reference(interaction_t *m)
{
    m->gravity_ready = false;
    m->gravity_predicted = false;
    m->gravity_acquire_samples = 0;
    m->accel_seen = false;
    m->view.motion_status &= (uint32_t)INTERACTION_MOTION_CALIBRATED;
    m->view.motion_status |= INTERACTION_MOTION_OUTPUT_SUPPRESSED;
    m->view.motion_block_reason = INTERACTION_MOTION_BLOCK_REFERENCE;
}

static void __attribute__((noinline)) invalidate_reference(interaction_t *m, interaction_reference_reason_t reason)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (d && m->gravity_ready) {
        reference_count(&d->losses[reason], &d->saturated);
        const uint32_t active = INTERACTION_REFERENCE_ENABLED | INTERACTION_REFERENCE_OUTPUT_READY |
            INTERACTION_REFERENCE_MOUSE | INTERACTION_REFERENCE_OWNER;
        if (m->view.motion_enabled && (d->work.context & (active | INTERACTION_REFERENCE_LOCKED)) == active) {
            reference_count(&d->active_losses, &d->saturated);
            if (!d->first_active.known) {
                d->first_active.reason = reason;
                d->first_active.episode = d->episode;
                d->first_active.terminal = d->work;
                d->first_active.known = true;
            }
        }
    }
    reference_accepted(m);
    clear_reference(m);
}

static void motion_status(interaction_t *m, interaction_motion_block_t reason)
{
    m->view.motion_status = (m->view.calibrated ? INTERACTION_MOTION_CALIBRATED : 0U) |
        (m->gravity_ready ? INTERACTION_MOTION_GRAVITY_READY : 0U) |
        (m->gravity_ready && m->gravity_predicted ? INTERACTION_MOTION_GRAVITY_PREDICTED : 0U) |
        (reason != INTERACTION_MOTION_BLOCK_NONE ? INTERACTION_MOTION_OUTPUT_SUPPRESSED : 0U);
    m->view.motion_block_reason = reason;
}

bool interaction_aim_reference_ready(const interaction_t *m)
{
    const uint64_t sample_ms = m->sample_us / 1000U;
    return m->view.calibrated && !m->view.calibrating && m->gravity_ready && m->sample_seen &&
        (!m->clock_seen || m->now_ms <= sample_ms ||
         m->now_ms - sample_ms <= INTERACTION_IMU_MAX_INTERVAL_US / 1000U);
}

static void __attribute__((noinline)) recenter_reference(interaction_t *m)
{
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (d) {
        reference_count(&d->recenter_resets, &d->saturated);
        d->last_reset = INTERACTION_REFERENCE_RESET_RECENTER;
        d->last_reset_ms = m->now_ms;
    }
    set_aim_basis(m);
    reset_motion(m);
}

static bool update_reference(interaction_t *m, const interaction_imu_sample_t *sample,
                             const float gyro[3], interaction_quaternion_t increment, float dt,
                             bool history)
{
    float accel[] = {sample->ax_g, sample->ay_g, sample->az_g};
    const float norm = sqrtf(dot3(accel, accel));
    /* A delayed sample has no contemporaneous haptic state. Keep its
     * acceleration sanity check and use its gyro for prediction only. */
    const bool haptic = !history && m->view.haptic_active;
    interaction_reference_diagnostics_t *d = reference_diagnostics(m);
    if (d) { d->work.dt = dt; d->work.dt_known = true; d->work.norm = norm; d->work.norm_known = true; }
    if (!haptic && (norm < .25f || norm > 2.5f)) {
        reference_checks(m, INTERACTION_REFERENCE_SAFE_NORM, 0);
        reference_rejected(m, false);
        invalidate_reference(m, INTERACTION_REFERENCE_ACCEL_NORM);
        return false;
    }
    if (m->gravity_ready) {
        if (!advance_attitude(&m->attitude, increment)) {
            invalidate_reference(m, haptic ? INTERACTION_REFERENCE_HAPTIC_NORMALIZE :
                                 INTERACTION_REFERENCE_PREDICT_NORMALIZE);
            return false;
        }
        rotate3(inverse_rotation(m->attitude), m->world_up, m->gravity);
    }
    if (haptic || history) {
        if (haptic) reference_rejected(m, true);
        m->gravity_acquire_samples = 0;
        m->accel_seen = false;
        m->gravity_predicted = m->gravity_ready;
        return m->gravity_ready;
    }
    reference_checks(m, INTERACTION_REFERENCE_SAFE_NORM, INTERACTION_REFERENCE_SAFE_NORM);
    float expected[3];
    rotate3(inverse_rotation(increment), m->previous_accel, expected);
    const float change[] = {accel[0] - expected[0], accel[1] - expected[1], accel[2] - expected[2]};
    const bool gentle = !m->accel_seen || (dt > 0 && sqrtf(dot3(change, change)) <= 3 * dt);
    memcpy(m->previous_accel, accel, sizeof accel);
    m->accel_seen = true;
    const bool near_one_g = fabsf(norm - 1) <= .08f;
    reference_checks(m, INTERACTION_REFERENCE_NEAR_ONE | INTERACTION_REFERENCE_GENTLE,
        (near_one_g ? INTERACTION_REFERENCE_NEAR_ONE : 0U) | (gentle ? INTERACTION_REFERENCE_GENTLE : 0U));
    if (!m->gravity_ready) {
        const float deviation[] = {accel[0] - m->acquire_accel[0],
            accel[1] - m->acquire_accel[1], accel[2] - m->acquire_accel[2]};
        const bool still = near_one_g && gentle && fabsf(gyro[0]) <= 5 &&
            fabsf(gyro[1]) <= 5 && fabsf(gyro[2]) <= 5;
        if (!still) { m->gravity_acquire_samples = 0; return false; }
        if (!m->gravity_acquire_samples || sqrtf(dot3(deviation, deviation)) > .03f) {
            memcpy(m->acquire_accel, accel, sizeof accel);
            m->gravity_acquire_us = sample->timestamp_us;
            m->gravity_acquire_samples = 1;
            return false;
        }
        ++m->gravity_acquire_samples;
        if (m->gravity_acquire_samples < 8 || sample->timestamp_us - m->gravity_acquire_us < 100000)
            return false;
        for (size_t i = 0; i < 3; ++i) accel[i] /= norm;
        if (!recover_attitude(m, accel)) return false;
        rotate3(inverse_rotation(m->attitude), m->world_up, m->gravity);
        m->gravity_ready = true;
        m->gravity_predicted = false;
        m->gravity_trusted_us = sample->timestamp_us;
        reference_accepted(m);
        return true;
    }
    for (size_t i = 0; i < 3; ++i) accel[i] /= norm;
    /* Reject inconsistent acceleration for correction only. The observed gyro
     * interval already advanced the attitude and stays usable for aiming. */
    const bool trusted = near_one_g && gentle && dot3(accel, m->gravity) >= .939692621f;
    if (near_one_g && gentle)
        reference_checks(m, INTERACTION_REFERENCE_INNOVATION, trusted ? INTERACTION_REFERENCE_INNOVATION : 0U);
    if (trusted) {
        float correction[3];
        cross3(accel, m->gravity, correction);
        const float half_alpha = dt / (2 * (.5f + dt));
        const interaction_quaternion_t rotation = {
            1, correction[0]*half_alpha, correction[1]*half_alpha, correction[2]*half_alpha
        };
        if (!advance_attitude(&m->attitude, rotation)) {
            invalidate_reference(m, INTERACTION_REFERENCE_CORRECT_NORMALIZE);
            return false;
        }
        rotate3(inverse_rotation(m->attitude), m->world_up, m->gravity);
        m->gravity_trusted_us = sample->timestamp_us;
        reference_accepted(m);
    } else {
        reference_rejected(m, false);
    }
    m->gravity_predicted = !trusted;
    return true;
}

void interaction_calibrate(interaction_t *m)
{
    reference_begin(m, NULL);
    m->view.calibrating = true;
    m->view.calibrated = false;
    m->view.motion_enabled = false;
    m->sample_seen = false;
    invalidate_reference(m, INTERACTION_REFERENCE_CALIBRATE);
    m->view.motion_status &= ~(uint32_t)INTERACTION_MOTION_CALIBRATED;
    reset_calibration(m);
    reset_motion(m);
}

void interaction_calibration_cancel(interaction_t *m)
{
    reference_begin(m, NULL);
    m->view.calibrating = false; m->view.calibrated = false; m->view.motion_enabled = false;
    m->sample_seen = false;
    memset(m->bias, 0, sizeof m->bias);
    reset_calibration(m); invalidate_reference(m, INTERACTION_REFERENCE_CANCEL); reset_motion(m);
    motion_status(m, INTERACTION_MOTION_BLOCK_CALIBRATION);
}

static void calibrate_sample(interaction_t *m, const interaction_imu_sample_t *s)
{
    const float gyro[] = {s->gx_dps, s->gy_dps, s->gz_dps};
    const float accel[] = {s->ax_g, s->ay_g, s->az_g};
    const float norm = sqrtf(s->ax_g * s->ax_g + s->ay_g * s->ay_g + s->az_g * s->az_g);
    bool stationary = fabsf(norm - 1) <= .08f && !m->view.button_down &&
        !m->view.touch_active && !m->view.haptic_active;
    for (size_t axis = 0; axis < 3; ++axis) {
        stationary = stationary && fabsf(gyro[axis]) <= 5;
        if (m->view.calibration_samples)
            stationary = stationary && fabsf(accel[axis] - m->calibration_accel_ref[axis]) <= .03f;
    }
    if (!stationary) { reset_calibration(m); return; }
    if (!m->view.calibration_samples) {
        m->calibration_start_us = s->timestamp_us;
        memcpy(m->calibration_accel_ref, accel, sizeof accel);
    }
    ++m->view.calibration_samples;
    const float count = (float)m->view.calibration_samples;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = gyro[axis] - m->calibration_mean[axis];
        m->calibration_mean[axis] += delta / count;
        m->calibration_m2[axis] += delta * (gyro[axis] - m->calibration_mean[axis]);
    }
    if (m->view.calibration_samples < INTERACTION_CALIBRATION_SAMPLES ||
        s->timestamp_us - m->calibration_start_us < 500000) return;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (m->calibration_m2[axis] / (count - 1) > .04f) {
            reset_calibration(m);
            return;
        }
    }
    memcpy(m->bias, m->calibration_mean, sizeof m->bias);
    m->view.calibrating = false;
    m->view.calibrated = true;
    m->gravity[0] = s->ax_g / norm;
    m->gravity[1] = s->ay_g / norm;
    m->gravity[2] = s->az_g / norm;
    memcpy(m->previous_accel, accel, sizeof accel);
    m->accel_seen = true;
    m->gravity_ready = true;
    m->gravity_predicted = false;
    m->gravity_trusted_us = s->timestamp_us;
    m->attitude = (interaction_quaternion_t){.w = 1};
    memcpy(m->world_up, m->gravity, sizeof m->world_up);
    set_aim_basis(m);
    motion_status(m, INTERACTION_MOTION_BLOCK_CONTROL);
    reset_motion(m);
}

static void move_focus(interaction_t *m, float x, float y, float dt)
{
    const float rates[] = {x, y};
    const unsigned int maximum[] = {INTERACTION_GRID_COLUMNS - 1, INTERACTION_GRID_ROWS - 1};
    const float width = m->config.selector_cell_degrees;
    for (size_t axis = 0; axis < 2; ++axis) {
        const float angle = m->selector_angle[axis] + rates[axis] * dt;
        m->selector_angle[axis] = fmaxf(0, fminf((float)maximum[axis] * width, angle));
    }
    const unsigned int current = m->view.focus;
    unsigned int nearest = current;
    float current_distance = 0;
    float nearest_distance = INFINITY;
    for (unsigned int cell = 0; cell < INTERACTION_GRID_CELLS; ++cell) {
        if (!m->view.cells[cell][0]) continue;
        const float dx = m->selector_angle[0] - (float)(cell % INTERACTION_GRID_COLUMNS) * width;
        const float dy = m->selector_angle[1] - (float)(cell / INTERACTION_GRID_COLUMNS) * width;
        const float distance = dx * dx + dy * dy;
        if (cell == current) current_distance = distance;
        if (distance < nearest_distance || (distance == nearest_distance && cell == current)) {
            nearest = cell;
            nearest_distance = distance;
        }
    }
    if (nearest == current) return;
    const float dx = (float)((int)(nearest % INTERACTION_GRID_COLUMNS) -
                             (int)(current % INTERACTION_GRID_COLUMNS)) * width;
    const float dy = (float)((int)(nearest / INTERACTION_GRID_COLUMNS) -
                             (int)(current / INTERACTION_GRID_COLUMNS)) * width;
    /* Comparing squared distances gives a Schmitt boundary offset by the
     * configured angular hysteresis along the line between valid centers.
     * Empty cells have no region, so they cannot receive or trap focus. */
    const float margin = 2 * m->config.selector_hysteresis_degrees * sqrtf(dx * dx + dy * dy);
    if (current_distance - nearest_distance >= margin) {
        m->view.focus = nearest;
        m->selector_cell[0] = (int)(nearest % INTERACTION_GRID_COLUMNS);
        m->selector_cell[1] = (int)(nearest / INTERACTION_GRID_COLUMNS);
    }
}

static float selected_rate(const float gyro[3], int8_t axis)
{
    const int index = axis < 0 ? -axis - 1 : axis - 1;
    return gyro[index] * (axis < 0 ? -1.0f : 1.0f);
}

static input_delta_t blocked_motion(interaction_t *m, interaction_motion_block_t reason)
{
    reset_motion(m);
    motion_status(m, reason);
    return (input_delta_t){0};
}

input_delta_t interaction_motion_sample(interaction_t *m, const interaction_imu_sample_t *s)
{
    return interaction_motion_sample_owned(m, INPUT_LOCAL, s);
}

static input_delta_t motion_sample(interaction_t *m, input_owner_t source,
                                   const interaction_imu_sample_t *s, bool history)
{
    if (!valid_source(source)) return blocked_motion(m, INTERACTION_MOTION_BLOCK_CONTROL);
    reference_begin(m, s);
    if (!s) {
        invalidate_reference(m, INTERACTION_REFERENCE_MISSING);
        m->sample_seen = false;
        reset_calibration(m);
        return blocked_motion(m, INTERACTION_MOTION_BLOCK_SAMPLE);
    }
    const float values[] = {s->gx_dps, s->gy_dps, s->gz_dps, s->ax_g, s->ay_g, s->az_g};
    for (size_t i = 0; i < 6; ++i) {
        if (!isfinite(values[i]) || fabsf(values[i]) >= (i < 3 ? 2000 : 16)) {
            invalidate_reference(m, INTERACTION_REFERENCE_INVALID_SAMPLE);
            reset_calibration(m);
            m->sample_seen = false;
            return blocked_motion(m, INTERACTION_MOTION_BLOCK_SAMPLE);
        }
    }
    const bool first = !m->sample_seen;
    const uint64_t previous = m->sample_us;
    m->sample_seen = true;
    m->sample_us = s->timestamp_us;
    if (!first && (s->timestamp_us <= previous || s->timestamp_us - previous > INTERACTION_IMU_MAX_INTERVAL_US)) {
        invalidate_reference(m, INTERACTION_REFERENCE_SAMPLE_TIME);
        reset_calibration(m);
        m->sample_seen = false;
        return blocked_motion(m, INTERACTION_MOTION_BLOCK_SAMPLE);
    }
    if (m->view.calibrating) {
        calibrate_sample(m, s);
        return blocked_motion(m, m->view.calibrating ? INTERACTION_MOTION_BLOCK_CALIBRATION :
                              INTERACTION_MOTION_BLOCK_CONTROL);
    }
    if (!m->view.calibrated)
        return blocked_motion(m, INTERACTION_MOTION_BLOCK_CALIBRATION);
    const float dt = first ? 0 : (float)(s->timestamp_us - previous) / 1000000.0f;
    const float gyro[] = {s->gx_dps - m->bias[0], s->gy_dps - m->bias[1], s->gz_dps - m->bias[2]};
    const interaction_quaternion_t previous_attitude = m->attitude;
    const bool had_reference = m->gravity_ready;
    const bool reference = update_reference(m, s, gyro, gyro_increment(gyro, dt), dt, history);
    if (history || first || m->control.locked || m->control.owner != source ||
        m->view.touch_active || (m->touch.active && m->touch.source == source) ||
        m->view.menu_open || m->view.haptic_active || m->view.button_down ||
        (m->view.mode == INPUT_KEYBOARD && m->click_pending) ||
        s->timestamp_us / 1000 < m->freeze_until_ms) {
        return blocked_motion(m, INTERACTION_MOTION_BLOCK_CONTROL);
    }
    float rates[2], response = 1;
    if (m->view.mode == INPUT_KEYBOARD) {
        rates[0] = selected_rate(gyro, m->config.selector_axis_x);
        rates[1] = selected_rate(gyro, m->config.selector_axis_y);
    } else {
        if (m->view.mode != INPUT_MOUSE || !m->view.motion_enabled || m->motion_source != source || !m->output_ready)
            return blocked_motion(m, INTERACTION_MOTION_BLOCK_CONTROL);
        if (!reference) return blocked_motion(m, INTERACTION_MOTION_BLOCK_REFERENCE);
        motion_status(m, INTERACTION_MOTION_BLOCK_NONE);
        if (!had_reference || !m->aim_aligned) {
            /* No delta crosses an inactive or unobserved interval. */
            m->aim_aligned = true;
            return (input_delta_t){0};
        }
        /* Project the gyro onto the tangent plane of the pointing ray in
         * the starting pose. Rotation around the ray contributes no motion.
         * Ease both axes to zero near vertical, where azimuth is ambiguous. */
        float up[3], right[3], tangent_up[3];
        rotate3(inverse_rotation(previous_attitude), m->world_up, up);
        cross3(m->aim_forward, up, right);
        cross3(right, m->aim_forward, tangent_up);
        const float scale = 1 / fmaxf(sqrtf(dot3(right, right)), .25f);
        /* Positive rotation about up moves the ray left. HID X increases right. */
        rates[0] = dot3(gyro, tangent_up) * scale * (m->config.invert_x ? 1.0f : -1.0f);
        rates[1] = dot3(gyro, right) * scale * (m->config.invert_y ? 1.0f : -1.0f);
        /* Keep the saved gain as the fine-motion base and cap fast turns
         * at twice that gain. Use the same response to shorten the filter. */
        const float speed = fmaxf(fabsf(rates[0]), fabsf(rates[1]));
        response += fminf(speed / 120, 1);
    }
    const float alpha = m->config.filter_seconds == 0 ? 1 :
        1 - expf(-dt * response / m->config.filter_seconds);
    if (m->view.mode == INPUT_MOUSE) {
        if (rates[0] * m->filtered[0] < 0) {
            m->filtered[0] = 0;
            m->pointer_motion.x = 0;
        }
        if (rates[1] * m->filtered[1] < 0) {
            m->filtered[1] = 0;
            m->pointer_motion.y = 0;
        }
    }
    for (size_t axis = 0; axis < 2; ++axis) {
        if (fabsf(rates[axis]) <= m->config.deadzone_dps) m->filtered[axis] = 0;
        else m->filtered[axis] += alpha * (rates[axis] - m->filtered[axis]);
    }
    if (m->view.mode == INPUT_KEYBOARD && editable_by(m, source) && m->view.keyboard_motion) {
        move_focus(m, m->filtered[0], m->filtered[1], dt);
        motion_status(m, INTERACTION_MOTION_BLOCK_CONTROL);
        return (input_delta_t){0};
    }
    if (m->view.mode != INPUT_MOUSE || !m->view.motion_enabled || m->motion_source != source || !m->output_ready)
        return blocked_motion(m, INTERACTION_MOTION_BLOCK_CONTROL);
    /* Threshold in angular units before applying independent gains. The shared
     * bounded integrator retains per-axis fractions and drops clamped excess. */
    const float x = fabsf(m->filtered[0]) <= m->config.deadzone_dps ? 0 :
        m->filtered[0] * fminf(m->config.pointer_gain_x * response, 100);
    const float y = fabsf(m->filtered[1]) <= m->config.deadzone_dps ? 0 :
        m->filtered[1] * fminf(m->config.pointer_gain_y * response, 100);
    return input_motion_step(&m->pointer_motion, x, y, dt, 1, 0, true);
}

input_delta_t interaction_motion_sample_owned(interaction_t *m, input_owner_t source,
                                              const interaction_imu_sample_t *s)
{
    return motion_sample(m, source, s, false);
}

void interaction_motion_track_history(interaction_t *m, const interaction_imu_sample_t *s)
{
    if (m->gravity_ready && m->sample_seen && m->view.calibrated && !m->view.calibrating)
        (void)motion_sample(m, INPUT_LOCAL, s, true);
}
