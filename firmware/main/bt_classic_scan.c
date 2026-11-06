// SPDX-License-Identifier: Apache-2.0
#include "bt_classic_scan.h"

#include <stdatomic.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hci/hci_layer.h"
#include "osi/thread.h"
#include "stack/btm_api.h"
#include "stack/btu.h"
#include "stack/hcimsgs.h"
#if CONFIG_APP_CLASSIC_CONNECT_DIAGNOSTICS
#include "bt_classic_connect_diag.h"
#endif

enum { NONE, QUEUED, DRAIN, OFF, HELD };
enum { SLOT_USED = 1, SLOT_ADDRESSED = 2, SCAN_STOPPING = 5 };

typedef struct {
    uint32_t epoch;
    esp_err_t status;
    uint8_t peer[6];
    bool addressed;
} scan_receipt_t;

/* One task-context lifetime lock for the fixed receipt and BTU queue handle.
 * The controller never waits for it. The mutex storage outlives BTU sessions. */
static StaticSemaphore_t gate_storage;
static _Atomic(SemaphoreHandle_t) gate;
static atomic_uint armed_epoch;
static atomic_uint stopping_epoch;
static uint32_t next_epoch;
static struct {
    osi_thread_t *thread;
    uint64_t request;
    uint32_t epoch, aborted_epoch;
    unsigned writes;
    uint8_t peer[6], desired_mask, phase;
    bool enabled, released, sent, reported, deferred;
    scan_receipt_t receipt;
} scan;

static SemaphoreHandle_t lock(TickType_t timeout)
{
    SemaphoreHandle_t current = atomic_load_explicit(&gate, memory_order_acquire);
    return current && xSemaphoreTake(current, timeout) == pdTRUE ? current : NULL;
}

static void retire(void)
{
    scan.phase = NONE;
    scan.request = 0;
    scan.receipt.epoch = 0;
}

/* An unobserved stop may still produce an end. It cannot be adopted by a
 * later request. The reserved token is never issued to a host request. */
static void mark_unknown_stop(void)
{
    uint32_t expected = 0;
    atomic_compare_exchange_strong_explicit(&stopping_epoch, &expected, UINT32_MAX,
                                            memory_order_acq_rel, memory_order_relaxed);
}

static void deliver(uint32_t epoch)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    uint64_t request = 0;
    scan_receipt_t result = {0};
    if (scan.epoch == epoch && scan.phase == HELD && !scan.released && !scan.reported &&
        scan.receipt.epoch == epoch) {
        scan.reported = true;
        request = scan.request;
        result = scan.receipt;
        if (result.addressed && memcmp(result.peer, scan.peer, sizeof(result.peer)))
            result.status = ESP_ERR_NOT_FINISHED;
    }
    xSemaphoreGive(held);
    if (!request) return;
    if (result.status == ESP_OK && result.addressed && !BTM_IsAclConnectionUp(result.peer, BT_TRANSPORT_BR_EDR))
        result.status = ESP_ERR_NOT_FINISHED;
    bt_audio_classic_scan_result(request, result.status);
}

static void receive(void *token)
{
    deliver((uint32_t)(uintptr_t)token);
}

static void publish(scan_receipt_t result)
{
    SemaphoreHandle_t held = lock(0);
    if (!held) return;
    if (scan.enabled && scan.epoch == result.epoch && scan.phase != NONE && !scan.released &&
        scan.receipt.epoch != result.epoch) {
        scan.receipt = result;
        if (!osi_thread_post(scan.thread, receive, (void *)(uintptr_t)result.epoch, 0, 0))
            scan.receipt.status = ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(held);
}

static void off_failed(uint32_t epoch)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    uint64_t drained = 0;
    if (scan.phase == OFF && scan.epoch == epoch) {
        scan.phase = HELD;
        if (scan.released) {
            drained = scan.request;
            retire();
        } else {
            scan.receipt = (scan_receipt_t){.epoch = epoch, .status = ESP_ERR_NO_MEM};
        }
    }
    xSemaphoreGive(held);
    if (drained) bt_audio_classic_scan_drained(drained, false);
    else deliver(epoch);
}

static void send_off(void)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    if (scan.phase != DRAIN || scan.writes) {
        xSemaphoreGive(held);
        return;
    }
    scan.phase = OFF;
    uint32_t epoch = scan.epoch;
    uint8_t mask = scan.desired_mask & HCI_INQUIRY_SCAN_ENABLED;
    xSemaphoreGive(held);

    BT_HDR *command = HCI_GET_CMD_BUF(1);
    if (!command) {
        off_failed(epoch);
        return;
    }
    command->len = HCIC_PREAMBLE_SIZE + 1;
    command->offset = 0;
    uint8_t *bytes = command->data;
    UINT16_TO_STREAM(bytes, HCI_WRITE_SCAN_ENABLE);
    UINT8_TO_STREAM(bytes, 1);
    UINT8_TO_STREAM(bytes, mask);
    hci_cmd_metadata_t *metadata = HCI_GET_CMD_METAMSG(command);
    metadata->context = (void *)(uintptr_t)epoch;

    held = lock(portMAX_DELAY);
    bool current = held && scan.enabled && scan.phase == OFF && scan.epoch == epoch;
    if (current) {
        scan.sent = true;
        atomic_store_explicit(&armed_epoch, epoch, memory_order_release);
    }
    if (held) xSemaphoreGive(held);
    if (current) btu_hcif_send_cmd(LOCAL_BR_EDR_CONTROLLER_ID, command);
    else HCI_FREE_CMD_BUF(command);
}

static void begin(void *token)
{
    uint32_t epoch = (uint32_t)(uintptr_t)token;
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    if (!scan.enabled || scan.epoch != epoch || scan.phase != QUEUED) {
        xSemaphoreGive(held);
        return;
    }
    bool reusable = BTM_IsAclConnectionUp(scan.peer, BT_TRANSPORT_BR_EDR);
    scan.phase = reusable ? HELD : DRAIN;
    if (reusable) scan.receipt = (scan_receipt_t){.epoch = epoch, .status = ESP_OK};
    xSemaphoreGive(held);
    if (reusable) deliver(epoch);
    else send_off();
}

esp_err_t bt_classic_scan_prepare(uint64_t request, const uint8_t peer[6])
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (request && scan.enabled && scan.phase == NONE && next_epoch < UINT32_MAX - 1) {
        scan.request = request;
        scan.epoch = ++next_epoch;
        scan.phase = QUEUED;
        scan.released = scan.sent = scan.reported = scan.deferred = false;
        memcpy(scan.peer, peer, sizeof(scan.peer));
        if (osi_thread_post(scan.thread, begin, (void *)(uintptr_t)scan.epoch, 0, 0)) result = ESP_OK;
        else {
            retire();
            result = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(held);
    return result;
}

bool bt_classic_scan_release(uint64_t request, bool *restore)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    *restore = false;
    if (!held) return true;
    bool drained = true;
    if (scan.request == request && scan.phase != NONE) {
        if (scan.phase == OFF) {
            scan.released = true;
            drained = false;
        } else {
            *restore = scan.sent || scan.deferred;
            retire();
        }
    }
    xSemaphoreGive(held);
    return drained;
}

bool bt_classic_scan_write(uint8_t mask, uint32_t epoch)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return !(mask & HCI_PAGE_SCAN_ENABLED);
    bool allow;
    if (epoch) {
        allow = scan.enabled && scan.phase == OFF && scan.epoch == epoch;
    } else {
        scan.desired_mask = mask;
        allow = scan.enabled ? scan.phase <= DRAIN : !(mask & HCI_PAGE_SCAN_ENABLED);
        if (scan.enabled && !allow) scan.deferred = true;
    }
    if (allow) ++scan.writes;
    xSemaphoreGive(held);
    return allow;
}

void bt_classic_scan_complete(uint32_t epoch, bool accepted)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    if (epoch && epoch == scan.aborted_epoch) {
        scan.aborted_epoch = 0;
        epoch = 0;
    } else if (epoch && (scan.epoch != epoch || scan.phase != OFF)) {
        xSemaphoreGive(held);
        return;
    }
    if (scan.writes) --scan.writes;
    uint64_t drained = 0;
    if (epoch) {
        uint32_t expected = epoch;
        if (atomic_compare_exchange_strong_explicit(&armed_epoch, &expected, 0,
                                                    memory_order_acq_rel, memory_order_relaxed))
            mark_unknown_stop();
        scan.phase = HELD;
        if (scan.released) {
            drained = scan.request;
            retire();
        } else if (!accepted) {
            scan.receipt = (scan_receipt_t){.epoch = epoch, .status = ESP_FAIL};
        }
    }
    xSemaphoreGive(held);
    if (drained) bt_audio_classic_scan_drained(drained, true);
    else if (epoch) deliver(epoch);
    else send_off();
}

bool bt_classic_scan_bind(struct osi_thread *thread)
{
    SemaphoreHandle_t current = atomic_load_explicit(&gate, memory_order_relaxed);
    if (!current) {
        current = xSemaphoreCreateMutexStatic(&gate_storage);
        if (!current) return false;
        atomic_store_explicit(&gate, current, memory_order_release);
    }
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return false;
    memset(&scan, 0, sizeof(scan));
    scan.thread = thread;
    scan.enabled = true;
    xSemaphoreGive(held);
    return true;
}

void bt_classic_scan_enable(void)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    scan.enabled = scan.thread != NULL;
    xSemaphoreGive(held);
}

static void disable(bool unbind)
{
    SemaphoreHandle_t held = lock(portMAX_DELAY);
    if (!held) return;
    scan.enabled = false;
    if (unbind) scan.thread = NULL;
    if (!unbind && scan.phase == OFF && scan.sent) scan.aborted_epoch = scan.epoch;
    if (atomic_exchange_explicit(&armed_epoch, 0, memory_order_acq_rel)) mark_unknown_stop();
    uint64_t request = scan.request;
    retire();
    xSemaphoreGive(held);
    if (request) {
        bt_audio_classic_scan_result(request, ESP_ERR_INVALID_STATE);
        bt_audio_classic_scan_drained(request, false);
    }
}

void bt_classic_scan_disable(void) { disable(false); }
void bt_classic_scan_unbind(void) { disable(true); }

/* The pinned controller's ODM/OLM handlers run on the controller message
 * task. The archive and call edges are checked by the target build. */
extern const uint8_t *r_olm_cfg_get(void);
extern const uint8_t *r_olm_get_conn_info(uint8_t index);
extern uint16_t r_olm_conn_get_state(uint8_t index);
extern void r_odm_get_scan_enable(uint8_t *mask);
extern uintptr_t __real_r_orca_hci_cc_evt_status_send(uint16_t opcode, uint8_t status);
extern void __real_r_olm_pscan_end(uint8_t link, const void *event);

static bool controller_context(void)
{
    return !xPortInIsrContext() && esp_cpu_get_core_id() == CONFIG_BT_CTRL_PINNED_TO_CORE;
}

static void sample(uint32_t epoch, bool can_wait)
{
    uint8_t mask = HCI_PAGE_SCAN_ENABLED;
    r_odm_get_scan_enable(&mask);
    const uint8_t *cfg = r_olm_cfg_get();
    const uint8_t *slot = cfg && cfg[2] == 1 ? r_olm_get_conn_info(0) : NULL;
    scan_receipt_t result = {.epoch = epoch, .status = ESP_ERR_INVALID_STATE};
    if (slot && !(mask & HCI_PAGE_SCAN_ENABLED)) {
        uint8_t flags = slot[32];
        if (!(flags & SLOT_USED)) {
            atomic_store_explicit(&stopping_epoch, 0, memory_order_release);
            result.status = ESP_OK;
        } else if (flags & SLOT_ADDRESSED) {
            atomic_store_explicit(&stopping_epoch, 0, memory_order_release);
            result.status = ESP_OK;
            result.addressed = true;
            memcpy(result.peer, slot + 18, sizeof(result.peer));
        } else {
            uint32_t expected = 0;
            if (can_wait && r_olm_conn_get_state(0) == SCAN_STOPPING &&
                atomic_compare_exchange_strong_explicit(&stopping_epoch, &expected, epoch,
                                                        memory_order_acq_rel, memory_order_relaxed)) return;
            result.status = ESP_ERR_NOT_FINISHED;
        }
    } else if (can_wait) mark_unknown_stop();
    publish(result);
}

/* Preserve the complete RV32 a0 return register across the adapter. */
uintptr_t __wrap_r_orca_hci_cc_evt_status_send(uint16_t opcode, uint8_t status)
{
    if (opcode == HCI_WRITE_SCAN_ENABLE) {
        uint32_t epoch = atomic_exchange_explicit(&armed_epoch, 0, memory_order_acq_rel);
        if (epoch && !status) {
            if (controller_context()) sample(epoch, true);
            else mark_unknown_stop();
        }
    }
    return __real_r_orca_hci_cc_evt_status_send(opcode, status);
}

void __wrap_r_olm_pscan_end(uint8_t link, const void *event)
{
#if CONFIG_APP_CLASSIC_CONNECT_DIAGNOSTICS
    bt_classic_connect_diag_scan_end(link, event);
#else
    __real_r_olm_pscan_end(link, event);
#endif
    if (link == 0 && atomic_load_explicit(&stopping_epoch, memory_order_acquire) && controller_context()) {
        uint32_t epoch = atomic_exchange_explicit(&stopping_epoch, 0, memory_order_acq_rel);
        if (epoch && epoch != UINT32_MAX) sample(epoch, false);
    }
}
