#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    PASSPORT_SOUND_OFF, PASSPORT_SOUND_STARTING, PASSPORT_SOUND_READY,
    PASSPORT_SOUND_STOPPING, PASSPORT_SOUND_UNAVAILABLE,
} passport_sound_output_t;

typedef enum {
    PASSPORT_CUE_CONFIRM = 1, PASSPORT_CUE_SUCCESS, PASSPORT_CUE_ERROR,
} passport_cue_t;

typedef struct {
    passport_sound_output_t output;
    bool enabled;
    bool preference_known;
} passport_sound_state_t;

// Called by the native UI only. Playback and board I/O belong to the sound task.
void passport_sound_load(void);
esp_err_t passport_sound_set(bool enabled);
passport_sound_state_t passport_sound_state(void);
bool passport_sound_cue(passport_cue_t cue);
