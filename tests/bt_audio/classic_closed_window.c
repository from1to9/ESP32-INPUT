// SPDX-License-Identifier: Apache-2.0
/* Real SDK functions are compiled below this fixture. Model only the task,
 * radio, and storage boundaries for the selected Classic SSP transaction. */
#define esp_bt_gap_get_bond_device_num model_bond_count
#define esp_bt_gap_get_bond_device_list model_bond_list
#define esp_bt_gap_ssp_confirm_reply model_confirm_reply
#define esp_bt_gap_ssp_reject_closed_window model_closed_window_reply
#define btc_inter_profile_call model_profile_call
#define main native_suite_main
#include "test_bt_audio.c"
#undef main
#undef btc_inter_profile_call
#undef esp_bt_gap_ssp_reject_closed_window
#undef esp_bt_gap_ssp_confirm_reply
#undef esp_bt_gap_get_bond_device_list
#undef esp_bt_gap_get_bond_device_num

#define TRUE true
#define FALSE false
#define CLASSIC_BT_INCLUDED TRUE
#define BLE_INCLUDED TRUE
#define SMP_INCLUDED TRUE
#define BTC_GAP_BT_INCLUDED TRUE
#define BTM_DISC_DURING_RS FALSE
#define BT_USE_TRACES FALSE
#define UNUSED(x) ((void)(x))
#define ESP_BLUEDROID_STATUS_CHECK(state) assert(esp_bluedroid_get_status() == (state))
#define BTC_TRACE_DEBUG(...) ((void)0)
#define BTC_TRACE_WARNING(...) ((void)0)
#define BTC_TRACE_ERROR(...) ((void)0)
#define APPL_TRACE_DEBUG(...) ((void)0)
#define BTM_TRACE_EVENT(...) ((void)0)
#define BTM_TRACE_ERROR(...) ((void)0)
#define BTC_ASSERTC(ok, text, value) assert(ok)
#define osi_malloc malloc
#define osi_free free
#define bdcmp(a, b) memcmp(a, b, 6)
#define bdcpy(a, b) memcpy(a, b, 6)
#define STREAM_TO_BDADDR(a, p) do { for (int i = 5; i >= 0; --i) (a)[i] = *(p)++; } while (0)
#define STREAM_TO_UINT32(n, p) do { (n) = (uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
    ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24); (p) += 4; } while (0)
#define BCM_STRCPY_S strcpy

enum { BD_ADDR_LEN = 6, ESP_BD_ADDR_LEN = 6, BD_NAME_LEN = 249, ESP_BT_GAP_MAX_BDNAME_LEN = 248,
       DEV_CLASS_LEN = 3 };
enum { BT_STATUS_FAIL = 1, BT_STATUS_RMT_DEV_DOWN, BT_STATUS_AUTH_REJECTED, BT_STATUS_AUTH_FAILURE };
enum { BTC_SIG_API_CB = 1, BTC_PID_GAP_BT = 3, BTC_GAP_BT_CFM_REQ_EVT, BTC_GAP_BT_AUTH_CMPL_EVT };
enum { HCI_SUCCESS, HCI_ERR_PAGE_TIMEOUT = 0x04, HCI_ERR_AUTH_FAILURE = 0x05, HCI_ERR_KEY_MISSING,
       HCI_ERR_CONNECTION_TOUT = 0x08, HCI_ERR_HOST_REJECT_SECURITY = 0x0e, HCI_ERR_PEER_USER = 0x13,
       HCI_ERR_CONN_CAUSE_LOCAL_HOST = 0x16, HCI_ERR_REPEATED_ATTEMPTS, HCI_ERR_PAIRING_NOT_ALLOWED,
       HCI_ERR_ENCRY_MODE_NOT_ACCEPTABLE = 0x25, HCI_ERR_UNIT_KEY_USED,
       HCI_ERR_PAIRING_WITH_UNIT_KEY_NOT_SUPPORTED = 0x29, HCI_ERR_LMP_RESPONSE_TIMEOUT = 0x22,
       HCI_ERR_INSUFFCIENT_SECURITY = 0x2f, HCI_ERR_UNSPECIFIED = 0x1f,
       HCI_ERR_LMP_ERR_TRANS_COLLISION = 0x23, HCI_ERR_DIFF_TRANSACTION_COLLISION = 0x2a,
       HCI_ERR_COMMAND_DISALLOWED = 0x0c };
enum { BTM_SUCCESS, BTM_SUCCESS_NO_SECURITY, BTM_NOT_AUTHORIZED, BTM_ERR_PROCESSING,
       BTM_SEC_DEV_REC_REMOVED, BTM_CMD_STARTED, BTM_AUTH_CLOSED_WINDOW = 0x100 };
enum { BTM_PAIR_STATE_IDLE, BTM_PAIR_STATE_WAIT_PIN_REQ, BTM_PAIR_STATE_WAIT_NUMERIC_CONFIRM,
       BTM_PAIR_STATE_WAIT_AUTH_COMPLETE, BTM_PAIR_STATE_WAIT_DISCONNECT,
       BTM_PAIR_STATE_KEY_ENTRY, BTM_PAIR_STATE_INCOMING_SSP };
enum { CLOSED_WINDOW_NONE, CLOSED_WINDOW_SENT, CLOSED_WINDOW_ACKED, CLOSED_WINDOW_FAILED };
enum { BTM_SEC_STATE_IDLE, BTM_SEC_STATE_AUTHENTICATING, BTM_SEC_STATE_DISCONNECTING,
       BTM_SEC_STATE_DISCONNECTING_BLE, BTM_SEC_STATE_DISCONNECTING_BOTH };
enum { BTM_PAIR_FLAGS_WE_STARTED_DD = 1, BTM_SEC_NONE = 0, BTM_SEC_AUTHORIZED = 1,
       BTM_SEC_AUTHENTICATED = 2, BTM_SEC_ENCRYPTED = 4, BTM_SEC_ROLE_SWITCHED = 8,
       BTM_SEC_LINK_KEY_KNOWN = 16, BTM_SEC_LINK_KEY_AUTHED = 32, BTM_SEC_LE_AUTHENTICATED = 0x200,
       BTM_SEC_LE_ENCRYPTED = 0x400, BTM_SEC_LE_LINK_KEY_KNOWN = 0x1000, BTM_SEC_16_DIGIT_PIN_AUTHED = 0x4000 };
enum { BTM_SEC_INVALID_HANDLE = 0xffff, BTM_SM4_TRUE = 0x10, BTA_DM_AUTH_CMPL_EVT = 1,
       BTM_SP_COMPLT_EVT = 2, BTM_SP_CFM_REQ_EVT, BTM_SP_KEY_REQ_EVT, BTM_SP_KEY_NOTIF_EVT,
       BT_TRANSPORT_BR_EDR = 1, BT_TRANSPORT_LE = 2,
       BTM_SEC_MAX_BONDS = 15, BTU_TTYPE_USER_FUNC = 1, BT_1SEC_TIMEOUT = 1, BTM_SEC_TIMEOUT_VALUE = 30 };
enum { BTM_IO_CAP_IO = 1, BTM_AUTH_SP_YES = 1, BTM_PAIR_FLAGS_DISC_WHEN_DONE = 4,
       BTM_SEC_OUT_AUTHENTICATE = 0x10, BTM_SEC_LE_LINK_KEY_AUTHED = 0x2000,
       BTM_SM4_RETRY = 8, BTM_ACL_LEGACY_AUTH_SELF = 1,
       BTM_LKEY_TYPE_AUTH_COMB = 5, BTM_LKEY_TYPE_AUTH_COMB_P_256 = 8 };
#define BTM_SEC_IS_SM4(value) ((value) & BTM_SM4_TRUE)
#define BTM_TRACE_DEBUG(...) ((void)0)

typedef uint8_t UINT8, BD_ADDR[6], DEV_CLASS[3], LINK_KEY[16];
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef bool BOOLEAN;
typedef int bt_status_t, tBT_TRANSPORT, tBTM_STATUS, tBTM_PAIRING_STATE, tBTM_SP_EVT;
typedef uintptr_t TIMER_PARAM_TYPE;
typedef char BD_NAME[249];
typedef void tBTM_SEC_CALLBACK(BD_ADDR, tBT_TRANSPORT, void *, tBTM_STATUS);
typedef struct { BD_ADDR bd_addr; DEV_CLASS dev_class; tBTM_STATUS status; } tBTM_SP_COMPLT;
typedef struct {
    BD_ADDR bd_addr;
    BD_NAME bd_name;
    LINK_KEY key;
    bool success, key_present, sc_support;
    UINT8 key_type, fail_reason, dev_type, addr_type;
} tBTA_DM_AUTH_CMPL;
typedef union { tBTA_DM_AUTH_CMPL auth_cmpl; } tBTA_DM_SEC;
typedef struct {
    BD_ADDR bd_addr;
    DEV_CLASS dev_class;
    BD_NAME bd_name;
    UINT32 num_val, confirmation_id;
    bool just_works;
    unsigned loc_auth_req, rmt_auth_req, loc_io_caps, rmt_io_caps;
} tBTA_DM_SP_CFM_REQ;
typedef union {
    tBTA_DM_SP_CFM_REQ cfm_req;
    struct { BD_ADDR bd_addr; DEV_CLASS dev_class; BD_NAME bd_name; UINT32 passkey; } key_notif;
} tBTM_SP_EVT_DATA;
typedef union { struct { bool accept; BD_ADDR bd_addr; UINT32 closed_window_id; } confirm; } tBTA_DM_MSG;
typedef struct {
    BD_ADDR bd_addr;
    DEV_CLASS dev_class;
    BD_NAME sec_bd_name;
    LINK_KEY link_key;
    UINT16 hci_handle, ble_hci_handle;
    unsigned sec_flags, sm4, security_required, sec_state, enc_key_size;
    unsigned rmt_io_caps, rmt_auth_req, pin_code_length, link_key_type;
    bool new_encryption_key_is_p256;
    bool enc_init_by_we;
    tBTM_SEC_CALLBACK *p_callback;
    void *p_ref_data;
} tBTM_SEC_DEV_REC;
typedef struct { TIMER_PARAM_TYPE param; } timer_tle_t;

static tBTM_SEC_DEV_REC record;
static struct {
    unsigned pairing_flags, acl_disc_reason, pin_code_len, collision_start_time;
    UINT32 pairing_confirmation_id;
    UINT8 closed_window_reply;
    tBTM_PAIRING_STATE pairing_state;
    BD_ADDR pairing_bda;
    struct { unsigned loc_io_caps, loc_auth_req; } devcb;
    timer_tle_t pairing_tle;
    bool discing;
    tBTM_SEC_DEV_REC *p_collided_dev_rec;
    struct {
        UINT8 (*p_auth_complete_callback)(BD_ADDR, DEV_CLASS, BD_NAME, int);
        tBTM_STATUS (*p_sp_callback)(int, tBTM_SP_EVT_DATA *);
    } api;
} btm_cb;
static struct {
    struct {
        unsigned count;
        struct { int transport; BD_ADDR peer_bdaddr; bool remove_dev_pending; } peer_device[2];
    } device_list;
    void (*p_sec_cback)(int, tBTA_DM_SEC *);
} bta_dm_cb;
static tBTA_DM_MSG pending_confirm;
static tBTA_DM_AUTH_CMPL pending_auth;
static bool confirm_posted, auth_posted, classic_acl;
static unsigned positive_confirms, negative_confirms, disconnects, positive_key_replies, negative_key_replies, key_deletes;
static UINT32 command_token;

static tBTM_SEC_DEV_REC *btm_find_dev(const UINT8 *address)
{ return memcmp(address, record.bd_addr, 6) == 0 ? &record : NULL; }
static tBTM_SEC_DEV_REC *btm_find_or_alloc_dev(UINT8 *address) { return btm_find_dev(address); }
static tBTM_SEC_DEV_REC *btm_find_dev_by_handle(UINT16 handle)
{ return handle == record.hci_handle || handle == record.ble_hci_handle ? &record : NULL; }
static void btm_sec_pairing_timeout(void) { assert(false); }
static void btu_stop_timer(timer_tle_t *timer) { assert(timer == &btm_cb.pairing_tle); }
static void btu_start_timer(timer_tle_t *timer, int type, int duration)
{ assert(timer == &btm_cb.pairing_tle && type == BTU_TTYPE_USER_FUNC && duration > 0); }
static void l2cu_update_lcb_4_bonding(BD_ADDR address, bool active) { (void)address; (void)active; }
static void btm_restore_mode(void) {}
static void btm_sec_check_pending_reqs(void) {}
static void btm_inq_clear_ssp(void) {}
static void btm_acl_resubmit_page(void) {}
static void l2c_pin_code_request(BD_ADDR address) { assert(btm_find_dev(address)); }
#define HCI_ROLE_UNKNOWN 0xff
static void btm_ble_update_mode_operation(int role, BD_ADDR address, UINT8 status)
{ assert(role == HCI_ROLE_UNKNOWN && btm_find_dev(address) && status == HCI_SUCCESS); }
static bool btsnd_hcic_user_conf_reply_tagged(BD_ADDR address, bool accept, UINT32 token)
{
    assert(btm_find_dev(address));
    if (accept) ++positive_confirms; else ++negative_confirms;
    command_token = token;
    return true;
}
static bool btsnd_hcic_user_conf_reply(BD_ADDR address, bool accept)
{ return btsnd_hcic_user_conf_reply_tagged(address, accept, 0); }
static void BTM_PasskeyReqReply(int status, BD_ADDR address, UINT32 key)
{ (void)status; (void)address; (void)key; assert(false); }
static void btsnd_hcic_user_passkey_neg_reply(BD_ADDR address) { (void)address; assert(false); }
static void btm_sec_disconnect(UINT16 handle, int reason) { (void)handle; (void)reason; assert(false); }
static void btsnd_hcic_link_key_req_reply(BD_ADDR address, LINK_KEY key)
{ assert(btm_find_dev(address) && key[0] == 0x42); ++positive_key_replies; }
static void btsnd_hcic_link_key_neg_reply(BD_ADDR address)
{ assert(btm_find_dev(address)); ++negative_key_replies; }
static void btm_sec_send_hci_disconnect(tBTM_SEC_DEV_REC *dev, UINT8 reason, UINT16 handle)
{ assert(dev == &record && reason == HCI_ERR_AUTH_FAILURE && handle == 0x801); ++disconnects; }
static void btm_sec_auth_collision(UINT16 handle) { (void)handle; assert(false); }
static void *btm_bda_to_acl(BD_ADDR address, int transport)
{ assert(btm_find_dev(address) && transport == BT_TRANSPORT_BR_EDR); return NULL; }
static void btm_sec_update_legacy_auth_state(void *acl, int status)
{ assert(!acl && status == BTM_ACL_LEGACY_AUTH_SELF); }
static void l2cu_start_post_bond_timer(UINT16 handle) { (void)handle; assert(false); }
static bool btm_sec_use_smp_br_chnl(tBTM_SEC_DEV_REC *dev) { (void)dev; assert(false); return false; }
static bool btm_sec_is_master(tBTM_SEC_DEV_REC *dev) { (void)dev; assert(false); return false; }
static void BTM_SetEncryption(BD_ADDR address, int transport, void *cb, void *arg)
{ (void)address; (void)transport; (void)cb; (void)arg; assert(false); }
static int btm_sec_execute_procedure(tBTM_SEC_DEV_REC *dev) { (void)dev; assert(false); return BTM_ERR_PROCESSING; }
static void btm_sec_dev_rec_cback_event(tBTM_SEC_DEV_REC *dev, int status, bool le)
{ assert(dev == &record && status == BTM_ERR_PROCESSING && !le); }
static bool BTM_IsAclConnectionUp(BD_ADDR address, int transport)
{ assert(btm_find_dev(address) && transport == BT_TRANSPORT_BR_EDR); return classic_acl; }
static bool BTM_SecDeleteDevice(BD_ADDR address, int transport)
{
    /* Verify the transport boundary; the existing same-address suite executes this leaf. */
    assert(btm_find_dev(address) && transport == BT_TRANSPORT_BR_EDR && !classic_acl);
    record.sec_flags &= ~BTM_SEC_LINK_KEY_KNOWN;
    ++key_deletes;
    return true;
}
static void BTM_ReadDevInfo(BD_ADDR address, UINT8 *type, UINT8 *addr_type)
{ assert(btm_find_dev(address)); *type = 3; *addr_type = 0; }

#define BTC_STORAGE_LINK_KEY_TYPE_STR "LinkKeyType"
#define BTC_STORAGE_PIN_LENGTH_STR "PinLength"
#define BTC_STORAGE_SC_SUPPORT "SCSupport"
#define BTC_STORAGE_LINK_KEY_STR "LinkKey"
#define BTC_STORAGE_DEV_CLASS_STR "DevClass"
#define CONFIG_FILE_PATH "model-bt-config"

typedef char bdstr_t[18];
typedef struct {
    const char *address;
    unsigned keys;
    LINK_KEY link_key;
    int key_type, pin_length;
    BOOLEAN sc_support;
    bool le_ltk;
} btc_config_section_iter_t;
static btc_config_section_iter_t sections[] = {{.address = "02:03:04:05:06:07", .le_ltk = true}};
static btc_config_section_iter_t persisted;
static const void *config = sections;
static bool config_locked;
static unsigned writes, flushes, removals;

static void bdaddr_to_string(const bt_bdaddr_t *address, char *out, size_t length)
{
    const UINT8 *p = address->address;
    assert(snprintf(out, length, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]) == 17);
}
static bool string_is_bdaddr(const char *name) { return !strcmp(name, sections[0].address); }
static bool string_to_bdaddr(const char *name, bt_bdaddr_t *address)
{ assert(string_is_bdaddr(name)); memcpy(address->address, peer, 6); return true; }
static void uint2devclass(UINT32 value, DEV_CLASS out) { (void)value; memset(out, 0, sizeof(DEV_CLASS)); }
static void btc_config_lock(void) { assert(!config_locked); config_locked = true; }
static void btc_config_unlock(void) { assert(config_locked); config_locked = false; }
static unsigned classic_field(const char *key)
{
    if (!strcmp(key, BTC_STORAGE_LINK_KEY_TYPE_STR)) return 1;
    if (!strcmp(key, BTC_STORAGE_PIN_LENGTH_STR)) return 2;
    if (!strcmp(key, BTC_STORAGE_SC_SUPPORT)) return 4;
    if (!strcmp(key, BTC_STORAGE_LINK_KEY_STR)) return 8;
    assert(!strcmp(key, BTC_STORAGE_DEV_CLASS_STR));
    return 0;
}
static bool btc_config_exist(const char *address, const char *key)
{ assert(config_locked); return string_is_bdaddr(address) && (sections[0].keys & classic_field(key)); }
static bool btc_config_remove(const char *address, const char *key)
{ assert(btc_config_exist(address, key)); sections[0].keys &= ~classic_field(key); ++removals; return true; }
static bool btc_config_set_int(const char *address, const char *key, int value)
{
    assert(config_locked && string_is_bdaddr(address));
    unsigned field = classic_field(key);
    assert(field == 1 || field == 2);
    if (field == 1) sections[0].key_type = value; else sections[0].pin_length = value;
    sections[0].keys |= field;
    ++writes;
    return true;
}
static bool btc_config_set_bin(const char *address, const char *key, const UINT8 *value, size_t length)
{
    assert(config_locked && string_is_bdaddr(address));
    unsigned field = classic_field(key);
    if (field == 4) { assert(length == 1); sections[0].sc_support = *value != 0; }
    else { assert(field == 8 && length == sizeof(LINK_KEY)); memcpy(sections[0].link_key, value, length); }
    sections[0].keys |= field;
    ++writes;
    return true;
}
static bool btc_config_get_bin(const char *address, const char *key, UINT8 *value, size_t *length)
{
    if (!btc_config_exist(address, key)) return false;
    if (classic_field(key) == 4) { assert(*length == 1); *value = sections[0].sc_support; }
    else { assert(*length == sizeof(LINK_KEY)); memcpy(value, sections[0].link_key, *length); }
    return true;
}
static bool btc_config_get_int(const char *address, const char *key, int *value)
{
    if (!btc_config_exist(address, key)) return false;
    *value = classic_field(key) == 1 ? sections[0].key_type : sections[0].pin_length;
    return true;
}
static const btc_config_section_iter_t *btc_config_section_begin(void) { return sections; }
static const btc_config_section_iter_t *btc_config_section_end(void) { return sections + 1; }
static const btc_config_section_iter_t *btc_config_section_next(const btc_config_section_iter_t *section) { return section + 1; }
static const char *btc_config_section_name(const btc_config_section_iter_t *section) { return section->address; }
static void btc_storage_evict_overflow_bonded_devices(unsigned max) { assert(config_locked && max >= 1); }
static bool config_save(const void *state, const char *file)
{
    assert(config_locked && state == sections && !strcmp(file, CONFIG_FILE_PATH));
    ++flushes;
    persisted = sections[0];
    return true;
}
static void BTA_DmAddDevice(BD_ADDR address, DEV_CLASS dc, LINK_KEY key, UINT8 *features,
                            UINT32 *trusted, UINT8 type, UINT8 io, UINT8 pin, UINT8 sc)
{
    (void)dc;
    assert(btm_find_dev(address) && !features && !trusted && !io && type == 8 && pin == 16 && sc);
    memcpy(record.link_key, key, sizeof(LINK_KEY));
    record.sec_flags |= BTM_SEC_LINK_KEY_KNOWN;
}

int btc_inter_profile_call(btc_msg_t *message)
{
    if (message->sig != BTC_SIG_API_CB) return model_profile_call(message);
    assert(!audio_locked && message->pid == BTC_PID_GAP_BT);
    esp_bt_gap_cb_param_t event;
    memcpy(&event, message->arg, sizeof(event));
    assert(message->act == BTC_GAP_BT_CFM_REQ_EVT || message->act == BTC_GAP_BT_AUTH_CMPL_EVT);
    gap(message->act == BTC_GAP_BT_CFM_REQ_EVT ? ESP_BT_GAP_CFM_REQ_EVT : ESP_BT_GAP_AUTH_CMPL_EVT, &event);
    return BT_STATUS_SUCCESS;
}

static esp_err_t post_confirm(esp_bd_addr_t address, bool accept, UINT32 token)
{
    assert(!audio_locked && btm_find_dev(address) && !confirm_posted);
    pending_confirm.confirm.accept = accept;
    pending_confirm.confirm.closed_window_id = token;
    memcpy(pending_confirm.confirm.bd_addr, address, 6);
    confirm_posted = true;
    return ESP_OK;
}

esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t address, bool accept)
{ return post_confirm(address, accept, 0); }

esp_err_t esp_bt_gap_ssp_reject_closed_window(esp_bd_addr_t address, uint32_t confirmation_id)
{ assert(confirmation_id); return post_confirm(address, false, confirmation_id); }

#include "sdk.inc"

static void post_auth(int event, tBTA_DM_SEC *param)
{
    assert(event == BTA_DM_AUTH_CMPL_EVT && !auth_posted);
    pending_auth = param->auth_cmpl;
    auth_posted = true;
}

static tBTM_STATUS pairing_callback(tBTM_SP_EVT event, tBTM_SP_EVT_DATA *data)
{
    if (event == BTM_SP_CFM_REQ_EVT) {
        assert(data->cfm_req.just_works && data->cfm_req.num_val == 123456);
        btc_dm_sp_cfm_req_evt(&data->cfm_req);
    } else {
        assert(event == BTM_SP_COMPLT_EVT);
    }
    return BTM_CMD_STARTED;
}

static void wire_address(UINT8 *wire)
{
    for (size_t i = 0; i < 6; ++i) wire[i] = record.bd_addr[5 - i];
}

static void start_prompt(void)
{
    /* Begin after an incoming ACL and the controller's IO exchange. */
    btm_cb.api.p_sp_callback = pairing_callback;
    btm_cb.pairing_state = BTM_PAIR_STATE_INCOMING_SSP;
    memcpy(btm_cb.pairing_bda, record.bd_addr, 6);
    UINT8 frame[10] = {0};
    wire_address(frame);
    frame[6] = 0x40; frame[7] = 0xe2; frame[8] = 1;
    btm_proc_sp_req_evt(BTM_SP_CFM_REQ_EVT, frame);
}

static void pairing_complete(UINT8 status)
{
    UINT8 completed[7] = {status};
    wire_address(completed + 1);
    btm_simple_pair_complete(completed, sizeof(completed));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    bool stranger = !strcmp(scenario, "unknown-ssp-denied");
    bool replace = !strcmp(scenario, "open-window-replacement");
    bool close_queued = !strcmp(scenario, "closed-while-queued");
    bool invalid = !strcmp(scenario, "invalid-key");
    bool auth_complete = invalid || !strcmp(scenario, "auth-complete");
    bool stale_completion = !strcmp(scenario, "stale-completion");
    bool controller_refusal = !strcmp(scenario, "controller-refusal");
    assert(bt_audio_init() == ESP_OK);
    initialized();
    memcpy(record.bd_addr, peer, 6);
    tBTA_DM_AUTH_CMPL original = {.success = true, .key_present = true, .sc_support = true,
                                .key_type = 8, .key = {0x42}};
    memcpy(original.bd_addr, peer, 6);
    btc_dm_auth_cmpl_evt(&original);
    assert(flushes == 1 && writes == 4 && persisted.keys == 15 && persisted.le_ltk);
    btc_config_lock();
    assert(_btc_storage_in_fetch_bonded_bt_device(sections[0].address, 1) == BT_STATUS_SUCCESS);
    btc_config_unlock();
    bt_audio_service();
    bt_audio_snapshot_t view = bt_audio_snapshot();
    bt_audio_bond_list_t saved = observed_bonds();
    assert(saved.known && saved.count == 1 && !view.pairing_requested && !pairing_claim);
    uint64_t request;
    if (replace || close_queued) {
        assert(bt_audio_pairing_open(1000) == ESP_OK);
    } else {
        assert(bt_audio_saved_request(view.generation, view.a2dp_link_generation, saved.revision,
                                     BT_AUDIO_CONNECT_A2DP, stranger ? other : peer, &request) ==
               (stranger ? ESP_ERR_NOT_FOUND : ESP_OK));
        bt_audio_service();
        assert(dispatch_calls == !stranger);
    }
    if (!strcmp(scenario, "saved-key-reuse")) {
        btm_sec_link_key_request(record.bd_addr);
        assert(positive_key_replies == 1 && !negative_key_replies && !confirm_posted && !removals);
        assert(observed_bonds().count == 1 && persisted.keys == 15);
        puts("saved-key-reuse: authorized request, loaded Classic key answers controller without any pairing prompt");
        return 0;
    }
    /* Radio boundary: an incoming Classic ACL enters SSP with an active CFM request.
     * The device trace did not capture whether an old key was requested or usable. */
    if (stranger) {
        memset(&record, 0, sizeof(record));
        memcpy(record.bd_addr, other, 6);
    } else {
        record.sec_flags |= BTM_SEC_LE_LINK_KEY_KNOWN;
    }
    record.hci_handle = 0x801;
    record.ble_hci_handle = 0;
    classic_acl = true;
    btm_cb.api.p_auth_complete_callback = bta_dm_authentication_complete_cback;
    bta_dm_cb.p_sec_cback = post_auth;
    start_prompt();
    if (close_queued) bt_audio_pairing_close();
    bt_audio_service();
    if (replace) {
        assert(confirm_posted && pending_confirm.confirm.accept && !pending_confirm.confirm.closed_window_id);
        bta_dm_confirm(&pending_confirm);
        assert(positive_confirms == 1 && negative_confirms == 0);
        pairing_complete(HCI_SUCCESS);
        assert(record.sec_flags & BTM_SEC_AUTHENTICATED);
        original.key[0] = 0x77;
        btc_dm_auth_cmpl_evt(&original);
        bt_audio_service();
        assert(bt_audio_snapshot().pairing_completed && persisted.link_key[0] == 0x77 && persisted.le_ltk);
        puts(scenario);
        return 0;
    }
    assert(confirm_posted && !pending_confirm.confirm.accept && !bt_audio_snapshot().pairing_handshake_pending);
    if (!strcmp(scenario, "stale-reply")) {
        tBTA_DM_MSG stale = pending_confirm;
        confirm_posted = false;
        btm_sec_change_pairing_state(BTM_PAIR_STATE_IDLE);
        start_prompt();
        bt_audio_service();
        bta_dm_confirm(&stale);
        assert(!negative_confirms && btm_cb.pairing_state == BTM_PAIR_STATE_WAIT_NUMERIC_CONFIRM);
    }
    bta_dm_confirm(&pending_confirm);
    assert(negative_confirms == 1 && btm_cb.acl_disc_reason == HCI_ERR_HOST_REJECT_SECURITY);
#if TEST_TAGGED_CONFIRMATION
    assert(command_token && command_token == pending_confirm.confirm.closed_window_id);
    UINT8 reply[7] = {controller_refusal ? HCI_ERR_COMMAND_DISALLOWED : HCI_SUCCESS};
    wire_address(reply + 1);
    btm_closed_window_reply_complete(stale_completion ? command_token + 1 : command_token, reply, sizeof(reply));
#endif

    /* Radio boundary: report a failed Simple Pairing Complete and acknowledge
     * the resulting locally requested disconnect. Both stack handlers are real. */
    if (auth_complete) record.sec_state = BTM_SEC_STATE_AUTHENTICATING;
    pairing_complete(HCI_ERR_AUTH_FAILURE);
    assert(disconnects == !auth_complete && !(record.sec_flags & BTM_SEC_AUTHENTICATED));
    if (!strcmp(scenario, "le-disconnect")) {
        assert(btm_sec_disconnected(0, HCI_ERR_PEER_USER));
        assert(!auth_posted && (record.sec_flags & BTM_SEC_LE_LINK_KEY_KNOWN));
    }
    classic_acl = false;
    if (auth_complete) btm_sec_auth_complete(0x801, invalid ? HCI_ERR_KEY_MISSING : HCI_ERR_AUTH_FAILURE);
    else assert(btm_sec_disconnected(0x801, HCI_ERR_CONN_CAUSE_LOCAL_HOST));
    assert(auth_posted);
    assert(stranger || (record.sec_flags & BTM_SEC_LE_LINK_KEY_KNOWN));
    btc_dm_auth_cmpl_evt(&pending_auth);
    assert(sections[0].le_ltk && persisted.le_ltk && !observed_bonds().known);
    bt_audio_service();
    bt_audio_bond_list_t refreshed = observed_bonds();
    bool closed_denial = !invalid && !stale_completion && !controller_refusal;
    assert(refreshed.known && writes == 4);
    if (closed_denial) {
        assert(persisted.keys == 15 && refreshed.count == 1 && "closed-window rejection erased the prior bond");
        assert(pending_auth.fail_reason == HCI_ERR_PAIRING_NOT_ALLOWED && key_deletes == 0);
        assert(removals == 0 && flushes == 1);
        if (!stranger) {
            btm_sec_link_key_request(record.bd_addr);
            assert(positive_key_replies == 1 && !negative_key_replies);
        }
    } else {
        assert(pending_auth.fail_reason == (invalid ? HCI_ERR_KEY_MISSING : HCI_ERR_AUTH_FAILURE));
        assert(key_deletes == 1 && removals == 4 && flushes == 2);
        assert(persisted.keys == 0 && refreshed.count == 0);
    }
    puts(scenario);
    return 0;
}
