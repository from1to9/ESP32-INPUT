// SPDX-License-Identifier: Apache-2.0
/* Shares the settings worker and NVS handle. The existing v5 record is also
 * readable by the rollback image, which ignores this independent key. */
_Static_assert(sizeof(app_boot_choices_t) == 11, "resume_v1 wire layout");
static app_boot_choices_t boot_requested, boot_saved;
static bool boot_readable, boot_dirty, boot_durable;
static settings_store_status_t boot_status;

static bool boot_choices_valid(const app_boot_choices_t *v)
{
    const app_boot_audio_t *a = &v->audio;
    const uint8_t empty[6] = {0};
    if (v->mode > 2 || a->route > 4 || (a->services | a->disconnected) > 3 ||
        (a->services & a->disconnected)) return false;
    if (!memcmp(a->peer, empty, 6) && (a->services || a->disconnected || a->speaker)) return false;
    return !a->speaker || ((a->route == 2 || a->route == 3) &&
        ((a->speaker == 2 && (a->services & APP_RESUME_HFP)) ||
         (a->speaker == 3 && (a->services & APP_RESUME_A2DP))));
}

static void load_boot_choices(void)
{
    app_boot_choices_t v = {0};
    size_t length = sizeof(v);
    esp_err_t error = nvs_get_blob(s_handle, "resume_v1", &v, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) { v = (app_boot_choices_t){0}; error = ESP_OK; }
    else if (error == ESP_OK) {
        if (length != sizeof(v) || !boot_choices_valid(&v)) error = ESP_ERR_INVALID_ARG;
        else boot_status.loaded = true;
    }
    boot_status.error = error;
    if (error == ESP_OK) {
        boot_requested = boot_saved = v;
        boot_readable = boot_durable = true;
    }
}

settings_store_status_t settings_store_boot_read(app_boot_choices_t *value)
{
    portENTER_CRITICAL(&s_lock);
    settings_store_status_t status = boot_status;
    status.available = s_status.available && boot_readable;
    if (value) *value = status.available ? boot_requested : (app_boot_choices_t){0};
    portEXIT_CRITICAL(&s_lock);
    return status;
}

esp_err_t settings_store_boot_request(const app_boot_choices_t *value)
{
    if (!value || !boot_choices_valid(value)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (!s_status.available || !boot_readable || boot_status.requested_revision == UINT64_MAX)
        error = ESP_ERR_INVALID_STATE;
    else if (memcmp(&boot_requested, value, sizeof(*value)) || (!boot_status.pending && !boot_durable)) {
        boot_requested = *value;
        ++boot_status.requested_revision;
        boot_dirty = boot_status.pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

static void save_boot_choices(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!boot_dirty) { portEXIT_CRITICAL(&s_lock); return; }
    app_boot_choices_t value = boot_requested;
    uint64_t revision = boot_status.requested_revision;
    bool same = boot_durable && !memcmp(&boot_saved, &value, sizeof(value));
    boot_dirty = false;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (!same) {
        error = nvs_set_blob(s_handle, "resume_v1", &value, sizeof(value));
        if (error == ESP_OK) error = nvs_commit(s_handle);
    }
    portENTER_CRITICAL(&s_lock);
    boot_durable = error == ESP_OK;
    boot_status.error = error;
    if (boot_durable) { boot_saved = value; boot_status.saved_revision = revision; }
    boot_status.pending = boot_dirty;
    portEXIT_CRITICAL(&s_lock);
}
