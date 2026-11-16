#pragma once
#include "passport_wifi.h"

typedef struct {
    uint32_t id;
    uint8_t ssid_length, security, password_length, reserved;
    uint8_t ssid[32];
    char password[64];
} passport_wifi_record_t;
typedef struct {
    uint32_t version, count, next_id, preferred;
    passport_wifi_record_t records[PASSPORT_WIFI_PROFILES];
} passport_wifi_store_t;

esp_err_t passport_wifi_store_load(passport_wifi_store_t *store);
esp_err_t passport_wifi_store_save(const passport_wifi_store_t *store);
