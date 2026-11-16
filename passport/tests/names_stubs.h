#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "theme_stubs.h"

#define ESP_ERR_INVALID_SIZE 8
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
void esp_fill_random(void *data, size_t size);
void explicit_bzero(void *data, size_t size);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);

typedef int mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 1
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type);
int mbedtls_md_hmac(const mbedtls_md_info_t *, const unsigned char *, size_t,
                   const unsigned char *, size_t, unsigned char *);

typedef int lv_font_t;
typedef struct { bool is_placeholder; } lv_font_glyph_dsc_t;
extern const lv_font_t lv_font_montserrat_12, lv_font_montserrat_14;
bool lv_font_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t scalar, uint32_t next);
