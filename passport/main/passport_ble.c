#include "passport_ble.h"
#include "passport_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_timer.h"
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
static uint8_t keyboard[8], mouse[4], leds;
static bool suspended, new_connection, selected_valid, transition, disconnect_requested, closing;
static ble_addr_t selected;
static passport_command_t pending;
static uint64_t window_end, release_end, lease_end;
static bool link_ready;
static bool pairing_failed;

static uint64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void result(const char *text) { snprintf(state.result, sizeof(state.result), "%s", text); }
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
        state.storage_known = false;
        state.peer_count = 0;
        if (previously_known || previous_count) ++state.bond_revision;
        return;
    }
    state.peer_count = count;
    for (int i = 0; i < count; ++i) {
        if (!stable_identity(&peers[i])) {
            state.storage_known = false;
            state.peer_count = 0;
            ++state.bond_revision;
            return;
        }
        state.peers[i].type = peers[i].type;
        memcpy(state.peers[i].address, peers[i].val, 6);
        format_peer(state.peers[i].label, sizeof(state.peers[i].label), &peers[i]);
    }
    if (!previously_known || previous_count != state.peer_count ||
        memcmp(previous, state.peers, state.peer_count * sizeof(previous[0]))) ++state.bond_revision;
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

static int secure_write(int type, const union ble_store_value *value)
{
    if (type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        if (!new_connection || closing || !state.pairing || now_ms() >= window_end) {
            pairing_failed = true;
            return BLE_HS_EREJECT;
        }
        if (!stable_identity(&value->sec.peer_addr)) {
            pairing_failed = true;
            result("Stable peer identity required");
            return BLE_HS_EREJECT;
        }
        // Identity exchange can reveal an existing bond behind an unrecognized RPA.
        // The cached list predates this synchronous SDK key-persistence callback.
        for (unsigned i = 0; i < state.peer_count; ++i) {
            if (state.peers[i].type == value->sec.peer_addr.type &&
                !memcmp(state.peers[i].address, value->sec.peer_addr.val, 6)) {
                pairing_failed = true;
                result("Forget this bond before re-pairing");
                return BLE_HS_EREJECT;
            }
        }
    }
    int rc = passport_store_write(type, value);
    if (rc && new_connection) pairing_failed = true;
    return rc;
}

static int store_full(struct ble_store_status_event *event, void *arg)
{
    (void)event; (void)arg;
    result("Bond store full; forget a host");
    return BLE_HS_ESTORE_CAP;
}

static bool stop_advertising(void)
{
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

static void advertise(bool pairing)
{
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
    if (!rc) rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, WINDOW_MS, &params, gap_event, NULL);
    if (rc) { result("Advertising failed"); return; }
    state.advertising = true;
    state.pairing = pairing;
    window_end = now_ms() + WINDOW_MS;
    result(pairing ? "Pair on host within 60 seconds" : "Waiting for selected host");
}

static bool send_report(void *arg, uint8_t id, const uint8_t *data, size_t length)
{
    (void)arg;
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
        bool window_open = state.advertising && window_end > now_ms() && state.storage_known;
        state.advertising = false;
        if (event->connect.status) {
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
            disconnect_link();
            return 0;
        }
        if (!new_connection) format_peer(state.current, sizeof(state.current), &desc.peer_id_addr);
        result(new_connection ? "Pairing" : "Connected; securing link");
        if (ble_gap_security_initiate(connection)) {
            result("Security request failed");
            disconnect_link();
        }
        return 0;
        }
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        ++state.generation;
        state.connected = state.encrypted = state.bonded = false;
        state.keyboard_subscribed = state.mouse_subscribed = false;
        state.ready = false;
        suspended = new_connection = closing = false;
        memset(keyboard, 0, sizeof(keyboard));
        memset(mouse, 0, sizeof(mouse));
        disconnect_requested = false;
        if (!transition || engine.state.release_pending || engine.count) state.release_unknown = true;
        hid_engine_set_link(&engine, false, false);
        link_ready = false;
        stop_advertising();
        if (!transition) result("Disconnected");
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
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
        }
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
        transition = false;
        return;
    }
    switch (pending.kind) {
    case PASSPORT_PAIR: selected_valid = false; advertise(true); break;
    case PASSPORT_ADVERTISE:
        selected = peer;
        selected_valid = true;
        advertise(false);
        break;
    case PASSPORT_FORGET:
        if (passport_store_forget(&peer)) result("Forget uncertain; restart required");
        else { result("Bond forgotten"); if (selected_valid && same(&selected, &peer)) selected_valid = false; }
        break;
    default: result("Disconnected; bond retained"); break;
    }
    transition = false;
    disconnect_requested = false;
}

static void command(const passport_command_t *request, uint64_t now)
{
    if (request->kind == PASSPORT_LOCK) {
        hid_engine_set_locked(&engine, true);
        close_pairing();
        result("Input locked");
        return;
    }
    // Closing a visible pairing window remains valid across its connect event.
    if (request->kind == PASSPORT_STOP_PAIR) { close_pairing(); result("Pairing closed"); return; }
    if (request->generation != state.generation) { result("Stale action rejected"); return; }
    if (request->kind == PASSPORT_UNLOCK) { hid_engine_set_locked(&engine, false); result("Unlocked"); return; }
    if (request->kind == PASSPORT_CANCEL) { hid_engine_cancel(&engine); lease_end = 0; return; }
    if (request->kind <= PASSPORT_FORGET) {
        if ((request->kind == PASSPORT_ADVERTISE || request->kind == PASSPORT_FORGET || request->kind == PASSPORT_DISCONNECT) &&
            request->bond_revision != state.bond_revision) { result("Host list changed; select again"); return; }
        if (request->kind == PASSPORT_DISCONNECT) {
            ble_addr_t peer = { .type = request->peer_type };
            memcpy(peer.val, request->peer, 6);
            if (!selected_valid || !same(&selected, &peer)) { result("Selected host is not active"); return; }
        }
        if (transition) { result("Host change in progress"); return; }
        pending = *request;
        if (!stop_advertising()) return;
        hid_engine_cancel(&engine);
        transition = true;
        release_end = now + RELEASE_MS;
        result("Releasing input");
        return;
    }
    if (transition || !state.ready) { result("Input unavailable"); return; }
    hid_engine_enqueue_result_t rc = HID_ENGINE_ENQUEUE_INVALID;
    if (request->kind == PASSPORT_MOVE) rc = hid_engine_enqueue_mouse_move(&engine, engine.state.epoch, INPUT_LOCAL, request->x, request->y);
    if (request->kind == PASSPORT_CLICK) rc = hid_engine_enqueue_mouse_click(&engine, engine.state.epoch, INPUT_LOCAL, 1);
    if (request->kind == PASSPORT_KEY) rc = hid_engine_enqueue_mouse_key(&engine, engine.state.epoch, INPUT_LOCAL, 0, request->usage);
    if (rc == HID_ENGINE_ENQUEUE_OK) { lease_end = now + LEASE_MS; result("Input queued"); }
    else { hid_engine_cancel(&engine); result("Input rejected; releasing"); }
}

static void service(struct ble_npl_event *event)
{
    (void)event;
    const uint64_t now = now_ms();
    refresh_peers();
    if (atomic_exchange(&emergency_lock, false)) {
        hid_engine_set_locked(&engine, true);
        close_pairing();
        xQueueReset(commands);
        result("Queue pressure; input locked");
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
            selected = desc.peer_id_addr;
            selected_valid = true;
            new_connection = false;
            state.pairing = false;
            window_end = 0;
            format_peer(state.current, sizeof(state.current), &selected);
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
    passport_command_t request;
    for (unsigned i = 0; i < 4 && xQueueReceive(commands, &request, 0) == pdTRUE; ++i) command(&request, now);
    hid_engine_step_result_t step = hid_engine_step(&engine, send_report, NULL);
    if (step == HID_ENGINE_STEP_RETRY_EXHAUSTED) {
        state.release_unknown = true;
        result("Release unknown; disconnecting");
        disconnect_link();
    }
    if (transition) {
        if (connection == BLE_HS_CONN_HANDLE_NONE) execute_management();
        else if (!disconnect_requested && (!engine.state.release_pending || now >= release_end)) {
            if (engine.state.release_pending) state.release_unknown = true;
            int rc = disconnect_link();
            if (rc) { result("Disconnect failed"); transition = false; }
            else disconnect_requested = true;
        }
    }
    state.locked = engine.state.locked;
    state.ready = transport_ready && engine.ready && !engine.state.release_pending && !state.locked && !transition;
    state.window_seconds = window_end > now ? (window_end - now + 999) / 1000 : 0;
    portENTER_CRITICAL(&state_lock);
    published = state;
    portEXIT_CRITICAL(&state_lock);
    ble_npl_callout_reset(&tick, ble_npl_time_ms_to_ticks32(20));
}

static void synced(void)
{
    state.synced = true;
    result("Select Bluetooth to pair");
}

static void reset(int reason)
{
    (void)reason;
    state.synced = false;
    state.connected = state.ready = false;
    state.advertising = state.pairing = false;
    state.keyboard_subscribed = state.mouse_subscribed = false;
    state.encrypted = state.bonded = false;
    suspended = new_connection = transition = disconnect_requested = closing = false;
    window_end = lease_end = 0;
    state.release_unknown = true;
    memset(keyboard, 0, sizeof(keyboard));
    memset(mouse, 0, sizeof(mouse));
    connection = BLE_HS_CONN_HANDLE_NONE;
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

esp_err_t passport_ble_start(void)
{
    commands = xQueueCreate(8, sizeof(passport_command_t));
    if (!commands) return ESP_ERR_NO_MEM;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { vQueueDelete(commands); commands = NULL; return err; }
    hid_engine_init(&engine);
    hid_engine_set_locked(&engine, true);
    state.locked = true;
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
    ble_hs_cfg.store_write_cb = secure_write;
    ble_hs_cfg.store_status_cb = store_full;
    ble_hs_cfg.sync_cb = synced;
    ble_hs_cfg.reset_cb = reset;
    ble_npl_callout_init(&tick, nimble_port_get_dflt_eventq(), service, NULL);
    ble_npl_callout_reset(&tick, ble_npl_time_ms_to_ticks32(20));
    if (xTaskCreate(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL, configMAX_PRIORITIES - 4, NULL) != pdPASS) {
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
