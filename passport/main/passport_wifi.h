#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PASSPORT_WIFI_PROFILES 4
#define PASSPORT_WIFI_SCAN_ROWS 16

typedef enum { PW_OPEN, PW_WPA2, PW_UNSUPPORTED } passport_wifi_security_t;
typedef struct {
    uint8_t bytes[32], length;
} passport_ssid_t;
typedef struct {
    uint32_t id;
    passport_ssid_t ssid;
    passport_wifi_security_t security;
} passport_wifi_profile_t;
typedef struct {
    passport_ssid_t ssid;
    uint8_t bssid[6], channel;
    int8_t rssi;
    passport_wifi_security_t security;
    uint32_t profile_id;
    bool current;
} passport_wifi_row_t;
typedef enum { PW_OFFLINE, PW_SWITCHING, PW_CONNECTING, PW_ASSOCIATED, PW_ONLINE } passport_wifi_phase_t;
typedef enum { PW_REQUEST_NONE, PW_REQUEST_WAITING, PW_REQUEST_JOINING, PW_REQUEST_SAVED, PW_REQUEST_FAILED, PW_REQUEST_CANCELLED } passport_wifi_request_state_t;
typedef struct {
    bool initialized, storage_known, scanning, scan_known, scan_truncated, provisioning_available;
    bool ap_started, dhcp_running, ap_client, ap_lease, setup_open, pending;
    passport_wifi_phase_t phase;
    uint32_t revision, scan_revision, active_profile, pending_id, setup_id;
    unsigned profile_count, scan_count, setup_seconds, disconnect_reason;
    uint32_t last_submission_id;
    bool submission_accepted;
    passport_wifi_request_state_t request_state;
    char request_result[64];
    char sta_ip[16], ap_ip[16], ap_client_ip[16], result[64];
    passport_ssid_t active_ssid, pending_ssid;
    passport_wifi_security_t pending_security;
    passport_wifi_profile_t profiles[PASSPORT_WIFI_PROFILES];
    passport_wifi_row_t rows[PASSPORT_WIFI_SCAN_ROWS];
} passport_wifi_state_t;
typedef enum {
    PW_SCAN, PW_SCAN_CANCEL, PW_USE_AP, PW_DISCONNECT, PW_CONNECT, PW_FORGET,
    PW_SETUP_OPEN, PW_SETUP_CLOSE, PW_CONFIRM, PW_REJECT
} passport_wifi_action_t;
typedef struct {
    passport_wifi_action_t action;
    uint32_t id, revision;
} passport_wifi_command_t;
typedef struct {
    uint32_t setup_id, scan_revision, row, request_id;
    char password[64]; // WPA2 passphrase: 8..63 ASCII bytes; empty for open networks.
} passport_wifi_submission_t;

// Before the ADC/radios start; the AP receives fresh random credentials each boot.
void passport_wifi_prepare(void);
esp_err_t passport_wifi_start(void);
void passport_wifi_snapshot(passport_wifi_state_t *out);
bool passport_wifi_request(const passport_wifi_command_t *command);
// Only the native screen may receive AP credentials. Never pass these to HTTP.
void passport_wifi_ap_credentials(char ssid[32], char password[17]);
// Local setup windows are unavailable until the provisioning routes are installed.
void passport_wifi_set_provisioning_available(bool available);
bool passport_wifi_setup_token(char token[33]);
bool passport_wifi_submit(const passport_wifi_submission_t *submission, const char token[33]);
bool passport_wifi_scan(uint32_t setup_id, const char token[33]);
const char *passport_wifi_security_name(passport_wifi_security_t security);
const char *passport_wifi_phase_name(passport_wifi_phase_t phase);
// Preserve raw SSID bytes; unsupported device glyphs use an explicit hex spelling.
void passport_wifi_ssid_text(const passport_ssid_t *ssid, char *out, size_t capacity);
