#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PASSPORT_SPEAKER_RATE 16000
#define PASSPORT_SPEAKER_BLOCK 128

// Owned by the optional sound task.
esp_err_t passport_speaker_open(void);
esp_err_t passport_speaker_write(const int16_t *stereo, size_t frames);
esp_err_t passport_speaker_close(void);
