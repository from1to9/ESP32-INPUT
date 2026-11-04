// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Public presentation metadata only. No credential, session or lease token. */
#define APP_UI_CONTROLLER_ROWS 5U
enum { APP_UI_GRANT_INPUT = 1U, APP_UI_GRANT_MANAGE = 2U, APP_UI_GRANT_UPDATE = 4U };
typedef enum { APP_UI_GRANT_6H, APP_UI_GRANT_24H, APP_UI_GRANT_PERMANENT } app_ui_grant_duration_t;
typedef enum { APP_UI_GRANT_APPROVE, APP_UI_GRANT_EDIT, APP_UI_GRANT_REVOKE, APP_UI_GRANT_REJECT } app_ui_grant_op_t;
typedef struct {
    uint64_t revision, pairing_request;
    uint8_t id[16];
} app_controller_key_t;
static inline bool app_controller_key_equal(const app_controller_key_t *a, const app_controller_key_t *b)
{
    return a->revision && a->revision == b->revision && a->pairing_request == b->pairing_request &&
        !memcmp(a->id, b->id, sizeof(a->id));
}

typedef struct {
    app_controller_key_t key;
    app_ui_grant_op_t operation;
    uint8_t permissions;
    app_ui_grant_duration_t duration;
} app_controller_request_t;
typedef enum {
    APP_CONTROLLER_PAIRING, APP_CONTROLLER_AUTHORIZED, APP_CONTROLLER_EXPIRED,
    APP_CONTROLLER_REVOKED, APP_CONTROLLER_TIME_UNKNOWN, APP_CONTROLLER_DISABLED
} app_controller_state_t;
typedef struct {
    app_controller_key_t key;
    char label[33], fingerprint[65];
    app_controller_state_t state;
    uint8_t permissions;
    app_ui_grant_duration_t duration;
    bool legacy, connected, controlling;
} app_controller_row_t;
typedef struct {
    bool known, busy, uncertain, clock_known, applied;
    uint8_t count;
    app_controller_row_t rows[APP_UI_CONTROLLER_ROWS];
    app_controller_key_t selection;
    char feedback[96];
} app_controllers_view_t;
