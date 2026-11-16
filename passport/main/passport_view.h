#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "passport_theme.h"

#define PASSPORT_VIEW_ITEMS 9
#define PASSPORT_LIST_ROWS 3

typedef enum { VIEW_HOME, VIEW_LIST, VIEW_MOUSE, VIEW_KEYS, VIEW_MESSAGE, VIEW_CONFIRM, VIEW_DETAILS, VIEW_QR, VIEW_NETWORK_CONFIRM, VIEW_IDENTITY } passport_layout_t;
typedef enum { TONE_MUTED, TONE_GOOD, TONE_WAIT, TONE_ERROR } passport_tone_t;
typedef enum {
    ICON_NONE, ICON_MOUSE, ICON_KEYS, ICON_CONNECTIONS, ICON_STATUS,
    ICON_BLUETOOTH, ICON_WIFI, ICON_APPEARANCE, ICON_ADD, ICON_BACK,
    ICON_UP, ICON_LEFT, ICON_CLICK, ICON_RIGHT, ICON_DOWN, ICON_LOCK, ICON_CHECK
} passport_icon_t;

typedef struct {
    char title[72], subtitle[48];
    passport_icon_t icon;
    bool checked;
} passport_item_t;

typedef struct {
    passport_theme_t theme;
    passport_layout_t layout;
    passport_tone_t tone, bluetooth_tone, wifi_tone;
    char title[28], target[32], bluetooth[12], message[48], detail[64], ok[16];
    char heading[32], body[224], hero[24], wifi[12], qr[128];
    passport_icon_t hero_icon;
    unsigned count, focus;
    passport_item_t items[PASSPORT_VIEW_ITEMS];
    char scan_summary[PASSPORT_LIST_ROWS][32];
} passport_view_t;
