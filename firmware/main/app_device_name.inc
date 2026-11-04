// SPDX-License-Identifier: Apache-2.0
/* Coordinator-owned transaction. No settings write occurs until both radio
 * owners confirm the same base. A prior confirmed pair is restored on a
 * recoverable partial failure. Unknown completions cannot authorize a retry. */
typedef struct {
    uint64_t request_id;
    ble_hid_name_status_t status;
    esp_err_t error;
    bool active_known;
    char requested[DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES + 1U];
    char active[DEVICE_SETTINGS_BLUETOOTH_NAME_MAX_BYTES + 1U];
} base_name_change_t;
static struct {
    base_name_change_t base;
    uint64_t input_id, audio_id;
    bool restoring;
} paired_name;

static bool device_name_busy(void)
{
    return paired_name.base.status == BLE_HID_NAME_PENDING || paired_name.base.status == BLE_HID_NAME_APPLYING;
}

static esp_err_t request_input_name(const char *base, uint64_t *id)
{
    char input[DEVICE_INPUT_NAME_MAX + 1U];
    snprintf(input, sizeof(input), "%s%s", base, DEVICE_INPUT_NAME_SUFFIX);
    return ble_hid_name_request(input, strlen(input), id);
}

static esp_err_t request_device_base(const char *base, size_t length, uint64_t *id)
{
    if (!device_settings_bluetooth_name_valid(base, length) || base[length] != 0) return ESP_ERR_INVALID_ARG;
    if (device_name_busy() || paired_name.base.status == BLE_HID_NAME_UNCERTAIN) return ESP_ERR_INVALID_STATE;
    if (id && !bt_audio_name_change().available) return ESP_ERR_INVALID_STATE;
    base = device_base_name(base);
    uint64_t input_id;
    esp_err_t error = request_input_name(base, &input_id);
    if (error != ESP_OK) return error;
    paired_name.input_id = paired_name.base.request_id = input_id;
    paired_name.audio_id = 0;
    paired_name.restoring = false;
    paired_name.base.status = BLE_HID_NAME_PENDING;
    paired_name.base.error = ESP_OK;
    strcpy(paired_name.base.requested, base);
    if (id) *id = input_id;
    return ESP_OK;
}

static void device_name_failure(esp_err_t error, bool uncertain, bool input_changed)
{
    if (!paired_name.restoring) paired_name.base.error = error;
    if (paired_name.restoring || uncertain || (input_changed && !paired_name.base.active_known)) {
        paired_name.base.status = BLE_HID_NAME_UNCERTAIN;
        paired_name.base.active_known = false;
    } else if (input_changed) {
        if (request_input_name(paired_name.base.active, &paired_name.input_id) == ESP_OK) {
            paired_name.restoring = true;
            paired_name.audio_id = 0;
        } else {
            paired_name.base.status = BLE_HID_NAME_UNCERTAIN;
            paired_name.base.active_known = false;
        }
    } else paired_name.base.status = BLE_HID_NAME_FAILED;
}

static void service_device_base(void)
{
    if (!device_name_busy()) return;
    const char *base = paired_name.restoring ? paired_name.base.active : paired_name.base.requested;
    paired_name.base.status = BLE_HID_NAME_APPLYING;
    if (!paired_name.audio_id) {
        ble_hid_name_snapshot_t input = ble_hid_name_snapshot();
        if (input.request_id != paired_name.input_id) {
            device_name_failure(ESP_ERR_INVALID_STATE, true, false); return;
        }
        if (input.status == BLE_HID_NAME_FAILED || input.status == BLE_HID_NAME_UNCERTAIN) {
            device_name_failure(input.error, input.status == BLE_HID_NAME_UNCERTAIN, false); return;
        }
        if (input.status != BLE_HID_NAME_APPLIED) return;
        char expected[DEVICE_INPUT_NAME_MAX + 1U], audio[DEVICE_AUDIO_NAME_MAX + 1U];
        snprintf(expected, sizeof(expected), "%s%s", base, DEVICE_INPUT_NAME_SUFFIX);
        if (!input.active_known || strcmp(input.active, expected)) {
            device_name_failure(ESP_ERR_INVALID_STATE, true, false); return;
        }
        snprintf(audio, sizeof(audio), "%s%s", base, DEVICE_AUDIO_NAME_SUFFIX);
        esp_err_t error = bt_audio_name_request(audio, &paired_name.audio_id);
        if (error != ESP_OK) device_name_failure(error, false, true);
        return;
    }
    bt_audio_name_change_t audio = bt_audio_name_change();
    if (audio.request_id != paired_name.audio_id) {
        device_name_failure(ESP_ERR_INVALID_STATE, true, true); return;
    }
    if (audio.busy) return;
    if (audio.error != ESP_OK || audio.uncertain) device_name_failure(audio.error, audio.uncertain, true);
    else if (paired_name.restoring) paired_name.base.status = BLE_HID_NAME_FAILED;
    else {
        strcpy(paired_name.base.active, base);
        paired_name.base.active_known = true;
        paired_name.base.status = BLE_HID_NAME_APPLIED;
    }
}
