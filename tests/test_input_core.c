// SPDX-License-Identifier: Apache-2.0
#include "input_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void transitions(void)
{
    input_state_t s;
    input_init(&s);
    assert(s.mode == INPUT_MOUSE && !s.gyro_enabled && !s.connected);
    assert(!input_set_gyro(&s, true));
    assert(!input_ack_release(&s, s.epoch));
    input_set_connected(&s, true);
    assert(!input_can_emit(&s, s.epoch, INPUT_LOCAL, INPUT_MOUSE));
    assert(input_ack_release(&s, s.epoch));
    assert(input_set_gyro(&s, true));
    uint64_t old = s.epoch;
    assert(input_set_mode(&s, INPUT_KEYBOARD));
    assert(!s.gyro_enabled && s.release_pending);
    assert(!input_ack_release(&s, old));
    assert(!input_can_emit(&s, old, INPUT_LOCAL, INPUT_KEYBOARD));
    assert(input_ack_release(&s, s.epoch));
    assert(input_can_emit(&s, s.epoch, INPUT_LOCAL, INPUT_KEYBOARD));
    assert(!input_can_emit(&s, s.epoch, INPUT_LOCAL, INPUT_MOUSE));
    assert(!input_set_gyro(&s, true));
    input_set_owner(&s, INPUT_REMOTE);
    assert(input_ack_release(&s, s.epoch));
    assert(!input_can_emit(&s, s.epoch, INPUT_LOCAL, INPUT_KEYBOARD));
    old = s.epoch;
    input_set_owner(&s, INPUT_LOCAL);
    assert(!input_can_emit(&s, old, INPUT_REMOTE, INPUT_KEYBOARD));
    input_set_locked(&s, true);
    assert(input_ack_release(&s, s.epoch));
    assert(!input_can_emit(&s, s.epoch, INPUT_LOCAL, INPUT_KEYBOARD));
    assert(!input_set_mode(&s, INPUT_MOUSE));
    input_set_locked(&s, false);
    assert(s.release_pending && !s.gyro_enabled);
    input_set_connected(&s, false);
    old = s.epoch;
    input_set_connected(&s, true);
    assert(input_ack_release(&s, s.epoch));
    assert(!input_can_emit(&s, old, INPUT_LOCAL, INPUT_KEYBOARD));
    input_set_mode(&s, INPUT_AUDIO);
    assert(input_ack_release(&s, s.epoch));
    assert(!input_can_emit(&s, s.epoch, INPUT_LOCAL, INPUT_AUDIO));
    assert(!input_set_mode(&s, (input_mode_t)99));
}

static int integrate_second(float dt, int count)
{
    input_motion_t m = {0};
    int distance = 0;
    for (int i = 0; i < count; i++)
        distance += input_motion_step(&m, 25, 0, dt, 4, 0.5f, true).x;
    return distance;
}

static void motion(void)
{
    assert(integrate_second(.01f, 100) == 100);
    assert(integrate_second(.02f, 50) == 100);
    input_motion_t m = {0};
    assert(input_motion_step(&m, 25, -25, .01f, 1, .5f, true).x == 0);
    for (int i = 0; i < 2; i++) input_motion_step(&m, 25, -25, .01f, 1, .5f, true);
    input_delta_t d = input_motion_step(&m, 25, -25, .01f, 1, .5f, true);
    assert(d.x == 1 && d.y == -1);
    input_motion_step(&m, 25, 0, .01f, 1, .5f, true);
    input_motion_step(&m, 25, 0, .01f, 1, .5f, false);
    assert(m.x == 0 && m.y == 0);
    assert(input_motion_step(&m, 2000, -2000, .05f, 100, 0, true).x == 127);
    assert(m.x == 0 && m.y == 0); /* No burst waiting after saturation. */
    assert(input_motion_step(&m, NAN, 1, .01f, 1, 0, true).x == 0);
    assert(input_motion_step(&m, 100, 1, 1, 1, 0, true).x == 0);
    assert(input_motion_step(&m, .2f, -.2f, .01f, 99, .5f, true).x == 0);
}

static void ascii(void)
{
    assert(sizeof(input_key_report_t) == 8);
    input_key_report_t r;
    for (int c = 0; c < 256; c++) {
        memset(&r, 0xff, sizeof r);
        bool ok = input_ascii_report((unsigned char)c, &r);
        assert(ok == (c >= 32 && c <= 126));
        assert(r.reserved == 0);
        for (int i = 1; i < 6; i++) assert(r.keys[i] == 0);
        if (!ok) assert(r.keys[0] == 0 && r.modifier == 0);
    }
    assert(input_ascii_report('A', &r) && r.keys[0] == 4 && r.modifier == 2);
    assert(input_ascii_report('z', &r) && r.keys[0] == 29 && !r.modifier);
    assert(input_ascii_report('0', &r) && r.keys[0] == 39);
    assert(input_ascii_report('?', &r) && r.keys[0] == 56 && r.modifier == 2);
    assert(input_ascii_report(')', &r) && r.keys[0] == 39 && r.modifier == 2);
    assert(!input_ascii_report('\n', &r));
}

int main(void)
{
    transitions(); motion(); ascii();
    puts("PASS: state boundaries, stale epochs, release gate, motion and US ASCII");
    return 0;
}
