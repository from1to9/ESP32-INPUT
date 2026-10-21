// SPDX-License-Identifier: Apache-2.0
#include "input_core.h"
#include <math.h>
#include <string.h>

void input_init(input_state_t *s)
{
    *s = (input_state_t){ .mode = INPUT_MOUSE, .owner = INPUT_LOCAL,
                          .epoch = 1, .release_pending = true };
}

void input_invalidate(input_state_t *s)
{
    ++s->epoch;
    s->gyro_enabled = false;
    s->release_pending = true;
}

bool input_set_mode(input_state_t *s, input_mode_t mode)
{
    if (mode < INPUT_MOUSE || mode > INPUT_AUDIO || s->locked) return false;
    if (s->mode != mode) {
        input_invalidate(s);
        s->mode = mode;
    }
    return true;
}

void input_set_connected(input_state_t *s, bool connected)
{
    if (s->connected == connected) return;
    input_invalidate(s);
    s->connected = connected;
    s->owner = INPUT_LOCAL; /* A reconnect never resumes a remote lease. */
}

void input_set_locked(input_state_t *s, bool locked)
{
    if (s->locked == locked) return;
    input_invalidate(s);
    s->locked = locked;
    s->owner = INPUT_LOCAL;
}

void input_set_owner(input_state_t *s, input_owner_t owner)
{
    if (owner != INPUT_LOCAL && owner != INPUT_REMOTE) return;
    if (s->owner == owner || (s->locked && owner == INPUT_REMOTE)) return;
    input_invalidate(s);
    s->owner = owner;
}

bool input_ack_release(input_state_t *s, uint64_t epoch)
{
    if (!s->connected || epoch != s->epoch) return false;
    s->release_pending = false;
    return true;
}

bool input_can_emit(const input_state_t *s, uint64_t epoch,
                    input_owner_t source, input_mode_t kind)
{
    return s->connected && !s->locked && !s->release_pending &&
        s->epoch == epoch && s->owner == source && s->mode == kind &&
        (kind == INPUT_MOUSE || kind == INPUT_KEYBOARD);
}

bool input_set_gyro(input_state_t *s, bool enabled)
{
    return input_set_gyro_owned(s, INPUT_LOCAL, enabled);
}

bool input_set_gyro_owned(input_state_t *s, input_owner_t source, bool enabled)
{
    if (source != INPUT_LOCAL && source != INPUT_REMOTE) return false;
    if (!enabled) { s->gyro_enabled = false; return true; }
    if (!input_can_emit(s, s->epoch, source, INPUT_MOUSE)) return false;
    s->gyro_enabled = true;
    return true;
}

void input_motion_reset(input_motion_t *motion)
{
    *motion = (input_motion_t){0};
}

static int8_t integrate(float *residual, float rate, float dt,
                        float gain, float deadzone)
{
    /* Stop immediately in the deadzone, including any fractional remainder. */
    if (fabsf(rate) <= deadzone) { *residual = 0; return 0; }
    float total = *residual + rate * dt * gain;
    if (!isfinite(total)) { *residual = 0; return 0; }
    if (total >= 127) { *residual = 0; return 127; }
    if (total <= -127) { *residual = 0; return -127; }
    int8_t whole = (int8_t)total;
    *residual = total - whole;
    return whole;
}

input_delta_t input_motion_step(input_motion_t *motion, float x_dps,
    float y_dps, float dt, float gain, float deadzone, bool allowed)
{
    if (!allowed || !isfinite(x_dps) || !isfinite(y_dps) || !isfinite(dt) ||
        !isfinite(gain) || !isfinite(deadzone) || dt <= 0 || dt > .05f ||
        gain < 0 || deadzone < 0) {
        input_motion_reset(motion);
        return (input_delta_t){0};
    }
    return (input_delta_t){
        integrate(&motion->x, x_dps, dt, gain, deadzone),
        integrate(&motion->y, y_dps, dt, gain, deadzone)
    };
}

bool input_ascii_report(unsigned char c, input_key_report_t *report)
{
    *report = (input_key_report_t){0};
    uint8_t usage = 0, modifier = 0;
    if (c >= 'a' && c <= 'z') usage = (uint8_t)(4 + c - 'a');
    else if (c >= 'A' && c <= 'Z') {
        usage = (uint8_t)(4 + c - 'A'); modifier = 2; /* Left Shift */
    } else if (c >= '1' && c <= '9') usage = (uint8_t)(30 + c - '1');
    else if (c == '0') usage = 39;
    else if (c == ' ') usage = 44;
    else {
        const char *plain = "-=[]\\;'`,./";
        const char *shifted = "_+{}|:\"~<>?";
        const char *digits_shifted = "!@#$%^&*()";
        const uint8_t usages[] = {45,46,47,48,49,51,52,53,54,55,56};
        /* Reject NUL before strchr: the string terminator is not a key. */
        const char *found = c ? strchr(plain, c) : NULL;
        if (found) usage = usages[found - plain];
        else if (c && (found = strchr(shifted, c))) {
            usage = usages[found - shifted]; modifier = 2;
        } else if (c && (found = strchr(digits_shifted, c))) {
            usage = (uint8_t)(30 + (found - digits_shifted)); modifier = 2;
        } else return false;
    }
    report->modifier = modifier;
    report->keys[0] = usage;
    return true;
}
