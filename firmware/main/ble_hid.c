// SPDX-License-Identifier: Apache-2.0
#include "ble_hid.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "hid_engine.h"
#include "nvs_flash.h"
#include "ble_hid_store.h"

#define HID_APP_ID 0x1812
#define CONTROL_QUEUE_LENGTH 16
#define CONTROL_TASK_STACK 6144
#define CONTROL_TASK_PRIORITY 5
#define CONTROL_POLL_MS 20
#define NOTIFICATION_TIMEOUT_MS 1000
#define SUBSCRIPTION_TIMEOUT_MS 3000
#define PAIRING_HANDSHAKE_TIMEOUT_MS 30000U
#define RSSI_INTERVAL_MS 2000U
#define RSSI_TIMEOUT_MS 2000U
#define RSSI_MAX_AGE_MS 5000U
#define FORGET_TIMEOUT_MS 10000U
#define REJECT_TIMEOUT_MS 2000U
#define PAIRING_BOND_CAPACITY CONFIG_BT_SMP_MAX_BONDS
_Static_assert(PAIRING_BOND_CAPACITY > 0 && PAIRING_BOND_CAPACITY <= BLE_HID_MAX_BONDS,
               "Configured bond capacity must fit the bounded cache");

enum {
    ATTR_SERVICE,
    ATTR_INFO_DECL, ATTR_INFO,
    ATTR_MAP_DECL, ATTR_MAP,
    ATTR_CONTROL_DECL, ATTR_CONTROL,
    ATTR_KEYBOARD_DECL, ATTR_KEYBOARD, ATTR_KEYBOARD_CCCD, ATTR_KEYBOARD_REF,
    ATTR_LED_DECL, ATTR_LED, ATTR_LED_REF,
    ATTR_MOUSE_DECL, ATTR_MOUSE, ATTR_MOUSE_CCCD, ATTR_MOUSE_REF,
    ATTR_COUNT
};

static const char *const TAG = "ble_hid";
static const uint16_t UUID_PRIMARY = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t UUID_DECLARE = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t UUID_CCCD = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint16_t UUID_HID = ESP_GATT_UUID_HID_SVC;
static const uint16_t UUID_INFO = ESP_GATT_UUID_HID_INFORMATION;
static const uint16_t UUID_MAP = ESP_GATT_UUID_HID_REPORT_MAP;
static const uint16_t UUID_CONTROL = ESP_GATT_UUID_HID_CONTROL_POINT;
static const uint16_t UUID_REPORT = ESP_GATT_UUID_HID_REPORT;
static const uint16_t UUID_REFERENCE = ESP_GATT_UUID_RPT_REF_DESCR;
static const uint8_t PROP_READ = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t PROP_INPUT = ESP_GATT_CHAR_PROP_BIT_READ |
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t PROP_OUTPUT = ESP_GATT_CHAR_PROP_BIT_READ |
    ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t PROP_CONTROL = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
/* HID 1.11, no country code, normally connectable, no remote wake claim. */
static const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x02};
static const uint8_t KEYBOARD_REF[] = {HID_ENGINE_KEYBOARD_REPORT_ID, 1};
static const uint8_t LED_REF[] = {HID_ENGINE_KEYBOARD_REPORT_ID, 2};
static const uint8_t MOUSE_REF[] = {HID_ENGINE_MOUSE_REPORT_ID, 1};
static uint8_t empty_report[HID_ENGINE_KEYBOARD_REPORT_LENGTH];

#define ATTRIBUTE(response, uuid, permissions, capacity, length, value) \
    {{response}, {ESP_UUID_LEN_16, (uint8_t *)&(uuid), permissions, \
                  capacity, length, (uint8_t *)(value)}}
#define DECLARATION(property) ATTRIBUTE(ESP_GATT_AUTO_RSP, UUID_DECLARE, \
    ESP_GATT_PERM_READ, 1, 1, &(property))
#define REFERENCE(value) ATTRIBUTE(ESP_GATT_AUTO_RSP, UUID_REFERENCE, \
    ESP_GATT_PERM_READ_ENCRYPTED, sizeof(value), sizeof(value), value)
#define CCCD ATTRIBUTE(ESP_GATT_RSP_BY_APP, UUID_CCCD, \
    ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED, 2, 2, empty_report)

/* App responses own all mutable values, so malformed/prepare writes cannot
 * change the database behind the adapter's gate. No Boot/Protocol Mode/Battery
 * characteristics are exposed. The portable engine owns the only Report Map. */
static esp_gatts_attr_db_t attributes[ATTR_COUNT] = {
    [ATTR_SERVICE] = ATTRIBUTE(ESP_GATT_AUTO_RSP, UUID_PRIMARY,
        ESP_GATT_PERM_READ, 2, 2, &UUID_HID),
    [ATTR_INFO_DECL] = DECLARATION(PROP_READ),
    [ATTR_INFO] = ATTRIBUTE(ESP_GATT_AUTO_RSP, UUID_INFO,
        ESP_GATT_PERM_READ_ENCRYPTED, sizeof(HID_INFO), sizeof(HID_INFO), HID_INFO),
    [ATTR_MAP_DECL] = DECLARATION(PROP_READ),
    /* Pointer and size are supplied at initialization. */
    [ATTR_MAP] = ATTRIBUTE(ESP_GATT_AUTO_RSP, UUID_MAP,
        ESP_GATT_PERM_READ_ENCRYPTED, 0, 0, NULL),
    [ATTR_CONTROL_DECL] = DECLARATION(PROP_CONTROL),
    [ATTR_CONTROL] = ATTRIBUTE(ESP_GATT_RSP_BY_APP, UUID_CONTROL,
        ESP_GATT_PERM_WRITE_ENCRYPTED, 1, 1, empty_report),
    [ATTR_KEYBOARD_DECL] = DECLARATION(PROP_INPUT),
    [ATTR_KEYBOARD] = ATTRIBUTE(ESP_GATT_RSP_BY_APP, UUID_REPORT,
        ESP_GATT_PERM_READ_ENCRYPTED, HID_ENGINE_KEYBOARD_REPORT_LENGTH,
        HID_ENGINE_KEYBOARD_REPORT_LENGTH, empty_report),
    [ATTR_KEYBOARD_CCCD] = CCCD,
    [ATTR_KEYBOARD_REF] = REFERENCE(KEYBOARD_REF),
    [ATTR_LED_DECL] = DECLARATION(PROP_OUTPUT),
    [ATTR_LED] = ATTRIBUTE(ESP_GATT_RSP_BY_APP, UUID_REPORT,
        ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
        HID_ENGINE_KEYBOARD_LED_LENGTH, HID_ENGINE_KEYBOARD_LED_LENGTH, empty_report),
    [ATTR_LED_REF] = REFERENCE(LED_REF),
    [ATTR_MOUSE_DECL] = DECLARATION(PROP_INPUT),
    [ATTR_MOUSE] = ATTRIBUTE(ESP_GATT_RSP_BY_APP, UUID_REPORT,
        ESP_GATT_PERM_READ_ENCRYPTED, HID_ENGINE_MOUSE_REPORT_LENGTH,
        HID_ENGINE_MOUSE_REPORT_LENGTH, empty_report),
    [ATTR_MOUSE_CCCD] = CCCD,
    [ATTR_MOUSE_REF] = REFERENCE(MOUSE_REF),
};

typedef enum {
    COMMAND_CREATE_SERVICE,
    COMMAND_START_SERVICE,
    COMMAND_ENCRYPT,
    COMMAND_SECURITY_REPLY,
    COMMAND_REJECT_CONNECTION,
    COMMAND_CCCD,
    COMMAND_RESPONSE
} command_kind_t;

typedef struct {
    command_kind_t kind;
    uint64_t generation;
    uint16_t conn_id;
    esp_bd_addr_t address;
    bool accept;
    bool reply;
    uint32_t auth_revision;
    uint32_t transaction;
    uint16_t handle;
    uint16_t offset;
    esp_gatt_status_t status;
    uint8_t length;
    uint8_t value[HID_ENGINE_KEYBOARD_REPORT_LENGTH];
} control_command_t;

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t control_queue;
static struct {
    ble_hid_snapshot_t public;
    bool init_attempted;
    bool service_started;
    bool adv_configured;
    bool scan_configured;
    bool adv_pending;
    bool adv_stop_pending;
    bool adv_stopped_by_connect;
    bool advertising;
    bool bonds_loaded;
    bool bond_refresh_pending;
    uint64_t bond_mutation_revision;
    ble_hid_bond_list_t bond_list;
    uint8_t peer_address_type;
    bool peer_address_type_known;
    bool pairing_user_controlled;
    TickType_t pairing_started;
    TickType_t pairing_duration;
    bool peer_saved;
    bool pairing_admitted;
    bool pairing_admission_pending;
    TickType_t handshake_started;
    bool rejected_pending;
    uint16_t rejected_conn_id;
    uint64_t rejected_generation;
    TickType_t rejected_started;
    esp_bd_addr_t rejected_address;
    bool rssi_inflight;
    bool rssi_timed_out;
    bool rssi_requested_before;
    TickType_t rssi_started;
    TickType_t rssi_sampled;
    uint64_t rssi_generation;
    esp_bd_addr_t rssi_address;
    TickType_t forget_started;
    uint64_t forget_generation;
    unsigned int forget_previous_count;
    bool remove_inflight;
    bool remove_completed;
    esp_err_t remove_result;
    esp_bd_addr_t remove_address;
    bool closing;
    bool link_fault;
    bool keyboard_cccd;
    bool mouse_cccd;
    bool auth_complete;
    bool bonded;
    bool bond_resolved;
    uint32_t auth_revision;
    uint8_t cccd_written;
    uint8_t cccd_pending;
    TickType_t cccd_started;
    uint8_t bond_fingerprint[SUBSCRIPTION_DIGEST_LENGTH];
    bool link_congested;
    bool submitting;
    bool notification_pending;
    bool notification_timed_out;
    bool timeout_reported;
    bool subscription_timed_out;
    bool subscription_timeout_reported;
    TickType_t notification_started;
    uint64_t connection_generation;
    uint64_t notification_generation;
    uint16_t notification_conn_id;
    uint16_t notification_handle;
    uint16_t conn_id;
    esp_bd_addr_t address;
    esp_gatt_if_t interface;
    uint16_t handles[ATTR_COUNT];
    uint8_t keyboard[HID_ENGINE_KEYBOARD_REPORT_LENGTH];
    uint8_t mouse[HID_ENGINE_MOUSE_REPORT_LENGTH];
} state = {.interface = ESP_GATT_IF_NONE,
           .bond_list = {.error = ESP_ERR_INVALID_STATE},
           .public = {.rssi_error = ESP_ERR_NOT_FOUND}};

/* Every helper with a _locked suffix requires state_lock. No SDK call, queue
 * wait or logging happens while this short cross-core critical section is held. */
static void refresh_locked(void)
{
    state.public.advertising = state.advertising;
    state.public.rssi_pending = state.rssi_inflight && !state.rssi_timed_out &&
        state.public.connected && state.rssi_generation == state.connection_generation;
    state.public.ready = state.public.connected && state.service_started &&
        !state.public.failed && !state.link_fault && state.public.encrypted &&
        state.public.bond_forget_state != BLE_HID_FORGET_RUNNING &&
        state.bond_resolved && state.cccd_pending == 0 &&
        !state.public.suspended && state.keyboard_cccd && state.mouse_cccd;
    state.public.congested = state.public.connected &&
        (state.link_congested || state.notification_pending || state.submitting);
}

static bool pairing_allowed_locked(TickType_t now)
{
    return state.public.pairing_open &&
        state.bond_list.known && state.bond_list.count < PAIRING_BOND_CAPACITY &&
        (TickType_t)(now - state.pairing_started) < state.pairing_duration &&
        state.public.bond_forget_state != BLE_HID_FORGET_RUNNING && !state.public.failed;
}

static void open_pairing_locked(TickType_t now, uint32_t duration_ms)
{
    state.pairing_started = now;
    state.pairing_duration = pdMS_TO_TICKS(duration_ms);
    state.public.pairing_open = true;
}

static bool saved_peer_locked(const esp_bd_addr_t address)
{
    for (unsigned int i = 0; i < state.bond_list.count; ++i) {
        if (memcmp(address, state.bond_list.bonds[i].address, ESP_BD_ADDR_LEN) == 0) return true;
    }
    return false;
}

static void bond_list_status_locked(bool known, bool updating, esp_err_t error)
{
    state.bond_list.known = known;
    state.bond_list.updating = updating;
    state.bond_list.error = error;
    ++state.bond_list.revision;
    if (state.bond_list.revision == 0) ++state.bond_list.revision;
    state.public.bond_count_known = known;
    state.public.bond_count = state.bond_list.count;
}

static void invalidate_bonds_locked(void)
{
    ++state.bond_mutation_revision;
    if (state.bond_mutation_revision == 0) ++state.bond_mutation_revision;
    state.bond_refresh_pending = true;
    bond_list_status_locked(false,
        state.public.bond_forget_state == BLE_HID_FORGET_RUNNING, ESP_ERR_INVALID_STATE);
}

/* Worker only. Publish addresses/count after a complete successful bounded
 * read; an unavailable list is never interpreted as zero saved hosts. */
static esp_err_t refresh_known_bonds(void)
{
    portENTER_CRITICAL(&state_lock);
    uint64_t mutation = state.bond_mutation_revision;
    portEXIT_CRITICAL(&state_lock);
    int count = esp_ble_get_bond_device_num();
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (count >= 0 && count <= (int)SUBSCRIPTION_CAPACITY) {
        /* Read with the full allocated capacity, then check the actual total:
         * an intervening addition must not truncate the list into false zero. */
        count = (int)SUBSCRIPTION_CAPACITY;
        error = esp_ble_get_bond_device_list(&count, subscriptions.bonds);
        if (error == ESP_OK && esp_ble_get_bond_device_num() != count) {
            error = ESP_ERR_INVALID_STATE;
        }
    }
    portENTER_CRITICAL(&state_lock);
    if (mutation != state.bond_mutation_revision) {
        /* A completion during this read may have persisted another bond.
         * Preserve its invalidation and retry on a later worker iteration. */
        portEXIT_CRITICAL(&state_lock);
        subscription_clear_bonds();
        return ESP_ERR_INVALID_STATE;
    }
    state.bond_refresh_pending = false;
    if (error != ESP_OK || count < 0 || count > (int)SUBSCRIPTION_CAPACITY) {
        if (error == ESP_OK) error = ESP_ERR_INVALID_STATE;
        bond_list_status_locked(false,
            state.public.bond_forget_state == BLE_HID_FORGET_RUNNING, error);
        portEXIT_CRITICAL(&state_lock);
        subscription_clear_bonds();
        return error;
    }
    memset(state.bond_list.bonds, 0, sizeof(state.bond_list.bonds));
    for (int i = 0; i < count; ++i) {
        ble_hid_bond_t *bond = &state.bond_list.bonds[i];
        memcpy(bond->address, subscriptions.bonds[i].bd_addr, ESP_BD_ADDR_LEN);
        bond->address_type_known = (unsigned int)subscriptions.bonds[i].bd_addr_type <= 3U;
        if (bond->address_type_known) {
            bond->address_type = (uint8_t)subscriptions.bonds[i].bd_addr_type;
        }
    }
    state.bond_list.count = (uint8_t)count;
    state.bonds_loaded = true;
    bool updating = state.public.bond_forget_state == BLE_HID_FORGET_RUNNING;
    bool failed = state.public.bond_forget_state == BLE_HID_FORGET_FAILED;
    bond_list_status_locked(!updating && !failed, updating,
        failed ? state.public.bond_forget_error : ESP_OK);
    portEXIT_CRITICAL(&state_lock);
    subscription_clear_bonds();
    return ESP_OK;
}

static void advance_generation_locked(void)
{
    ++state.public.generation;
    /* Zero is reserved for the never-connected state. */
    if (state.public.generation == 0) {
        ++state.public.generation;
    }
}

static void fail_transport(const char *operation, esp_err_t error)
{
    portENTER_CRITICAL(&state_lock);
    if (!state.public.failed) {
        advance_generation_locked();
    }
    state.public.failed = true;
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
}

static void fail_link_current(const control_command_t *command, uint32_t revision,
                              bool check_revision, const char *operation, esp_err_t error)
{
    portENTER_CRITICAL(&state_lock);
    bool current = state.public.connected && state.conn_id == command->conn_id &&
        state.connection_generation == command->generation &&
        memcmp(state.address, command->address, ESP_BD_ADDR_LEN) == 0 &&
        (!check_revision || state.auth_revision == revision);
    if (current) {
        if (!state.public.failed) {
            advance_generation_locked();
        }
        state.public.failed = true;
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
    if (current) {
        ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    }
}

static void post_control(const control_command_t *command)
{
    if (xQueueSend(control_queue, command, 0) != pdTRUE) {
        /* No silent loss of security responses, ATT replies or link changes. */
        if (command->kind == COMMAND_CREATE_SERVICE || command->kind == COMMAND_START_SERVICE) {
            fail_transport("Control queue capacity", ESP_ERR_NO_MEM);
        } else {
            fail_link_current(command, 0, false, "Control queue capacity", ESP_ERR_NO_MEM);
        }
    }
}

static void reset_link_locked(void)
{
    state.public.connected = false;
    state.public.encrypted = false;
    state.public.suspended = false;
    state.public.keyboard_leds = 0;
    state.public.peer_known = false;
    state.peer_address_type_known = false;
    state.peer_address_type = 0;
    memset(state.public.peer_address, 0, sizeof(state.public.peer_address));
    state.public.rssi_valid = false;
    state.public.rssi_pending = false;
    state.public.rssi_dbm = 0;
    state.public.rssi_age_ms = 0;
    state.public.rssi_error = ESP_ERR_NOT_FOUND;
    state.peer_saved = false;
    state.pairing_admitted = false;
    state.pairing_admission_pending = false;
    state.rssi_requested_before = false;
    state.keyboard_cccd = false;
    state.mouse_cccd = false;
    state.auth_complete = false;
    state.bonded = false;
    state.bond_resolved = false;
    state.auth_revision = 0;
    state.cccd_written = 0;
    state.cccd_pending = 0;
    memset(state.bond_fingerprint, 0, sizeof(state.bond_fingerprint));
    state.link_congested = false;
    state.link_fault = false;
    state.closing = false;
    memset(state.keyboard, 0, sizeof(state.keyboard));
    memset(state.mouse, 0, sizeof(state.mouse));
    /* Keep any SDK submission outstanding. Advertising cannot restart until
     * its CONF event drains, preventing conn_id reuse from retargeting it. */
    advance_generation_locked();
    refresh_locked();
}

/* The structured Bluedroid API accepts 128-bit UUID entries, including for
 * Bluetooth-assigned 16-bit services. It compresses this base UUID on air. */
static uint8_t advertised_services[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};
static esp_ble_adv_data_t advertising_data = {
    .include_name = true,
    .appearance = 0x03c0, /* Generic Human Interface Device. */
    .service_uuid_len = sizeof(advertised_services),
    .p_service_uuid = advertised_services,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};
static esp_ble_adv_data_t scan_data = {.set_scan_rsp = true, .include_name = true};
static esp_ble_adv_params_t advertising_parameters = {
    .adv_int_min = 0x30, /* 30..60 ms in 0.625 ms units. */
    .adv_int_max = 0x60,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void notification_watchdog(TimerHandle_t timer)
{
    (void)timer;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    bool timed_out = state.notification_pending && !state.notification_timed_out &&
        (TickType_t)(now - state.notification_started) >=
            pdMS_TO_TICKS(NOTIFICATION_TIMEOUT_MS);
    bool subscription_timeout = state.cccd_pending != 0 && !state.subscription_timed_out &&
        (TickType_t)(now - state.cccd_started) >= pdMS_TO_TICKS(SUBSCRIPTION_TIMEOUT_MS);
    if (state.public.pairing_open &&
        (TickType_t)(now - state.pairing_started) >= state.pairing_duration) {
        state.public.pairing_open = false;
    }
    if (state.rejected_pending && !state.public.failed &&
        (TickType_t)(now - state.rejected_started) >= pdMS_TO_TICKS(REJECT_TIMEOUT_MS)) {
        state.public.failed = true;
        advance_generation_locked();
    }
    if (state.public.connected && !state.auth_complete && !state.link_fault &&
        (TickType_t)(now - state.handshake_started) >= pdMS_TO_TICKS(PAIRING_HANDSHAKE_TIMEOUT_MS)) {
        state.link_fault = true;
        advance_generation_locked();
    }
    if (state.rssi_inflight && !state.rssi_timed_out &&
        (TickType_t)(now - state.rssi_started) >= pdMS_TO_TICKS(RSSI_TIMEOUT_MS)) {
        state.rssi_timed_out = true;
        if (state.public.connected && state.rssi_generation == state.connection_generation) {
            state.public.rssi_valid = false;
            state.public.rssi_dbm = 0;
            state.public.rssi_error = ESP_ERR_TIMEOUT;
        }
        /* Keep the in-flight token quarantined until its callback drains.
         * Optional RSSI must never relabel an old sample as a new peer's. */
    }
    if (state.public.rssi_valid &&
        (TickType_t)(now - state.rssi_sampled) > pdMS_TO_TICKS(RSSI_MAX_AGE_MS)) {
        state.public.rssi_valid = false;
        state.public.rssi_dbm = 0;
        state.public.rssi_error = ESP_ERR_TIMEOUT;
    }
    if (state.public.bond_forget_state == BLE_HID_FORGET_RUNNING &&
        (TickType_t)(now - state.forget_started) >= pdMS_TO_TICKS(FORGET_TIMEOUT_MS)) {
        state.public.bond_forget_state = BLE_HID_FORGET_FAILED;
        state.public.bond_forget_error = ESP_ERR_TIMEOUT;
        bond_list_status_locked(false, false, ESP_ERR_TIMEOUT);
        state.public.pairing_open = false;
        if (!state.public.failed) advance_generation_locked();
        state.public.failed = true;
    }
    if (timed_out) {
        state.notification_timed_out = true;
        state.public.failed = true;
        advance_generation_locked();
        refresh_locked();
    }
    if (subscription_timeout) {
        state.subscription_timed_out = true;
        state.public.failed = true;
        advance_generation_locked();
        refresh_locked();
    }
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);
    /* Timer callback only changes state. The deadline remains observable even
     * if the Bluetooth worker is waiting inside the SDK's internal queue.
     * A late CONF cannot identify a later conn_id; only reboot resets failure. */
}

static void fail_forget(uint64_t operation, esp_err_t error)
{
    portENTER_CRITICAL(&state_lock);
    bool current = state.public.bond_forget_state == BLE_HID_FORGET_RUNNING &&
        state.forget_generation == operation;
    if (current) {
        state.public.bond_forget_state = BLE_HID_FORGET_FAILED;
        state.public.bond_forget_error = error;
        bond_list_status_locked(false, false, error);
        state.public.pairing_open = false;
        if (!state.public.failed) advance_generation_locked();
        state.public.failed = true;
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
    if (current) ESP_LOGE(TAG, "Forget bonds failed: %s", esp_err_to_name(error));
}

static void service_bond_forget(void)
{
    portENTER_CRITICAL(&state_lock);
    bool running = state.public.bond_forget_state == BLE_HID_FORGET_RUNNING;
    uint64_t operation = state.forget_generation;
    bool failed = state.public.failed;
    bool blocked = state.public.connected || state.rejected_pending || state.advertising ||
        state.adv_pending || state.adv_stop_pending || state.notification_pending || state.submitting;
    bool waiting = state.remove_inflight && !state.remove_completed;
    esp_err_t completed = state.remove_result;
    if (state.remove_completed) {
        state.remove_completed = false;
        state.remove_inflight = false;
    }
    portEXIT_CRITICAL(&state_lock);
    if (!running) return;
    if (failed) {
        fail_forget(operation, ESP_FAIL);
        return;
    }
    if (blocked || waiting) return;
    if (completed != ESP_OK) {
        fail_forget(operation, completed);
        return;
    }
    esp_err_t error = refresh_known_bonds();
    if (error != ESP_OK) {
        fail_forget(operation, error);
        return;
    }
    portENTER_CRITICAL(&state_lock);
    unsigned int count = state.bond_list.count;
    bool current = state.public.bond_forget_state == BLE_HID_FORGET_RUNNING &&
        state.forget_generation == operation && state.public.generation == operation &&
        !state.public.failed && !state.public.connected && !state.rejected_pending;
    bool progress = count < state.forget_previous_count;
    esp_bd_addr_t address = {0};
    if (count != 0) memcpy(address, state.bond_list.bonds[0].address, sizeof(address));
    portEXIT_CRITICAL(&state_lock);
    if (!current) return;
    if (!progress) {
        fail_forget(operation, ESP_FAIL);
        return;
    }
    if (count == 0) {
        error = subscription_forget_all();
        if (error == ESP_OK) error = refresh_known_bonds();
        if (error != ESP_OK) {
            fail_forget(operation, error);
            return;
        }
        portENTER_CRITICAL(&state_lock);
        current = state.public.bond_forget_state == BLE_HID_FORGET_RUNNING &&
            state.forget_generation == operation && state.public.generation == operation &&
            !state.public.failed && !state.public.connected && !state.rejected_pending;
        bool empty = state.bond_list.count == 0;
        if (current && empty) {
            state.public.bond_forget_state = BLE_HID_FORGET_DONE;
            state.public.bond_forget_error = ESP_OK;
            bond_list_status_locked(true, false, ESP_OK);
            open_pairing_locked(xTaskGetTickCount(), BLE_HID_PAIRING_DEFAULT_MS);
            refresh_locked();
        }
        portEXIT_CRITICAL(&state_lock);
        if (current && !empty) fail_forget(operation, ESP_FAIL);
        return;
    }
    portENTER_CRITICAL(&state_lock);
    state.forget_previous_count = count;
    state.remove_inflight = true;
    state.remove_completed = false;
    state.remove_result = ESP_OK;
    memcpy(state.remove_address, address, sizeof(address));
    portEXIT_CRITICAL(&state_lock);
    error = esp_ble_remove_bond_device(address);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&state_lock);
        state.remove_inflight = false;
        portEXIT_CRITICAL(&state_lock);
        fail_forget(operation, error);
    }
}

static void service_rssi(void)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    bool request = state.public.connected && !state.public.failed && !state.link_fault &&
        !state.closing && !state.rssi_inflight &&
        state.public.bond_forget_state != BLE_HID_FORGET_RUNNING &&
        (!state.rssi_requested_before ||
         (TickType_t)(now - state.rssi_started) >= pdMS_TO_TICKS(RSSI_INTERVAL_MS));
    esp_bd_addr_t address;
    memcpy(address, state.address, sizeof(address));
    uint64_t generation = state.connection_generation;
    if (request) {
        state.rssi_requested_before = true;
        state.rssi_inflight = true;
        state.rssi_timed_out = false;
        state.rssi_started = now;
        state.public.rssi_error = ESP_OK;
        state.rssi_generation = generation;
        memcpy(state.rssi_address, address, sizeof(address));
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
    if (!request) return;
    esp_err_t error = esp_ble_gap_read_rssi(address);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&state_lock);
        if (state.rssi_inflight && state.rssi_generation == generation) {
            state.rssi_inflight = false;
            if (state.public.connected && state.connection_generation == generation) {
                state.public.rssi_valid = false;
                state.public.rssi_dbm = 0;
                state.public.rssi_error = error;
            }
            refresh_locked();
        }
        portEXIT_CRITICAL(&state_lock);
    }
}

static void service_link_control(void)
{
    notification_watchdog(NULL);
    bool close_link = false;
    bool start_adv = false;
    bool stop_adv = false;
    bool report_timeout = false;
    bool report_subscription_timeout = false;
    uint16_t conn_id;
    esp_gatt_if_t interface;
    control_command_t identity = {0};
    uint32_t revision;
    portENTER_CRITICAL(&state_lock);
    conn_id = state.conn_id;
    interface = state.interface;
    identity.conn_id = conn_id;
    identity.generation = state.connection_generation;
    memcpy(identity.address, state.address, sizeof(identity.address));
    revision = state.auth_revision;
    if (state.notification_timed_out && !state.timeout_reported) {
        state.timeout_reported = true;
        report_timeout = true;
    }
    if (state.subscription_timed_out && !state.subscription_timeout_reported) {
        state.subscription_timeout_reported = true;
        report_subscription_timeout = true;
    }
    if ((state.public.failed || state.link_fault) && state.public.connected &&
        !state.closing) {
        state.closing = true;
        close_link = true;
    }
    if ((state.public.failed || state.public.connected ||
         state.public.bond_forget_state == BLE_HID_FORGET_RUNNING) &&
        state.advertising && !state.adv_stop_pending) {
        state.adv_stop_pending = true;
        state.adv_stopped_by_connect = false;
        stop_adv = true;
    }
    if (!state.public.failed && !state.public.connected && state.service_started &&
        state.adv_configured && state.scan_configured && !state.adv_pending &&
        !state.advertising && !state.adv_stop_pending && !state.rejected_pending &&
        state.bonds_loaded && state.public.bond_forget_state != BLE_HID_FORGET_RUNNING &&
        !state.submitting && !state.notification_pending) {
        state.adv_pending = true;
        start_adv = true;
    }
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);
    if (report_timeout) {
        ESP_LOGE(TAG, "Notification completion timeout; reboot required (bonds preserved)");
    }
    if (report_subscription_timeout) {
        ESP_LOGE(TAG, "Subscription persistence timeout; reboot required (bonds preserved)");
    }
    if (close_link) {
        esp_err_t error = esp_ble_gatts_close(interface, conn_id);
        if (error != ESP_OK) {
            fail_link_current(&identity, revision, true, "Close connection", error);
        }
    }
    if (stop_adv) {
        esp_err_t error = esp_ble_gap_stop_advertising();
        if (error != ESP_OK) {
            fail_transport("Stop advertising", error);
        }
    }
    if (start_adv) {
        esp_err_t error = esp_ble_gap_start_advertising(&advertising_parameters);
        if (error != ESP_OK) {
            fail_transport("Start advertising", error);
        }
    }
}

static struct {
    control_command_t jobs[CONTROL_QUEUE_LENGTH];
    unsigned int count;
} pending_subscriptions;

static bool command_current_locked(const control_command_t *command)
{
    return state.public.connected && state.conn_id == command->conn_id &&
        state.connection_generation == command->generation &&
        memcmp(state.address, command->address, ESP_BD_ADDR_LEN) == 0;
}

/* Durable bond changes outlive the link that caused them. Authentication
 * invalidates this cache in the callback; disconnect never clears the latch. */
static void service_bond_cache(void)
{
    portENTER_CRITICAL(&state_lock);
    bool refresh = state.bond_refresh_pending && !state.public.failed &&
        state.public.bond_forget_state != BLE_HID_FORGET_RUNNING;
    portEXIT_CRITICAL(&state_lock);
    if (refresh) (void)refresh_known_bonds();
    /* A failed read remains explicitly unavailable. Saved-host auth can
     * trigger another read; an unknown peer cannot use the stale capacity. */
}

static void service_pairing_admission(void)
{
    control_command_t identity = {0};
    portENTER_CRITICAL(&state_lock);
    bool pending = state.public.connected && state.pairing_admission_pending &&
        !state.auth_complete && !state.public.failed && !state.link_fault;
    identity.conn_id = state.conn_id;
    identity.generation = state.connection_generation;
    memcpy(identity.address, state.address, sizeof(identity.address));
    uint32_t revision = state.auth_revision;
    portEXIT_CRITICAL(&state_lock);
    if (!pending) return;

    /* A cached free slot only admits the physical link. Confirm actual SDK
     * capacity before starting SMP, whose persistence can evict an old bond. */
    esp_err_t error = refresh_known_bonds();
    portENTER_CRITICAL(&state_lock);
    bool current = command_current_locked(&identity) && state.auth_revision == revision &&
        state.pairing_admission_pending && !state.auth_complete &&
        !state.public.failed && !state.link_fault;
    bool retry = error != ESP_OK && state.bond_refresh_pending;
    bool grant = current && error == ESP_OK && state.bond_list.known &&
        state.bond_list.count < PAIRING_BOND_CAPACITY &&
        (TickType_t)(xTaskGetTickCount() - state.handshake_started) <
            pdMS_TO_TICKS(PAIRING_HANDSHAKE_TIMEOUT_MS);
    if (current && !retry) {
        state.pairing_admission_pending = false;
        state.pairing_admitted = grant;
        if (!grant && error == ESP_OK) {
            state.link_fault = true;
            advance_generation_locked();
        }
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
    if (!current || retry) return;
    if (grant) error = esp_ble_set_encryption(identity.address, ESP_BLE_SEC_ENCRYPT);
    if (error != ESP_OK) {
        fail_link_current(&identity, revision, true, "Pairing capacity/encryption", error);
    }
}

static void respond_control(const control_command_t *command)
{
    portENTER_CRITICAL(&state_lock);
    bool current = command_current_locked(command);
    esp_gatt_if_t interface = state.interface;
    uint32_t revision = command->auth_revision;
    if (revision == 0 && state.auth_revision == 1) {
        revision = 1; /* Initial CCCD write received before first bond completion. */
    }
    portEXIT_CRITICAL(&state_lock);
    if (!current) {
        return;
    }
    esp_gatt_rsp_t response = {0};
    response.attr_value.handle = command->handle;
    response.attr_value.offset = command->offset;
    response.attr_value.len = command->length;
    memcpy(response.attr_value.value, command->value, command->length);
    esp_err_t error = esp_ble_gatts_send_response(interface, command->conn_id,
        command->transaction, command->status, &response);
    if (error != ESP_OK) {
        fail_link_current(command, revision, true, "ATT response", error);
    }
}

static void resolve_subscriptions(void)
{
    portENTER_CRITICAL(&state_lock);
    bool resolve = state.public.connected && state.auth_complete &&
        !state.bond_resolved && !state.public.failed;
    bool bonded = state.bonded;
    bool cache_pending = state.bond_refresh_pending;
    bool cache_known = state.bond_list.known;
    esp_err_t cache_error = state.bond_list.error;
    uint64_t generation = state.connection_generation;
    uint32_t revision = state.auth_revision;
    uint16_t conn_id = state.conn_id;
    esp_bd_addr_t address;
    memcpy(address, state.address, sizeof(address));
    portEXIT_CRITICAL(&state_lock);
    if (!resolve || (bonded && cache_pending)) {
        return;
    }
    esp_err_t error = ESP_OK;
    if (bonded) {
        error = cache_known ? ESP_OK : cache_error;
        if (!cache_known && error == ESP_OK) error = ESP_ERR_INVALID_STATE;
        if (error != ESP_OK) {
            control_command_t identity = {.generation = generation, .conn_id = conn_id};
            memcpy(identity.address, address, sizeof(address));
            fail_link_current(&identity, revision, true, "Bond list refresh", error);
            return;
        }
    }
    uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH] = {0};
    error = bonded ? subscription_bond(address, fingerprint) : ESP_OK;
    if (error != ESP_OK) {
        control_command_t identity = {.generation = generation, .conn_id = conn_id};
        memcpy(identity.address, address, sizeof(address));
        fail_link_current(&identity, revision, true, "Trusted bond identity unavailable", error);
        return;
    }
    uint8_t mask = bonded ? subscription_mask(fingerprint) : 0;
    portENTER_CRITICAL(&state_lock);
    if (state.public.connected && state.connection_generation == generation &&
        state.auth_revision == revision && memcmp(state.address, address, sizeof(address)) == 0 &&
        !state.public.failed) {
        memcpy(state.bond_fingerprint, fingerprint, sizeof(fingerprint));
        state.bond_resolved = true;
        if (bonded) {
            bool newly_saved = !state.peer_saved;
            state.peer_saved = true;
            if (newly_saved) state.public.pairing_open = false;
        }
        /* Actual writes received on this link always supersede restoration,
         * even when a slow NVS read was already in progress. */
        if (!(state.cccd_written & 1)) {
            state.keyboard_cccd = (mask & 1) != 0;
        }
        if (!(state.cccd_written & 2)) {
            state.mouse_cccd = (mask & 2) != 0;
        }
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
}

static void process_subscription_jobs(void)
{
    resolve_subscriptions();
    unsigned int index = 0;
    while (index < pending_subscriptions.count) {
        control_command_t command = pending_subscriptions.jobs[index];
        portENTER_CRITICAL(&state_lock);
        bool current = command_current_locked(&command);
        bool failed = state.public.failed;
        bool resolved = state.bond_resolved;
        bool bonded = state.bonded;
        uint32_t revision = state.auth_revision;
        bool revision_matches = command.auth_revision == state.auth_revision ||
            (command.auth_revision == 0 && state.auth_revision == 1);
        uint8_t mask = (state.keyboard_cccd ? 1 : 0) | (state.mouse_cccd ? 2 : 0);
        uint8_t fingerprint[SUBSCRIPTION_DIGEST_LENGTH];
        memcpy(fingerprint, state.bond_fingerprint, sizeof(fingerprint));
        portEXIT_CRITICAL(&state_lock);
        if (current && !resolved && !failed) {
            ++index;
            continue;
        }
        if (current) {
            esp_err_t error = ESP_OK;
            uint8_t bit = command.handle == state.handles[ATTR_KEYBOARD_CCCD] ? 1 : 2;
            mask = command.value[0] ? (uint8_t)(mask | bit) : (uint8_t)(mask & (uint8_t)~bit);
            if (failed || !revision_matches) {
                error = ESP_ERR_INVALID_STATE;
            } else if (bonded) {
                uint8_t verified[SUBSCRIPTION_DIGEST_LENGTH];
                error = subscription_bond(command.address, verified);
                if (error == ESP_OK && memcmp(verified, fingerprint, sizeof(verified)) != 0) {
                    error = ESP_ERR_INVALID_STATE;
                }
                if (error == ESP_OK) {
                    error = subscription_save(fingerprint, mask);
                }
            }
            if (error != ESP_OK) {
                command.status = revision_matches ? ESP_GATT_INSUF_RESOURCE : ESP_GATT_INSUF_AUTHENTICATION;
                if (revision_matches) {
                    fail_link_current(&command, revision, true, "Subscription persistence", error);
                }
            }
            portENTER_CRITICAL(&state_lock);
            current = command_current_locked(&command);
            if (current) {
                if (error == ESP_OK && !state.public.failed &&
                    state.bond_resolved && memcmp(state.bond_fingerprint,
                        fingerprint, sizeof(fingerprint)) == 0 &&
                    (command.auth_revision == state.auth_revision ||
                     (command.auth_revision == 0 && state.auth_revision == 1))) {
                    if (bit == 1) {
                        state.keyboard_cccd = command.value[0] != 0;
                    } else {
                        state.mouse_cccd = command.value[0] != 0;
                    }
                } else if (error == ESP_OK) {
                    command.status = ESP_GATT_INSUF_AUTHENTICATION;
                }
                if ((command.auth_revision == state.auth_revision ||
                    (command.auth_revision == 0 && state.auth_revision == 1)) &&
                    state.cccd_pending > 0) {
                    --state.cccd_pending;
                }
                refresh_locked();
            }
            portEXIT_CRITICAL(&state_lock);
            if (current && command.reply) {
                respond_control(&command);
            }
        }
        --pending_subscriptions.count;
        memmove(&pending_subscriptions.jobs[index], &pending_subscriptions.jobs[index + 1],
            (pending_subscriptions.count - index) * sizeof(control_command_t));
    }
}

static void control_worker(void *argument)
{
    (void)argument;
    if (!subscriptions.loaded) {
        esp_err_t error = psa_crypto_init() == PSA_SUCCESS ? subscription_load() : ESP_FAIL;
        if (error != ESP_OK) {
            fail_transport("Subscription store initialization", error);
        }
    }
    portENTER_CRITICAL(&state_lock);
    bool load_bonds = !state.bonds_loaded && !state.public.failed;
    portEXIT_CRITICAL(&state_lock);
    if (load_bonds) {
        esp_err_t error = refresh_known_bonds();
        if (error != ESP_OK) {
            fail_transport("Bond list initialization", error);
        } else {
            portENTER_CRITICAL(&state_lock);
            if (state.bond_list.count == 0 && !state.pairing_user_controlled) {
                open_pairing_locked(xTaskGetTickCount(), BLE_HID_PAIRING_DEFAULT_MS);
            }
            portEXIT_CRITICAL(&state_lock);
        }
    }
    for (;;) {
        service_bond_cache();
        service_pairing_admission();
        control_command_t command;
        if (xQueueReceive(control_queue, &command,
                          pdMS_TO_TICKS(CONTROL_POLL_MS)) == pdTRUE) {
            esp_gatt_if_t interface;
            bool current;
            uint16_t service;
            uint32_t revision;
            portENTER_CRITICAL(&state_lock);
            interface = state.interface;
            service = state.handles[ATTR_SERVICE];
            revision = state.auth_revision;
            current = state.public.connected && state.conn_id == command.conn_id &&
                state.connection_generation == command.generation;
            portEXIT_CRITICAL(&state_lock);
            esp_err_t error = ESP_OK;
            switch (command.kind) {
            case COMMAND_CREATE_SERVICE:
                error = esp_ble_gatts_create_attr_tab(attributes, interface, ATTR_COUNT, 0);
                break;
            case COMMAND_START_SERVICE:
                error = esp_ble_gatts_start_service(service);
                break;
            case COMMAND_ENCRYPT:
                /* Outside the window, a saved host must use its existing LTK.
                 * Starting a peripheral SMP Security Request here would let
                 * a fresh Pair Request bypass the app's security grant.
                 * The independent admission service confirms capacity first. */
                break;
            case COMMAND_SECURITY_REPLY:
                portENTER_CRITICAL(&state_lock);
                current = command_current_locked(&command);
                bool grant = state.pairing_admitted && !state.link_fault &&
                    state.bond_list.known && !state.bond_refresh_pending &&
                    state.bond_list.count < PAIRING_BOND_CAPACITY &&
                    (TickType_t)(xTaskGetTickCount() - state.handshake_started) <
                        pdMS_TO_TICKS(PAIRING_HANDSHAKE_TIMEOUT_MS);
                portEXIT_CRITICAL(&state_lock);
                error = esp_ble_gap_security_rsp(command.address, command.accept && current && grant);
                break;
            case COMMAND_REJECT_CONNECTION:
                portENTER_CRITICAL(&state_lock);
                bool reject_current = state.rejected_pending &&
                    state.rejected_generation == command.generation &&
                    state.rejected_conn_id == command.conn_id &&
                    memcmp(state.rejected_address, command.address, ESP_BD_ADDR_LEN) == 0;
                portEXIT_CRITICAL(&state_lock);
                if (reject_current) error = esp_ble_gatts_close(interface, command.conn_id);
                break;
            case COMMAND_CCCD:
                if (pending_subscriptions.count == CONTROL_QUEUE_LENGTH) {
                    error = ESP_ERR_NO_MEM;
                } else {
                    pending_subscriptions.jobs[pending_subscriptions.count++] = command;
                }
                break;
            case COMMAND_RESPONSE:
                respond_control(&command);
                break;
            }
            if (error != ESP_OK) {
                if (command.kind == COMMAND_CREATE_SERVICE || command.kind == COMMAND_START_SERVICE) {
                    fail_transport("Bluetooth service operation", error);
                } else {
                    fail_link_current(&command, revision, true, "Bluetooth link operation", error);
                }
            }
        }
        process_subscription_jobs();
        service_link_control();
        service_bond_forget();
        service_rssi();
    }
}

static void handle_read(const esp_ble_gatts_cb_param_t *parameter)
{
    control_command_t command = {
        .kind = COMMAND_RESPONSE, .conn_id = parameter->read.conn_id,
        .transaction = parameter->read.trans_id, .handle = parameter->read.handle,
        .offset = parameter->read.offset, .status = ESP_GATT_OK,
    };
    memcpy(command.address, parameter->read.bda, ESP_BD_ADDR_LEN);
    portENTER_CRITICAL(&state_lock);
    bool current = state.public.connected && state.conn_id == parameter->read.conn_id &&
        memcmp(state.address, parameter->read.bda, ESP_BD_ADDR_LEN) == 0;
    if (current) {
        command.generation = state.connection_generation;
        command.auth_revision = state.auth_revision;
        /* Encrypted permissions are checked by GATT before app dispatch. This
         * also proves encryption on bonded reconnects without a pairing event. */
        state.public.encrypted = true;
        if (command.handle == state.handles[ATTR_KEYBOARD]) {
            command.length = sizeof(state.keyboard);
            memcpy(command.value, state.keyboard, command.length);
        } else if (command.handle == state.handles[ATTR_MOUSE]) {
            command.length = sizeof(state.mouse);
            memcpy(command.value, state.mouse, command.length);
        } else if (command.handle == state.handles[ATTR_LED]) {
            command.length = 1;
            command.value[0] = state.public.keyboard_leds;
        } else if (command.handle == state.handles[ATTR_KEYBOARD_CCCD] ||
                   command.handle == state.handles[ATTR_MOUSE_CCCD]) {
            command.length = 2;
            command.value[0] = command.handle == state.handles[ATTR_KEYBOARD_CCCD] ?
                state.keyboard_cccd : state.mouse_cccd;
        } else {
            command.status = ESP_GATT_READ_NOT_PERMIT;
        }
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
    if (!current || !parameter->read.need_rsp) {
        return;
    }
    if (command.offset > command.length) {
        command.status = ESP_GATT_INVALID_OFFSET;
        command.length = 0;
    } else {
        command.length -= command.offset;
        memmove(command.value, command.value + command.offset, command.length);
    }
    post_control(&command);
}

static void handle_write(const esp_ble_gatts_cb_param_t *parameter)
{
    bool capacity_failure = false;
    control_command_t command = {
        .kind = COMMAND_RESPONSE, .conn_id = parameter->write.conn_id,
        .transaction = parameter->write.trans_id, .handle = parameter->write.handle,
        .status = ESP_GATT_OK, .reply = parameter->write.need_rsp,
    };
    TickType_t started = xTaskGetTickCount();
    memcpy(command.address, parameter->write.bda, ESP_BD_ADDR_LEN);
    portENTER_CRITICAL(&state_lock);
    bool current = state.public.connected && state.conn_id == parameter->write.conn_id &&
        memcmp(state.address, parameter->write.bda, ESP_BD_ADDR_LEN) == 0;
    if (current) {
        command.generation = state.connection_generation;
        bool was_ready = state.public.ready;
        command.auth_revision = state.auth_revision;
        state.public.encrypted = true;
        uint16_t handle = parameter->write.handle;
        bool cccd = handle == state.handles[ATTR_KEYBOARD_CCCD] ||
                    handle == state.handles[ATTR_MOUSE_CCCD];
        if (parameter->write.is_prep) {
            command.status = ESP_GATT_REQ_NOT_SUPPORTED;
        } else if (parameter->write.offset != 0) {
            command.status = ESP_GATT_INVALID_OFFSET;
        } else if (parameter->write.len != (cccd ? 2 : 1) || !parameter->write.value) {
            command.status = ESP_GATT_INVALID_ATTR_LEN;
        } else if (cccd) {
            const uint8_t *value = parameter->write.value;
            if (value[1] != 0 || value[0] > 1) {
                command.status = ESP_GATT_CCC_CFG_ERR;
            } else if (state.cccd_pending == CONTROL_QUEUE_LENGTH) {
                command.status = ESP_GATT_INSUF_RESOURCE;
                capacity_failure = true;
                /* A Write Command has no ATT error response. Never silently
                 * retain enabled notifications when its disable cannot queue. */
                if (!value[0]) {
                    if (handle == state.handles[ATTR_KEYBOARD_CCCD]) {
                        state.keyboard_cccd = false;
                    } else {
                        state.mouse_cccd = false;
                    }
                }
            } else {
                command.kind = COMMAND_CCCD;
                command.value[0] = value[0];
                command.auth_revision = state.auth_revision;
                uint8_t bit = handle == state.handles[ATTR_KEYBOARD_CCCD] ? 1 : 2;
                state.cccd_written |= bit;
                if (state.cccd_pending++ == 0) {
                    state.cccd_started = started;
                }
                /* Disable immediately; enable only after the worker has
                 * committed a trusted bond's write successfully. */
                if (!value[0]) {
                    if (bit == 1) {
                        state.keyboard_cccd = false;
                    } else {
                        state.mouse_cccd = false;
                    }
                }
            }
        } else if (handle == state.handles[ATTR_CONTROL]) {
            if (parameter->write.value[0] > 1) {
                command.status = ESP_GATT_OUT_OF_RANGE;
            } else {
                state.public.suspended = parameter->write.value[0] == 0;
            }
        } else if (handle == state.handles[ATTR_LED]) {
            /* Padding bits are not LED state. */
            state.public.keyboard_leds = parameter->write.value[0] & 0x1f;
        } else {
            command.status = ESP_GATT_WRITE_NOT_PERMIT;
        }
        refresh_locked();
        if (was_ready && !state.public.ready) {
            advance_generation_locked();
        }
    }
    portEXIT_CRITICAL(&state_lock);
    if (capacity_failure) {
        fail_link_current(&command, 0, false, "Subscription queue capacity", ESP_ERR_NO_MEM);
    }
    if (current && (parameter->write.need_rsp || command.kind == COMMAND_CCCD)) {
        post_control(&command);
    }
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *parameter)
{
    bool failure = false;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        state.adv_configured = parameter->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
        failure = !state.adv_configured;
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        state.scan_configured = parameter->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
        failure = !state.scan_configured;
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        state.adv_pending = false;
        state.advertising = parameter->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
        failure = !state.advertising;
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (parameter->adv_stop_cmpl.status == ESP_BT_STATUS_SUCCESS || state.adv_stopped_by_connect) {
            /* A legacy connect already stopped advertising in the controller. */
            state.adv_stop_pending = false;
            state.advertising = false;
            state.adv_stopped_by_connect = false;
        } else {
            failure = true;
        }
        break;
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
        if (state.rssi_inflight && memcmp(state.rssi_address,
            parameter->read_rssi_cmpl.remote_addr, ESP_BD_ADDR_LEN) == 0) {
            bool current = state.public.connected &&
                state.connection_generation == state.rssi_generation &&
                memcmp(state.address, state.rssi_address, ESP_BD_ADDR_LEN) == 0;
            if (current && !state.rssi_timed_out) {
                bool valid = parameter->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS &&
                    parameter->read_rssi_cmpl.rssi >= -127 && parameter->read_rssi_cmpl.rssi <= 20;
                state.public.rssi_valid = valid;
                state.public.rssi_dbm = valid ? parameter->read_rssi_cmpl.rssi : 0;
                state.public.rssi_error = valid ? ESP_OK :
                    (parameter->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS ? ESP_ERR_NOT_FOUND : ESP_FAIL);
                if (valid) state.rssi_sampled = now;
            }
            state.rssi_inflight = false;
        }
        break;
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
        if (state.public.bond_forget_state == BLE_HID_FORGET_RUNNING && state.remove_inflight &&
            !state.remove_completed && memcmp(state.remove_address,
                parameter->remove_bond_dev_cmpl.bd_addr, ESP_BD_ADDR_LEN) == 0) {
            state.remove_result = parameter->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS ?
                ESP_OK : ESP_FAIL;
            state.remove_completed = true;
        }
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        /* The SDK may already have persisted keys, even if DISCONNECT has
         * arrived or this event belongs to an obsolete link. Invalidate the
         * durable view independently of current connection/auth state. */
        invalidate_bonds_locked();
        if (parameter->ble_security.auth_cmpl.success &&
            (parameter->ble_security.auth_cmpl.auth_mode & ESP_LE_AUTH_BOND) &&
            (!state.public.connected || !state.peer_saved || memcmp(state.address,
                parameter->ble_security.auth_cmpl.bd_addr, ESP_BD_ADDR_LEN) != 0)) {
            state.public.pairing_open = false;
        }
        if (state.public.connected && memcmp(state.address,
            parameter->ble_security.auth_cmpl.bd_addr, ESP_BD_ADDR_LEN) == 0) {
            bool was_ready = state.public.ready;
            state.bond_resolved = false;
            if (parameter->ble_security.auth_cmpl.success) {
                if (state.auth_complete) {
                    /* A new authentication on an existing link may replace
                     * its bond. Only new writes or the new fingerprint's
                     * saved record may authorize that incarnation. */
                    state.keyboard_cccd = false;
                    state.mouse_cccd = false;
                    state.cccd_written = 0;
                    state.cccd_pending = 0;
                }
                /* In the pinned SMP_ENC_CMPL path this follows actual link
                 * encryption, including bonded re-encryption. Bond lookup and
                 * real stored CCCDs are still required before restoration. */
                state.public.encrypted = true;
                state.auth_complete = true;
                state.pairing_admitted = false; /* The one admitted handshake is consumed. */
                state.pairing_admission_pending = false;
                state.bonded = (parameter->ble_security.auth_cmpl.auth_mode & ESP_LE_AUTH_BOND) != 0;
                if (state.bonded && !(parameter->ble_security.auth_cmpl.auth_mode &
                                      ESP_LE_AUTH_REQ_SC_ONLY)) {
                    /* The public bond export omits local-only legacy LTKs.
                     * Require SC's exported PENC for durable bond identity. */
                    state.link_fault = true;
                    state.auth_complete = false;
                }
                ++state.auth_revision;
                if (state.auth_revision == 0) {
                    state.link_fault = true;
                    state.auth_complete = false;
                }
            } else {
                state.public.encrypted = false;
                state.link_fault = true;
            }
            refresh_locked();
            if (was_ready && !state.public.ready) {
                advance_generation_locked();
            }
        }
        break;
    default:
        break;
    }
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);
    if (failure) {
        fail_transport("Asynchronous advertising configuration", ESP_FAIL);
    }
    if (event == ESP_GAP_BLE_SEC_REQ_EVT) {
        control_command_t command = {.kind = COMMAND_SECURITY_REPLY};
        memcpy(command.address, parameter->ble_security.ble_req.bd_addr, ESP_BD_ADDR_LEN);
        portENTER_CRITICAL(&state_lock);
        command.generation = state.connection_generation;
        command.auth_revision = state.auth_revision;
        command.conn_id = state.conn_id;
        command.accept = state.public.connected && !state.public.failed && !state.link_fault &&
            (state.pairing_admitted || state.pairing_admission_pending) &&
            (TickType_t)(now - state.handshake_started) < pdMS_TO_TICKS(PAIRING_HANDSHAKE_TIMEOUT_MS) &&
            memcmp(state.address, command.address, ESP_BD_ADDR_LEN) == 0;
        portEXIT_CRITICAL(&state_lock);
        post_control(&command);
    }
}

static void gatts_callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface,
                           esp_ble_gatts_cb_param_t *parameter)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (parameter->reg.app_id != HID_APP_ID) {
            return;
        }
        if (parameter->reg.status != ESP_GATT_OK) {
            fail_transport("GATT registration", ESP_FAIL);
            return;
        }
        portENTER_CRITICAL(&state_lock);
        state.interface = interface;
        portEXIT_CRITICAL(&state_lock);
        post_control(&(control_command_t){.kind = COMMAND_CREATE_SERVICE});
        return;
    }
    portENTER_CRITICAL(&state_lock);
    bool owned = interface == state.interface;
    portEXIT_CRITICAL(&state_lock);
    if (!owned) {
        return;
    }
    switch (event) {
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (parameter->add_attr_tab.status != ESP_GATT_OK ||
            parameter->add_attr_tab.num_handle != ATTR_COUNT ||
            parameter->add_attr_tab.svc_inst_id != 0 ||
            !parameter->add_attr_tab.handles) {
            fail_transport("HID attribute table", ESP_FAIL);
            break;
        }
        portENTER_CRITICAL(&state_lock);
        memcpy(state.handles, parameter->add_attr_tab.handles, sizeof(state.handles));
        portEXIT_CRITICAL(&state_lock);
        post_control(&(control_command_t){.kind = COMMAND_START_SERVICE});
        break;
    case ESP_GATTS_START_EVT: {
        portENTER_CRITICAL(&state_lock);
        bool expected_service = parameter->start.service_handle == state.handles[ATTR_SERVICE];
        portEXIT_CRITICAL(&state_lock);
        if (parameter->start.status != ESP_GATT_OK || !expected_service) {
            fail_transport("HID service start", ESP_FAIL);
            break;
        }
        portENTER_CRITICAL(&state_lock);
        state.service_started = true;
        refresh_locked();
        portEXIT_CRITICAL(&state_lock);
        break;
    }
    case ESP_GATTS_CONNECT_EVT: {
        control_command_t command = {.kind = COMMAND_ENCRYPT,
                                    .conn_id = parameter->connect.conn_id};
        memcpy(command.address, parameter->connect.remote_bda, ESP_BD_ADDR_LEN);
        TickType_t now = xTaskGetTickCount();
        portENTER_CRITICAL(&state_lock);
        bool saved = state.bonds_loaded && saved_peer_locked(command.address);
        bool pairing = pairing_allowed_locked(now);
        if (state.adv_stop_pending) state.adv_stopped_by_connect = true;
        state.advertising = false; /* Legacy connectable advertising stops on connect. */
        if (state.public.connected || state.rejected_pending || state.public.failed ||
            state.public.bond_forget_state == BLE_HID_FORGET_RUNNING ||
            state.notification_pending || state.submitting || !state.bonds_loaded ||
            (!saved && !pairing)) {
            command.kind = COMMAND_REJECT_CONNECTION;
            state.rejected_pending = true;
            state.rejected_conn_id = command.conn_id;
            ++state.rejected_generation;
            if (state.rejected_generation == 0) ++state.rejected_generation;
            state.rejected_started = now;
            command.generation = state.rejected_generation;
            memcpy(state.rejected_address, command.address, sizeof(state.rejected_address));
        } else {
            reset_link_locked();
            state.conn_id = command.conn_id;
            memcpy(state.address, command.address, ESP_BD_ADDR_LEN);
            state.public.connected = true;
            state.public.peer_known = true;
            memcpy(state.public.peer_address, command.address, ESP_BD_ADDR_LEN);
            state.peer_address_type_known = (unsigned int)parameter->connect.ble_addr_type <= 3U;
            if (state.peer_address_type_known) {
                state.peer_address_type = (uint8_t)parameter->connect.ble_addr_type;
            }
            state.peer_saved = saved;
            state.pairing_admission_pending = pairing;
            state.handshake_started = now;
            state.connection_generation = state.public.generation;
            command.generation = state.connection_generation;
        }
        refresh_locked();
        portEXIT_CRITICAL(&state_lock);
        post_control(&command);
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
        portENTER_CRITICAL(&state_lock);
        if (state.rejected_pending && state.rejected_conn_id == parameter->disconnect.conn_id &&
            memcmp(state.rejected_address, parameter->disconnect.remote_bda, ESP_BD_ADDR_LEN) == 0) {
            state.rejected_pending = false;
        }
        if (state.public.connected && state.conn_id == parameter->disconnect.conn_id &&
            memcmp(state.address, parameter->disconnect.remote_bda, ESP_BD_ADDR_LEN) == 0) {
            reset_link_locked();
        }
        refresh_locked();
        portEXIT_CRITICAL(&state_lock);
        break;
    case ESP_GATTS_CONGEST_EVT:
        portENTER_CRITICAL(&state_lock);
        if (state.public.connected && state.conn_id == parameter->congest.conn_id) {
            state.link_congested = parameter->congest.congested;
            refresh_locked();
        }
        portEXIT_CRITICAL(&state_lock);
        break;
    case ESP_GATTS_CONF_EVT:
        portENTER_CRITICAL(&state_lock);
        if (state.notification_pending &&
            state.notification_conn_id == parameter->conf.conn_id &&
            state.notification_handle == parameter->conf.handle) {
            if (parameter->conf.status != ESP_GATT_OK && state.public.connected &&
                state.public.generation == state.notification_generation) {
                state.link_fault = true;
                advance_generation_locked();
            }
            state.notification_pending = false;
            refresh_locked();
        }
        portEXIT_CRITICAL(&state_lock);
        break;
    case ESP_GATTS_READ_EVT:
        handle_read(parameter);
        break;
    case ESP_GATTS_WRITE_EVT:
        handle_write(parameter);
        break;
    case ESP_GATTS_EXEC_WRITE_EVT: {
        /* No prepare buffer exists and no queued writes can be committed. */
        control_command_t command = {.kind = COMMAND_RESPONSE,
            .conn_id = parameter->exec_write.conn_id,
            .transaction = parameter->exec_write.trans_id,
            .status = parameter->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_CANCEL ?
                ESP_GATT_OK : ESP_GATT_REQ_NOT_SUPPORTED};
        memcpy(command.address, parameter->exec_write.bda, ESP_BD_ADDR_LEN);
        portENTER_CRITICAL(&state_lock);
        command.generation = state.connection_generation;
        command.auth_revision = state.auth_revision;
        portEXIT_CRITICAL(&state_lock);
        post_control(&command);
        break;
    }
    case ESP_GATTS_STOP_EVT:
    case ESP_GATTS_UNREG_EVT:
        fail_transport("HID service stopped", ESP_ERR_INVALID_STATE);
        break;
    default:
        break;
    }
}

ble_hid_snapshot_t ble_hid_snapshot(void)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    ble_hid_snapshot_t snapshot = state.public;
    snapshot.bond_capacity = PAIRING_BOND_CAPACITY;
    snapshot.pairing_open = pairing_allowed_locked(now);
    snapshot.pairing_remaining_ms = snapshot.pairing_open ?
        (uint32_t)pdTICKS_TO_MS(state.pairing_duration - (TickType_t)(now - state.pairing_started)) : 0;
    snapshot.rssi_age_ms = snapshot.rssi_valid ?
        (uint32_t)pdTICKS_TO_MS((TickType_t)(now - state.rssi_sampled)) : 0;
    if (snapshot.rssi_valid && snapshot.rssi_age_ms > RSSI_MAX_AGE_MS) {
        snapshot.rssi_valid = false;
        snapshot.rssi_dbm = 0;
        snapshot.rssi_error = ESP_ERR_TIMEOUT;
    }
    portEXIT_CRITICAL(&state_lock);
    return snapshot;
}

ble_hid_bond_list_t ble_hid_bond_list(void)
{
    portENTER_CRITICAL(&state_lock);
    ble_hid_bond_list_t list = state.bond_list;
    list.capacity = PAIRING_BOND_CAPACITY;
    /* Matching requires successful authentication and resolution of this
     * connection's actual bond, not just an address seen during admission.
     * Do not guess when a private/pseudo address or type cannot be matched. */
    bool verified = list.known && !list.updating && state.public.connected &&
        state.public.encrypted && !state.public.failed && !state.link_fault &&
        state.auth_complete && state.bonded && state.bond_resolved &&
        state.peer_address_type_known;
    unsigned int matches = 0;
    unsigned int matched = 0;
    list.current_match_known = false;
    for (unsigned int i = 0; i < list.count; ++i) {
        ble_hid_bond_t *bond = &list.bonds[i];
        bond->is_current = false;
        if (verified && bond->address_type_known &&
            bond->address_type == state.peer_address_type &&
            memcmp(bond->address, state.address, ESP_BD_ADDR_LEN) == 0) {
            ++matches;
            matched = i;
        }
    }
    if (matches == 1) {
        list.current_match_known = true;
        list.bonds[matched].is_current = true;
    }
    portEXIT_CRITICAL(&state_lock);
    return list;
}

bool ble_hid_pairing_open(uint32_t duration_ms)
{
    if (duration_ms == 0 || duration_ms > BLE_HID_PAIRING_MAX_MS) return false;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    bool accepted = state.public.initialized && !state.public.failed &&
        !state.public.connected && !state.rejected_pending &&
        state.bond_list.known && state.bond_list.count < PAIRING_BOND_CAPACITY &&
        state.public.bond_forget_state != BLE_HID_FORGET_RUNNING;
    if (accepted) {
        state.pairing_user_controlled = true;
        open_pairing_locked(now, duration_ms);
    }
    portEXIT_CRITICAL(&state_lock);
    return accepted;
}

void ble_hid_pairing_close(void)
{
    portENTER_CRITICAL(&state_lock);
    state.pairing_user_controlled = true;
    state.public.pairing_open = false;
    portEXIT_CRITICAL(&state_lock);
}

bool ble_hid_forget_bonds(uint64_t generation)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    bool accepted = state.public.initialized && !state.public.failed &&
        state.public.generation == generation && !state.public.connected &&
        !state.rejected_pending && !state.notification_pending && !state.submitting &&
        !state.remove_inflight && state.public.bond_forget_state != BLE_HID_FORGET_RUNNING;
    if (accepted) {
        state.public.pairing_open = false;
        state.pairing_user_controlled = true;
        state.public.bond_forget_state = BLE_HID_FORGET_RUNNING;
        state.public.bond_forget_error = ESP_OK;
        bond_list_status_locked(false, true, ESP_OK);
        advance_generation_locked();
        state.forget_generation = state.public.generation;
        state.forget_started = now;
        state.forget_previous_count = SUBSCRIPTION_CAPACITY + 1U;
        state.remove_result = ESP_OK;
        state.remove_completed = false;
        refresh_locked();
    }
    portEXIT_CRITICAL(&state_lock);
    return accepted;
}

bool ble_hid_send_generation(uint64_t generation, uint8_t report_id,
                             const uint8_t *data, size_t length)
{
    unsigned int index;
    if (report_id == HID_ENGINE_KEYBOARD_REPORT_ID &&
        length == HID_ENGINE_KEYBOARD_REPORT_LENGTH) {
        index = ATTR_KEYBOARD;
    } else if (report_id == HID_ENGINE_MOUSE_REPORT_ID &&
               length == HID_ENGINE_MOUSE_REPORT_LENGTH) {
        index = ATTR_MOUSE;
    } else {
        return false;
    }
    if (!data) {
        return false;
    }
    TickType_t started = xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    if (state.public.generation != generation || !state.public.ready ||
        state.public.congested) {
        portEXIT_CRITICAL(&state_lock);
        return false;
    }
    uint16_t conn_id = state.conn_id;
    uint16_t handle = state.handles[index];
    esp_gatt_if_t interface = state.interface;
    state.submitting = true;
    state.notification_pending = true;
    state.notification_timed_out = false;
    state.notification_started = started;
    state.notification_generation = generation;
    state.notification_conn_id = conn_id;
    state.notification_handle = handle;
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);

    /* Reservation blocks further sends and reconnect advertising until this
     * request has both returned and drained through the pinned SDK's CONF_EVT.
     * No SDK operation runs with interrupts disabled or under a callback lock. */
    esp_err_t error = esp_ble_gatts_send_indicate(interface, conn_id, handle,
        (uint16_t)length, (uint8_t *)data, false);

    portENTER_CRITICAL(&state_lock);
    state.submitting = false;
    if (error != ESP_OK) {
        state.notification_pending = false;
    } else if (state.public.connected && state.public.generation == generation) {
        memcpy(index == ATTR_KEYBOARD ? state.keyboard : state.mouse, data, length);
    }
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);
    return error == ESP_OK;
}

bool ble_hid_send(uint8_t report_id, const uint8_t *data, size_t length)
{
    return ble_hid_send_generation(ble_hid_snapshot().generation, report_id, data, length);
}

bool ble_hid_disconnect_generation(uint64_t generation)
{
    portENTER_CRITICAL(&state_lock);
    if (!state.public.connected || state.public.generation != generation) {
        portEXIT_CRITICAL(&state_lock);
        return false;
    }
    state.link_fault = true;
    advance_generation_locked();
    refresh_locked();
    portEXIT_CRITICAL(&state_lock);
    /* The worker observes the flag within its bounded poll interval. No SDK
     * call, queue space or Bluetooth callback is needed to close the gate. */
    return true;
}

esp_err_t ble_hid_init(void)
{
    portENTER_CRITICAL(&state_lock);
    bool attempted = state.init_attempted;
    state.init_attempted = true;
    portEXIT_CRITICAL(&state_lock);
    if (attempted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hid_engine_report_descriptor_length == 0 ||
        hid_engine_report_descriptor_length > ESP_GATT_MAX_ATTR_LEN) {
        fail_transport("Report Map length", ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    attributes[ATTR_MAP].att_desc.max_length = (uint16_t)hid_engine_report_descriptor_length;
    attributes[ATTR_MAP].att_desc.length = (uint16_t)hid_engine_report_descriptor_length;
    attributes[ATTR_MAP].att_desc.value = (uint8_t *)hid_engine_report_descriptor;
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        fail_transport("NVS initialization (data preserved)", error);
        return error;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE ||
        esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        fail_transport("Bluetooth already owned", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_command_t));
    if (!control_queue) {
        fail_transport("Control queue allocation", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    esp_bt_controller_config_t configuration = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    error = esp_bt_controller_init(&configuration);
    if (error == ESP_OK) {
        error = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    }
    if (error == ESP_OK) {
        error = esp_bluedroid_init();
    }
    if (error == ESP_OK) {
        error = esp_bluedroid_enable();
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_register_callback(gap_callback);
    }
    if (error == ESP_OK) {
        error = esp_ble_gatts_register_callback(gatts_callback);
    }
    uint8_t authentication = ESP_LE_AUTH_REQ_SC_BOND;
    uint8_t specified_authentication = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    uint8_t capability = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    if (error == ESP_OK) {
        error = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                              &authentication, sizeof(authentication));
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
            &specified_authentication, sizeof(specified_authentication));
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                              &capability, sizeof(capability));
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                              &key_size, sizeof(key_size));
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_set_device_name("ESP32-INPUT");
    }
    if (error == ESP_OK && xTaskCreate(control_worker, "ble_hid_control",
        CONTROL_TASK_STACK, NULL, CONTROL_TASK_PRIORITY, NULL) != pdPASS) {
        error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) {
        TimerHandle_t timer = xTimerCreate("ble_hid_deadline",
            pdMS_TO_TICKS(CONTROL_POLL_MS), pdTRUE, NULL, notification_watchdog);
        if (!timer || xTimerStart(timer, 0) != pdPASS) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_config_adv_data(&advertising_data);
    }
    if (error == ESP_OK) {
        error = esp_ble_gap_config_adv_data(&scan_data);
    }
    if (error == ESP_OK) {
        error = esp_ble_gatts_app_register(HID_APP_ID);
    }
    if (error != ESP_OK) {
        fail_transport("BLE initialization", error);
        return error;
    }
    portENTER_CRITICAL(&state_lock);
    state.public.initialized = true;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}
