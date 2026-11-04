// SPDX-License-Identifier: Apache-2.0
/* Only copied owner/clock/telemetry identity ports are substituted. The actual
 * diagnostic encoder and exact runtime dispatch execute unchanged. No SDK,
 * network, Keychain, device or mutable owner API is linked. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_runtime_diagnostics.h"
#include "app_control_runtime.h"
#include "app_audio.h"
#include "board_audio_mosaico.h"
#include "usb_audio.h"
#include "usb_hid.h"
#include "ble_hid.h"
#include "app_sensor_api.h"
#include "control_json.h"
#include "control_sensor_usb.h"
static struct {
    app_wifi_snapshot_t wifi;
    app_wifi_scan_snapshot_t scan;
    app_wifi_scan_diagnostics_t scan_diagnostics;
    app_control_runtime_snapshot_t control;
    usb_hid_snapshot_t usb;
    usb_audio_snapshot_t uac;
    ble_hid_snapshot_t ble;
    ble_hid_failure_t failure;
    ae_snapshot_t audio;
    board_audio_status_t codec;
    app_update_snapshot_t update;
} owners;
static bool audio_known, reenter;
static int64_t clock_us = 1234567;
static cp_error_t catalog_error;
static const char *catalog = "{\"boot\":\"0123456789abcdef0123456789abcdef\",\"git_commit\":\"0123456789abcdef0123456789abcdef01234567\",\"git_state\":\"clean\",\"elf_sha256\":null,\"config_sha256\":null}";
static unsigned catalog_reads, owner_reads;
static cj_document_t document;
static char output[CP_MAX_RESPONSE_BYTES + 1U];
int64_t esp_timer_get_time(void) { return clock_us; }
void app_wifi_get_snapshot(app_wifi_snapshot_t *out) { ++owner_reads; *out = owners.wifi; }
void app_wifi_get_scan_snapshot(app_wifi_scan_snapshot_t *out) { ++owner_reads; *out = owners.scan; }
void app_wifi_get_scan_diagnostics(app_wifi_scan_diagnostics_t *out) { ++owner_reads; *out = owners.scan_diagnostics; }
void app_control_runtime_snapshot(app_control_runtime_snapshot_t *out) { ++owner_reads; *out = owners.control; }
usb_hid_snapshot_t usb_hid_snapshot(void) { ++owner_reads; return owners.usb; }
usb_audio_snapshot_t usb_audio_snapshot(void) { ++owner_reads; return owners.uac; }
ble_hid_snapshot_t ble_hid_snapshot(void) { ++owner_reads; return owners.ble; }
ble_hid_failure_t ble_hid_failure(void) { ++owner_reads; return owners.failure; }
bool app_audio_snapshot(ae_snapshot_t *out) { ++owner_reads; *out = owners.audio; return audio_known; }
board_audio_status_t board_audio_mosaico_status(void) { ++owner_reads; return owners.codec; }
void app_update_snapshot(app_update_snapshot_t *out) { ++owner_reads; *out = owners.update; }
bool app_update_failure_json(uint64_t job_id, uint64_t revision, char *out, size_t capacity, size_t *length)
{
    (void)job_id; (void)revision;
    if (capacity < 5) return false;
    memcpy(out, "null", 5); *length = 4; return true;
}
cp_error_t app_sensor_api_read(const char *resource, char *out, size_t capacity, size_t *length)
{
    assert(!strcmp(resource, "/api/v1/sensors/catalog")); ++catalog_reads;
    if (reenter) {
        char nested[32]; size_t count = 99;
        assert(app_runtime_diagnostics_read(nested, sizeof(nested), &count) == CP_BUSY && !count && !nested[0]);
    }
    if (catalog_error) return catalog_error;
    size_t count = strlen(catalog);
    if (count >= capacity) return CP_TOO_LARGE;
    memcpy(out, catalog, count + 1U); *length = count; return CP_OK;
}
/* Other existing read resources remain at their named owning boundaries. */
bool app_control_trust_update_policy(const uint8_t principal[16], app_update_policy_t *out)
{ (void)principal; (void)out; return false; }
static bool update_policy(const uint8_t principal[16], app_update_policy_t *out)
{ return app_control_trust_update_policy(principal, out); }
void app_wifi_profiles_snapshot(app_wifi_profiles_snapshot_t *out) { memset(out, 0, sizeof(*out)); }
void app_clock_snapshot(app_clock_snapshot_t *out) { *out = owners.control.clock; }
void app_control_trust_snapshot(app_control_trust_snapshot_t *out) { *out = owners.control.trust; }
size_t app_control_trust_grants(const uint8_t principal[16], cp_capture_grant_t out[CP_MAX_CAPTURE_GRANTS])
{ (void)principal; (void)out; return 0; }
esp_err_t app_wifi_get_scan_record(uint32_t generation, uint16_t index, app_wifi_scan_record_t *out)
{ (void)generation; (void)index; (void)out; return ESP_ERR_NOT_FOUND; }
static void hex(const uint8_t *bytes, size_t count, char *out)
{ (void)bytes; memset(out, '0', count * 2U); out[count * 2U] = 0; }
/* The selected runtime read function accesses only its copied host read port. */
static struct { app_control_host_t host; } runtime_boundary;
#define runtime (&runtime_boundary)
/* PRODUCTION */
/* TESTS */
static uint16_t child(uint16_t node, const char *key)
{ uint16_t result = cj_get(&document, node, key); assert(result != CJ_NONE); return result; }
static void read_ok(void)
{
    size_t count = 0;
    unsigned char before[sizeof(owners)]; memcpy(before, &owners, sizeof(owners));
    assert(app_runtime_diagnostics_read(output, sizeof(output), &count) == CP_OK);
    assert(count > 0 && count <= CP_MAX_RESPONSE_BYTES && output[count] == 0 && cj_parse(&document, output, count));
    assert(!memcmp(before, &owners, sizeof(owners)));
}
int main(void)
{
    owners.update.error = ESP_ERR_INVALID_STATE;
    read_ok();
    assert(document.nodes[child(child(0, "wifi"), "health")].type == CJ_NULL);
    assert(document.nodes[child(child(0, "wifi"), "scan")].type == CJ_NULL);
    assert(document.nodes[child(child(0, "control"), "health")].type == CJ_NULL);
    assert(document.nodes[child(child(0, "usb_hid"), "error")].type == CJ_NULL);
    assert(document.nodes[child(child(0, "audio"), "health")].type == CJ_NULL);
    assert(document.nodes[child(child(0, "update"), "running_confirmed")].type == CJ_NULL);
    assert(document.nodes[child(child(0, "update"), "health_confirmation_error")].type == CJ_NULL);
    owners.wifi.initialized = owners.wifi.lan_ready = true; owners.wifi.state = APP_WIFI_CONNECTED;
    strcpy(owners.wifi.ssid, "PRIVATE_SSID_SENTINEL"); strcpy(owners.wifi.stored_ssid, "PRIVATE_STORED_SENTINEL");
    strcpy(owners.wifi.setup_ssid, "PRIVATE_AP_SENTINEL"); strcpy(owners.wifi.ip, "192.0.2.253");
    owners.scan_diagnostics.current = (app_wifi_scan_observation_t){.known = true, .generation = 9, .phase = APP_WIFI_SCAN_PHASE_COMPLETE};
    owners.scan_diagnostics.last_failure = (app_wifi_scan_observation_t){.known = true, .generation = 7, .phase = APP_WIFI_SCAN_PHASE_SDK_START, .error = ESP_FAIL};
    owners.control.initialized = true; owners.control.trust.known = true;
    strcpy(owners.control.server.address, "PRIVATE_SERVER_ADDRESS_SENTINEL");
    owners.usb.initialized = owners.uac.initialized = owners.ble.initialized = true;
    owners.ble.connected = owners.ble.ready = true; memset(owners.ble.peer_address, 0xa5, 6);
    owners.failure = (ble_hid_failure_t){.recorded = true, .stage = "First SDK \"failure\"", .error = ESP_FAIL, .uptime_ms = 9};
    audio_known = true;
    owners.audio.hardware_off_confirmed = true;
    owners.codec.worker_started = owners.codec.probe_complete = true; owners.codec.probe_result = AE_OK;
    reenter = true; read_ok(); reenter = false;
    assert(!strstr(output, "PRIVATE_") && !strstr(output, "192.0.2.253") && !strstr(output, "peer_address"));
    assert(strstr(output, "First SDK \\\"failure\\\"") && strstr(output, "\"last_failure\":{\"generation\":7"));
    assert(document.nodes[child(child(child(0, "ble"), "first_failure"), "sdk_status")].type == CJ_NULL);
    owners.failure.stage = NULL; read_ok();
    assert(document.nodes[child(child(child(0, "ble"), "first_failure"), "stage")].type == CJ_NULL);
    assert(document.nodes[child(child(child(0, "ble"), "first_failure"), "error")].value.integer == ESP_FAIL);
    owners.failure.stage = "First SDK failure";
    owners.usb.generation = owners.usb.accepted = owners.usb.submitted = owners.usb.completed = UINT64_MAX;
    owners.uac.link_generation = owners.uac.audio_generation = owners.ble.generation = UINT64_MAX;
    owners.audio.audio_generation = owners.audio.revision = owners.update.revision = UINT64_MAX;
    owners.failure.uptime_ms = UINT64_MAX; owners.failure.sdk_status_known = true; owners.failure.sdk_status = UINT32_MAX;
    owners.wifi.revision = owners.control.revision = UINT32_MAX;
    owners.scan_diagnostics.current.heap_known = owners.scan_diagnostics.last_failure.heap_known = true;
    owners.scan_diagnostics.current.free_internal = owners.scan_diagnostics.current.largest_internal = UINT32_MAX;
    owners.scan_diagnostics.current.free_dma = owners.scan_diagnostics.current.largest_dma = UINT32_MAX;
    owners.scan_diagnostics.last_failure = owners.scan_diagnostics.current;
    owners.scan_diagnostics.last_failure.count_known = true; owners.scan_diagnostics.last_failure.driver_count = UINT16_MAX;
    owners.scan_diagnostics.last_failure.event_status_known = true; owners.scan_diagnostics.last_failure.event_status = UINT32_MAX;
    static const char full_identity[] = "{\"boot\":\"0123456789abcdef0123456789abcdef\",\"git_commit\":\"0123456789abcdef0123456789abcdef01234567\",\"git_state\":\"unknown\",\"elf_sha256\":\"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\",\"config_sha256\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\"}";
    catalog = full_identity;
    owners.wifi.last_error = owners.wifi.connection_error = owners.wifi.storage_error = owners.wifi.backend_error = INT_MIN;
    owners.control.error = owners.control.server.error = owners.control.trust.error = owners.control.clock.error = INT_MIN;
    owners.usb.error = owners.uac.error = owners.failure.error = owners.update.error = INT_MIN;
    owners.update.running_state_error = owners.update.health_confirmation_error = INT_MIN;
    owners.update.health_confirmation_attempted = true; owners.codec.first_sdk_error = INT_MIN;
    read_ok(); printf("Runtime diagnostics maximum-field fixture: %zu bytes\n", strlen(output));
    size_t count = 999; char small[1024]; unsigned reads_before_small = owner_reads;
    assert(app_runtime_diagnostics_read(small, sizeof(small), &count) == CP_TOO_LARGE && !count && !small[0]);
    assert(owner_reads > reads_before_small); /* The encoder, not identity input, overflowed. */
    clock_us = -1; read_ok(); assert(document.nodes[child(0, "sampled_us")].type == CJ_NULL); clock_us = 1234567;
    catalog_error = CP_UNAVAILABLE;
    assert(app_runtime_diagnostics_read(output, sizeof(output), &count) == CP_UNAVAILABLE && !count && !output[0]); catalog_error = CP_OK;
    const char *saved = catalog; catalog = "{\"boot\":\"malformed\"}";
    assert(app_runtime_diagnostics_read(output, sizeof(output), &count) == CP_INTERNAL && !count && !output[0]); catalog = saved;
    cp_identity_t who = {.scopes = CP_SCOPE_VIEW}; unsigned before = catalog_reads;
    assert(read_resource(NULL, &who, APP_RUNTIME_DIAGNOSTICS_RESOURCE, output, sizeof(output), &count) == CP_FORBIDDEN && catalog_reads == before);
    who.scopes = CP_SCOPE_DIAGNOSTICS_READ;
    assert(read_resource(NULL, &who, APP_RUNTIME_DIAGNOSTICS_RESOURCE, output, sizeof(output), &count) == CP_OK);
    assert(catalog_reads == before + 1U && owner_reads > 0);
    puts("PASS: actual runtime diagnostic dispatch/encoder, scope, unavailable versus zero, retained failure, privacy, read neutrality, contention and bounds");
}
