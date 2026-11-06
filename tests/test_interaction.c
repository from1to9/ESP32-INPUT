// SPDX-License-Identifier: Apache-2.0
#include "interaction.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void setup(interaction_t *m, input_mode_t mode)
{
    interaction_init(m);
    input_state_t state;
    input_init(&state);
    input_set_connected(&state, true);
    assert(input_set_mode(&state, mode));
    assert(input_ack_release(&state, state.epoch));
    interaction_sync(m, &state, true, 0);
    interaction_config_t config = m->config;
    config.filter_seconds = 0;
    assert(interaction_configure(m, &config));
}

static interaction_imu_sample_t sample(uint64_t time, float gx, float gy)
{
    return (interaction_imu_sample_t){
        .timestamp_us = time, .gx_dps = gx, .gy_dps = gy, .gz_dps = .5f, .az_g = 1
    };
}

static uint64_t calibrate(interaction_t *m)
{
    uint64_t time = 0;
    for (size_t i = 0; i < 64; ++i, time += 10000) {
        const interaction_imu_sample_t s = sample(time, 1, -2);
        const input_delta_t delta = interaction_motion_sample(m, &s);
        assert(delta.x == 0 && delta.y == 0);
    }
    assert(m->view.calibrated && !m->view.calibrating);
    return time - 10000;
}

static input_delta_t motion(interaction_t *m, uint64_t *time, uint64_t dt,
                            float gx, float gy)
{
    *time += dt;
    interaction_imu_sample_t s = sample(*time, gx, gy);
    /* Existing scalar pointer tests request horizontal/vertical angular rates.
     * Mouse fixtures express them as screen-up yaw Z and pitch X; keyboard
     * fixtures retain their explicit X/Y selector mapping. */
    if (m->view.mode == INPUT_MOUSE) {
        s.gx_dps = 1 + gy + 2;
        s.gy_dps = -2;
        s.gz_dps = .5f + gx - 1;
    }
    return interaction_motion_sample(m, &s);
}

static void align_mouse_output(interaction_t *m, uint64_t *time)
{
    assert(m->gravity_ready && m->view.motion_enabled);
    const input_delta_t d = motion(m, time, 10000, 1, -2);
    assert(!d.x && !d.y && m->aim_aligned);
}

static void assert_valid_focus(const interaction_t *m)
{
    assert(m->view.focus < INTERACTION_GRID_CELLS);
    assert(m->view.cells[m->view.focus][0] != '\0');
}

static void focus_cell(interaction_t *m, uint64_t *time, unsigned int target)
{
    assert(target < INTERACTION_GRID_CELLS && m->view.cells[target][0]);
    for (size_t step = 0; step < 200 && m->view.focus != target; ++step) {
        const int dx = (int)(target % 3) - (int)(m->view.focus % 3);
        const int dy = (int)(target / 3) - (int)(m->view.focus / 3);
        const input_delta_t delta = motion(m, time, 10000,
            1 + 100.0f * (float)dx, -2 + 100.0f * (float)dy);
        assert(delta.x == 0 && delta.y == 0);
        assert_valid_focus(m);
    }
    assert(m->view.focus == target);
}

static void nine_cell_grid_and_preview(void)
{
    interaction_t m;
    setup(&m, INPUT_KEYBOARD);
    const char *const groups[] = {
        "ABCD", "EFGH", "IJKL", "MNOP", "QRST", "UVWX", "YZ01", "2345", "6789"
    };
    const char *const punctuation[] = {
        ".,!?", ";:-_", "+=/*", "()[]", "{}@#", "$%&", "\\|<", ">\"'", "`~^"
    };
    bool covered[128] = {false};
    assert(INTERACTION_GRID_CELLS == 9 && m.view.focus == 4);
    for (unsigned int page = 0; page < 2; ++page) {
        assert(interaction_set_page(&m, page ? INTERACTION_SYMBOLS : INTERACTION_LETTERS));
        const char *const *expected = page ? punctuation : groups;
        for (unsigned int group = 0; group < 9; ++group) {
            assert(strcmp(m.view.cells[group], expected[group]) == 0);
            const size_t count = strlen(expected[group]);
            for (unsigned int character = 0; character < count; ++character) {
                assert(interaction_select_cell(&m, group));
                assert(m.view.preview_length == 0); /* First confirm never appends. */
                assert(m.view.group == (int)group && m.view.focus == 1);
                assert(interaction_select_cell(&m, character));
                assert(m.view.preview_length == 1 && m.view.preview[0] == expected[group][character]);
                const unsigned char c = (unsigned char)m.view.preview[0];
                assert(c < 128 && !covered[c]);
                covered[c] = true;
                assert(m.view.group == -1 && m.view.focus == 4);
                assert(interaction_delete(&m));
            }
            assert(interaction_select_cell(&m, group));
            for (unsigned int empty = (unsigned int)count; empty < 9; ++empty) {
                assert(!interaction_select_cell(&m, empty));
                assert(m.view.focus == 1 && m.view.preview_length == 0);
            }
            interaction_back(&m);
            assert(m.view.focus == 4 && m.view.group == -1);
        }
    }
    for (unsigned int c = 0; c < 128; ++c)
        assert(covered[c] == (c >= 33 && c <= 126 && !(c >= 'a' && c <= 'z')));
    assert(!interaction_set_page(&m, INTERACTION_DIGITS));
    assert(!interaction_set_page(&m, (interaction_page_t)99));
    assert(!interaction_select_cell(&m, 9));
    assert(interaction_set_page(&m, INTERACTION_LETTERS));
    assert(interaction_set_lowercase(&m, true));
    assert(strcmp(m.view.cells[0], "abcd") == 0);
    bool lower_covered[128] = {false};
    for (unsigned int group = 0; group < 9; ++group) {
        for (unsigned int cell = 0; cell < 4; ++cell) {
            assert(interaction_select_cell(&m, group));
            assert(interaction_select_cell(&m, cell));
            unsigned char expected = (unsigned char)groups[group][cell];
            if (expected >= 'A' && expected <= 'Z') expected = (unsigned char)(expected + ('a' - 'A'));
            assert((unsigned char)m.view.preview[0] == expected && !lower_covered[expected]);
            lower_covered[expected] = true;
            assert(interaction_delete(&m));
        }
    }
    for (unsigned int c = 0; c < 128; ++c)
        assert(lower_covered[c] == ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')));
    assert(interaction_select_cell(&m, 0) && interaction_select_cell(&m, 0));
    assert(strcmp(m.view.preview, "a") == 0);
    assert(interaction_append(&m, 'A') && strcmp(m.view.preview, "aa") == 0);
    assert(interaction_delete(&m));
    assert(interaction_append(&m, ' ') && interaction_append(&m, '9'));
    assert(strcmp(m.view.preview, "a 9") == 0);
    assert(interaction_clear(&m));
    for (unsigned int c = 0; c < 256; ++c) {
        const bool accepted = interaction_append(&m, (unsigned char)c);
        assert(accepted == (c >= 32 && c <= 126));
        if (accepted) assert(interaction_delete(&m));
    }
    for (size_t i = 0; i < 64; ++i) assert(interaction_append(&m, 'A'));
    assert(!interaction_append(&m, 'B') && m.view.preview_length == 64);
    assert(m.view.preview[64] == '\0');
    interaction_view_t view;
    interaction_get_view(&m, &view);
    assert(strcmp(view.preview, m.view.preview) == 0);
    assert(interaction_clear(&m) && !interaction_delete(&m));
}

static void send_prefix_and_interruption(void)
{
    interaction_t m;
    setup(&m, INPUT_KEYBOARD);
    assert(interaction_send_begin(&m) == 0);
    char text[65];
    for (size_t i = 0; i < 64; ++i) {
        text[i] = (char)('A' + (int)(i % 26));
        assert(interaction_append(&m, (unsigned char)text[i]));
    }
    text[64] = '\0';
    const uint64_t token = interaction_send_begin(&m);
    assert(token && m.view.sending && m.view.send_total == 64);
    interaction_sync(&m, &m.control, false, 10);
    interaction_sync(&m, &m.control, true, 20);
    assert(m.view.sending && !m.view.send_settling && m.view.send_token == token);
    assert(!interaction_append(&m, ' ') && !interaction_clear(&m) && !interaction_delete(&m));
    assert(interaction_send_begin(&m) == 0);
    assert(strcmp(m.view.preview, text) == 0); /* Explicit begin emits/removes nothing. */
    assert(interaction_send_update(&m, token, 32, true));
    assert(strcmp(m.view.preview, text + 32) == 0);
    assert(interaction_send_update(&m, token, 33, true)); /* Next HID batch, cumulative prefix. */
    assert(strcmp(m.view.preview, text + 33) == 0);
    assert(!interaction_send_update(&m, token, 32, true));
    assert(!interaction_send_update(&m, token + 1, 34, true));
    assert(!interaction_send_update(&m, token, 65, false));
    input_state_t state = m.control;
    input_set_locked(&state, true);
    interaction_sync(&m, &state, false, 1000);
    assert(m.view.send_settling && !m.view.sending);
    assert(!interaction_clear(&m));
    assert(interaction_send_update(&m, token, 34, true)); /* Final accepted key-down at interruption. */
    assert(!m.view.sending && !m.view.send_settling);
    assert(strcmp(m.view.preview, text + 34) == 0);
    assert(!interaction_send_update(&m, token, 35, true));
    input_set_locked(&state, false);
    assert(input_ack_release(&state, state.epoch));
    interaction_sync(&m, &state, true, 1100);
    const uint64_t next = interaction_send_begin(&m);
    assert(next && next != token);
    assert(interaction_send_update(&m, next, 0, false)); /* Queue rejection retains all unsent text. */
    assert(strcmp(m.view.preview, text + 34) == 0);
    const uint64_t final = interaction_send_begin(&m);
    assert(final && !interaction_send_update(&m, next, 1, false));
    assert(interaction_send_update(&m, final, 30, false));
    assert(m.view.preview_length == 0 && m.view.preview[0] == '\0');
    assert(m.view.send_accepted == 30 && !m.view.sending);

    assert(interaction_append(&m, 'x') && interaction_append(&m, 'y'));
    assert(m.view.send_accepted == 0 && m.view.send_total == 0);
    const uint64_t disconnected = interaction_send_begin(&m);
    input_set_connected(&state, false);
    interaction_sync(&m, &state, false, 1200);
    assert(interaction_send_update(&m, disconnected, 1, false));
    assert(strcmp(m.view.preview, "y") == 0 && interaction_send_begin(&m) == 0);
}

static void progress_until_edit(void)
{
    interaction_t m;
    setup(&m, INPUT_KEYBOARD);
    assert(interaction_append(&m, 'a') && interaction_append(&m, 'b'));
    uint64_t token = interaction_send_begin(&m);
    assert(interaction_send_update(&m, token, 1, false));
    assert(m.view.send_total == 2 && m.view.send_accepted == 1);
    assert(interaction_set_page(&m, INTERACTION_SYMBOLS));
    assert(m.view.send_total == 2 && m.view.send_accepted == 1);
    assert(interaction_delete(&m));
    assert(m.view.send_total == 0 && m.view.send_accepted == 0);
    assert(interaction_append(&m, 'x') && interaction_append(&m, 'y'));
    token = interaction_send_begin(&m);
    assert(interaction_send_update(&m, token, 1, false));
    assert(interaction_clear(&m));
    assert(m.view.send_total == 0 && m.view.send_accepted == 0);
    assert(interaction_append(&m, 'x'));
    token = interaction_send_begin(&m);
    assert(interaction_send_update(&m, token, 1, false));
    assert(m.view.send_total == 1 && m.view.send_accepted == 1);
    assert(interaction_append(&m, 'y'));
    assert(m.view.send_total == 0 && m.view.send_accepted == 0);
}

static void button_timing(void)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    assert(interaction_button_update(&m, true, 100) == INTERACTION_EVENT_DOWN);
    assert(m.view.button_down);
    assert(interaction_button_update(&m, false, 130) == 0);
    interaction_sync(&m, &m.control, false, 200);
    interaction_sync(&m, &m.control, true, 300);
    assert(interaction_tick(&m, 409) == 0);
    assert(interaction_tick(&m, 410) == INTERACTION_EVENT_SINGLE);
    assert(interaction_tick(&m, 1000) == 0);

    setup(&m, INPUT_MOUSE);
    assert(interaction_button_update(&m, true, 100) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&m, false, 150) == 0);
    assert(interaction_button_update(&m, true, 429) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&m, false, 450) == INTERACTION_EVENT_DOUBLE);
    assert(interaction_tick(&m, 1000) == 0);

    setup(&m, INPUT_MOUSE);
    assert(interaction_button_update(&m, true, 100) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&m, false, 150) == 0);
    assert(interaction_button_update(&m, true, 430) ==
           (INTERACTION_EVENT_SINGLE | INTERACTION_EVENT_DOWN));
    assert(interaction_button_update(&m, false, 450) == 0);
    assert(interaction_tick(&m, 730) == INTERACTION_EVENT_SINGLE);

    setup(&m, INPUT_MOUSE);
    assert(interaction_button_update(&m, true, 100) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&m, false, 150) == 0);
    assert(interaction_button_update(&m, true, 200) == INTERACTION_EVENT_DOWN);
    assert(interaction_tick(&m, 1100) == INTERACTION_EVENT_HOLD);
    assert(interaction_button_update(&m, false, 1110) == 0);
    assert(interaction_tick(&m, 2000) == 0); /* The second hold consumes the first click too. */

    setup(&m, INPUT_MOUSE);
    assert(interaction_button_update(&m, true, 100) == INTERACTION_EVENT_DOWN);
    assert(interaction_tick(&m, 999) == 0);
    assert(interaction_tick(&m, 1000) == INTERACTION_EVENT_HOLD);
    assert(interaction_tick(&m, 2000) == 0);
    input_state_t state = m.control;
    input_set_locked(&state, true);
    interaction_sync(&m, &state, false, 2000);
    assert(interaction_tick(&m, 2900) == 0); /* A continuous hold cannot unlock again. */
    assert(interaction_button_update(&m, false, 3000) == 0);
    assert(interaction_tick(&m, 4000) == 0);
    assert(interaction_button_update(&m, true, 5000) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&m, false, 5900) == INTERACTION_EVENT_HOLD);

    setup(&m, INPUT_MOUSE);
    assert(interaction_button_update(&m, true, 10) == INTERACTION_EVENT_DOWN);
    interaction_consume_press(&m);
    assert(interaction_tick(&m, 910) == INTERACTION_EVENT_HOLD); /* Takeover can still lock. */
    assert(interaction_button_update(&m, false, 911) == 0);
    assert(interaction_button_update(&m, true, 1000) == INTERACTION_EVENT_DOWN);
    interaction_consume_press(&m);
    assert(interaction_button_update(&m, false, 1010) == 0);
    assert(interaction_tick(&m, 1500) == 0);
    assert(interaction_button_update(&m, true, 2000) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&m, false, 2010) == 0);
    assert(interaction_tick(&m, 2009) == 0); /* Clock regression drops pending gestures. */
    assert(interaction_tick(&m, 3000) == 0);
}

static int pointer_second(uint64_t dt, size_t count)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    uint64_t time = calibrate(&m);
    assert(interaction_set_motion(&m, true));
    align_mouse_output(&m, &time);
    int total = 0;
    for (size_t i = 0; i < count; ++i) total += motion(&m, &time, dt, 26, -2).x;
    return total;
}

static void pending_click_keeps_keyboard_selection(void)
{
    const unsigned int selected_cells[] = {0, 2, 8};
    for (unsigned int child = 0; child < 2; ++child) {
        for (size_t chosen = 0; chosen < sizeof selected_cells / sizeof selected_cells[0]; ++chosen) {
            const unsigned int selected = selected_cells[chosen];
            if (child && selected >= 4) continue;
            interaction_t m;
            setup(&m, INPUT_KEYBOARD);
            uint64_t time = calibrate(&m);
            interaction_config_t config = m.config;
            config.filter_seconds = .03f;
            assert(interaction_configure(&m, &config));
            if (child) assert(interaction_select_cell(&m, 0));
            focus_cell(&m, &time, selected);
            assert(m.view.focus == selected && m.view.preview_length == 0);
            const uint64_t down_ms = time / 1000 + 10;
            const uint64_t up_ms = down_ms + 20;
            assert(interaction_button_update(&m, true, down_ms) == INTERACTION_EVENT_DOWN);
            assert(interaction_button_update(&m, false, up_ms) == INTERACTION_EVENT_NONE);
            time = up_ms * 1000;
            const float moving_rate = selected ? -79.0f : 81.0f;
            for (uint64_t elapsed = 1; elapsed < INTERACTION_CLICK_WINDOW_MS; ++elapsed) {
                assert(interaction_tick(&m, up_ms + elapsed) == INTERACTION_EVENT_NONE);
                const input_delta_t delta = motion(&m, &time, 1000, moving_rate, -2);
                assert(delta.x == 0 && delta.y == 0);
                assert(m.view.focus == selected && m.view.preview_length == 0);
            }
            /* The deadline's sample can arrive before the coordinator polls
             * the event. It must still confirm the cell selected at DOWN. */
            assert(motion(&m, &time, 1000, moving_rate, -2).x == 0);
            assert(m.view.focus == selected);
            assert(interaction_tick(&m, up_ms + INTERACTION_CLICK_WINDOW_MS) ==
                   INTERACTION_EVENT_SINGLE);
            assert(interaction_confirm(&m));
            if (child) {
                assert(m.view.group == -1 && m.view.preview_length == 1);
                assert(m.view.preview[0] == (selected ? 'C' : 'A'));
            } else {
                assert(m.view.group == (int)selected && m.view.preview_length == 0);
                const char first[] = {'A', 'I', '6'};
                assert(m.view.cells[0][0] == first[chosen]);
            }
            const unsigned int neutral = child ? 4U : 1U;
            assert(m.view.focus == neutral);
            for (size_t i = 0; i < 10; ++i)
                assert(motion(&m, &time, 10000, 1, -2).x == 0);
            assert(m.view.focus == neutral); /* Suppressed movement has no residual replay. */
            for (size_t i = 0; i < 12; ++i)
                assert(motion(&m, &time, 10000, 81, -2).x == 0);
            assert(m.view.focus == (child ? 5U : 2U)); /* Selection resumes after classification/confirmation. */
            assert(m.view.preview_length == (child ? 1U : 0U));
            assert(interaction_tick(&m, time / 1000) == INTERACTION_EVENT_NONE);
        }
    }
}

static void pending_selection_double_hold_and_mouse(void)
{
    for (unsigned int hold = 0; hold < 2; ++hold) {
        interaction_t m;
        setup(&m, INPUT_KEYBOARD);
        uint64_t time = calibrate(&m);
        assert(interaction_select_cell(&m, 0));
        focus_cell(&m, &time, 0);
        const uint64_t down_ms = time / 1000 + 10;
        const uint64_t up_ms = down_ms + 20;
        assert(interaction_button_update(&m, true, down_ms) == INTERACTION_EVENT_DOWN);
        assert(interaction_button_update(&m, false, up_ms) == INTERACTION_EVENT_NONE);
        time = up_ms * 1000;
        for (uint64_t elapsed = 1; elapsed < INTERACTION_CLICK_WINDOW_MS; ++elapsed) {
            assert(interaction_tick(&m, up_ms + elapsed) == INTERACTION_EVENT_NONE);
            assert(motion(&m, &time, 1000, 81, -2).x == 0);
            assert(m.view.focus == 0 && m.view.preview_length == 0);
        }
        const uint64_t second_ms = up_ms + INTERACTION_CLICK_WINDOW_MS - 1;
        assert(interaction_button_update(&m, true, second_ms) == INTERACTION_EVENT_DOWN);
        const uint64_t duration = hold ? INTERACTION_HOLD_MS : 20;
        for (uint64_t elapsed = 1; elapsed < duration; ++elapsed) {
            assert(interaction_tick(&m, second_ms + elapsed) == INTERACTION_EVENT_NONE);
            assert(motion(&m, &time, 1000, 81, -2).x == 0);
            assert(m.view.focus == 0 && m.view.preview_length == 0);
        }
        if (hold) {
            assert(interaction_tick(&m, second_ms + duration) == INTERACTION_EVENT_HOLD);
            assert(interaction_button_update(&m, false, second_ms + duration) ==
                   INTERACTION_EVENT_NONE);
        } else {
            assert(interaction_button_update(&m, false, second_ms + duration) ==
                   INTERACTION_EVENT_DOUBLE);
        }
        assert(m.view.preview_length == 0 && m.view.group == 0);
        assert(interaction_tick(&m, second_ms + duration + INTERACTION_CLICK_WINDOW_MS) ==
               INTERACTION_EVENT_NONE);
    }

    interaction_t mouse;
    setup(&mouse, INPUT_MOUSE);
    uint64_t time = calibrate(&mouse);
    assert(interaction_set_motion(&mouse, true));
    align_mouse_output(&mouse, &time);
    const uint64_t down_ms = time / 1000 + 10;
    const uint64_t up_ms = down_ms + 20;
    assert(interaction_button_update(&mouse, true, down_ms) == INTERACTION_EVENT_DOWN);
    assert(interaction_button_update(&mouse, false, up_ms) == INTERACTION_EVENT_NONE);
    time = up_ms * 1000;
    for (uint64_t elapsed = 10; elapsed < INTERACTION_FREEZE_MS; elapsed += 10) {
        assert(interaction_tick(&mouse, up_ms + elapsed) == INTERACTION_EVENT_NONE);
        assert(motion(&mouse, &time, 10000, 26, -2).x == 0);
    }
    assert(interaction_tick(&mouse, up_ms + INTERACTION_FREEZE_MS) == INTERACTION_EVENT_NONE);
    assert(motion(&mouse, &time, 10000, 26, -2).x == 0);
    assert(motion(&mouse, &time, 10000, 26, -2).x == 1);
}

static void motion_calibration_and_focus(void)
{
    assert(pointer_second(10000, 100) == 100);
    assert(pointer_second(20000, 50) == 100);
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    uint64_t time = calibrate(&m);
    assert(interaction_set_motion(&m, true));
    align_mouse_output(&m, &time);
    for (size_t i = 0; i < 1000; ++i) {
        const input_delta_t delta = motion(&m, &time, 10000, 1.2f, -1.8f);
        assert(delta.x == 0 && delta.y == 0); /* Corrected stationary bias/deadzone. */
    }
    assert(motion(&m, &time, 50001, 26, -2).x == 0);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    for (unsigned i = 0; i < 13; ++i)
        assert(motion(&m, &time, 10000, 1, -2).x == 0);
    assert(m.gravity_ready); /* A gap needs fresh still evidence, with no catch-up. */
    assert(motion(&m, &time, 10000, 26, -2).x == 1);
    interaction_imu_sample_t invalid = sample(time + 10000, NAN, -2);
    assert(interaction_motion_sample(&m, &invalid).x == 0);
    invalid.gx_dps = 2001;
    assert(interaction_motion_sample(&m, &invalid).x == 0);

    setup(&m, INPUT_KEYBOARD);
    time = calibrate(&m);
    for (size_t i = 0; i < 3; ++i) {
        const input_delta_t delta = motion(&m, &time, 50000, 21, -2);
        assert(delta.x == 0 && delta.y == 0 && m.view.focus == 4);
    }
    assert(motion(&m, &time, 50000, 21, -2).x == 0 && m.view.focus == 5);
    for (size_t i = 0; i < 6; ++i) {
        (void)motion(&m, &time, 50000, -4, -2);
        assert(m.view.focus == 5); /* Hysteresis retains focus above the return threshold. */
    }
    (void)motion(&m, &time, 50000, -4, -2);
    assert(m.view.focus == 5);
    (void)motion(&m, &time, 50000, -4, -2);
    assert(m.view.focus == 4);
    for (size_t i = 0; i < 4; ++i) (void)motion(&m, &time, 50000, 21, -2);
    assert(m.view.focus == 5 && m.view.preview_length == 0);
    assert(interaction_confirm(&m));
    assert(m.view.group == 5 && m.view.focus == 1 && m.view.preview_length == 0);
    assert(interaction_select_cell(&m, 0));
    assert(strcmp(m.view.preview, "U") == 0 && m.view.focus == 4 && m.view.group == -1);
    for (size_t i = 0; i < 100; ++i) (void)motion(&m, &time, 10000, 101, 98);
    assert(strcmp(m.view.preview, "U") == 0); /* Dwell/motion never confirms. */
    const unsigned int focus = m.view.focus;
    interaction_set_keyboard_motion(&m, false);
    for (size_t i = 0; i < 100; ++i) (void)motion(&m, &time, 10000, -99, -102);
    assert(m.view.focus == focus);
}

static void third_row_and_empty_cell_geometry(void)
{
    interaction_t m;
    setup(&m, INPUT_KEYBOARD);
    uint64_t time = calibrate(&m);
    for (size_t i = 0; i < 3; ++i) {
        const input_delta_t delta = motion(&m, &time, 50000, 1, 18);
        assert(delta.x == 0 && delta.y == 0 && m.view.focus == 4);
    }
    assert(motion(&m, &time, 50000, 1, 18).y == 0 && m.view.focus == 7);
    assert(strcmp(m.view.cells[7], "2345") == 0);
    for (size_t i = 0; i < 7; ++i) {
        (void)motion(&m, &time, 50000, 1, -7);
        assert(m.view.focus == 7);
    }
    (void)motion(&m, &time, 50000, 1, -7);
    assert(m.view.focus == 4); /* Third-row return has the same Schmitt margin. */

    assert(interaction_select_cell(&m, 0));
    assert(m.view.focus == 1);
    for (size_t i = 0; i < 7; ++i) {
        (void)motion(&m, &time, 50000, 1, 18);
        assert(m.view.focus == 1);
    }
    (void)motion(&m, &time, 50000, 1, 18);
    assert(m.view.focus == 3 && strcmp(m.view.cells[3], "D") == 0);
    for (size_t i = 0; i < 13; ++i) {
        (void)motion(&m, &time, 50000, 1, -7);
        assert(m.view.focus == 3);
    }
    (void)motion(&m, &time, 50000, 1, -7);
    assert(m.view.focus == 1); /* Nearest available diagonal centers also have hysteresis. */
    interaction_back(&m);
    focus_cell(&m, &time, 8);
    assert(interaction_confirm(&m));
    assert(m.view.group == 8 && m.view.focus == 1 && strcmp(m.view.cells[3], "9") == 0);
    focus_cell(&m, &time, 3);
    assert(interaction_confirm(&m));
    assert(strcmp(m.view.preview, "9") == 0 && !m.view.sending && m.view.focus == 4);

    assert(interaction_set_page(&m, INTERACTION_SYMBOLS));
    focus_cell(&m, &time, 8);
    assert(interaction_confirm(&m));
    assert(m.view.group == 8 && m.view.focus == 1 && strcmp(m.view.cells[2], "^") == 0);
    for (unsigned int cell = 3; cell < 9; ++cell) assert(!interaction_select_cell(&m, cell));
    focus_cell(&m, &time, 2);
    assert(interaction_confirm(&m) && strcmp(m.view.preview, "9^") == 0);

    const float directions[][2] = {{201, 198}, {-199, 198}, {-199, -202}, {201, -202}};
    for (unsigned int page = 0; page < 2; ++page) {
        assert(interaction_set_page(&m, page ? INTERACTION_SYMBOLS : INTERACTION_LETTERS));
        assert(interaction_clear(&m));
        for (unsigned int group = 0; group < 9; ++group) {
            assert(interaction_select_cell(&m, group));
            const size_t count = page && group >= 5 ? 3U : 4U;
            for (size_t direction = 0; direction < 4; ++direction) {
                for (size_t step = 0; step < 30; ++step) {
                    const input_delta_t delta = motion(&m, &time, 10000,
                        directions[direction][0], directions[direction][1]);
                    assert(delta.x == 0 && delta.y == 0);
                    assert_valid_focus(&m);
                    assert(m.view.focus < count && m.view.preview_length == 0);
                }
            }
            interaction_back(&m);
            assert(m.view.focus == 4);
            assert(motion(&m, &time, 10000, 1, -2).x == 0 && m.view.focus == 4);
        }
    }
}

static void gyro_children_and_context_reset(void)
{
    interaction_t m;
    setup(&m, INPUT_KEYBOARD);
    uint64_t time = calibrate(&m);
    for (unsigned int page = 0; page < 2; ++page) {
        assert(interaction_set_page(&m, page ? INTERACTION_SYMBOLS : INTERACTION_LETTERS));
        assert(interaction_clear(&m));
        for (unsigned int group = 0; group < 9; ++group) {
            const unsigned int count = page && group >= 5 ? 3U : 4U;
            for (unsigned int target = 0; target < count; ++target) {
                assert(interaction_select_cell(&m, group));
                const char expected = m.view.cells[target][0];
                focus_cell(&m, &time, target);
                const size_t before = m.view.preview_length;
                assert(interaction_confirm(&m));
                assert(m.view.preview_length == before + 1 && m.view.preview[before] == expected);
                assert(m.view.group == -1 && m.view.focus == 4);
            }
        }
    }
    focus_cell(&m, &time, 8);
    assert(interaction_confirm(&m));
    focus_cell(&m, &time, 2);
    assert(interaction_set_page(&m, INTERACTION_LETTERS));
    assert(m.view.group == -1 && m.view.focus == 4 && strcmp(m.view.cells[8], "6789") == 0);
    assert(motion(&m, &time, 10000, 1, -2).x == 0 && m.view.focus == 4);
    assert(interaction_select_cell(&m, 8));
    focus_cell(&m, &time, 3);
    input_state_t state = m.control;
    input_invalidate(&state);
    assert(input_ack_release(&state, state.epoch));
    const size_t retained = m.view.preview_length;
    interaction_sync(&m, &state, true, time / 1000);
    for (size_t i = 0; i < 20; ++i) {
        (void)motion(&m, &time, 10000, 1, -2);
        assert(m.view.group == -1 && m.view.focus == 4 && m.view.preview_length == retained);
    }
}

static void calibration_rejection(void)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    for (size_t i = 0; i < 100; ++i) {
        interaction_imu_sample_t s = sample((uint64_t)i * 10000, 1, -2);
        s.az_g = 2;
        (void)interaction_motion_sample(&m, &s);
    }
    assert(!m.view.calibrated && m.view.calibration_samples == 0);
    interaction_calibrate(&m);
    interaction_imu_sample_t tilted = sample(0, 1, -2);
    (void)interaction_motion_sample(&m, &tilted);
    tilted.timestamp_us = 10000;
    tilted.ax_g = .1f;
    tilted.az_g = sqrtf(.99f);
    (void)interaction_motion_sample(&m, &tilted);
    assert(m.view.calibration_samples == 0); /* Constant norm cannot hide changing orientation. */
    for (size_t i = 0; i < 64; ++i) {
        const interaction_imu_sample_t moving = sample(20000 + (uint64_t)i * 10000, 6, -2);
        (void)interaction_motion_sample(&m, &moving);
    }
    assert(!m.view.calibrated && m.view.calibration_samples == 0);
    interaction_calibrate(&m);
    for (size_t i = 0; i < 64; ++i) {
        const interaction_imu_sample_t s = sample((uint64_t)i * 10000, i % 2 ? 2 : 0, -2);
        (void)interaction_motion_sample(&m, &s);
    }
    assert(!m.view.calibrated); /* A low mean does not conceal gyro variance. */
    interaction_calibrate(&m);
    for (size_t i = 0; i < 500; ++i) {
        const interaction_imu_sample_t s = sample((uint64_t)i * 1000, 1, -2);
        (void)interaction_motion_sample(&m, &s);
    }
    assert(!m.view.calibrated); /* Samples alone cannot bypass the time window. */
    const interaction_imu_sample_t final = sample(500000, 1, -2);
    (void)interaction_motion_sample(&m, &final);
    assert(m.view.calibrated);
}

static void suppression_and_touch(void)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    uint64_t time = calibrate(&m);
    assert(interaction_set_motion(&m, true));
    align_mouse_output(&m, &time);
    assert(motion(&m, &time, 10000, 26, -2).x == 1);
    interaction_set_suppressed(&m, true, false, false, time / 1000);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(interaction_set_touch_divisor(&m, 4));
    for (size_t i = 0; i < 3; ++i) assert(interaction_touch_move(&m, 1, -1).x == 0);
    input_delta_t touch = interaction_touch_move(&m, 1, -1);
    assert(touch.x == 1 && touch.y == -1);
    assert(interaction_set_touch_divisor(&m, 8));
    for (size_t i = 0; i < 7; ++i) assert(interaction_touch_move(&m, 1, 0).x == 0);
    assert(interaction_touch_move(&m, 1, 0).x == 1);
    assert(!interaction_set_touch_divisor(&m, 3));
    assert(interaction_touch_move(&m, NAN, 1).x == 0);
    assert(interaction_touch_move(&m, 4097, 0).x == 0);
    assert(interaction_touch_move(&m, 4096, -4096).x == 127);
    interaction_set_suppressed(&m, false, false, false, time / 1000);
    for (size_t i = 0; i < 14; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(motion(&m, &time, 10000, 26, -2).x == 1);
    assert(m.view.motion_enabled);
    interaction_sync(&m, &m.control, false, time / 1000);
    assert(motion(&m, &time, 10000, 26, -2).x == 0 && m.view.motion_enabled);
    interaction_sync(&m, &m.control, true, time / 1000);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(motion(&m, &time, 10000, 26, -2).x == 1); /* Short backpressure does not disable intent. */
    assert(interaction_button_update(&m, true, time / 1000) == INTERACTION_EVENT_DOWN);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(interaction_button_update(&m, false, time / 1000) == 0);
    interaction_set_suppressed(&m, false, true, false, time / 1000);
    assert(!m.view.motion_enabled && !interaction_set_motion(&m, true));
    interaction_set_suppressed(&m, false, false, false, time / 1000);
    for (size_t i = 0; i < 20; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(interaction_set_motion(&m, true));
    align_mouse_output(&m, &time);
    interaction_set_suppressed(&m, false, false, true, time / 1000);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(m.view.motion_enabled);
    input_state_t state = m.control;
    assert(input_set_mode(&state, INPUT_AUDIO));
    assert(input_ack_release(&state, state.epoch));
    interaction_sync(&m, &state, true, time / 1000);
    assert(!m.view.motion_enabled);
    assert(input_set_mode(&state, INPUT_MOUSE));
    assert(input_ack_release(&state, state.epoch));
    interaction_sync(&m, &state, true, time / 1000);
    assert(!m.view.motion_enabled); /* Returning to Mouse never restores old intent. */
}

static void asynchronous_freeze(void)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    uint64_t time = calibrate(&m);
    interaction_config_t config = m.config;
    config.pointer_gain_x = 1;
    assert(interaction_configure(&m, &config));
    assert(interaction_set_motion(&m, true));
    align_mouse_output(&m, &time);
    const uint64_t deadline = time / 1000 + 70;
    for (size_t i = 0; i < 3; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    interaction_freeze_until(&m, deadline);
    assert(interaction_tick(&m, 100) == 0 && m.view.motion_enabled); /* No clock observation. */
    for (size_t i = 0; i < 3; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    for (size_t i = 0; i < 3; ++i) {
        interaction_freeze_until(&m, deadline); /* Repeated delivery must preserve new fractional carry. */
        assert(motion(&m, &time, 10000, 26, -2).x == 0);
    }
    interaction_freeze_until(&m, deadline - 1);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(motion(&m, &time, 10000, 26, -2).x == 1);
    assert(m.view.motion_enabled && !m.view.touch_active);
    interaction_set_suppressed(&m, true, false, false, time / 1000);
    assert(interaction_set_touch_divisor(&m, 4));
    for (size_t i = 0; i < 3; ++i) assert(interaction_touch_move(&m, 1, 0).x == 0);
    interaction_freeze_until(&m, m.freeze_until_ms + 1);
    assert(interaction_touch_move(&m, 1, 0).x == 0); /* Old touch carry is also discarded. */
    assert(m.view.touch_active && m.view.motion_enabled);
}

static void canonical_boundaries(void)
{
    for (unsigned int change = 0; change < 5; ++change) {
        interaction_t m;
        setup(&m, INPUT_KEYBOARD);
        assert(interaction_append(&m, 'a') && interaction_append(&m, 'b'));
        const uint64_t token = interaction_send_begin(&m);
        assert(token);
        assert(interaction_button_update(&m, true, 100) == INTERACTION_EVENT_DOWN);
        assert(interaction_button_update(&m, false, 110) == 0);
        input_state_t state = m.control;
        switch (change) {
        case 0: assert(input_set_mode(&state, INPUT_MOUSE)); break;
        case 1: input_set_connected(&state, false); break;
        case 2: input_set_owner(&state, INPUT_REMOTE); break;
        case 3: input_set_locked(&state, true); break;
        default: input_invalidate(&state); break;
        }
        interaction_sync(&m, &state, false, 200);
        assert(interaction_tick(&m, 1000) == 0);
        assert(!m.view.motion_enabled && m.view.group == -1 && m.view.focus == 4);
        assert(!m.view.sending && m.view.send_settling);
        assert(interaction_send_update(&m, token, 1, false));
        assert(strcmp(m.view.preview, "b") == 0);
        assert(!m.view.sending && !m.view.send_settling);
    }
}

static void invalid_configuration_and_samples(void)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    const interaction_config_t valid = m.config;
    interaction_config_t bad = valid;
    bad.pointer_gain_x = NAN;
    assert(!interaction_configure(&m, &bad));
    bad = valid; bad.pointer_gain_x = -1;
    assert(!interaction_configure(&m, &bad));
    bad = valid; bad.deadzone_dps = INFINITY;
    assert(!interaction_configure(&m, &bad));
    bad = valid; bad.filter_seconds = -1;
    assert(!interaction_configure(&m, &bad));
    bad = valid; bad.selector_cell_degrees = 0;
    assert(!interaction_configure(&m, &bad));
    bad = valid; bad.selector_hysteresis_degrees = bad.selector_cell_degrees / 2;
    assert(!interaction_configure(&m, &bad));
    assert(!interaction_configure(&m, NULL));
    uint64_t time = calibrate(&m);
    assert(interaction_set_motion(&m, true));
    align_mouse_output(&m, &time);
    const interaction_imu_sample_t duplicate = sample(time, 26, -2);
    assert(interaction_motion_sample(&m, &duplicate).x == 0);
    const interaction_imu_sample_t backward = sample(time - 1, 26, -2);
    assert(interaction_motion_sample(&m, &backward).x == 0);
    interaction_imu_sample_t invalid = sample(time + 10000, 26, -2);
    invalid.az_g = INFINITY;
    assert(interaction_motion_sample(&m, &invalid).x == 0);
    invalid.az_g = 17;
    assert(interaction_motion_sample(&m, &invalid).x == 0);
}

static interaction_touch_result_t pad(interaction_t *m, interaction_touch_phase_t phase,
    uint64_t gesture, uint64_t event_ms, uint64_t now_ms, float x, float y, bool allowed)
{
    const interaction_touch_event_t event = {.phase = phase, .gesture = gesture,
        .epoch = m->control.epoch, .ui_revision = 7, .timestamp_ms = event_ms, .x = x, .y = y};
    return interaction_touch_input(m, &event, now_ms, 7, allowed);
}

static void admit_pad(interaction_t *m, interaction_touch_result_t result,
    interaction_touch_effect_t expected)
{
    assert(result.effect == expected && result.token && !result.allow_motion);
    assert(interaction_touch_admit(m, result.token, true).effect == INTERACTION_TOUCH_NONE);
}

static void first_pad_tap(interaction_t *m)
{
    assert(pad(m, INTERACTION_TOUCH_BEGIN, 1, 100, 100, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    admit_pad(m, pad(m, INTERACTION_TOUCH_END, 1, 120, 120, 100, 100, true), INTERACTION_TOUCH_CLICK);
}

static void held_pad(interaction_t *m)
{
    setup(m, INPUT_MOUSE);
    first_pad_tap(m);
    admit_pad(m, pad(m, INTERACTION_TOUCH_BEGIN, 2, 200, 200, 100, 100, true), INTERACTION_TOUCH_LEFT_DOWN);
    assert(interaction_touch_holding(m));
}

static void pad_single_double_and_drag_effects(void)
{
    for (unsigned drag = 0; drag < 2; ++drag) {
        interaction_t m; held_pad(&m);
        if (drag) {
            interaction_touch_result_t result = pad(&m, INTERACTION_TOUCH_MOVE, 2, 210, 210, 20, 7, true);
            assert(result.effect == INTERACTION_TOUCH_NONE && result.allow_motion);
            interaction_set_suppressed(&m, true, false, false, 210);
            input_delta_t delta = interaction_touch_move(&m, 20, 7);
            assert(delta.x == 20 && delta.y == 7);
            assert(pad(&m, INTERACTION_TOUCH_MOVE, 2, 220, 220, 0, 0, true).effect == INTERACTION_TOUCH_NONE);
        }
        admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 2, 230, 230,
            drag ? 120.0f : 100.0f, drag ? 107.0f : 100.0f, true), INTERACTION_TOUCH_LEFT_UP);
        assert(!interaction_touch_holding(&m));
        assert(pad(&m, INTERACTION_TOUCH_END, 2, 240, 240, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
        assert(interaction_touch_tick(&m, 600, 7).effect == INTERACTION_TOUCH_NONE);
        assert(pad(&m, INTERACTION_TOUCH_BEGIN, 3, 700, 700, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
        admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 3, 720, 720, 100, 100, true), INTERACTION_TOUCH_CLICK);
    }
}

static void pad_timing_distance_and_raw_slop_boundaries(void)
{
    const unsigned divisors[] = {1,4,8};
    for (size_t divisor = 0; divisor < sizeof divisors / sizeof divisors[0]; ++divisor)
        for (unsigned outside = 0; outside < 2; ++outside) {
            interaction_t m; setup(&m, INPUT_MOUSE);
            assert(interaction_set_touch_divisor(&m, divisors[divisor]));
            (void)pad(&m, INTERACTION_TOUCH_BEGIN, 1, 100, 100, 100, 100, true);
            float distance = outside ? 8.01f : 8.0f;
            assert(pad(&m, INTERACTION_TOUCH_MOVE, 1, 110, 110, distance, 0, true).allow_motion);
            (void)pad(&m, INTERACTION_TOUCH_MOVE, 1, 120, 120, -distance, 0, true);
            interaction_touch_result_t result = pad(&m, INTERACTION_TOUCH_END, 1, 130, 130, 100, 100, true);
            if (outside) assert(result.effect == INTERACTION_TOUCH_NONE);
            else admit_pad(&m, result, INTERACTION_TOUCH_CLICK);
        }
    for (unsigned late = 0; late < 2; ++late) {
        interaction_t m; setup(&m, INPUT_MOUSE);
        (void)pad(&m, INTERACTION_TOUCH_BEGIN, 1, 100, 100, 100, 100, true);
        interaction_touch_result_t result = pad(&m, INTERACTION_TOUCH_END, 1,
            350U + late, 350U + late, 100, 100, true);
        if (late) assert(result.effect == INTERACTION_TOUCH_NONE);
        else admit_pad(&m, result, INTERACTION_TOUCH_CLICK);
    }
    for (unsigned late = 0; late < 2; ++late) {
        interaction_t m; setup(&m, INPUT_MOUSE); first_pad_tap(&m);
        interaction_touch_result_t result = pad(&m, INTERACTION_TOUCH_BEGIN, 2,
            400U + late, 400U + late, 116, 100, true);
        if (late) assert(result.effect == INTERACTION_TOUCH_NONE);
        else admit_pad(&m, result, INTERACTION_TOUCH_LEFT_DOWN);
    }
    interaction_t m; setup(&m, INPUT_MOUSE); first_pad_tap(&m);
    assert(pad(&m, INTERACTION_TOUCH_BEGIN, 2, 200, 200, 116.01f, 100, true).effect == INTERACTION_TOUCH_NONE);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 116.01f, 100, true), INTERACTION_TOUCH_CLICK);
}

static void pad_movement_only_and_external_drag_never_click(void)
{
    for (unsigned external = 0; external < 2; ++external) {
        interaction_t m; setup(&m, INPUT_MOUSE);
        interaction_touch_config_t config = m.touch_config;
        config.tap_to_click = external != 0;
        assert(interaction_touch_configure(&m, &config));
        (void)pad(&m, INTERACTION_TOUCH_BEGIN, 1, 100, 100, 100, 100, external == 0);
        interaction_touch_result_t result = pad(&m, INTERACTION_TOUCH_MOVE, 1, 110, 110, 20, 0, external == 0);
        assert(result.allow_motion && result.effect == INTERACTION_TOUCH_NONE);
        assert(pad(&m, INTERACTION_TOUCH_END, 1, 120, 120, 120, 100, external == 0).effect == INTERACTION_TOUCH_NONE);
        (void)pad(&m, INTERACTION_TOUCH_BEGIN, 2, 200, 200, 120, 100, external == 0);
        assert(pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 120, 100, external == 0).effect == INTERACTION_TOUCH_NONE);
        assert(!interaction_touch_holding(&m));
    }
    interaction_t m; held_pad(&m);
    interaction_touch_config_t config = m.touch_config; config.tap_to_click = false;
    assert(interaction_touch_configure(&m, &config));
    interaction_touch_result_t release = interaction_touch_tick(&m, 210, 7);
    assert(release.effect == INTERACTION_TOUCH_RELEASE_REQUIRED && interaction_touch_holding(&m));
    admit_pad(&m, release, INTERACTION_TOUCH_RELEASE_REQUIRED);
    assert(pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    assert(!interaction_touch_holding(&m));
}

static void pad_admission_rejection_and_cleanup_latch(void)
{
    interaction_t m; setup(&m, INPUT_MOUSE);
    (void)pad(&m, INTERACTION_TOUCH_BEGIN, 1, 100, 100, 100, 100, true);
    interaction_touch_result_t click = pad(&m, INTERACTION_TOUCH_END, 1, 120, 120, 100, 100, true);
    assert(click.effect == INTERACTION_TOUCH_CLICK);
    assert(interaction_touch_admit(&m, click.token, false).effect == INTERACTION_TOUCH_NONE);
    assert(pad(&m, INTERACTION_TOUCH_BEGIN, 2, 200, 200, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 100, 100, true), INTERACTION_TOUCH_CLICK);
    interaction_touch_result_t down = pad(&m, INTERACTION_TOUCH_BEGIN, 3, 300, 300, 100, 100, true);
    assert(down.effect == INTERACTION_TOUCH_LEFT_DOWN);
    assert(interaction_touch_admit(&m, down.token, false).effect == INTERACTION_TOUCH_NONE);
    assert(!interaction_touch_holding(&m));
    assert(!pad(&m, INTERACTION_TOUCH_MOVE, 3, 310, 310, 20, 0, true).allow_motion);
    assert(pad(&m, INTERACTION_TOUCH_END, 3, 320, 320, 120, 100, true).effect == INTERACTION_TOUCH_NONE);
    assert(interaction_touch_tick(&m, 500, 7).effect == INTERACTION_TOUCH_NONE);
    held_pad(&m);
    interaction_touch_result_t up = pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 100, 100, true);
    interaction_touch_result_t release = interaction_touch_admit(&m, up.token, false);
    assert(release.effect == INTERACTION_TOUCH_RELEASE_REQUIRED && release.token != up.token);
    assert(interaction_touch_admit(&m, up.token, true).effect == INTERACTION_TOUCH_NONE);
    assert(interaction_touch_holding(&m));
    assert(interaction_touch_admit(&m, release.token, false).token == release.token);
    assert(interaction_touch_tick(&m, 500, 7).token == release.token);
    admit_pad(&m, release, INTERACTION_TOUCH_RELEASE_REQUIRED);
    assert(!interaction_touch_holding(&m));
}

static void pad_context_changes_and_congestion_release(void)
{
    for (unsigned boundary = 0; boundary < 7; ++boundary) {
        interaction_t m; held_pad(&m);
        input_state_t state = m.control;
        if (boundary == 0) input_set_locked(&state, true);
        if (boundary == 1) assert(input_set_mode(&state, INPUT_KEYBOARD));
        if (boundary == 2) input_set_owner(&state, INPUT_REMOTE);
        if (boundary == 3) input_set_connected(&state, false);
        if (boundary == 4) input_invalidate(&state);
        if (boundary == 5) interaction_set_suppressed(&m, true, true, false, 210);
        interaction_sync(&m, &state, true, 210);
        interaction_touch_result_t result = interaction_touch_tick(&m, 210, boundary == 6 ? 8 : 7);
        assert(result.effect == INTERACTION_TOUCH_RELEASE_REQUIRED);
        admit_pad(&m, result, INTERACTION_TOUCH_RELEASE_REQUIRED);
        assert(!interaction_touch_holding(&m));
        assert(pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    }
    interaction_t m; held_pad(&m);
    interaction_sync(&m, &m.control, false, 210);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 100, 100, true), INTERACTION_TOUCH_LEFT_UP);
    assert(!interaction_touch_holding(&m));
}

static void pad_heartbeat_deadlines_and_old_gesture_isolation(void)
{
    interaction_t m; held_pad(&m);
    assert(pad(&m, INTERACTION_TOUCH_CANCEL, 1, 1190, 1190, 0, 0, true).effect == INTERACTION_TOUCH_NONE);
    assert(interaction_touch_tick(&m, 1199, 7).effect == INTERACTION_TOUCH_NONE);
    admit_pad(&m, interaction_touch_tick(&m, 1200, 7), INTERACTION_TOUCH_RELEASE_REQUIRED);
    assert(!interaction_touch_holding(&m));
    held_pad(&m);
    interaction_touch_config_t config = m.touch_config;
    config.drag_max_ms = 1000;
    assert(interaction_touch_configure(&m, &config));
    admit_pad(&m, interaction_touch_tick(&m, 200, 7), INTERACTION_TOUCH_RELEASE_REQUIRED);
    /* Configure before a new gesture; no restoration of the cancelled hold. */
    assert(pad(&m, INTERACTION_TOUCH_BEGIN, 3, 300, 300, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 3, 320, 320, 100, 100, true), INTERACTION_TOUCH_CLICK);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_BEGIN, 4, 400, 400, 100, 100, true), INTERACTION_TOUCH_LEFT_DOWN);
    for (uint64_t now = 450; now < 1400; now += 50)
        assert(pad(&m, INTERACTION_TOUCH_MOVE, 4, now, now, 0, 0, true).effect == INTERACTION_TOUCH_NONE);
    assert(interaction_touch_tick(&m, 1399, 7).effect == INTERACTION_TOUCH_NONE);
    admit_pad(&m, interaction_touch_tick(&m, 1400, 7), INTERACTION_TOUCH_RELEASE_REQUIRED);
}

static void pad_event_time_staleness_clock_and_malformed_input(void)
{
    interaction_t m; setup(&m, INPUT_MOUSE);
    (void)pad(&m, INTERACTION_TOUCH_BEGIN, 1, 100, 500, 100, 100, true);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 1, 120, 500, 100, 100, true), INTERACTION_TOUCH_CLICK);
    /* Native timestamps, not processing delay, decide the double-tap window. */
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_BEGIN, 2, 200, 500, 100, 100, true), INTERACTION_TOUCH_LEFT_DOWN);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 2, 220, 500, 100, 100, true), INTERACTION_TOUCH_LEFT_UP);
    setup(&m, INPUT_MOUSE);
    assert(pad(&m, INTERACTION_TOUCH_BEGIN, 1, 100, 1100, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    assert(pad(&m, INTERACTION_TOUCH_END, 1, 120, 1100, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    for (unsigned fault = 0; fault < 6; ++fault) {
        held_pad(&m);
        interaction_touch_event_t event = {.phase = INTERACTION_TOUCH_MOVE, .gesture = 2,
            .epoch = m.control.epoch, .ui_revision = 7, .timestamp_ms = 210};
        if (fault == 0) --event.epoch;
        if (fault == 1) --event.ui_revision;
        if (fault == 2) event.timestamp_ms = 211;
        if (fault == 3) event.timestamp_ms = 199;
        if (fault == 4) event.x = NAN;
        if (fault == 5) event.y = 4097;
        admit_pad(&m, interaction_touch_input(&m, &event, 210, 7, true), INTERACTION_TOUCH_RELEASE_REQUIRED);
    }
    held_pad(&m);
    admit_pad(&m, interaction_touch_tick(&m, 199, 7), INTERACTION_TOUCH_RELEASE_REQUIRED);
    held_pad(&m);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_MOVE, 2, 210, 210, 0, 0, false), INTERACTION_TOUCH_RELEASE_REQUIRED);
    setup(&m, INPUT_MOUSE);
    (void)pad(&m, INTERACTION_TOUCH_BEGIN, 1, UINT64_MAX - 100, UINT64_MAX - 100, 100, 100, true);
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 1, UINT64_MAX, UINT64_MAX, 100, 100, true), INTERACTION_TOUCH_CLICK);
}

static void pad_configuration_and_token_exhaustion(void)
{
    interaction_t m; held_pad(&m);
    const interaction_touch_config_t valid = m.touch_config;
    for (unsigned fault = 0; fault < 12; ++fault) {
        interaction_touch_config_t bad = valid;
        if (fault == 0) bad.tap_max_ms = 49;
        if (fault == 1) bad.tap_max_ms = 501;
        if (fault == 2) bad.double_window_ms = 99;
        if (fault == 3) bad.double_window_ms = 501;
        if (fault == 4) bad.contact_timeout_ms = 99;
        if (fault == 5) bad.contact_timeout_ms = 2001;
        if (fault == 6) bad.drag_max_ms = 999;
        if (fault == 7) bad.drag_max_ms = 60001;
        if (fault == 8) bad.tap_slop_px = NAN;
        if (fault == 9) bad.tap_slop_px = 1;
        if (fault == 10) bad.double_slop_px = 49;
        if (fault == 11) bad.double_slop_px = 7;
        assert(!interaction_touch_configure(&m, &bad));
        assert(m.touch_config.tap_to_click && m.touch_config.tap_slop_px == 8 && interaction_touch_holding(&m));
    }
    assert(interaction_touch_configure(&m, &valid));
    assert(interaction_touch_tick(&m, 210, 7).effect == INTERACTION_TOUCH_NONE);
    m.touch.serial = UINT64_MAX - 1;
    admit_pad(&m, pad(&m, INTERACTION_TOUCH_END, 2, 220, 220, 100, 100, true), INTERACTION_TOUCH_RELEASE_REQUIRED);
    assert(m.touch.exhausted && !interaction_touch_holding(&m));
    assert(pad(&m, INTERACTION_TOUCH_BEGIN, 3, 300, 300, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
    assert(pad(&m, INTERACTION_TOUCH_END, 3, 320, 320, 100, 100, true).effect == INTERACTION_TOUCH_NONE);
}

static input_delta_t physical_sample(interaction_t *m, uint64_t *time, uint64_t interval,
                                     float x, float y, float z, float ax, float ay, float az)
{
    *time += interval;
    const interaction_imu_sample_t s = {.timestamp_us = *time,
        .gx_dps = x, .gy_dps = y, .gz_dps = z, .ax_g = ax, .ay_g = ay, .az_g = az};
    return interaction_motion_sample(m, &s);
}

static uint64_t physical_grip(interaction_t *m, float tilt)
{
    setup(m, INPUT_MOUSE);
    uint64_t time = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const input_delta_t d = physical_sample(m, &time, 10000, 0, 0, 0, 0, sinf(tilt), cosf(tilt));
        assert(d.x == 0 && d.y == 0);
    }
    assert(m->view.calibrated && m->gravity_ready);
    assert(interaction_set_motion(m, true));
    for (unsigned i = 0; i < 12; ++i) {
        const input_delta_t d = physical_sample(m, &time, 10000, 0, 0, 0, 0, sinf(tilt), cosf(tilt));
        assert(d.x == 0 && d.y == 0);
    }
    assert(m->gravity_ready);
    return time;
}

static void auto_grip_projection_and_independent_gains(void)
{
    const uint64_t intervals[] = {5000, 10000, 20000};
    for (unsigned grip = 0; grip <= 6; ++grip) {
        const float tilt = (float)grip * 1.57079632679f / 6;
        for (size_t rate = 0; rate < sizeof intervals / sizeof intervals[0]; ++rate) {
            for (int direction = -1; direction <= 1; direction += 2) {
                interaction_t m;
                uint64_t time = physical_grip(&m, tilt);
                interaction_config_t config = m.config;
                config.pointer_gain_x = 8;
                config.pointer_gain_y = 2;
                config.invert_x = direction < 0;
                assert(interaction_configure(&m, &config));
                (void)physical_sample(&m, &time, intervals[rate], 0, 0, 0, 0, sinf(tilt), cosf(tilt));
                int total_x = 0, total_y = 0;
                const float yaw = 25 * (float)direction;
                for (uint64_t elapsed = 0; elapsed < 1000000; elapsed += intervals[rate]) {
                    const input_delta_t d = physical_sample(&m, &time, intervals[rate], 0,
                        yaw * sinf(tilt), yaw * cosf(tilt), 0, sinf(tilt), cosf(tilt));
                    total_x += d.x; total_y += d.y;
                }
                assert(total_x >= 199 && total_x <= 200 && total_y == 0);
                time = physical_grip(&m, tilt);
                config = m.config;
                config.pointer_gain_x = 8;
                config.pointer_gain_y = 2;
                config.invert_y = direction < 0;
                assert(interaction_configure(&m, &config));
                (void)physical_sample(&m, &time, intervals[rate], 0, 0, 0, 0, sinf(tilt), cosf(tilt));
                total_x = 0; total_y = 0;
                for (uint64_t elapsed = intervals[rate]; elapsed <= 1000000; elapsed += intervals[rate]) {
                    const float pitch = tilt + (float)direction * 25 * .017453292519943295f *
                        (float)elapsed / 1000000;
                    const input_delta_t d = physical_sample(&m, &time, intervals[rate],
                        25 * (float)direction, 0, 0, 0, sinf(pitch), cosf(pitch));
                    total_x += d.x; total_y += d.y;
                }
                assert(total_y >= 49 && total_y <= 50 && total_x == 0);
            }
        }
    }
}

static void aiming_turns_stop_and_return_across_angle_wrap(void)
{
    const float rates[] = {.75f, 250};
    const unsigned counts[] = {500, 160};
    for (size_t speed = 0; speed < sizeof rates / sizeof rates[0]; ++speed) {
        interaction_t m;
        uint64_t time = physical_grip(&m, 0);
        int displacement = 0;
        for (unsigned direction = 0; direction < 2; ++direction) {
            const float rate = direction ? -rates[speed] : rates[speed];
            for (unsigned i = 0; i < counts[speed]; ++i) {
                const input_delta_t delta = physical_sample(&m, &time, 10000, 0, 0, rate, 0, 0, 1);
                assert(m.gravity_ready && delta.y == 0 && (direction ? delta.x <= 0 : delta.x >= 0));
                assert(delta.x >= -10 && delta.x <= 10);
                displacement += delta.x;
            }
            if (!direction) {
                const int expected = speed ? 1600 : 15;
                assert(displacement >= expected - 1 && displacement <= expected);
            }
            for (unsigned i = 0; i < 30; ++i) {
                const input_delta_t stopped = physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
                assert(!stopped.x && !stopped.y);
            }
        }
        assert(displacement >= -1 && displacement <= 1 && fabsf(m.attitude.w) > .9999f);
    }
}

static void aiming_sideways_calibration_and_gravity_only_correction(void)
{
    interaction_t m;
    setup(&m, INPUT_MOUSE);
    uint64_t time = 0;
    for (unsigned i = 0; i < 64; ++i)
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 1, 0, 0);
    assert(interaction_set_motion(&m, true));
    assert(m.gravity_ready && m.aim_pitch[1] == 1 && m.world_up[0] == 1);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 1, 0, 0);
    const input_delta_t yaw = physical_sample(&m, &time, 10000, 50, 0, 0, 1, 0, 0);
    assert(yaw.x == 2 && yaw.y == 0);
    /* A smooth accelerometer drift inside the correction gate can change the
     * attitude estimate. Zero gyro still means zero cursor movement. */
    for (unsigned i = 0; i < 200; ++i) {
        const float angle = (float)i * .0007f;
        const input_delta_t delta = physical_sample(&m, &time, 10000, 0, 0, 0,
            cosf(angle), sinf(angle), 0);
        assert(!delta.x && !delta.y && m.gravity_ready && !m.gravity_predicted);
    }
    assert(fabsf(m.gravity[1]) > .05f && m.world_up[0] == 1 && m.aim_pitch[1] == 1);
}

static void gravity_transients_gaps_and_reacquisition(void)
{
    interaction_t m;
    uint64_t time = physical_grip(&m, 0);
    for (unsigned i = 0; i < 150; ++i) {
        const input_delta_t d = physical_sample(&m, &time, 10000, 0, 0, 25, .8f, 0, .6f);
        assert(d.x == 1 && d.y == 0);
        assert(m.gravity_predicted && m.gravity_ready); /* Norm alone is insufficient. */
    }
    for (unsigned i = 0; i < 150; ++i)
        assert(physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1).x == 0);
    assert(m.gravity_ready && !m.gravity_predicted);
    const float failures[][3] = {{0,0,0}, {0,0,3}, {NAN,0,1}, {0,0,16}};
    for (size_t fault = 0; fault < sizeof failures / sizeof failures[0]; ++fault) {
        const input_delta_t d = physical_sample(&m, &time, 10000, 0, 0, 25,
            failures[fault][0], failures[fault][1], failures[fault][2]);
        assert(d.x == 0 && d.y == 0 && !m.gravity_ready);
        for (unsigned i = 0; i < 15; ++i)
            assert(physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1).x == 0);
        assert(m.gravity_ready);
        assert(physical_sample(&m, &time, 10000, 0, 0, 25, 0, 0, 1).x == 1);
    }
    assert(physical_sample(&m, &time, 50001, 0, 0, 25, 0, 0, 1).x == 0);
    assert(!m.gravity_ready && m.pointer_motion.x == 0);
    for (unsigned i = 0; i < 15; ++i)
        assert(physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1).x == 0);
    interaction_set_suppressed(&m, false, false, true, time / 1000);
    assert(physical_sample(&m, &time, 10000, 0, 0, 100, .8f, 0, .6f).x == 0);
    assert(m.gravity_ready && m.gravity_predicted && m.view.motion_enabled);
    interaction_set_suppressed(&m, false, false, false, time / 1000);
    for (unsigned i = 0; i < 20; ++i)
        assert(physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1).x == 0);
    assert(m.gravity_ready && physical_sample(&m, &time, 10000, 0, 0, 25, 0, 0, 1).x == 1);
    interaction_set_suppressed(&m, false, false, true, time / 1000);
    for (unsigned i = 0; i < 26; ++i)
        assert(physical_sample(&m, &time, 10000, 0, 0, 100, 0, 0, 1).x == 0);
    assert(m.gravity_ready && m.gravity_predicted); /* Gyro propagation continues during vibration. */
}

static void orientation_tracks_during_touch_and_crosses_old_geometry_limit(void)
{
    interaction_t m;
    uint64_t time = physical_grip(&m, 0);
    interaction_set_suppressed(&m, true, false, false, time / 1000);
    for (unsigned i = 1; i <= 100; ++i) {
        const float tilt = (float)i * 1.57079632679f / 100;
        const input_delta_t d = physical_sample(&m, &time, 10000, 90, 0, 0, 0, sinf(tilt), cosf(tilt));
        assert(d.x == 0 && d.y == 0 && m.gravity_ready);
    }
    interaction_set_suppressed(&m, false, false, false, time / 1000);
    for (unsigned i = 0; i < 15; ++i)
        assert(physical_sample(&m, &time, 10000, 0, 0, 0, 0, 1, 0).x == 0);
    assert(physical_sample(&m, &time, 10000, 0, 25, 0, 0, 1, 0).x == 1);
    time = physical_grip(&m, 0);
    for (unsigned i = 1; i <= 200; ++i) {
        const float roll = (float)i * 3.14159265359f / 200;
        const input_delta_t d = physical_sample(&m, &time, 10000, 0, -90, 0,
            sinf(roll), 0, cosf(roll));
        assert(d.x == 0 && d.y == 0);
        assert(m.gravity_ready && m.view.motion_block_reason == INTERACTION_MOTION_BLOCK_NONE);
    }
    assert(physical_sample(&m, &time, 10000, 0, 0, -25, 0, 0, -1).x == 1);
    interaction_config_t config = m.config;
    config.freeze_ms = 1000;
    assert(interaction_configure(&m, &config));
    interaction_freeze_after(&m, UINT64_MAX - 10);
    assert(m.freeze_until_ms == UINT64_MAX && m.pointer_motion.x == 0);
    const interaction_config_t good = m.config;
    for (unsigned fault = 0; fault < 6; ++fault) {
        config = good;
        if (fault == 0) config.pointer_gain_y = INFINITY;
        if (fault == 1) config.pointer_gain_x = .24f;
        if (fault == 2) config.freeze_ms = 1001;
        if (fault == 3) config.selector_axis_x = 0;
        if (fault == 4) config.selector_axis_y = -config.selector_axis_x;
        if (fault == 5) config.selector_axis_x = -128;
        assert(!interaction_configure(&m, &config));
        assert(m.config.freeze_ms == good.freeze_ms);
    }
}

static void calibration_cancellation_requires_a_new_explicit_start(void)
{
    interaction_t m; setup(&m, INPUT_MOUSE);
    for (uint64_t i=0; i<63; ++i) {
        const interaction_imu_sample_t s=sample(i*10000,1,-2);
        (void)interaction_motion_sample(&m,&s);
    }
    assert(m.view.calibrating && m.view.calibration_samples==63);
    interaction_calibration_cancel(&m);
    for (uint64_t i=63; i<200; ++i) {
        const interaction_imu_sample_t s=sample(i*10000,1,-2);
        const input_delta_t d=interaction_motion_sample(&m,&s);
        assert(!d.x && !d.y && !m.view.calibrating && !m.view.calibrated && !m.gravity_ready);
    }
    assert(!m.view.calibration_samples && !m.view.motion_enabled);
    interaction_calibrate(&m);
    for (uint64_t i=200; i<264; ++i) {
        const interaction_imu_sample_t s=sample(i*10000,1,-2);
        (void)interaction_motion_sample(&m,&s);
    }
    assert(m.view.calibrated && !m.view.calibrating && m.gravity_ready && !m.view.motion_enabled);
}

static uint64_t reference_fixture(interaction_t *m, interaction_reference_diagnostics_t *d)
{
    uint64_t time = physical_grip(m, 0);
    interaction_reference_diagnostics_bind(m, d);
    assert(interaction_tick(m, time / 1000) == INTERACTION_EVENT_NONE);
    (void)physical_sample(m, &time, 10000, 0, 0, 0, 0, 0, 1);
    return time;
}

static void reference_incidents_preserve_exact_cause_and_first_evidence(void)
{
    interaction_t m;
    interaction_reference_diagnostics_t d;
    uint64_t time = reference_fixture(&m, &d);
    assert(!d.first_active.known && !d.episode.first_known);
    const uint64_t previous = time;
    const input_delta_t stopped = physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 3);
    assert(!stopped.x && !stopped.y && !m.gravity_ready && m.view.motion_enabled);
    assert(d.first_active.known && d.first_active.reason == INTERACTION_REFERENCE_ACCEL_NORM);
    assert(d.active_losses == 1 && d.losses[INTERACTION_REFERENCE_ACCEL_NORM] == 1);
    const interaction_reference_frame_t *f = &d.first_active.terminal;
    assert(f->sample_known && f->previous_known && f->sample.timestamp_us == time);
    assert(f->previous.timestamp_us == previous && f->previous.az_g == 1 && f->sample.az_g == 3);
    assert(f->gravity_ready && f->accel_seen && f->dt_known && f->norm_known && f->norm == 3);
    assert(f->gravity_before[2] == 1 && f->previous_accel[2] == 1);
    assert(f->checks_known == INTERACTION_REFERENCE_SAFE_NORM && f->checks_pass == 0);
    assert(d.first_active.episode.first_known && d.first_active.episode.rejected == 1);
    const interaction_reference_incident_t frozen = d.first_active;
    (void)interaction_motion_sample(&m, NULL);
    assert(!d.losses[INTERACTION_REFERENCE_MISSING]); /* Already unavailable, not a new loss. */
    for (unsigned i = 0; i < 15; ++i)
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
    assert(m.gravity_ready);
    (void)physical_sample(&m, &time, 60000, 0, 0, 0, 0, 0, 1);
    assert(d.active_losses == 2 && d.losses[INTERACTION_REFERENCE_SAMPLE_TIME] == 1);
    assert(memcmp(&frozen, &d.first_active, sizeof frozen) == 0);
}

static void reference_rejection_episodes_keep_unknown_checks_distinct(void)
{
    for (unsigned cause = 0; cause < 3; ++cause) {
        interaction_t m;
        interaction_reference_diagnostics_t d;
        uint64_t time = reference_fixture(&m, &d);
        const uint64_t trusted = m.gravity_trusted_us;
        for (unsigned i = 0; i < 26; ++i) {
            float ax = 0, az = 1.2f;
            if (cause == 1) { ax = .8f; az = .6f; }
            if (cause == 2) { ax = i % 2 ? -.04f : .04f; az = sqrtf(1 - .04f * .04f); }
            (void)physical_sample(&m, &time, 10000, 0, 0, 0, ax, 0, az);
            assert(m.gravity_ready && m.gravity_predicted && !d.first_active.known);
        }
        const interaction_reference_episode_t *e = &d.episode;
        assert(e->first_known && e->rejected == 26 && !e->saturated && !e->haptic);
        assert(e->first.sample.timestamp_us == trusted + 10000);
        assert(time - m.gravity_trusted_us == 260000);
        assert(!(e->first.checks_known & INTERACTION_REFERENCE_INNOVATION));
        if (cause == 0) {
            assert(e->near_one == 26 && e->gentle == 1 && e->innovation == 0);
            assert(!(d.work.checks_known & INTERACTION_REFERENCE_INNOVATION));
        } else if (cause == 1) {
            assert(e->near_one == 0 && e->gentle == 1 && e->innovation == 25);
            assert(d.work.checks_known & INTERACTION_REFERENCE_INNOVATION);
            assert(!(d.work.checks_pass & INTERACTION_REFERENCE_INNOVATION));
        } else assert(e->near_one == 0 && e->gentle == 26 && e->innovation == 0);
    }
    interaction_t m;
    interaction_reference_diagnostics_t d;
    uint64_t time = reference_fixture(&m, &d);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1.2f);
    assert(d.episode.first_known);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
    assert(!d.episode.first_known && !d.episode.rejected && !d.first_active.known);
    interaction_set_suppressed(&m, false, false, true, time / 1000);
    for (unsigned i = 0; i < 26; ++i)
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
    assert(m.gravity_ready && !d.first_active.known);
    assert(d.episode.haptic == 26 && !d.work.checks_known);
    assert(d.work.context & INTERACTION_REFERENCE_HAPTIC);
}

static void reference_attachment_reset_staleness_and_saturation(void)
{
    interaction_t m;
    interaction_reference_diagnostics_t d;
    uint64_t time = reference_fixture(&m, &d);
    interaction_t copy = m;
    assert(interaction_reference_diagnostics_get(&copy) == NULL);
    const interaction_reference_diagnostics_t saved = d;
    interaction_reference_diagnostics_reset(&copy);
    (void)physical_sample(&copy, &time, 10000, 0, 0, 0, 0, 0, 3);
    assert(memcmp(&saved, &d, sizeof d) == 0);
    d.losses[INTERACTION_REFERENCE_ACCEL_NORM] = UINT32_MAX;
    d.active_losses = UINT32_MAX;
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 3);
    assert(d.saturated && d.active_losses == UINT32_MAX);
    assert(d.losses[INTERACTION_REFERENCE_ACCEL_NORM] == UINT32_MAX);
    copy = m;
    interaction_reference_diagnostics_reset(&m);
    assert(!d.first_active.known && !d.previous_known && !d.active_losses && !d.saturated);
    assert(!d.work.sample_known && !d.work.norm_known && !d.work.checks_known);
    assert(memcmp(&copy, &m, sizeof m) == 0); /* Reset is observation-only. */
    interaction_reference_diagnostics_bind(&m, NULL);
    assert(interaction_reference_diagnostics_get(&m) == NULL);

    time = reference_fixture(&m, &d);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1.2f);
    d.episode.rejected = d.episode.near_one = UINT32_MAX;
    for (unsigned i = 0; i < 25; ++i)
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1.2f);
    assert(d.episode.saturated && d.episode.rejected == UINT32_MAX);
    interaction_init(&m);
    assert(interaction_reference_diagnostics_get(&m) == NULL);
}

static void reference_external_causes_and_inactive_losses_are_truthful(void)
{
    const interaction_reference_reason_t reasons[] = {INTERACTION_REFERENCE_MISSING,
        INTERACTION_REFERENCE_INVALID_SAMPLE, INTERACTION_REFERENCE_SAMPLE_TIME, INTERACTION_REFERENCE_CLOCK};
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; ++i) {
        interaction_t m;
        interaction_reference_diagnostics_t d;
        uint64_t time = reference_fixture(&m, &d);
        if (i == 0) (void)interaction_motion_sample(&m, NULL);
        if (i == 1) (void)physical_sample(&m, &time, 10000, 2000, 0, 0, 0, 0, 1);
        if (i == 2) (void)physical_sample(&m, &time, 50001, 0, 0, 0, 0, 0, 1);
        if (i == 3) (void)interaction_tick(&m, 0);
        assert(d.first_active.known && d.first_active.reason == reasons[i]);
        assert(!d.first_active.episode.first_known && !d.first_active.terminal.dt_known);
        assert(!d.first_active.terminal.norm_known && !d.first_active.terminal.checks_known);
        assert(d.first_active.terminal.sample_known == (i == 1 || i == 2));
    }
    interaction_t m;
    interaction_reference_diagnostics_t d;
    uint64_t time = reference_fixture(&m, &d);
    assert(interaction_set_motion(&m, false));
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 3);
    assert(d.losses[INTERACTION_REFERENCE_ACCEL_NORM] == 1 && !d.first_active.known);
    for (unsigned i = 0; i < 15; ++i)
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
    assert(m.gravity_ready && interaction_set_motion(&m, true));
    for (unsigned i = 0; i < 12; ++i)
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
    assert(m.gravity_ready);
    interaction_calibrate(&m);
    assert(d.losses[INTERACTION_REFERENCE_CALIBRATE] == 1 && !d.first_active.known);
    time = reference_fixture(&m, &d);
    interaction_calibration_cancel(&m);
    assert(d.losses[INTERACTION_REFERENCE_CANCEL] == 1 && !d.first_active.known);
    /* Bind midstream without a prior observed sample: never fabricate its pair. */
    time = physical_grip(&m, 0);
    interaction_reference_diagnostics_bind(&m, &d);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 3);
    assert(!d.first_active.terminal.previous_known);
    /* Corrupt orientation fault injection still fails closed, with its exact
     * normalization branch, rather than presenting a plausible sensor cause. */
    for (unsigned haptic = 0; haptic < 2; ++haptic) {
        time = reference_fixture(&m, &d);
        m.attitude = (interaction_quaternion_t){0};
        interaction_set_suppressed(&m, false, false, haptic != 0, time / 1000);
        (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 1);
        assert(d.first_active.reason == (haptic ? INTERACTION_REFERENCE_HAPTIC_NORMALIZE :
            INTERACTION_REFERENCE_PREDICT_NORMALIZE));
        assert(!m.gravity_ready && d.first_active.terminal.gravity_before[2] == 1);
    }
}

static void reference_observation_preserves_all_control_state_and_output(void)
{
    interaction_t observed, plain;
    interaction_reference_diagnostics_t d;
    uint64_t time = reference_fixture(&observed, &d);
    plain = observed;
    plain.reference_diagnostics = NULL;
    for (unsigned i = 0; i < 800; ++i) {
        time += i % 127 == 0 ? 60000 : 10000;
        interaction_imu_sample_t s = {.timestamp_us = time, .az_g = 1,
            .gz_dps = i % 50 < 25 ? 25 : -25};
        if (i % 113 == 0) s.az_g = 3;
        if (i % 97 == 0) s.ax_g = .8f;
        if (i % 89 == 0) s.gx_dps = 2000;
        const bool haptic = i % 79 < 30;
        interaction_set_suppressed(&observed, false, false, haptic, time / 1000);
        interaction_set_suppressed(&plain, false, false, haptic, time / 1000);
        const input_delta_t a = interaction_motion_sample(&observed, i % 137 ? &s : NULL);
        const input_delta_t b = interaction_motion_sample(&plain, i % 137 ? &s : NULL);
        assert(a.x == b.x && a.y == b.y);
        interaction_t control = observed;
        control.reference_diagnostics = NULL;
        assert(memcmp(&control, &plain, sizeof plain) == 0);
    }
}

static void aim_toggle_and_recenter_preserve_bias_intent_and_fault_history(void)
{
    interaction_t m;
    uint64_t time = physical_grip(&m, 0);
    interaction_reference_diagnostics_t d;
    interaction_reference_diagnostics_bind(&m, &d);
    const interaction_config_t config = m.config;
    float bias[3]; memcpy(bias, m.bias, sizeof bias);
    assert(interaction_set_motion(&m, false));
    /* Turn while disabled. Gravity stays the same and yaw is only observable
     * through the quaternion. The original pitch axis remains fixed. */
    for (unsigned i = 0; i < 100; ++i) {
        input_delta_t out = physical_sample(&m, &time, 10000, 0, 0, 90, 0, 0, 1);
        assert(!out.x && !out.y);
    }
    assert(interaction_set_motion(&m, true));
    assert(interaction_aim_reference_ready(&m) && !d.enable_resets && !d.active_losses);
    input_delta_t out = physical_sample(&m, &time, 10000, 0, 0, 25, 0, 0, 1);
    assert(!out.x && !out.y && m.aim_aligned);
    /* Body right is now approximately world Y. It is perpendicular to the
     * calibrated pitch axis and produces no pointer motion before Recenter. */
    assert(physical_sample(&m, &time, 10000, 50, 0, 0, 0, 0, 1).y == 0);
    assert(physical_sample(&m, &time, 10000, 0, 0, 12.5f, 0, 0, 1).x == 0);
    assert(m.pointer_motion.x > 0);
    const input_state_t control = m.control;
    const uint64_t last_sample = m.sample_us;
    const interaction_quaternion_t attitude = m.attitude;
    assert(interaction_recenter_owned(&m, INPUT_LOCAL));
    assert(interaction_aim_reference_ready(&m) && m.pointer_motion.x == 0 && m.pointer_motion.y == 0);
    assert(m.sample_seen && m.sample_us == last_sample && m.view.motion_enabled && m.view.calibrated);
    assert(memcmp(&m.attitude, &attitude, sizeof attitude) == 0 && m.aim_pitch[1] > .99f);
    assert(memcmp(&m.config, &config, sizeof config) == 0 && memcmp(m.bias, bias, sizeof bias) == 0);
    assert(memcmp(&m.control, &control, sizeof control) == 0);
    assert(d.recenter_resets == 1 && d.last_reset == INTERACTION_REFERENCE_RESET_RECENTER);
    out = physical_sample(&m, &time, 10000, 50, 0, 0, 0, 0, 1);
    assert(!out.x && !out.y); /* First interval after an explicit alignment. */
    out = physical_sample(&m, &time, 10000, 50, 0, 0, 0, 0, 1);
    assert(!out.x && out.y >= 1);
    (void)physical_sample(&m, &time, 10000, 0, 0, 0, 0, 0, 3);
    assert(d.first_active.known && d.first_active.reason == INTERACTION_REFERENCE_ACCEL_NORM);
    const interaction_reference_incident_t incident = d.first_active;
    const uint32_t losses = d.active_losses;
    assert(!interaction_recenter_owned(&m, INPUT_LOCAL));
    assert(d.active_losses == losses && memcmp(&incident, &d.first_active, sizeof incident) == 0);
    assert(interaction_set_motion(&m, false)); /* Waiting is always stoppable. */
    assert(!interaction_recenter_owned(&m, INPUT_LOCAL));
}

static void recenter_rejection_has_no_partial_effect(void)
{
    interaction_t base;
    (void)physical_grip(&base, 0);
    for (unsigned fault = 0; fault < 10; ++fault) {
        interaction_t m = base;
        if (fault == 0) m.view.mode = INPUT_KEYBOARD;
        if (fault == 1) m.view.motion_enabled = false;
        if (fault == 2) m.view.calibrated = false;
        if (fault == 3) m.view.calibrating = true;
        if (fault == 4) m.output_ready = false;
        if (fault == 5) m.control.locked = true;
        if (fault == 6) m.control.owner = INPUT_REMOTE;
        if (fault == 7) m.motion_source = INPUT_REMOTE;
        if (fault == 8) m.view.menu_open = true;
        const interaction_t before = m;
        assert(!interaction_recenter_owned(&m, fault == 9 ? (input_owner_t)99 : INPUT_LOCAL));
        assert(memcmp(&before, &m, sizeof m) == 0);
    }
    base.control.owner = base.motion_source = INPUT_REMOTE;
    assert(interaction_recenter_owned(&base, INPUT_REMOTE));
    assert(base.view.motion_enabled && base.gravity_ready);
}

int main(void)
{
    nine_cell_grid_and_preview();
    send_prefix_and_interruption();
    progress_until_edit();
    button_timing();
    pending_click_keeps_keyboard_selection();
    pending_selection_double_hold_and_mouse();
    motion_calibration_and_focus();
    third_row_and_empty_cell_geometry();
    gyro_children_and_context_reset();
    calibration_rejection();
    suppression_and_touch();
    asynchronous_freeze();
    canonical_boundaries();
    invalid_configuration_and_samples();
    pad_single_double_and_drag_effects();
    pad_timing_distance_and_raw_slop_boundaries();
    pad_movement_only_and_external_drag_never_click();
    pad_admission_rejection_and_cleanup_latch();
    pad_context_changes_and_congestion_release();
    pad_heartbeat_deadlines_and_old_gesture_isolation();
    pad_event_time_staleness_clock_and_malformed_input();
    pad_configuration_and_token_exhaustion();
    auto_grip_projection_and_independent_gains();
    aiming_turns_stop_and_return_across_angle_wrap();
    aiming_sideways_calibration_and_gravity_only_correction();
    gravity_transients_gaps_and_reacquisition();
    orientation_tracks_during_touch_and_crosses_old_geometry_limit();
    calibration_cancellation_requires_a_new_explicit_start();
    reference_incidents_preserve_exact_cause_and_first_evidence();
    reference_rejection_episodes_keep_unknown_checks_distinct();
    reference_attachment_reset_staleness_and_saturation();
    reference_external_causes_and_inactive_losses_are_truthful();
    reference_observation_preserves_all_control_state_and_output();
    aim_toggle_and_recenter_preserve_bias_intent_and_fault_history();
    recenter_rejection_has_no_partial_effect();
    puts("PASS: nine-cell grid, 64-character preview, send prefixes, tap/double/drag admission and cleanup, calibration and motion suppression");
    return 0;
}
