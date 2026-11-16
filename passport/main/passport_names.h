#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PASSPORT_NAME_BYTES 48
#define PASSPORT_NAME_SCALARS 16
#define PASSPORT_BOND_ID_BYTES 16
#define PASSPORT_AP_NAMES 16
#define PASSPORT_NAME_CAPACITY_BLE 3
#define PASSPORT_NAME_CAPACITY_PROFILES 4

typedef enum { PN_BLE, PN_WIFI } passport_name_domain_t;
typedef enum { PN_HID_BOND, PN_WIFI_PROFILE, PN_SCAN_AP } passport_name_kind_t;

typedef struct {
    uint8_t ssid[32], bssid[6], length, auth;
} passport_ap_name_key_t;
typedef struct {
    passport_name_kind_t kind;
    union {
        uint8_t bond[PASSPORT_BOND_ID_BYTES];
        uint32_t profile;
        passport_ap_name_key_t ap;
    };
} passport_name_key_t;
typedef struct {
    passport_name_key_t key;
    char label[PASSPORT_NAME_BYTES + 1];
} passport_name_entry_t;
typedef struct {
    uint32_t revision;
    unsigned count, saved_capacity, ap_capacity;
    bool known;
} passport_name_catalog_t;
typedef struct {
    passport_name_key_t key;
    uint32_t revision, scan_revision;
    bool clear;
    char label[PASSPORT_NAME_BYTES + 1];
} passport_name_change_t;
typedef enum {
    PN_OK, PN_NOT_FOUND, PN_INVALID, PN_LOCKED, PN_STALE, PN_FULL, PN_BUSY,
    PN_PENDING, PN_UNAVAILABLE, PN_UNCERTAIN, PN_RESULT_UNKNOWN,
} passport_name_result_t;

// The catalog revision applies to all entries in this domain. Read by index
// with that revision, then use the entry's key, never its index, for changes.
passport_name_catalog_t passport_names_catalog(passport_name_domain_t domain);
passport_name_result_t passport_names_entry(passport_name_domain_t domain, uint32_t revision,
                                            unsigned index, passport_name_entry_t *out);
passport_name_result_t passport_names_lookup(const passport_name_key_t *key, char *label);

bool passport_names_equal(const passport_name_key_t *a, const passport_name_key_t *b);
// Length is explicit so a caller can reject embedded NUL before making a request.
bool passport_names_text_valid(const char *label, size_t length);
// Admission only. The owner validates live identity, revision, Lock and storage.
// Results are kept per domain until the next completion. A caller that missed
// its ticket must read the catalog again before claiming a durable outcome.
passport_name_result_t passport_names_submit(const passport_name_change_t *change, uint32_t *ticket);
passport_name_result_t passport_names_result(passport_name_domain_t domain, uint32_t ticket);
