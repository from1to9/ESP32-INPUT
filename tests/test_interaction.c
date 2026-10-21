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
    const interaction_imu_sample_t s = sample(*time, gx, gy);
    return interaction_motion_sample(m, &s);
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
    for (size_t i = 0; i < 1000; ++i) {
        const input_delta_t delta = motion(&m, &time, 10000, 1.2f, -1.8f);
        assert(delta.x == 0 && delta.y == 0); /* Corrected stationary bias/deadzone. */
    }
    assert(motion(&m, &time, 50001, 26, -2).x == 0);
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
    assert(motion(&m, &time, 10000, 26, -2).x == 1);
    assert(m.view.motion_enabled);
    interaction_sync(&m, &m.control, false, time / 1000);
    assert(motion(&m, &time, 10000, 26, -2).x == 0 && m.view.motion_enabled);
    interaction_sync(&m, &m.control, true, time / 1000);
    assert(motion(&m, &time, 10000, 26, -2).x == 1); /* Short backpressure does not disable intent. */
    assert(interaction_button_update(&m, true, time / 1000) == INTERACTION_EVENT_DOWN);
    assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(interaction_button_update(&m, false, time / 1000) == 0);
    interaction_set_suppressed(&m, false, true, false, time / 1000);
    assert(!m.view.motion_enabled && !interaction_set_motion(&m, true));
    interaction_set_suppressed(&m, false, false, false, time / 1000);
    for (size_t i = 0; i < 20; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    assert(interaction_set_motion(&m, true));
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
    config.pointer_gain = 1;
    assert(interaction_configure(&m, &config));
    assert(interaction_set_motion(&m, true));
    for (size_t i = 0; i < 3; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    interaction_freeze_until(&m, 700);
    assert(interaction_tick(&m, 100) == 0 && m.view.motion_enabled); /* No clock observation. */
    for (size_t i = 0; i < 3; ++i) assert(motion(&m, &time, 10000, 26, -2).x == 0);
    for (size_t i = 0; i < 3; ++i) {
        interaction_freeze_until(&m, 700); /* Repeated delivery must preserve new fractional carry. */
        assert(motion(&m, &time, 10000, 26, -2).x == 0);
    }
    interaction_freeze_until(&m, 699);
    assert(motion(&m, &time, 10000, 26, -2).x == 1);
    assert(m.view.motion_enabled && !m.view.touch_active);
    interaction_set_suppressed(&m, true, false, false, time / 1000);
    assert(interaction_set_touch_divisor(&m, 4));
    for (size_t i = 0; i < 3; ++i) assert(interaction_touch_move(&m, 1, 0).x == 0);
    interaction_freeze_until(&m, 1000);
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
    bad.pointer_gain = NAN;
    assert(!interaction_configure(&m, &bad));
    bad = valid; bad.pointer_gain = -1;
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
    puts("PASS: nine-cell grid, 64-character preview, send prefixes, gestures, calibration and motion suppression");
    return 0;
}
