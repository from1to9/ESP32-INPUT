#include "passport_ble.h"
#include "passport_store.h"
#include "passport_names_owner.h"
#include "passport_control.h"
#include "passport_control_http.h"
#include "passport_management.h"
#include "passport_wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_engine.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define WINDOW_MS 60000
#define RELEASE_MS 300
#define LEASE_MS 3000
static QueueHandle_t commands;
static struct ble_npl_callout tick;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static passport_ble_state_t published, state;
static atomic_bool emergency_lock;
static hid_engine_t engine;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE, keyboard_handle, mouse_handle;
// The pinned host can restore CCCDs before its delayed CONNECT callback.
// Keep those observations separate from admitted connection state.
static struct {
    uint16_t connection;
    bool keyboard, mouse;
} early_subscriptions = { .connection = BLE_HS_CONN_HANDLE_NONE };
static uint8_t keyboard[8], mouse[4], leds;
static bool suspended, new_connection, selected_valid, transition, disconnect_requested, closing;
static bool startup_restore;
static uint64_t reconnect_end, reconnect_next;
static unsigned reconnect_attempts;
static ble_addr_t selected;
static passport_command_t pending;
static uint64_t window_end, release_end, lease_end;
static bool link_ready;
static bool pairing_failed;
static uint32_t peer_security_revision;
static bool names_reconciled, bond_ids_known;
static passport_wifi_state_t network;
static passport_control_observation_t control_state;
static struct {
    uint64_t key, deadline;
    cp_operation_id_t operation;
    cp_error_t error;
    bool done, started, uncertain, wrong_host;
    char target[PASSPORT_PEER_TEXT];
} remote_management;
static void remote_management_cancel(cp_error_t error);
static bool physical_inputs_available;
static struct {
    passport_command_t request;
    uint64_t deadline, epoch;
    bool active, uncertain;
} usb_command;
static void usb_command_finish(passport_result_t result, int32_t detail)
{
    if (!usb_command.active) return;
    state.command = (passport_receipt_t){usb_command.request.command_id, result, detail};
    usb_command.active = false;
}


static uint64_t now_ms(void) { return esp_timer_get_time() / 1000; }
#if CONFIG_PASSPORT_DIAGNOSTICS
static uint64_t diagnostic_until;
static uint32_t diagnostic_flags = UINT32_MAX;
static bool diagnostic_active(void) { return now_ms() < diagnostic_until; }
#endif
static void diagnostic(const char *event, int status)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    if (diagnostic_active()) {
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        ESP_LOGI("passport_ble_diag", "event=%s t=%llu status=%d connected=%u encrypted=%u bonded=%u storage=%u peers=%u pairing=%u handshake=%u closing=%u window=%u keyboard=%u mouse=%u ready=%u adv_active=%u control=%u heap=%zu largest=%zu",
                 event, (unsigned long long)now_ms(), status, state.connected, state.encrypted,
                 state.bonded, state.storage_known, state.peer_count, state.pairing, new_connection, closing, window_end > now_ms(),
                 state.keyboard_subscribed, state.mouse_subscribed, state.ready,
                 (unsigned)ble_gap_adv_active(), state.network_available,
                 heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps));
    }
#else
    (void)event; (void)status;
#endif
}
static void result(const char *text)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    if (diagnostic_active() && strcmp(state.result, text)) {
        // Callers supply fixed outcome text, never peer identities or key data.
        ESP_LOGI("passport_ble_diag", "t=%llu result=%s", (unsigned long long)now_ms(), text);
    }
#endif
    snprintf(state.result, sizeof(state.result), "%s", text);
}
static bool same(const ble_addr_t *a, const ble_addr_t *b) { return ble_addr_cmp(a, b) == 0; }
static bool stable_identity(const ble_addr_t *peer)
{
    return peer->type == BLE_ADDR_PUBLIC ||
        (peer->type == BLE_ADDR_RANDOM && (peer->val[5] & 0xc0) == 0xc0);
}
static int gap_event(struct ble_gap_event *event, void *arg);

static int disconnect_link(void)
{
    closing = true;
    if (disconnect_requested) return 0;
    int rc = ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
    if (!rc) disconnect_requested = true;
    return rc;
}


static void format_peer(char *text, size_t size, const ble_addr_t *peer)
{
    snprintf(text, size, "%02X:%02X:%02X:%02X:%02X:%02X/%u",
             peer->val[5], peer->val[4], peer->val[3], peer->val[2], peer->val[1], peer->val[0], peer->type);
}

static bool is_saved(const ble_addr_t *peer)
{
    union ble_store_key key = { .sec = { .peer_addr = *peer } };
    union ble_store_value value;
    int rc = ble_store_read_our_sec(&key.sec, &value.sec);
    bool saved = rc == 0 && value.sec.ltk_present;
    explicit_bzero(&value, sizeof(value));
    return saved;
}

static bool bond_id(const ble_addr_t *peer, uint8_t id[PASSPORT_BOND_ID_BYTES])
{
    union ble_store_key key = {.sec = {.peer_addr = *peer}};
    union ble_store_value value;
    bool known = false;
    if (ble_store_read_our_sec(&key.sec, &value.sec) == 0 && value.sec.ltk_present) {
        // Explicit byte order and domain version. Only immutable pairing fields
        // are included. NimBLE refresh counters and CCCDs do not affect identity.
        uint8_t material[36] = {1, peer->type};
        memcpy(material + 2, peer->val, sizeof(peer->val));
        material[8] = value.sec.key_size;
        material[9] = value.sec.sc;
        material[10] = value.sec.ediv;
        material[11] = value.sec.ediv >> 8;
        for (unsigned i = 0; i < 8; ++i) material[12 + i] = value.sec.rand_num >> (i * 8);
        memcpy(material + 20, value.sec.ltk, sizeof(value.sec.ltk));
        known = passport_names_bond_id(material, sizeof(material), id);
        explicit_bzero(material, sizeof(material));
    }
    explicit_bzero(&value, sizeof(value));
    return known;
}

static void refresh_peers(void)
{
    passport_peer_t previous[PASSPORT_PEERS];
    memcpy(previous, state.peers, sizeof(previous));
    unsigned previous_count = state.peer_count;
    bool previously_known = state.storage_known;
    ble_addr_t peers[PASSPORT_PEERS];
    int count = 0;
    state.storage_known = passport_store_known();
    if (!state.storage_known || ble_store_util_bonded_peers(peers, &count, PASSPORT_PEERS)) {
        bond_ids_known = false;
        state.storage_known = false;
        state.peer_count = 0;
        if (previously_known || previous_count) ++state.bond_revision;
        return;
    }
    state.peer_count = count;
    const uint32_t revision = passport_store_security_revision();
    const bool reuse = previously_known && peer_security_revision == revision;
    if (!reuse) bond_ids_known = true;
    peer_security_revision = revision;
    for (int i = 0; i < count; ++i) {
        if (!stable_identity(&peers[i])) {
            bond_ids_known = false;
            state.storage_known = false;
            state.peer_count = 0;
            ++state.bond_revision;
            return;
        }
        state.peers[i].type = peers[i].type;
        memcpy(state.peers[i].address, peers[i].val, 6);
        format_peer(state.peers[i].label, sizeof(state.peers[i].label), &peers[i]);
        if (reuse && (unsigned)i < previous_count && previous[i].type == peers[i].type &&
            !memcmp(previous[i].address, peers[i].val, sizeof(peers[i].val))) {
            memcpy(state.peers[i].bond_id, previous[i].bond_id, PASSPORT_BOND_ID_BYTES);
        } else {
            memset(state.peers[i].bond_id, 0, PASSPORT_BOND_ID_BYTES);
            if (!bond_id(&peers[i], state.peers[i].bond_id)) bond_ids_known = false;
        }
    }
    if (!previously_known || previous_count != state.peer_count ||
        memcmp(previous, state.peers, state.peer_count * sizeof(previous[0]))) ++state.bond_revision;
}

static bool bond_retained(const passport_name_key_t *key)
{
    for (unsigned i = 0; i < state.peer_count; ++i) {
        if (!memcmp(key->bond, state.peers[i].bond_id, sizeof(key->bond))) return true;
    }
    return false;
}

static passport_name_result_t remove_bond_name(const ble_addr_t *peer)
{
    for (unsigned i = 0; i < state.peer_count; ++i) {
        const passport_peer_t *record = &state.peers[i];
        if (record->type != peer->type || memcmp(record->address, peer->val, sizeof(peer->val))) continue;
        passport_name_key_t key = {.kind = PN_HID_BOND};
        memcpy(key.bond, record->bond_id, sizeof(key.bond));
        return bond_ids_known ? passport_names_remove(&key) : PN_UNAVAILABLE;
    }
    return PN_UNAVAILABLE;
}

static void service_names(void)
{
    if (!names_reconciled && state.storage_known && bond_ids_known) {
        names_reconciled = true;
        if (passport_names_prune(PN_BLE, bond_retained) != PN_OK) result("Host label storage unavailable");
    }
    passport_name_change_t change;
    if (!passport_names_take(PN_BLE, &change)) return;
    passport_name_result_t outcome = !state.synced || !state.storage_known || !bond_ids_known ? PN_UNAVAILABLE :
        engine.state.locked ? PN_LOCKED : !bond_retained(&change.key) ? PN_NOT_FOUND : passport_names_apply(&change);
    passport_names_finish(PN_BLE, outcome);
}

static int access_hid(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    (void)attr;
    const uintptr_t item = (uintptr_t)arg;
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR || ctx->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        static const uint8_t info[] = {0x11, 0x01, 0, 0x02};
        static const uint8_t neutral[8];
        static const uint8_t refs[][2] = {{1, 1}, {2, 1}, {1, 2}};
        const void *data = NULL;
        size_t length = 0;
        switch (item) {
        case 1: data = hid_engine_report_descriptor; length = hid_engine_report_descriptor_length; break;
        case 2: data = info; length = sizeof(info); break;
        case 3: data = engine.state.release_pending ? neutral : keyboard; length = sizeof(keyboard); break;
        case 4: data = engine.state.release_pending ? neutral : mouse; length = sizeof(mouse); break;
        case 5: data = &leds; length = 1; break;
        case 6: data = refs[0]; length = 2; break;
        case 7: data = refs[1]; length = 2; break;
        case 8: data = refs[2]; length = 2; break;
        default: return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        return os_mbuf_append(ctx->om, data, length) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR && conn == connection) {
        uint8_t value;
        if (OS_MBUF_PKTLEN(ctx->om) != 1 || os_mbuf_copydata(ctx->om, 0, 1, &value)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (item == 5 && !(value & ~0x1f)) { leds = value; return 0; }
        if (item == 9 && value <= 1) {
            suspended = value == 0;
            hid_engine_cancel(&engine);
            ++state.generation;
            return 0;
        }
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

#define UUID(value) BLE_UUID16_DECLARE(value)
static struct ble_gatt_dsc_def keyboard_ref[] = {
    { .uuid = UUID(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = access_hid, .arg = (void *)6 }, {0}
};
static struct ble_gatt_dsc_def mouse_ref[] = {
    { .uuid = UUID(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = access_hid, .arg = (void *)7 }, {0}
};
static struct ble_gatt_dsc_def led_ref[] = {
    { .uuid = UUID(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = access_hid, .arg = (void *)8 }, {0}
};
static const struct ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = UUID(0x1812), .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = UUID(0x2a4b), .access_cb = access_hid, .arg = (void *)1, .flags = BLE_GATT_CHR_F_READ },
        { .uuid = UUID(0x2a4a), .access_cb = access_hid, .arg = (void *)2, .flags = BLE_GATT_CHR_F_READ },
        { .uuid = UUID(0x2a4d), .access_cb = access_hid, .arg = (void *)3,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
          .val_handle = &keyboard_handle, .descriptors = keyboard_ref },
        { .uuid = UUID(0x2a4d), .access_cb = access_hid, .arg = (void *)4,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
          .val_handle = &mouse_handle, .descriptors = mouse_ref },
        { .uuid = UUID(0x2a4d), .access_cb = access_hid, .arg = (void *)5,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                   BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC, .descriptors = led_ref },
        { .uuid = UUID(0x2a4c), .access_cb = access_hid, .arg = (void *)9,
          .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC }, {0}
    }}, {0}
};

static int store_result(int type, const union ble_store_value *value, int status)
{
#if CONFIG_PASSPORT_DIAGNOSTICS
    if (diagnostic_active()) {
        const bool security = type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC;
        ESP_LOGI("passport_ble_diag", "t=%llu store_type=%d status=%d identity_type=%d random_class=%d stable=%u",
                 (unsigned long long)now_ms(), type, status, security ? value->sec.peer_addr.type : -1,
                 security && value->sec.peer_addr.type == BLE_ADDR_RANDOM ? value->sec.peer_addr.val[5] >> 6 : -1,
                 security && stable_identity(&value->sec.peer_addr));
        diagnostic("store", status);
    }
#else
    (void)type; (void)value;
#endif
    return status;
}

static int secure_write(int type, const union ble_store_value *value)
{
    if (type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        if (!new_connection || closing || !state.pairing || now_ms() >= window_end) {
            pairing_failed = true;
            return store_result(type, value, BLE_HS_EREJECT);
        }
        if (!stable_identity(&value->sec.peer_addr)) {
            pairing_failed = true;
            result("Stable peer identity required");
            return store_result(type, value, BLE_HS_EREJECT);
        }
        // Identity exchange can reveal an existing bond behind an unrecognized RPA.
        // The cached list predates this synchronous SDK key-persistence callback.
        for (unsigned i = 0; i < state.peer_count; ++i) {
            if (state.peers[i].type == value->sec.peer_addr.type &&
                !memcmp(state.peers[i].address, value->sec.peer_addr.val, 6)) {
                pairing_failed = true;
                result("Forget this bond before re-pairing");
                return store_result(type, value, BLE_HS_EREJECT);
            }
        }
    }
    int rc = passport_store_write(type, value);
    if (rc && new_connection) pairing_failed = true;
    return store_result(type, value, rc);
}

static int store_full(struct ble_store_status_event *event, void *arg)
{
    (void)event; (void)arg;
    result("Bond store full; forget a host");
    return BLE_HS_ESTORE_CAP;
}

static bool stop_advertising(void)
{
    early_subscriptions.connection = BLE_HS_CONN_HANDLE_NONE;
    int rc = ble_gap_adv_stop();
    if (rc && rc != BLE_HS_EALREADY) result("Advertising stop failed");
    else state.advertising = false;
    // Always close admission, even if the controller could not stop advertising.
    state.pairing = false;
    window_end = 0;
    return !rc || rc == BLE_HS_EALREADY;
}

static void close_pairing(void)
{
    if (transition && pending.kind == PASSPORT_PAIR) transition = false;
    if (state.pairing || new_connection) {
        stop_advertising();
        if (new_connection && connection != BLE_HS_CONN_HANDLE_NONE) disconnect_link();
    }
}

static void advertise(bool pairing, uint32_t duration)
{
    if (!duration) duration=WINDOW_MS;
#if CONFIG_PASSPORT_DIAGNOSTICS
    diagnostic_until = now_ms() + duration + 10000;
    diagnostic_flags = UINT32_MAX;
#endif
    diagnostic("advertise_request", 0);
    refresh_peers();
    if (!state.synced || !state.storage_known || connection != BLE_HS_CONN_HANDLE_NONE) {
        result("Bluetooth unavailable");
        return;
    }
    if (pairing && state.peer_count == PASSPORT_PEERS) { result("Bond store full; forget a host"); return; }
    if (!pairing && (!selected_valid || !is_saved(&selected))) { result("Selected bond unavailable"); return; }
    if (!stop_advertising()) return;
    ble_uuid16_t hid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (uint8_t *)"Passport Input", .name_len = 14, .name_is_complete = 1,
        .uuids16 = &hid, .num_uuids16 = 1, .uuids16_is_complete = 1,
        .appearance = 0x03c0, .appearance_is_present = 1
    };
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN
    };
    int rc = ble_gap_adv_set_fields(&fields);
    if (!rc && !pairing) {
        rc = ble_gap_wl_set(&selected, 1);
        params.filter_policy = BLE_HCI_ADV_FILT_CONN;
    }
    if (!rc) rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, duration, &params, gap_event, NULL);
    diagnostic("advertise_start", rc);
    if (rc) { result("Advertising failed"); return; }
    state.advertising = true;
    state.pairing = pairing;
    window_end = now_ms() + duration;
    result(pairing ? "Pair in host Bluetooth settings" : "Waiting for selected host");
}

static bool send_report(void *arg, uint8_t id, const uint8_t *data, size_t length)
{
    (void)arg;
    if (engine.state.owner==INPUT_REMOTE && !engine.state.release_pending && !passport_control_report_authorized(now_ms())) return false;
    struct os_mbuf *buffer = ble_hs_mbuf_from_flat(data, length);
    if (!buffer) return false;
    uint16_t handle = id == HID_ENGINE_KEYBOARD_REPORT_ID ? keyboard_handle : mouse_handle;
    int rc = ble_gatts_notify_custom(connection, handle, buffer);
    if (!rc) memcpy(id == HID_ENGINE_KEYBOARD_REPORT_ID ? keyboard : mouse, data, length);
    return rc == 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        {
        diagnostic("connect", event->connect.status);
        bool window_open = state.advertising && window_end > now_ms() && state.storage_known;
        state.advertising = false;
        if (event->connect.status) {
            early_subscriptions.connection = BLE_HS_CONN_HANDLE_NONE;
            reconnect_next = now_ms() + 1000;
            state.pairing = false;
            window_end = 0;
            result("Connection failed");
            return 0;
        }
        connection = event->connect.conn_handle;
        pairing_failed = false;
        ++state.generation;
        state.connected = true;
        if (!window_open) { result("Connection outside window"); disconnect_link(); return 0; }
        if (ble_gap_conn_find(connection, &desc)) { result("Peer identity unknown"); disconnect_link(); return 0; }
        // An unbonded connection can still expose an RPA until identity exchange.
        snprintf(state.current, sizeof(state.current), "Identity pending");
        new_connection = state.pairing && now_ms() < window_end && !is_saved(&desc.peer_id_addr);
        if (!new_connection && (!selected_valid || !same(&selected, &desc.peer_id_addr) || !is_saved(&desc.peer_id_addr))) {
            result("Unexpected host rejected");
            if (remote_management.key) { remote_management.error=CP_STALE; remote_management.done=true; remote_management.wrong_host=true; }
            disconnect_link();
            return 0;
        }
        if (!new_connection) format_peer(state.current, sizeof(state.current), &desc.peer_id_addr);
        state.keyboard_subscribed = early_subscriptions.connection == connection && early_subscriptions.keyboard;
        state.mouse_subscribed = early_subscriptions.connection == connection && early_subscriptions.mouse;
        early_subscriptions.connection = BLE_HS_CONN_HANDLE_NONE;
        result(new_connection ? "Pairing" : "Connected; securing link");
        // Encryption can also precede CONNECT. A redundant peripheral security
        // request starts another 30-second procedure even on an encrypted link.
        if (!desc.sec_state.encrypted) {
            int rc = ble_gap_security_initiate(connection);
            diagnostic("security_request", rc);
            if (rc && rc != BLE_HS_EALREADY) {
                result("Security request failed");
                disconnect_link();
            }
        }
        return 0;
        }
    case BLE_GAP_EVENT_DISCONNECT:
        {
        const bool locally_closing = closing;
        diagnostic("disconnect", event->disconnect.reason);
        connection = BLE_HS_CONN_HANDLE_NONE;
        reconnect_next = now_ms() + 1000;
        ++state.generation;
        state.connected = state.encrypted = state.bonded = false;
        state.keyboard_subscribed = state.mouse_subscribed = false;
        state.ready = false;
        suspended = new_connection = closing = false;
        memset(keyboard, 0, sizeof(keyboard));
        memset(mouse, 0, sizeof(mouse));
        disconnect_requested = false;
        if (!transition || engine.state.release_pending || engine.count) {
            state.release_unknown = true; remote_management.uncertain=true; usb_command.uncertain=true;
        }
        hid_engine_set_link(&engine, false, false);
        link_ready = false;
        stop_advertising();
        if (!transition && !locally_closing) result("Disconnected");
        return 0;
        }
    case BLE_GAP_EVENT_ENC_CHANGE:
        diagnostic("encryption", event->enc_change.status);
        hid_engine_cancel(&engine);
        ++state.generation;
        if (event->enc_change.status) {
            result("Encryption failed");
            disconnect_link();
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == connection) {
            if (event->subscribe.attr_handle == keyboard_handle) state.keyboard_subscribed = event->subscribe.cur_notify;
            if (event->subscribe.attr_handle == mouse_handle) state.mouse_subscribed = event->subscribe.cur_notify;
            if (!event->subscribe.cur_notify) {
                hid_engine_set_link(&engine, true, false);
                link_ready = false;
                ++state.generation;
            }
        } else if (connection == BLE_HS_CONN_HANDLE_NONE && state.advertising &&
                   state.storage_known && window_end > now_ms() &&
                   event->subscribe.conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                   (event->subscribe.attr_handle == keyboard_handle || event->subscribe.attr_handle == mouse_handle)) {
            if (early_subscriptions.connection != event->subscribe.conn_handle) {
                early_subscriptions.connection = event->subscribe.conn_handle;
                early_subscriptions.keyboard = early_subscriptions.mouse = false;
            }
            if (event->subscribe.attr_handle == keyboard_handle) early_subscriptions.keyboard = event->subscribe.cur_notify;
            if (event->subscribe.attr_handle == mouse_handle) early_subscriptions.mouse = event->subscribe.cur_notify;
        }
        diagnostic("subscribe", 0);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        result("Forget this bond before re-pairing");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        result("Pairing method unavailable");
        disconnect_link();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        state.advertising = false;
        if (!state.connected) { state.pairing = false; window_end = 0; result("Advertising window ended"); }
        return 0;
    default: return 0;
    }
}

static void execute_management(void)
{
    ble_addr_t peer = { .type = pending.peer_type };
    memcpy(peer.val, pending.peer, 6);
    refresh_peers();
    if ((pending.kind == PASSPORT_ADVERTISE || pending.kind == PASSPORT_FORGET) &&
        (pending.bond_revision != state.bond_revision || !state.storage_known || !is_saved(&peer))) {
        result("Host list changed; select again");
        if (remote_management.key) { remote_management.error=CP_STALE; remote_management.done=true; }
        usb_command_finish(PR_STALE, 0);
        transition = false;
        return;
    }
    reconnect_end = 0; // Explicit host management replaces startup intent.
    switch (pending.kind) {
    case PASSPORT_PAIR: selected_valid = false; advertise(true,pending.window_ms); break;
    case PASSPORT_ADVERTISE:
        if (passport_store_select(&peer)) {
            result("Host selection not saved");
            if (remote_management.key) { remote_management.error=CP_UNAVAILABLE; remote_management.done=true; }
            usb_command_finish(PR_STORAGE_UNCERTAIN, 0);
            break;
        }
        selected = peer;
        selected_valid = true;
        advertise(false,pending.window_ms);
        break;
    case PASSPORT_FORGET:
        if (passport_store_forget(&peer)) {
            result("Forget uncertain; restart required");
            usb_command_finish(PR_STORAGE_UNCERTAIN, 0);
        }
        else {
            passport_name_result_t removed = remove_bond_name(&peer);
            result(removed == PN_OK ? "Bond forgotten" : "Bond forgotten; label cleanup failed");
            if (selected_valid && same(&selected, &peer)) selected_valid = false;
            refresh_peers();
            if (removed != PN_OK) usb_command_finish(PR_STORAGE_UNCERTAIN, removed);
        }
        break;
    default: result("Disconnected; bond retained"); break;
    }
    transition = false;
    disconnect_requested = false;
}

static passport_result_t command_impl(const passport_command_t *request, uint64_t now)
{
    remote_management_cancel(CP_STALE);
    if (request->kind == PASSPORT_LOCK) {
        if (reconnect_end) { reconnect_end = 0; stop_advertising(); }
        passport_control_local(now);
        hid_engine_set_locked(&engine, true);
        close_pairing();
        result("Input locked");
        return PR_PENDING;
    }
    // Closing a visible pairing window remains valid across its connect event.
    if (request->kind == PASSPORT_STOP_PAIR) {
        if (reconnect_end) { reconnect_end = 0; stop_advertising(); }
        close_pairing();
        if (request->command_id && state.advertising && !stop_advertising()) return PR_UNAVAILABLE;
        result("Pairing closed"); return PR_PENDING;
    }
    if (request->generation != state.generation) { result("Stale action rejected"); return PR_STALE; }
    if (request->kind == PASSPORT_UNLOCK) {
        if (request->command_id && (!physical_inputs_available || atomic_load(&emergency_lock))) return PR_LOCKED;
        hid_engine_set_locked(&engine, false); result("Unlocked"); return PR_APPLIED;
    }
    if (request->kind == PASSPORT_CANCEL) {
        if (request->network_change) passport_control_network_reset(now);
        else passport_control_local(now);
        hid_engine_cancel(&engine); lease_end = 0; ++state.cancel_revision; return PR_PENDING;
    }
    if (request->command_id && engine.state.locked) return PR_LOCKED;
    if (request->command_id && request->kind <= PASSPORT_FORGET && (!state.synced || !state.storage_known)) return PR_UNAVAILABLE;
    bool local_input=request->kind==PASSPORT_MOVE || request->kind==PASSPORT_CLICK || request->kind==PASSPORT_KEY || request->kind==PASSPORT_SCROLL;
    if (local_input && request->input_revision!=state.input_revision) { result("Fresh press required"); return PR_STALE; }
    if (request->command_id && local_input && engine.count) return PR_BUSY;
    if (request->kind==PASSPORT_TAKEOVER || (local_input && engine.state.owner==INPUT_REMOTE)) {
        passport_control_local(now);
        (void)hid_engine_set_mode(&engine,INPUT_MOUSE);
        ++state.input_revision; lease_end=now+LEASE_MS;
        result("Local control; press again"); return request->kind == PASSPORT_TAKEOVER ? PR_PENDING : PR_RELEASE_REQUIRED;
    }
    if (request->kind <= PASSPORT_FORGET) {
        if ((request->kind == PASSPORT_ADVERTISE || request->kind == PASSPORT_FORGET || request->kind == PASSPORT_DISCONNECT) &&
            request->bond_revision != state.bond_revision) { result("Host list changed; select again"); return PR_STALE; }
        if (request->kind == PASSPORT_DISCONNECT) {
            ble_addr_t peer = { .type = request->peer_type };
            memcpy(peer.val, request->peer, 6);
            if (!selected_valid || !same(&selected, &peer)) { result("Selected host is not active"); return PR_STALE; }
        }
        if (transition) { result("Host change in progress"); return PR_BUSY; }
        pending = *request;
        if (!stop_advertising()) return PR_UNAVAILABLE;
        passport_control_local(now);
        hid_engine_cancel(&engine);
        transition = true;
        release_end = now + RELEASE_MS;
        result("Releasing input");
        return PR_PENDING;
    }
    if (transition || !state.ready) { result("Input unavailable"); return PR_UNAVAILABLE; }
    if (local_input && engine.state.mode!=INPUT_MOUSE) {
        (void)hid_engine_set_mode(&engine,INPUT_MOUSE);
        ++state.input_revision; result("Mouse mode; press again"); return PR_RELEASE_REQUIRED;
    }
    hid_engine_enqueue_result_t rc = HID_ENGINE_ENQUEUE_INVALID;
    if (request->kind == PASSPORT_MOVE) rc = hid_engine_enqueue_mouse_move(&engine, engine.state.epoch, INPUT_LOCAL, request->x, request->y);
    if (request->kind == PASSPORT_CLICK) rc = hid_engine_enqueue_mouse_click(&engine, engine.state.epoch, INPUT_LOCAL, request->buttons ? request->buttons : 1);
    if (request->kind == PASSPORT_KEY) rc = hid_engine_enqueue_mouse_key(&engine, engine.state.epoch, INPUT_LOCAL, request->modifier, request->usage);
    if (request->kind == PASSPORT_SCROLL) rc = hid_engine_enqueue_mouse_scroll(&engine, engine.state.epoch, INPUT_LOCAL, request->y);
    if (rc == HID_ENGINE_ENQUEUE_OK) { lease_end = now + LEASE_MS; result("Input queued"); return PR_PENDING; }
    else { hid_engine_cancel(&engine); result("Input rejected; releasing"); }
    return rc == HID_ENGINE_ENQUEUE_INVALID ? PR_INVALID : PR_UNAVAILABLE;
}

#include "passport_ble_usb.inc"
#include "passport_ble_control.inc"

static void reconnect_startup(uint64_t now)
{
    if (!reconnect_end) return;
    if (now >= reconnect_end || state.ready || !state.storage_known ||
        !selected_valid || engine.state.locked) { reconnect_end = 0; return; }
    if (state.connected || state.advertising || transition || now < reconnect_next) return;
    if (reconnect_attempts == 3) { reconnect_end = 0; return; }
    ++reconnect_attempts;
    reconnect_next = now + 1000;
    advertise(false, (uint32_t)(reconnect_end - now));
}

static void service(struct ble_npl_event *event)
{
    (void)event;
    const uint64_t now = now_ms();
    refresh_peers();
    if (atomic_exchange(&emergency_lock, false)) {
        usb_command_finish(PR_LOCKED, 0);
        if (reconnect_end) { reconnect_end = 0; stop_advertising(); }
        remote_management_cancel(CP_LOCKED);
        passport_control_local(now);
        hid_engine_set_locked(&engine, true);
        close_pairing();
        xQueueReset(commands);
        // The UI also uses this priority path for inactivity and button faults.
        result("Input locked");
    }
    struct ble_gap_conn_desc desc;
    bool transport_ready = false;
    if (state.connected && !ble_gap_conn_find(connection, &desc)) {
        state.encrypted = desc.sec_state.encrypted;
        state.bonded = desc.sec_state.bonded && !(new_connection && pairing_failed) && is_saved(&desc.peer_id_addr);
        if (new_connection && pairing_failed && !closing) {
            result("Bond admission failed");
            disconnect_link();
        }
        if (new_connection && !pairing_failed && state.bonded && state.encrypted && !closing &&
            state.pairing && now < window_end) {
            if (passport_store_select(&desc.peer_id_addr)) {
                pairing_failed = true;
                result("Host selection not saved");
                disconnect_link();
            } else {
                selected = desc.peer_id_addr;
                selected_valid = true;
                new_connection = false;
                state.pairing = false;
                window_end = 0;
                format_peer(state.current, sizeof(state.current), &selected);
                result("New host saved");
            }
        }
        transport_ready = selected_valid && same(&selected, &desc.peer_id_addr) &&
            state.encrypted && state.bonded && state.keyboard_subscribed && state.mouse_subscribed &&
            state.storage_known && !suspended && !closing;
    }
    if (window_end && now >= window_end) {
        stop_advertising();
        if (new_connection && connection != BLE_HS_CONN_HANDLE_NONE) disconnect_link();
        result("Window expired");
    }
    if (!state.storage_known && connection != BLE_HS_CONN_HANDLE_NONE) disconnect_link();
    if (transport_ready != link_ready || engine.state.connected != state.connected) {
        hid_engine_set_link(&engine, state.connected, transport_ready);
        link_ready = transport_ready;
    }
    hid_engine_observe_transport(&engine, now, false);
    if (lease_end && now >= lease_end) { hid_engine_cancel(&engine); lease_end = 0; }
    state.ready = transport_ready && !engine.state.release_pending && !engine.state.locked && !transition;
    passport_wifi_snapshot(&network);
    control_state=(passport_control_observation_t){.transport_generation=state.generation,
        .network_generation=network.transport_generation,.local_busy=lease_end>now,.transition=transition};
    snprintf(control_state.target,sizeof(control_state.target),"%s",state.current);
    if (network.phase==PW_ONLINE) memcpy(control_state.sta_ip,network.sta_ip,sizeof(control_state.sta_ip));
    if (network.ap_started && network.dhcp_running) memcpy(control_state.ap_ip,network.ap_ip,sizeof(control_state.ap_ip));
    passport_control_observe(&control_state,now);
    passport_command_t request;
    for (unsigned i = 0; i < 4 && xQueueReceive(commands, &request, 0) == pdTRUE; ++i) command(&request, now);
    control_state.local_busy=lease_end>now; control_state.transition=transition;
    passport_control_observe(&control_state,now);
    passport_control_http_service(now);
    hid_engine_step_result_t step = hid_engine_step(&engine, send_report, NULL);
    passport_control_service(now);
    if (step == HID_ENGINE_STEP_RETRY_EXHAUSTED) {
        usb_command_finish(PR_UNKNOWN, 0);
        state.release_unknown = true;
        result("Release unknown; disconnecting");
        disconnect_link();
    }
    if (transition) {
        if (connection == BLE_HS_CONN_HANDLE_NONE) execute_management();
        else if (!disconnect_requested && (!engine.state.release_pending || now >= release_end)) {
            if (engine.state.release_pending) { state.release_unknown = true; remote_management.uncertain=true; usb_command.uncertain=true; }
            int rc = disconnect_link();
            if (rc) { result("Disconnect failed"); transition = false;
                usb_command_finish(PR_UNAVAILABLE, rc);
                if (remote_management.key) { remote_management.error=CP_UNAVAILABLE; remote_management.done=true; }
            }
            else disconnect_requested = true;
        }
    }
    service_names();
    state.locked = engine.state.locked;
    state.release_pending = engine.state.release_pending;
    state.ready = transport_ready && engine.ready && !engine.state.release_pending && !state.locked && !transition;
    if (state.ready) window_end = 0;
    reconnect_startup(now);
    state.updating=transition; state.handshake=new_connection;
    state.selected_id[0]=0;
    for (unsigned i=0;selected_valid && i<state.peer_count;++i) {
        if (state.peers[i].type!=selected.type || memcmp(state.peers[i].address,selected.val,6)) continue;
        for (unsigned j=0;j<16;++j) snprintf(state.selected_id+j*2,3,"%02x",state.peers[i].bond_id[j]);
    }
    state.window_seconds = window_end > now ? (window_end - now + 999) / 1000 : 0;
#if CONFIG_PASSPORT_DIAGNOSTICS
    const uint32_t flags = state.connected | (state.encrypted << 1) | (state.bonded << 2) |
        (state.storage_known << 3) | (state.pairing << 4) | (new_connection << 5) |
        (state.keyboard_subscribed << 6) | (state.mouse_subscribed << 7) | (state.ready << 8) |
        (state.advertising << 9) | (state.peer_count << 10);
    if (flags != diagnostic_flags) {
        diagnostic_flags = flags;
        diagnostic("state", 0);
    }
#endif
    passport_control_view_t view; passport_control_view(&view);
    state.network_available=view.available; state.network_active=view.active;
    memcpy(state.controller,view.controller,sizeof(state.controller));
    usb_command_poll(now, step);
    portENTER_CRITICAL(&state_lock);
    published = state;
    portEXIT_CRITICAL(&state_lock);
    ble_npl_callout_reset(&tick, ble_npl_time_ms_to_ticks32(20));
}

static void synced(void)
{
    state.synced = true;
    result("Select Bluetooth to pair");
    if (!startup_restore) return;
    startup_restore = false;
    refresh_peers();
    ble_addr_t peer;
    int rc = passport_store_selected(&peer);
    // Upgrade an older installation only when its sole saved bond is unambiguous.
    if (rc == BLE_HS_ENOENT && state.storage_known && state.peer_count == 1) {
        peer.type = state.peers[0].type;
        memcpy(peer.val, state.peers[0].address, sizeof(peer.val));
        rc = passport_store_select(&peer);
    }
    if (rc == BLE_HS_ENOENT || rc == PASSPORT_STORE_SELECTION_CLEARED) return;
    if (rc) { result("Saved selection unavailable"); return; }
    if (!is_saved(&peer)) { result("Selected bond unavailable"); return; }
    selected = peer;
    selected_valid = true;
    reconnect_attempts = 0;
    reconnect_next = now_ms();
    reconnect_end = reconnect_next + WINDOW_MS;
    result("Restoring saved host");
}

static void reset(int reason)
{
    usb_command_finish(PR_UNKNOWN, reason);
    if (remote_management.key) { remote_management.done=true; remote_management.error=CP_UNAVAILABLE; }
    (void)reason;
    passport_control_network_reset(now_ms());
    state.synced = false;
    state.connected = state.ready = false;
    state.advertising = state.pairing = false;
    state.keyboard_subscribed = state.mouse_subscribed = false;
    state.encrypted = state.bonded = false;
    suspended = new_connection = transition = disconnect_requested = closing = false;
    window_end = lease_end = 0;
    reconnect_end = 0;
    state.release_unknown = true;
    memset(keyboard, 0, sizeof(keyboard));
    memset(mouse, 0, sizeof(mouse));
    connection = BLE_HS_CONN_HANDLE_NONE;
    early_subscriptions.connection = BLE_HS_CONN_HANDLE_NONE;
    ++state.generation;
    hid_engine_set_link(&engine, false, false);
    hid_engine_set_locked(&engine, true);
    xQueueReset(commands);
    link_ready = false;
    result("Bluetooth reset; input unknown");
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    reset(0);
    vTaskDelete(NULL);
}

static bool random_bytes(void *user, uint8_t *out, size_t length)
{ (void)user; esp_fill_random(out,length); return true; }

esp_err_t passport_ble_start(bool buttons_available)
{
    physical_inputs_available = buttons_available;
    startup_restore = true;
    selected_valid = false;
    reconnect_end = reconnect_next = 0;
#if CONFIG_PASSPORT_DIAGNOSTICS
    diagnostic_until = now_ms() + 180000;
    diagnostic_flags = UINT32_MAX;
#endif
    commands = xQueueCreate(8, sizeof(passport_command_t));
    if (!commands) return ESP_ERR_NO_MEM;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { vQueueDelete(commands); commands = NULL; return err; }
    hid_engine_init(&engine);
    hid_engine_set_locked(&engine, !buttons_available);
    state.locked = !buttons_available;
    state.generation = 1;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Passport Input");
    ble_svc_gap_device_appearance_set(0x03c0);
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    int rc = passport_store_init();
    if (!rc) rc = ble_gatts_count_cfg(services);
    if (!rc) rc = ble_gatts_add_svcs(services);
    if (rc) { nimble_port_deinit(); vQueueDelete(commands); commands = NULL; return ESP_FAIL; }
    passport_names_load(PN_BLE);
    // Failure disables network input explicitly; native BLE controls still work.
    state.network_available=passport_control_init(&engine,random_bytes);
    diagnostic("control_init",state.network_available ? 0 : CP_UNAVAILABLE);
    if (state.network_available) passport_management_install();
    ble_hs_cfg.store_write_cb = secure_write;
    ble_hs_cfg.store_status_cb = store_full;
    ble_hs_cfg.sync_cb = synced;
    ble_hs_cfg.reset_cb = reset;
    ble_npl_callout_init(&tick, nimble_port_get_dflt_eventq(), service, NULL);
    ble_npl_callout_reset(&tick, ble_npl_time_ms_to_ticks32(20));
    if (xTaskCreate(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL, configMAX_PRIORITIES - 4, NULL) != pdPASS) {
        passport_control_deinit();
        ble_npl_callout_stop(&tick);
        nimble_port_deinit(); vQueueDelete(commands); commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void passport_ble_snapshot(passport_ble_state_t *output)
{
    portENTER_CRITICAL(&state_lock);
    *output = published;
    portEXIT_CRITICAL(&state_lock);
}

bool passport_ble_request(const passport_command_t *request)
{
    if (!commands) return false;
    if (xQueueSend(commands, request, 0) == pdTRUE) return true;
    passport_ble_emergency_lock();
    return false;
}

void passport_ble_emergency_lock(void) { atomic_store(&emergency_lock, true); }
