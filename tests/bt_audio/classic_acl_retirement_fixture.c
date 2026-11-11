// SPDX-License-Identifier: Apache-2.0
// Synthetic fields at SDK callback boundaries. No real peer identifiers.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t UINT8, BOOLEAN, tBT_TRANSPORT;
typedef uint16_t UINT16;
typedef uint8_t BD_ADDR[6];
#define TRUE 1
#define FALSE 0
#define CLASSIC_BT_INCLUDED TRUE
#define CONFIG_APP_CLASSIC_SCAN_COORDINATION 1
#define BLE_INCLUDED TRUE
#define SMP_INCLUDED FALSE
#define BTM_SCO_INCLUDED TRUE
#define L2CAP_NUM_FIXED_CHNLS 0
#define BLE_TOPOLOGY_CHECK FALSE
#define BLE_50_FEATURE_SUPPORT FALSE
#define BLE_42_ADV_EN FALSE
#define BLE_42_FEATURE_SUPPORT FALSE
#define BLE_FEAT_ISO_CIG_EN FALSE
#define GATTC_CONNECT_RETRY_EN FALSE
#define BD_ADDR_LEN 6
#define BT_TRANSPORT_BR_EDR 1
#define BT_TRANSPORT_LE 2
#define LST_CONNECTED 1
#define LST_DISCONNECTING 2
#define LST_CONNECTING 3
#define LST_CONNECTING_WAIT_SWITCH 4
#define LST_CONNECT_HOLDING 5
#define LST_DISCONNECTED 6
#define HCI_ERR_DIRECTED_ADVERTISING_TIMEOUT 0x3c
#define HCI_SUCCESS 0
#define HCI_ERR_COMMAND_DISALLOWED 0x0c
#define HCI_ERR_CONN_FAILED_ESTABLISHMENT 0x3e
#define HCI_ERR_HOST_REJECT_SECURITY 0x0e
#define L2CEVT_LP_DISCONNECT_IND 4
#define L2CAP_PING_RESULT_NO_RESP 0
#define BTM_TRACE_API(...) ((void)0)
#define BTM_TRACE_DEBUG(...) ((void)0)
#define BTM_TRACE_WARNING(...) ((void)0)
#define HCI_TRACE_WARNING(...) ((void)0)
#define L2CAP_TRACE_DEBUG(...) ((void)0)
#define L2CAP_TRACE_EVENT(...) ((void)0)
#define L2CAP_TRACE_WARNING(...) ((void)0)
#define ACL_DB_BDA 0
#define ACL_DB_HANDLE 1
#define BTM_SEC_INVALID_HANDLE UINT16_MAX
#define HCID_GET_HANDLE(handle) ((handle) & 0x0fff)
#define STREAM_TO_UINT16(value, p) do { (value) = (p)[0] | ((UINT16)(p)[1] << 8); (p) += 2; } while (0)
#define STREAM_TO_UINT8(value, p) ((value) = *(p)++)
#define UNUSED(value) ((void)(value))

typedef struct tL2C_CCB { struct tL2C_CCB *p_next_ccb; } tL2C_CCB;
typedef void tL2CA_ECHO_RSP_CB(unsigned);
typedef struct { UINT16 event; } BT_HDR;
typedef struct {
    struct { tL2C_CCB *p_first_ccb; } ccb_queue;
    tL2C_CCB *p_pending_ccb;
    BD_ADDR remote_bd_addr;
    UINT8 transport, link_state, disc_reason;
    tL2CA_ECHO_RSP_CB *p_echo_rsp_cb;
    void *link_xmit_data_q;
} tL2C_LCB;
typedef UINT8 tL2C_LINK_STATE;
typedef struct { UINT8 in_use, transport; BD_ADDR remote_addr; UINT16 hci_handle; } tACL_CONN;
typedef struct { BD_ADDR bd_addr; UINT16 hci_handle, ble_hci_handle; } tBTM_SEC_DEV_REC;
typedef struct { unsigned type; void *p_data1, *p_data2; } tACL_DB_PARAM;
typedef tACL_CONN list_node_t;
static tACL_CONN acl[2];
static struct { list_node_t *p_acl_db_list; UINT8 acl_disc_reason; } btm_cb = {acl, 0x16};
static struct { BOOLEAN is_ble_connecting; BD_ADDR ble_connecting_bda; } l2cb;
static tL2C_LCB lcb;
static tL2C_CCB channel;
static BD_ADDR peer = {1, 2, 3, 4, 5, 6};
static bool lcb_present = true, retry_succeeds;
static unsigned retires[3], retries;

static list_node_t *list_foreach(list_node_t *list, BOOLEAN (*fn)(void *, void *), void *arg)
{
    for (size_t i = 0; i < 2; ++i) if (!fn(&list[i], arg)) return &list[i];
    return NULL;
}
static void *list_node(list_node_t *node) { return node; }
static bool list_is_empty(const void *q) { (void)q; return true; }
static void *list_front(const void *q) { (void)q; return NULL; }
static void list_remove(void *q, const void *item) { (void)q; (void)item; assert(false); }
static void osi_free(void *value) { (void)value; assert(false); }
static void BTM_Recovery_Pre_State(void) { assert(false); }
static tL2C_LCB *l2cu_find_lcb_by_handle(UINT16 handle)
{ return handle == 0x801 && lcb_present ? &lcb : NULL; }
static tL2C_LCB *l2cu_find_lcb_by_state(UINT8 state) { assert(state == LST_CONNECT_HOLDING); return NULL; }
static void l2c_csm_execute(tL2C_CCB *ccb, unsigned event, void *reason)
{
    (void)reason;
    assert(ccb == &channel && event == L2CEVT_LP_DISCONNECT_IND);
    lcb.ccb_queue.p_first_ccb = NULL;
}
static void btm_sco_acl_removed(BD_ADDR address) { assert(!memcmp(address, peer, sizeof(peer))); }
static void btm_acl_removed(BD_ADDR address, tBT_TRANSPORT transport)
{
    assert(!memcmp(address, peer, sizeof(peer)));
    for (size_t i = 0; i < 2; ++i) {
        if (acl[i].transport == transport && acl[i].in_use) {
            acl[i].in_use = false;
            ++retires[transport];
        }
    }
}
static BOOLEAN l2cu_create_conn(tL2C_LCB *link, tBT_TRANSPORT transport)
{
    assert(link == &lcb && transport == BT_TRANSPORT_BR_EDR);
    ++retries;
    if (!retry_succeeds) return FALSE;
    link->link_state = LST_CONNECTING;
    return TRUE;
}
static void L2CA_CancelBleConnectReq(BD_ADDR address) { (void)address; assert(false); }

BOOLEAN l2c_link_hci_disc_comp(UINT16 handle, UINT8 reason);
static BOOLEAN btm_acl_disconnected(UINT16 handle, UINT8 reason)
{
    if (handle == 0) return TRUE;
    return l2c_link_hci_disc_comp(handle, reason);
}
static tBTM_SEC_DEV_REC *btm_find_dev_by_handle(UINT16 handle) { (void)handle; return NULL; }
static tBTM_SEC_DEV_REC *btm_find_dev(BD_ADDR address) { (void)address; return NULL; }
static void btm_sec_free_dev(tBTM_SEC_DEV_REC *record, tBT_TRANSPORT transport)
{ (void)record; (void)transport; assert(false); }

#include "classic_acl_retirement_sdk.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool failed_receipt = strcmp(argv[1], "failed-receipt") == 0;
    bool synthetic = strcmp(argv[1], "synthetic") == 0;
    bool le_complete = strcmp(argv[1], "le-complete") == 0;
    bool pending = strcmp(argv[1], "no-retry") != 0 && !le_complete;
    retry_succeeds = strcmp(argv[1], "pending") == 0 || failed_receipt || synthetic;
    for (unsigned i = 0; i < 2; ++i) {
        acl[i] = (tACL_CONN){.in_use = true, .transport = (UINT8)(i + 1),
                            .hci_handle = i ? 0 : 0x801};
        memcpy(acl[i].remote_addr, peer, sizeof(peer));
    }
    lcb = (tL2C_LCB){.transport = BT_TRANSPORT_BR_EDR, .link_state = LST_CONNECTED};
    memcpy(lcb.remote_bd_addr, peer, sizeof(peer));
    if (pending) lcb.ccb_queue.p_first_ccb = lcb.p_pending_ccb = &channel;
    assert(BTM_IsAclConnectionUp(peer, BT_TRANSPORT_BR_EDR) && BTM_IsAclConnectionUp(peer, BT_TRANSPORT_LE));

    UINT8 event[] = {failed_receipt ? HCI_ERR_COMMAND_DISALLOWED : HCI_SUCCESS,
                    le_complete ? 0 : 1, le_complete ? 0 : 8, 0x16};
    if (synthetic) {
        assert(l2c_link_hci_disc_comp(0x801, 0x16));
    } else {
        btu_hcif_disconnection_comp_evt(event);
    }
    assert(retries == pending);
    bool should_retire = !failed_receipt && !synthetic && !le_complete;
    if (retry_succeeds) {
        assert(lcb_present && lcb.link_state == LST_CONNECTING);
        /* The stack can end a pending page via its terminal link timer. */
        l2c_link_timeout(&lcb);
    }
    assert(lcb_present == le_complete);
    assert(BTM_IsAclConnectionUp(peer, BT_TRANSPORT_LE) && !retires[BT_TRANSPORT_LE]);
    printf("%s host_br_acl=%u host_le_acl=%u br_retire=%u retries=%u\n", argv[1],
           BTM_IsAclConnectionUp(peer, BT_TRANSPORT_BR_EDR), BTM_IsAclConnectionUp(peer, BT_TRANSPORT_LE),
           retires[BT_TRANSPORT_BR_EDR], retries);
    fflush(stdout);
    assert(BTM_IsAclConnectionUp(peer, BT_TRANSPORT_BR_EDR) == !should_retire);
    assert(retires[BT_TRANSPORT_BR_EDR] == should_retire);
}
