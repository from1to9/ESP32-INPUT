#include "passport_sound.h"
#include "passport_speaker.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *const sound_namespace = "passport_ui";
static const char *const sound_key = "sound";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static passport_sound_output_t output;
// Only the UI reads or writes the preference fields.
static bool enabled_saved, preference_known;
static TaskHandle_t worker;
static bool wanted, active, cleanup_unconfirmed;
static passport_cue_t pending;
static uint32_t pending_id;
static passport_receipt_t cue_receipt;

static bool output_wanted(void)
{
    portENTER_CRITICAL(&lock);
    bool enabled = wanted;
    portEXIT_CRITICAL(&lock);
    return enabled;
}

static esp_err_t play(passport_cue_t cue, bool *completed)
{
    // Peak amplitude is 1/8 scale before codec gain; each tone ramps in and out.
    static const int16_t sine[] = {
        0, 799, 1567, 2276, 2896, 3406, 3784, 4017,
        4096, 4017, 3784, 3406, 2896, 2276, 1567, 799,
        0, -799, -1567, -2276, -2896, -3406, -3784, -4017,
        -4096, -4017, -3784, -3406, -2896, -2276, -1567, -799,
    };
    static const struct { unsigned hz, frames; } tones[] = {
        [PASSPORT_CUE_CONFIRM] = {750, 512},
        [PASSPORT_CUE_SUCCESS] = {1040, 1024},
        [PASSPORT_CUE_ERROR] = {375, 1024},
    };
    enum { RAMP = 64 };
    const unsigned frames = tones[cue].frames;
    const uint32_t phase_step = tones[cue].hz * 65536U / PASSPORT_SPEAKER_RATE;
    uint16_t phase = 0;
    int16_t block[PASSPORT_SPEAKER_BLOCK][2];
    unsigned offset = 0;
    for (; offset < frames && output_wanted(); offset += PASSPORT_SPEAKER_BLOCK) {
        unsigned count = frames - offset;
        if (count > PASSPORT_SPEAKER_BLOCK) count = PASSPORT_SPEAKER_BLOCK;
        for (unsigned i = 0; i < count; ++i) {
            unsigned position = offset + i;
            unsigned edge = position < frames / 2 ? position : frames - 1 - position;
            int gain = edge < RAMP ? (int)edge : RAMP;
            block[i][0] = block[i][1] = sine[phase >> 11] * gain / RAMP;
            phase += phase_step;
        }
        esp_err_t result = passport_speaker_write(&block[0][0], count);
        if (result != ESP_OK) return result;
    }
    *completed = offset >= frames;
    return ESP_OK;
}

static void service(void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&lock);
    worker = xTaskGetCurrentTaskHandle();
    const bool start = wanted;
    portEXIT_CRITICAL(&lock);

    esp_err_t result = start ? passport_speaker_open() : ESP_OK;
    portENTER_CRITICAL(&lock);
    if (start && wanted && result == ESP_OK) output = PASSPORT_SOUND_READY;
    portEXIT_CRITICAL(&lock);

    while (result == ESP_OK) {
        portENTER_CRITICAL(&lock);
        const bool run = wanted;
        passport_cue_t cue = pending;
        uint32_t id = pending_id;
        pending = 0;
        pending_id = 0;
        portEXIT_CRITICAL(&lock);
        if (!run) break;
        if (cue) {
            bool completed = false;
            result = play(cue, &completed);
            portENTER_CRITICAL(&lock);
            if (id && cue_receipt.id == id && cue_receipt.result == PR_PENDING)
                cue_receipt = (passport_receipt_t){id, result != ESP_OK ? PR_UNAVAILABLE : completed ? PR_APPLIED : PR_CANCELLED, result};
            portEXIT_CRITICAL(&lock);
        }
        else ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    if (result != ESP_OK) {
        portENTER_CRITICAL(&lock);
        output = PASSPORT_SOUND_STOPPING;
        portEXIT_CRITICAL(&lock);
    }
    esp_err_t closed = start ? passport_speaker_close() : ESP_OK;
    if (result != ESP_OK || closed != ESP_OK) {
        ESP_LOGW("passport_sound", "Output failed: %s, cleanup: %s",
                 esp_err_to_name(result), esp_err_to_name(closed));
    }

    portENTER_CRITICAL(&lock);
    worker = NULL;
    pending = 0;
    pending_id = 0;
    if (cue_receipt.result == PR_PENDING) cue_receipt.result = result != ESP_OK ? PR_UNAVAILABLE : PR_CANCELLED;
    active = false;
    if (start) cleanup_unconfirmed = closed != ESP_OK;
    output = wanted || cleanup_unconfirmed ? PASSPORT_SOUND_UNAVAILABLE : PASSPORT_SOUND_OFF;
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}

static void start_output(void)
{
    portENTER_CRITICAL(&lock);
    wanted = true;
    const bool create = !active;
    if (create) {
        active = true;
        output = PASSPORT_SOUND_STARTING;
    }
    portEXIT_CRITICAL(&lock);
    if (create && xTaskCreate(service, "passport_sound", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&lock);
        active = false;
        output = PASSPORT_SOUND_UNAVAILABLE;
        portEXIT_CRITICAL(&lock);
    }
}

static void stop_output(void)
{
    portENTER_CRITICAL(&lock);
    wanted = false;
    pending = 0;
    pending_id = 0;
    if (cue_receipt.result == PR_PENDING) cue_receipt.result = PR_CANCELLED;
    if (active) {
        output = PASSPORT_SOUND_STOPPING;
        if (worker) xTaskNotifyGive(worker);
    } else if (!cleanup_unconfirmed) {
        output = PASSPORT_SOUND_OFF;
    }
    portEXIT_CRITICAL(&lock);
}

void passport_sound_load(void)
{
    uint8_t value = 0;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(sound_namespace, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = nvs_get_u8(handle, sound_key, &value);
        nvs_close(handle);
    }
    preference_known = result == ESP_ERR_NVS_NOT_FOUND || (result == ESP_OK && value <= 1);
    enabled_saved = result == ESP_OK && value == 1;
    if (enabled_saved) start_output();
}

esp_err_t passport_sound_set(bool enabled)
{
    if (enabled && passport_sound_state().output == PASSPORT_SOUND_STOPPING) return ESP_ERR_INVALID_STATE;
    if (!enabled) stop_output();
    if (!preference_known || enabled_saved != enabled) {
        nvs_handle_t handle;
        esp_err_t result = nvs_open(sound_namespace, NVS_READWRITE, &handle);
        if (result == ESP_OK) {
            result = nvs_set_u8(handle, sound_key, enabled);
            if (result == ESP_OK) result = nvs_commit(handle);
            nvs_close(handle);
        }
        preference_known = result == ESP_OK;
        if (result != ESP_OK) return result;
        enabled_saved = enabled;
    }
    if (enabled) start_output();
    return ESP_OK;
}

passport_sound_state_t passport_sound_state(void)
{
    portENTER_CRITICAL(&lock);
    passport_sound_state_t snapshot = {.output = output, .enabled = enabled_saved,
        .preference_known = preference_known, .cue = cue_receipt};
    portEXIT_CRITICAL(&lock);
    return snapshot;
}

static bool queue_cue(passport_cue_t cue, uint32_t id)
{
    if (cue < PASSPORT_CUE_CONFIRM || cue > PASSPORT_CUE_ERROR) return false;
    portENTER_CRITICAL(&lock);
    bool queued = output == PASSPORT_SOUND_READY && !pending && (!id || cue_receipt.result != PR_PENDING);
    if (queued) {
        pending = cue;
        pending_id = id;
        if (id) cue_receipt = (passport_receipt_t){id, PR_PENDING, 0};
        xTaskNotifyGive(worker);
    }
    portEXIT_CRITICAL(&lock);
    return queued;
}
bool passport_sound_cue(passport_cue_t cue) { return queue_cue(cue, 0); }
bool passport_sound_cue_usb(passport_cue_t cue, uint32_t id) { return id && queue_cue(cue, id); }
