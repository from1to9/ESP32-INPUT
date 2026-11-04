#!/usr/bin/env python3
"""Exercise pinned SMP encryption callbacks and the next peripheral LTK reply."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / ".tools/esp-idf")
    args = parser.parse_args()
    stack = args.sdk / "components/bt/host/bluedroid/stack"
    smp = (stack / "smp/smp_act.c").read_text()
    btm = (stack / "btm/btm_ble.c").read_text()
    machine = (stack / "smp/smp_main.c").read_text()
    internal = (stack / "smp/include/smp_int.h").read_text()
    api = (stack / "include/stack/smp_api.h").read_text()
    definitions = "\n".join(re.findall(r"^#define SMP_OPCODE_.*$", api, re.M)) + "\n"
    definitions += internal[internal.index("#define SMP_PAIRING_REQ_EVT"):
                            internal.index("typedef UINT8 tSMP_STATE;") + len("typedef UINT8 tSMP_STATE;")]
    definitions += "\n" + re.search(r"^#define SMP_PAIR_FLAG_ENC_AFTER_PAIR.*$", internal, re.M)[0] + "\n"
    tables = machine[machine.index("#define SMP_SM_IGNORE"):machine.index("#if SMP_DYNAMIC_MEMORY")]
    actions = re.search(r"smp_sm_action\[SMP_SM_NO_ACTION\] = \{(.*?)\};", tables, re.S)[1]
    names = re.findall(r"\bsmp_\w+\b", actions)
    declarations = "".join(f"void {name}(tSMP_CB *, tSMP_INT_DATA *);\n" for name in names)
    implemented = {"smp_check_auth_req", "smp_enc_cmpl", "smp_pairing_cmpl", "smp_key_distribution",
                   "smp_send_pair_fail", "smp_pair_terminate"}
    unused = "".join(f'void {name}(tSMP_CB *c, tSMP_INT_DATA *d) {{ '
                     '(void)c; (void)d; assert(!"Unexpected SMP action"); }\n'
                     for name in names if name not in implemented)
    actual = function(smp, "void smp_check_auth_req(") + function(smp, "void smp_link_encrypted(")
    actual += function(smp, "void smp_enc_cmpl(") + function(btm, "void btm_ble_ltk_request_reply(")
    actual += function(smp, "void smp_send_pair_fail(")
    actual += function(smp, "void smp_pair_terminate(")
    actual += function(machine, "void smp_set_state(") + function(machine, "void smp_sm_event(")
    boundary = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UINT8;
typedef bool BOOLEAN;
typedef uint8_t BD_ADDR[6];
typedef uint8_t BT_OCTET16[16];
typedef struct { UINT8 value; } tSMP_INT_DATA;
typedef struct {
    BD_ADDR pairing_bda;
    UINT8 flags, local_i_key, local_r_key, peer_auth_req, loc_auth_req;
    UINT8 role, state, loc_enc_size;
    UINT8 status, failure;
    bool le_secure_connections_mode_is_used, wait_for_authorization_complete;
} tSMP_CB;
typedef struct {
    BD_ADDR bd_addr;
    bool role_master, enc_init_by_we;
    unsigned ble_hci_handle;
    struct { UINT8 key_type; struct { UINT8 key_size; BT_OCTET16 lltk; } keys; } ble;
} tBTM_SEC_DEV_REC;
typedef struct { unsigned enc_handle; UINT8 key_size; } tBTM_CB;
#define TRUE true
#define FALSE false
#define BLE_INCLUDED TRUE
#define SMP_CTKD_INCLUDED FALSE
#define SMP_SEC_KEY_TYPE_ENC 1
#define SMP_SEC_KEY_TYPE_ID 2
#define SMP_SEC_KEY_TYPE_CSRK 4
#define SMP_SEC_KEY_TYPE_LK 8
#define SMP_SUCCESS 0
#define SMP_ENC_FAIL 0x14
#define SMP_MAX_FAIL_RSN_PER_SPEC 0x0e
#define SMP_CONN_TOUT 0x19
#define HCI_ROLE_MASTER 0
#define HCI_ROLE_SLAVE 1
#define BTM_LE_KEY_LENC (SMP_SEC_KEY_TYPE_ENC << 4)
#define BTM_LE_KEY_PENC SMP_SEC_KEY_TYPE_ENC
#define BD_ADDR_LEN 6
#define SMP_TRACE_DEBUG(...) ((void)0)
#define SMP_TRACE_EVENT(...) ((void)0)
#define SMP_TRACE_ERROR(...) ((void)0)
#define BTM_TRACE_DEBUG(...) ((void)0)
#define BTM_TRACE_ERROR(...) ((void)0)
static tSMP_CB smp_cb;
static tBTM_CB btm_cb;
static tBTM_SEC_DEV_REC peer;
static unsigned clears, authenticated, distribute, replies, negative_replies, sizes;
static UINT8 auth_reason;
static bool peer_exists;
static const BD_ADDR address = {0x42};
static const BT_OCTET16 old_ltk = {0x11, 0x22, 0x33};
static tBTM_SEC_DEV_REC *btm_find_dev(const UINT8 *bda) {
    return peer_exists && !memcmp(bda, peer.bd_addr, BD_ADDR_LEN) ? &peer : NULL;
}
static void btm_sec_clear_ble_keys(tBTM_SEC_DEV_REC *record) {
    ++clears; record->ble.key_type = 0; memset(&record->ble.keys, 0, sizeof(record->ble.keys));
}
static void btm_ble_update_sec_key_size(const UINT8 *bda, UINT8 size) {
    assert(!memcmp(bda, address, BD_ADDR_LEN)); peer.ble.keys.key_size = size; ++sizes;
}
static void smp_pairing_cmpl(tSMP_CB *control, tSMP_INT_DATA *data) {
    (void)control; auth_reason = *(UINT8 *)data; ++authenticated;
}
static void smp_proc_pairing_cmpl(tSMP_CB *control) { auth_reason = control->status; ++authenticated; }
static void smp_key_distribution(tSMP_CB *control, tSMP_INT_DATA *data) {
    (void)control; (void)data; ++distribute;
}
static void smp_send_cmd(UINT8 opcode, tSMP_CB *control) { (void)opcode; (void)control; }
static void btsnd_hcic_ble_ltk_req_reply(unsigned handle, const UINT8 *key) {
    assert(handle == 7); assert(!memcmp(key, old_ltk, sizeof(old_ltk))); ++replies;
}
static void btsnd_hcic_ble_ltk_req_neg_reply(unsigned handle) {
    assert(handle == 7); ++negative_replies;
}
'''
    checks = r'''
static void setup(void) {
    memset(&smp_cb, 0, sizeof(smp_cb)); memset(&peer, 0, sizeof(peer));
    memcpy(peer.bd_addr, address, sizeof(address));
    peer.ble_hci_handle = 7; peer.ble.key_type = BTM_LE_KEY_LENC | BTM_LE_KEY_PENC;
    peer.ble.keys.key_size = 16; memcpy(peer.ble.keys.lltk, old_ltk, sizeof(old_ltk));
    peer_exists = true; clears = authenticated = distribute = replies = negative_replies = sizes = 0;
}
int main(void) {
    /* Existing LTK: actual encryption callback, then the next connection's
     * actual key-reply function. No fresh pairing/key distribution occurs. */
    for (unsigned master = 0; master < 2; ++master) {
        setup(); peer.role_master = master != 0; peer.enc_init_by_we = master != 0;
        smp_link_encrypted(peer.bd_addr, 1);
        assert(authenticated == 1 && auth_reason == SMP_SUCCESS && !distribute && !sizes);
        assert(clears == 0 && peer.ble.keys.key_size == 16);
        assert(peer.ble.key_type == (BTM_LE_KEY_LENC | BTM_LE_KEY_PENC));
        btm_ble_ltk_request_reply(peer.bd_addr, false, NULL);
        assert(replies == 1 && negative_replies == 0);
    }
    /* A fresh authenticated pairing still replaces the old key exactly once,
     * including legacy pairing. SC must request distribution of its new LTK. */
    for (unsigned secure = 0; secure < 2; ++secure) {
        setup(); memcpy(smp_cb.pairing_bda, address, sizeof(address));
        smp_cb.state = SMP_STATE_ENCRYPTION_PENDING; smp_cb.role = HCI_ROLE_SLAVE;
        smp_cb.flags = SMP_PAIR_FLAG_ENC_AFTER_PAIR; smp_cb.loc_enc_size = 16;
        smp_cb.local_i_key = SMP_SEC_KEY_TYPE_ID; smp_cb.local_r_key = SMP_SEC_KEY_TYPE_ID;
        smp_cb.le_secure_connections_mode_is_used = secure != 0;
        smp_link_encrypted(peer.bd_addr, 1);
        assert(clears == 1 && !peer.ble.key_type && sizes == 1 && distribute == 1 && !authenticated);
        assert(!!(smp_cb.local_r_key & SMP_SEC_KEY_TYPE_ENC) == (secure != 0));
    }
    /* Neither failed new pairing nor failed old-key encryption erases a bond. */
    for (unsigned fresh = 0; fresh < 2; ++fresh) {
        setup(); memcpy(smp_cb.pairing_bda, address, sizeof(address));
        smp_cb.state = SMP_STATE_ENCRYPTION_PENDING;
        smp_cb.role = HCI_ROLE_SLAVE;
        smp_cb.flags = fresh ? SMP_PAIR_FLAG_ENC_AFTER_PAIR : 0;
        smp_link_encrypted(peer.bd_addr, 0);
        assert(!clears && !distribute && authenticated == fresh);
        if (fresh) assert(auth_reason == SMP_ENC_FAIL);
        btm_ble_ltk_request_reply(peer.bd_addr, false, NULL);
        assert(replies == 1 && !negative_replies);
    }
    setup(); memcpy(smp_cb.pairing_bda, address, sizeof(address));
    smp_cb.state = SMP_STATE_PAIR_REQ_RSP; smp_cb.role = HCI_ROLE_SLAVE;
    smp_cb.flags = SMP_PAIR_FLAG_ENC_AFTER_PAIR;
    smp_sm_event(&smp_cb, SMP_L2CAP_DISCONN_EVT, NULL);
    assert(!clears && !distribute && authenticated == 1 && auth_reason == SMP_CONN_TOUT);
    btm_ble_ltk_request_reply(peer.bd_addr, false, NULL);
    assert(replies == 1 && !negative_replies);
    /* Missing records and missing keys never fabricate an LTK. */
    setup(); peer_exists = false; smp_link_encrypted(peer.bd_addr, 1);
    btm_ble_ltk_request_reply(peer.bd_addr, false, NULL);
    assert(!clears && !authenticated && !replies && !negative_replies);
    setup(); peer.ble.key_type = 0; btm_ble_ltk_request_reply(peer.bd_addr, false, NULL);
    assert(!replies && negative_replies == 1);
    puts("Pinned SMP reconnect: existing LTK retained; fresh pairing replacement and failure controls passed.");
    return 0;
}
'''
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-sdk-reconnect-") as temporary:
        source = Path(temporary) / "sdk_reconnect.c"
        binary = Path(temporary) / "sdk_reconnect"
        state_boundary = '''
typedef void (*tSMP_ACT)(tSMP_CB *, tSMP_INT_DATA *);
void smp_sm_event(tSMP_CB *, tSMP_EVENT, void *);
#define SMP_ALL_TBL_MASK 0x80
'''
        source.write_text(boundary + definitions + state_boundary + declarations + unused + tables + actual + checks)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-Wno-unused-parameter",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
