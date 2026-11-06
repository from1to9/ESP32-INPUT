#!/usr/bin/env python3
"""Run pinned Classic failure cleanup, security deletion and the next LE LTK reply."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    match = re.search(re.escape(signature) + r"[^;{]*\{", source)
    if not match:
        raise ValueError(signature)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / ".tools/esp-idf")
    parser.add_argument("--case", choices=("idle", "le-live", "le-down", "br-live", "both-live", "success", "forget"))
    parser.add_argument("--reason", type=int, choices=(5, 9, 0x13))
    args = parser.parse_args()
    base = args.sdk / "components/bt/host/bluedroid"
    dm = (base / "bta/dm/bta_dm_act.c").read_text()
    dev = (base / "stack/btm/btm_dev.c").read_text()
    sec = (base / "stack/btm/btm_sec.c").read_text()
    ble = (base / "stack/btm/btm_ble.c").read_text()
    actual = function(dev, "BOOLEAN btm_find_sec_dev_in_list (")
    actual += function(dev, "tBTM_SEC_DEV_REC *btm_find_dev(")
    actual += function(sec, "void btm_sec_clear_ble_keys (")
    actual += function(dev, "void btm_sec_free_dev (")
    actual += function(dev, "BOOLEAN BTM_SecDeleteDevice (")
    actual += function(dev, "extern void BTM_SecClearSecurityFlags (")
    actual += function(dm, "static BOOLEAN bta_dm_remove_sec_dev_entry(")
    if "static BOOLEAN bta_dm_remove_bredr_sec_dev_entry(" in dm:
        actual += function(dm, "static BOOLEAN bta_dm_remove_bredr_sec_dev_entry(")
    actual += function(dm, "static UINT8 bta_dm_authentication_complete_cback(")
    actual += function(ble, "void btm_ble_ltk_request_reply(")
    boundary = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef bool BOOLEAN;
typedef uint8_t BD_ADDR[6];
typedef uint8_t BD_NAME[249];
typedef uint8_t DEV_CLASS[3];
typedef uint8_t BT_OCTET16[16];
typedef int tBT_TRANSPORT;
#define TRUE true
#define FALSE false
#define BLE_INCLUDED TRUE
#define SMP_INCLUDED TRUE
#define CLASSIC_BT_INCLUDED TRUE
#define BLE_PRIVACY_SPT FALSE
#define BLE_SMP_ID_RESET_ENABLE FALSE
#define GATTC_INCLUDED FALSE
#define BTM_SUCCESS 0
#define BTM_SEC_DEV_REC_REMOVED 1
#define BTM_AUTH_CLOSED_WINDOW 0x100
#define HCI_ERR_PAIRING_NOT_ALLOWED 0x18
#define BTA_DM_AUTH_CMPL_EVT 2
#define BT_TRANSPORT_INVALID 0
#define BT_TRANSPORT_BR_EDR 1
#define BT_TRANSPORT_LE 2
#define BOND_TYPE_UNKNOWN 0
#define BTM_SEC_STATE_IDLE 0
#define BTM_SM4_UNKNOWN 0
#define BTM_LE_KEY_NONE 0
#define BTM_LE_KEY_LENC 0x10
#define SEC_DEV_BDA 0
#define SEC_DEV_HDL 1
#define SEC_DEV_BTDM_BDA 2
#define BD_ADDR_LEN 6
#define BD_NAME_LEN 249
#define LINK_KEY_LEN 16
#define BTM_SEC_AUTHORIZED 0x01
#define BTM_SEC_AUTHENTICATED 0x02
#define BTM_SEC_ENCRYPTED 0x04
#define BTM_SEC_NAME_KNOWN 0x08
#define BTM_SEC_LINK_KEY_KNOWN 0x10
#define BTM_SEC_LINK_KEY_AUTHED 0x20
#define BTM_SEC_ROLE_SWITCHED 0x40
#define BTM_SEC_IN_USE 0x80
#define BTM_SEC_LE_AUTHORIZATION 0x100
#define BTM_SEC_LE_AUTHENTICATED 0x200
#define BTM_SEC_LE_ENCRYPTED 0x400
#define BTM_SEC_LE_NAME_KNOWN 0x800
#define BTM_SEC_LE_LINK_KEY_KNOWN 0x1000
#define BTM_SEC_LE_LINK_KEY_AUTHED 0x2000
#define BTM_SEC_16_DIGIT_PIN_AUTHED 0x4000
#define UNUSED(x) ((void)(x))
#define APPL_TRACE_DEBUG(...) ((void)0)
#define APPL_TRACE_WARNING(...) ((void)0)
#define APPL_TRACE_ERROR(...) ((void)0)
#define BTM_TRACE_DEBUG(...) ((void)0)
#define BTM_TRACE_WARNING(...) ((void)0)
#define BTM_TRACE_ERROR(...) ((void)0)
#define bdcmp(a,b) memcmp(a,b,6)
#define bdcpy(a,b) memcpy(a,b,6)
typedef struct { UINT8 key_size; BT_OCTET16 lltk; } tBTM_SEC_BLE_KEYS;
typedef struct {
    BD_ADDR bd_addr;
    unsigned sec_flags, sec_state, sm4, bond_type, hci_handle, ble_hci_handle;
    BT_OCTET16 link_key;
    struct { UINT8 key_type; BD_ADDR pseudo_addr; tBTM_SEC_BLE_KEYS keys; } ble;
} tBTM_SEC_DEV_REC;
typedef tBTM_SEC_DEV_REC list_node_t;
typedef struct { unsigned type; bool free_check; union {
    const UINT8 *p_bd_addr; unsigned handle;
} context; } tSecDevContext;
typedef struct {
    tBTM_SEC_DEV_REC *p_collided_dev_rec;
    struct { unsigned param; } sec_collision_tle;
    void *p_sec_dev_rec_list;
    unsigned enc_handle, key_size;
} tBTM_CB;
typedef struct { struct {
    BD_ADDR bd_addr; BD_NAME bd_name;
    UINT8 dev_type, addr_type, fail_reason;
    bool success;
} auth_cmpl; } tBTA_DM_SEC;
static struct {
    void (*p_sec_cback)(unsigned, tBTA_DM_SEC *);
    struct { unsigned count; struct {
        BD_ADDR peer_bdaddr; tBT_TRANSPORT transport; bool remove_dev_pending;
    } peer_device[2]; } device_list;
} bta_dm_cb;
static tBTM_CB btm_cb;
static tBTM_SEC_DEV_REC peer, other;
static const BD_ADDR address = {0x42};
static const BT_OCTET16 ltk = {0x12,0x34};
static bool le_up, br_up, exists;
static unsigned removed, br_key_deletes, failures, replies, negative_replies;
static unsigned failure_reason;
static const BD_ADDR bd_addr_null = {0};
static bool btm_ble_addr_resolvable(const UINT8 *bda, const tBTM_SEC_DEV_REC *record) {
    /* Public addresses and exact pseudo aliases; IRK cryptography is outside this fixture. */
    (void)bda; (void)record; return false;
}
static list_node_t *list_foreach(void *list, bool (*visit)(void *,void *), void *context) {
    assert(list == &peer);
    if (exists && !visit(&peer,context)) return &peer;
    return !visit(&other,context) ? &other : NULL;
}
static void *list_node(list_node_t *node) { return node; }
static bool BTM_IsAclConnectionUp(const UINT8 *bda, int transport) {
    return !memcmp(bda, address, 6) && (transport == BT_TRANSPORT_LE ? le_up : br_up);
}
static void BTM_DeleteStoredLinkKey(const UINT8 *bda, void *callback) {
    assert(!memcmp(bda, address, 6) && !callback); ++br_key_deletes;
}
static void btu_stop_timer(void *timer) { (void)timer; }
static void list_remove(void *list, void *record) {
    (void)list; assert(record == &peer); exists = false; ++removed;
}
static void BTM_ReadDevInfo(const UINT8 *bda, UINT8 *type, UINT8 *addr) {
    assert(!memcmp(bda, address, 6)); *type = 3; *addr = 0;
}
static void auth_event(unsigned event, tBTA_DM_SEC *data) {
    assert(event == BTA_DM_AUTH_CMPL_EVT && !data->auth_cmpl.success);
    assert(data->auth_cmpl.fail_reason == failure_reason && !memcmp(data->auth_cmpl.bd_addr,address,6));
    ++failures;
}
static void btsnd_hcic_ble_ltk_req_reply(unsigned handle, const UINT8 *key) {
    assert(handle == 0 && !memcmp(key,ltk,16)); ++replies;
}
static void btsnd_hcic_ble_ltk_req_neg_reply(unsigned handle) { assert(!handle); ++negative_replies; }
'''
    checks = r'''
int main(int argc, char **argv) {
    assert(argc == 3); const char *scenario = argv[1]; failure_reason = (unsigned)atoi(argv[2]);
    exists = true; memcpy(peer.bd_addr,address,6);
    btm_cb.p_sec_dev_rec_list = &peer;
    peer.sec_flags = BTM_SEC_IN_USE | BTM_SEC_LINK_KEY_KNOWN | BTM_SEC_LE_LINK_KEY_KNOWN;
    peer.ble.key_type = BTM_LE_KEY_LENC; peer.ble.keys.key_size = 16;
    memcpy(peer.ble.keys.lltk,ltk,16); memset(peer.link_key,0x7a,16);
    other = peer; other.bd_addr[0] = 0x43;
    tBTM_SEC_DEV_REC unchanged_other = other;
    BD_ADDR alias = {0x54}; memcpy(peer.ble.pseudo_addr,alias,6);
    assert(btm_find_dev(peer.bd_addr) == &peer && btm_find_dev(alias) == &peer);
    assert(btm_find_dev(other.bd_addr) == &other);
    bta_dm_cb.p_sec_cback = auth_event;
    bool both = !strcmp(scenario,"both-live");
    le_up = both || !strcmp(scenario,"le-live"); br_up = both || !strcmp(scenario,"br-live");
    if (le_up || br_up || !strcmp(scenario,"le-down")) {
        bta_dm_cb.device_list.count = 1;
        memcpy(bta_dm_cb.device_list.peer_device[0].peer_bdaddr,address,6);
        bta_dm_cb.device_list.peer_device[0].transport = br_up && !both ? BT_TRANSPORT_BR_EDR : BT_TRANSPORT_LE;
        if (both) {
            bta_dm_cb.device_list.count = 2;
            memcpy(bta_dm_cb.device_list.peer_device[1].peer_bdaddr,address,6);
            bta_dm_cb.device_list.peer_device[1].transport = BT_TRANSPORT_BR_EDR;
        }
    }
    if (!strcmp(scenario,"forget")) {
        /* Explicit removal remains a separate, working security operation. */
        assert(BTM_SecDeleteDevice(peer.bd_addr,BT_TRANSPORT_LE));
        assert(!peer.ble.key_type && peer.link_key[0] == 0x7a && !br_key_deletes);
        assert(BTM_SecDeleteDevice(peer.bd_addr,BT_TRANSPORT_BR_EDR));
        assert(removed == 1 && br_key_deletes == 1);
        puts("forget: explicit LE and BR/EDR deletion retained"); return 0;
    }
    DEV_CLASS dev_class = {0}; BD_NAME name = "Synthetic host";
    bool success = !strcmp(scenario,"success");
    UINT8 result = bta_dm_authentication_complete_cback(peer.bd_addr,dev_class,name,success ? 0 : (int)failure_reason);
    /* Verify the actual next LTK response before asserting record retention. */
    btm_ble_ltk_request_reply(peer.bd_addr,FALSE,NULL);
    printf("%s reason=%u: result=%u failures=%u LTK replies=%u negative=%u removed=%u pending=%u\n",
           scenario,failure_reason,result,failures,replies,negative_replies,removed,
           bta_dm_cb.device_list.peer_device[0].remove_dev_pending);
    fflush(stdout);
    assert(result == (!success && !br_up ? BTM_SEC_DEV_REC_REMOVED : BTM_SUCCESS) && failures == !success);
    assert(exists && !removed && replies == 1 && !negative_replies);
    assert(peer.sec_flags & BTM_SEC_LE_LINK_KEY_KNOWN);
    assert(br_key_deletes == (!success && !br_up));
    assert(bta_dm_cb.device_list.peer_device[both ? 1 : 0].remove_dev_pending == (br_up && !success));
    if (both) assert(!bta_dm_cb.device_list.peer_device[0].remove_dev_pending);
    if (br_up && !success) {
        /* The marked Classic link later drops; its existing deletion path
         * must still leave the LE service's next LTK response intact. */
        br_up = false; assert(BTM_SecDeleteDevice(peer.bd_addr,BT_TRANSPORT_BR_EDR));
        btm_ble_ltk_request_reply(peer.bd_addr,FALSE,NULL);
        assert(exists && replies == 2 && !negative_replies && peer.ble.key_type == BTM_LE_KEY_LENC);
    }
    assert(!memcmp(&other,&unchanged_other,sizeof(other)));
    return 0;
}
'''
    compiler = shutil.which("clang") or "cc"
    with tempfile.TemporaryDirectory(prefix="esp32-classic-auth-failure-") as directory:
        source = Path(directory) / "test.c"
        binary = Path(directory) / "test"
        source.write_text(boundary + actual + checks)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        str(source), "-o", str(binary)], check=True)
        for case in ([args.case] if args.case else ("idle", "le-live", "le-down", "br-live", "both-live", "success", "forget")):
            reasons = [args.reason] if args.reason is not None else ((5,) if case in ("success", "forget") else (5, 9, 0x13))
            for reason in reasons:
                subprocess.run([str(binary), case, str(reason)], check=True)


if __name__ == "__main__":
    main()
