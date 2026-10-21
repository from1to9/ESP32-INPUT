// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef struct lv_display_t lv_display_t;
typedef struct lv_obj_t lv_obj_t;
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
lv_obj_t *lv_display_get_screen_active(lv_display_t *display);
void lv_obj_invalidate_area(const lv_obj_t *object, const lv_area_t *area);
typedef struct lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);
lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *data);
typedef struct lv_event_t lv_event_t;
typedef enum { LV_EVENT_ALL, LV_EVENT_REFR_START, LV_EVENT_REFR_READY,
    LV_EVENT_FLUSH_FINISH, LV_EVENT_DELETE } lv_event_code_t;
typedef enum { LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90 } lv_display_rotation_t;
typedef void (*lv_event_cb_t)(lv_event_t *event);
void *lv_event_get_target(lv_event_t *event);
lv_event_code_t lv_event_get_code(lv_event_t *event);
int32_t lv_display_get_horizontal_resolution(const lv_display_t *display);
int32_t lv_display_get_vertical_resolution(const lv_display_t *display);
int32_t lv_display_get_offset_x(const lv_display_t *display);
int32_t lv_display_get_offset_y(const lv_display_t *display);
lv_display_rotation_t lv_display_get_rotation(lv_display_t *display);
void lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback, lv_event_code_t filter, void *data);
