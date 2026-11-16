#include "passport_widgets.h"
#include <stdio.h>
#include <string.h>

typedef struct { lv_obj_t *box, *icon, *title, *subtitle; } item_widgets_t;
static lv_obj_t *root, *brand, *radio, *wifi, *title, *target, *notice, *notice_text, *notice_detail;
static lv_obj_t *panel, *hero, *heading, *body, *footer, *footer_up, *footer_ok, *footer_down;
static item_widgets_t items[PASSPORT_VIEW_ITEMS];
static lv_style_t background, card, focused, plain_text, small_text, large_text;
static passport_view_t previous;
static bool rendered;
static uint32_t ink, muted, accent, surface, border, field, focus_fill, warning, danger;

static const char *icon_text(passport_icon_t icon)
{
    switch (icon) {
    case ICON_MOUSE: return "";
    case ICON_KEYS: return LV_SYMBOL_KEYBOARD;
    case ICON_CONNECTIONS: return LV_SYMBOL_SHUFFLE;
    case ICON_STATUS: return LV_SYMBOL_LIST;
    case ICON_BLUETOOTH: return LV_SYMBOL_BLUETOOTH;
    case ICON_WIFI: return LV_SYMBOL_WIFI;
    case ICON_APPEARANCE: return LV_SYMBOL_TINT;
    case ICON_ADD: return LV_SYMBOL_PLUS;
    case ICON_BACK: return LV_SYMBOL_LEFT;
    case ICON_UP: return LV_SYMBOL_UP;
    case ICON_LEFT: return LV_SYMBOL_LEFT;
    case ICON_CLICK: return LV_SYMBOL_OK;
    case ICON_RIGHT: return LV_SYMBOL_RIGHT;
    case ICON_DOWN: return LV_SYMBOL_DOWN;
    case ICON_LOCK: return "";
    case ICON_CHECK: return LV_SYMBOL_OK;
    default: return "";
    }
}

static void draw_icon(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_target(event);
    passport_icon_t icon = (passport_icon_t)(uintptr_t)lv_obj_get_user_data(object);
    if (icon != ICON_MOUSE && icon != ICON_LOCK) return;
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    int x = (area.x1 + area.x2) / 2;
    int y = area.y1 + 1;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_color_t color = lv_obj_get_style_text_color(object, 0);
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_TRANSP;
    rect.border_color = color;
    rect.border_width = 2;
    rect.radius = icon == ICON_MOUSE ? 7 : 3;
    lv_area_t bounds = {x - 9, y + (icon == ICON_LOCK ? 9 : 0), x + 9, y + 21};
    lv_draw_rect(layer, &rect, &bounds);
    if (icon == ICON_LOCK) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = color;
        arc.width = 2;
        arc.radius = 7;
        arc.center = (lv_point_t){x, y + 10};
        arc.start_angle = 180;
        arc.end_angle = 360;
        lv_draw_arc(layer, &arc);
    } else {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = color;
        line.width = 2;
        line.p1 = (lv_point_precise_t){x, y + 4};
        line.p2 = (lv_point_precise_t){x, y + 9};
        lv_draw_line(layer, &line);
    }
}

static void set_icon(lv_obj_t *object, passport_icon_t icon)
{
    if ((uintptr_t)lv_obj_get_user_data(object) != (uintptr_t)icon) {
        lv_obj_set_user_data(object, (void *)(uintptr_t)icon);
        lv_obj_invalidate(object);
    }
}

static void text(lv_obj_t *label, const char *value)
{
    if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void place(lv_obj_t *object, int x, int y, int width, int height)
{
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(object, &card, 0);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, lv_style_t *style)
{
    lv_obj_t *object = lv_label_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_add_style(object, style, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_label_set_text(object, "");
    return object;
}

static void palette(passport_theme_t theme)
{
    bool dark = theme == PASSPORT_DARK;
    field = dark ? 0x101519 : 0xF7F6F2;
    surface = dark ? 0x1C252C : 0xFFFFFF;
    ink = dark ? 0xF4F7F8 : 0x172D3D;
    muted = dark ? 0xA4B0BA : 0x5C6B76;
    accent = dark ? 0x69E8CD : 0x007865;
    border = dark ? 0x35434D : 0xD1D9DB;
    focus_fill = dark ? 0x203D3D : 0xDFF3EC;
    warning = dark ? 0xF4C76D : 0x805600;
    danger = dark ? 0xFF9A91 : 0xAC342E;
    lv_style_set_bg_color(&background, lv_color_hex(field));
    lv_style_set_bg_color(&card, lv_color_hex(surface));
    lv_style_set_border_color(&card, lv_color_hex(border));
    lv_style_set_bg_color(&focused, lv_color_hex(focus_fill));
    lv_style_set_border_color(&focused, lv_color_hex(accent));
    lv_style_set_text_color(&plain_text, lv_color_hex(ink));
    lv_style_set_text_color(&large_text, lv_color_hex(ink));
    lv_style_set_text_color(&small_text, lv_color_hex(muted));
    lv_obj_report_style_change(NULL);
}

bool passport_widgets_init(lv_obj_t *screen)
{
    lv_style_init(&background);
    lv_style_set_bg_opa(&background, LV_OPA_COVER);
    lv_style_init(&card);
    lv_style_set_bg_opa(&card, LV_OPA_COVER);
    lv_style_set_border_width(&card, 1);
    lv_style_set_radius(&card, 10);
    lv_style_set_pad_all(&card, 0);
    lv_style_init(&focused);
    lv_style_set_border_width(&focused, 2);
    lv_style_init(&plain_text);
    lv_style_set_text_font(&plain_text, &lv_font_montserrat_14);
    lv_style_init(&small_text);
    lv_style_set_text_font(&small_text, &lv_font_montserrat_12);
    lv_style_init(&large_text);
    lv_style_set_text_font(&large_text, &lv_font_montserrat_18);
    palette(PASSPORT_DARK);
    root = lv_obj_create(screen);
    if (!root) return false;
    lv_obj_remove_style_all(root);
    lv_obj_add_style(root, &background, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    place(root, 0, 0, 240, 320);
    brand = label(root, &plain_text);
    radio = label(root, &small_text);
    wifi = label(root, &small_text);
    title = label(root, &large_text);
    target = label(root, &small_text);
    panel = box(root);
    if (!brand || !radio || !wifi || !title || !target || !panel) goto fail;
    hero = label(panel, &large_text);
    heading = label(panel, &large_text);
    body = label(panel, &small_text);
    notice = box(root);
    footer = box(root);
    if (!hero || !heading || !body || !notice || !footer) goto fail;
    lv_obj_add_event_cb(hero, draw_icon, LV_EVENT_DRAW_MAIN, NULL);
    notice_text = label(notice, &small_text);
    notice_detail = label(notice, &small_text);
    footer_up = label(footer, &small_text);
    footer_ok = label(footer, &small_text);
    footer_down = label(footer, &small_text);
    if (!notice_text || !notice_detail || !footer_up || !footer_ok || !footer_down) goto fail;
    for (unsigned i = 0; i < PASSPORT_VIEW_ITEMS; ++i) {
        item_widgets_t *item = &items[i];
        item->box = box(root);
        if (!item->box) goto fail;
        item->title = label(item->box, &plain_text);
        item->subtitle = label(item->box, &small_text);
        item->icon = label(item->box, &large_text);
        if (!item->title || !item->subtitle || !item->icon) goto fail;
        lv_obj_add_event_cb(item->icon, draw_icon, LV_EVENT_DRAW_MAIN, NULL);
        visible(item->box, false);
    }
    text(brand, "PASSPORT");
    place(brand, 12, 9, 104, 18);
    place(radio, 104, 11, 68, 16);
    place(wifi, 180, 11, 48, 16);
    text(wifi, "WiFi ?");
    lv_obj_set_style_text_align(wifi, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(radio, LV_TEXT_ALIGN_RIGHT, 0);
    place(title, 12, 36, 216, 22);
    place(target, 12, 61, 216, 16);
    place(panel, 12, 84, 216, 158);
    place(notice, 12, 242, 216, 46);
    place(notice_text, 8, 3, 200, 15);
    place(notice_detail, 8, 18, 200, 26);
    lv_label_set_long_mode(notice_detail, LV_LABEL_LONG_WRAP);
    place(footer, 0, 292, 240, 28);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    place(footer_up, 12, 7, 64, 16);
    place(footer_ok, 80, 7, 80, 16);
    place(footer_down, 164, 7, 64, 16);
    lv_obj_set_style_text_align(footer_ok, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(footer_down, LV_TEXT_ALIGN_RIGHT, 0);
    text(footer_up, "UP  Prev");
    text(footer_down, "DOWN");
    visible(panel, false);
    rendered = false;
    return true;
fail:
    lv_obj_delete(root);
    root = NULL;
    return false;
}

static void item_geometry(item_widgets_t *item, passport_layout_t layout, unsigned slot)
{
    int x = 12, y = 84, width = 216, height = 48;
    if (layout == VIEW_HOME) {
        x += (slot % 2) * 112;
        y += (slot / 2) * 78;
        width = 104; height = 70;
    } else if (layout == VIEW_MOUSE) {
        static const int positions[][2] = {{94,84},{36,134},{94,134},{152,134},{94,184},{12,210}};
        x = positions[slot][0]; y = positions[slot][1];
        width = slot == 5 ? 66 : 52; height = slot == 5 ? 22 : 44;
    } else if (layout == VIEW_KEYS) {
        x += (slot % 2) * 112;
        y += (slot / 2) * 32;
        width = 104; height = 28;
        if (slot == 8) { x = 12; y = 216; width = 216; height = 24; }
    } else if (layout == VIEW_CONFIRM) {
        x += slot * 112; y = 216; width = 104; height = 24;
    } else if (layout == VIEW_MESSAGE) {
        y = 216; height = 24;
    } else {
        y += slot * 54;
    }
    place(item->box, x, y, width, height);
    bool square = layout == VIEW_MOUSE && slot < 5;
    bool compact = square || layout == VIEW_KEYS || layout == VIEW_CONFIRM || layout == VIEW_MESSAGE || (layout == VIEW_MOUSE && slot == 5);
    visible(item->subtitle, !compact);
    visible(item->icon, !compact || square);
    visible(item->title, !square);
    lv_obj_set_style_text_font(item->title, layout == VIEW_HOME ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
    if (layout == VIEW_HOME) {
        place(item->icon, 10, 9, 28, 22);
        place(item->title, 10, 35, width - 16, 18);
        place(item->subtitle, 10, 53, width - 16, 14);
    } else if (compact) {
        place(item->title, 4, (height - 14) / 2, width - 8, 18);
        lv_obj_set_style_text_align(item->title, LV_TEXT_ALIGN_CENTER, 0);
        place(item->icon, 0, 11, width, 22);
        lv_obj_set_style_text_align(item->icon, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        place(item->icon, 10, 14, 22, 22);
        place(item->title, 40, 6, width - 48, 18);
        place(item->subtitle, 40, 27, width - 48, 16);
    }
    if (!compact) {
        lv_obj_set_style_text_align(item->title, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_align(item->icon, LV_TEXT_ALIGN_LEFT, 0);
    }
}

static uint32_t tone_color(passport_tone_t tone)
{
    return tone == TONE_GOOD ? accent : tone == TONE_ERROR ? danger : tone == TONE_WAIT ? warning : muted;
}

void passport_widgets_update(const passport_view_t *view)
{
    bool theme_changed = !rendered || view->theme != previous.theme;
    bool geometry_changed = !rendered || view->layout != previous.layout || view->count != previous.count;
    if (theme_changed) palette(view->theme);
    text(title, view->title);
    text(target, view->target);
    text(radio, view->bluetooth);
    lv_obj_set_style_text_color(radio, lv_color_hex(tone_color(view->bluetooth_tone)), 0);
    text(notice_text, view->message);
    text(notice_detail, view->detail);
    lv_obj_set_style_text_color(notice_text, lv_color_hex(tone_color(view->tone)), 0);
    lv_obj_set_style_border_color(notice, lv_color_hex(tone_color(view->tone)), 0);
    char action[24];
    snprintf(action, sizeof(action), "OK  %s", view->ok);
    text(footer_ok, action);
    lv_obj_set_style_text_color(footer_ok, lv_color_hex(accent), 0);
    unsigned first = view->layout == VIEW_LIST && view->focus >= 3 ? view->focus - 2 : 0;
    unsigned old_first = previous.layout == VIEW_LIST && previous.focus >= 3 ? previous.focus - 2 : 0;
    geometry_changed |= first != old_first;
    for (unsigned slot = 0; slot < PASSPORT_VIEW_ITEMS; ++slot) {
        unsigned index = first + slot;
        bool show = index < view->count && (view->layout != VIEW_LIST || slot < 3);
        visible(items[slot].box, show);
        if (!show) continue;
        if (geometry_changed) item_geometry(&items[slot], view->layout, slot);
        bool selected = index == view->focus;
        if (geometry_changed || theme_changed || view->focus != previous.focus) {
            lv_obj_remove_style(items[slot].box, &focused, 0);
            if (selected) lv_obj_add_style(items[slot].box, &focused, 0);
            lv_obj_set_style_text_color(items[slot].icon, lv_color_hex(selected ? accent : muted), 0);
        }
        const passport_item_t *item = &view->items[index];
        text(items[slot].title, item->title);
        text(items[slot].subtitle, item->subtitle);
        text(items[slot].icon, icon_text(item->checked ? ICON_CHECK : item->icon));
        set_icon(items[slot].icon, item->checked ? ICON_CHECK : item->icon);
    }
    bool show_panel = view->layout == VIEW_MESSAGE || view->layout == VIEW_CONFIRM || view->layout == VIEW_DETAILS;
    visible(panel, show_panel);
    if (show_panel) {
        bool details = view->layout == VIEW_DETAILS;
        bool confirmation = view->layout == VIEW_CONFIRM;
        place(panel, 12, 84, 216, details ? 156 : 124);
        visible(hero, !details);
        visible(heading, !details);
        place(hero, 10, 10, 194, 28);
        lv_obj_set_style_text_align(hero, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(hero, lv_color_hex(confirmation ? warning : accent), 0);
        place(heading, 10, 43, 194, 24);
        lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
        place(body, 12, details ? 8 : 70, 190, details ? 142 : 48);
        lv_obj_set_style_text_align(body, details ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        text(hero, view->hero[0] ? view->hero : icon_text(view->hero_icon));
        set_icon(hero, view->hero[0] ? ICON_NONE : view->hero_icon);
        text(heading, view->heading);
        text(body, view->body);
    }
    previous = *view;
    rendered = true;
}
