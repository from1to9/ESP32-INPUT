// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scan_stubs.h"
#include "bt_classic_scan.c"

#define UNUSED_ATTR __attribute__((unused))
#define TRUE 1
#define CLASSIC_BT_INCLUDED TRUE
#define BLE_INCLUDED 0
#define HCI_TRACE_ERROR(...) ((void)0)
#define HCI_TRACE_WARNING(...) ((void)0)
#define osi_free free

enum { HOST_TASK, BTU_TASK, CONTROLLER_TASK };
static unsigned context, commands, completions, end_calls, outcomes, drains;
static uint32_t tokens[16];
static uint8_t masks[16], odm_mask, slot_state, slot[56], config[] = {0, 0, 1};
static unsigned allocated;
static bool acl_up, full, isr, fail_alloc;
static uint64_t result_request, drain_request;
static esp_err_t last_result;
static bool must_restore;
static const uint8_t target[6] = {1, 2, 3, 4, 5, 6}, other[6] = {6, 5, 4, 3, 2, 1};
static osi_thread_t btu = {1};
static struct { void (*fn)(void *); void *arg; } queue[16];
static size_t head, tail;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    assert(context != CONTROLLER_TASK || timeout == 0);
    assert(!mutex->held);
    mutex->held = true;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex->held); mutex->held = false; }
int esp_cpu_get_core_id(void) { return 0; }
bool xPortInIsrContext(void) { return isr; }

bool osi_thread_post(osi_thread_t *thread, void (*fn)(void *), void *data, int index, uint32_t wait)
{
    assert(thread == &btu && thread->session && index == 0 && wait == 0);
    if (full) return false;
    assert(tail < sizeof(queue) / sizeof(*queue));
    queue[tail].fn = fn;
    queue[tail++].arg = data;
    return true;
}

static void step(void)
{
    assert(head < tail);
    context = BTU_TASK;
    size_t at = head++;
    queue[at].fn(queue[at].arg);
    context = HOST_TASK;
}

void bt_audio_classic_scan_result(uint64_t request, esp_err_t status)
{
    assert(context != CONTROLLER_TASK && !gate_storage.held);
    result_request = request;
    last_result = status;
    ++outcomes;
}
void bt_audio_classic_scan_drained(uint64_t request, bool restore)
{
    assert(context != CONTROLLER_TASK && !gate_storage.held);
    drain_request = request;
    must_restore = restore;
    ++drains;
}

bool BTM_IsAclConnectionUp(const uint8_t *peer, int transport)
{
    assert(context == BTU_TASK && transport == BT_TRANSPORT_BR_EDR && !memcmp(peer, target, 6));
    return acl_up;
}

const uint8_t *r_olm_cfg_get(void) { assert(context == CONTROLLER_TASK && !isr); return config; }
const uint8_t *r_olm_get_conn_info(uint8_t index) { assert(!index); return slot; }
uint16_t r_olm_conn_get_state(uint8_t index) { assert(!index); return slot_state; }
void r_odm_get_scan_enable(uint8_t *mask) { assert(context == CONTROLLER_TASK); *mask = odm_mask; }
uintptr_t __real_r_orca_hci_cc_evt_status_send(uint16_t opcode, uint8_t status)
{
    assert(context == CONTROLLER_TASK && opcode == HCI_WRITE_SCAN_ENABLE);
    ++completions;
    return UINT32_C(0x12345600) | status;
}
void __real_r_olm_pscan_end(uint8_t link, const void *event)
{
    assert(context == CONTROLLER_TASK && !link && !event);
    ++end_calls;
    slot[32] = 0;
    slot_state = 0;
}
#if CONFIG_APP_CLASSIC_CONNECT_DIAGNOSTICS
void bt_classic_connect_diag_scan_end(uint8_t link, const void *event) { __real_r_olm_pscan_end(link, event); }
#endif

BT_HDR *HCI_GET_CMD_BUF(size_t bytes)
{
    if (fail_alloc) return NULL;
    hci_cmd_metadata_t *packet = calloc(1, sizeof(*packet) + 3 + bytes);
    assert(packet);
    ++allocated;
    packet->command.layer_specific = HCI_CMD_BUF_TYPE_METADATA;
    return &packet->command;
}
void HCI_FREE_CMD_BUF(BT_HDR *packet)
{
    assert(allocated);
    --allocated;
    free(HCI_GET_CMD_METAMSG(packet));
}

static void btu_hcif_command_complete_evt(BT_HDR *response, void *arg) { (void)response; (void)arg; }
static void btu_hcif_command_status_evt(uint8_t status, BT_HDR *command, void *arg)
{ (void)status; (void)command; (void)arg; }
static void btu_hcif_hdl_command_complete(uint16_t opcode, uint8_t *data, uint16_t len, void *arg)
{ (void)opcode; (void)data; (void)len; (void)arg; assert(false); }

static void transmit(BT_HDR *packet, complete_cb complete, status_cb status, void *token)
{
    hci_cmd_metadata_t *metadata = HCI_GET_CMD_METAMSG(packet);
    assert(context == BTU_TASK && packet->len == 4 && packet->data[0] == 0x1a && packet->data[1] == 0x0c);
    assert(metadata->context == token && complete == btu_hcif_command_complete_evt && status == btu_hcif_command_status_evt);
    assert(commands < sizeof(masks));
    tokens[commands] = (uint32_t)(uintptr_t)token;
    masks[commands++] = packet->data[3];
    HCI_FREE_CMD_BUF(packet);
}
static const hci_t *hci_layer_get_interface(void)
{
    static const hci_t hci = {.transmit_command = transmit};
    return &hci;
}

#include "scan_btu.inc"

static void visibility(uint8_t mask)
{
    context = BTU_TASK;
    BT_HDR *packet = HCI_GET_CMD_BUF(1);
    assert(packet);
    packet->len = 4;
    memcpy(packet->data, (uint8_t[]){0x1a, 0x0c, 1, mask}, 4);
    btu_hcif_send_cmd(0, packet);
    context = HOST_TASK;
}

static void ack(uint32_t token, uint8_t status)
{
    context = BTU_TASK;
    BT_HDR *event = calloc(1, sizeof(*event) + sizeof(command_complete_hack_t));
    BT_HDR *response = calloc(1, sizeof(*response) + 6);
    assert(event && response);
    response->len = 6;
    memcpy(response->data, (uint8_t[]){0x0e, 4, 1, 0x1a, 0x0c, status}, 6);
    command_complete_hack_t *hack = (void *)event->data;
    hack->response = response;
    hack->context = (void *)(uintptr_t)token;
    btu_hcif_command_complete_evt_on_task(event);
    context = HOST_TASK;
}

static void controller_complete(uint8_t status)
{
    context = CONTROLLER_TASK;
    assert(__wrap_r_orca_hci_cc_evt_status_send(HCI_WRITE_SCAN_ENABLE, status) ==
           (UINT32_C(0x12345600) | status));
    context = HOST_TASK;
}

static void end(void)
{
    context = CONTROLLER_TASK;
    __wrap_r_olm_pscan_end(0, NULL);
    context = HOST_TASK;
    assert(end_calls == 1);
}

static void start(uint64_t request)
{
    assert(bt_classic_scan_prepare(request, target) == ESP_OK);
    step();
}

static void expect(unsigned count, uint64_t request, esp_err_t status)
{ assert(outcomes == count && result_request == request && last_result == status); }

int main(int argc, char **argv)
{
    assert(argc == 2 && bt_classic_scan_bind(&btu));
    const char *test = argv[1];
    bool restore;
    if (!strcmp(test, "drain")) {
        visibility(0);
        visibility(2);
        start(1);
        assert(commands == 2 && !outcomes);
        ack(0, 0);
        ack(99, 0); /* A stale tagged event cannot consume the second ordinary write. */
        assert(commands == 2);
        ack(0, 0);
        assert(commands == 3 && tokens[2] && masks[2] == 0);
        visibility(3);
        assert(commands == 3);
        controller_complete(0);
        step();
        assert(!outcomes);
        ack(tokens[2], 0);
        expect(1, 1, ESP_OK);
        assert(bt_classic_scan_release(1, &restore) && restore);
        visibility(3);
        assert(commands == 4 && masks[3] == 3);
    } else if (!strcmp(test, "async") || !strcmp(test, "async-before-ack")) {
        start(1);
        slot[32] = 1;
        slot_state = 5;
        controller_complete(0);
        assert(!outcomes && head == tail);
        if (!strcmp(test, "async")) {
            ack(tokens[0], 0);
            assert(!outcomes);
            end(); step();
        } else {
            end(); step();
            assert(!outcomes);
            ack(tokens[0], 0);
        }
        expect(1, 1, ESP_OK);
    } else if (!strcmp(test, "no-scan")) {
        start(1);
        controller_complete(0);
        ack(tokens[0], 0);
        expect(1, 1, ESP_OK);
        step();
        expect(1, 1, ESP_OK);
    } else if (!strcmp(test, "incoming")) {
        for (unsigned i = 0; i < 3; ++i) {
            start(i + 1);
            slot[32] = 3;
            slot_state = 6;
            memcpy(slot + 18, i == 0 ? other : target, sizeof(target));
            acl_up = i == 2;
            controller_complete(0);
            ack(tokens[i], 0);
            step();
            expect(i + 1, i + 1, i == 2 ? ESP_OK : ESP_ERR_NOT_FINISHED);
            assert(bt_classic_scan_release(i + 1, &restore) && restore);
            acl_up = false;
        }
    } else if (!strcmp(test, "reuse")) {
        acl_up = true;
        start(1);
        expect(1, 1, ESP_OK);
        assert(!commands && bt_classic_scan_release(1, &restore) && !restore);
        start(2);
        visibility(3);
        assert(!commands && bt_classic_scan_release(2, &restore) && restore);
    } else if (!strcmp(test, "cancel")) {
        start(1);
        slot[32] = 1;
        slot_state = 5;
        controller_complete(0);
        assert(!bt_classic_scan_release(1, &restore) && !restore);
        visibility(2);
        assert(commands == 1 && bt_classic_scan_prepare(2, target) == ESP_ERR_INVALID_STATE);
        ack(tokens[0], 0);
        assert(drains == 1 && drain_request == 1 && must_restore && !outcomes);
        visibility(2);
        ack(0, 0);
        start(2);
        controller_complete(0); /* State 5 still belongs to request 1. */
        ack(tokens[2], 0); step();
        expect(1, 2, ESP_ERR_NOT_FINISHED);
        end();
        assert(head == tail && outcomes == 1);
    } else if (!strcmp(test, "ack-failed")) {
        start(1);
        controller_complete(0);
        ack(tokens[0], 9);
        step();
        expect(1, 1, ESP_FAIL);
    } else if (!strcmp(test, "alloc-failed")) {
        fail_alloc = true;
        start(1);
        expect(1, 1, ESP_ERR_NO_MEM);
        assert(!commands && bt_classic_scan_release(1, &restore) && !restore);
    } else if (!strcmp(test, "queue-full")) {
        full = true;
        assert(bt_classic_scan_prepare(1, target) == ESP_ERR_NO_MEM && !commands);
        full = false;
        start(2);
        full = true;
        controller_complete(0);
        ack(tokens[0], 0);
        expect(1, 2, ESP_ERR_NO_MEM);
        assert(bt_classic_scan_release(2, &restore) && restore);
    } else if (!strcmp(test, "async-queue-full")) {
        start(1);
        slot[32] = 1; slot_state = 5;
        controller_complete(0); ack(tokens[0], 0);
        full = true;
        end();
        assert(!outcomes && head == tail);
        assert(bt_classic_scan_release(1, &restore) && restore);
    } else if (!strcmp(test, "teardown")) {
        start(1);
        controller_complete(0);
        bt_classic_scan_disable();
        expect(1, 1, ESP_ERR_INVALID_STATE);
        assert(drains == 1 && !must_restore);
        visibility(2);
        assert(commands == 1);
        bt_classic_scan_unbind();
        btu.session = 0;
        step();
        ack(tokens[0], 0);
        assert(outcomes == 1 && bt_classic_scan_prepare(2, target) == ESP_ERR_INVALID_STATE);
    } else if (!strcmp(test, "reenable")) {
        start(1);
        bt_classic_scan_disable();
        expect(1, 1, ESP_ERR_INVALID_STATE);
        bt_classic_scan_enable();
        visibility(2);
        start(2);
        assert(commands == 2);
        ack(0, 0);
        assert(commands == 2);
        controller_complete(0);
        ack(tokens[0], 0);
        assert(commands == 3 && tokens[2] != tokens[0]);
        controller_complete(0);
        ack(tokens[2], 0); step();
        expect(2, 2, ESP_OK);
    } else if (!strcmp(test, "unsupported-context")) {
        start(1);
        slot[32] = 1; slot_state = 5;
        isr = true;
        controller_complete(0);
        ack(tokens[0], 0);
        assert(!outcomes && head == tail);
        assert(bt_classic_scan_release(1, &restore) && restore);
        isr = false;
        start(2);
        controller_complete(0);
        ack(tokens[1], 0); step();
        expect(1, 2, ESP_ERR_NOT_FINISHED);
        end();
        assert(outcomes == 1 && head == tail);
    } else assert(false);
    assert(!allocated && !gate_storage.held);
    printf("%s observer=%d\n", test, CONFIG_APP_CLASSIC_CONNECT_DIAGNOSTICS);
}
