#!/usr/bin/env python3
"""Exercise the actual pinned SDK Just Works dispatch function after patching."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    subprocess.run(["python3", str(ROOT / "tools/idf_patches.py"), "--check"], check=True)
    source = (ROOT / ".tools/esp-idf/components/bt/host/bluedroid/btc/core/btc_dm.c").read_text()
    start = source.index("static void btc_dm_sp_cfm_req_evt(")
    end = source.index("\nstatic void ", start + 1)
    actual_function = source[start:end]
    boundary = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { bool just_works; uint8_t bd_addr[6]; uint32_t num_val; } tBTA_DM_SP_CFM_REQ;
typedef union { struct { uint8_t bda[6]; uint32_t num_val; } cfm_req; } esp_bt_gap_cb_param_t;
typedef struct { unsigned int sig, pid, act; uint8_t arg[]; } btc_msg_t;
typedef int bt_status_t;
#define ESP_BD_ADDR_LEN 6
#define BTC_SIG_API_CB 1
#define BTC_PID_GAP_BT 2
#define BTC_GAP_BT_CFM_REQ_EVT 3
#define BT_STATUS_SUCCESS 0
#define BTC_TRACE_ERROR(...) ((void)0)
static bool allocation_failure, dispatch_failure;
static unsigned int allocations, releases, dispatches, automatic_accepts;
void *osi_malloc(size_t bytes) {
    if (allocation_failure) return NULL;
    ++allocations; return malloc(bytes);
}
void osi_free(void *pointer) { ++releases; free(pointer); }
void BTA_DmConfirm(const uint8_t *peer, bool accept) { (void)peer; if (accept) ++automatic_accepts; }
bt_status_t btc_inter_profile_call(btc_msg_t *message) {
    assert(message->sig == BTC_SIG_API_CB && message->pid == BTC_PID_GAP_BT && message->act == BTC_GAP_BT_CFM_REQ_EVT);
    const esp_bt_gap_cb_param_t *event = (const void *)message->arg;
    assert(event->cfm_req.bda[0] == 0x42 && event->cfm_req.num_val == 123456);
    ++dispatches; return dispatch_failure ? -1 : BT_STATUS_SUCCESS;
}
'''
    checks = r'''
int main(void) {
    tBTA_DM_SP_CFM_REQ request = {.just_works = true, .bd_addr = {0x42}, .num_val = 123456};
    btc_dm_sp_cfm_req_evt(&request);
    assert(dispatches == 1 && automatic_accepts == 0 && allocations == releases);
    request.just_works = false; btc_dm_sp_cfm_req_evt(&request);
    assert(dispatches == 2 && automatic_accepts == 0 && allocations == releases);
    allocation_failure = true; btc_dm_sp_cfm_req_evt(&request);
    assert(dispatches == 2 && automatic_accepts == 0 && allocations == releases);
    allocation_failure = false; dispatch_failure = true; request.just_works = true;
    btc_dm_sp_cfm_req_evt(&request);
    assert(dispatches == 3 && automatic_accepts == 0 && allocations == releases);
    return 0;
}
'''
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-sdk-admission-") as temporary:
        test = Path(temporary) / "sdk_admission.c"
        binary = Path(temporary) / "sdk_admission"
        test.write_text(boundary + actual_function + checks)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-fsanitize=address,undefined", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("Actual SDK Just Works dispatch: admission, allocation and dispatch failure checks passed.")


if __name__ == "__main__":
    main()
