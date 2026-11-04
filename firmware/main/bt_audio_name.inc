// SPDX-License-Identifier: Apache-2.0
/* The Classic GAP callback owns the single local-name readback stream.
 * The GET completion after SET fences earlier EIR notifications. Only a new
 * complete-name EIR observation after that read can finish the request. */
typedef enum { AUDIO_NAME_IDLE, AUDIO_NAME_START, AUDIO_NAME_READ,
    AUDIO_NAME_CONFIGURE_EIR, AUDIO_NAME_WAIT_EIR } audio_name_phase_t;
static struct {
    bt_audio_name_change_t result;
    audio_name_phase_t phase;
    uint64_t started_us;
    bool eir_complete, eir_failed;
    char target[BT_AUDIO_DEVICE_NAME_MAX_BYTES + 1U];
} classic_name;

esp_err_t bt_audio_name_request(const char *target, uint64_t *id)
{
    if (!target || !id || !*target || strlen(target) > BT_AUDIO_DEVICE_NAME_MAX_BYTES) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&audio_lock);
    bool allowed = audio.submitted && !audio.public.failed && !classic_name.result.busy &&
        !classic_name.result.uncertain && classic_name.result.request_id != UINT64_MAX;
    if (allowed) {
        *id = ++classic_name.result.request_id;
        classic_name.result.busy = true;
        classic_name.result.error = ESP_OK;
        strcpy(classic_name.target, target);
        classic_name.started_us = now_us();
        classic_name.phase = AUDIO_NAME_START;
    }
    portEXIT_CRITICAL(&audio_lock);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bt_audio_name_change_t bt_audio_name_change(void)
{
    portENTER_CRITICAL(&audio_lock);
    bt_audio_name_change_t result = classic_name.result;
    result.available = audio.submitted && !audio.public.failed && !result.uncertain;
    portEXIT_CRITICAL(&audio_lock);
    return result;
}

#if AUDIO_PROFILES_SUPPORTED
static void classic_name_done_locked(esp_err_t error, bool uncertain)
{
    classic_name.result.busy = false;
    classic_name.result.error = error;
    classic_name.result.uncertain = uncertain;
    classic_name.phase = AUDIO_NAME_IDLE;
}

static bool classic_name_expired_locked(void)
{
    if (!classic_name.result.busy || now_us() - classic_name.started_us < BT_AUDIO_INIT_TIMEOUT_US) return false;
    classic_name_done_locked(ESP_ERR_TIMEOUT, true);
    return true;
}

static void service_classic_name(void)
{
    char target[sizeof(classic_name.target)];
    audio_name_phase_t issue = AUDIO_NAME_IDLE;
    portENTER_CRITICAL(&audio_lock);
    classic_name_expired_locked();
    if (classic_name.phase == AUDIO_NAME_START && !audio.name_pending) {
        issue = AUDIO_NAME_START;
        memcpy(target, classic_name.target, sizeof(target));
        audio.name_pending = true;
        audio.name_started_us = now_us();
        audio.public.device_name_known = false;
        classic_name.phase = AUDIO_NAME_READ;
    } else if (classic_name.phase == AUDIO_NAME_CONFIGURE_EIR) {
        issue = AUDIO_NAME_CONFIGURE_EIR;
        classic_name.phase = AUDIO_NAME_WAIT_EIR;
        classic_name.eir_complete = classic_name.eir_failed = false;
        audio.name_pending = true;
        audio.name_started_us = now_us();
    }
    portEXIT_CRITICAL(&audio_lock);
    esp_err_t err = ESP_OK;
    if (issue == AUDIO_NAME_START) {
        err = esp_bt_gap_set_device_name(target);
        if (err == ESP_OK) err = esp_bt_gap_get_device_name();
    } else if (issue == AUDIO_NAME_CONFIGURE_EIR) {
        esp_bt_eir_data_t eir = {.include_name = true, .include_uuid = true};
        err = esp_bt_gap_config_eir_data(&eir);
        /* BTC/BTA also emits EIR events for internal UUID/name updates.
         * An ordered GET after configuration bounds the observation window. */
        if (err == ESP_OK) err = esp_bt_gap_get_device_name();
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&audio_lock);
        audio.name_pending = false;
        audio.public.device_name_error = err;
        if (classic_name.result.busy && !classic_name_expired_locked()) classic_name_done_locked(err, false);
        portEXIT_CRITICAL(&audio_lock);
    }
}

static void classic_name_read_locked(void)
{
    if (classic_name_expired_locked()) return;
    if (classic_name.phase == AUDIO_NAME_WAIT_EIR) {
        bool verified = audio.public.device_name_known && !strcmp(audio.public.device_name, classic_name.target) &&
            classic_name.eir_complete && !classic_name.eir_failed;
        classic_name_done_locked(verified ? ESP_OK : ESP_FAIL, false);
        return;
    }
    if (classic_name.phase != AUDIO_NAME_READ) return;
    if (audio.public.device_name_known && !strcmp(audio.public.device_name, classic_name.target))
        classic_name.phase = AUDIO_NAME_CONFIGURE_EIR;
    else classic_name_done_locked(audio.public.device_name_error != ESP_OK ?
        audio.public.device_name_error : ESP_ERR_INVALID_STATE, false);
}

static void classic_name_eir_locked(const esp_bt_gap_cb_param_t *p)
{
    if (classic_name_expired_locked() || classic_name.phase != AUDIO_NAME_WAIT_EIR) return;
    bool complete = false;
    for (unsigned i = 0; i < p->config_eir_data.eir_type_num && i < ESP_BT_EIR_TYPE_MAX_NUM; ++i)
        if (p->config_eir_data.eir_type[i] == ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME) complete = true;
    if (p->config_eir_data.stat == ESP_BT_STATUS_SUCCESS && complete) classic_name.eir_complete = true;
    else classic_name.eir_failed = true;
}
#endif
