// SPDX-License-Identifier: Apache-2.0
// Synthetic controller and channel callbacks around the pinned L2CAP state machine.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
typedef UINT8 BD_ADDR[6];
#define TRUE 1
#define FALSE 0
#define CLASSIC_BT_INCLUDED TRUE
#define BD_ADDR_LEN 6
#define BLE_INCLUDED TRUE
enum { BT_TRANSPORT_BR_EDR = 1, BT_TRANSPORT_LE = 2 };
enum { LST_CONNECTING = 1, LST_CONNECTED, LST_DISCONNECTING, LST_CONNECT_HOLDING };
enum { CST_OPEN = 1, CST_W4_L2CAP_DISCONNECT_RSP, CST_W4_L2CA_DISCONNECT_RSP };
enum { HCI_SUCCESS = 0, HCI_ERR_MAX_NUM_OF_CONNECTIONS = 9, HCI_ERR_CONNECTION_EXISTS = 11,
       HCI_INVALID_HANDLE = 0xffff };
enum { BTM_BLI_PAGE_DONE_EVT, L2CAP_EXTENDED_FEATURES_INFO_TYPE, L2CEVT_LP_CONNECT_CFM,
       L2CEVT_LP_CONNECT_CFM_NEG, BTU_TTYPE_L2CAP_LINK, BTU_TTYPE_L2CAP_LINK_RETRY,
       L2CAP_ECHO_RSP_TOUT, L2CAP_LINK_STARTUP_TOUT, L2CAP_MAX_RECONNECT_ON_COLLISION,
       L2C_LP_CONN_RETRY_DELAY_TOUT };
#define L2CAP_TRACE_WARNING(...) ((void)0)
#define L2CAP_TRACE_ERROR(...) ((void)0)
typedef struct ccb { struct ccb *p_next_ccb; int in_use, chnl_state; } tL2C_CCB;
typedef struct { void *param; } test_timer_t;
typedef struct {
    int in_use, transport, link_state, link_role, is_bonding, br_edr_create_con_retries, disc_reason;
    UINT16 handle;
    struct { tL2C_CCB *p_first_ccb, *p_last_ccb; } ccb_queue;
    void *p_echo_rsp_cb;
    test_timer_t timer_entry, retry_timer_entry;
} tL2C_LCB;
typedef struct { int status; BD_ADDR bd_addr; } tL2C_CONN_INFO;
typedef struct { int dev_class, sec_bd_name; } tBTM_SEC_DEV_REC;
typedef struct node { tL2C_LCB *data; struct node *next; } list_node_t;
typedef void *TIMER_PARAM_TYPE;

static tL2C_LCB classic, other;
static tL2C_CCB connecting = {.in_use = 1}, other_ccb = {.in_use = 1, .chnl_state = CST_OPEN};
static list_node_t first, second;
static struct { list_node_t *p_lcb_pool; } l2cb;
static unsigned negative_callbacks, releases;
static BD_ADDR peer = {0x12, 2, 3, 4, 5, 6};

static list_node_t *list_begin(list_node_t *node) { return node; }
static list_node_t *list_next(list_node_t *node) { return node->next; }
static tL2C_LCB *list_node(list_node_t *node) { return node->data; }
static void btm_acl_update_busy_level(int event) { assert(event == BTM_BLI_PAGE_DONE_EVT); }
static tL2C_LCB *l2cu_find_lcb_by_bd_addr(BD_ADDR bda, int transport)
{
    assert(memcmp(bda, peer, sizeof(peer)) == 0 && transport == BT_TRANSPORT_BR_EDR);
    return &classic;
}
static void l2c_link_hci_disc_comp(UINT16 handle, int status) { (void)handle; (void)status; assert(false); }
static void l2cu_send_peer_info_req(tL2C_LCB *link, int type) { (void)link; (void)type; assert(false); }
static tBTM_SEC_DEV_REC *btm_find_dev(BD_ADDR addr) { (void)addr; assert(false); return NULL; }
#define btm_acl_created(...) assert(false)
#define BTM_SetLinkSuperTout(...) assert(false)
static bool l2cu_start_post_bond_timer(UINT16 handle) { (void)handle; assert(false); return false; }
#define l2c_process_held_packets(...) assert(false)
#define btu_stop_timer(...) assert(false)
#define l2cu_send_peer_echo_req(...) assert(false)
#define btu_start_timer(...) assert(false)
#define l2cu_create_conn(...) assert(false)
static void l2c_csm_execute(tL2C_CCB *ccb, int event, tL2C_CONN_INFO *info)
{
    assert(ccb == &connecting && event == L2CEVT_LP_CONNECT_CFM_NEG);
    assert(info->status == HCI_ERR_MAX_NUM_OF_CONNECTIONS);
    classic.ccb_queue.p_first_ccb = classic.ccb_queue.p_last_ccb = NULL;
    ++negative_callbacks;
}
static void l2cu_release_lcb(tL2C_LCB *link)
{
    assert(link == &classic);
    link->in_use = 0;
    ++releases;
}

#include "classic_connect_capacity_sdk.inc"

int main(int argc, char **argv)
{
    const struct {
        const char *name;
        int transport;
        int state;
        int channel_state;
        bool hold;
    } cases[] = {
        {"no-other", 0, 0, 0, false},
        {"br-busy", BT_TRANSPORT_BR_EDR, LST_CONNECTED, CST_OPEN, false},
        {"br-draining", BT_TRANSPORT_BR_EDR, LST_DISCONNECTING, 0, true},
        {"br-idle", BT_TRANSPORT_BR_EDR, LST_CONNECTED, 0, true},
        {"br-last-closing", BT_TRANSPORT_BR_EDR, LST_CONNECTED, CST_W4_L2CAP_DISCONNECT_RSP, true},
        {"br-connecting-empty", BT_TRANSPORT_BR_EDR, LST_CONNECTING, 0, false},
        {"le-fixed-only", BT_TRANSPORT_LE, LST_CONNECTED, 0, false},
        {"le-draining", BT_TRANSPORT_LE, LST_DISCONNECTING, 0, false},
    };
    assert(argc == 2);
    size_t index = 0;
    while (index < sizeof(cases) / sizeof(cases[0]) && strcmp(argv[1], cases[index].name)) ++index;
    assert(index < sizeof(cases) / sizeof(cases[0]));
    classic = (tL2C_LCB){.in_use = 1, .transport = BT_TRANSPORT_BR_EDR, .link_state = LST_CONNECTING,
                        .handle = HCI_INVALID_HANDLE, .ccb_queue = {&connecting, &connecting}};
    other = (tL2C_LCB){.in_use = cases[index].transport != 0, .transport = cases[index].transport,
                       .link_state = cases[index].state};
    if (cases[index].channel_state) {
        other_ccb.chnl_state = cases[index].channel_state;
        other.ccb_queue.p_first_ccb = other.ccb_queue.p_last_ccb = &other_ccb;
    }
    second = (list_node_t){.data = &other};
    first = (list_node_t){.data = &classic, .next = other.in_use ? &second : NULL};
    l2cb.p_lcb_pool = &first;
    assert(l2c_link_hci_conn_comp(HCI_ERR_MAX_NUM_OF_CONNECTIONS, HCI_INVALID_HANDLE, peer));
    bool held = classic.in_use && classic.link_state == LST_CONNECT_HOLDING;
    printf("%s held=%d negative_callbacks=%u releases=%u other_live=%d\n",
           argv[1], held, negative_callbacks, releases, other.in_use);
    fflush(stdout);
    assert(held == cases[index].hold);
    unsigned terminal = !cases[index].hold;
    assert(negative_callbacks == terminal && releases == terminal);
    assert(other.in_use == (cases[index].transport != 0));
}
