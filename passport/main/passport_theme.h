#pragma once
#include "esp_err.h"

typedef enum { PASSPORT_DARK, PASSPORT_LIGHT } passport_theme_t;
// Missing preference defaults to Dark. Other read failures stay observable.
esp_err_t passport_theme_load(passport_theme_t *theme);
esp_err_t passport_theme_save(passport_theme_t theme);
