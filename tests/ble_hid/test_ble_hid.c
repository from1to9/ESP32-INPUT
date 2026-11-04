// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/ble_hid.c"
#include "../../firmware/main/app_bluetooth_management.h"

/* Existing state-transition assertions exercise copy-out and legacy callers. */
static ble_hid_bond_list_t observed_bonds(void)
{
    ble_hid_bond_list_t copy, legacy;
    memset(&copy, 0xa5, sizeof(copy));
    ble_hid_copy_bond_list(NULL);
    ble_hid_copy_bond_list(&copy);
    legacy = ble_hid_bond_list();
    assert(copy.known == legacy.known && copy.updating == legacy.updating &&
        copy.error == legacy.error && copy.revision == legacy.revision &&
        copy.count == legacy.count && copy.capacity == legacy.capacity && copy.preferences_revision == legacy.preferences_revision &&
        copy.current_match_known == legacy.current_match_known);
    assert(memcmp(copy.bonds, legacy.bonds, sizeof(copy.bonds)) == 0);
    /* The caller owns this buffer; changing it must not alter cached state. */
    copy.bonds[0].address[0] ^= 0xffU;
    ble_hid_copy_bond_list(&copy);
    assert(memcmp(copy.bonds, legacy.bonds, sizeof(copy.bonds)) == 0);
    return copy;
}


typedef struct { unsigned count, head, capacity, item_size; unsigned char data[16 * sizeof(control_command_t)]; } fake_queue_t;
static fake_queue_t queue_storage;
static esp_err_t nvs_result = ESP_OK;
static esp_err_t notify_result = ESP_OK;
static unsigned sdk_calls, notify_calls, advertising_calls;
static void (*notify_hook)(void);
static uint8_t copied_payload[8];
static TickType_t fake_tick;
static int64_t fake_uptime_us = -1;
static unsigned uptime_reads;
static unsigned tick_reads, bond_count_reads, bond_list_reads;
static unsigned advertising_heap_reads;
static esp_err_t controller_init_result = ESP_OK;
static bool task_create_fails;
static void (*registration_hook)(void);
static void (*advertising_start_hook)(void);
static bool worker_step_mode;
static unsigned worker_receive_count;
static jmp_buf worker_exit;
static esp_gatt_rsp_t last_response;
static esp_gatt_status_t last_response_status;
static unsigned response_calls;
static unsigned adv_config_calls, app_registration_calls;
static uint8_t durable_blob[SUBSCRIPTION_BLOB_LENGTH], staged_blob[SUBSCRIPTION_BLOB_LENGTH];
static size_t durable_length, staged_length;
static unsigned commit_calls;
static esp_err_t storage_result = ESP_OK, commit_result = ESP_OK, bond_result = ESP_OK;
static esp_ble_bond_dev_t fake_bonds[SUBSCRIPTION_CAPACITY];
static int fake_bond_count;
static bool callback_active;
size_t heap_caps_get_free_size(uint32_t capabilities) {
    assert(!callback_active);
    ++advertising_heap_reads;
    return (capabilities & MALLOC_CAP_DMA) ? 9000U : 42000U;
}
size_t heap_caps_get_largest_free_block(uint32_t capabilities) {
    assert(!callback_active);
    ++advertising_heap_reads;
    return (capabilities & MALLOC_CAP_DMA) ? 4000U : 25000U;
}

static void (*commit_hook)(void);
static void (*bond_hook)(void);
static void (*bond_count_hook)(void);
static uint8_t active_peer = 0x42;
static psa_status_t crypto_init_result = PSA_SUCCESS, hash_result = PSA_SUCCESS;
static unsigned last_response_commit_count;
static bool crypto_initialized, strict_auth_configured;
static unsigned rssi_calls, remove_calls;
static unsigned encryption_calls;
static esp_err_t rssi_request_result = ESP_OK, remove_request_result = ESP_OK;
static esp_bd_addr_t last_rssi_address, last_remove_address;
static bool last_security_accept;
static void (*security_response_hook)(void);
static esp_err_t security_response_result;
static char fake_sdk_name[33] = BLE_HID_DEFAULT_NAME;
static char configured_adv_name[33], configured_scan_name[33];
static unsigned name_set_calls, name_get_calls, name_reads_pending, close_calls, adv_stop_calls;
/* Model the documented SDK distinction: logical GATT close can notify before
 * its physical ACL is gone; GAP disconnect waits for that actual completion. */
static bool model_acl_lifecycle, model_acl_present, model_acl_closing;
static unsigned gatts_close_calls, gap_disconnect_calls, premature_advertising_calls;
static esp_err_t model_gap_disconnect_result;
static esp_bd_addr_t model_acl_address, last_gap_disconnect_address;
static void model_logical_disconnect(void);
static unsigned model_lock_depth, model_reservation_hooks;
static void (*model_after_close_reservation)(void);
static unsigned model_before_state_lock_count;
static void (*model_before_state_lock)(void);
void test_port_enter_critical(portMUX_TYPE *lock) {
    if (lock == &state_lock && !model_lock_depth && model_before_state_lock_count &&
        !--model_before_state_lock_count) {
        void (*hook)(void) = model_before_state_lock;
        model_before_state_lock = NULL; assert(hook); hook();
    }
    ++model_lock_depth;
}
void test_port_exit_critical(portMUX_TYPE *lock) {
    (void)lock; assert(model_lock_depth); --model_lock_depth;
    if (!model_lock_depth && model_after_close_reservation && state.closing && state.public.connected) {
        void (*hook)(void) = model_after_close_reservation;
        model_after_close_reservation = NULL; ++model_reservation_hooks; hook();
    }
}

static esp_err_t name_set_result = ESP_OK, name_get_result = ESP_OK;
static esp_err_t adv_data_result = ESP_OK, scan_data_result = ESP_OK;
static esp_err_t adv_start_result = ESP_OK, adv_stop_result = ESP_OK;
static bool drop_name_set;
static int shared_bond_count = -2;
static uint8_t target_durable_blob[2048], target_staged_blob[2048];
static size_t target_durable_length, target_staged_length;
static unsigned target_commit_calls;
static esp_err_t target_open_error, target_write_error, target_commit_error, target_read_error;
static bool target_commit_persists_on_error, target_corrupt_readback;
static void (*target_commit_hook)(void);

psa_status_t psa_crypto_init(void) {
    crypto_initialized = crypto_init_result == PSA_SUCCESS;
    return crypto_init_result;
}
/* Crypto hardware boundary substitute, deliberately not a SHA implementation.
 * Production target uses the SDK's PSA SHA-256 implementation. */
psa_status_t psa_hash_compute(unsigned int algorithm, const uint8_t *data, size_t length,
                             uint8_t *output, size_t capacity, size_t *produced) {
    assert(!callback_active && algorithm == PSA_ALG_SHA_256 && capacity >= 32);
    if (!crypto_initialized) return PSA_ERROR_BAD_STATE;
    if (hash_result != PSA_SUCCESS) return hash_result;
    for (unsigned i = 0; i < 32; ++i) output[i] = (uint8_t)(i * 7U);
    for (size_t i = 0; i < length; ++i) {
        unsigned slot = (unsigned)(i % 32);
        output[slot] = (uint8_t)((output[slot] * 33U) ^ data[i]);
    }
    *produced = 32;
    return PSA_SUCCESS;
}
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle) {
    if (strcmp(name, "hid_targets") == 0) {
        assert(!callback_active);
        if (target_open_error) return target_open_error;
        if (mode == NVS_READONLY && !target_durable_length) return ESP_ERR_NVS_NOT_FOUND;
        *handle = 2; return ESP_OK;
    }
    assert(!callback_active && strcmp(name, "hid_cccd") == 0);
    if (storage_result != ESP_OK) return storage_result;
    if (mode == NVS_READONLY && durable_length == 0) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
    if (handle == 2) {
        assert(!callback_active && strcmp(key, "preferences") == 0);
        if (target_read_error) return target_read_error;
        if (*length < target_durable_length) return ESP_ERR_INVALID_SIZE;
        memcpy(value, target_durable_blob, target_durable_length); *length = target_durable_length;
        if (target_corrupt_readback && *length) ((uint8_t *)value)[*length - 1] ^= 1U;
        return ESP_OK;
    }
    assert(!callback_active && handle == 1 && strcmp(key, "subscriptions") == 0);
    assert(*length >= durable_length);
    memcpy(value, durable_blob, durable_length);
    *length = durable_length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    if (handle == 2) {
        assert(!callback_active && strcmp(key, "preferences") == 0 && length <= sizeof(target_staged_blob));
        if (target_write_error) return target_write_error;
        memcpy(target_staged_blob, value, length); target_staged_length = length; return ESP_OK;
    }
    assert(!callback_active && handle == 1 && strcmp(key, "subscriptions") == 0);
    assert(length <= sizeof(staged_blob));
    memcpy(staged_blob, value, length);
    staged_length = length;
    return storage_result;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    if (handle == 2) {
        assert(!callback_active);
        if (target_commit_hook) target_commit_hook();
        if (!target_commit_error || target_commit_persists_on_error) {
            memcpy(target_durable_blob, target_staged_blob, target_staged_length);
            target_durable_length = target_staged_length;
        }
        ++target_commit_calls; return target_commit_error;
    }
    assert(!callback_active && handle == 1);
    if (commit_hook) commit_hook();
    if (commit_result != ESP_OK) return commit_result;
    memcpy(durable_blob, staged_blob, staged_length);
    durable_length = staged_length;
    ++commit_calls;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(!callback_active && (handle == 1 || handle == 2)); }
int esp_ble_get_bond_device_num(void) {
    assert(!callback_active);
    ++bond_count_reads;
    int count = fake_bond_count;
    if (bond_count_hook) bond_count_hook();
    return count;
}
esp_err_t esp_ble_get_bond_device_list(int *count, esp_ble_bond_dev_t *bonds) {
    assert(!callback_active && *count >= fake_bond_count);
    ++bond_list_reads;
    *count = fake_bond_count;
    if (bond_hook) bond_hook();
    memcpy(bonds, fake_bonds, (size_t)*count * sizeof(*bonds));
    return bond_result;
}
TickType_t xTaskGetTickCount(void) { ++tick_reads; return fake_tick; }
int64_t esp_timer_get_time(void) {
    ++uptime_reads;
    return fake_uptime_us >= 0 ? fake_uptime_us : (int64_t)fake_tick * 1000;
}

QueueHandle_t xQueueCreate(unsigned int count, unsigned int size) {
    assert(count <= 16 && size <= sizeof(control_command_t));
    memset(&queue_storage, 0, sizeof(queue_storage));
    queue_storage.capacity = count;
    queue_storage.item_size = size;
    return &queue_storage;
}
int xQueueSend(QueueHandle_t queue, const void *item, unsigned int timeout) {
    fake_queue_t *q = queue;
    assert(timeout == 0);
    if (q->count == q->capacity) return 0;
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->item_size, item, q->item_size);
    ++q->count;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, unsigned int timeout) {
    (void)timeout;
    if (worker_step_mode && worker_receive_count++ == 1) {
        longjmp(worker_exit, 1);
    }
    fake_queue_t *q = queue;
    if (q->count == 0) return 0;
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    return pdTRUE;
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned int stack, void *argument, unsigned int priority, void *handle) {
    (void)task; (void)name; (void)stack; (void)argument; (void)priority; (void)handle;
    return task_create_fails ? 0 : pdPASS;
}
TimerHandle_t xTimerCreate(const char *name, TickType_t period, int reload,
                          void *identifier, void (*callback)(TimerHandle_t)) {
    (void)name; (void)identifier;
    assert(period == CONTROL_POLL_MS && reload == pdTRUE && callback);
    return &queue_storage;
}
int xTimerStart(TimerHandle_t timer, TickType_t timeout) {
    assert(timer && timeout == 0);
    return pdPASS;
}
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
esp_err_t nvs_flash_init(void) { return nvs_result; }
esp_bt_controller_status_t esp_bt_controller_get_status(void) { return ESP_BT_CONTROLLER_STATUS_IDLE; }
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *config) { (void)config; return controller_init_result; }
esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode) {
#if CONFIG_BT_CLASSIC_ENABLED
    assert(mode == ESP_BT_MODE_BTDM);
#else
    assert(mode == ESP_BT_MODE_BLE);
#endif
    return ESP_OK;
}
int btc_storage_get_num_all_bond_devices(void) { assert(!callback_active); return shared_bond_count == -2 ? fake_bond_count : shared_bond_count; }
esp_bluedroid_status_t esp_bluedroid_get_status(void) { return ESP_BLUEDROID_STATUS_UNINITIALIZED; }
esp_err_t esp_bluedroid_init(void) { return ESP_OK; }
esp_err_t esp_bluedroid_enable(void) { return ESP_OK; }
esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t callback) { assert(callback); return ESP_OK; }
esp_err_t esp_ble_gatts_register_callback(esp_gatts_cb_t callback) { assert(callback); return ESP_OK; }
esp_err_t esp_ble_gap_set_security_param(esp_ble_sm_param_t type, void *value, uint8_t length) {
    assert(value && length);
    if (type == ESP_BLE_SM_AUTHEN_REQ_MODE) assert(*(uint8_t *)value == ESP_LE_AUTH_REQ_SC_BOND);
    if (type == ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH) {
        assert(*(uint8_t *)value == ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE);
        strict_auth_configured = true;
    }
    return ESP_OK;
}
esp_err_t esp_ble_gap_set_device_name(const char *name) {
    assert(!callback_active && name && strlen(name) < sizeof(fake_sdk_name));
    ++name_set_calls;
    if (name_set_result == ESP_OK && !drop_name_set) strcpy(fake_sdk_name, name);
    return name_set_result;
}
esp_err_t esp_ble_gap_get_device_name(void) {
    assert(!callback_active);
    ++name_get_calls;
    if (name_get_result == ESP_OK) {
        ++name_reads_pending;
        assert(name_reads_pending == 1);
    }
    return name_get_result;
}
esp_err_t esp_ble_gap_config_adv_data(esp_ble_adv_data_t *data) {
    ++adv_config_calls;
    /* Pinned esp_gap_ble_api.c:39-53: structured UUID input is 16*n bytes,
     * even when the assigned service is compressed to 16 bits on air. */
    if (!data || (data->service_uuid_len & 0x0fU)) return ESP_ERR_INVALID_ARG;
    if (data->set_scan_rsp) strcpy(configured_scan_name, fake_sdk_name);
    else strcpy(configured_adv_name, fake_sdk_name);
    return data->set_scan_rsp ? scan_data_result : adv_data_result;
}
esp_err_t esp_ble_gatts_app_register(uint16_t app_id) {
    assert(app_id == HID_APP_ID);
    ++app_registration_calls;
    if (registration_hook) registration_hook();
    return ESP_OK;
}
esp_err_t esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *db, esp_gatt_if_t interface, uint16_t count, uint8_t instance) { assert(db && interface && count == ATTR_COUNT && instance == 0); ++sdk_calls; return ESP_OK; }
esp_err_t esp_ble_gatts_start_service(uint16_t handle) { assert(handle); ++sdk_calls; return ESP_OK; }
esp_err_t esp_ble_set_encryption(esp_bd_addr_t address, esp_ble_sec_act_t action) { (void)address; assert(action == ESP_BLE_SEC_ENCRYPT); ++encryption_calls; ++sdk_calls; return ESP_OK; }
esp_err_t esp_ble_gap_security_rsp(esp_bd_addr_t address, bool accept) {
    (void)address; assert(!callback_active); last_security_accept = accept; ++sdk_calls;
    if (security_response_hook) security_response_hook();
    return security_response_result;
}
esp_err_t esp_ble_gap_read_rssi(esp_bd_addr_t address) {
    assert(!callback_active);
    memcpy(last_rssi_address, address, sizeof(last_rssi_address));
    ++rssi_calls;
    return rssi_request_result;
}
esp_err_t esp_ble_remove_bond_device(esp_bd_addr_t address) {
    assert(!callback_active);
    memcpy(last_remove_address, address, sizeof(last_remove_address));
    ++remove_calls;
    return remove_request_result;
}
esp_err_t esp_ble_gatts_close(esp_gatt_if_t interface, uint16_t conn_id) {
    (void)interface; (void)conn_id; ++close_calls; ++gatts_close_calls; ++sdk_calls;
    if (model_acl_lifecycle) { model_acl_closing = true; model_logical_disconnect(); }
    return ESP_OK;
}
esp_err_t esp_ble_gap_disconnect(esp_bd_addr_t address) {
    assert(!callback_active); ++close_calls; ++gap_disconnect_calls; ++sdk_calls;
    memcpy(last_gap_disconnect_address,address,ESP_BD_ADDR_LEN);
    if (model_gap_disconnect_result == ESP_OK && model_acl_lifecycle) model_acl_closing = true;
    return model_gap_disconnect_result;
}
esp_err_t esp_ble_gatts_send_response(esp_gatt_if_t interface, uint16_t conn_id, uint32_t transaction, esp_gatt_status_t status, esp_gatt_rsp_t *response) { (void)interface; (void)conn_id; (void)transaction; assert(response); last_response = *response; last_response_status = status; last_response_commit_count = commit_calls; ++response_calls; ++sdk_calls; return ESP_OK; }
esp_err_t esp_ble_gap_start_advertising(esp_ble_adv_params_t *parameters) { assert(parameters); ++advertising_calls; if (model_acl_lifecycle && model_acl_present) ++premature_advertising_calls; if (advertising_start_hook) advertising_start_hook(); return adv_start_result; }
esp_err_t esp_ble_gap_stop_advertising(void) { ++adv_stop_calls; ++sdk_calls; return adv_stop_result; }
esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t interface, uint16_t conn_id, uint16_t handle, uint16_t length, uint8_t *value, bool confirmation) {
    assert(interface == 7 && conn_id == 3 && handle && length <= sizeof(copied_payload) && !confirmation);
    memcpy(copied_payload, value, length);
    ++notify_calls;
    if (notify_hook) notify_hook();
    return notify_result;
}

/* Run the actual worker to its next receive boundary with no real thread or
 * wall-clock wait. All event/state code remains production code. */
static void worker_cycle(void) {
    worker_step_mode = true;
    worker_receive_count = 0;
    if (setjmp(worker_exit) == 0) control_worker(NULL);
    worker_step_mode = false;
}
static void drain(void) { control_command_t command; while (xQueueReceive(control_queue, &command, 0)) {} }
static control_command_t pop(void) { control_command_t command; assert(xQueueReceive(control_queue, &command, 0)); return command; }
static void reset(void) {
    memset(&admission_diagnostics, 0, sizeof(admission_diagnostics));
    admission_trace_head = 0;
    memset(&advertising_diagnostics, 0, sizeof(advertising_diagnostics));
    advertising_trace_head = 0;
    advertising_start_sequence = advertising_stop_sequence = 0;
    advertising_heap_reads = 0;
    memset(&state, 0, sizeof(state));
    first_failure = (ble_hid_failure_t){.stage = "none", .error = ESP_OK};
    memset(&device_name, 0, sizeof(device_name));
    strcpy(fake_sdk_name, BLE_HID_DEFAULT_NAME);
    strcpy(configured_adv_name, BLE_HID_DEFAULT_NAME);
    strcpy(configured_scan_name, BLE_HID_DEFAULT_NAME);
    name_set_calls = name_get_calls = name_reads_pending = close_calls = adv_stop_calls = 0;
    name_set_result = name_get_result = adv_data_result = scan_data_result = ESP_OK;
    adv_start_result = adv_stop_result = ESP_OK;
    drop_name_set = false;
    shared_bond_count = -2;
    target_durable_length = target_staged_length = 0; target_commit_calls = 0;
    target_open_error = target_write_error = target_commit_error = target_read_error = ESP_OK;
    target_commit_persists_on_error = target_corrupt_readback = false; target_commit_hook = NULL;
    fake_uptime_us = -1;
    uptime_reads = 0;
    tick_reads = bond_count_reads = bond_list_reads = 0;
    security_response_hook = NULL; security_response_result = ESP_OK;
    controller_init_result = ESP_OK;
    task_create_fails = false;
    registration_hook = NULL;
    advertising_start_hook = NULL;
    model_acl_lifecycle = model_acl_present = model_acl_closing = false;
    gatts_close_calls = gap_disconnect_calls = premature_advertising_calls = 0;
    model_gap_disconnect_result = ESP_OK;
    model_lock_depth = model_reservation_hooks = 0; model_after_close_reservation = NULL;
    model_before_state_lock_count = 0; model_before_state_lock = NULL;
    memset(model_acl_address,0,sizeof model_acl_address);
    memset(last_gap_disconnect_address,0,sizeof last_gap_disconnect_address);
    memset(&subscriptions, 0, sizeof(subscriptions));
    memset(&pending_subscriptions, 0, sizeof(pending_subscriptions));
    memset(fake_bonds, 0, sizeof(fake_bonds));
    durable_length = staged_length = 0;
    fake_bond_count = 0;
    storage_result = commit_result = bond_result = ESP_OK;
    commit_calls = 0;
    commit_hook = NULL;
    bond_hook = NULL;
    bond_count_hook = NULL;
    active_peer = 0x42;
    crypto_init_result = hash_result = PSA_SUCCESS;
    last_response_commit_count = 0;
    crypto_initialized = strict_auth_configured = false;
    rssi_calls = remove_calls = 0;
    encryption_calls = 0;
    rssi_request_result = remove_request_result = ESP_OK;
    last_security_accept = false;
    callback_active = false;
    state.interface = 7;
    state.public.initialized = true;
    state.target.loaded = true;
    state.service_started = true;
    state.adv_configured = true;
    state.scan_configured = true;
    for (unsigned i = 0; i < ATTR_COUNT; ++i) state.handles[i] = (uint16_t)(20 + i);
    control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_command_t));
    nvs_result = notify_result = ESP_OK;
    sdk_calls = notify_calls = advertising_calls = 0;
    adv_config_calls = app_registration_calls = 0;
    notify_hook = NULL;
    fake_tick = 0;
    state.bonds_loaded = true;
    state.public.bond_count_known = true;
    state.bond_list.known = true;
    open_pairing_locked(fake_tick, BLE_HID_PAIRING_DEFAULT_MS);
    worker_step_mode = false;
    response_calls = 0;
}
static void connect_peer(void) {
    esp_ble_gatts_cb_param_t p = {0};
    p.connect.conn_id = 3;
    p.connect.remote_bda[0] = active_peer;
    p.connect.ble_addr_type = 1;
    gatts_callback(ESP_GATTS_CONNECT_EVT, 7, &p);
}
static void disconnect_peer(void) {
    esp_ble_gatts_cb_param_t p = {0};
    p.disconnect.conn_id = 3;
    p.disconnect.remote_bda[0] = active_peer;
    gatts_callback(ESP_GATTS_DISCONNECT_EVT, 7, &p);
}
static void write_attribute(unsigned index, uint8_t *value, uint16_t length, uint16_t offset, bool prepare) {
    esp_ble_gatts_cb_param_t p = {0};
    p.write.conn_id = 3;
    p.write.bda[0] = active_peer;
    p.write.handle = state.handles[index];
    p.write.need_rsp = true;
    p.write.len = length;
    p.write.value = value;
    p.write.offset = offset;
    p.write.is_prep = prepare;
    callback_active = true;
    gatts_callback(ESP_GATTS_WRITE_EVT, 7, &p);
    callback_active = false;
}
static void authentication_event(bool bonded, bool success) {
    esp_ble_gap_cb_param_t p = {0};
    p.ble_security.auth_cmpl.bd_addr[0] = active_peer;
    p.ble_security.auth_cmpl.success = success;
    p.ble_security.auth_cmpl.auth_mode = bonded ? ESP_LE_AUTH_REQ_SC_BOND : 0;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_AUTH_CMPL_EVT, &p);
    callback_active = false;
}
static void authorize_peer(bool bonded) {
    authentication_event(bonded, true);
    worker_cycle();
}
static void subscribe(void) {
    if (!state.auth_complete) authorize_peer(fake_bond_count != 0);
    uint8_t enabled[] = {1, 0};
    write_attribute(ATTR_KEYBOARD_CCCD, enabled, 2, 0, false);
    assert(!ble_hid_snapshot().ready);
    worker_cycle();
    write_attribute(ATTR_MOUSE_CCCD, enabled, 2, 0, false);
    worker_cycle();
    assert(ble_hid_snapshot().ready);
    drain();
    sdk_calls = 0;
    response_calls = 0;
}
static void complete_notification(esp_gatt_status_t status) {
    esp_ble_gatts_cb_param_t p = {0};
    p.conf.conn_id = 3;
    p.conf.handle = state.notification_handle;
    p.conf.status = status;
    gatts_callback(ESP_GATTS_CONF_EVT, 7, &p);
}
static void test_snapshot_peer_identity(void) {
    reset();
    static const uint8_t zero[6] = {0};
    const uint8_t first[6] = {0x42, 0x13, 0x24, 0x35, 0x46, 0x57};
    const uint8_t second[6] = {0x43, 0x68, 0x79, 0x8a, 0x9b, 0xac};
    ble_hid_snapshot_t snapshot = ble_hid_snapshot();
    assert(!snapshot.connected && !snapshot.peer_known);
    assert(memcmp(snapshot.peer_address, zero, sizeof zero) == 0);
    esp_ble_gatts_cb_param_t p = {0};
    p.connect.conn_id = 3;
    memcpy(p.connect.remote_bda, first, sizeof first);
    gatts_callback(ESP_GATTS_CONNECT_EVT, 7, &p);
    drain();
    snapshot = ble_hid_snapshot();
    assert(snapshot.connected && snapshot.peer_known);
    assert(memcmp(snapshot.peer_address, first, sizeof first) == 0);
    uint64_t first_generation = snapshot.generation;
    snapshot.peer_address[0] = 0; /* A snapshot is a value, not a writable view. */
    assert(ble_hid_snapshot().peer_address[0] == first[0]);

    memcpy(p.connect.remote_bda, second, sizeof second);
    gatts_callback(ESP_GATTS_CONNECT_EVT, 7, &p);
    assert(pop().kind == COMMAND_REJECT_CONNECTION);
    snapshot = ble_hid_snapshot();
    assert(snapshot.generation == first_generation && snapshot.peer_known);
    assert(memcmp(snapshot.peer_address, first, sizeof first) == 0);
    memset(&p, 0, sizeof p);
    p.disconnect.conn_id = 3;
    memcpy(p.disconnect.remote_bda, second, sizeof second);
    gatts_callback(ESP_GATTS_DISCONNECT_EVT, 7, &p);
    assert(ble_hid_snapshot().connected);

    memset(&p, 0, sizeof p);
    p.disconnect.conn_id = 3;
    memcpy(p.disconnect.remote_bda, first, sizeof first);
    gatts_callback(ESP_GATTS_DISCONNECT_EVT, 7, &p);
    snapshot = ble_hid_snapshot();
    assert(!snapshot.connected && !snapshot.peer_known);
    assert(memcmp(snapshot.peer_address, zero, sizeof zero) == 0);
    memset(&p, 0, sizeof p);
    p.connect.conn_id = 3;
    memcpy(p.connect.remote_bda, second, sizeof second);
    gatts_callback(ESP_GATTS_CONNECT_EVT, 7, &p);
    drain();
    snapshot = ble_hid_snapshot();
    assert(snapshot.connected && snapshot.peer_known && snapshot.generation != first_generation);
    assert(memcmp(snapshot.peer_address, second, sizeof second) == 0);
    /* A delayed disconnect for the former address cannot clear the new peer. */
    memset(&p, 0, sizeof p);
    p.disconnect.conn_id = 3;
    memcpy(p.disconnect.remote_bda, first, sizeof first);
    gatts_callback(ESP_GATTS_DISCONNECT_EVT, 7, &p);
    assert(ble_hid_snapshot().peer_known);
    memcpy(p.disconnect.remote_bda, second, sizeof second);
    gatts_callback(ESP_GATTS_DISCONNECT_EVT, 7, &p);
    snapshot = ble_hid_snapshot();
    assert(!snapshot.peer_known && memcmp(snapshot.peer_address, zero, sizeof zero) == 0);
}
static void test_gate_and_epoch(void) {
    reset(); connect_peer(); drain();
    assert(ble_hid_snapshot().connected && !ble_hid_snapshot().ready && !ble_hid_snapshot().encrypted);
    uint8_t report[8] = {0};
    assert(!ble_hid_send_generation(state.public.generation, 1, report, 8));
    subscribe();
    uint64_t old = state.public.generation;
    uint8_t disabled[] = {0, 0}, enabled[] = {1, 0};
    write_attribute(ATTR_KEYBOARD_CCCD, disabled, 2, 0, false);
    assert(!state.public.ready && state.public.generation != old);
    write_attribute(ATTR_KEYBOARD_CCCD, enabled, 2, 0, false);
    worker_cycle();
    worker_cycle();
    assert(state.public.ready && !ble_hid_send_generation(old, 1, report, 8));
    uint64_t generation = state.public.generation;
    uint8_t suspend = 0, resume = 1;
    write_attribute(ATTR_CONTROL, &suspend, 1, 0, false);
    write_attribute(ATTR_CONTROL, &resume, 1, 0, false);
    assert(state.public.ready && state.public.generation != generation);
    assert(!ble_hid_send_generation(generation, 1, report, 8));
    drain();
    generation = state.public.generation;
    disconnect_peer(); connect_peer(); drain();
    assert(state.public.generation != generation && !state.public.ready && !state.public.keyboard_leds);
    assert(!ble_hid_send_generation(generation, 1, report, 8));
}
static void test_malformed_writes(void) {
    reset(); connect_peer(); drain(); subscribe();
    uint64_t generation = state.public.generation;
    uint8_t invalid[] = {2, 0};
    write_attribute(ATTR_KEYBOARD_CCCD, invalid, 2, 0, false);
    assert(pop().status == ESP_GATT_CCC_CFG_ERR);
    write_attribute(ATTR_KEYBOARD_CCCD, invalid, 1, 0, false);
    assert(pop().status == ESP_GATT_INVALID_ATTR_LEN);
    write_attribute(ATTR_KEYBOARD_CCCD, invalid, 2, 1, false);
    assert(pop().status == ESP_GATT_INVALID_OFFSET);
    write_attribute(ATTR_KEYBOARD_CCCD, invalid, 2, 0, true);
    assert(pop().status == ESP_GATT_REQ_NOT_SUPPORTED);
    uint8_t invalid_control = 2;
    write_attribute(ATTR_CONTROL, &invalid_control, 1, 0, false);
    assert(pop().status == ESP_GATT_OUT_OF_RANGE);
    assert(state.public.ready && state.public.generation == generation);
    uint8_t leds = 0xff;
    write_attribute(ATTR_LED, &leds, 1, 0, false);
    assert(pop().status == ESP_GATT_OK && state.public.keyboard_leds == 0x1f);
    assert(sdk_calls == 0); /* All callback SDK work is deferred. */
}
static void disconnect_during_send(void) {
    disconnect_peer();
    service_link_control();
    assert(advertising_calls == 0 && state.notification_pending && state.submitting);
}
static void test_send_race_and_backpressure(void) {
    reset(); connect_peer(); drain(); subscribe();
    uint8_t report[8] = {0, 0, 4};
    uint64_t generation = state.public.generation;
    assert(!ble_hid_send_generation(generation, 1, report, 7));
    assert(!ble_hid_send_generation(generation, 9, report, 8));
    assert(!ble_hid_send_generation(generation, 1, NULL, 8));
    assert(ble_hid_send_generation(generation, 1, report, 8));
    assert(state.public.congested && !ble_hid_send_generation(generation, 1, report, 8));
    assert(notify_calls == 1 && copied_payload[2] == 4);
    complete_notification(ESP_GATT_OK);
    assert(!state.public.congested);
    notify_hook = disconnect_during_send;
    assert(ble_hid_send_generation(generation, 1, report, 8));
    assert(!state.public.connected && state.notification_pending);
    service_link_control();
    assert(advertising_calls == 0);
    complete_notification(ESP_GATT_ERROR);
    service_link_control();
    assert(advertising_calls == 1 && !state.notification_pending && !state.public.failed);
    connect_peer(); drain(); subscribe();
    assert(!ble_hid_send_generation(generation, 1, report, 8));
    assert(notify_calls == 2);
}
static void test_fail_closed(void) {
    reset(); connect_peer(); drain(); subscribe();
    uint8_t report[8] = {0};
    notify_result = ESP_FAIL;
    assert(!ble_hid_send(1, report, 8) && !state.notification_pending && !state.public.congested);
    notify_result = ESP_OK;
    assert(ble_hid_send(1, report, 8));
    uint64_t old = state.public.generation;
    complete_notification(ESP_GATT_ERROR);
    assert(!state.public.ready && state.public.generation != old);
    service_link_control();
    assert(state.closing && sdk_calls == 1);
    reset(); connect_peer(); drain(); subscribe();
    control_command_t command = {.kind=COMMAND_RESPONSE, .conn_id=state.conn_id,
        .generation=state.connection_generation};
    memcpy(command.address, state.address, sizeof(command.address));
    for (unsigned i = 0; i < CONTROL_QUEUE_LENGTH; ++i) post_control(&command);
    post_control(&command);
    assert(state.public.failed && !state.public.ready);
    reset(); nvs_result = ESP_ERR_INVALID_STATE;
    assert(ble_hid_init() == ESP_ERR_INVALID_STATE && state.public.failed);
    assert(ble_hid_init() == ESP_ERR_INVALID_STATE);
}
static void test_advertising_prerequisites(void) {
    reset(); state.adv_configured = false;
    service_link_control(); assert(advertising_calls == 0);
    state.adv_configured = true; state.scan_configured = false;
    service_link_control(); assert(advertising_calls == 0);
    state.scan_configured = true; state.service_started = false;
    service_link_control(); assert(advertising_calls == 0);
    state.service_started = true;
    service_link_control(); assert(advertising_calls == 1);
    service_link_control(); assert(advertising_calls == 1);
    reset(); state.interface = ESP_GATT_IF_NONE;
    esp_ble_gatts_cb_param_t p = {0};
    p.reg.app_id = HID_APP_ID;
    p.reg.status = ESP_GATT_OK;
    gatts_callback(ESP_GATTS_REG_EVT, 7, &p);
    assert(pop().kind == COMMAND_CREATE_SERVICE && sdk_calls == 0);
}
static void reset_startup(void) {
    reset();
    state.public.initialized = false;
    state.target.loaded = false;
    state.service_started = state.adv_configured = state.scan_configured = false;
    state.interface = ESP_GATT_IF_NONE;
    state.bonds_loaded = false;
    state.bond_list.known = false;
    state.bond_list.error = ESP_ERR_INVALID_STATE;
    state.public.bond_count_known = false;
    state.public.pairing_open = false;
}
static void test_advertising_initialization_contract(void) {
    /* Independent expected wire identity: Bluetooth-assigned HID 0x1812,
     * supplied as little-endian 00001812-0000-1000-8000-00805f9b34fb. */
    const uint8_t expected_hid_uuid[16] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00
    };
    assert(advertising_data.service_uuid_len == sizeof(expected_hid_uuid));
    assert(memcmp(advertising_data.p_service_uuid, expected_hid_uuid, sizeof(expected_hid_uuid)) == 0);
#if CONFIG_BT_CLASSIC_ENABLED
    assert(advertising_data.flag == 0x02); /* A BTDM controller supports BR/EDR. */
#else
    assert(advertising_data.flag == 0x06);
#endif
    assert(advertising_data.appearance == 0x03c0);
    assert(advertising_data.include_name && !advertising_data.set_scan_rsp);
    assert(scan_data.include_name && scan_data.set_scan_rsp);

    /* Exercise the actual production init with the shipped invalid argument.
     * This fails if the boundary substitute starts returning blind success. */
    reset_startup();
    uint16_t valid_length = advertising_data.service_uuid_len;
    advertising_data.service_uuid_len = 2;
    esp_err_t rejected = ble_hid_init();
    advertising_data.service_uuid_len = valid_length;
    assert(rejected == ESP_ERR_INVALID_ARG && state.public.failed);
    assert(!state.public.initialized && adv_config_calls == 1 && app_registration_calls == 0);
    assert(advertising_calls == 0);

    reset_startup();
    assert(ble_hid_init() == ESP_OK && state.public.initialized && !state.public.failed);
    assert(adv_config_calls == 2 && app_registration_calls == 1 && advertising_calls == 0);
    esp_ble_gatts_cb_param_t gatt = {0};
    gatt.reg.app_id = HID_APP_ID;
    gatt.reg.status = ESP_GATT_OK;
    gatts_callback(ESP_GATTS_REG_EVT, 7, &gatt);
    worker_cycle();
    uint16_t handles[ATTR_COUNT];
    for (unsigned i = 0; i < ATTR_COUNT; ++i) handles[i] = (uint16_t)(20 + i);
    memset(&gatt, 0, sizeof(gatt));
    gatt.add_attr_tab.status = ESP_GATT_OK;
    gatt.add_attr_tab.num_handle = ATTR_COUNT;
    gatt.add_attr_tab.handles = handles;
    gatts_callback(ESP_GATTS_CREAT_ATTR_TAB_EVT, 7, &gatt);
    worker_cycle();
    memset(&gatt, 0, sizeof(gatt));
    gatt.start.status = ESP_GATT_OK;
    gatt.start.service_handle = handles[ATTR_SERVICE];
    gatts_callback(ESP_GATTS_START_EVT, 7, &gatt);
    worker_cycle();
    assert(state.service_started && advertising_calls == 0);
    esp_ble_gap_cb_param_t gap = {0};
    gap.adv_data_cmpl.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT, &gap);
    worker_cycle();
    assert(state.adv_configured && !state.scan_configured && advertising_calls == 0);
    gap.scan_rsp_data_cmpl.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT, &gap);
    worker_cycle();
    assert(advertising_calls == 1 && state.adv_pending && !state.advertising);
    gap.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &gap);
    assert(state.advertising && !state.adv_pending && !state.public.failed);
}
static void test_notification_timeout(void) {
    reset(); connect_peer(); drain(); subscribe();
    uint8_t report[8] = {0};
    assert(ble_hid_send(1, report, 8));
    fake_tick = NOTIFICATION_TIMEOUT_MS - 1;
    service_link_control();
    assert(!state.public.failed && !state.closing);
    fake_tick = NOTIFICATION_TIMEOUT_MS;
    /* The deadline is independent of a worker stuck in a Bluetooth API. */
    notification_watchdog(NULL);
    assert(state.public.failed && !state.public.ready && sdk_calls == 0);
    service_link_control();
    assert(state.closing);
    assert(sdk_calls == 1 && advertising_calls == 0);
    disconnect_peer();
    complete_notification(ESP_GATT_OK);
    service_link_control();
    assert(state.public.failed && advertising_calls == 0);
    connect_peer();
    assert(pop().kind == COMMAND_REJECT_CONNECTION);
    assert(!state.public.connected && !ble_hid_send(1, report, 8));

    reset(); connect_peer(); drain(); subscribe();
    fake_tick = UINT32_MAX - 500U;
    assert(ble_hid_send(1, report, 8));
    fake_tick += NOTIFICATION_TIMEOUT_MS;
    service_link_control();
    assert(state.public.failed && state.closing); /* Tick rollover is bounded. */

    reset(); connect_peer(); drain(); subscribe();
    assert(ble_hid_send(1, report, 8));
    disconnect_peer();
    fake_tick = NOTIFICATION_TIMEOUT_MS;
    service_link_control();
    assert(state.public.failed && advertising_calls == 0);
    complete_notification(ESP_GATT_ERROR);
    service_link_control();
    assert(state.public.failed && advertising_calls == 0);
}
static void test_coordinator_disconnect(void) {
    reset(); connect_peer(); drain(); subscribe();
    uint64_t generation = state.public.generation;
    assert(!ble_hid_disconnect_generation(generation + 1));
    assert(state.public.ready && sdk_calls == 0);
    assert(ble_hid_disconnect_generation(generation));
    assert(!state.public.ready && state.public.generation != generation && sdk_calls == 0);
    assert(!ble_hid_disconnect_generation(generation));
    service_link_control();
    assert(state.closing && sdk_calls == 1);
    disconnect_peer();
    assert(!ble_hid_disconnect_generation(state.public.generation));
    service_link_control();
    assert(advertising_calls == 1 && !state.public.failed);
}
static void test_control_worker_and_reads(void) {
    reset(); connect_peer();
    worker_cycle();
    assert(sdk_calls == 1); /* Encryption is requested outside the callback. */
    subscribe();
    uint8_t leds = 5;
    write_attribute(ATTR_LED, &leds, 1, 0, false);
    assert(response_calls == 0);
    worker_cycle();
    assert(response_calls == 1 && last_response_status == ESP_GATT_OK);
    esp_ble_gatts_cb_param_t p = {0};
    p.read.conn_id = 3;
    p.read.bda[0] = 0x42;
    p.read.need_rsp = true;
    p.read.handle = state.handles[ATTR_LED];
    gatts_callback(ESP_GATTS_READ_EVT, 7, &p);
    worker_cycle();
    assert(last_response.attr_value.len == 1 && last_response.attr_value.value[0] == 5);
    p.read.offset = 2;
    gatts_callback(ESP_GATTS_READ_EVT, 7, &p);
    worker_cycle();
    assert(last_response_status == ESP_GATT_INVALID_OFFSET && last_response.attr_value.len == 0);

    /* An old ATT response cannot satisfy a transaction on a reused connection. */
    p.read.offset = 0;
    gatts_callback(ESP_GATTS_READ_EVT, 7, &p);
    unsigned previous_responses = response_calls;
    disconnect_peer();
    connect_peer();
    worker_cycle();
    assert(response_calls == previous_responses);
    drain();
}
static void save_sdk_bond(unsigned int index, uint8_t address, uint8_t key) {
    assert(index < SUBSCRIPTION_CAPACITY);
    esp_ble_bond_dev_t *bond = &fake_bonds[index];
    memset(bond, 0, sizeof(*bond));
    bond->bd_addr[0] = address; /* SDK-resolved bond index, not rotating on-air RPA. */
    bond->bd_addr_type = 1;
    bond->bond_key.key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    bond->bond_key.penc_key.key_size = 16;
    memset(bond->bond_key.penc_key.ltk, key, 16);
    memset(bond->bond_key.pid_key.irk, 0x5a, 16);
    bond->bond_key.pid_key.static_addr[0] = (uint8_t)(address ^ 0xa5U);
    bond->bond_key.pid_key.addr_type = 1;
    if (fake_bond_count <= (int)index) fake_bond_count = (int)index + 1;
}
static void make_bond(unsigned int index, uint8_t address, uint8_t key) {
    save_sdk_bond(index, address, key);
    if (state.bonds_loaded && bond_result == ESP_OK) assert(refresh_known_bonds() == ESP_OK);
}
static void start_bonded(void) {
    reset(); make_bond(0, active_peer, 0x11);
    connect_peer(); drain(); authorize_peer(true);
    assert(!state.public.ready && state.bond_resolved);
}
static void write_cccd(unsigned int index, bool enabled) {
    uint8_t value[] = {enabled ? 1 : 0, 0};
    write_attribute(index, value, 2, 0, false);
    worker_cycle();
}
static void simulate_reboot(void) {
    uint8_t saved[SUBSCRIPTION_BLOB_LENGTH];
    esp_ble_bond_dev_t bonds[SUBSCRIPTION_CAPACITY];
    size_t length = durable_length;
    int count = fake_bond_count;
    uint8_t peer = active_peer;
    memcpy(saved, durable_blob, sizeof(saved));
    memcpy(bonds, fake_bonds, sizeof(bonds));
    reset();
    memcpy(durable_blob, saved, sizeof(saved));
    durable_length = length;
    memcpy(fake_bonds, bonds, sizeof(bonds));
    fake_bond_count = count;
    active_peer = peer;
}
static void test_bonded_reconnect_and_reboot(void) {
    start_bonded();
    assert(commit_calls == 0);
    subscribe();
    assert(commit_calls == 2 && durable_length == 4 + SUBSCRIPTION_RECORD_LENGTH);
    assert(last_response_commit_count == 2); /* ATT success follows durability. */
    uint64_t old_generation = state.public.generation;
    disconnect_peer(); connect_peer(); drain();
    assert(!state.public.ready && !state.public.encrypted);
    authorize_peer(true); /* No CCCD rewrite and no encrypted GATT read. */
    assert(state.public.ready && state.public.generation != old_generation);
    assert(commit_calls == 2);
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    assert(commit_calls == 2); /* Identical writes do not wear flash. */
    simulate_reboot();
    connect_peer(); drain(); authorize_peer(true);
    assert(state.public.ready && commit_calls == 0);
    write_cccd(ATTR_KEYBOARD_CCCD, false);
    assert(!state.public.ready && commit_calls == 1);
    simulate_reboot();
    connect_peer(); drain(); authorize_peer(true);
    assert(!state.public.ready && !state.keyboard_cccd && state.mouse_cccd);
    assert(commit_calls == 0);
}
static void test_peer_isolation_and_repair(void) {
    start_bonded(); subscribe();
    make_bond(1, 0x43, 0x22);
    disconnect_peer(); active_peer = 0x43; connect_peer(); drain(); authorize_peer(true);
    assert(!state.keyboard_cccd && !state.mouse_cccd && !state.public.ready);
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    assert(!state.public.ready);
    write_cccd(ATTR_MOUSE_CCCD, true);
    assert(state.public.ready && subscriptions.count == 2);
    disconnect_peer(); active_peer = 0x42; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.ready);
    disconnect_peer();
    make_bond(0, 0x42, 0x33); /* Same address and IRK, different bond LTK. */
    connect_peer(); drain(); authorize_peer(true);
    assert(!state.public.ready && !state.keyboard_cccd && !state.mouse_cccd);
    assert(subscriptions.count == 2); /* Neither old peer record was rewritten. */
}
static void test_pending_bond_and_deadline(void) {
    reset(); make_bond(0, active_peer, 0x11); connect_peer(); drain();
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    write_cccd(ATTR_MOUSE_CCCD, true);
    assert(response_calls == 0 && commit_calls == 0 && state.cccd_pending == 2);
    assert(!state.public.ready);
    authorize_peer(true);
    assert(response_calls == 2 && commit_calls == 2 && state.public.ready);

    reset(); connect_peer(); drain();
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    fake_tick = SUBSCRIPTION_TIMEOUT_MS - 1;
    notification_watchdog(NULL); assert(!state.public.failed);
    fake_tick = SUBSCRIPTION_TIMEOUT_MS;
    notification_watchdog(NULL); assert(state.public.failed && !state.public.ready);
    worker_cycle();
    assert(last_response_status == ESP_GATT_INSUF_RESOURCE && state.closing);

    reset(); make_bond(0, active_peer, 0x11); connect_peer(); drain();
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    disconnect_peer(); active_peer = 0x43; make_bond(1, active_peer, 0x22);
    connect_peer(); drain(); authorize_peer(true);
    assert(!state.public.ready && commit_calls == 0 && response_calls == 0);
}
static void disable_during_bond_lookup(void) {
    bond_hook = NULL;
    uint8_t disabled[] = {0, 0};
    write_attribute(ATTR_KEYBOARD_CCCD, disabled, 2, 0, false);
}
static void disconnect_during_commit(void) {
    commit_hook = NULL;
    disconnect_peer();
    connect_peer();
}
static void test_subscription_races(void) {
    start_bonded(); subscribe();
    disconnect_peer(); connect_peer(); drain();
    bond_hook = disable_during_bond_lookup;
    authorize_peer(true);
    assert(!state.public.ready && !state.keyboard_cccd && state.mouse_cccd);
    worker_cycle();
    assert(!state.public.ready && !state.keyboard_cccd);
    disconnect_peer(); connect_peer(); drain(); authorize_peer(true);
    assert(!state.public.ready && !state.keyboard_cccd && state.mouse_cccd);

    start_bonded();
    commit_hook = disconnect_during_commit;
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    assert(commit_calls == 1 && response_calls == 0 && !state.public.ready);
    assert(!state.keyboard_cccd && state.cccd_pending == 0);
    drain(); authorize_peer(true);
    assert(state.keyboard_cccd && !state.mouse_cccd && !state.public.ready);
}
static void test_storage_and_crypto_failures(void) {
    start_bonded(); subscribe();
    commit_result = ESP_FAIL;
    write_cccd(ATTR_KEYBOARD_CCCD, false);
    assert(state.public.failed && !state.public.ready && state.closing);
    assert(commit_calls == 2 && last_response_status == ESP_GATT_INSUF_RESOURCE);
    assert(subscriptions.records[0].mask == 3); /* Failed commit cannot update RAM truth. */

    reset(); durable_length = 4; memset(durable_blob, 0xff, 4);
    worker_cycle(); assert(state.public.failed && commit_calls == 0 && durable_length == 4);
    reset(); storage_result = ESP_ERR_NO_MEM;
    worker_cycle(); assert(state.public.failed && !subscriptions.loaded);
    reset(); crypto_init_result = PSA_ERROR_BAD_STATE;
    worker_cycle(); assert(state.public.failed && !subscriptions.loaded);
    reset(); make_bond(0, active_peer, 0x11); connect_peer(); drain(); hash_result = PSA_ERROR_BAD_STATE;
    authorize_peer(true); assert(state.public.failed && !state.bond_resolved);
    reset(); make_bond(0, active_peer, 0x11); fake_bonds[0].bond_key.key_mask &= (uint8_t)~ESP_BLE_ENC_KEY_MASK;
    connect_peer(); drain(); authorize_peer(true); assert(state.public.failed && !state.public.ready);
    reset(); make_bond(0, active_peer, 0x11); bond_result = ESP_FAIL;
    connect_peer(); drain(); authorize_peer(true); assert(state.public.failed && commit_calls == 0);
}
static void replace_peer_during_failed_lookup(void) {
    bond_hook = NULL;
    disconnect_peer();
    active_peer = 0x43;
    connect_peer();
}
static void test_stale_failure_cannot_fault_new_peer(void) {
    start_bonded(); subscribe();
    make_bond(1, 0x43, 0x22);
    disconnect_peer(); connect_peer(); drain();
    bond_result = ESP_FAIL;
    bond_hook = replace_peer_during_failed_lookup;
    authorize_peer(true);
    assert(state.public.connected && active_peer == 0x43 && !state.public.failed);
    assert(!state.public.ready && !state.closing);
    bond_result = ESP_OK;
    drain(); authorize_peer(true);
    assert(state.bond_resolved && !state.public.failed && !state.public.ready);

    start_bonded(); make_bond(1, 0x43, 0x22);
    commit_result = ESP_FAIL;
    commit_hook = replace_peer_during_failed_lookup;
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    assert(state.public.connected && active_peer == 0x43 && !state.public.failed);
    assert(!state.public.ready && !state.closing && response_calls == 0);
    commit_result = ESP_OK;
    drain(); authorize_peer(true);
    assert(state.bond_resolved && !state.public.failed && !state.public.ready);
}
static void test_secure_connections_and_crypto_initialization(void) {
    reset();
    assert(ble_hid_init() == ESP_OK && strict_auth_configured);
    assert(!crypto_initialized); /* Initialization belongs to the actual worker. */
    worker_cycle();
    assert(crypto_initialized && !state.public.failed);

    start_bonded();
    crypto_initialized = false;
    write_cccd(ATTR_KEYBOARD_CCCD, true);
    assert(state.public.failed && !state.public.ready && commit_calls == 0);

    reset(); make_bond(0, active_peer, 0x11); connect_peer(); drain();
    esp_ble_gap_cb_param_t p = {0};
    p.ble_security.auth_cmpl.bd_addr[0] = active_peer;
    p.ble_security.auth_cmpl.success = true;
    p.ble_security.auth_cmpl.auth_mode = ESP_LE_AUTH_BOND; /* Existing legacy bond. */
    gap_callback(ESP_GAP_BLE_AUTH_CMPL_EVT, &p);
    worker_cycle();
    assert(!state.public.ready && state.closing && !state.bond_resolved);
    assert(commit_calls == 0 && fake_bond_count == 1);
}
static void test_old_authentication_jobs_cannot_fault_replacement(void) {
    start_bonded();
    uint8_t enabled[] = {1, 0};
    write_attribute(ATTR_KEYBOARD_CCCD, enabled, 2, 0, false); /* Revision 1 job. */
    assert(state.cccd_pending == 1);
    make_bond(0, active_peer, 0x33);
    esp_ble_gap_cb_param_t p = {0};
    p.ble_security.auth_cmpl.bd_addr[0] = active_peer;
    p.ble_security.auth_cmpl.success = true;
    p.ble_security.auth_cmpl.auth_mode = ESP_LE_AUTH_REQ_SC_BOND;
    gap_callback(ESP_GAP_BLE_AUTH_CMPL_EVT, &p); /* Revision 2, same physical link. */
    assert(state.cccd_pending == 0);
    write_attribute(ATTR_MOUSE_CCCD, enabled, 2, 0, false); /* New revision's job. */
    assert(state.cccd_pending == 1);
    worker_cycle(); /* Drops old job; must not consume the new revision's count. */
    assert(!state.public.failed && !state.public.ready && state.cccd_pending == 1);
    assert(commit_calls == 0 && last_response_status == ESP_GATT_INSUF_AUTHENTICATION);
    worker_cycle();
    assert(!state.public.failed && state.cccd_pending == 0 && commit_calls == 1);
    assert(!state.keyboard_cccd && state.mouse_cccd && !state.public.ready);
}
static void test_no_response_disable_overflow(void) {
    reset(); connect_peer(); drain(); authorize_peer(false);
    esp_ble_gatts_cb_param_t p = {0};
    p.write.conn_id = 3;
    p.write.bda[0] = active_peer;
    p.write.handle = state.handles[ATTR_KEYBOARD_CCCD];
    p.write.need_rsp = false;
    uint8_t value[] = {1, 0};
    p.write.value = value;
    p.write.len = 2;
    callback_active = true;
    for (unsigned i = 0; i < CONTROL_QUEUE_LENGTH; ++i) {
        gatts_callback(ESP_GATTS_WRITE_EVT, 7, &p);
    }
    value[0] = 0;
    gatts_callback(ESP_GATTS_WRITE_EVT, 7, &p);
    callback_active = false;
    assert(state.public.failed && !state.public.ready && !state.keyboard_cccd);
    for (unsigned i = 0; i < CONTROL_QUEUE_LENGTH; ++i) worker_cycle();
    assert(state.public.failed && !state.public.ready && !state.keyboard_cccd && state.closing);
    assert(response_calls == 0);
}
static void test_subscription_capacity(void) {
    reset();
    for (unsigned i = 0; i < SUBSCRIPTION_CAPACITY; ++i) make_bond(i, (uint8_t)(0x40U + i), (uint8_t)(i + 1));
    for (unsigned i = 0; i < SUBSCRIPTION_CAPACITY; ++i) {
        active_peer = (uint8_t)(0x40U + i);
        connect_peer(); drain(); authorize_peer(true);
        write_cccd(ATTR_KEYBOARD_CCCD, true);
        assert(!state.public.failed && !state.public.ready);
        disconnect_peer();
    }
    assert(subscriptions.count == SUBSCRIPTION_CAPACITY);
    make_bond(0, 0x80, 0x55); active_peer = 0x80;
    connect_peer(); drain(); authorize_peer(true);
    write_cccd(ATTR_MOUSE_CCCD, true);
    assert(!state.public.failed && subscriptions.count == SUBSCRIPTION_CAPACITY);
    assert(durable_length == SUBSCRIPTION_BLOB_LENGTH);
}
static void rssi_complete(uint8_t peer, int status, int8_t value) {
    esp_ble_gap_cb_param_t p = {0};
    p.read_rssi_cmpl.status = status;
    p.read_rssi_cmpl.rssi = value;
    p.read_rssi_cmpl.remote_addr[0] = peer;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT, &p);
    callback_active = false;
}
static void security_request(void) {
    esp_ble_gap_cb_param_t p = {0};
    p.ble_security.ble_req.bd_addr[0] = active_peer;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_SEC_REQ_EVT, &p);
    callback_active = false;
    worker_cycle();
}
static void remove_complete(const esp_bd_addr_t address, int status, bool actually_remove) {
    if (status == ESP_BT_STATUS_SUCCESS && actually_remove) {
        for (int i = 0; i < fake_bond_count; ++i) {
            if (memcmp(fake_bonds[i].bd_addr, address, ESP_BD_ADDR_LEN) == 0) {
                --fake_bond_count;
                memmove(&fake_bonds[i], &fake_bonds[i + 1],
                    (size_t)(fake_bond_count - i) * sizeof(fake_bonds[0]));
                break;
            }
        }
    }
    esp_ble_gap_cb_param_t p = {0};
    p.remove_bond_dev_cmpl.status = status;
    memcpy(p.remove_bond_dev_cmpl.bd_addr, address, ESP_BD_ADDR_LEN);
    callback_active = true;
    gap_callback(ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT, &p);
    callback_active = false;
}
static void advertising_stopped(int status) {
    esp_ble_gap_cb_param_t p = {0};
    p.adv_stop_cmpl.status = status;
    gap_callback(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, &p);
}
static void test_pairing_window_lifecycle(void) {
    reset_startup();
    assert(ble_hid_init() == ESP_OK);
    assert(!ble_hid_snapshot().pairing_open && !ble_hid_snapshot().bond_count_known);
    worker_cycle();
    ble_hid_snapshot_t snapshot = ble_hid_snapshot();
    assert(snapshot.bond_count_known && snapshot.bond_count == 0);
    assert(snapshot.pairing_open && snapshot.pairing_remaining_ms == BLE_HID_PAIRING_DEFAULT_MS);
    fake_tick = BLE_HID_PAIRING_DEFAULT_MS - 1;
    assert(ble_hid_snapshot().pairing_remaining_ms == 1);
    ++fake_tick;
    notification_watchdog(NULL);
    assert(!ble_hid_snapshot().pairing_open && ble_hid_snapshot().pairing_remaining_ms == 0);
    worker_cycle();
    assert(!ble_hid_snapshot().pairing_open); /* Zero bonds do not reopen an expired window. */
    assert(!ble_hid_pairing_open(0) && !ble_hid_pairing_open(BLE_HID_PAIRING_MAX_MS + 1));
    esp_ble_gatts_cb_param_t registration = {0};
    registration.reg.app_id = HID_APP_ID;
    registration.reg.status = ESP_GATT_OK;
    gatts_callback(ESP_GATTS_REG_EVT, 7, &registration);
    drain();
    connect_peer();
    assert(!state.public.connected && state.rejected_pending);
    worker_cycle();
    assert(state.rejected_pending && !state.public.connected);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    disconnect_peer();
    assert(!state.rejected_pending);
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_MAX_MS));
    connect_peer(); drain();
    assert(state.public.connected && state.pairing_admission_pending && !state.pairing_admitted);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    ble_hid_pairing_close();
    security_request();
    assert(last_security_accept); /* Already admitted handshake may finish. */
    authorize_peer(false);
    assert(state.auth_complete && !state.public.failed);
    security_request(); assert(!last_security_accept); /* No fresh pairing on the completed link. */

    reset_startup(); make_bond(0, active_peer, 0x11);
    assert(ble_hid_init() == ESP_OK);
    worker_cycle();
    assert(ble_hid_snapshot().bond_count_known && ble_hid_snapshot().bond_count == 1);
    assert(!ble_hid_snapshot().pairing_open);

    reset_startup();
    assert(ble_hid_init() == ESP_OK);
    ble_hid_pairing_close();
    worker_cycle();
    assert(!ble_hid_snapshot().pairing_open); /* Explicit close wins over first-boot default. */

    reset();
    fake_tick = UINT32_MAX - 1000U;
    assert(ble_hid_pairing_open(2000));
    fake_tick += 1999U;
    assert(ble_hid_snapshot().pairing_open && ble_hid_snapshot().pairing_remaining_ms == 1);
    ++fake_tick;
    assert(!ble_hid_snapshot().pairing_open);
}
static void test_saved_reconnect_outside_pairing(void) {
    start_bonded(); subscribe();
    disconnect_peer(); ble_hid_pairing_close();
    connect_peer();
    assert(state.public.connected && state.peer_saved && !state.pairing_admitted);
    sdk_calls = 0;
    worker_cycle();
    assert(sdk_calls == 0); /* No fresh peripheral SMP procedure outside the window. */
    security_request();
    assert(!last_security_accept && !state.public.failed);
    authorize_peer(true); /* Host's existing LTK re-encryption completion. */
    assert(state.public.ready && !ble_hid_snapshot().pairing_open);
    disconnect_peer(); active_peer = 0x43;
    connect_peer();
    assert(!state.public.connected && state.rejected_pending && !state.public.ready);
    worker_cycle(); disconnect_peer();
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    connect_peer(); drain();
    fake_tick += PAIRING_HANDSHAKE_TIMEOUT_MS - 1;
    notification_watchdog(NULL); assert(!state.link_fault);
    ++fake_tick;
    notification_watchdog(NULL);
    assert(state.link_fault && !state.public.ready);
    worker_cycle();
    assert(state.closing);
    security_request(); assert(!last_security_accept);
}
static void test_stale_rejection_and_reject_deadline(void) {
    reset(); ble_hid_pairing_close();
    connect_peer(); /* Queued rejection for A; A leaves before worker handles it. */
    disconnect_peer();
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    active_peer = 0x43; connect_peer();
    worker_cycle();
    assert(sdk_calls == 1 && state.public.connected && !state.closing && !state.public.failed);
    worker_cycle(); assert(sdk_calls == 1); /* Only B's independently admitted encryption request. */

    reset(); ble_hid_pairing_close();
    connect_peer(); uint64_t first_reject = state.rejected_generation;
    disconnect_peer(); connect_peer();
    assert(state.rejected_generation != first_reject);
    worker_cycle(); assert(sdk_calls == 0);
    worker_cycle(); assert(sdk_calls == 1 && state.rejected_pending);
    fake_tick = REJECT_TIMEOUT_MS;
    notification_watchdog(NULL);
    assert(state.public.failed && !state.public.ready);
    disconnect_peer();
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
}
static void test_rssi_actual_async_samples(void) {
    reset(); connect_peer(); drain(); authorize_peer(false);
    assert(rssi_calls == 1 && ble_hid_snapshot().rssi_pending && !ble_hid_snapshot().rssi_valid);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -54);
    ble_hid_snapshot_t snapshot = ble_hid_snapshot();
    assert(snapshot.rssi_valid && snapshot.rssi_dbm == -54 && snapshot.rssi_age_ms == 0);
    assert(!snapshot.rssi_pending && snapshot.rssi_error == ESP_OK);
    fake_tick = 1000;
    assert(ble_hid_snapshot().rssi_age_ms == 1000);
    worker_cycle(); assert(rssi_calls == 1);
    fake_tick = RSSI_INTERVAL_MS;
    worker_cycle(); assert(rssi_calls == 2);
    rssi_complete(active_peer, 1, 0);
    assert(!ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_error == ESP_FAIL);
    fake_tick += RSSI_INTERVAL_MS;
    worker_cycle(); rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, 127);
    assert(!ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_error == ESP_ERR_NOT_FOUND);
    fake_tick += RSSI_INTERVAL_MS;
    worker_cycle(); rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -128);
    assert(!ble_hid_snapshot().rssi_valid);
    fake_tick += RSSI_INTERVAL_MS;
    worker_cycle(); rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, 20);
    assert(ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_dbm == 20);
    fake_tick += RSSI_MAX_AGE_MS + 1;
    assert(!ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_error == ESP_ERR_TIMEOUT);
    notification_watchdog(NULL);
    assert(!state.public.rssi_valid && !state.public.failed);

    reset(); rssi_request_result = ESP_FAIL;
    connect_peer(); drain(); authorize_peer(false);
    assert(!state.rssi_inflight && !ble_hid_snapshot().rssi_pending);
    assert(ble_hid_snapshot().rssi_error == ESP_FAIL && !state.public.failed);
    rssi_request_result = ESP_OK;
    fake_tick += RSSI_INTERVAL_MS; worker_cycle();
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -127);
    assert(ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_dbm == -127);
}
static void test_rssi_stale_callbacks_and_timeout(void) {
    reset(); connect_peer(); drain(); authorize_peer(false);
    uint64_t old = state.connection_generation;
    disconnect_peer(); connect_peer(); drain(); authorize_peer(false);
    assert(state.connection_generation != old && rssi_calls == 1);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -77);
    assert(!ble_hid_snapshot().rssi_valid); /* Same address, older connection. */
    worker_cycle(); assert(rssi_calls == 2);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -42);
    assert(ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_dbm == -42);

    fake_tick += RSSI_INTERVAL_MS; worker_cycle();
    disconnect_peer(); active_peer = 0x43; connect_peer(); drain(); authorize_peer(false);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -10);
    assert(state.rssi_inflight && !ble_hid_snapshot().rssi_valid); /* Foreign callback cannot drain A. */
    rssi_complete(0x42, ESP_BT_STATUS_SUCCESS, -88);
    assert(!state.rssi_inflight && !ble_hid_snapshot().rssi_valid);
    worker_cycle();
    assert(last_rssi_address[0] == active_peer);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -33);
    assert(ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_dbm == -33);

    reset(); connect_peer(); drain(); authorize_peer(false);
    fake_tick = RSSI_TIMEOUT_MS;
    notification_watchdog(NULL);
    assert(!ble_hid_snapshot().rssi_pending && !ble_hid_snapshot().rssi_valid);
    assert(ble_hid_snapshot().rssi_error == ESP_ERR_TIMEOUT && state.rssi_inflight);
    worker_cycle(); assert(rssi_calls == 1 && !state.public.failed);
    disconnect_peer(); connect_peer(); drain(); authorize_peer(false);
    assert(rssi_calls == 1);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -9);
    assert(!ble_hid_snapshot().rssi_valid);
    worker_cycle(); assert(rssi_calls == 2);
    rssi_complete(active_peer, ESP_BT_STATUS_SUCCESS, -50);
    assert(ble_hid_snapshot().rssi_valid && ble_hid_snapshot().rssi_dbm == -50);
}
static void test_forget_guards_and_completion(void) {
    start_bonded(); subscribe();
    uint64_t generation = state.public.generation;
    assert(!ble_hid_forget_bonds(generation) && state.public.ready);
    uint8_t zero[8] = {0};
    assert(ble_hid_send(1, zero, sizeof zero));
    disconnect_peer();
    generation = state.public.generation;
    assert(!ble_hid_forget_bonds(generation)); /* Old notification must drain first. */
    complete_notification(ESP_GATT_OK);
    assert(!ble_hid_forget_bonds(generation - 1));
    assert(ble_hid_forget_bonds(generation));
    assert(state.public.generation != generation && !ble_hid_snapshot().pairing_open);
    assert(ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_RUNNING);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    assert(!ble_hid_forget_bonds(state.public.generation));
    unsigned notifications = notify_calls;
    worker_cycle();
    assert(remove_calls == 1 && state.remove_inflight && last_remove_address[0] == 0x42);
    assert(!ble_hid_snapshot().bond_count_known && commit_calls == 2);
    esp_bd_addr_t foreign = {0x43};
    remove_complete(foreign, ESP_BT_STATUS_SUCCESS, false);
    worker_cycle(); assert(remove_calls == 1 && state.remove_inflight);
    esp_bd_addr_t saved;
    memcpy(saved, last_remove_address, sizeof saved);
    remove_complete(saved, ESP_BT_STATUS_SUCCESS, true);
    worker_cycle();
    ble_hid_snapshot_t snapshot = ble_hid_snapshot();
    assert(snapshot.bond_forget_state == BLE_HID_FORGET_DONE && snapshot.bond_forget_error == ESP_OK);
    assert(snapshot.bond_count_known && snapshot.bond_count == 0 && fake_bond_count == 0);
    assert(snapshot.pairing_open && snapshot.pairing_remaining_ms == BLE_HID_PAIRING_DEFAULT_MS);
    assert(subscriptions.count == 0 && durable_length == 4 && durable_blob[3] == 0);
    assert(notify_calls == notifications && !state.public.failed);
    remove_complete(saved, 1, false); /* Late unrelated completion cannot rewrite DONE. */
    assert(ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_DONE);
}
static void test_forget_multiple_and_capacity(void) {
    reset();
    for (unsigned i = 0; i < SUBSCRIPTION_CAPACITY; ++i)
        make_bond(i, (uint8_t)(0x40U + i), (uint8_t)(i + 1));
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.known && list.count == BLE_HID_MAX_BONDS && !list.current_match_known);
    for (unsigned i = 0; i < BLE_HID_MAX_BONDS; ++i) {
        assert(list.bonds[i].address[0] == (uint8_t)(0x40U + i));
        assert(list.bonds[i].address_type_known && list.bonds[i].address_type == 1);
    }
    ble_hid_pairing_close();
    assert(ble_hid_forget_bonds(state.public.generation));
    worker_cycle();
    for (unsigned i = 0; i < SUBSCRIPTION_CAPACITY; ++i) {
        assert(remove_calls == i + 1 && state.remove_inflight);
        esp_bd_addr_t address;
        memcpy(address, last_remove_address, sizeof address);
        remove_complete(address, ESP_BT_STATUS_SUCCESS, true);
        worker_cycle();
    }
    assert(fake_bond_count == 0 && remove_calls == SUBSCRIPTION_CAPACITY);
    assert(ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_DONE);
}
static void assert_current_bond(int expected, unsigned count) {
    ble_hid_bond_list_t list = observed_bonds();
    assert(list.known && !list.updating && list.error == ESP_OK && list.count == count);
    assert(list.current_match_known == (expected >= 0));
    for (unsigned i = 0; i < count; ++i) {
        assert(list.bonds[i].is_current == (expected >= 0 && i == (unsigned)expected));
    }
}

static void test_multiple_saved_hosts_and_reconnect(void) {
    reset();
    for (unsigned i = 0; i < 3; ++i) {
        assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
        assert(ble_hid_bond_list().count == i && fake_bond_count == (int)i);
        active_peer = (uint8_t)(0x50U + i);
        connect_peer(); drain();
        save_sdk_bond(i, active_peer, (uint8_t)(0x11U + i));
        /* SDK persistence is not published until the worker reads it. */
        assert(ble_hid_bond_list().count == i && !ble_hid_bond_list().current_match_known);
        uint64_t previous = ble_hid_bond_list().revision;
        authorize_peer(true);
        assert(ble_hid_bond_list().revision > previous);
        assert_current_bond((int)i, i + 1U);
        if (i != 2) write_cccd(ATTR_KEYBOARD_CCCD, true);
        if (i != 1) write_cccd(ATTR_MOUSE_CCCD, true);
        assert(!ble_hid_snapshot().pairing_open);
        assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
        disconnect_peer();
        assert_current_bond(-1, i + 1U);
    }
    assert(subscriptions.count == 3 && fake_bond_count == 3 && remove_calls == 0);
    unsigned commits = commit_calls;
    const unsigned order[] = {0, 2, 1, 0, 1, 2};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        unsigned host = order[i];
        active_peer = (uint8_t)(0x50U + host);
        connect_peer(); drain();
        assert(state.public.connected && state.peer_saved && !state.pairing_admitted);
        assert_current_bond(-1, 3); /* Admission is not verified authentication. */
        authorize_peer(true);
        assert_current_bond((int)host, 3);
        assert(state.keyboard_cccd == (host != 2) && state.mouse_cccd == (host != 1));
        assert(ble_hid_snapshot().ready == (host == 0));
        assert(fake_bond_count == 3 && subscriptions.count == 3 && commit_calls == commits);
        disconnect_peer();
        assert_current_bond(-1, 3);
    }
    active_peer = 0x50; connect_peer(); drain(); authorize_peer(true);
    active_peer = 0x51; connect_peer();
    assert(state.public.connected && state.address[0] == 0x50 && state.rejected_pending);
    assert_current_bond(0, 3); /* Several saved hosts still means one active target. */
    worker_cycle(); disconnect_peer();
    active_peer = 0x50; disconnect_peer();
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    assert_current_bond(-1, 3);
    assert(fake_bond_count == 3 && remove_calls == 0 && notify_calls == 0 && commit_calls == commits);
}

static void test_pairing_capacity_preserves_existing_bonds(void) {
    reset();
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    for (unsigned i = 0; i < PAIRING_BOND_CAPACITY; ++i) {
        make_bond(i, (uint8_t)(0x40U + i), (uint8_t)(i + 1));
    }
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.known && list.count == PAIRING_BOND_CAPACITY && list.capacity == PAIRING_BOND_CAPACITY);
    ble_hid_snapshot_t snapshot = ble_hid_snapshot();
    assert(snapshot.bond_count_known && snapshot.bond_count == snapshot.bond_capacity);
    assert(snapshot.bond_capacity == list.capacity);
    assert(!ble_hid_snapshot().pairing_open);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    active_peer = 0x99; connect_peer();
    assert(!state.public.connected && state.rejected_pending);
    worker_cycle(); disconnect_peer();
    assert(fake_bond_count == PAIRING_BOND_CAPACITY && remove_calls == 0 && commit_calls == 0);
    active_peer = 0x40; connect_peer(); drain(); authorize_peer(true);
    assert_current_bond(0, PAIRING_BOND_CAPACITY);
    assert(state.public.connected && !state.pairing_admitted);
    assert(fake_bond_count == PAIRING_BOND_CAPACITY && remove_calls == 0);
    disconnect_peer();
    bond_result = ESP_FAIL; assert(refresh_known_bonds() == ESP_FAIL);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
}

static void prepare_last_bond_slot(void) {
    reset();
    for (unsigned i = 0; i + 1U < PAIRING_BOND_CAPACITY; ++i) {
        make_bond(i, (uint8_t)(0x10U + i), (uint8_t)(i + 1U));
    }
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    active_peer = 0x80; connect_peer(); drain();
    assert(state.pairing_admission_pending && !state.pairing_admitted);
    worker_cycle();
    assert(state.pairing_admitted && !state.pairing_admission_pending && encryption_calls == 1);
}

static void test_auth_disconnect_refreshes_durable_bonds(void) {
    prepare_last_bond_slot();
    save_sdk_bond(PAIRING_BOND_CAPACITY - 1U, active_peer, 0x77);
    authentication_event(true, true); /* Do not step the worker here. */
    assert(!ble_hid_bond_list().known && state.bond_refresh_pending);
    assert(!ble_hid_snapshot().pairing_open);
    disconnect_peer();
    assert(state.bond_refresh_pending && !ble_hid_snapshot().connected);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    active_peer = 0x81; connect_peer();
    assert(state.rejected_pending && !state.public.connected && !state.pairing_admitted);
    worker_cycle(); /* The disconnected peer's durable refresh must still run. */
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.known && list.count == PAIRING_BOND_CAPACITY && !state.bond_refresh_pending);
    assert(list.bonds[PAIRING_BOND_CAPACITY - 1U].address[0] == 0x80);
    for (unsigned i = 0; i + 1U < PAIRING_BOND_CAPACITY; ++i) {
        assert(list.bonds[i].address[0] == (uint8_t)(0x10U + i));
    }
    disconnect_peer();
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    connect_peer(); worker_cycle();
    assert(state.rejected_pending && !state.public.connected && encryption_calls == 1);
    disconnect_peer(); active_peer = 0x80;
    connect_peer(); drain(); authorize_peer(true);
    assert_current_bond((int)PAIRING_BOND_CAPACITY - 1, PAIRING_BOND_CAPACITY);
    assert(state.peer_saved && !state.pairing_admitted && encryption_calls == 1);
    assert(fake_bond_count == PAIRING_BOND_CAPACITY && remove_calls == 0 && commit_calls == 0);
}

static void test_auth_after_disconnect_and_refresh_failure(void) {
    prepare_last_bond_slot();
    save_sdk_bond(PAIRING_BOND_CAPACITY - 1U, active_peer, 0x77);
    disconnect_peer();
    authentication_event(true, true); /* Late completion has no active link. */
    assert(state.bond_refresh_pending && !ble_hid_bond_list().known);
    assert(!ble_hid_snapshot().pairing_open);
    bond_result = ESP_FAIL; worker_cycle();
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(!list.known && list.error == ESP_FAIL && list.count == PAIRING_BOND_CAPACITY - 1U);
    assert(!state.bond_refresh_pending && !ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    active_peer = 0x81; connect_peer(); worker_cycle();
    assert(!state.public.connected && state.rejected_pending && encryption_calls == 1);
    disconnect_peer();
    /* An existing peer may recover a fresh list without admitting new SMP. */
    bond_result = ESP_OK; active_peer = 0x10;
    connect_peer(); drain(); authorize_peer(true);
    assert_current_bond(0, PAIRING_BOND_CAPACITY);
    assert(!state.public.failed && !state.pairing_admitted && encryption_calls == 1);
    assert(fake_bond_count == PAIRING_BOND_CAPACITY && remove_calls == 0);
}

static void test_pairing_rechecks_actual_capacity(void) {
    reset();
    for (unsigned i = 0; i + 1U < PAIRING_BOND_CAPACITY; ++i) {
        make_bond(i, (uint8_t)(0x10U + i), (uint8_t)(i + 1U));
    }
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    /* Simulate a durable SDK update whose completion has not reached the app. */
    save_sdk_bond(PAIRING_BOND_CAPACITY - 1U, 0x80, 0x77);
    assert(ble_hid_bond_list().known && ble_hid_bond_list().count == PAIRING_BOND_CAPACITY - 1U);
    active_peer = 0x81; connect_peer();
    assert(state.public.connected && state.pairing_admission_pending && !state.pairing_admitted);
    security_request();
    assert(!last_security_accept && !state.pairing_admitted && state.link_fault && state.closing);
    assert(encryption_calls == 0 && remove_calls == 0 && commit_calls == 0);
    assert(ble_hid_bond_list().known && ble_hid_bond_list().count == PAIRING_BOND_CAPACITY);
    assert(!ble_hid_snapshot().pairing_open && fake_bond_count == PAIRING_BOND_CAPACITY);

    reset(); connect_peer();
    bond_result = ESP_FAIL; security_request();
    assert(!state.pairing_admitted && !last_security_accept && encryption_calls == 0);
    assert(!ble_hid_bond_list().known && ble_hid_bond_list().error == ESP_FAIL);
    assert(state.public.failed && state.closing);
}

static void auth_change_during_cache_read(void) {
    bond_hook = NULL;
    save_sdk_bond(PAIRING_BOND_CAPACITY - 1U, 0x80, 0x77);
    authentication_event(true, true);
}

static void test_auth_invalidates_an_inflight_cache_read(void) {
    reset();
    for (unsigned i = 0; i + 1U < PAIRING_BOND_CAPACITY; ++i) {
        make_bond(i, (uint8_t)(0x10U + i), (uint8_t)(i + 1U));
    }
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    uint64_t revision = ble_hid_bond_list().revision;
    bond_hook = auth_change_during_cache_read;
    assert(refresh_known_bonds() == ESP_ERR_INVALID_STATE);
    assert(!ble_hid_bond_list().known && state.bond_refresh_pending);
    assert(ble_hid_bond_list().revision > revision && !ble_hid_snapshot().pairing_open);
    worker_cycle();
    assert_current_bond(-1, PAIRING_BOND_CAPACITY);
    assert(!state.bond_refresh_pending && !ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    assert(remove_calls == 0 && encryption_calls == 0 && fake_bond_count == PAIRING_BOND_CAPACITY);
}

static void test_bond_list_private_unknown_and_types(void) {
    reset();
    for (unsigned i = 0; i < 3; ++i) make_bond(i, (uint8_t)(0x42U + i), (uint8_t)(0x11U + i));
    connect_peer(); drain(); authorize_peer(true);
    assert_current_bond(0, 3);
    fake_bonds[0].bd_addr_type = 0;
    assert(refresh_known_bonds() == ESP_OK);
    assert_current_bond(-1, 3); /* Same bytes with a different type is not proof. */
    fake_bonds[0].bd_addr_type = 99;
    assert(refresh_known_bonds() == ESP_OK);
    assert(!ble_hid_bond_list().bonds[0].address_type_known);
    assert_current_bond(-1, 3);
    fake_bonds[0].bd_addr_type = 1;
    fake_bonds[3] = fake_bonds[0]; fake_bond_count = 4;
    assert(refresh_known_bonds() == ESP_OK);
    assert_current_bond(-1, 4); /* An ambiguous list cannot identify a current row. */
    fake_bond_count = 3; fake_bonds[0].bd_addr[0] = 0x71;
    assert(refresh_known_bonds() == ESP_OK);
    assert_current_bond(-1, 3); /* An unmapped/private alias stays unidentified. */
    disconnect_peer(); active_peer = 0x99;
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    connect_peer(); drain(); authorize_peer(false);
    assert(ble_hid_snapshot().connected && ble_hid_snapshot().peer_address[0] == 0x99);
    assert_current_bond(-1, 3);

    for (uint8_t type = 0; type < 4; ++type) {
        reset(); make_bond(0, active_peer, 0x11);
        fake_bonds[0].bd_addr_type = type;
        assert(refresh_known_bonds() == ESP_OK);
        esp_ble_gatts_cb_param_t p = {0};
        p.connect.conn_id = 3; p.connect.remote_bda[0] = active_peer;
        p.connect.ble_addr_type = type;
        gatts_callback(ESP_GATTS_CONNECT_EVT, 7, &p); drain(); authorize_peer(true);
        assert_current_bond(0, 1);
        assert(ble_hid_bond_list().bonds[0].address_type == type);
    }
}

static void remove_during_bond_read(void) {
    fake_bond_count = 0;
    bond_hook = NULL;
}

static void add_after_bond_count(void) {
    bond_count_hook = NULL;
    save_sdk_bond(0, active_peer, 0x11);
}

static void test_bond_list_cache_availability(void) {
    reset_startup();
    ble_hid_bond_list_t list = observed_bonds();
    assert(!list.known && !list.updating && list.error == ESP_ERR_INVALID_STATE);
    assert(!ble_hid_snapshot().bond_count_known);
    assert(ble_hid_snapshot().bond_capacity == PAIRING_BOND_CAPACITY);
    assert(list.revision == 0 && !list.current_match_known);
    assert(ble_hid_init() == ESP_OK); worker_cycle();
    list = observed_bonds();
    assert(list.known && list.count == 0 && list.error == ESP_OK && list.revision != 0);
    for (unsigned i = 0; i < 3; ++i) make_bond(i, (uint8_t)(0x42U + i), (uint8_t)(i + 1));
    list = observed_bonds();
    /* Render copies may run with SDK/storage unavailable and have no side
     * effects on the cached revision, status or actual saved devices. */
    callback_active = true;
    for (unsigned i = 0; i < 1000; ++i) {
        ble_hid_bond_list_t copy = observed_bonds();
        assert(copy.revision == list.revision && copy.known && copy.count == 3);
    }
    callback_active = false;
    assert(fake_bond_count == 3 && commit_calls == 0 && remove_calls == 0);
    bond_result = ESP_FAIL;
    assert(refresh_known_bonds() == ESP_FAIL);
    ble_hid_bond_list_t failed = observed_bonds();
    assert(!failed.known && !failed.updating && failed.error == ESP_FAIL && failed.count == 3);
    assert(failed.revision > list.revision && failed.bonds[0].address[0] == 0x42);
    assert(!ble_hid_snapshot().bond_count_known && !failed.current_match_known);
    bond_result = ESP_OK; assert(refresh_known_bonds() == ESP_OK);
    assert_current_bond(-1, 3);
    fake_bond_count = -1;
    assert(refresh_known_bonds() == ESP_ERR_INVALID_STATE && !observed_bonds().known);
    fake_bond_count = (int)BLE_HID_MAX_BONDS + 1;
    assert(refresh_known_bonds() == ESP_ERR_INVALID_STATE && observed_bonds().count == 3);
    fake_bond_count = 0; assert(refresh_known_bonds() == ESP_OK);
    list = observed_bonds();
    assert(list.known && list.count == 0);
    for (unsigned i = 0; i < BLE_HID_MAX_BONDS; ++i) {
        assert(list.bonds[i].address[0] == 0 && !list.bonds[i].address_type_known);
    }

    reset_startup(); save_sdk_bond(0, active_peer, 0x11);
    bond_hook = remove_during_bond_read;
    assert(ble_hid_init() == ESP_OK); worker_cycle();
    assert(state.public.failed && !ble_hid_snapshot().pairing_open);
    list = observed_bonds();
    assert(!list.known && list.error == ESP_ERR_INVALID_STATE);

    reset_startup();
    bond_count_hook = add_after_bond_count;
    assert(ble_hid_init() == ESP_OK); worker_cycle();
    list = observed_bonds();
    assert(list.known && list.count == 1 && list.bonds[0].address[0] == active_peer);
    assert(!ble_hid_snapshot().pairing_open && !state.public.failed);
}

static void test_partial_forget_cached_list(void) {
    reset();
    for (unsigned i = 0; i < 3; ++i) make_bond(i, (uint8_t)(0x42U + i), (uint8_t)(i + 1));
    uint64_t revision = ble_hid_bond_list().revision;
    assert(ble_hid_forget_bonds(state.public.generation));
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(!list.known && list.updating && list.count == 3 && list.revision > revision);
    worker_cycle();
    esp_bd_addr_t first, second;
    memcpy(first, last_remove_address, sizeof first);
    remove_complete(first, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
    list = ble_hid_bond_list();
    assert(!list.known && list.updating && list.count == 2 && fake_bond_count == 2);
    memcpy(second, last_remove_address, sizeof second);
    remove_complete(second, 1, false); worker_cycle();
    list = ble_hid_bond_list();
    assert(!list.known && !list.updating && list.error == ESP_FAIL && list.count == 2);
    assert(list.bonds[0].address[0] == 0x43 && list.bonds[1].address[0] == 0x44);
    assert(!list.current_match_known && !ble_hid_snapshot().bond_count_known);
    assert(ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_FAILED);
    assert(!ble_hid_snapshot().pairing_open && commit_calls == 0 && fake_bond_count == 2);
    revision = list.revision;
    remove_complete(first, ESP_BT_STATUS_SUCCESS, false); worker_cycle();
    assert(ble_hid_bond_list().revision == revision && !ble_hid_bond_list().known);
}

static void test_forget_advertising_and_rejected_peer(void) {
    reset(); make_bond(0, 0x42, 0x11);
    state.advertising = true;
    assert(ble_hid_forget_bonds(state.public.generation));
    worker_cycle();
    assert(state.adv_stop_pending && state.advertising && remove_calls == 0);
    active_peer = 0x43;
    connect_peer();
    assert(state.rejected_pending && !state.public.connected);
    worker_cycle();
    assert(remove_calls == 0);
    disconnect_peer();
    /* The connect stopped legacy advertising even if its explicit stop later fails. */
    advertising_stopped(1);
    worker_cycle();
    assert(!state.adv_stop_pending && !state.public.failed && remove_calls == 1);
    esp_bd_addr_t saved;
    memcpy(saved, last_remove_address, sizeof saved);
    remove_complete(saved, ESP_BT_STATUS_SUCCESS, true);
    worker_cycle();
    assert(ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_DONE);
}
static void test_forget_failures_and_late_callbacks(void) {
    reset(); make_bond(0, 0x42, 0x11);
    remove_request_result = ESP_FAIL;
    assert(ble_hid_forget_bonds(state.public.generation)); worker_cycle();
    assert(state.public.failed && ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_FAILED);
    assert(ble_hid_snapshot().bond_forget_error == ESP_FAIL && !ble_hid_snapshot().bond_count_known);
    assert(fake_bond_count == 1 && !ble_hid_snapshot().pairing_open);

    reset(); make_bond(0, 0x42, 0x11);
    assert(ble_hid_forget_bonds(state.public.generation)); worker_cycle();
    esp_bd_addr_t address;
    memcpy(address, last_remove_address, sizeof address);
    remove_complete(address, 1, false); worker_cycle();
    assert(state.public.failed && ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_FAILED);

    reset(); make_bond(0, 0x42, 0x11);
    assert(ble_hid_forget_bonds(state.public.generation)); worker_cycle();
    memcpy(address, last_remove_address, sizeof address);
    remove_complete(address, ESP_BT_STATUS_SUCCESS, false); worker_cycle();
    assert(state.public.failed && remove_calls == 1); /* Successful status without actual deletion is insufficient. */

    reset(); make_bond(0, 0x42, 0x11);
    assert(ble_hid_forget_bonds(state.public.generation)); worker_cycle();
    memcpy(address, last_remove_address, sizeof address);
    fake_tick = FORGET_TIMEOUT_MS; notification_watchdog(NULL);
    assert(ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_FAILED);
    assert(ble_hid_snapshot().bond_forget_error == ESP_ERR_TIMEOUT && state.public.failed);
    remove_complete(address, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
    assert(!ble_hid_snapshot().pairing_open && ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_FAILED);
    assert(!ble_hid_forget_bonds(state.public.generation));

    reset(); make_bond(0, 0x42, 0x11); state.advertising = true;
    assert(ble_hid_forget_bonds(state.public.generation)); worker_cycle();
    fake_tick = FORGET_TIMEOUT_MS; notification_watchdog(NULL);
    advertising_stopped(ESP_BT_STATUS_SUCCESS); worker_cycle();
    assert(state.public.failed && remove_calls == 0 && !ble_hid_snapshot().pairing_open);

    reset(); make_bond(0, 0x42, 0x11);
    assert(ble_hid_forget_bonds(state.public.generation)); worker_cycle();
    memcpy(address, last_remove_address, sizeof address);
    remove_complete(address, ESP_BT_STATUS_SUCCESS, true);
    commit_result = ESP_FAIL; worker_cycle();
    assert(state.public.failed && ble_hid_snapshot().bond_forget_state == BLE_HID_FORGET_FAILED);
    assert(!ble_hid_snapshot().bond_count_known && !ble_hid_snapshot().pairing_open);

    reset(); make_bond(0, 0x42, 0x11);
    assert(ble_hid_forget_bonds(state.public.generation));
    bond_result = ESP_FAIL; worker_cycle();
    assert(state.public.failed && remove_calls == 0 && !ble_hid_snapshot().bond_count_known);
}
static void test_repeated_lifecycle(void) {
    reset();
    uint8_t keyboard[8] = {0};
    uint8_t mouse[4] = {0};
    uint64_t previous_generation = 0;
    fake_tick = UINT32_MAX - 100U;
    for (unsigned cycle = 0; cycle < 100000U; ++cycle) {
        assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
        connect_peer();
        drain();
        assert(state.public.generation > previous_generation);
        assert(!state.public.ready && !state.public.keyboard_leds);
        assert(!ble_hid_send_generation(previous_generation, 1, keyboard, 8));
        subscribe();
        assert(ble_hid_send(1, keyboard, sizeof(keyboard)));
        complete_notification(ESP_GATT_OK);
        assert(ble_hid_send(2, mouse, sizeof(mouse)));
        complete_notification(ESP_GATT_OK);
        uint8_t leds = (uint8_t)(cycle & 0x1fU);
        write_attribute(ATTR_LED, &leds, 1, 0, false);
        drain();
        previous_generation = state.public.generation;
        disconnect_peer();
        assert(!state.public.ready && !state.public.connected && !state.public.congested);
        assert(!state.public.keyboard_leds && !state.notification_pending);
        fake_tick += 60000U;
        service_link_control();
        assert(!state.public.failed);
        esp_ble_gap_cb_param_t p = {0};
        p.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS;
        gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &p);
    }
    assert(notify_calls == 200000U && advertising_calls == 100000U);
    assert(!ble_hid_failure().recorded && uptime_reads == advertising_diagnostics.total_events);
}

static void assert_same_failure(ble_hid_failure_t expected) {
    ble_hid_failure_t actual = ble_hid_failure();
    assert(actual.recorded == expected.recorded && actual.stage == expected.stage);
    assert(actual.error == expected.error && actual.sdk_status_known == expected.sdk_status_known);
    assert(actual.sdk_status == expected.sdk_status && actual.uptime_ms == expected.uptime_ms);
    assert(ble_hid_failure_stage_name() == expected.stage);
}

static void test_first_failure_initialization(void) {
    reset_startup();
    ble_hid_failure_t none = ble_hid_failure();
    assert(!none.recorded && none.error == ESP_OK && strcmp(none.stage, "none") == 0);
    assert(!none.sdk_status_known && none.sdk_status == 0 && none.uptime_ms == 0);
    assert(uptime_reads == advertising_diagnostics.total_events);
    fake_uptime_us = 1234567;
    nvs_result = ESP_ERR_NO_MEM;
    assert(ble_hid_init() == ESP_ERR_NO_MEM);
    ble_hid_failure_t first = ble_hid_failure();
    assert(first.recorded && first.error == ESP_ERR_NO_MEM && first.uptime_ms == 1234);
    assert(strcmp(first.stage, "NVS initialization (data preserved)") == 0);
    assert(!first.sdk_status_known && !state.public.initialized && state.public.failed);
    fake_uptime_us = 9000000;
    assert(ble_hid_init() == ESP_ERR_INVALID_STATE);
    fail_transport("Later failure", ESP_FAIL);
    assert_same_failure(first);
    assert(uptime_reads == advertising_diagnostics.total_events + 1U);

    reset_startup(); controller_init_result = ESP_ERR_NO_MEM;
    assert(ble_hid_init() == ESP_ERR_NO_MEM);
    first = ble_hid_failure();
    assert(first.recorded && first.error == ESP_ERR_NO_MEM);
    assert(strcmp(first.stage, "Controller initialization") == 0 && !first.sdk_status_known);

    reset_startup(); task_create_fails = true;
    assert(ble_hid_init() == ESP_ERR_NO_MEM);
    assert(strcmp(ble_hid_failure().stage, "Control worker allocation") == 0);
    assert(app_registration_calls == 0 && !state.public.initialized);

    reset_startup(); /* Simulated reboot, the only supported clearing boundary. */
    assert(!ble_hid_failure().recorded && ble_hid_init() == ESP_OK);
    assert(!ble_hid_failure().recorded && state.public.initialized && !state.public.failed);
    assert(ble_hid_init() == ESP_ERR_INVALID_STATE && !ble_hid_failure().recorded);
}

static void registration_failure_before_init_returns(void) {
    esp_ble_gatts_cb_param_t p = {0};
    p.reg.app_id = HID_APP_ID;
    p.reg.status = ESP_GATT_ERROR;
    callback_active = true;
    gatts_callback(ESP_GATTS_REG_EVT, 7, &p);
    callback_active = false;
}

static void test_first_failure_asynchronous_setup(void) {
    reset_startup();
    fake_uptime_us = 5001987;
    registration_hook = registration_failure_before_init_returns;
    assert(ble_hid_init() == ESP_OK);
    assert(state.public.initialized && state.public.failed);
    ble_hid_failure_t first = ble_hid_failure();
    assert(first.recorded && first.error == ESP_FAIL && first.sdk_status_known);
    assert(first.sdk_status == ESP_GATT_ERROR && first.uptime_ms == 5001);
    assert(strcmp(first.stage, "GATT registration completion") == 0);
    esp_ble_gap_cb_param_t gap = {0};
    gap.adv_data_cmpl.status = 7;
    fake_uptime_us = 8000000;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT, &gap);
    callback_active = false;
    assert_same_failure(first);
    assert(uptime_reads == advertising_diagnostics.total_events + 1U);

    reset_startup(); assert(ble_hid_init() == ESP_OK);
    esp_ble_gatts_cb_param_t p = {0};
    p.reg.app_id = HID_APP_ID; p.reg.status = ESP_GATT_OK;
    gatts_callback(ESP_GATTS_REG_EVT, 7, &p);
    assert(!ble_hid_failure().recorded);
    p.add_attr_tab.status = 0x11;
    fake_uptime_us = 6789000;
    callback_active = true;
    gatts_callback(ESP_GATTS_CREAT_ATTR_TAB_EVT, 7, &p);
    callback_active = false;
    first = ble_hid_failure();
    assert(state.public.failed && first.sdk_status_known && first.sdk_status == 0x11);
    assert(first.uptime_ms == 6789 && strcmp(first.stage, "HID attribute table completion") == 0);

    const esp_gap_ble_cb_event_t events[] = {ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT,
        ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT, ESP_GAP_BLE_ADV_START_COMPLETE_EVT,
        ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT};
    const char *const names[] = {"Advertising data completion", "Scan response completion",
        "Advertising start completion", "Advertising stop completion"};
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        reset();
        gap.adv_data_cmpl.status = (int)i + 5; /* Shared status field in the SDK union. */
        callback_active = true;
        gap_callback(events[i], &gap);
        callback_active = false;
        first = ble_hid_failure();
        assert(first.recorded && first.sdk_status_known && first.sdk_status == i + 5);
        assert(first.error == ESP_FAIL && strcmp(first.stage, names[i]) == 0 && state.public.failed);
    }
}

static void test_first_failure_recovery_and_observation(void) {
    reset(); connect_peer(); drain(); subscribe();
    assert(!ble_hid_failure().recorded && state.public.ready);
    unsigned char before[sizeof(state)];
    memcpy(before, &state, sizeof(state));
    unsigned calls = sdk_calls;
    callback_active = true;
    for (unsigned i = 0; i < 1000; ++i) {
        assert(!ble_hid_failure().recorded);
        assert(strcmp(ble_hid_failure_stage_name(), "none") == 0);
    }
    callback_active = false;
    assert(memcmp(before, &state, sizeof(state)) == 0 && sdk_calls == calls && uptime_reads == advertising_diagnostics.total_events);
    uint8_t report[8] = {0};
    notify_result = ESP_ERR_NO_MEM; fake_uptime_us = 22000999;
    uint64_t generation = state.public.generation;
    assert(!ble_hid_send(1, report, sizeof(report)));
    ble_hid_failure_t first = ble_hid_failure();
    assert(first.recorded && first.error == ESP_ERR_NO_MEM && !first.sdk_status_known);
    assert(first.uptime_ms == 22000 && strcmp(first.stage, "Notification submission") == 0);
    assert(state.public.ready && !state.public.congested && state.public.generation == generation);
    notify_result = ESP_OK;
    assert(ble_hid_send(1, report, sizeof(report))); complete_notification(ESP_GATT_OK);
    assert(state.public.ready && !state.public.failed);
    assert_same_failure(first);
    fake_uptime_us = 33000000;
    assert(ble_hid_send(1, report, sizeof(report))); complete_notification(ESP_GATT_ERROR);
    assert(state.link_fault && !state.public.ready);
    disconnect_peer();
    assert(ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    connect_peer(); drain(); subscribe();
    assert(state.public.ready && !state.public.failed && !state.link_fault);
    assert_same_failure(first);
    assert(uptime_reads == advertising_diagnostics.total_events + 1U);
}

static void test_first_failure_authentication_and_deadline(void) {
    reset(); connect_peer(); drain();
    esp_ble_gap_cb_param_t p = {0};
    p.ble_security.auth_cmpl.bd_addr[0] = active_peer;
    p.ble_security.auth_cmpl.success = false;
    p.ble_security.auth_cmpl.fail_reason = 0x05;
    fake_uptime_us = 4567000;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_AUTH_CMPL_EVT, &p);
    callback_active = false;
    ble_hid_failure_t first = ble_hid_failure();
    assert(first.sdk_status_known && first.sdk_status == 0x05 && first.error == ESP_FAIL);
    assert(strcmp(first.stage, "Authentication completion") == 0 && first.uptime_ms == 4567);
    assert(state.link_fault && !state.public.failed);
    disconnect_peer(); worker_cycle();
    connect_peer(); drain(); subscribe();
    assert(state.public.ready && !state.public.failed);
    assert_same_failure(first);

    reset(); connect_peer(); drain(); subscribe();
    uint8_t report[8] = {0};
    assert(ble_hid_send(1, report, sizeof(report)));
    fake_tick = NOTIFICATION_TIMEOUT_MS;
    fake_uptime_us = INT64_C(4294968529) * 1000;
    notification_watchdog(NULL);
    first = ble_hid_failure();
    assert(first.recorded && first.uptime_ms == UINT64_C(4294968529));
    assert(first.error == ESP_ERR_TIMEOUT && !first.sdk_status_known);
    assert(strcmp(first.stage, "Notification completion timeout") == 0 && state.public.failed);
    fake_uptime_us += 1000000;
    complete_notification(ESP_GATT_OK); disconnect_peer();
    assert_same_failure(first);
}

static void test_first_failure_ignores_nonfaults_and_stale_jobs(void) {
    reset();
    uint8_t report[8] = {0};
    assert(!ble_hid_send(0, report, sizeof(report)));
    assert(!ble_hid_pairing_open(0) && !ble_hid_forget_bonds(1));
    rssi_request_result = ESP_FAIL;
    connect_peer(); drain(); subscribe();
    assert(rssi_calls != 0 && !ble_hid_failure().recorded);
    control_command_t old = {.conn_id = state.conn_id, .generation = state.connection_generation};
    memcpy(old.address, state.address, sizeof(old.address));
    assert(ble_hid_disconnect_generation(state.public.generation));
    service_link_control(); disconnect_peer();
    connect_peer(); drain();
    fail_link_current(&old, 0, false, "Obsolete link error", ESP_ERR_NO_MEM);
    assert(!state.public.failed && !ble_hid_failure().recorded && uptime_reads == advertising_diagnostics.total_events);
}

static void name_read_complete(int status, const char *value) {
    assert(name_reads_pending == 1);
    name_reads_pending = 0;
    esp_ble_gap_cb_param_t p = {0};
    p.get_dev_name_cmpl.status = status;
    p.get_dev_name_cmpl.name = (char *)value;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_GET_DEV_NAME_COMPLETE_EVT, &p);
    callback_active = false;
}

static void name_payload_complete(bool scan, int status) {
    esp_ble_gap_cb_param_t p = {0};
    if (scan) p.scan_rsp_data_cmpl.status = status;
    else p.adv_data_cmpl.status = status;
    callback_active = true;
    gap_callback(scan ? ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT :
        ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT, &p);
    callback_active = false;
}

static void name_start_complete(int status) {
    esp_ble_gap_cb_param_t p = {0};
    p.adv_start_cmpl.status = status;
    callback_active = true;
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &p);
    callback_active = false;
}

static void name_finish_payloads(void) {
    worker_cycle(); assert(device_name.ack == NAME_ACK_ADV);
    name_payload_complete(false, ESP_BT_STATUS_SUCCESS);
    worker_cycle(); assert(device_name.ack == NAME_ACK_SCAN);
    name_payload_complete(true, ESP_BT_STATUS_SUCCESS);
    worker_cycle();
    if (!state.public.connected) {
        assert(device_name.ack == NAME_ACK_START);
        name_start_complete(ESP_BT_STATUS_SUCCESS);
    }
}

static void name_begin_runtime(const char *name, bool advertising) {
    unsigned previous_gets = name_get_calls;
    state.advertising = advertising;
    assert(ble_hid_name_request(name, strlen(name), NULL) == ESP_OK);
    worker_cycle();
    if (advertising) {
        assert(device_name.ack == NAME_ACK_STOP && name_get_calls == previous_gets);
        advertising_stopped(ESP_BT_STATUS_SUCCESS);
        worker_cycle();
    }
    assert(device_name.ack == NAME_ACK_GET);
    name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    worker_cycle();
    assert(device_name.ack == NAME_ACK_GET && strcmp(fake_sdk_name, name) == 0);
    name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
}

static void test_name_validation_submission_and_copy(void) {
    reset_startup();
    const char *invalid[] = {"", " leading", "trailing ", "line\nname", "\x7f", "\xc0\xaf", "\xed\xa0\x80",
        "1234567890123456789"};
    uint64_t id = 99;
    assert(ble_hid_name_request(NULL, 1, &id) == ESP_ERR_INVALID_ARG);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(ble_hid_name_request(invalid[i], strlen(invalid[i]), &id) == ESP_ERR_INVALID_ARG);
    }
    const char embedded[] = {'a', '\0', 'b'};
    assert(ble_hid_name_request(embedded, sizeof(embedded), &id) == ESP_ERR_INVALID_ARG);
    assert(id == 99 && name_set_calls == 0 && name_get_calls == 0 && !ble_hid_failure().recorded);
    char input[] = {'D', 'e', 's', 'k'}; /* Explicit bounded input, no terminator. */
    assert(ble_hid_name_request(input, sizeof(input), &id) == ESP_OK && id != 0);
    memset(input, 'X', sizeof(input));
    ble_hid_name_snapshot_t snapshot = ble_hid_name_snapshot();
    assert(snapshot.status == BLE_HID_NAME_PENDING && strcmp(snapshot.requested, "Desk") == 0);
    assert(!snapshot.active_known && name_set_calls == 0 && name_get_calls == 0);
    uint64_t unchanged = 88;
    assert(ble_hid_name_request("Other", 5, &unchanged) == ESP_ERR_INVALID_STATE && unchanged == 88);
    assert(ble_hid_name_snapshot().request_id == id);
    callback_active = true;
    for (unsigned i = 0; i < 1000; ++i) assert(ble_hid_name_snapshot().request_id == id);
    callback_active = false;
    assert(name_set_calls == 0 && name_get_calls == 0);
}

static void test_name_preinit_and_startup_callback_ownership(void) {
    reset_startup();
    uint64_t id;
    assert(ble_hid_name_request("Desk Input", 10, &id) == ESP_OK);
    assert(ble_hid_init() == ESP_OK && strcmp(fake_sdk_name, "Desk Input") == 0);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_PENDING && advertising_calls == 0);
    esp_ble_gatts_cb_param_t gatt = {0};
    gatt.reg.app_id = HID_APP_ID; gatt.reg.status = ESP_GATT_OK;
    gatts_callback(ESP_GATTS_REG_EVT, 7, &gatt); worker_cycle();
    uint16_t handles[ATTR_COUNT];
    for (unsigned i = 0; i < ATTR_COUNT; ++i) handles[i] = (uint16_t)(20U + i);
    memset(&gatt, 0, sizeof(gatt));
    gatt.add_attr_tab.status = ESP_GATT_OK; gatt.add_attr_tab.num_handle = ATTR_COUNT;
    gatt.add_attr_tab.handles = handles;
    gatts_callback(ESP_GATTS_CREAT_ATTR_TAB_EVT, 7, &gatt); worker_cycle();
    memset(&gatt, 0, sizeof(gatt)); gatt.start.status = ESP_GATT_OK;
    gatt.start.service_handle = handles[ATTR_SERVICE];
    gatts_callback(ESP_GATTS_START_EVT, 7, &gatt);
    name_payload_complete(false, ESP_BT_STATUS_SUCCESS); worker_cycle();
    assert(name_get_calls == 0 && advertising_calls == 0);
    name_payload_complete(true, ESP_BT_STATUS_SUCCESS); worker_cycle();
    assert(name_get_calls == 1 && advertising_calls == 0);
    name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name); worker_cycle();
    assert(device_name.ack == NAME_ACK_START && ble_hid_name_snapshot().status == BLE_HID_NAME_APPLYING);
    name_start_complete(ESP_BT_STATUS_SUCCESS);
    ble_hid_name_snapshot_t snapshot = ble_hid_name_snapshot();
    assert(snapshot.request_id == id && snapshot.status == BLE_HID_NAME_APPLIED && snapshot.active_known);
    assert(strcmp(snapshot.active, "Desk Input") == 0 && snapshot.error == ESP_OK);
    assert(name_set_calls == 1 && adv_config_calls == 2 && commit_calls == 0 && close_calls == 0);
    assert(strcmp(configured_adv_name, "Desk Input") == 0 && strcmp(configured_scan_name, "Desk Input") == 0);
}

static void test_name_connected_apply_preserves_hid_and_bonds(void) {
    start_bonded(); subscribe();
    uint64_t generation = state.public.generation;
    unsigned commits = commit_calls;
    uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH];
    memcpy(fingerprint, state.bond_fingerprint, sizeof(fingerprint));
    assert(ble_hid_name_request("Desk Mouse", 10, NULL) == ESP_OK);
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    assert(state.public.ready && state.public.generation == generation && !state.public.failed);
    uint8_t report[8] = {0};
    assert(ble_hid_send(1, report, sizeof(report))); complete_notification(ESP_GATT_OK);
    name_finish_payloads();
    ble_hid_name_snapshot_t snapshot = ble_hid_name_snapshot();
    assert(snapshot.status == BLE_HID_NAME_APPLIED && snapshot.active_known);
    assert(strcmp(snapshot.active, "Desk Mouse") == 0 && strcmp(fake_sdk_name, "Desk Mouse") == 0);
    assert(strcmp(configured_adv_name, "Desk Mouse") == 0 && strcmp(configured_scan_name, "Desk Mouse") == 0);
    assert(state.public.ready && state.public.generation == generation && close_calls == 0 && adv_stop_calls == 0);
    assert(commit_calls == commits && fake_bond_count == 1 && subscriptions.count == 1);
    assert(memcmp(fingerprint, state.bond_fingerprint, sizeof(fingerprint)) == 0);
    unsigned sets = name_set_calls, gets = name_get_calls, configs = adv_config_calls;
    uint64_t old_id = snapshot.request_id, new_id;
    assert(ble_hid_name_request("Desk Mouse", 10, &new_id) == ESP_OK && new_id != old_id);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_APPLIED);
    worker_cycle(); assert(sets == name_set_calls && gets == name_get_calls && configs == adv_config_calls);
}

static void test_name_utf8_full_advertisement_and_rollback(void) {
    reset();
    const char utf8[] = "\xe6\x88\x91\xe6\x88\x91\xe6\x88\x91\xe6\x88\x91\xe6\x88\x91\xe6\x88\x91";
    assert(sizeof(utf8) - 1U == BLE_HID_NAME_MAX_BYTES);
    name_begin_runtime(utf8, true);
    name_finish_payloads();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_APPLIED && state.advertising);
    assert(strcmp(configured_adv_name, utf8) == 0 && strcmp(configured_scan_name, utf8) == 0);
    /* Independent AD budget: flags 3, appearance 4, HID UUID 4, name header 2. */
    assert(3U + 4U + 4U + 2U + strlen(configured_adv_name) == 31U);
    assert(close_calls == 0 && remove_calls == 0 && commit_calls == 0);

    name_begin_runtime("Other Input", true);
    worker_cycle(); name_payload_complete(false, 7);
    assert(device_name.rollback && !state.public.failed && !state.advertising);
    assert(ble_hid_name_snapshot().rollback_attempted && !ble_hid_name_snapshot().rollback_confirmed);
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    name_finish_payloads();
    ble_hid_name_snapshot_t snapshot = ble_hid_name_snapshot();
    assert(snapshot.status == BLE_HID_NAME_FAILED && snapshot.error == ESP_FAIL);
    assert(snapshot.sdk_status_known && snapshot.sdk_status == 7);
    assert(snapshot.rollback_confirmed && snapshot.rollback_error == ESP_OK && snapshot.active_known);
    assert(strcmp(snapshot.active, utf8) == 0 && strcmp(snapshot.requested, "Other Input") == 0);
    assert(strcmp(fake_sdk_name, utf8) == 0 && state.advertising && !state.public.failed);
    assert(strcmp(configured_adv_name, utf8) == 0 && strcmp(configured_scan_name, utf8) == 0);
    assert(close_calls == 0 && commit_calls == 0 && remove_calls == 0);
    uint64_t previous_id = snapshot.request_id;
    assert(ble_hid_name_request(utf8, sizeof(utf8) - 1U, NULL) == ESP_OK);
    snapshot = ble_hid_name_snapshot();
    assert(snapshot.status == BLE_HID_NAME_APPLIED && snapshot.error == ESP_OK);
    assert(snapshot.request_id != previous_id && !snapshot.rollback_attempted);
}

static void test_name_readback_and_rollback_failures(void) {
    reset();
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK);
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    drop_name_set = true;
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    assert(device_name.rollback && ble_hid_name_snapshot().error == ESP_ERR_INVALID_STATE);
    drop_name_set = false;
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    name_finish_payloads();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_FAILED && ble_hid_name_snapshot().rollback_confirmed);

    start_bonded(); subscribe();
    uint64_t generation = state.public.generation;
    name_begin_runtime("Changed", false);
    worker_cycle(); name_payload_complete(false, 9);
    name_set_result = ESP_ERR_NO_MEM;
    worker_cycle();
    ble_hid_name_snapshot_t snapshot = ble_hid_name_snapshot();
    assert(snapshot.status == BLE_HID_NAME_UNCERTAIN && snapshot.error == ESP_FAIL);
    assert(snapshot.rollback_error == ESP_ERR_NO_MEM && !snapshot.rollback_confirmed && !snapshot.active_known);
    assert(ble_hid_name_request("Again", 5, NULL) == ESP_ERR_INVALID_STATE);
    assert(state.public.ready && state.public.generation == generation && close_calls == 0 && !state.public.failed);
}

static void test_name_timeout_and_late_callbacks(void) {
    reset(); assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK);
    worker_cycle(); assert(name_reads_pending == 1);
    fake_tick = BLE_HID_NAME_TIMEOUT_MS;
    notification_watchdog(NULL);
    uint64_t id = ble_hid_name_snapshot().request_id;
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN);
    assert(ble_hid_name_snapshot().error == ESP_ERR_TIMEOUT);
    assert(ble_hid_name_request("Again", 5, NULL) == ESP_ERR_INVALID_STATE);
    name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name); worker_cycle();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN && ble_hid_name_snapshot().request_id == id);
    assert(name_set_calls == 0 && advertising_calls == 0 && !state.public.failed);

    start_bonded(); subscribe(); name_begin_runtime("Changed", false);
    worker_cycle(); assert(device_name.ack == NAME_ACK_ADV);
    fake_tick = BLE_HID_NAME_TIMEOUT_MS; notification_watchdog(NULL);
    name_payload_complete(false, 7); worker_cycle();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN && !state.public.failed);
    assert(state.public.ready && close_calls == 0 && advertising_calls == 0);
    assert(ble_hid_name_request("Again", 5, NULL) == ESP_ERR_INVALID_STATE);
}

static void test_name_connect_during_stop_and_restart(void) {
    reset(); state.advertising = true;
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK); worker_cycle();
    connect_peer(); drain();
    advertising_stopped(7); /* Connection proves legacy advertising already stopped. */
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
    name_finish_payloads();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_APPLIED && state.public.connected);
    assert(!state.public.failed && close_calls == 0);

    reset(); name_begin_runtime("Changed", false);
    worker_cycle(); name_payload_complete(false, ESP_BT_STATUS_SUCCESS);
    worker_cycle(); name_payload_complete(true, ESP_BT_STATUS_SUCCESS);
    worker_cycle(); assert(device_name.ack == NAME_ACK_START);
    connect_peer(); drain(); disconnect_peer();
    name_start_complete(ESP_BT_STATUS_SUCCESS);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_APPLIED && !state.advertising);
    assert(close_calls == 0 && !state.public.failed);
    worker_cycle(); assert(state.adv_pending); /* A fresh normal advertisement is needed. */
}

static void test_name_submission_and_restart_failures(void) {
    reset(); state.advertising = true; adv_stop_result = ESP_FAIL;
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK); worker_cycle();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_FAILED && state.advertising);
    assert(!state.public.failed && name_set_calls == 0 && name_get_calls == 0 && close_calls == 0);

    reset(); state.advertising = true;
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK); worker_cycle();
    advertising_stopped(7); worker_cycle();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_FAILED && state.advertising);
    assert(!state.adv_stop_pending && !state.public.failed && name_set_calls == 0);

    reset(); name_get_result = ESP_ERR_NO_MEM;
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK); worker_cycle();
    assert(name_reads_pending == 0 && device_name.ack == NAME_ACK_START);
    name_start_complete(ESP_BT_STATUS_SUCCESS);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_FAILED && name_set_calls == 0);
    assert(ble_hid_name_snapshot().error == ESP_ERR_NO_MEM && !state.public.failed);

    for (unsigned failure = 0; failure < 4; ++failure) {
        reset(); name_begin_runtime("Changed", false);
        if (failure == 0) {
            adv_data_result = ESP_ERR_NO_MEM;
            worker_cycle();
            adv_data_result = ESP_OK;
        } else {
            worker_cycle(); name_payload_complete(false, ESP_BT_STATUS_SUCCESS);
            if (failure == 1) {
                scan_data_result = ESP_ERR_NO_MEM; worker_cycle(); scan_data_result = ESP_OK;
            } else {
                worker_cycle();
                if (failure == 2) name_payload_complete(true, 7);
                else {
                    name_payload_complete(true, ESP_BT_STATUS_SUCCESS);
                    adv_start_result = ESP_ERR_NO_MEM; worker_cycle(); adv_start_result = ESP_OK;
                }
            }
        }
        assert(device_name.rollback && ble_hid_name_snapshot().rollback_attempted);
        assert(!state.public.failed && !state.advertising && close_calls == 0);
        worker_cycle(); name_read_complete(ESP_BT_STATUS_SUCCESS, fake_sdk_name);
        name_finish_payloads();
        ble_hid_name_snapshot_t snapshot = ble_hid_name_snapshot();
        assert(snapshot.status == BLE_HID_NAME_FAILED && snapshot.rollback_confirmed && snapshot.active_known);
        assert(strcmp(snapshot.active, BLE_HID_DEFAULT_NAME) == 0 && state.advertising);
        assert(commit_calls == 0 && remove_calls == 0 && close_calls == 0);
    }
}

static void test_name_guards_and_start_timeout(void) {
    reset_startup();
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK);
    nvs_result = ESP_ERR_NO_MEM;
    assert(ble_hid_init() == ESP_ERR_NO_MEM);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_FAILED && name_set_calls == 0);

    reset(); state.init_attempted = true; state.public.initialized = false;
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_ERR_INVALID_STATE);
    reset(); assert(ble_hid_forget_bonds(state.public.generation));
    assert(ble_hid_name_request("Changed", 7, NULL) == ESP_ERR_INVALID_STATE);
    reset(); assert(ble_hid_name_request("Changed", 7, NULL) == ESP_OK);
    assert(!ble_hid_forget_bonds(state.public.generation));

    reset(); name_begin_runtime("Changed", false);
    worker_cycle(); name_payload_complete(false, ESP_BT_STATUS_SUCCESS);
    worker_cycle(); name_payload_complete(true, ESP_BT_STATUS_SUCCESS);
    worker_cycle(); assert(device_name.ack == NAME_ACK_START);
    fake_tick = BLE_HID_NAME_TIMEOUT_MS; notification_watchdog(NULL);
    name_start_complete(ESP_BT_STATUS_SUCCESS); worker_cycle();
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN && state.adv_stop_pending);
    advertising_stopped(ESP_BT_STATUS_SUCCESS); worker_cycle();
    assert(!state.advertising && !state.public.failed && close_calls == 0);
    assert(ble_hid_name_snapshot().status == BLE_HID_NAME_UNCERTAIN);
    assert(ble_hid_name_request("Again", 5, NULL) == ESP_ERR_INVALID_STATE);
}

static ble_hid_target_t target_key(unsigned int index) {
    ble_hid_target_t key = {.address_type = fake_bonds[index].bd_addr_type};
    memcpy(key.address, fake_bonds[index].bd_addr, 6); return key;
}
static void test_saved_target_selection_and_order(void) {
    reset();
    assert(psa_crypto_init() == PSA_SUCCESS);
    for (unsigned int i = 0; i < 3; ++i) {
        save_sdk_bond(i, (uint8_t)(0xc2U + i), (uint8_t)(i + 1));
        memcpy(fake_bonds[i].bond_key.pid_key.static_addr, fake_bonds[i].bd_addr, 6);
    }
    /* One offline private/pseudo key remains visible but not selectable. */
    fake_bonds[2].bd_addr_type = 2;
    assert(refresh_known_bonds() == ESP_OK);
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.bonds[0].selection_supported && !list.bonds[2].selection_supported);
    ble_hid_target_t order[] = {target_key(1), target_key(0), target_key(2)};
    ble_hid_target_t duplicate[] = {target_key(1), target_key(1), target_key(2)};
    assert(ble_hid_target_order(list.revision, duplicate, 3) == ESP_ERR_INVALID_ARG);
    assert(ble_hid_target_order(list.revision, order, 2) == ESP_ERR_INVALID_ARG);
    assert(ble_hid_target_order(list.revision + 1, order, 3) == ESP_ERR_INVALID_STATE);
    assert(ble_hid_target_order(list.revision, order, 3) == ESP_OK);
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_PENDING && target_commit_calls == 0);
    worker_cycle(); assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED);
    assert(ble_hid_bond_list().bonds[0].address[0] == 0xc3);
    assert(refresh_known_bonds() == ESP_OK && ble_hid_bond_list().bonds[0].address[0] == 0xc3);
    list = ble_hid_bond_list();
    assert(ble_hid_target_select(list.revision, &order[2]) == ESP_ERR_NOT_SUPPORTED);
    assert(ble_hid_target_select(list.revision, &order[0]) == ESP_OK);
    worker_cycle(); assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED);
    assert(!ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    assert(ble_hid_bond_list().bonds[0].is_selected);
    ble_hid_target_snapshot_t selected = ble_hid_target_snapshot();
    assert(selected.selected && selected.host_initiated_only && !selected.nearby_detection_supported);
    active_peer = 0xc2; connect_peer(); assert(state.rejected_pending && !state.public.connected); drain(); disconnect_peer();
    active_peer = 0xc3; connect_peer(); assert(state.public.connected && !state.rejected_pending); drain();
    assert(ble_hid_target_select(list.revision, NULL) == ESP_ERR_INVALID_STATE);
    disconnect_peer();
    /* A now-unresolvable selected key blocks all admission until explicit clear. */
    fake_bonds[1].bd_addr_type = 2;
    assert(refresh_known_bonds() == ESP_OK);
    active_peer = 0xc3; connect_peer(); assert(state.rejected_pending && !state.public.connected); drain(); disconnect_peer();
    assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK);
    worker_cycle();
    assert(!ble_hid_target_snapshot().selected && ble_hid_pairing_open(BLE_HID_PAIRING_DEFAULT_MS));
    assert(fake_bond_count == 3 && remove_calls == 0 && commit_calls == 0);
}
static void test_shared_classic_pairing_guard(void) {
    reset(); assert(!ble_hid_pairing_claim_classic(false));
    TickType_t started = state.pairing_started, duration = state.pairing_duration;
    assert(ble_hid_pairing_claim_classic(true) && state.public.pairing_open && !pairing_allowed_locked(fake_tick));
    ++fake_tick; ble_hid_pairing_release_classic();
    assert(state.pairing_started == started && state.pairing_duration == duration && pairing_allowed_locked(fake_tick));
    assert(ble_hid_pairing_claim_classic(true));
    fake_tick = started + duration; (void)ble_hid_snapshot();
    ble_hid_pairing_release_classic(); assert(!ble_hid_snapshot().pairing_open);
    start_bonded(); write_cccd(ATTR_KEYBOARD_CCCD, true); write_cccd(ATTR_MOUSE_CCCD, true);
    assert(ble_hid_pairing_claim_classic(true) && ble_hid_snapshot().ready);
    uint8_t report[8] = {0}; assert(ble_hid_send(1, report, sizeof(report)));
    ble_hid_pairing_release_classic();
    reset();
    ble_hid_pairing_close(); assert(ble_hid_pairing_claim_classic(false));
    assert(!ble_hid_pairing_open(1000) && !ble_hid_pairing_claim_classic(false));
    active_peer = 0x80; connect_peer(); assert(state.rejected_pending && !state.public.connected);
    drain(); disconnect_peer(); ble_hid_pairing_release_classic();
    assert(ble_hid_pairing_open(1000));
    connect_peer(); drain(); assert(state.pairing_admission_pending && !ble_hid_pairing_claim_classic(false) && !ble_hid_pairing_claim_classic(true));
#if CONFIG_BT_CLASSIC_ENABLED
    shared_bond_count = PAIRING_BOND_CAPACITY;
    service_pairing_admission();
    assert(!state.pairing_admitted && state.link_fault && encryption_calls == 0);
    reset(); ble_hid_pairing_close(); assert(ble_hid_pairing_open(1000)); connect_peer(); drain();
    shared_bond_count = -1; service_pairing_admission();
    assert(!state.pairing_admitted && state.link_fault && encryption_calls == 0);
#endif
}

static void prepare_durable_targets(void) {
    reset(); assert(psa_crypto_init() == PSA_SUCCESS);
    for (unsigned int i = 0; i < 2; ++i) {
        save_sdk_bond(i, (uint8_t)(0xc2U + i), (uint8_t)(i + 1));
        memcpy(fake_bonds[i].bond_key.pid_key.static_addr, fake_bonds[i].bd_addr, 6);
    }
    assert(refresh_known_bonds() == ESP_OK);
}
static ble_hid_target_t public_target(const ble_hid_bond_t *row) {
    ble_hid_target_t key = {.address_type = row->address_type, .bond_id_known = row->bond_id_known};
    memcpy(key.address, row->address, 6); memcpy(key.bond_id, row->bond_id, BLE_HID_BOND_ID_BYTES); return key;
}
static ble_hid_target_preferences_request_t target_request(void) {
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.count == 2 && list.bonds[0].bond_id_known && list.bonds[1].bond_id_known);
    return (ble_hid_target_preferences_request_t){.expected_bond_revision = list.revision,
        .expected_revision = ble_hid_target_snapshot().revision, .selected = true,
        .target = public_target(&list.bonds[1]), .count = 2,
        .order = {public_target(&list.bonds[1]), public_target(&list.bonds[0])}};
}
static void reboot_target_preferences(void) {
    uint8_t saved[sizeof(target_durable_blob)]; size_t length = target_durable_length;
    esp_ble_bond_dev_t bonds[SUBSCRIPTION_CAPACITY]; int count = fake_bond_count;
    memcpy(saved, target_durable_blob, length); memcpy(bonds, fake_bonds, sizeof(bonds));
    reset(); memcpy(target_durable_blob, saved, length); target_durable_length = length;
    memcpy(fake_bonds, bonds, sizeof(bonds)); fake_bond_count = count;
    state.target.loaded = false; state.bonds_loaded = false; state.bond_list.known = false;
    state.public.pairing_open = false; worker_cycle();
}
static void test_durable_target_reboot_replay_and_identity(void) {
    prepare_durable_targets();
    ble_hid_target_preferences_request_t request = target_request(), replay = request;
    uint64_t id = 0;
    assert(ble_hid_target_preferences_request(&request, &id) == ESP_OK && id == 1);
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_PENDING &&
        ble_hid_target_snapshot().revision == 0 && !ble_hid_target_snapshot().selected && target_commit_calls == 0);
    assert(!ble_hid_pairing_open(1000) && !ble_hid_forget_bonds(state.public.generation));
    assert(ble_hid_name_request("Blocked", 7, NULL) == ESP_ERR_INVALID_STATE);
    active_peer = 0xc2; connect_peer(); assert(state.rejected_pending && !state.public.connected); drain(); disconnect_peer();
    memset(&request, 0, sizeof(request)); /* Submitted value cannot retain borrowed storage. */
    worker_cycle();
    ble_hid_target_snapshot_t snapshot = ble_hid_target_snapshot();
    assert(snapshot.loaded && snapshot.durable && snapshot.selected && snapshot.available &&
        snapshot.revision == 1 && snapshot.request_id == id && snapshot.request_status == BLE_HID_TARGET_APPLIED);
    assert(target_commit_calls == 1 && ble_hid_bond_list().bonds[0].address[0] == 0xc3);
    assert(ble_hid_target_preferences_request(&replay, NULL) == ESP_ERR_INVALID_STATE && target_commit_calls == 1);
    assert(ble_hid_target_snapshot().request_id == id && ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED);
    reboot_target_preferences(); snapshot = ble_hid_target_snapshot();
    assert(snapshot.loaded && snapshot.durable && snapshot.revision == 1 && snapshot.selected && snapshot.available);
    assert(snapshot.request_status == BLE_HID_TARGET_IDLE && target_commit_calls == 0);
    assert(ble_hid_target_preferences_request(&replay, NULL) == ESP_ERR_INVALID_STATE && target_commit_calls == 0);
    assert(ble_hid_bond_list().bonds[0].address[0] == 0xc3 && ble_hid_bond_list().bonds[0].is_selected);
    active_peer = 0xc2; connect_peer(); assert(state.rejected_pending && !state.public.connected); drain(); disconnect_peer();
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true);
    assert(state.public.connected && state.bond_resolved && !state.link_fault); disconnect_peer();
    fake_bonds[1].bond_key.penc_key.ltk[0] ^= 1U;
    assert(refresh_known_bonds() == ESP_OK);
    snapshot = ble_hid_target_snapshot(); assert(snapshot.selected && !snapshot.available && snapshot.error == ESP_ERR_NOT_FOUND);
    connect_peer(); assert(state.rejected_pending && !state.public.connected); drain(); disconnect_peer();
    reboot_target_preferences(); assert(ble_hid_target_snapshot().selected && !ble_hid_target_snapshot().available);
    fake_bond_count = 1; assert(refresh_known_bonds() == ESP_OK);
    reboot_target_preferences(); snapshot = ble_hid_target_snapshot();
    assert(snapshot.selected && !snapshot.available && snapshot.error == ESP_ERR_NOT_FOUND && snapshot.revision == 1);
    assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_OK);
    worker_cycle(); assert(ble_hid_target_snapshot().revision == 2 && !ble_hid_target_snapshot().selected);
    reboot_target_preferences(); snapshot = ble_hid_target_snapshot();
    assert(snapshot.loaded && snapshot.durable && snapshot.available && !snapshot.selected && snapshot.revision == 2);
    assert(fake_bond_count == 1 && remove_calls == 0);
}
static void test_durable_target_storage_failures(void) {
    for (unsigned int failure = 0; failure < 5; ++failure) {
        prepare_durable_targets(); ble_hid_target_preferences_request_t request = target_request();
        assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK);
        if (failure == 0) target_open_error = ESP_FAIL;
        if (failure == 1) target_write_error = ESP_FAIL;
        if (failure == 2) target_commit_error = ESP_FAIL;
        if (failure == 3) target_read_error = ESP_FAIL;
        if (failure == 4) target_corrupt_readback = true;
        worker_cycle(); ble_hid_target_snapshot_t snapshot = ble_hid_target_snapshot();
        assert(snapshot.revision == 0 && !snapshot.selected && !snapshot.durable && snapshot.request_error != ESP_OK);
        assert(snapshot.request_status == (failure == 0 ? BLE_HID_TARGET_FAILED : BLE_HID_TARGET_UNCERTAIN));
        if (failure) {
            assert(!snapshot.available && !ble_hid_pairing_open(1000));
            assert(ble_hid_target_select(ble_hid_bond_list().revision, NULL) == ESP_ERR_INVALID_STATE);
            active_peer = 0xc2; connect_peer(); assert(state.rejected_pending && !state.public.connected); drain(); disconnect_peer();
        }
        reboot_target_preferences(); snapshot = ble_hid_target_snapshot();
        assert(snapshot.loaded && snapshot.available && !state.public.failed);
        assert(snapshot.selected == (failure >= 3)); /* Actual committed record wins after reboot. */
    }
    prepare_durable_targets(); ble_hid_target_preferences_request_t request = target_request();
    assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK);
    target_commit_error = ESP_FAIL; target_commit_persists_on_error = true; worker_cycle();
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_UNCERTAIN && target_durable_length);
    reboot_target_preferences(); assert(ble_hid_target_snapshot().selected && ble_hid_target_snapshot().revision == 1);
    size_t retained = target_durable_length; target_durable_blob[20] ^= 1U;
    reboot_target_preferences();
    assert(state.public.failed && !ble_hid_target_snapshot().loaded && target_durable_length == retained && target_commit_calls == 0);
    active_peer = 0xc3; connect_peer(); assert(state.rejected_pending && !state.public.connected);
}
static void peer_during_target_commit(void) {
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLYING);
    callback_active = true; active_peer = 0xc3; connect_peer(); callback_active = false;
    assert(state.rejected_pending && !state.public.connected && !state.public.ready);
}
static void test_durable_target_commit_race_and_old_record(void) {
    prepare_durable_targets(); ble_hid_target_preferences_request_t request = target_request();
    assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK);
    target_commit_hook = peer_during_target_commit; worker_cycle(); target_commit_hook = NULL;
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED && !state.public.connected);
    disconnect_peer(); drain();
    for (unsigned int persisted = 0; persisted < 2; ++persisted) {
        prepare_durable_targets(); request = target_request();
        assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK); worker_cycle();
        ble_hid_bond_list_t list = ble_hid_bond_list();
        request = (ble_hid_target_preferences_request_t){.expected_bond_revision = list.revision,
            .expected_revision = 1, .selected = true, .target = public_target(&list.bonds[1]),
            .count = 2, .order = {public_target(&list.bonds[0]), public_target(&list.bonds[1])}};
        assert(request.target.address[0] == 0xc2);
        assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK);
        target_commit_error = ESP_FAIL; target_commit_persists_on_error = persisted != 0; worker_cycle();
        ble_hid_target_snapshot_t snapshot = ble_hid_target_snapshot();
        assert(snapshot.request_status == BLE_HID_TARGET_UNCERTAIN && snapshot.selected &&
            snapshot.target.address[0] == 0xc3 && snapshot.revision == 1 && !snapshot.available);
        reboot_target_preferences(); snapshot = ble_hid_target_snapshot();
        assert(snapshot.loaded && snapshot.durable && snapshot.available &&
            snapshot.target.address[0] == (persisted ? 0xc2 : 0xc3) && snapshot.revision == (persisted ? 2U : 1U));
    }
}

static void assert_target_submission_scratch_empty(void) {
    const uint8_t *bytes = (const uint8_t *)&state.target_request_scratch;
    for (size_t i = 0; i < sizeof(state.target_request_scratch); ++i) assert(bytes[i] == 0);
}

static void reject_target_submissions_while_worker_owns_candidate(void) {
    assert(state.target.request_status == BLE_HID_TARGET_PENDING ||
        state.target.request_status == BLE_HID_TARGET_APPLYING);
    unsigned char pending[sizeof(state.target_candidate)];
    memcpy(pending, &state.target_candidate, sizeof(pending));
    ble_hid_target_preferences_request_t request = target_request();
    uint64_t id = UINT64_MAX, active_id = state.target.request_id;
    assert(ble_hid_target_select(state.bond_list.revision, NULL) == ESP_ERR_INVALID_STATE);
    assert_target_submission_scratch_empty();
    assert(ble_hid_target_order(state.bond_list.revision, request.order, request.count) == ESP_ERR_INVALID_STATE);
    assert_target_submission_scratch_empty();
    assert(ble_hid_target_preferences_request(&request, &id) == ESP_ERR_INVALID_STATE && id == UINT64_MAX);
    assert(state.target.request_id == active_id && !memcmp(pending, &state.target_candidate, sizeof(pending)));
}

static void test_target_submission_workspace_ownership(void) {
    prepare_durable_targets();
    unsigned char previous[sizeof(state.target_candidate)];
    memcpy(previous, &state.target_candidate, sizeof(previous));
    ble_hid_target_preferences_request_t request = target_request(), invalid = request;
    uint64_t id = UINT64_MAX;
    invalid.order[1] = invalid.order[0]; /* Reject after one valid order entry. */
    assert(ble_hid_target_preferences_request(&invalid, &id) == ESP_ERR_INVALID_ARG && id == UINT64_MAX);
    assert(!memcmp(previous, &state.target_candidate, sizeof(previous)) && !state.target.request_id);
    invalid = request; invalid.target.address[0] ^= 0x40U; /* Reject after the complete order. */
    assert(ble_hid_target_preferences_request(&invalid, &id) == ESP_ERR_NOT_FOUND && id == UINT64_MAX);
    assert(!memcmp(previous, &state.target_candidate, sizeof(previous)) && !state.target.request_id);
    assert(ble_hid_target_preferences_request(&request, &id) == ESP_OK);
    ble_hid_target_t selected = request.target;
    memset(&request, 0, sizeof(request)); /* No caller storage survives submission. */
    reject_target_submissions_while_worker_owns_candidate();
    target_commit_hook = reject_target_submissions_while_worker_owns_candidate;
    worker_cycle(); target_commit_hook = NULL;
    assert(state.target.request_status == BLE_HID_TARGET_APPLIED && state.target.request_id == id &&
        state.target.selected && !memcmp(state.target.target.bond_id, selected.bond_id, sizeof(selected.bond_id)));
    /* A completed candidate can be borrowed as input to the wrappers. They
     * must capture it before the next candidate is cleared and published. */
    assert(ble_hid_target_select(state.bond_list.revision, &state.target_candidate.target) == ESP_OK);
    assert_target_submission_scratch_empty();
    worker_cycle();
    assert(state.target.request_status == BLE_HID_TARGET_APPLIED && state.target.selected &&
        !memcmp(state.target.target.bond_id, selected.bond_id, sizeof(selected.bond_id)));
    assert(ble_hid_target_order(state.bond_list.revision, state.target_candidate.order,
        state.target_candidate.count) == ESP_OK);
    assert_target_submission_scratch_empty();
    worker_cycle();
    assert(state.target.request_status == BLE_HID_TARGET_APPLIED && state.target.selected &&
        !memcmp(state.target.target.bond_id, selected.bond_id, sizeof(selected.bond_id)) && target_commit_calls == 3);
}

static void test_durable_target_stale_request_and_schema(void) {
    prepare_durable_targets(); ble_hid_target_preferences_request_t request = target_request();
    request.order[0].bond_id[0] ^= 1U;
    assert(ble_hid_target_preferences_request(&request, NULL) == ESP_ERR_INVALID_ARG && target_commit_calls == 0);
    request = target_request(); assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK);
    assert(refresh_known_bonds() == ESP_OK); worker_cycle();
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED && target_commit_calls == 1);
    request = target_request(); assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK);
    fake_bonds[0].bond_key.penc_key.ltk[0] ^= 1U; authentication_event(true, true); worker_cycle();
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_FAILED && target_commit_calls == 1);
    request = target_request(); assert(ble_hid_target_preferences_request(&request, NULL) == ESP_OK); worker_cycle();
    target_durable_blob[3] = 2; size_t retained = target_durable_length;
    reboot_target_preferences();
    assert(state.public.failed && ble_hid_target_snapshot().request_error == ESP_ERR_NOT_SUPPORTED && target_durable_length == retained);
    prepare_durable_targets();
    fake_bonds[0].bond_key.key_mask &= (uint8_t)~ESP_BLE_ENC_KEY_MASK;
    assert(refresh_known_bonds() == ESP_OK);
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(!list.bonds[0].bond_id_known && !list.bonds[0].selection_supported && list.bonds[1].selection_supported);
    ble_hid_target_t modern = public_target(&list.bonds[1]);
    assert(ble_hid_target_select(list.revision, &modern) == ESP_OK); worker_cycle();
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED && ble_hid_bond_list().count == 2);
    reboot_target_preferences();
    assert(ble_hid_target_snapshot().selected && ble_hid_target_snapshot().available && ble_hid_bond_list().count == 2);
}

static ble_hid_target_t single_target(void) {
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.known && list.count == 2 && list.bonds[1].bond_id_known);
    return public_target(&list.bonds[1]);
}
static uint64_t request_single(const ble_hid_target_t *target) {
    uint64_t id = 0;
    assert(ble_hid_forget_bond_request(state.public.generation, ble_hid_bond_list().revision, target, &id) == ESP_OK);
    assert(id && ble_hid_bond_forget_snapshot().request_id == id && ble_hid_bond_forget_snapshot().cancellable);
    return id;
}
static void test_single_bond_forget_success_and_preservation(void) {
    prepare_durable_targets();
    for (unsigned i = 0; i < 2; ++i) { active_peer = (uint8_t)(0xc2U + i); connect_peer(); drain(); authorize_peer(true); subscribe(); disconnect_peer(); }
    assert(subscriptions.count == 2);
    ble_hid_target_t target = single_target();
    assert(ble_hid_target_select(ble_hid_bond_list().revision, &target) == ESP_OK); worker_cycle();
    uint64_t preference_revision = ble_hid_target_snapshot().revision;
    assert(ble_hid_bond_list().preferences_revision == preference_revision);
    esp_ble_gap_cb_param_t started = {.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS};
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &started);
    uint64_t id = request_single(&target);
    assert(!ble_hid_forget_bonds(state.public.generation) && !ble_hid_pairing_open(1000));
    worker_cycle(); assert(state.adv_stop_pending && !remove_calls);
    advertising_stopped(ESP_BT_STATUS_SUCCESS); worker_cycle();
    assert(remove_calls == 1 && ble_hid_bond_forget_snapshot().sdk_inflight && !memcmp(last_remove_address, target.address, 6));
    assert(ble_hid_forget_bond_cancel(id) == ESP_ERR_NOT_SUPPORTED);
    esp_bd_addr_t unrelated = {0xff}; remove_complete(unrelated, ESP_BT_STATUS_SUCCESS, false); worker_cycle();
    assert(ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_REMOVING && remove_calls == 1);
    remove_complete(target.address, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
    ble_hid_bond_forget_snapshot_t job = ble_hid_bond_forget_snapshot();
    assert(job.request_id == id && job.status == BLE_HID_BOND_FORGET_SUCCEEDED && !job.sdk_inflight && !job.cancellable);
    ble_hid_bond_list_t list = ble_hid_bond_list();
    assert(list.known && !list.updating && list.count == 1 && list.bonds[0].address[0] == 0xc2 && fake_bond_count == 1);
    assert(subscriptions.count == 1 && subscription_mask(list.bonds[0].bond_id) == 3 && durable_blob[3] == 1);
    assert(remove_calls == 1 && !ble_hid_snapshot().pairing_open && ble_hid_target_snapshot().selected &&
        !ble_hid_target_snapshot().available && ble_hid_target_snapshot().revision == preference_revision);
}
static void test_single_bond_forget_cancel_and_stale(void) {
    prepare_durable_targets(); ble_hid_target_t target = single_target(); uint64_t id;
    assert(ble_hid_forget_bond_request(state.public.generation + 1U, ble_hid_bond_list().revision, &target, NULL) == ESP_ERR_INVALID_STATE);
    assert(ble_hid_forget_bond_request(state.public.generation, ble_hid_bond_list().revision + 1U, &target, NULL) == ESP_ERR_INVALID_STATE);
    ble_hid_target_t wrong = target; wrong.bond_id[0] ^= 1U;
    assert(ble_hid_forget_bond_request(state.public.generation, ble_hid_bond_list().revision, &wrong, NULL) == ESP_ERR_NOT_FOUND);
    wrong = target; wrong.bond_id_known = false;
    assert(ble_hid_forget_bond_request(state.public.generation, ble_hid_bond_list().revision, &wrong, NULL) == ESP_ERR_NOT_SUPPORTED);
    active_peer = 0xc2; connect_peer(); drain();
    assert(ble_hid_forget_bond_request(state.public.generation, ble_hid_bond_list().revision, &target, NULL) == ESP_ERR_INVALID_STATE); disconnect_peer();
    id = request_single(&target); assert(ble_hid_forget_bond_cancel(id + 1U) == ESP_ERR_INVALID_STATE);
    assert(ble_hid_forget_bond_cancel(id) == ESP_OK && ble_hid_forget_bond_cancel(id) == ESP_OK);
    worker_cycle(); assert(!remove_calls && fake_bond_count == 2 && ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_CANCELLED);
    uint64_t second = request_single(&target); assert(second > id && ble_hid_forget_bond_cancel(id) == ESP_ERR_INVALID_STATE);
    fake_bonds[1].bond_key.penc_key.ltk[0] ^= 1U; authentication_event(true, true); worker_cycle();
    assert(!remove_calls && ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_FAILED && fake_bond_count == 2);
}
static void test_single_bond_forget_failures_and_late_events(void) {
    for (unsigned failure = 0; failure < 5; ++failure) {
        prepare_durable_targets(); ble_hid_target_t target = single_target(); uint64_t id = request_single(&target);
        if (failure == 0) bond_result = ESP_FAIL;
        if (failure == 1) remove_request_result = ESP_FAIL;
        worker_cycle();
        if (failure <= 1) {
            assert(ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_FAILED && fake_bond_count == 2);
            if (!failure) assert(!remove_calls && !ble_hid_bond_list().known);
            else assert(remove_calls == 1 && !ble_hid_bond_forget_snapshot().sdk_inflight);
            continue;
        }
        assert(ble_hid_bond_forget_snapshot().sdk_inflight);
        if (failure == 2) remove_complete(target.address, 1, false);
        if (failure == 3) remove_complete(target.address, ESP_BT_STATUS_SUCCESS, false);
        if (failure == 4) { fake_tick += FORGET_TIMEOUT_MS; notification_watchdog(NULL); }
        worker_cycle(); ble_hid_bond_forget_snapshot_t job = ble_hid_bond_forget_snapshot();
        assert(job.request_id == id && job.status == (failure == 2 ? BLE_HID_BOND_FORGET_FAILED : BLE_HID_BOND_FORGET_UNCERTAIN));
        assert(remove_calls == 1 && fake_bond_count == 2);
        if (failure == 4) {
            assert(job.sdk_inflight && state.public.failed);
            remove_complete(target.address, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
            assert(ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_UNCERTAIN && !ble_hid_bond_forget_snapshot().sdk_inflight);
        }
    }
}
static void test_single_bond_forget_private_and_partial_cleanup(void) {
    prepare_durable_targets(); fake_bonds[1].bd_addr_type = 2; assert(refresh_known_bonds() == ESP_OK);
    ble_hid_target_t target = single_target(); assert(!ble_hid_bond_list().bonds[1].selection_supported);
    request_single(&target); worker_cycle(); remove_complete(target.address, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
    assert(ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_SUCCEEDED && fake_bond_count == 1);
    prepare_durable_targets();
    active_peer = 0xc3; connect_peer(); drain(); authorize_peer(true); subscribe(); disconnect_peer();
    target = single_target(); request_single(&target); worker_cycle();
    remove_complete(target.address, ESP_BT_STATUS_SUCCESS, true); commit_result = ESP_FAIL; worker_cycle();
    assert(ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_UNCERTAIN && state.public.failed && fake_bond_count == 1);
    prepare_durable_targets(); target = single_target(); request_single(&target); worker_cycle();
    remove_complete(target.address, ESP_BT_STATUS_SUCCESS, true); fake_bond_count = 0; worker_cycle();
    assert(ble_hid_bond_forget_snapshot().status == BLE_HID_BOND_FORGET_UNCERTAIN); /* Unrelated deletion cannot be hidden. */
}

static void test_quiet_forget_all_and_empty_durable_preferences(void) {
    prepare_durable_targets(); ble_hid_target_t target = single_target();
    assert(ble_hid_target_select(ble_hid_bond_list().revision, &target) == ESP_OK); worker_cycle();
    esp_ble_gap_cb_param_t started = {.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS};
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &started);
    assert(ble_hid_forget_bonds_without_pairing(state.public.generation));
    assert(!state.public.pairing_open); worker_cycle();
    assert(state.adv_stop_pending); advertising_stopped(ESP_BT_STATUS_SUCCESS); worker_cycle();
    for (unsigned i = 0; i < BLE_HID_MAX_BONDS + 1U && state.public.bond_forget_state == BLE_HID_FORGET_RUNNING; ++i) {
        assert(state.remove_inflight && !state.public.pairing_open);
        esp_bd_addr_t address; memcpy(address, last_remove_address, sizeof(address));
        remove_complete(address, ESP_BT_STATUS_SUCCESS, true); worker_cycle();
        assert(!state.public.pairing_open);
    }
    assert(state.public.bond_forget_state == BLE_HID_FORGET_DONE && !fake_bond_count && remove_calls == 2);
    ble_hid_target_preferences_request_t empty = {.expected_bond_revision = ble_hid_bond_list().revision,
        .expected_revision = ble_hid_target_snapshot().revision};
    assert(ble_hid_target_preferences_request(&empty, NULL) == ESP_OK); worker_cycle();
    assert(ble_hid_target_snapshot().request_status == BLE_HID_TARGET_APPLIED && ble_hid_target_snapshot().durable &&
        !ble_hid_target_snapshot().selected && !state.target_order_count && !state.public.pairing_open);
    reboot_target_preferences();
    assert(ble_hid_target_snapshot().loaded && ble_hid_target_snapshot().durable && !ble_hid_target_snapshot().selected &&
        !state.target_order_count && !state.public.pairing_open);
    assert(ble_hid_pairing_open(1000)); /* A later explicit local action still works. */
}

#include "native_management_cases.inc"
#include "advertising_diagnostics_cases.inc"
#include "physical_disconnect_cases.inc"
#include "admission_diagnostics_cases.inc"
#include "manual_disconnect_cases.inc"
#include "management_edges_cases.inc"
#include "current_selection_cases.inc"

int main(void) {
    if (getenv("BLE_CURRENT_REVIEW")) { current_selection_review_cases(); return 0; }
    current_selection_review_cases();
    if (getenv("BLE_CURRENT_SELECTION")) { current_selection_cases(); return 0; }
    current_selection_cases();
    if (getenv("BLE_MANAGEMENT_EDGES")) { management_edges_cases(); return 0; }
    management_edges_cases();
    if (getenv("BLE_MANUAL_CASE")) { manual_disconnect_cases(); return 0; }
    manual_disconnect_cases();
    test_admission_saved_denial_and_explicit_add();
    test_admission_worker_predicates_and_stale_commands();
    test_admission_anchors_overflow_freeze_and_copy();
    test_admission_failure_before_api_return_and_unknown_origin();
    test_admission_observation_neutrality();
    physical_disconnect_reservation_auth_and_replacement();
    physical_disconnect_add_waits_for_acl();
    physical_disconnect_missing_error_and_identity();
    physical_disconnect_stale_duplicate_and_rejected_peers();
    physical_disconnect_reused_ids_and_obsolete_reject();
    test_advertising_diagnostics_first_fault_and_copy();
    test_advertising_diagnostics_completion_before_api_return();
    test_advertising_diagnostics_bounds_missing_and_benign_stop();
    test_advertising_diagnostics_name_fault_classification();
    test_advertising_diagnostics_name_benign_and_late_drain();
    native_management_selected_add_and_reconnect();
    native_management_all_and_single_removal();
    native_management_generation_and_replacement();
    native_management_cancellation_failure_and_stale();
    test_target_submission_workspace_ownership();
    test_quiet_forget_all_and_empty_durable_preferences();
    test_single_bond_forget_success_and_preservation(); test_single_bond_forget_cancel_and_stale();
    test_single_bond_forget_failures_and_late_events(); test_single_bond_forget_private_and_partial_cleanup();
    test_durable_target_reboot_replay_and_identity(); test_durable_target_storage_failures();
    test_durable_target_stale_request_and_schema(); test_durable_target_commit_race_and_old_record();
    test_saved_target_selection_and_order(); test_shared_classic_pairing_guard();
    test_name_validation_submission_and_copy(); test_name_preinit_and_startup_callback_ownership();
    test_name_connected_apply_preserves_hid_and_bonds(); test_name_utf8_full_advertisement_and_rollback();
    test_name_readback_and_rollback_failures(); test_name_timeout_and_late_callbacks();
    test_name_connect_during_stop_and_restart();
    test_name_submission_and_restart_failures(); test_name_guards_and_start_timeout();
    test_first_failure_initialization(); test_first_failure_asynchronous_setup();
    test_first_failure_recovery_and_observation(); test_first_failure_authentication_and_deadline();
    test_first_failure_ignores_nonfaults_and_stale_jobs();
    test_snapshot_peer_identity();
    test_gate_and_epoch(); test_malformed_writes(); test_send_race_and_backpressure();
    test_fail_closed(); test_advertising_prerequisites();
    test_advertising_initialization_contract();
    test_notification_timeout(); test_coordinator_disconnect();
    test_control_worker_and_reads();
    test_repeated_lifecycle();
    test_bonded_reconnect_and_reboot(); test_peer_isolation_and_repair();
    test_pending_bond_and_deadline(); test_subscription_races();
    test_storage_and_crypto_failures(); test_subscription_capacity();
    test_no_response_disable_overflow();
    test_stale_failure_cannot_fault_new_peer();
    test_secure_connections_and_crypto_initialization();
    test_old_authentication_jobs_cannot_fault_replacement();
    test_pairing_window_lifecycle(); test_saved_reconnect_outside_pairing();
    test_stale_rejection_and_reject_deadline();
    test_rssi_actual_async_samples(); test_rssi_stale_callbacks_and_timeout();
    test_forget_guards_and_completion(); test_forget_multiple_and_capacity();
    test_forget_advertising_and_rejected_peer(); test_forget_failures_and_late_callbacks();
    test_multiple_saved_hosts_and_reconnect(); test_bond_list_private_unknown_and_types();
    test_pairing_capacity_preserves_existing_bonds();
    test_auth_disconnect_refreshes_durable_bonds(); test_auth_after_disconnect_and_refresh_failure();
    test_pairing_rechecks_actual_capacity(); test_auth_invalidates_an_inflight_cache_read();
    test_bond_list_cache_availability(); test_partial_forget_cached_list();
    puts("BLE adapter: advertising initialization, gates, missed edges, malformed writes, reconnect submission race, backpressure, persistence, pairing, RSSI, saved-host cache, bond removal, first-failure diagnostics, configurable name apply/rollback, failures and 100000 lifecycle cycles passed");
}
