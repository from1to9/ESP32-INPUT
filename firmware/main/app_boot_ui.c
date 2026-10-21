// SPDX-License-Identifier: Apache-2.0
#include "app_boot_ui.h"
#include <stdio.h>
#include <string.h>

#define BACKGROUND 0x080b10
#define SURFACE 0x111a24
#define ACCENT 0x8dd8ff
#define TEXT 0xecf4fb
#define MUTED 0xb6c4d2
#define FAULT 0xffb4ab

#if LV_FONT_MONTSERRAT_22
#define TITLE_FONT (&lv_font_montserrat_22)
#else
#define TITLE_FONT LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_14
#define META_FONT (&lv_font_montserrat_14)
#else
#define META_FONT LV_FONT_DEFAULT
#endif

static lv_obj_t *s_root, *s_status, *s_stage, *s_detail, *s_steps;
static app_boot_ui_view_t s_view;
static bool s_have_view;
static const lv_point_precise_t s_pointer[] = {
    {0, 0}, {0, 44}, {10, 34}, {19, 51}, {27, 47}, {18, 30}, {36, 29}, {0, 0}
};

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height,
                     uint32_t color, int radius)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static lv_obj_t *label(int y, int height, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *object = lv_label_create(s_root);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, 48, y);
    lv_obj_set_size(object, 384, height);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, "");
    return object;
}

static void deleted(lv_event_t *event)
{
    (void)event;
    s_root = s_status = s_stage = s_detail = s_steps = NULL;
    s_view = (app_boot_ui_view_t){0};
    s_have_view = false;
}

static bool valid_text(const char *text, size_t capacity, bool required)
{
    for (size_t i = 0; i < capacity; ++i) {
        if (!text[i]) return i != 0 || !required;
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126) return false;
    }
    return false;
}

bool app_boot_ui_create(lv_display_t *display)
{
    if (!display || s_root || lv_display_get_horizontal_resolution(display) != 480 ||
        lv_display_get_vertical_resolution(display) != 480) return false;
    s_root = box(lv_display_get_screen_active(display), 0, 0, 480, 480, BACKGROUND, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_root, deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *plate = box(s_root, 160, 90, 160, 108, SURFACE, 28);
    box(plate, 28, 42, 9, 24, ACCENT, 4);
    box(plate, 43, 28, 9, 52, ACCENT, 4);
    box(plate, 58, 36, 9, 36, ACCENT, 4);
    lv_obj_t *pointer = lv_line_create(plate);
    lv_obj_remove_style_all(pointer);
    lv_obj_set_pos(pointer, 94, 29);
    lv_line_set_points(pointer, s_pointer, sizeof(s_pointer) / sizeof(s_pointer[0]));
    lv_obj_set_style_line_color(pointer, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_line_width(pointer, 3, 0);
    lv_obj_set_style_line_rounded(pointer, true, 0);
    lv_obj_t *brand = label(226, 30, TITLE_FONT, TEXT);
    lv_label_set_text(brand, "ESP32 INPUT");
    s_status = label(292, 24, META_FONT, ACCENT);
    s_stage = label(324, 60, LV_FONT_DEFAULT, TEXT);
    s_detail = label(386, 66, META_FONT, MUTED);
    s_steps = label(454, 22, META_FONT, MUTED);
    const app_boot_ui_view_t initial = {
        .state = APP_BOOT_UI_STARTING,
        .stage = "Starting local controls",
        .detail = "Waiting for startup status."
    };
    return app_boot_ui_update(&initial);
}

bool app_boot_ui_update(const app_boot_ui_view_t *view)
{
    if (!s_root || !view || view->state < APP_BOOT_UI_STARTING || view->state > APP_BOOT_UI_FAULT ||
        !valid_text(view->stage, sizeof(view->stage), true) ||
        !valid_text(view->detail, sizeof(view->detail), false) ||
        (view->steps_known && (!view->total_steps || view->completed_steps > view->total_steps ||
         (view->state == APP_BOOT_UI_READY && view->completed_steps != view->total_steps)))) return false;
    if (!s_have_view || view->state != s_view.state) {
        const char *status = view->state == APP_BOOT_UI_FAULT ? "STARTUP FAULT" :
                             view->state == APP_BOOT_UI_READY ? "LOCAL CONTROLS READY" : "STARTING";
        lv_label_set_text(s_status, status);
        lv_obj_set_style_text_color(s_status, lv_color_hex(view->state == APP_BOOT_UI_FAULT ? FAULT : ACCENT), 0);
    }
    if (!s_have_view || strcmp(view->stage, s_view.stage)) lv_label_set_text(s_stage, view->stage);
    if (!s_have_view || strcmp(view->detail, s_view.detail)) lv_label_set_text(s_detail, view->detail);
    if (!s_have_view || view->steps_known != s_view.steps_known ||
        (view->steps_known && (view->completed_steps != s_view.completed_steps || view->total_steps != s_view.total_steps))) {
        char progress[48];
        if (view->steps_known) {
            snprintf(progress, sizeof(progress), "Startup steps: %u / %u",
                     (unsigned)view->completed_steps, (unsigned)view->total_steps);
        } else {
            snprintf(progress, sizeof(progress), "Progress unavailable");
        }
        lv_label_set_text(s_steps, progress);
    }
    s_view = *view;
    s_have_view = true;
    return true;
}

void app_boot_ui_destroy(void)
{
    if (s_root) lv_obj_delete(s_root);
}

bool app_boot_ui_active(void)
{
    return s_root != NULL;
}
