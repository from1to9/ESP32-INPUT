// SPDX-License-Identifier: Apache-2.0
#include "app_control_runtime.h"
#include "control_json.h"
#include "usb_hid.h"
#include "app_sensor_api.h"
#include "app_runtime_diagnostics.h"
#include "control_sensor_usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INBOX_CAPACITY 4U
typedef struct {
    cp_command_t command;
    cp_result_t result;
    bool used, started, completion_pending;
    cp_error_t invalidated;
    uint64_t started_ms;
    uint32_t initial_revision;
    uint64_t initial_resource_revision;
    uint64_t expected_management_generation;
    uint64_t update_policy_revision;
    uint64_t profile_request_id;
    char expected_ssid[APP_WIFI_SSID_CAPACITY];
} work_t;
typedef struct {
    app_control_host_t host;
    app_control_runtime_snapshot_t view;
    cp_device_state_t state;
    bool state_known, approve_pending, usb_attached;
    work_t work[INBOX_CAPACITY];
    uint8_t token[32];
    char package[APP_CONTROL_TRUST_PACKAGE_BYTES + 1];
    size_t package_length;
    char line[384];
    size_t line_length;
    char incoming[APP_CONTROL_TRUST_PACKAGE_BYTES + 1];
    uint8_t expected_digest[32];
    size_t incoming_length, incoming_expected;
    bool importing;
    char clock_names[2][129];
    uint64_t clock_trust_revision;
    uint64_t update_trust_revision;
    cp_sensor_usb_t sensor_usb;
    unsigned usb_input_mode;
    bool usb_discard_line;
} runtime_t;
static runtime_t *runtime;
static portMUX_TYPE runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}
static cp_error_t error_code(esp_err_t e)
{
    if (e == ESP_OK) {
        return CP_OK;
    }
    if (e == ESP_ERR_NO_MEM) {
        return CP_BUSY;
    }
    if (e == ESP_ERR_NOT_SUPPORTED) {
        return CP_UNAVAILABLE;
    }
    if (e == ESP_ERR_INVALID_ARG) {
        return CP_INVALID;
    }
    if (e == ESP_ERR_INVALID_STATE) {
        return CP_BUSY;
    }
    if (e == ESP_ERR_TIMEOUT) {
        return CP_EXPIRED;
    }
    if (e == ESP_ERR_NOT_FOUND) {
        return CP_STALE;
    }
    return CP_INTERNAL;
}
static bool profile_operation(cp_operation_id_t op)
{
    return op == CP_OP_WIFI_PROFILE_FORGET || op == CP_OP_WIFI_PROFILE_SAVE ||
           op == CP_OP_WIFI_PROFILE_JOIN || op == CP_OP_WIFI_PROFILES_REORDER ||
           op == CP_OP_WIFI_PROFILES_FORGET_ALL;
}
static bool update_operation(cp_operation_id_t op)
{
    return (op >= CP_OP_FIRMWARE_CHECK && op <= CP_OP_FIRMWARE_DISCARD) ||
           op == CP_OP_FIRMWARE_CANCEL;
}
static bool owned(cp_operation_id_t op)
{
    return (op >= CP_OP_WIFI_SCAN_START && op <= CP_OP_WIFI_JOIN) || profile_operation(op) ||
           update_operation(op);
}
static bool supported_native(cp_operation_id_t op)
{
    return owned(op) && op != CP_OP_WIFI_DISCONNECT;
}
static bool random_source(void *unused, uint8_t *out, size_t n)
{
    (void)unused;
    esp_fill_random(out, n);
    return true;
}
static bool available(void *unused, cp_operation_id_t op, char reason[CP_REASON_CAPACITY])
{
    (void)unused;
    if (update_operation(op)) {
        app_update_snapshot_t s;
        app_update_snapshot(&s);
        bool yes =
            s.available && ((op != CP_OP_FIRMWARE_DOWNLOAD && op != CP_OP_FIRMWARE_ACTIVATE) ||
                            runtime->host.update_authority);
        if (!yes) {
            strcpy(reason, "update_trust_layout_or_policy_unavailable");
        }
        return yes;
    }
    if (owned(op)) {
        app_wifi_snapshot_t s;
        app_wifi_get_snapshot(&s);
        bool yes = supported_native(op) && s.initialized && s.state != APP_WIFI_UNAVAILABLE;
        if (!yes) {
            strcpy(reason, "wifi_unavailable");
        }
        return yes;
    }
    if (op == CP_OP_CAPTURE_GRANTS_LIST || op == CP_OP_CLOCK_GET || op == CP_OP_SESSION_HEARTBEAT ||
        op == CP_OP_WIFI_PROFILES_LIST || op == CP_OP_WIFI_SCAN_RESULTS ||
        op == CP_OP_FIRMWARE_STATE || op == CP_OP_FIRMWARE_RELEASES ||
        op == CP_OP_SENSORS_CATALOG || op == CP_OP_SENSORS_LATEST || op == CP_OP_SENSORS_SAMPLES) {
        return true;
    }
    return runtime->host.available && runtime->host.available(runtime->host.user, op, reason);
}
static cp_error_t submit(void *unused, const cp_command_t *command, cp_result_t *result)
{
    (void)unused;
    if (!owned(command->operation)) {
        return runtime->host.submit ? runtime->host.submit(runtime->host.user, command, result)
                                    : CP_UNAVAILABLE;
    }
    if (!supported_native(command->operation)) {
        return CP_NOT_IMPLEMENTED;
    }
    cp_error_t e = CP_BUSY;
    portENTER_CRITICAL(&runtime_lock);
    for (unsigned i = 0; i < INBOX_CAPACITY; ++i) {
        if (!runtime->work[i].used) {
            runtime->work[i] =
                (work_t){.command = *command,
                         .used = true,
                         .expected_management_generation = command->management_generation};
            *result = (cp_result_t){.command_key = command->command_key,
                                    .state = CP_RESULT_ACCEPTED,
                                    .uncertain_index = -1};
            e = CP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&runtime_lock);
    return e;
}
static void safety(void *unused, cp_safety_reason_t reason, const cp_identity_t *identity)
{
    (void)unused;
    if (reason == CP_SAFETY_LOCKED || reason == CP_SAFETY_SESSION_LOST ||
        reason == CP_SAFETY_REVOKED) {
        portENTER_CRITICAL(&runtime_lock);
        for (unsigned i = 0; i < INBOX_CAPACITY; ++i) {
            work_t *w = &runtime->work[i];
            if (w->used && !w->started &&
                (!identity ||
                 (!memcmp(identity->session_id, w->command.identity.session_id, 16) &&
                  !memcmp(identity->principal_id, w->command.identity.principal_id, 16)))) {
                w->invalidated = reason == CP_SAFETY_LOCKED ? CP_LOCKED : CP_UNAUTHENTICATED;
            }
        }
        portEXIT_CRITICAL(&runtime_lock);
    }
    if (runtime->host.safety) {
        runtime->host.safety(runtime->host.user, reason, identity);
    }
}
static void hex(const uint8_t *p, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    out[2 * n] = 0;
}
static bool unhex(const char *s, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = s[2 * i + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
    return true;
}
static cp_error_t read_resource(void *unused, const cp_identity_t *who, const char *resource,
                                char *json, size_t capacity, size_t *length)
{
    (void)unused;
    int n = -1;
    if (!strcmp(resource, APP_RUNTIME_DIAGNOSTICS_RESOURCE)) {
        if (!(who->scopes & CP_SCOPE_DIAGNOSTICS_READ)) return CP_FORBIDDEN;
        return app_runtime_diagnostics_read(json, capacity, length);
    }
    cp_sensor_resource_t sensor = cp_sensor_resource(resource);
    if (sensor != CP_SENSOR_RESOURCE_NONE) {
        if (!(who->scopes & CP_SCOPE_DIAGNOSTICS_READ)) return CP_FORBIDDEN;
        if (sensor == CP_SENSOR_RESOURCE_INVALID) return CP_INVALID;
        return app_sensor_api_read(resource, json, capacity, length);
    }
    if (!strcmp(resource, "/api/v1/firmware/policy")) {
        app_update_policy_t p;
        if (!app_control_trust_update_policy(who->principal_id, &p)) {
            return CP_UNAVAILABLE;
        }
        char id[33];
        hex(p.id, 16, id);
        n = snprintf(json, capacity,
                     "{\"id\":\"%s\",\"revision\":\"%llu\",\"expires_unix_seconds\":\"%llu\","
                     "\"maximum_job_ms\":%u,\"maximum_bytes\":%u,\"allow_download\":%s,\"allow_"
                     "activate\":%s,\"allow_restart\":%s,\"continue_after_owner_loss\":%s,\"allow_"
                     "signed_releases\":%s,\"allowed_digest\":\"%s\"}",
                     id, (unsigned long long)p.revision, (unsigned long long)p.expires_unix_seconds,
                     (unsigned)p.maximum_job_ms, (unsigned)p.maximum_bytes,
                     p.allow_download ? "true" : "false", p.allow_activate ? "true" : "false",
                     p.allow_restart ? "true" : "false",
                     p.continue_after_owner_loss ? "true" : "false",
                     p.allow_signed_releases ? "true" : "false", p.allowed_digest);
    } else if (!strcmp(resource, "/api/v1/wifi/profiles")) {
        app_wifi_profiles_snapshot_t s;
        app_wifi_profiles_snapshot(&s);
        int k = snprintf(json, capacity,
                         "{\"known\":%s,\"uncertain\":%s,\"revision\":\"%llu\",\"capacity\":8,"
                         "\"operation_pending\":%s,\"profiles\":[",
                         s.known ? "true" : "false", s.uncertain ? "true" : "false",
                         (unsigned long long)s.revision, s.operation_pending ? "true" : "false");
        if (k < 0 || (size_t)k >= capacity) {
            return CP_TOO_LARGE;
        }
        size_t used = (size_t)k;
        for (unsigned i = 0; i < s.count; ++i) {
            char ssid[201];
            if (!cj_escape(s.profiles[i].ssid, ssid, sizeof(ssid))) {
                return CP_INTERNAL;
            }
            k = snprintf(
                json + used, capacity - used,
                "%s{\"id\":\"%s\",\"ssid\":%s,\"enabled\":%s,\"current\":%s,\"priority\":%u}",
                i ? "," : "", s.profiles[i].id, ssid, s.profiles[i].enabled ? "true" : "false",
                s.profiles[i].current ? "true" : "false", s.profiles[i].priority);
            if (k < 0 || (size_t)k >= capacity - used) {
                return CP_TOO_LARGE;
            }
            used += (size_t)k;
        }
        k = snprintf(json + used, capacity - used, "]}");
        if (k < 0 || (size_t)k >= capacity - used) {
            return CP_TOO_LARGE;
        }
        *length = used + (size_t)k;
        return CP_OK;
    } else if (!strcmp(resource, "/api/v1/firmware/state") ||
               !strcmp(resource, "/api/v1/firmware/releases")) {
        if (!(who->scopes &
              (CP_SCOPE_UPDATE_CHECK | CP_SCOPE_UPDATE_STAGE | CP_SCOPE_UPDATE_ACTIVATE))) {
            return CP_FORBIDDEN;
        }
        app_update_snapshot_t s;
        app_update_snapshot(&s);
        char reason[401];
        if (!cj_escape(s.reason, reason, sizeof(reason))) {
            return CP_INTERNAL;
        }
        char health_error[24] = "null";
        if (s.health_confirmation_attempted) {
            snprintf(health_error, sizeof(health_error), "%d", s.health_confirmation_error);
        }
        n = snprintf(
            json, capacity,
            "{\"available\":%s,\"state\":%u,\"job_id\":\"%llu\",\"revision\":\"%llu\",\"policy_"
            "revision\":\"%llu\",\"release_id\":\"%s\",\"digest\":\"%s\",\"received_bytes\":%u,"
            "\"written_bytes\":%u,\"total_bytes\":%u,\"cleanup_pending\":%s,\"boot_selected\":%s,"
            "\"running_expected\":%s,\"running_state_known\":%s,\"running_confirmed\":%s,"
            "\"probation_active\":%s,\"health_confirmation_attempted\":%s,"
            "\"running_state_error\":%d,\"health_confirmation_error\":%s,\"error\":%d,\"reason\":%"
            "s}",
            s.available ? "true" : "false", (unsigned)s.state, (unsigned long long)s.job_id,
            (unsigned long long)s.revision, (unsigned long long)s.policy_revision, s.release_id,
            s.digest, (unsigned)s.received_bytes, (unsigned)s.written_bytes,
            (unsigned)s.total_bytes, s.cleanup_pending ? "true" : "false",
            s.boot_selected ? "true" : "false", s.running_expected ? "true" : "false",
            s.running_state_known ? "true" : "false", s.running_confirmed ? "true" : "false",
            s.probation_active ? "true" : "false",
            s.health_confirmation_attempted ? "true" : "false", s.running_state_error, health_error,
            s.error, reason);
    } else if (!strcmp(resource, "/api/v1/clock")) {
        app_clock_snapshot_t s;
        app_clock_snapshot(&s);
        char seconds[32];
        snprintf(seconds, sizeof(seconds), "\"%llu\"", (unsigned long long)s.unix_seconds);
        n = snprintf(json, capacity,
                     "{\"known\":%s,\"unix_seconds\":%s,\"synchronizing\":%s,\"cryptographically_"
                     "authenticated\":false,\"revision\":%u,\"error\":%d}",
                     s.known ? "true" : "false", s.known ? seconds : "null",
                     s.synchronizing ? "true" : "false", (unsigned)s.revision, s.error);
    } else if (!strcmp(resource, "/api/v1/trust")) {
        if (!(who->scopes & CP_SCOPE_ADMIN_CLIENTS)) {
            return CP_FORBIDDEN;
        }
        app_control_trust_snapshot_t s;
        app_control_trust_snapshot(&s);
        n = snprintf(json, capacity,
                     "{\"known\":%s,\"present\":%s,\"uncertain\":%s,\"at_rest_protected\":%s,"
                     "\"revision\":\"%llu\",\"error\":%d}",
                     s.known ? "true" : "false", s.present ? "true" : "false",
                     s.uncertain ? "true" : "false", s.at_rest_protected ? "true" : "false",
                     (unsigned long long)s.revision, s.error);
    } else if (!strcmp(resource, "/api/v1/capture/grants")) {
        cp_capture_grant_t grants[CP_MAX_CAPTURE_GRANTS];
        size_t count = app_control_trust_grants(who->principal_id, grants);
        size_t used = 0;
        int k = snprintf(json, capacity, "{\"source\":\"local_microphone\",\"grants\":[");
        if (k < 0 || (size_t)k >= capacity) {
            return CP_TOO_LARGE;
        }
        used = (size_t)k;
        static const char *const purposes[] = {"speech", "live_capture", "recording",
                                               "background_recording"};
        for (size_t i = 0; i < count; ++i) {
            char id[33];
            hex(grants[i].id, 16, id);
            cp_capture_grant_t *g = &grants[i];
            k = snprintf(
                json + used, capacity - used,
                "%s{\"id\":\"%s\",\"revision\":\"%llu\",\"purpose\":\"%s\",\"expires_unix_"
                "seconds\":\"%llu\",\"maximum_duration_seconds\":%u,\"maximum_bytes\":\"%"
                "llu\",\"maximum_retention_seconds\":%u,\"live_destination\":%s,\"file_"
                "destination\":%s,\"owner_download\":%s}",
                i ? "," : "", id, (unsigned long long)g->revision, purposes[g->purpose],
                (unsigned long long)g->expires_unix_seconds, (unsigned)g->maximum_duration_seconds,
                (unsigned long long)g->maximum_bytes, (unsigned)g->maximum_retention_seconds,
                g->live_destination ? "true" : "false", g->file_destination ? "true" : "false",
                g->owner_download ? "true" : "false");
            if (k < 0 || (size_t)k >= capacity - used) {
                return CP_TOO_LARGE;
            }
            used += (size_t)k;
        }
        k = snprintf(json + used, capacity - used, "]}");
        if (k < 0 || (size_t)k >= capacity - used) {
            return CP_TOO_LARGE;
        }
        *length = used + (size_t)k;
        return CP_OK;
    } else if (!strncmp(resource, "/api/v1/wifi/scans/", 19)) {
        app_wifi_scan_snapshot_t s;
        app_wifi_get_scan_snapshot(&s);
        n = snprintf(json, capacity,
                     "{\"state\":%u,\"generation\":%u,\"count\":%u,\"capacity_reached\":%s,"
                     "\"cleanup_pending\":%s,\"remaining_ms\":%u,\"error\":%d,\"records\":[",
                     s.state, (unsigned)s.generation, s.count,
                     s.capacity_reached ? "true" : "false", s.cleanup_pending ? "true" : "false",
                     (unsigned)s.remaining_ms, s.error);
        if (n < 0 || (size_t)n >= capacity) {
            return CP_TOO_LARGE;
        }
        size_t used = (size_t)n;
        unsigned start = 0;
        const char *page = strrchr(resource, '/');
        if (page && page[1] >= '0' && page[1] <= '9') {
            char *end = NULL;
            unsigned long v = strtoul(page + 1, &end, 10);
            if (!end || *end || v > 31) {
                return CP_INVALID;
            }
            start = (unsigned)v;
        }
        for (unsigned i = start; i < s.count && i < start + 8; ++i) {
            app_wifi_scan_record_t r;
            if (app_wifi_get_scan_record(s.generation, (uint16_t)i, &r) != ESP_OK) {
                return CP_STALE;
            }
            char label[APP_WIFI_SCAN_LABEL_CAPACITY * 6 + 3];
            if (!cj_escape(r.label, label, sizeof(label))) {
                return CP_INTERNAL;
            }
            int k = snprintf(
                json + used, capacity - used,
                "%s{\"index\":%u,\"ssid\":%s,\"rssi_dbm\":%d,\"channel\":%u,\"security\":%u,\"join_"
                "availability\":%u,\"current\":%s,\"remembered\":%s}",
                i > start ? "," : "", i, label, r.rssi_dbm, r.channel, r.security, r.availability,
                r.current ? "true" : "false", r.remembered ? "true" : "false");
            if (k < 0 || (size_t)k >= capacity - used) {
                return CP_TOO_LARGE;
            }
            used += (size_t)k;
        }
        int k = snprintf(json + used, capacity - used, "],\"next_index\":%u}",
                         start + 8 < s.count ? start + 8 : 0);
        if (k < 0 || (size_t)k >= capacity - used) {
            return CP_TOO_LARGE;
        }
        *length = used + (size_t)k;
        return CP_OK;
    } else if (!strcmp(resource, "/api/v1/state")) {
        /* Host state remains available through the parent port. Network status
         * is read from its own actual manager rather than inferred from HID. */
        return runtime->host.read
                   ? runtime->host.read(runtime->host.user, who, resource, json, capacity, length)
                   : CP_UNAVAILABLE;
    } else {
        return runtime->host.read
                   ? runtime->host.read(runtime->host.user, who, resource, json, capacity, length)
                   : CP_UNAVAILABLE;
    }
    if (n < 0 || (size_t)n >= capacity) {
        return CP_TOO_LARGE;
    }
    *length = (size_t)n;
    return CP_OK;
}
static bool u32(const cp_command_t *c, const char *key, uint32_t *out)
{
    char value[21];
    if (!cp_command_string(c, key, value, sizeof(value))) {
        return false;
    }
    char *end = NULL;
    unsigned long long n = strtoull(value, &end, 10);
    if (!end || *end || n > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)n;
    return true;
}
static bool argument_u64(const cp_command_t *c, const char *name, uint64_t *out)
{
    char value[21];
    if (!cp_command_string(c, name, value, sizeof(value))) {
        return false;
    }
    char *end = NULL;
    unsigned long long v = strtoull(value, &end, 10);
    if (!end || *end) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}
static esp_err_t start_native(work_t *w)
{
    cp_command_t *c = &w->command;
    app_wifi_selection_owner_t owner;
    memcpy(owner.principal_id, c->identity.principal_id, 16);
    memcpy(owner.session_id, c->identity.session_id, 16);
    if (profile_operation(c->operation)) {
        app_wifi_profiles_snapshot_t snapshot;
        app_wifi_profiles_snapshot(&snapshot);
        if (c->resource_revision != snapshot.revision) {
            return ESP_ERR_INVALID_STATE;
        }
        w->initial_resource_revision = snapshot.revision;
        char id[33] = {0};
        (void)cp_command_string(c, "profile_id", id, sizeof(id));
        if (c->operation == CP_OP_WIFI_PROFILE_FORGET) {
            return app_wifi_profile_forget(id, c->resource_revision);
        }
        if (c->operation == CP_OP_WIFI_PROFILE_JOIN) {
            for (unsigned i = 0; i < snapshot.count; ++i) {
                if (!strcmp(id, snapshot.profiles[i].id)) {
                    strcpy(w->expected_ssid, snapshot.profiles[i].ssid);
                }
            }
            return app_wifi_profile_join(id, c->resource_revision);
        }
        if (c->operation == CP_OP_WIFI_PROFILES_FORGET_ALL) {
            bool acknowledge = false;
            if (!cp_command_boolean(c, "acknowledge_unknown_store", &acknowledge)) {
                return ESP_ERR_INVALID_ARG;
            }
            return app_wifi_profiles_forget_all(c->resource_revision, acknowledge);
        }
        if (c->operation == CP_OP_WIFI_PROFILE_SAVE) {
            app_wifi_profile_save_t save = {.expected_revision = c->resource_revision};
            strcpy(save.id, id);
            bool valid = cp_command_string(c, "ssid", save.ssid, sizeof(save.ssid)) &&
                         cp_command_boolean(c, "keep_password", &save.keep_password) &&
                         cp_command_boolean(c, "enabled", &save.enabled) &&
                         cp_command_boolean(c, "join", &save.join);
            if (!save.keep_password) {
                valid =
                    valid && cp_command_string(c, "password", save.password, sizeof(save.password));
            }
            esp_err_t e = valid ? app_wifi_profile_save(&save) : ESP_ERR_INVALID_ARG;
            wipe(&save, sizeof(save));
            return e;
        }
        if (c->operation == CP_OP_WIFI_PROFILES_REORDER) {
            char encoded[300];
            if (!cp_command_json(c, "profile_ids", encoded, sizeof(encoded))) {
                return ESP_ERR_INVALID_ARG;
            }
            cj_document_t *d =
                heap_caps_aligned_calloc(_Alignof(cj_document_t), 1, sizeof(*d), MALLOC_CAP_8BIT);
            if (!d) {
                return ESP_ERR_NO_MEM;
            }
            char ids[8][33] = {0};
            uint8_t count = 0;
            bool valid = cj_parse(d, encoded, strlen(encoded));
            for (uint16_t t = valid ? d->nodes[0].child : CJ_NONE; t != CJ_NONE && valid;
                 t = d->nodes[t].next) {
                const char *id_value = cj_string(d, t);
                if (count == 8 || !id_value || strlen(id_value) != 32) {
                    valid = false;
                    break;
                }
                memcpy(ids[count++], id_value, 33);
            }
            esp_err_t e = valid ? app_wifi_profiles_reorder((const char(*)[33])ids, count,
                                                            c->resource_revision)
                                : ESP_ERR_INVALID_ARG;
            wipe(d, sizeof(*d));
            heap_caps_free(d);
            return e;
        }
    }
    if (update_operation(c->operation)) {
        app_update_snapshot_t snapshot;
        app_update_snapshot(&snapshot);
        w->initial_resource_revision = snapshot.revision;
        uint64_t job = 0;
        char release[65];
        app_update_authority_t authority = {0};
        if (c->operation == CP_OP_FIRMWARE_CHECK) {
            return app_update_check();
        }
        if (c->operation == CP_OP_FIRMWARE_DISCARD || c->operation == CP_OP_FIRMWARE_CANCEL) {
            if (!argument_u64(c, "job_id", &job)) {
                return ESP_ERR_INVALID_ARG;
            }
            return c->operation == CP_OP_FIRMWARE_DISCARD ? app_update_discard(job)
                                                          : app_update_cancel(job);
        }
        if (app_control_runtime_update_authority(c, &authority) != CP_OK ||
            !runtime->host.update_authority ||
            runtime->host.update_authority(runtime->host.user, c, &authority) != CP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        w->update_policy_revision = authority.policy_revision;
        if (c->operation == CP_OP_FIRMWARE_DOWNLOAD) {
            if (!cp_command_string(c, "release_id", release, sizeof(release))) {
                return ESP_ERR_INVALID_ARG;
            }
            return app_update_download(release, &authority);
        }
        if (!argument_u64(c, "job_id", &job)) {
            return ESP_ERR_INVALID_ARG;
        }
        return app_update_activate(job, &authority);
    }
    switch (c->operation) {
    case CP_OP_WIFI_SCAN_START:
        return app_wifi_scan_start();
    case CP_OP_WIFI_SCAN_CANCEL:
        return app_wifi_scan_cancel();
    case CP_OP_WIFI_SETUP_START:
        return app_wifi_start_setup();
    case CP_OP_WIFI_SETUP_STOP:
        return app_wifi_stop_setup();
    case CP_OP_WIFI_STORAGE_VERIFY:
        return app_wifi_verify_storage();
    case CP_OP_WIFI_SELECTION_RESERVE: {
        uint32_t generation;
        int64_t index;
        app_wifi_selection_t selection;
        if (!u32(c, "scan_generation", &generation) ||
            !cp_command_integer(c, "record_index", &index)) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t e =
            app_wifi_select_scan_record_owned(generation, (uint16_t)index, &owner, &selection);
        if (e == ESP_OK) {
            w->result.job_id = selection.id;
        }
        return e;
    }
    case CP_OP_WIFI_SELECTION_RELEASE: {
        uint32_t id;
        if (!u32(c, "selection_id", &id)) {
            return ESP_ERR_INVALID_ARG;
        }
        return app_wifi_release_selection_owned(id, &owner);
    }
    case CP_OP_WIFI_JOIN: {
        app_wifi_join_request_t request = {0};
        if (!cp_command_string(c, "ssid", request.ssid, sizeof(request.ssid)) ||
            !cp_command_string(c, "password", request.password, sizeof(request.password)) ||
            !cp_command_boolean(c, "remember", &request.remember)) {
            return ESP_ERR_INVALID_ARG;
        }
        (void)u32(c, "scan_generation", &request.scan_generation);
        (void)u32(c, "selection_id", &request.selection_id);
        int64_t index = 0;
        (void)cp_command_integer(c, "record_index", &index);
        request.record_index = (uint16_t)index;
        esp_err_t e = app_wifi_join_owned(&request, &owner);
        wipe(&request, sizeof(request));
        return e;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
static cp_error_t queued_authority(work_t *w, uint64_t now)
{
    portENTER_CRITICAL(&runtime_lock);
    cp_device_state_t state = runtime->state;
    bool known = runtime->state_known;
    cp_error_t invalidated = w->invalidated;
    portEXIT_CRITICAL(&runtime_lock);
    if (invalidated != CP_OK) {
        return invalidated;
    }
    if (now >= w->command.deadline_ms) {
        return CP_EXPIRED;
    }
    if (!known) {
        return CP_UNAVAILABLE;
    }
    if (state.locked) {
        return CP_LOCKED;
    }
    if (state.management_generation != w->expected_management_generation) {
        return CP_STALE;
    }
    const cp_operation_info_t *op = cp_operation_at(w->command.operation - 1U);
    if (!op || (w->command.identity.scopes & op->scopes) != op->scopes) {
        return CP_FORBIDDEN;
    }
    if (!app_control_server_identity_alive(&w->command.identity)) {
        return CP_UNAUTHENTICATED;
    }
    /* Claim native admission atomically after the external identity lookup.
     * A queued cancellation/state replacement wins until this linearization;
     * after it, report actual manager effects rather than fabricated rollback. */
    portENTER_CRITICAL(&runtime_lock);
    invalidated = w->invalidated;
    if (invalidated == CP_OK && !runtime->state_known) {
        invalidated = CP_UNAVAILABLE;
    }
    if (invalidated == CP_OK && runtime->state.locked) {
        invalidated = CP_LOCKED;
    }
    if (invalidated == CP_OK &&
        runtime->state.management_generation != w->expected_management_generation) {
        invalidated = CP_STALE;
    }
    if (invalidated == CP_OK) {
        w->started = true;
        w->started_ms = now;
    }
    portEXIT_CRITICAL(&runtime_lock);
    return invalidated;
}
static void process_work(work_t *w)
{
    portENTER_CRITICAL(&runtime_lock);
    bool used = w->used;
    portEXIT_CRITICAL(&runtime_lock);
    if (!used) {
        return;
    }
    if (w->completion_pending) {
        if (app_control_server_complete(w->command.command_key, &w->result) == ESP_OK) {
            portENTER_CRITICAL(&runtime_lock);
            wipe(w, sizeof(*w));
            portEXIT_CRITICAL(&runtime_lock);
        }
        return;
    }
    app_wifi_snapshot_t s;
    app_wifi_get_snapshot(&s);
    uint64_t now = now_ms();
    if (!w->started) {
        cp_error_t authority = queued_authority(w, now);
        if (authority != CP_OK) {
            w->result = (cp_result_t){.command_key = w->command.command_key,
                                      .state = CP_RESULT_CANCELLED,
                                      .error = authority,
                                      .uncertain_index = -1};
            strcpy(w->result.reason, "authority_changed_before_start");
            wipe(w->command.args_json, sizeof(w->command.args_json));
            w->completion_pending = true;
            return;
        }
        w->result = (cp_result_t){.command_key = w->command.command_key,
                                  .state = CP_RESULT_RUNNING,
                                  .uncertain_index = -1};
        w->initial_revision = s.revision;
        (void)cp_command_string(&w->command, "ssid", w->expected_ssid, sizeof(w->expected_ssid));
        esp_err_t e = start_native(w);
        if (e == ESP_OK && profile_operation(w->command.operation) &&
            w->command.operation != CP_OP_WIFI_PROFILE_JOIN) {
            app_wifi_profiles_snapshot_t profiles;
            app_wifi_profiles_snapshot(&profiles);
            w->profile_request_id = profiles.request_id;
        }
        /* Secret only belongs to native worker after admission; never retain
         * it in a job/status record while waiting for completion. */
        if (!update_operation(w->command.operation)) {
            wipe(w->command.args_json, sizeof(w->command.args_json));
        }
        if (e != ESP_OK) {
            w->result.error = error_code(e);
            w->result.state = CP_RESULT_FAILED;
            w->completion_pending = true;
            return;
        }
        if (w->command.operation == CP_OP_WIFI_SELECTION_RESERVE ||
            w->command.operation == CP_OP_WIFI_SELECTION_RELEASE) {
            w->result.state = CP_RESULT_APPLIED;
            w->completion_pending = true;
            return;
        }
    }
    if (update_operation(w->command.operation)) {
        if ((w->command.operation == CP_OP_FIRMWARE_DOWNLOAD ||
             w->command.operation == CP_OP_FIRMWARE_ACTIVATE) &&
            runtime->host.update_authority) {
            app_update_authority_t a = {0};
            if (app_control_runtime_update_authority(&w->command, &a) != CP_OK ||
                runtime->host.update_authority(runtime->host.user, &w->command, &a) != CP_OK) {
                a.authorized = false;
                a.policy_revision = w->update_policy_revision;
            }
            if (a.policy_revision != w->update_policy_revision) {
                a.authorized = false;
                a.policy_revision = w->update_policy_revision;
            }
            app_update_publish_authority(&a);
        }
        app_update_snapshot_t update;
        app_update_snapshot(&update);
        w->result.job_id = update.job_id;
        w->result.resource_revision = update.revision;
        bool failed = update.state == APP_UPDATE_ERROR || update.state == APP_UPDATE_CANCELLED ||
                      update.state == APP_UPDATE_BOOT_SELECTION_UNCERTAIN;
        bool done =
            failed ||
            ((w->command.operation == CP_OP_FIRMWARE_CHECK &&
              update.state == APP_UPDATE_AVAILABLE) ||
             (w->command.operation == CP_OP_FIRMWARE_DOWNLOAD &&
              update.state == APP_UPDATE_STAGED) ||
             (w->command.operation == CP_OP_FIRMWARE_ACTIVATE &&
              update.state == APP_UPDATE_RESTART_PENDING) ||
             (w->command.operation == CP_OP_FIRMWARE_DISCARD && update.state == APP_UPDATE_IDLE) ||
             (w->command.operation == CP_OP_FIRMWARE_CANCEL &&
              update.state == APP_UPDATE_CANCELLED));
        if (update.revision != w->initial_resource_revision && done) {
            w->result.state = failed ? CP_RESULT_FAILED : CP_RESULT_APPLIED;
            w->result.error = failed ? error_code(update.error) : CP_OK;
            w->completion_pending = true;
        }
        return;
    }
    if (profile_operation(w->command.operation) &&
        w->command.operation != CP_OP_WIFI_PROFILE_JOIN) {
        app_wifi_profile_result_t result;
        esp_err_t queried = app_wifi_profile_result(w->profile_request_id, &result);
        if (queried == ESP_ERR_INVALID_STATE) {
            return;
        }
        w->result.resource_revision =
            queried == ESP_OK && result.storage_known ? result.revision : 0;
        w->result.error = queried == ESP_OK ? error_code(result.error) : CP_RESULT_UNKNOWN;
        w->result.state = w->result.error == CP_OK ? CP_RESULT_APPLIED : CP_RESULT_FAILED;
        if (queried == ESP_OK && !result.storage_known && result.storage_uncertain) {
            w->result.error = CP_STORAGE_UNCERTAIN;
        }
        app_wifi_profiles_snapshot_t profiles;
        app_wifi_profiles_snapshot(&profiles);
        for (unsigned i = 0; i < profiles.count; ++i) {
            if (!strcmp(w->expected_ssid, profiles.profiles[i].ssid)) {
                strcpy(w->result.object_id, profiles.profiles[i].id);
            }
        }
        w->completion_pending = true;
        return;
    }
    bool done = false;
    esp_err_t error = ESP_OK;
    switch (w->command.operation) {
    case CP_OP_WIFI_SCAN_START:
    case CP_OP_WIFI_SCAN_CANCEL: {
        app_wifi_scan_snapshot_t scan;
        app_wifi_get_scan_snapshot(&scan);
        done = (scan.state == APP_WIFI_SCAN_READY || scan.state == APP_WIFI_SCAN_CANCELLED ||
                scan.state == APP_WIFI_SCAN_ERROR) &&
               !scan.cleanup_pending;
        error = scan.error;
        w->result.resource_revision = scan.generation;
        break;
    }
    case CP_OP_WIFI_SETUP_START:
        done = s.setup_active || (!s.operation_pending && s.revision != w->initial_revision &&
                                  s.last_error != ESP_OK);
        error = s.last_error;
        break;
    case CP_OP_WIFI_SETUP_STOP:
        done = !s.setup_active && !s.operation_pending && !s.cleanup_pending;
        error = s.cleanup_error;
        break;
    case CP_OP_WIFI_STORAGE_VERIFY:
        done = !s.operation_pending && s.revision != w->initial_revision;
        error = s.storage_error;
        break;
    case CP_OP_WIFI_PROFILE_JOIN:
    case CP_OP_WIFI_JOIN:
        done = (s.lan_ready && !s.operation_pending && s.revision != w->initial_revision &&
                strcmp(s.ssid, w->expected_ssid) == 0) ||
               (!s.operation_pending && s.revision != w->initial_revision &&
                (s.connection_error != ESP_OK || s.storage_error != ESP_OK));
        error = s.storage_error != ESP_OK ? s.storage_error : s.connection_error;
        break;
    default:
        break;
    }
    if (now - w->started_ms > 60000) {
        done = true;
        error = ESP_ERR_TIMEOUT;
    }
    if (done) {
        w->result.state = error == ESP_OK ? CP_RESULT_APPLIED : CP_RESULT_FAILED;
        w->result.error = error_code(error);
        if (!w->result.resource_revision) {
            w->result.resource_revision = s.revision;
        }
        w->completion_pending = true;
    }
}
static void usb_reply(const char *text) { (void)write(STDOUT_FILENO, text, strlen(text)); }
static void abort_import(void)
{
    runtime->importing = false;
    runtime->incoming_length = runtime->incoming_expected = 0;
    wipe(runtime->incoming, sizeof(runtime->incoming));
    wipe(runtime->expected_digest, sizeof(runtime->expected_digest));
}
static void usb_line(void)
{
    char *line = runtime->line;
    if (!strcmp(line, "CP1 CHALLENGE")) {
        char token[65], reply[96];
        hex(runtime->token, 32, token);
        snprintf(reply, sizeof(reply), "CP1 TOKEN %s\n", token);
        usb_reply(reply);
        wipe(token, sizeof(token));
        wipe(reply, sizeof(reply));
        return;
    }
    if (!strncmp(line, "CP1 BEGIN ", 10)) {
        char token[65], digest[65], extra;
        unsigned length = 0;
        bool valid = sscanf(line + 10, "%64s %u %64s %c", token, &length, digest, &extra) == 3 &&
                     strlen(token) == 64 && strlen(digest) == 64 && length > 0 &&
                     length <= APP_CONTROL_TRUST_PACKAGE_BYTES;
        uint8_t decoded[32];
        valid = valid && unhex(token, 32, decoded) && memcmp(decoded, runtime->token, 32) == 0;
        abort_import();
        if (valid && unhex(digest, 32, runtime->expected_digest)) {
            runtime->importing = true;
            runtime->incoming_expected = length;
            usb_reply("CP1 READY\n");
        } else {
            usb_reply("CP1 ERROR invalid_begin\n");
        }
        wipe(token, sizeof(token));
        wipe(decoded, sizeof(decoded));
        return;
    }
    if (!strncmp(line, "CP1 DATA ", 9) && runtime->importing) {
        char *end = NULL;
        unsigned long offset = strtoul(line + 9, &end, 10);
        const char *bytes = end && *end == ' ' ? end + 1 : "";
        size_t length = strlen(bytes);
        uint8_t decoded[128];
        bool valid =
            length > 0 && length % 2 == 0 && length <= 256 && offset <= runtime->incoming_length &&
            offset + length / 2 <= runtime->incoming_expected && unhex(bytes, length / 2, decoded);
        if (valid && offset < runtime->incoming_length) {
            valid = offset + length / 2 <= runtime->incoming_length &&
                    memcmp(runtime->incoming + offset, decoded, length / 2) == 0;
        } else if (valid) {
            memcpy(runtime->incoming + offset, decoded, length / 2);
            runtime->incoming_length += length / 2;
        }
        if (valid) {
            usb_reply("CP1 ACK\n");
        } else {
            abort_import();
            usb_reply("CP1 ERROR invalid_chunk\n");
        }
        wipe(decoded, sizeof(decoded));
        return;
    }
    if (!strcmp(line, "CP1 END") && runtime->importing) {
        uint8_t digest[32];
        cp_sha256(runtime->incoming, runtime->incoming_length, digest);
        bool valid = runtime->incoming_length == runtime->incoming_expected &&
                     memcmp(digest, runtime->expected_digest, 32) == 0;
        esp_err_t error = valid ? app_control_runtime_stage_commissioning(runtime->incoming,
                                                                          runtime->incoming_length)
                                : ESP_ERR_INVALID_ARG;
        abort_import();
        usb_reply(error == ESP_OK ? "CP1 PENDING_REVIEW\n" : "CP1 ERROR rejected_package\n");
        return;
    }
    if (!strcmp(line, "CP1 STATUS")) {
        app_control_trust_snapshot_t s;
        app_control_trust_snapshot(&s);
        usb_reply(s.review_pending            ? "CP1 PENDING_REVIEW\n"
                  : s.present && !s.uncertain ? "CP1 STORED\n"
                                              : "CP1 ERROR unavailable\n");
        return;
    }
    usb_reply("CP1 ERROR invalid_command\n");
}
static bool usb_local_unlocked(void)
{
    portENTER_CRITICAL(&runtime_lock);
    bool unlocked = runtime->state_known && !runtime->state.locked;
    portEXIT_CRITICAL(&runtime_lock);
    return unlocked;
}
static bool sensor_usb_allowed(void *unused)
{
    (void)unused;
    app_control_trust_snapshot_t trust_view;
    app_control_trust_snapshot(&trust_view);
    return runtime->usb_attached && usb_hid_snapshot().mounted && usb_local_unlocked() &&
           !trust_view.import_window && !runtime->importing;
}
static cp_error_t sensor_usb_read(void *unused, const char *resource, char *json, size_t capacity, size_t *length)
{
    (void)unused;
    return app_sensor_api_read(resource, json, capacity, length);
}
static cp_error_t sensor_usb_batch(void *unused, const char boot[33], uint64_t after, unsigned limit,
                                  uint64_t id, uint8_t *raw, size_t capacity, size_t *length)
{
    (void)unused;
    return app_sensor_api_read_batch(boot, after, limit, id, raw, capacity, length);
}
static size_t sensor_usb_write(void *unused, const char *bytes, size_t length)
{
    (void)unused;
    ssize_t written = write(STDOUT_FILENO, bytes, length);
    return written > 0 ? (size_t)written : 0;
}
static void poll_usb(bool open)
{
    usb_hid_snapshot_t bus = usb_hid_snapshot();
    app_control_trust_snapshot_t trust_view;
    app_control_trust_snapshot(&trust_view);
    bool connected = runtime->usb_attached && bus.mounted && usb_local_unlocked();
    unsigned mode = !connected ? 0U : trust_view.import_window ? (open ? 1U : 0U) : 2U;
    if (mode != runtime->usb_input_mode) {
        if (runtime->line_length) runtime->usb_discard_line = true;
        runtime->usb_input_mode = mode;
        runtime->line_length = 0;
        wipe(runtime->line, sizeof(runtime->line));
    }
    if (mode != 1U) abort_import();
    if (mode != 2U) cp_sensor_usb_cancel(&runtime->sensor_usb);
    if (!runtime->usb_attached || !bus.mounted) {
        abort_import();
        runtime->line_length = 0;
        runtime->usb_discard_line = false;
        wipe(runtime->line, sizeof(runtime->line));
        return;
    }
    cp_sensor_usb_port_t sensor = {.allowed = sensor_usb_allowed, .read = sensor_usb_read,
        .batch = sensor_usb_batch, .write = sensor_usb_write};
    char bytes[128];
    ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
    for (ssize_t i = 0; i < n; ++i) {
        char c = bytes[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            runtime->line[runtime->line_length] = 0;
            if (!runtime->usb_discard_line) {
                if (mode == 1U) {
                    app_control_trust_snapshot(&trust_view);
                    if (trust_view.import_window && usb_local_unlocked() && !strncmp(runtime->line, "CP1 ", 4)) usb_line();
                } else if (mode == 2U) {
                    (void)cp_sensor_usb_receive(&runtime->sensor_usb, runtime->line, now_ms(), &sensor);
                }
            }
            wipe(runtime->line, sizeof(runtime->line));
            runtime->line_length = 0;
            runtime->usb_discard_line = false;
        } else if (!runtime->usb_discard_line && runtime->line_length + 1 < sizeof(runtime->line) && c >= 32 && c <= 126) {
            runtime->line[runtime->line_length++] = c;
        } else {
            abort_import();
            runtime->line_length = 0;
            wipe(runtime->line, sizeof(runtime->line));
            runtime->usb_discard_line = true;
        }
    }
    wipe(bytes, sizeof(bytes));
    cp_sensor_usb_service(&runtime->sensor_usb, now_ms(), &sensor);
}
static void worker(void *unused)
{
    (void)unused;
    esp_err_t error = app_control_trust_init();
    cp_callbacks_t callbacks = {.random = random_source,
                                .available = available,
                                .dispatch = submit,
                                .safety = safety,
                                .read = read_resource};
    app_clock_config_t clock_config = {0};
    (void)app_clock_init(&clock_config);
    (void)app_update_init(NULL);
    uint64_t next_start = 0;
    for (;;) {
        app_wifi_snapshot_t wifi;
        app_wifi_get_snapshot(&wifi);
        app_clock_station_ready(wifi.lan_ready);
        app_control_trust_snapshot_t trust_view;
        app_control_trust_snapshot(&trust_view);
        if (trust_view.known && trust_view.present &&
            runtime->clock_trust_revision != trust_view.revision) {
            size_t count = app_control_trust_clock_servers(runtime->clock_names);
            clock_config = (app_clock_config_t){0};
            for (size_t i = 0; i < count; ++i) {
                clock_config.servers[i] = runtime->clock_names[i];
            }
            (void)app_clock_init(&clock_config);
            runtime->clock_trust_revision = trust_view.revision;
        }
        if (trust_view.known && trust_view.present &&
            runtime->update_trust_revision != trust_view.revision) {
            app_update_provisioned_config_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_8BIT);
            if (p) {
                app_update_config_t config = {0};
                bool configured = app_control_trust_update_config(p);
                if (configured) {
                    config =
                        (app_update_config_t){.abi_version = APP_UPDATE_ABI,
                                              .catalog_url = p->catalog_url,
                                              .tls_ca_pem = p->tls_ca_pem,
                                              .signing_key_id = p->signing_key_id,
                                              .signing_public_key = p->signing_public_key,
                                              .product = p->product,
                                              .board = p->board,
                                              .layout = p->layout,
                                              .allow_private_source = p->allow_private_source,
                                              .minimum_release_index = p->minimum_release_index,
                                              .trust_revision = trust_view.revision};
                }
                esp_err_t e = app_update_configure(configured ? &config : NULL);
                if (e == ESP_OK || e == ESP_ERR_NOT_SUPPORTED) {
                    runtime->update_trust_revision = trust_view.revision;
                }
                wipe(p, sizeof(*p));
                heap_caps_free(p);
            }
        }
        app_control_server_snapshot_t server;
        app_control_server_snapshot(&server);
        app_wifi_set_backend_state(server.state == APP_CONTROL_READY && server.tls,
                                   server.state == APP_CONTROL_READY ? ESP_OK
                                   : server.error != ESP_OK          ? server.error
                                   : trust_view.error != ESP_OK      ? trust_view.error
                                                                     : ESP_ERR_INVALID_STATE);
        portENTER_CRITICAL(&runtime_lock);
        bool approved = runtime->approve_pending;
        runtime->approve_pending = false;
        bool state_known = runtime->state_known;
        cp_device_state_t state = runtime->state;
        portEXIT_CRITICAL(&runtime_lock);
        if (trust_view.import_window || !wifi.lan_ready) {
            if (server.state == APP_CONTROL_READY || server.state == APP_CONTROL_STARTING) {
                (void)app_control_server_stop();
            }
        }
        if (approved) {
            if (server.state == APP_CONTROL_STOPPED || server.state == APP_CONTROL_ERROR) {
                error = app_control_trust_import(runtime->token, runtime->package,
                                                 runtime->package_length);
                wipe(runtime->package, sizeof(runtime->package));
                runtime->package_length = 0;
                wipe(runtime->token, sizeof(runtime->token));
            } else {
                (void)app_control_server_stop();
                portENTER_CRITICAL(&runtime_lock);
                runtime->approve_pending = true;
                portEXIT_CRITICAL(&runtime_lock);
            }
        } else if (wifi.lan_ready && trust_view.known && trust_view.present &&
                   !trust_view.import_window &&
                   (server.state == APP_CONTROL_STOPPED || server.state == APP_CONTROL_ERROR) &&
                   now_ms() >= next_start) {
            error = app_control_trust_start_server(&callbacks);
            next_start = now_ms() + 5000;
        }
        if (state_known && server.state == APP_CONTROL_READY) {
            (void)app_control_server_publish_state(&state);
        }
        for (unsigned i = 0; i < INBOX_CAPACITY; ++i) {
            process_work(&runtime->work[i]);
        }
        poll_usb(trust_view.import_window);
        if (!trust_view.import_window && !approved) {
            wipe(runtime->package, sizeof(runtime->package));
            runtime->package_length = 0;
            wipe(runtime->token, sizeof(runtime->token));
        }
        app_control_runtime_snapshot_t view = {.initialized = true,
                                               .commissioning_open = trust_view.import_window,
                                               .usb_import_active = runtime->importing,
                                               .trust = trust_view,
                                               .server = server,
                                               .error = error};
        app_clock_snapshot(&view.clock);
        portENTER_CRITICAL(&runtime_lock);
        view.revision = runtime->view.revision + 1;
        runtime->view = view;
        portEXIT_CRITICAL(&runtime_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t app_control_runtime_init(const app_control_host_t *host)
{
    if (!host || host->abi_version != APP_CONTROL_RUNTIME_ABI || runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime_t *created = heap_caps_aligned_calloc(_Alignof(runtime_t), 1, sizeof(*created),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!created) {
        created = heap_caps_aligned_calloc(_Alignof(runtime_t), 1, sizeof(*created), MALLOC_CAP_8BIT);
    }
    if (!created) {
        return ESP_ERR_NO_MEM;
    }
    created->host = *host;
    runtime = created;
    if (xTaskCreate(worker, "control_runtime", 12288, NULL, 1, NULL) != pdPASS) {
        runtime = NULL;
        heap_caps_free(created);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t app_control_runtime_publish_state(const cp_device_state_t *state)
{
    if (!runtime || !state) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime_lock);
    for (unsigned i = 0; i < INBOX_CAPACITY; ++i) {
        work_t *w = &runtime->work[i];
        if (w->used && !w->started &&
            (state->locked || state->management_generation != w->expected_management_generation)) {
            w->invalidated = state->locked ? CP_LOCKED : CP_STALE;
        }
    }
    runtime->state = *state;
    runtime->state_known = true;
    portEXIT_CRITICAL(&runtime_lock);
    return app_control_server_publish_state(state);
}
esp_err_t app_control_runtime_complete(uint64_t key, const cp_result_t *r)
{
    return app_control_server_complete(key, r);
}
void app_control_runtime_physical_override(bool locked)
{
    app_control_server_physical_override(locked);
}
void app_control_runtime_snapshot(app_control_runtime_snapshot_t *out)
{
    if (!out) {
        return;
    }
    if (!runtime) {
        *out = (app_control_runtime_snapshot_t){.error = ESP_ERR_INVALID_STATE};
        return;
    }
    portENTER_CRITICAL(&runtime_lock);
    *out = runtime->view;
    portEXIT_CRITICAL(&runtime_lock);
}
bool app_control_runtime_wall_time(uint64_t *s) { return app_clock_wall_time(s); }
bool app_control_runtime_session_alive(const uint8_t session[16], const uint8_t principal[16])
{
    return app_control_server_session_alive(session, principal);
}
static bool matches(const cp_capture_grant_t *g, const cp_capture_authority_request_t *r,
                    cp_capture_purpose_t purpose, uint64_t revision, uint64_t wall)
{
    return g->purpose == purpose && g->revision == revision && g->expires_unix_seconds > wall &&
           memcmp(g->principal_id, r->identity.principal_id, 16) == 0 && r->duration_seconds &&
           r->duration_seconds <= g->maximum_duration_seconds && r->bytes &&
           r->bytes <= g->maximum_bytes && r->retention_seconds <= g->maximum_retention_seconds &&
           (!r->live_destination || g->live_destination) &&
           (!r->file_destination || g->file_destination) &&
           (!r->owner_download || g->owner_download);
}
cp_error_t app_control_runtime_capture_authority(const cp_capture_authority_request_t *r,
                                                 cp_capture_authority_t *out)
{
    if (!r || !out) {
        return CP_INVALID;
    }
    *out = (cp_capture_authority_t){.error = CP_FORBIDDEN};
    if (r->purpose >= CP_CAPTURE_BACKGROUND || (!r->live_destination && !r->file_destination) ||
        (r->owner_download && !r->file_destination) ||
        (r->continue_local && (!r->file_destination || r->purpose != CP_CAPTURE_RECORDING))) {
        return CP_FORBIDDEN;
    }
    uint64_t wall;
    if (!app_clock_wall_time(&wall)) {
        out->error = CP_UNAVAILABLE;
        return out->error;
    }
    cp_capture_grant_t grant;
    if (!app_control_trust_grant(r->grant_id, &grant) ||
        !matches(&grant, r, r->purpose, r->grant_revision, wall)) {
        return CP_FORBIDDEN;
    }
    cp_scope_mask_t needed =
        r->purpose == CP_CAPTURE_RECORDING ? CP_SCOPE_RECORDING_CONTROL : CP_SCOPE_AUDIO_CAPTURE;
    if ((r->identity.scopes & needed) != needed) {
        return CP_FORBIDDEN;
    }
    uint64_t now = now_ms(), remaining = grant.expires_unix_seconds - wall;
    out->allowed = true;
    out->error = CP_OK;
    out->owner_session_alive =
        app_control_runtime_session_alive(r->identity.session_id, r->identity.principal_id);
    out->source_valid_until_ms =
        now + (remaining > UINT64_C(86400) ? UINT64_C(86400) : remaining) * 1000;
    if (r->continue_local) {
        cp_capture_grant_t background;
        cp_capture_authority_request_t file = *r;
        file.live_destination = false;
        file.file_destination = true;
        if (!app_control_trust_grant(r->background_grant_id, &background) ||
            !matches(&background, &file, CP_CAPTURE_BACKGROUND, r->background_grant_revision,
                     wall) ||
            memcmp(r->background_grant_id, r->grant_id, 16) == 0) {
            out->allowed = false;
            out->error = CP_FORBIDDEN;
            return out->error;
        }
        out->background_allowed = true;
        remaining = background.expires_unix_seconds - wall;
        out->background_valid_until_ms =
            now + (remaining > UINT64_C(86400) ? UINT64_C(86400) : remaining) * 1000;
    }
    return CP_OK;
}
esp_err_t app_control_runtime_open_commissioning(void)
{
    if (!runtime) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime_lock);
    bool unlocked = runtime->state_known && !runtime->state.locked;
    portEXIT_CRITICAL(&runtime_lock);
    if (!unlocked) {
        return ESP_ERR_INVALID_STATE;
    }
    if (app_update_flash_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    return app_control_trust_open_local_window(runtime->token);
}
void app_control_runtime_close_commissioning(void)
{
    if (!runtime) {
        return;
    }
    app_control_trust_close_local_window();
    wipe(runtime->token, sizeof(runtime->token));
}
esp_err_t app_control_runtime_stage_commissioning(const char *json, size_t length)
{
    if (!runtime || !json || !length || length > APP_CONTROL_TRUST_PACKAGE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = app_control_trust_stage(runtime->token, json, length);
    if (error == ESP_OK) {
        memcpy(runtime->package, json, length);
        runtime->package[length] = 0;
        runtime->package_length = length;
    }
    return error;
}
bool app_control_runtime_commissioning_review(app_control_trust_review_t *out)
{
    return app_control_trust_review(out);
}
esp_err_t app_control_runtime_approve_commissioning(const char digest[65])
{
    if (!runtime) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime_lock);
    bool unlocked = runtime->state_known && !runtime->state.locked;
    portEXIT_CRITICAL(&runtime_lock);
    if (!unlocked) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = app_control_trust_approve_local(digest);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&runtime_lock);
        runtime->approve_pending = true;
        portEXIT_CRITICAL(&runtime_lock);
    }
    return error;
}
esp_err_t app_control_runtime_attach_usb_stdio(void)
{
    if (!runtime) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime_lock);
    runtime->usb_attached = true;
    portEXIT_CRITICAL(&runtime_lock);
    return ESP_OK;
}

cp_error_t app_control_runtime_update_authority(const cp_command_t *command,
                                                app_update_authority_t *out)
{
    if (!command || !out) {
        return CP_INVALID;
    }
    *out = (app_update_authority_t){0};
    bool activation = command->operation == CP_OP_FIRMWARE_ACTIVATE;
    if (command->operation != CP_OP_FIRMWARE_DOWNLOAD && !activation) {
        return CP_INVALID;
    }
    cp_scope_mask_t scope = activation ? CP_SCOPE_UPDATE_ACTIVATE : CP_SCOPE_UPDATE_STAGE;
    if ((command->identity.scopes & scope) != scope) {
        return CP_FORBIDDEN;
    }
    app_update_policy_t policy;
    if (!app_control_trust_update_policy(command->identity.principal_id, &policy)) {
        return CP_FORBIDDEN;
    }
    char id[33], expected[65];
    uint64_t revision = 0;
    if (!cp_command_string(command, "policy_id", id, sizeof(id)) ||
        !argument_u64(command, "policy_revision", &revision) || revision != policy.revision ||
        !cp_command_string(command, "digest", expected, sizeof(expected))) {
        return CP_INVALID;
    }
    char enrolled[33];
    hex(policy.id, 16, enrolled);
    if (strcmp(id, enrolled)) {
        return CP_FORBIDDEN;
    }
    uint64_t wall, now = now_ms();
    if (!app_clock_wall_time(&wall) || wall >= policy.expires_unix_seconds) {
        return CP_UNAVAILABLE;
    }
    if (command->deadline_ms > UINT64_MAX - policy.maximum_job_ms ||
        now >= command->deadline_ms + policy.maximum_job_ms) {
        return CP_EXPIRED;
    }
    if ((activation && !policy.allow_activate) || (!activation && !policy.allow_download) ||
        (!policy.allow_signed_releases && strcmp(expected, policy.allowed_digest))) {
        return CP_FORBIDDEN;
    }
    if (!policy.continue_after_owner_loss &&
        !app_control_runtime_session_alive(command->identity.session_id,
                                           command->identity.principal_id)) {
        return CP_UNAUTHENTICATED;
    }
    app_update_snapshot_t update;
    app_update_snapshot(&update);
    app_control_trust_snapshot_t trusted;
    app_control_trust_snapshot(&trusted);
    if (!update.available || update.trust_revision != trusted.revision ||
        strcmp(update.digest, expected) || !update.total_bytes ||
        update.total_bytes > policy.maximum_bytes) {
        return CP_STALE;
    }
    if (activation) {
        uint64_t job;
        if (!argument_u64(command, "job_id", &job) || job != update.job_id) {
            return CP_STALE;
        }
    } else {
        char release[65];
        if (!cp_command_string(command, "release_id", release, sizeof(release)) ||
            strcmp(release, update.release_id)) {
            return CP_STALE;
        }
    }
    uint64_t remaining = policy.expires_unix_seconds - wall;
    if (remaining > 600) {
        remaining = 600;
    }
    uint64_t until = now + remaining * 1000,
             job_until = command->deadline_ms + policy.maximum_job_ms;
    if (until > job_until) {
        until = job_until;
    }
    *out = (app_update_authority_t){.authorized = true,
                                    .policy_revision = policy.revision,
                                    .authority_expires_ms = until,
                                    .restart_authorized = activation && policy.allow_restart};
    strcpy(out->expected_digest, expected);
    return CP_OK;
}
