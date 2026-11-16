#pragma once
#include "passport_view.h"
#include "lvgl.h"

// All calls require the BSP LVGL mutex. Objects are created once and retained.
bool passport_widgets_init(lv_obj_t *screen);
void passport_widgets_update(const passport_view_t *view);
