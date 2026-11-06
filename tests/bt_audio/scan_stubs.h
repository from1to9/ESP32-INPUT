// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned TickType_t;
typedef struct { bool held; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout);
void xSemaphoreGive(SemaphoreHandle_t mutex);
bool xPortInIsrContext(void);

typedef struct osi_thread { unsigned session; } osi_thread_t;
bool osi_thread_post(osi_thread_t *thread, void (*callback)(void *), void *data, int queue, uint32_t wait);

enum {
    HCI_INQUIRY_SCAN_ENABLED = 1, HCI_PAGE_SCAN_ENABLED = 2,
    HCI_WRITE_SCAN_ENABLE = 0x0c1a, HCI_READ_LOCAL_NAME = 0x0c14,
    HCI_USER_CONF_VALUE_NEG_REPLY = 0x042d, HCI_GRP_VENDOR_SPECIFIC = 0xfc00,
    HCIC_PREAMBLE_SIZE = 3, HCI_CMD_BUF_TYPE_METADATA = 1,
    HCI_SUCCESS = 0, LOCAL_BR_EDR_CONTROLLER_ID = 0, BT_TRANSPORT_BR_EDR = 1
};
typedef uint8_t UINT8;
typedef uint16_t command_opcode_t;
typedef struct { uint16_t event, len, offset, layer_specific; uint8_t data[]; } BT_HDR;
typedef void (*complete_cb)(BT_HDR *, void *);
typedef void (*status_cb)(uint8_t, BT_HDR *, void *);
typedef struct {
    complete_cb command_complete_cb;
    status_cb command_status_cb;
    void *context;
    uint16_t opcode;
    BT_HDR command;
} hci_cmd_metadata_t;
typedef struct { BT_HDR *response; void *context; } command_complete_hack_t;
typedef struct { void (*transmit_command)(BT_HDR *, complete_cb, status_cb, void *); } hci_t;

#define STREAM_TO_UINT16(n, p) do { (n) = (uint16_t)((p)[0] | (p)[1] << 8); (p) += 2; } while (0)
#define UINT16_TO_STREAM(p, n) do { *(p)++ = (uint8_t)(n); *(p)++ = (uint8_t)((n) >> 8); } while (0)
#define UINT8_TO_STREAM(p, n) (*(p)++ = (n))
#define HCI_GET_CMD_METAMSG(p) ((void *)((uint8_t *)(p) - offsetof(hci_cmd_metadata_t, command)))
BT_HDR *HCI_GET_CMD_BUF(size_t bytes);
void HCI_FREE_CMD_BUF(BT_HDR *command);
void btu_hcif_send_cmd(UINT8 controller, BT_HDR *command);
bool BTM_IsAclConnectionUp(const uint8_t *peer, int transport);
