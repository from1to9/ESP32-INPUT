#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define PASSPORT_PEERS 3
#define PASSPORT_PEER_TEXT 24

typedef enum {
    PASSPORT_STOP_PAIR, PASSPORT_PAIR, PASSPORT_ADVERTISE, PASSPORT_DISCONNECT, PASSPORT_FORGET,
    PASSPORT_LOCK, PASSPORT_UNLOCK, PASSPORT_CANCEL, PASSPORT_MOVE,
    PASSPORT_CLICK, PASSPORT_KEY
} passport_command_kind_t;

typedef struct {
    passport_command_kind_t kind;
    uint32_t generation;
    uint32_t bond_revision;
    uint8_t peer_type, peer[6];
    int8_t x, y;
    uint8_t usage;
} passport_command_t;

typedef struct {
    uint8_t type, address[6];
    char label[PASSPORT_PEER_TEXT];
} passport_peer_t;

typedef struct {
    bool synced, storage_known, connected, encrypted, bonded;
    bool keyboard_subscribed, mouse_subscribed, ready, locked, advertising, pairing;
    bool release_unknown, release_pending;
    uint32_t cancel_revision;
    uint32_t generation;
    uint32_t bond_revision;
    unsigned peer_count, window_seconds;
    passport_peer_t peers[PASSPORT_PEERS];
    char current[PASSPORT_PEER_TEXT];
    char result[48];
} passport_ble_state_t;

esp_err_t passport_ble_start(void);
void passport_ble_snapshot(passport_ble_state_t *state);
// Admission only. Results and input readiness arrive in the next snapshot.
bool passport_ble_request(const passport_command_t *command);
void passport_ble_emergency_lock(void);
