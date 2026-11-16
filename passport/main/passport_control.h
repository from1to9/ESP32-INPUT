// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_protocol.h"
#include "hid_engine.h"

typedef struct {
    uint64_t transport_generation, network_generation;
    char target[CP_TARGET_CAPACITY], sta_ip[16], ap_ip[16];
    bool local_busy, transition;
} passport_control_observation_t;
typedef enum { PC_HTTP, PC_WS_OPEN, PC_WS_FRAME, PC_WS_HEARTBEAT } passport_control_request_kind_t;
typedef struct {
    passport_control_request_kind_t kind;
    char method[8], resource[128], host[32], origin[48], authorization[80], csrf[33], open[8], ticket[65];
    char body[CP_MAX_FRAME_BYTES + 1];
    size_t length;
    cp_identity_t identity; // Internal transport copy, never decoded from a request body.
    cp_http_response_t response;
    cp_error_t error;
} passport_control_request_t;
typedef struct { bool available, active; char controller[9]; } passport_control_view_t;

// All functions below belong to the sole NimBLE/HID owner. No network or SDK I/O.
bool passport_control_init(hid_engine_t *, cp_random_fn);
void passport_control_deinit(void);
void passport_control_observe(const passport_control_observation_t *, uint64_t now_ms);
void passport_control_service(uint64_t now_ms);
void passport_control_execute(passport_control_request_t *, uint64_t now_ms);
void passport_control_close(const cp_identity_t *, uint64_t now_ms);
void passport_control_network_reset(uint64_t now_ms);
// A physical takeover consumes its gesture; the UI must require a fresh press.
void passport_control_local(uint64_t now_ms);
bool passport_control_report_authorized(uint64_t now_ms);
void passport_control_view(passport_control_view_t *);
