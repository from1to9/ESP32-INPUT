// SPDX-License-Identifier: Apache-2.0
#include "app_boot_ui.h"
#include "ui_memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-only rendering storage; the production module owns neither buffer. */
static unsigned char s_draw[480 * 40 * 2];
static unsigned char s_frame[480 * 480 * 3];
static unsigned s_flushes;
static const char *s_output;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    ++s_flushes;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 480 && y >= 0 && y < 480);
            size_t offset = ((size_t)y * 480 + (size_t)x) * 3;
            uint16_t rgb = (uint16_t)(pixels[0] | (uint16_t)pixels[1] << 8);
            s_frame[offset] = (unsigned char)(((rgb >> 11) & 31U) * 255U / 31U);
            s_frame[offset + 1] = (unsigned char)(((rgb >> 5) & 63U) * 255U / 63U);
            s_frame[offset + 2] = (unsigned char)((rgb & 31U) * 255U / 31U);
            pixels += 2;
        }
    }
    lv_display_flush_ready(display);
}

static void settle(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
}

static unsigned objects(lv_obj_t *object)
{
    unsigned count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += objects(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static unsigned timers(void)
{
    unsigned count = 0;
    lv_timer_t *timer = NULL;
    while ((timer = lv_timer_get_next(timer)) != NULL) ++count;
    return count;
}

static bool has_text(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (has_text(lv_obj_get_child(object, (int32_t)i), text)) return true;
    return false;
}

static void geometry(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 480 && area.y2 < 480);
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_point_t measured;
        lv_text_get_size(&measured, lv_label_get_text(object), lv_obj_get_style_text_font(object, 0),
                         lv_obj_get_style_text_letter_space(object, 0),
                         lv_obj_get_style_text_line_space(object, 0), lv_obj_get_content_width(object), 0);
        if (measured.y > lv_obj_get_content_height(object))
            fprintf(stderr, "Text exceeds label height: %s (%d > %d)\n", lv_label_get_text(object),
                    (int)measured.y, (int)lv_obj_get_content_height(object));
        assert(measured.y <= lv_obj_get_content_height(object));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        geometry(lv_obj_get_child(object, (int32_t)i));
}

static void screenshot(const char *name, lv_obj_t *screen)
{
    settle();
    geometry(screen);
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s.ppm", s_output, name);
    assert(written > 0 && (size_t)written < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "P6\n480 480\n255\n") > 0);
    assert(fwrite(s_frame, sizeof(s_frame), 1, file) == 1);
    assert(fclose(file) == 0);
}

static void no_activity(lv_display_t *display, const app_boot_ui_view_t *view)
{
    settle();
    ui_memory_stats_t before = ui_memory_stats();
    unsigned flushes = s_flushes;
    for (unsigned i = 0; i < 1000; ++i) {
        assert(app_boot_ui_update(view));
        lv_tick_inc(20);
        lv_timer_handler();
    }
    ui_memory_stats_t after = ui_memory_stats();
    assert(after.allocations == before.allocations && after.reallocations == before.reallocations);
    assert(after.live_bytes == before.live_bytes && after.live_blocks == before.live_blocks);
    assert(s_flushes == flushes);
    assert(lv_display_get_horizontal_resolution(display) == 480);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_output = argv[1];
    ui_memory_check_observer();
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_t *unrelated = lv_obj_create(screen);
    lv_obj_set_size(unrelated, 1, 1);
    settle();
    unsigned timer_count = timers();
    unsigned base_objects = objects(screen);
    assert(!app_boot_ui_create(NULL));
    lv_display_t *wrong = lv_display_create(240, 480);
    assert(!app_boot_ui_create(wrong));
    lv_display_delete(wrong);
    assert(app_boot_ui_create(display));
    assert(app_boot_ui_active() && !app_boot_ui_create(display));
    assert(timers() == timer_count);
    unsigned added_objects = objects(screen) - base_objects;
    assert(added_objects <= 16);
    screenshot("starting", screen);
    assert(has_text(screen, "Progress unavailable"));
    app_boot_ui_view_t view = {
        .state = APP_BOOT_UI_STARTING, .stage = "Preparing input services",
        .detail = "Local controls are still starting.",
        .steps_known = true, .completed_steps = 2, .total_steps = 4
    };
    assert(app_boot_ui_update(&view));
    screenshot("progress", screen);
    assert(has_text(screen, "Startup steps: 2 / 4"));
    no_activity(display, &view);
    app_boot_ui_view_t invalid = view;
    invalid.completed_steps = 5;
    assert(!app_boot_ui_update(&invalid));
    invalid = view;
    invalid.total_steps = 0;
    assert(!app_boot_ui_update(&invalid));
    invalid = view;
    invalid.state = APP_BOOT_UI_READY;
    assert(!app_boot_ui_update(&invalid));
    invalid = view;
    memset(invalid.detail, 'X', sizeof(invalid.detail));
    assert(!app_boot_ui_update(&invalid));
    invalid = view;
    invalid.stage[0] = '\0';
    assert(!app_boot_ui_update(&invalid));
    invalid = view;
    invalid.detail[0] = '\n';
    assert(!app_boot_ui_update(&invalid));
    invalid = view;
    invalid.state = (app_boot_ui_state_t)99;
    assert(!app_boot_ui_update(&invalid));
    assert(!app_boot_ui_update(NULL));
    assert(has_text(screen, "Preparing input services"));
    no_activity(display, &view);
    view.state = APP_BOOT_UI_FAULT;
    snprintf(view.stage, sizeof(view.stage), "Input service unavailable");
    snprintf(view.detail, sizeof(view.detail), "Startup stopped. Check the device connection.");
    assert(app_boot_ui_update(&view));
    screenshot("fault", screen);
    assert(has_text(screen, "STARTUP FAULT"));
    no_activity(display, &view);
    view.state = APP_BOOT_UI_READY;
    view.completed_steps = 4;
    snprintf(view.stage, sizeof(view.stage), "Opening your controls");
    snprintf(view.detail, sizeof(view.detail), "Connections are shown on the next screen.");
    assert(app_boot_ui_update(&view));
    screenshot("ready", screen);
    assert(has_text(screen, "LOCAL CONTROLS READY"));
    view.steps_known = false;
    memset(view.stage, 'W', sizeof(view.stage) - 1);
    view.stage[sizeof(view.stage) - 1] = '\0';
    memset(view.detail, 'W', sizeof(view.detail) - 1);
    view.detail[sizeof(view.detail) - 1] = '\0';
    assert(app_boot_ui_update(&view));
    screenshot("maximum-text", screen);
    no_activity(display, &view);
    app_boot_ui_destroy();
    app_boot_ui_destroy();
    assert(!app_boot_ui_active());
    assert(objects(screen) == base_objects && lv_obj_is_valid(unrelated));
    settle();
    ui_memory_stats_t baseline = ui_memory_stats();
    for (unsigned i = 0; i < 1000; ++i) {
        assert(app_boot_ui_create(display));
        assert(app_boot_ui_update(&view));
        settle();
        app_boot_ui_destroy();
        settle();
        ui_memory_stats_t current = ui_memory_stats();
        assert(current.live_bytes == baseline.live_bytes && current.live_blocks == baseline.live_blocks);
        assert(timers() == timer_count && objects(screen) == base_objects);
    }
    assert(app_boot_ui_create(display));
    lv_obj_clean(screen);
    assert(!app_boot_ui_active());
    assert(!app_boot_ui_update(&view));
    assert(app_boot_ui_create(display));
    lv_display_delete(display);
    assert(!app_boot_ui_active());
    lv_deinit();
    ui_memory_stats_t final = ui_memory_stats();
    assert(final.live_bytes == 0 && final.live_blocks == 0 && final.failures == 0);
    printf("{\"passed\":true,\"cycles\":1000,\"boot_objects\":%u,\"peak_lvgl_bytes\":%zu,"
           "\"final_live_bytes\":%zu,\"final_live_blocks\":%zu}\n",
           added_objects, final.peak_live_bytes, final.live_bytes, final.live_blocks);
    return 0;
}
