// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_wifi.h"

#define APP_WIFI_PROFILE_CAPACITY 8U
#define APP_WIFI_PROFILE_ID_CAPACITY 33U
typedef struct {
    char id[APP_WIFI_PROFILE_ID_CAPACITY];
    char ssid[APP_WIFI_SSID_CAPACITY];
    bool enabled, current;
    uint8_t priority;
} app_wifi_profile_view_t;
typedef struct {
    bool known, uncertain, operation_pending;
    uint64_t revision;
    uint8_t count, capacity;
    esp_err_t error;
    uint64_t request_id, completed_request_id;
    esp_err_t operation_error;
    app_wifi_profile_view_t profiles[APP_WIFI_PROFILE_CAPACITY];
} app_wifi_profiles_snapshot_t;
typedef struct {
    uint64_t expected_revision;
    char id[APP_WIFI_PROFILE_ID_CAPACITY]; /* Empty creates; never silent eviction. */
    char ssid[APP_WIFI_SSID_CAPACITY];
    char password[APP_WIFI_PASSWORD_CAPACITY];
    bool keep_password, enabled, join;
} app_wifi_profile_save_t;
typedef struct {
    uint64_t request_id, revision;
    esp_err_t error;
    bool storage_known, storage_uncertain;
} app_wifi_profile_result_t;

/* Same app_wifi worker and RAM driver configuration own every path. Secrets
 * are write-only. New catalog banks preserve the legacy WIF1 record for old
 * firmware recovery. All ESP_OK returns acknowledge bounded admission only. */
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *);
esp_err_t app_wifi_profile_save(const app_wifi_profile_save_t *);
esp_err_t app_wifi_profile_join(const char *id, uint64_t expected_revision);
esp_err_t app_wifi_profile_forget(const char *id, uint64_t expected_revision);
esp_err_t
app_wifi_profiles_reorder(const char ids[APP_WIFI_PROFILE_CAPACITY][APP_WIFI_PROFILE_ID_CAPACITY],
                          uint8_t count, uint64_t expected_revision);
esp_err_t app_wifi_profiles_forget_all(uint64_t expected_revision, bool acknowledge_unknown_store);
/* Exact asynchronous operation identity for staged factory-reset ownership.
 * ESP_OK is admission; complete only when completed_request_id matches and
 * operation_pending is false. Uncertain durable state is reported separately. */
esp_err_t app_wifi_profiles_forget_all_request(uint64_t expected_revision,
                                              bool acknowledge_unknown_store,
                                              uint64_t *request_id);
/* Eight retained terminal results. INVALID_STATE means still pending;
 * NOT_FOUND means absent/evicted, never a substitute result for a later job. */
esp_err_t app_wifi_profile_result(uint64_t request_id, app_wifi_profile_result_t *);
