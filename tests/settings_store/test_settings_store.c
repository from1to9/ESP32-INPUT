// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* Exercise the actual worker, request admission and status code. Only SDK
 * storage/task/clock boundaries are replaced; the record codec is linked. */
#include "../../firmware/main/settings_store.c"

typedef struct {
    int64_t now_us;
    unsigned critical_depth;
    bool handle_open, exists, staged, task_ok, worker_running;
    bool commit_applies_on_error;
    uint8_t durable[64], staging[64];
    size_t durable_length, staging_length;
    esp_err_t open_error, get_error, set_error, commit_error;
    unsigned opens, gets, sets, commits, closes, tasks, delays;
    app_boot_choices_t boot_data, boot_staging;
    bool boot_exists, staging_boot;
    esp_err_t boot_read_error;
    unsigned boot_writes;
    TaskFunction_t worker;
    void *worker_argument;
    void (*commit_hook)(void);
} storage_double_t;

static storage_double_t hw;
static jmp_buf worker_exit;
static device_settings_t hook_settings;
static settings_store_status_t hook_status;

void settings_test_enter(portMUX_TYPE *lock)
{
    assert(lock && hw.critical_depth == 0);
    ++hw.critical_depth;
}

void settings_test_exit(portMUX_TYPE *lock)
{
    assert(lock && hw.critical_depth == 1);
    --hw.critical_depth;
}

int64_t esp_timer_get_time(void)
{
    assert(hw.critical_depth == 0);
    return hw.now_us;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(hw.critical_depth == 0 && !hw.handle_open && handle);
    assert(strcmp(name, "input_prefs") == 0 && mode == NVS_READWRITE);
    ++hw.opens;
    if (hw.open_error != ESP_OK) return hw.open_error;
    *handle = 41;
    hw.handle_open = true;
    return ESP_OK;
}

static void check_handle(nvs_handle_t handle)
{
    assert(hw.critical_depth == 0 && hw.handle_open && handle == 41);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    check_handle(handle);
    if (!strcmp(key, "resume_v1")) {
        if (hw.boot_read_error) return hw.boot_read_error;
        if (!hw.boot_exists) return ESP_ERR_NVS_NOT_FOUND;
        assert(*length >= sizeof(hw.boot_data)); memcpy(value, &hw.boot_data, sizeof(hw.boot_data));
        *length = sizeof(hw.boot_data); return ESP_OK;
    }
    assert(strcmp(key, "record") == 0 && value && length);
    ++hw.gets;
    if (hw.get_error != ESP_OK) return hw.get_error;
    if (!hw.exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < hw.durable_length) {
        *length = hw.durable_length;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(value, hw.durable, hw.durable_length);
    *length = hw.durable_length;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    check_handle(handle);
    if (!strcmp(key, "resume_v1")) {
        assert(length == sizeof(hw.boot_staging)); ++hw.boot_writes;
        if (hw.set_error) return hw.set_error;
        memcpy(&hw.boot_staging, value, length); hw.staged = hw.staging_boot = true; return ESP_OK;
    }
    hw.staging_boot = false;
    assert(strcmp(key, "record") == 0 && value && length == DEVICE_SETTINGS_RECORD_SIZE);
    assert(length <= sizeof hw.staging);
    ++hw.sets;
    if (hw.set_error != ESP_OK) return hw.set_error;
    memcpy(hw.staging, value, length);
    hw.staging_length = length;
    hw.staged = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    check_handle(handle);
    assert(hw.staged);
    ++hw.commits;
    if (hw.staging_boot) {
        if (!hw.commit_error || hw.commit_applies_on_error) { hw.boot_data = hw.boot_staging; hw.boot_exists = true; }
        hw.staged = hw.staging_boot = false;
        return hw.commit_error;
    }
    if (hw.commit_error == ESP_OK || hw.commit_applies_on_error) {
        memcpy(hw.durable, hw.staging, hw.staging_length);
        hw.durable_length = hw.staging_length;
        hw.exists = true;
        hw.staged = false;
    }
    if (hw.commit_hook) {
        void (*hook)(void) = hw.commit_hook;
        hw.commit_hook = NULL;
        hook();
    }
    return hw.commit_error;
}

void nvs_close(nvs_handle_t handle)
{
    check_handle(handle);
    ++hw.closes;
    hw.handle_open = false;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_size,
                      void *argument, UBaseType_t priority, void *handle)
{
    assert(hw.critical_depth == 0 && task && name && stack_size && priority);
    (void)handle;
    ++hw.tasks;
    if (!hw.task_ok) return pdFAIL;
    assert(!hw.worker);
    hw.worker = task;
    hw.worker_argument = argument;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    assert(hw.critical_depth == 0 && hw.worker_running && ticks > 0);
    ++hw.delays;
    longjmp(worker_exit, 1);
}

static void reset_runtime(void)
{
    boot_status = (settings_store_status_t){0};
    boot_requested = boot_saved = (app_boot_choices_t){0};
    boot_dirty = boot_durable = boot_readable = false;
    s_status = (settings_store_status_t){0};
    s_started = s_dirty = s_saved_known = false;
    memset(s_requested, 0, sizeof s_requested);
    memset(s_saved, 0, sizeof s_saved);
    s_changed_us = 0;
    s_handle = 0;
    s_lock = portMUX_INITIALIZER_UNLOCKED;
    hw.critical_depth = 0;
    hw.handle_open = false;
    hw.worker = NULL;
    hw.worker_argument = NULL;
    hw.worker_running = false;
    hw.staged = false; /* Simulated reboot discards uncommitted staging. */
}

static void reset(void)
{
    hw = (storage_double_t){.now_us = 10000000, .task_ok = true};
    reset_runtime();
}

static device_settings_t defaults(void)
{
    device_settings_t settings;
    device_settings_defaults(&settings);
    return settings;
}

static void assert_defaults(const device_settings_t *settings)
{
    assert(settings->theme == DEVICE_THEME_DARK);
    assert(settings->host_modifier == DEVICE_HOST_CTRL && settings->haptics_enabled);
    assert(settings->brightness_percent == 50 && settings->idle_timeout == DEVICE_IDLE_5_MIN);
    assert(settings->preferred_transport == DEVICE_TRANSPORT_BLUETOOTH);
    assert(settings->tap_to_click && strcmp(settings->bluetooth_name, "ESP32") == 0);
}

static device_settings_t alternative(void)
{
    return (device_settings_t){
        .theme = DEVICE_THEME_LIGHT, .host_modifier = DEVICE_HOST_COMMAND,
        .haptics_enabled = false, .brightness_percent = 10, .idle_timeout = DEVICE_IDLE_OFF,
        .preferred_transport = DEVICE_TRANSPORT_USB,
        .tap_to_click = false, .bluetooth_name = "Test desktop",
        .motion_gain_x_centi = 650, .motion_gain_y_centi = 225,
        .motion_deadzone_mdps = 250, .motion_filter_ms = 20, .motion_freeze_ms = 75,
        .motion_invert_x = true
    };
}

static void seed(const device_settings_t *settings)
{
    assert(device_settings_encode(settings, hw.durable, DEVICE_SETTINGS_RECORD_SIZE) ==
           DEVICE_SETTINGS_OK);
    hw.durable_length = DEVICE_SETTINGS_RECORD_SIZE;
    hw.exists = true;
}

static void assert_durable(const device_settings_t *settings)
{
    uint8_t expected[DEVICE_SETTINGS_RECORD_SIZE];
    assert(device_settings_encode(settings, expected, sizeof expected) == DEVICE_SETTINGS_OK);
    assert(hw.exists && hw.durable_length == sizeof expected);
    assert(memcmp(hw.durable, expected, sizeof expected) == 0);
}

static void worker_at(int64_t now_us)
{
    assert(hw.worker && !hw.worker_running);
    hw.now_us = now_us;
    hw.worker_running = true;
    if (setjmp(worker_exit) == 0) {
        hw.worker(hw.worker_argument);
        assert(0 && "The settings worker must yield rather than return");
    }
    hw.worker_running = false;
    assert(hw.critical_depth == 0);
}

static void init_missing(void)
{
    device_settings_t settings = alternative();
    assert(settings_store_init(&settings) == ESP_OK);
    assert_defaults(&settings);
}

static void missing_defaults_do_not_write(void)
{
    reset();
    assert(settings_store_init(NULL) == ESP_ERR_INVALID_ARG && hw.opens == 0);
    init_missing();
    settings_store_status_t status = settings_store_status();
    assert(status.available && !status.loaded && !status.pending && status.error == ESP_OK);
    assert(status.requested_revision == 0 && status.saved_revision == 0);
    for (int64_t i = 0; i < 100; ++i) worker_at(10000000 + i * 1000000);
    assert(hw.opens == 1 && hw.gets == 1 && hw.tasks == 1);
    assert(hw.sets == 0 && hw.commits == 0 && !hw.exists);
    device_settings_t untouched = alternative();
    assert(settings_store_init(&untouched) == ESP_ERR_INVALID_STATE);
    assert(untouched.theme == DEVICE_THEME_LIGHT && untouched.idle_timeout == DEVICE_IDLE_OFF);
    status.error = ESP_FAIL;
    status.available = false;
    assert(settings_store_status().available && settings_store_status().error == ESP_OK);
}

static void valid_load_and_unchanged_requests_do_not_write(void)
{
    const uint8_t chosen_brightness[] = {10, 70};
    for (size_t choice = 0; choice < sizeof chosen_brightness / sizeof chosen_brightness[0]; ++choice) {
        reset();
        device_settings_t expected = alternative();
        expected.brightness_percent = chosen_brightness[choice];
        seed(&expected);
        device_settings_t loaded;
        assert(settings_store_init(&loaded) == ESP_OK);
        assert(loaded.theme == DEVICE_THEME_LIGHT && loaded.host_modifier == DEVICE_HOST_COMMAND);
        assert(!loaded.haptics_enabled && loaded.brightness_percent == chosen_brightness[choice]);
        assert(loaded.idle_timeout == DEVICE_IDLE_OFF && settings_store_status().loaded);
        assert(device_settings_equal(&loaded, &expected));
        for (int64_t i = 0; i < 1000; ++i) {
            hw.now_us = 10000000 + i * 1000000;
            assert(settings_store_request(&loaded) == ESP_OK);
            worker_at(hw.now_us);
        }
        settings_store_status_t status = settings_store_status();
        assert(!status.pending && status.error == ESP_OK);
        assert(status.requested_revision == 0 && status.saved_revision == 0);
        assert(hw.sets == 0 && hw.commits == 0 && hw.gets == 1);
        assert_durable(&expected);
    }
}

static void bad_record_lengths_are_preserved(void)
{
    const size_t lengths[] = {0, DEVICE_SETTINGS_RECORD_SIZE - 1, DEVICE_SETTINGS_RECORD_SIZE + 1};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        reset();
        memset(hw.durable, 0xa5, sizeof hw.durable);
        hw.exists = true;
        hw.durable_length = lengths[i];
        device_settings_t settings = alternative();
        const esp_err_t expected = lengths[i] > DEVICE_SETTINGS_RECORD_SIZE ?
            ESP_ERR_NVS_INVALID_LENGTH : ESP_ERR_INVALID_ARG;
        assert(settings_store_init(&settings) == expected);
        assert_defaults(&settings);
        settings_store_status_t status = settings_store_status();
        assert(status.available && !status.loaded && status.error == expected && !status.pending);
        worker_at(20000000);
        assert(hw.sets == 0 && hw.commits == 0 && hw.durable_length == lengths[i]);
        for (size_t byte = 0; byte < sizeof hw.durable; ++byte) assert(hw.durable[byte] == 0xa5);
    }
}

static void startup_boundary_failures_are_visible(void)
{
    reset();
    hw.open_error = ESP_FAIL;
    device_settings_t settings = alternative();
    assert(settings_store_init(&settings) == ESP_FAIL);
    assert_defaults(&settings);
    assert(!settings_store_status().available && settings_store_status().error == ESP_FAIL);
    assert(hw.gets == 0 && hw.tasks == 0 && hw.closes == 0);
    assert(settings_store_request(&settings) == ESP_ERR_INVALID_STATE);

    reset();
    device_settings_t existing = alternative();
    seed(&existing);
    hw.get_error = ESP_FAIL;
    assert(settings_store_init(&settings) == ESP_FAIL);
    assert_defaults(&settings);
    assert(settings_store_status().available && !settings_store_status().loaded);
    assert(settings_store_status().error == ESP_FAIL);
    worker_at(20000000);
    assert_durable(&existing);
    assert(hw.sets == 0 && hw.commits == 0);

    reset();
    seed(&existing);
    hw.task_ok = false;
    assert(settings_store_init(&settings) == ESP_ERR_NO_MEM);
    assert(!settings_store_status().available && settings_store_status().loaded);
    assert(settings_store_status().error == ESP_ERR_NO_MEM);
    assert(hw.closes == 1 && !hw.handle_open && !hw.worker);
    assert(settings_store_request(&existing) == ESP_ERR_INVALID_STATE);
    assert(hw.sets == 0 && hw.commits == 0);
}

static void corrupt_and_unsupported_records_are_preserved(void)
{
    const device_settings_status_t expected[] = {
        DEVICE_SETTINGS_UNSUPPORTED_VERSION, DEVICE_SETTINGS_INVALID_CHECKSUM,
        DEVICE_SETTINGS_INVALID_MAGIC, DEVICE_SETTINGS_INVALID_LENGTH
    };
    for (size_t fault = 0; fault < sizeof expected / sizeof expected[0]; ++fault) {
        reset();
        device_settings_t stored = alternative();
        seed(&stored);
        /* V4 is 56 bytes: version at 4, length at 5 and CRC at 52..55.
         * Confirm the fixture's intended rejection before testing NVS. */
        if (fault == 0) hw.durable[4] = 255;
        if (fault == 1) hw.durable[55] ^= 0x80;
        if (fault == 2) hw.durable[0] = 'X';
        if (fault == 3) hw.durable[5] = 15;
        uint8_t original[DEVICE_SETTINGS_RECORD_SIZE];
        memcpy(original, hw.durable, sizeof original);
        device_settings_t decoded = stored;
        assert(device_settings_decode(original, sizeof original, &decoded) == expected[fault]);
        device_settings_t loaded = stored;
        assert(settings_store_init(&loaded) == ESP_ERR_INVALID_ARG);
        assert_defaults(&loaded);
        settings_store_status_t status = settings_store_status();
        assert(status.available && !status.loaded && !status.pending);
        assert(status.error == ESP_ERR_INVALID_ARG && status.requested_revision == 0);
        worker_at(20000000);
        worker_at(30000000);
        assert(hw.sets == 0 && hw.commits == 0);
        assert(memcmp(hw.durable, original, sizeof original) == 0);
        /* A later explicit setting change may replace the rejected record. */
        assert(settings_store_request(&stored) == ESP_OK);
        assert(settings_store_status().error == ESP_ERR_INVALID_ARG);
        worker_at(31000000);
        assert_durable(&stored);
        assert(settings_store_status().error == ESP_OK);
    }
}

static void invalid_requests_cannot_mutate_pending_state(void)
{
    reset();
    init_missing();
    device_settings_t wanted = alternative();
    assert(settings_store_request(&wanted) == ESP_OK);
    assert(settings_store_request(NULL) == ESP_ERR_INVALID_ARG);
    for (unsigned fault = 0; fault < 8; ++fault) {
        device_settings_t invalid = wanted;
        if (fault == 0) invalid.theme = (device_theme_t)99;
        if (fault == 1) invalid.host_modifier = (device_host_modifier_t)99;
        if (fault == 2) invalid.brightness_percent = 0;
        if (fault == 3) invalid.idle_timeout = (device_idle_timeout_t)2;
        if (fault == 4) invalid.preferred_transport = (device_transport_t)2;
        if (fault == 5) invalid.bluetooth_name[0] = '\0';
        if (fault == 6) strcpy(invalid.bluetooth_name, " Leading");
        if (fault == 7) memset(invalid.bluetooth_name, 'X', sizeof invalid.bluetooth_name);
        assert(settings_store_request(&invalid) == ESP_ERR_INVALID_ARG);
        assert(settings_store_status().requested_revision == 1);
    }
    worker_at(11000000);
    assert_durable(&wanted);
    assert(settings_store_status().saved_revision == 1);
}

static void quiet_deadline_and_latest_value_coalescing(void)
{
    reset();
    init_missing();
    device_settings_t a = alternative();
    assert(settings_store_request(&a) == ESP_OK);
    worker_at(10999999);
    assert(hw.sets == 0 && hw.commits == 0 && settings_store_status().pending);
    assert(settings_store_request(&a) == ESP_OK); /* Duplicate must not postpone the save. */
    assert(settings_store_status().requested_revision == 1);
    worker_at(11000000);
    assert(hw.sets == 1 && hw.commits == 1 && !settings_store_status().pending);
    assert_durable(&a);

    reset();
    init_missing();
    device_settings_t b = a;
    b.brightness_percent = 90;
    assert(settings_store_request(&a) == ESP_OK);
    hw.now_us = 10500000;
    assert(settings_store_request(&b) == ESP_OK);
    worker_at(11000000);
    worker_at(11499999);
    assert(hw.sets == 0 && settings_store_status().requested_revision == 2);
    worker_at(11500000);
    assert(hw.sets == 1 && hw.commits == 1);
    assert(settings_store_status().saved_revision == 2 && !settings_store_status().pending);
    assert_durable(&b);
    reset_runtime();
    device_settings_t loaded;
    assert(settings_store_init(&loaded) == ESP_OK);
    assert(loaded.brightness_percent == 90 && loaded.idle_timeout == DEVICE_IDLE_OFF);
    assert(settings_store_status().loaded && hw.sets == 1 && hw.commits == 1);
}

static void returning_to_durable_value_avoids_write(void)
{
    reset();
    device_settings_t a = defaults(), b = alternative(), loaded;
    seed(&a);
    assert(settings_store_init(&loaded) == ESP_OK);
    assert(settings_store_request(&b) == ESP_OK);
    hw.now_us += 500000;
    assert(settings_store_request(&a) == ESP_OK);
    worker_at(11500000);
    assert(hw.sets == 0 && hw.commits == 0);
    settings_store_status_t status = settings_store_status();
    assert(status.requested_revision == 2 && status.saved_revision == 2);
    assert(!status.pending && status.error == ESP_OK);
    assert_durable(&a);
}

static void prolonged_changes_coalesce_without_frame_writes(void)
{
    reset();
    init_missing();
    device_settings_t wanted = alternative();
    for (unsigned i = 0; i < 1000; ++i) {
        hw.now_us += 10000;
        wanted.brightness_percent = (uint8_t)(10U + 10U * (i % 10U));
        assert(settings_store_request(&wanted) == ESP_OK);
        worker_at(hw.now_us);
        assert(hw.sets == 0 && hw.commits == 0);
    }
    assert(settings_store_status().requested_revision == 1000);
    const int64_t last_change = hw.now_us;
    worker_at(last_change + 999999);
    assert(hw.sets == 0);
    worker_at(last_change + 1000000);
    assert(hw.sets == 1 && hw.commits == 1 && hw.tasks == 1);
    assert(settings_store_status().saved_revision == 1000);
    assert_durable(&wanted);
    for (unsigned i = 0; i < 1000; ++i) {
        hw.now_us += 1000000;
        assert(settings_store_request(&wanted) == ESP_OK);
        worker_at(hw.now_us);
    }
    assert(hw.sets == 1 && hw.commits == 1 && settings_store_status().requested_revision == 1000);
}

static void request_inside_commit(void)
{
    hook_status = settings_store_status();
    assert(hook_status.pending);
    assert(settings_store_request(&hook_settings) == ESP_OK);
}

static void new_request_during_commit_is_not_lost(void)
{
    for (unsigned same_value = 0; same_value < 2; ++same_value) {
        reset();
        init_missing();
        device_settings_t a = alternative();
        hook_settings = a;
        if (!same_value) hook_settings.brightness_percent = 90;
        assert(settings_store_request(&a) == ESP_OK);
        hw.commit_hook = request_inside_commit;
        worker_at(11000000);
        settings_store_status_t status = settings_store_status();
        assert(hook_status.requested_revision == 1 && hook_status.saved_revision == 0);
        assert(status.pending && status.requested_revision == 2 && status.saved_revision == 1);
        assert_durable(&a);
        worker_at(11999999);
        assert(hw.commits == 1);
        worker_at(12000000);
        status = settings_store_status();
        assert(!status.pending && status.saved_revision == 2 && status.error == ESP_OK);
        assert(hw.sets == (same_value ? 1U : 2U) && hw.commits == hw.sets);
        assert_durable(&hook_settings);
    }
}

static void save_failure_waits_for_explicit_retry(void)
{
    for (unsigned fail_commit = 0; fail_commit < 2; ++fail_commit) {
        reset();
        device_settings_t a = defaults(), b = alternative(), loaded;
        seed(&a);
        assert(settings_store_init(&loaded) == ESP_OK);
        assert(settings_store_request(&b) == ESP_OK);
        if (fail_commit) hw.commit_error = ESP_FAIL;
        else hw.set_error = ESP_FAIL;
        worker_at(11000000);
        settings_store_status_t status = settings_store_status();
        assert(status.available && !status.pending && status.error == ESP_FAIL);
        assert(status.requested_revision == 1 && status.saved_revision == 0);
        assert(hw.sets == 1 && hw.commits == fail_commit);
        assert_durable(&a);
        hw.set_error = hw.commit_error = ESP_OK;
        for (int64_t i = 0; i < 100; ++i) worker_at(12000000 + i * 1000000);
        assert(hw.sets == 1 && hw.commits == fail_commit);
        assert(settings_store_status().error == ESP_FAIL);
        const int64_t retry_at = hw.now_us;
        assert(settings_store_request(&b) == ESP_OK);
        assert(settings_store_status().requested_revision == 2);
        assert(settings_store_status().error == ESP_FAIL); /* Acceptance is not saved. */
        worker_at(retry_at + 999999);
        assert(hw.sets == 1);
        worker_at(retry_at + 1000000);
        assert_durable(&b);
        status = settings_store_status();
        assert(!status.pending && status.error == ESP_OK && status.saved_revision == 2);
    }
}

static void failed_commit_keeps_new_explicit_request(void)
{
    reset();
    init_missing();
    device_settings_t a = alternative();
    hook_settings = a;
    hook_settings.brightness_percent = 90;
    assert(settings_store_request(&a) == ESP_OK);
    hw.commit_error = ESP_FAIL;
    hw.commit_hook = request_inside_commit;
    worker_at(11000000);
    settings_store_status_t status = settings_store_status();
    assert(status.pending && status.error == ESP_FAIL && status.requested_revision == 2);
    assert(status.saved_revision == 0);
    hw.commit_error = ESP_OK;
    worker_at(11999999);
    assert(hw.commits == 1);
    worker_at(12000000);
    assert_durable(&hook_settings);
    assert(settings_store_status().saved_revision == 2 && !settings_store_status().pending);
}

static void backward_clock_restarts_one_quiet_interval(void)
{
    reset();
    init_missing();
    device_settings_t wanted = alternative();
    assert(settings_store_request(&wanted) == ESP_OK);
    worker_at(9000000);
    worker_at(9000000);
    worker_at(9999999);
    assert(hw.sets == 0 && settings_store_status().pending);
    worker_at(10000000);
    assert(hw.commits == 1 && settings_store_status().saved_revision == 1);
    assert_durable(&wanted);
    worker_at(8000000);
    worker_at(20000000);
    assert(hw.commits == 1 && !settings_store_status().pending);
}

static void uncertain_failed_commit_cannot_dedupe_against_old_value(void)
{
    reset();
    device_settings_t a = defaults(), b = alternative(), loaded;
    seed(&a);
    assert(settings_store_init(&loaded) == ESP_OK);
    assert(settings_store_request(&b) == ESP_OK);
    /* A storage error is not proof of rollback. The pinned simple NVS handle
     * writes before commit (commit is currently a no-op), so model the outcome
     * where B is durable even though the final API result reports failure. */
    hw.commit_error = ESP_FAIL;
    hw.commit_applies_on_error = true;
    worker_at(11000000);
    assert_durable(&b);
    assert(settings_store_status().error == ESP_FAIL);
    hw.commit_error = ESP_OK;
    assert(settings_store_request(&a) == ESP_OK);
    worker_at(12000000);
    assert(hw.sets == 2 && hw.commits == 2); /* Old cached A is no longer evidence. */
    assert_durable(&a);
    assert(settings_store_status().error == ESP_OK && !settings_store_status().pending);
    reset_runtime();
    assert(settings_store_init(&loaded) == ESP_OK);
    assert_defaults(&loaded);
}

static void revision_exhaustion_preserves_pending_value(void)
{
    reset();
    init_missing();
    /* Reach the integer boundary directly rather than simulate 2^64 requests. */
    s_status.requested_revision = UINT64_MAX - 1;
    device_settings_t a = alternative(), b = defaults();
    assert(settings_store_request(&a) == ESP_OK);
    assert(settings_store_status().requested_revision == UINT64_MAX);
    assert(settings_store_request(&b) == ESP_ERR_INVALID_STATE);
    assert(settings_store_status().pending);
    worker_at(11000000);
    assert_durable(&a);
    assert(settings_store_status().saved_revision == UINT64_MAX);
    assert(settings_store_request(&a) == ESP_OK); /* Dedupe remains valid at exhaustion. */
    assert(settings_store_request(&b) == ESP_ERR_INVALID_STATE);
    assert(hw.commits == 1 && !settings_store_status().pending);
}

static void legacy_record_migrates_only_after_explicit_transport_change(void)
{
    /* Frozen independent v1/v2 CRC fixtures include old 70% and custom fields. */
    const uint8_t legacy[][16] = {
        {0x44,0x53,0x45,0x54,1,16,1,0,1,70,5,0,0xfc,0x00,0x35,0x2f},
        {0x44,0x53,0x45,0x54,2,16,1,0,1,70,5,0,0x1f,0x07,0xba,0xa1},
        {0x44,0x53,0x45,0x54,0x02,0x10,0x00,0x01,0x00,0x1e,0x1e,0x01,0x6b,0x1e,0xa4,0x53}
    };
    for (size_t fixture = 0; fixture < sizeof legacy / sizeof legacy[0]; ++fixture) {
        reset();
        memcpy(hw.durable, legacy[fixture], sizeof legacy[fixture]);
        hw.durable_length = sizeof legacy[fixture]; hw.exists = true;
        device_settings_t loaded = alternative(), expected = defaults();
        expected.brightness_percent = 70;
        if (fixture == 2) {
            expected.theme = DEVICE_THEME_LIGHT;
            expected.host_modifier = DEVICE_HOST_COMMAND;
            expected.haptics_enabled = false;
            expected.brightness_percent = 30;
            expected.idle_timeout = DEVICE_IDLE_30_MIN;
            expected.preferred_transport = DEVICE_TRANSPORT_USB;
        }
        assert(settings_store_init(&loaded) == ESP_OK);
        assert(device_settings_equal(&loaded, &expected) && settings_store_status().loaded);
        device_settings_t transient = loaded;
        transient.tap_to_click = false;
        assert(settings_store_request(&transient) == ESP_OK);
        assert(settings_store_request(&loaded) == ESP_OK);
        worker_at(11000000); /* Return to the known logical value: no format-upgrade write. */
        assert(hw.sets == 0 && hw.commits == 0);
        for (int64_t i = 0; i < 1000; ++i) {
            hw.now_us = 11000000 + i * 1000000;
            assert(settings_store_request(&loaded) == ESP_OK);
            worker_at(hw.now_us);
        }
        assert(hw.sets == 0 && hw.commits == 0 && hw.durable_length == 16);
        assert(memcmp(hw.durable, legacy[fixture], sizeof legacy[fixture]) == 0);
        loaded.preferred_transport = loaded.preferred_transport == DEVICE_TRANSPORT_USB ?
            DEVICE_TRANSPORT_BLUETOOTH : DEVICE_TRANSPORT_USB;
        assert(settings_store_request(&loaded) == ESP_OK);
        const int64_t requested = hw.now_us;
        worker_at(requested + 999999);
        assert(hw.sets == 0);
        worker_at(requested + 1000000);
        assert(hw.sets == 1 && hw.commits == 1 && hw.durable_length == 56 && hw.durable[4] == 5);
        reset_runtime();
        device_settings_t reloaded = defaults();
        assert(settings_store_init(&reloaded) == ESP_OK);
        assert(device_settings_equal(&loaded, &reloaded));
    }
}

static void tap_and_name_preferences_persist_without_repeated_writes(void)
{
    reset();
    init_missing();
    device_settings_t wanted = defaults();
    wanted.tap_to_click = false;
    assert(settings_store_request(&wanted) == ESP_OK);
    worker_at(11000000);
    assert_durable(&wanted);
    assert(hw.sets == 1 && hw.commits == 1 && hw.durable[12] == 0);
    strcpy(wanted.bluetooth_name, "Office Pad");
    assert(settings_store_request(&wanted) == ESP_OK);
    worker_at(12000000);
    assert_durable(&wanted);
    assert(hw.sets == 2 && hw.commits == 2);
    for (int64_t i = 0; i < 1000; ++i) {
        hw.now_us = 12000000 + i * 1000000;
        assert(settings_store_request(&wanted) == ESP_OK);
        worker_at(hw.now_us);
    }
    assert(hw.sets == 2 && hw.commits == 2 && !settings_store_status().pending);
    reset_runtime();
    device_settings_t loaded;
    assert(settings_store_init(&loaded) == ESP_OK);
    assert(device_settings_equal(&loaded, &wanted));
    assert(!loaded.tap_to_click && strcmp(loaded.bluetooth_name, "Office Pad") == 0);
}

static const uint8_t v3_tap_off[40] = {
    0x44,0x53,0x45,0x54,0x03,0x28,0x01,0x00,
    0x01,0x32,0x05,0x00,0x00,0x0b,0x45,0x53,
    0x50,0x33,0x32,0x2d,0x49,0x4e,0x50,0x55,
    0x54,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x2b,0x9c,0x05,0x26
};

static void v3_migration_preserves_tap_and_motion_defaults_without_boot_write(void)
{
    reset();
    memcpy(hw.durable, v3_tap_off, sizeof v3_tap_off);
    hw.durable_length = sizeof v3_tap_off; hw.exists = true;
    device_settings_t loaded;
    assert(settings_store_init(&loaded) == ESP_OK);
    assert(!loaded.tap_to_click && loaded.motion_gain_x_centi == 400 && loaded.motion_gain_y_centi == 400);
    assert(settings_store_request(&loaded) == ESP_OK);
    worker_at(10000000);
    assert(hw.sets == 0 && hw.commits == 0 && hw.durable_length == 40);
    device_settings_t changed = loaded;
    changed.motion_gain_x_centi = 825;
    assert(settings_store_request(&changed) == ESP_OK);
    assert(settings_store_request(&loaded) == ESP_OK);
    worker_at(12000000);
    assert(hw.sets == 0 && hw.commits == 0);
    changed.motion_invert_y = true;
    changed.motion_freeze_ms = 0;
    assert(settings_store_request(&changed) == ESP_OK);
    worker_at(14000000);
    assert(hw.sets == 1 && hw.commits == 1 && hw.durable_length == 56);
    assert_durable(&changed);
    reset_runtime();
    assert(settings_store_init(&loaded) == ESP_OK);
    assert(device_settings_equal(&loaded, &changed) && !loaded.tap_to_click);
}

static void boot_choices_round_trip(void)
{
    reset(); device_settings_t old = alternative(); seed(&old);
    device_settings_t current; assert(settings_store_init(&current) == ESP_OK);
    assert(device_settings_equal(&old, &current));
    app_boot_choices_t choices;
    settings_store_status_t status = settings_store_boot_read(&choices);
    assert(status.available && !status.loaded && !choices.mode && !choices.audio.services && !hw.boot_writes);
    choices = (app_boot_choices_t){.mode = 2, .audio = {.route = 3, .peer = {1,2,3,4,5,6}, .services = 3, .speaker = 3}};
    assert(settings_store_boot_request(&choices) == ESP_OK);
    assert(settings_store_boot_read(NULL).pending && !hw.boot_exists);
    worker_at(hw.now_us); assert(hw.boot_writes == 1 && !hw.sets);
    app_boot_choices_t restored; reset_runtime(); assert(settings_store_init(&current) == ESP_OK);
    status = settings_store_boot_read(&restored);
    assert(status.loaded && !memcmp(&restored, &choices, sizeof(choices)) && device_settings_equal(&old, &current));
    assert(settings_store_boot_request(&choices) == ESP_OK); worker_at(hw.now_us);
    assert(hw.boot_writes == 1);
    choices.audio.speaker = 0; hw.commit_error = ESP_FAIL;
    assert(settings_store_boot_request(&choices) == ESP_OK); worker_at(hw.now_us);
    status = settings_store_boot_read(NULL);
    assert(status.error == ESP_FAIL && !status.pending && status.saved_revision != status.requested_revision);
    hw.commit_error = ESP_OK; worker_at(hw.now_us); assert(hw.boot_writes == 2);
    hw.boot_read_error = ESP_FAIL; reset_runtime(); assert(settings_store_init(&current) == ESP_OK);
    status = settings_store_boot_read(&restored);
    assert(!status.available && status.error == ESP_FAIL && !restored.audio.speaker);
    assert(settings_store_boot_request(&choices) == ESP_ERR_INVALID_STATE);
}
int main(int argc, char **argv)
{
    boot_choices_round_trip();
    assert(argc <= 2);
    unsigned passed = 0;
#define RUN(test) do { if (argc == 1 || strcmp(argv[1], #test) == 0) { \
    test(); ++passed; } } while (0)
    RUN(missing_defaults_do_not_write);
    RUN(valid_load_and_unchanged_requests_do_not_write);
    RUN(bad_record_lengths_are_preserved);
    RUN(startup_boundary_failures_are_visible);
    RUN(corrupt_and_unsupported_records_are_preserved);
    RUN(invalid_requests_cannot_mutate_pending_state);
    RUN(quiet_deadline_and_latest_value_coalescing);
    RUN(returning_to_durable_value_avoids_write);
    RUN(prolonged_changes_coalesce_without_frame_writes);
    RUN(new_request_during_commit_is_not_lost);
    RUN(save_failure_waits_for_explicit_retry);
    RUN(failed_commit_keeps_new_explicit_request);
    RUN(backward_clock_restarts_one_quiet_interval);
    RUN(uncertain_failed_commit_cannot_dedupe_against_old_value);
    RUN(revision_exhaustion_preserves_pending_value);
    RUN(legacy_record_migrates_only_after_explicit_transport_change);
    RUN(tap_and_name_preferences_persist_without_repeated_writes);
    RUN(v3_migration_preserves_tap_and_motion_defaults_without_boot_write);
#undef RUN
    assert(passed);
    if (argc == 2) printf("PASS: settings store case %s\n", argv[1]);
    else printf("PASS: %u production settings-store cases for defaults, durability, coalescing, "
                "faults, interleavings and revisions\n", passed);
    return 0;
}
