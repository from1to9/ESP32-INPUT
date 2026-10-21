// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_control_runtime.h"
#include "freertos/FreeRTOS.h"
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
/* PRODUCTION */
/* BOUNDARIES */
static uint64_t clock_ms;
static uint64_t now_ms(void) { return clock_ms; }
static unsigned mutations, publications;
static bool identity_live;
static unsigned interleave;
static void safety(void *, cp_safety_reason_t, const cp_identity_t *);
static app_wifi_snapshot_t native_state;
static void host_safety(void *u, cp_safety_reason_t r, const cp_identity_t *who)
{
    (void)u;
    (void)r;
    (void)who;
}
static esp_err_t start_native(work_t *w)
{
    assert(w->command.operation == CP_OP_WIFI_JOIN);
    ++mutations;
    return ESP_OK;
}
void app_wifi_get_snapshot(app_wifi_snapshot_t *s) { *s = native_state; }
void app_wifi_get_scan_snapshot(app_wifi_scan_snapshot_t *s) { *s = (app_wifi_scan_snapshot_t){0}; }
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *s)
{
    *s = (app_wifi_profiles_snapshot_t){0};
}
esp_err_t app_wifi_profile_result(uint64_t id, app_wifi_profile_result_t *out)
{
    (void)id;
    (void)out;
    return ESP_ERR_INVALID_STATE;
}
void app_update_snapshot(app_update_snapshot_t *s) { *s = (app_update_snapshot_t){0}; }
void app_update_publish_authority(const app_update_authority_t *a) { (void)a; }
cp_error_t app_control_runtime_update_authority(const cp_command_t *c, app_update_authority_t *a)
{
    (void)c;
    (void)a;
    return CP_FORBIDDEN;
}
esp_err_t app_control_server_complete(uint64_t key, const cp_result_t *r)
{
    assert(key && r);
    ++publications;
    return ESP_OK;
}
esp_err_t app_control_server_publish_state(const cp_device_state_t *s)
{
    assert(s);
    return ESP_OK;
}
bool app_control_server_identity_alive(const cp_identity_t *who)
{
    assert(who);
    if (interleave == 1) {
        safety(NULL, CP_SAFETY_LOCKED, NULL);
    }
    if (interleave == 2) {
        cp_device_state_t changed = runtime->state;
        ++changed.management_generation;
        assert(app_control_runtime_publish_state(&changed) == ESP_OK);
    }
    if (interleave == 3) {
        safety(NULL, CP_SAFETY_SESSION_LOST, who);
    }
    return identity_live;
}
/* PRODUCTION */
/* TESTS */
static runtime_t fixture;
static cp_command_t begin(void)
{
    memset(&fixture, 0, sizeof(fixture));
    runtime = &fixture;
    fixture.state_known = true;
    fixture.state.management_generation = 1;
    fixture.host.safety = host_safety;
    identity_live = true;
    interleave = 0;
    mutations = publications = 0;
    clock_ms = 100;
    native_state = (app_wifi_snapshot_t){.initialized = true, .revision = 1};
    cp_command_t c = {.operation = CP_OP_WIFI_JOIN,
                      .command_key = 1,
                      .deadline_ms = 1000,
                      .management_generation = 1,
                      .identity = {.scopes = CP_SCOPE_ALL, .session_id = {1}, .principal_id = {2}}};
    strcpy(c.args_json, "{\"ssid\":\"test-network\",\"password\":\"synthetic-secret\"}");
    cp_result_t result;
    assert(submit(NULL, &c, &result) == CP_OK);
    return c;
}
int main(void)
{
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        cp_command_t c = begin();
        if (scenario == 0) {
            fixture.state.locked = true;
            safety(NULL, CP_SAFETY_LOCKED, NULL);
        }
        if (scenario == 1) {
            identity_live = false;
            safety(NULL, CP_SAFETY_SESSION_LOST, &c.identity);
        }
        if (scenario == 2) {
            cp_device_state_t next = fixture.state;
            ++next.management_generation;
            assert(app_control_runtime_publish_state(&next) == ESP_OK);
        }
        if (scenario == 3) {
            clock_ms = c.deadline_ms;
        }
        if (scenario == 4) {
            fixture.state_known = false;
        }
        if (scenario == 5) {
            fixture.work[0].command.identity.scopes = CP_SCOPE_VIEW;
        }
        if (scenario == 6) {
            safety(NULL, CP_SAFETY_REVOKED, &c.identity);
        }
        if (scenario == 7) {
            safety(NULL, CP_SAFETY_LOCKED, NULL);
            fixture.state.locked = false;
        }
        process_work(&fixture.work[0]);
        assert(!mutations && !fixture.work[0].started && fixture.work[0].completion_pending);
        assert(fixture.work[0].result.state == CP_RESULT_CANCELLED &&
               fixture.work[0].result.error != CP_OK);
        assert(!fixture.work[0].command.args_json[0]);
        process_work(&fixture.work[0]);
        assert(!fixture.work[0].used && publications == 1);
    }
    for (unsigned event = 1; event <= 3; ++event) {
        (void)begin();
        interleave = event;
        process_work(&fixture.work[0]);
        assert(!mutations && !fixture.work[0].started && fixture.work[0].completion_pending);
    }
    const cp_safety_reason_t input_only[] = {CP_SAFETY_STATE_CHANGED, CP_SAFETY_LOCAL_OVERRIDE,
                                             CP_SAFETY_LEASE_EXPIRED, CP_SAFETY_CLIENT_STOP,
                                             CP_SAFETY_REQUEST_CONFLICT};
    for (unsigned i = 0; i < sizeof(input_only) / sizeof(input_only[0]); ++i) {
        cp_command_t c = begin();
        safety(NULL, input_only[i], &c.identity);
        process_work(&fixture.work[0]);
        assert(mutations == 1 && fixture.work[0].started);
    }
    cp_command_t c = begin();
    process_work(&fixture.work[0]);
    assert(mutations == 1);
    identity_live = false;
    fixture.state.locked = true;
    safety(NULL, CP_SAFETY_SESSION_LOST, &c.identity);
    native_state.revision = 2;
    native_state.lan_ready = true;
    strcpy(native_state.ssid, "test-network");
    process_work(&fixture.work[0]);
    assert(mutations == 1 && fixture.work[0].result.state == CP_RESULT_APPLIED);
    puts("PASS: actual queued runtime work rechecks session/scopes/lock/generation/deadline; "
         "input-only loss stays scoped and started effects retain truthful results");
    return 0;
}
