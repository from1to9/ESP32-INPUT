// SPDX-License-Identifier: Apache-2.0
#include "interaction.h"
#include <math.h>
#include <string.h>

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

static void reset_motion(interaction_t *m)
{
    input_motion_reset(&m->pointer_motion);
    memset(m->filtered, 0, sizeof m->filtered);
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

static uint64_t after_freeze(uint64_t now_ms)
{
    return now_ms > UINT64_MAX - INTERACTION_FREEZE_MS ? UINT64_MAX :
        now_ms + INTERACTION_FREEZE_MS;
}

static bool observe_clock(interaction_t *m, uint64_t now_ms)
{
    const bool valid = !m->clock_seen || now_ms >= m->now_ms;
    m->clock_seen = true;
    m->now_ms = now_ms;
    if (!valid) {
        m->view.motion_enabled = false;
        m->click_pending = false;
        m->second_click = false;
        m->press_consumed = true;
        m->hold_emitted = m->view.button_down;
        m->freeze_until_ms = after_freeze(now_ms);
        reset_motion(m);
    }
    return valid;
}

static bool editable(const interaction_t *m)
{
    return m->view.mode == INPUT_KEYBOARD && !m->control.locked &&
        m->control.owner == INPUT_LOCAL && !m->send_open && !m->view.menu_open;
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
        .pointer_gain = 4, .deadzone_dps = .5f, .filter_seconds = .03f,
        .selector_cell_degrees = 6, .selector_hysteresis_degrees = 1
    };
    m->view.mode = INPUT_MOUSE;
    m->view.keyboard_motion = true;
    m->view.touch_divisor = 1;
    reset_grid(m);
    interaction_calibrate(m);
}

void interaction_get_view(const interaction_t *m, interaction_view_t *view)
{
    *view = m->view;
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
    m->output_ready = output_ready && state->connected && !state->locked &&
        !state->release_pending;
    if (boundary) {
        m->view.motion_enabled = false;
        m->click_pending = false;
        m->second_click = false;
        m->press_consumed = m->view.button_down;
        m->freeze_until_ms = after_freeze(now_ms);
        if (m->send_open) {
            m->view.sending = false;
            m->view.send_settling = true;
        }
        reset_grid(m);
    }
}

bool interaction_configure(interaction_t *m, const interaction_config_t *c)
{
    if (!c || !isfinite(c->pointer_gain) || c->pointer_gain < 0 || c->pointer_gain > 100 ||
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
    if (enabled && (m->view.mode != INPUT_MOUSE || !m->output_ready ||
        m->control.owner != INPUT_LOCAL || m->view.menu_open)) return false;
    m->view.motion_enabled = enabled;
    reset_motion(m);
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
        m->freeze_until_ms = after_freeze(now_ms);
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
    if (!isfinite(dx) || !isfinite(dy) || fabsf(dx) > 4096 || fabsf(dy) > 4096 ||
        !m->view.touch_active || m->view.mode != INPUT_MOUSE || !m->output_ready ||
        m->control.owner != INPUT_LOCAL || m->view.menu_open || m->view.button_down) {
        memset(m->touch_carry, 0, sizeof m->touch_carry);
        return (input_delta_t){0};
    }
    return (input_delta_t){touch_axis(&m->touch_carry[0], dx, m->view.touch_divisor),
                           touch_axis(&m->touch_carry[1], dy, m->view.touch_divisor)};
}

bool interaction_append(interaction_t *m, unsigned char c)
{
    if (m->view.lowercase) c = lower(c);
    input_key_report_t report;
    if (!editable(m) || m->view.preview_length >= INTERACTION_PREVIEW_CAPACITY ||
        !input_ascii_report(c, &report)) return false;
    m->view.preview[m->view.preview_length++] = (char)c;
    m->view.preview[m->view.preview_length] = '\0';
    draft_edited(m);
    reset_grid(m);
    return true;
}

bool interaction_confirm(interaction_t *m)
{
    if (!editable(m) || m->view.focus >= INTERACTION_GRID_CELLS ||
        m->view.cells[m->view.focus][0] == '\0') return false;
    if (m->view.group < 0) {
        m->view.group = (int)m->view.focus;
        refresh_grid(m);
        reset_focus(m);
        return true;
    }
    return interaction_append(m, (unsigned char)m->view.cells[m->view.focus][0]);
}

bool interaction_select_cell(interaction_t *m, unsigned int cell)
{
    if (!editable(m) || cell >= INTERACTION_GRID_CELLS || !m->view.cells[cell][0]) return false;
    m->view.focus = cell;
    return interaction_confirm(m);
}

void interaction_back(interaction_t *m)
{
    if (editable(m)) reset_grid(m);
}

bool interaction_set_page(interaction_t *m, interaction_page_t page)
{
    if (!editable(m) || (page != INTERACTION_LETTERS && page != INTERACTION_SYMBOLS)) return false;
    m->view.page = page;
    reset_grid(m);
    return true;
}

bool interaction_set_lowercase(interaction_t *m, bool lowercase)
{
    if (!editable(m)) return false;
    m->view.lowercase = lowercase;
    refresh_grid(m);
    return true;
}

bool interaction_delete(interaction_t *m)
{
    if (!editable(m) || m->view.preview_length == 0) return false;
    m->view.preview[--m->view.preview_length] = '\0';
    draft_edited(m);
    return true;
}

bool interaction_clear(interaction_t *m)
{
    if (!editable(m)) return false;
    m->view.preview_length = 0;
    m->view.preview[0] = '\0';
    draft_edited(m);
    reset_grid(m);
    return true;
}

uint64_t interaction_send_begin(interaction_t *m)
{
    if (!editable(m) || !m->output_ready || !m->view.preview_length ||
        m->view.send_token == UINT64_MAX) return 0;
    ++m->view.send_token;
    m->send_epoch = m->control.epoch;
    m->send_open = true;
    m->view.sending = true;
    m->view.send_settling = false;
    m->view.send_total = m->view.preview_length;
    m->view.send_accepted = 0;
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
    m->freeze_until_ms = after_freeze(now_ms);
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

void interaction_calibrate(interaction_t *m)
{
    m->view.calibrating = true;
    m->view.calibrated = false;
    m->view.motion_enabled = false;
    m->sample_seen = false;
    reset_calibration(m);
    reset_motion(m);
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

input_delta_t interaction_motion_sample(interaction_t *m, const interaction_imu_sample_t *s)
{
    const float values[] = {s->gx_dps, s->gy_dps, s->gz_dps, s->ax_g, s->ay_g, s->az_g};
    for (size_t i = 0; i < 6; ++i) {
        if (!isfinite(values[i]) || fabsf(values[i]) > (i < 3 ? 2000 : 16)) {
            reset_motion(m);
            reset_calibration(m);
            m->sample_seen = false;
            return (input_delta_t){0};
        }
    }
    const bool first = !m->sample_seen;
    const uint64_t previous = m->sample_us;
    m->sample_seen = true;
    m->sample_us = s->timestamp_us;
    if (!first && (s->timestamp_us <= previous || s->timestamp_us - previous > 50000)) {
        reset_motion(m);
        reset_calibration(m);
        return (input_delta_t){0};
    }
    if (m->view.calibrating) {
        calibrate_sample(m, s);
        return (input_delta_t){0};
    }
    if (first || !m->view.calibrated || m->control.locked || m->control.owner != INPUT_LOCAL ||
        m->view.touch_active || m->view.menu_open || m->view.haptic_active || m->view.button_down ||
        (m->view.mode == INPUT_KEYBOARD && m->click_pending) ||
        s->timestamp_us / 1000 < m->freeze_until_ms) {
        reset_motion(m);
        return (input_delta_t){0};
    }
    const float dt = (float)(s->timestamp_us - previous) / 1000000.0f;
    const float rates[] = {s->gx_dps - m->bias[0], s->gy_dps - m->bias[1]};
    const float alpha = m->config.filter_seconds == 0 ? 1 : dt / (m->config.filter_seconds + dt);
    for (size_t axis = 0; axis < 2; ++axis) {
        if (fabsf(rates[axis]) <= m->config.deadzone_dps) m->filtered[axis] = 0;
        else m->filtered[axis] += alpha * (rates[axis] - m->filtered[axis]);
    }
    if (m->view.mode == INPUT_KEYBOARD && editable(m) && m->view.keyboard_motion) {
        move_focus(m, m->filtered[0], m->filtered[1], dt);
        return (input_delta_t){0};
    }
    return input_motion_step(&m->pointer_motion, m->filtered[0], m->filtered[1], dt,
        m->config.pointer_gain, m->config.deadzone_dps,
        m->view.mode == INPUT_MOUSE && m->view.motion_enabled && m->output_ready);
}
