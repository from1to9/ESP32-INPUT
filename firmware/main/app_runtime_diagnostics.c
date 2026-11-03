// SPDX-License-Identifier: Apache-2.0
#include "app_runtime_diagnostics.h"
#include "app_sensor_api.h"
#include "app_control_runtime.h"
#include "app_audio.h"
#include "board_audio_mosaico.h"
#include "usb_audio.h"
#include "usb_hid.h"
#include "ble_hid.h"
#include "control_json.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static bool diagnostics_busy;
/* Query-only, separate from the sensor encoder and every mutable owner. */
static EXT_RAM_BSS_ATTR struct {
    cj_document_t document;
    char identity[5][70], stage[257];
    app_wifi_snapshot_t wifi;
    app_wifi_scan_snapshot_t scan;
    app_wifi_scan_diagnostics_t scan_diagnostics;
    app_control_runtime_snapshot_t control;
    usb_hid_snapshot_t usb;
    usb_audio_snapshot_t uac;
    ble_hid_snapshot_t bluetooth;
    ble_hid_failure_t bluetooth_failure;
    ae_snapshot_t audio;
    board_audio_status_t codec;
    app_update_snapshot_t update;
} scratch;
typedef struct { char *json; size_t capacity, used; bool valid; } writer_t;
static const char *boolean(bool value) { return value ? "true" : "false"; }
static void append(writer_t *w, const char *format, ...)
{
    if (!w->valid) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(w->json + w->used, w->capacity - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->capacity - w->used) w->valid = false;
    else w->used += (size_t)n;
}
static void error_value(writer_t *w, int error, bool known)
{
    if (known) append(w, "%d", error); else append(w, "null");
}
static void scan_observation(writer_t *w, const app_wifi_scan_observation_t *o)
{
    if (!o->known) { append(w, "null"); return; }
    append(w, "{\"generation\":%u,\"phase\":%u,\"error\":%d,\"driver_admitted\":%s,\"event_status\":",
        o->generation, (unsigned)o->phase, o->error, boolean(o->driver_admitted));
    if (o->event_status_known) append(w, "%u", o->event_status); else append(w, "null");
    append(w, ",\"heap\":");
    if (o->heap_known) append(w, "{\"internal\":%u,\"largest\":%u,\"dma\":%u,\"largest_dma\":%u}",
        o->free_internal, o->largest_internal, o->free_dma, o->largest_dma);
    else append(w, "null");
    append(w, ",\"driver_count\":");
    if (o->count_known) append(w, "%u", (unsigned)o->driver_count); else append(w, "null");
    append(w, ",\"read_count\":%u}", (unsigned)o->read_count);
}
static cp_error_t identity(char *json, size_t capacity)
{
    size_t length = 0;
    cp_error_t result = app_sensor_api_read("/api/v1/sensors/catalog", json, capacity, &length);
    if (result != CP_OK) return result;
    if (!length || length >= capacity || !cj_parse(&scratch.document, json, length)) return CP_INTERNAL;
    static const char *const keys[] = {"boot", "git_commit", "git_state", "elf_sha256", "config_sha256"};
    for (unsigned i = 0; i < 5; ++i) {
        uint16_t node = cj_get(&scratch.document, 0, keys[i]); size_t used;
        if (node == CJ_NONE || (scratch.document.nodes[node].type != CJ_STRING && scratch.document.nodes[node].type != CJ_NULL) ||
            !cj_canonical(&scratch.document, node, scratch.identity[i], sizeof(scratch.identity[i]), &used)) return CP_INTERNAL;
    }
    const char *boot = cj_string(&scratch.document, cj_get(&scratch.document, 0, "boot"));
    if (!boot || strlen(boot) != 32 || strspn(boot, "0123456789abcdef") != 32) return CP_INTERNAL;
    return CP_OK;
}
cp_error_t app_runtime_diagnostics_read(char *json, size_t capacity, size_t *length)
{
    if (!length) return CP_INVALID;
    *length = 0;
    if (!json || !capacity) return CP_INVALID;
    json[0] = 0;
    portENTER_CRITICAL(&diagnostics_lock);
    bool busy = diagnostics_busy;
    if (!busy) diagnostics_busy = true;
    portEXIT_CRITICAL(&diagnostics_lock);
    if (busy) return CP_BUSY;
    cp_error_t result = identity(json, capacity);
    if (result != CP_OK) goto done;
    int64_t sampled = esp_timer_get_time();
    app_wifi_get_snapshot(&scratch.wifi);
    app_wifi_get_scan_snapshot(&scratch.scan);
    app_wifi_get_scan_diagnostics(&scratch.scan_diagnostics);
    app_control_runtime_snapshot(&scratch.control);
    scratch.usb = usb_hid_snapshot(); scratch.uac = usb_audio_snapshot();
    scratch.bluetooth = ble_hid_snapshot(); scratch.bluetooth_failure = ble_hid_failure();
    bool audio_known = app_audio_snapshot(&scratch.audio);
    scratch.codec = board_audio_mosaico_status();
    app_update_snapshot(&scratch.update);
    writer_t w = {.json = json, .capacity = capacity > CP_MAX_RESPONSE_BYTES + 1U ? CP_MAX_RESPONSE_BYTES + 1U : capacity, .valid = true};
    append(&w, "{\"schema\":1,\"identity\":{\"telemetry_boot\":%s,\"git_commit\":%s,\"git_state\":%s,\"elf_sha256\":%s,\"config_sha256\":%s},\"sampled_us\":",
        scratch.identity[0], scratch.identity[1], scratch.identity[2], scratch.identity[3], scratch.identity[4]);
    if (sampled >= 0) append(&w, "\"%" PRId64 "\"", sampled); else append(&w, "null");
    app_wifi_snapshot_t *wifi = &scratch.wifi;
    append(&w, ",\"wifi\":{\"initialized\":%s,\"error\":", boolean(wifi->initialized));
    error_value(&w, wifi->last_error, wifi->initialized || wifi->last_error != ESP_OK);
    append(&w, ",\"health\":");
    if (wifi->initialized) append(&w, "{\"state\":%u,\"lan_ready\":%s,\"backend_available\":%s,\"backend_error\":%d,\"operation_pending\":%s,\"setup_active\":%s,\"cleanup_pending\":%s,\"connection_error\":%d,\"storage_known\":%s,\"storage_uncertain\":%s,\"storage_error\":%d,\"revision\":%u}",
        (unsigned)wifi->state, boolean(wifi->lan_ready), boolean(wifi->backend_available), wifi->backend_error,
        boolean(wifi->operation_pending), boolean(wifi->setup_active), boolean(wifi->cleanup_pending), wifi->connection_error,
        boolean(wifi->stored_data_known), boolean(wifi->storage_uncertain), wifi->storage_error, wifi->revision);
    else append(&w, "null");
    append(&w, ",\"scan\":");
    if (wifi->initialized || scratch.scan_diagnostics.current.known || scratch.scan_diagnostics.last_failure.known) {
        append(&w, "{\"state\":%u,\"error\":%d,\"cleanup_pending\":%s,\"cleanup_error\":%d,\"current\":",
            (unsigned)scratch.scan.state, scratch.scan.error, boolean(scratch.scan.cleanup_pending), scratch.scan.cleanup_error);
        scan_observation(&w, &scratch.scan_diagnostics.current);
        append(&w, ",\"last_failure\":"); scan_observation(&w, &scratch.scan_diagnostics.last_failure); append(&w, "}");
    } else append(&w, "null");
    append(&w, "}");
    app_control_runtime_snapshot_t *control = &scratch.control;
    append(&w, ",\"control\":{\"initialized\":%s,\"health\":", boolean(control->initialized));
    if (control->initialized) append(&w, "{\"error\":%d,\"revision\":%u,\"commissioning_open\":%s,\"usb_import_active\":%s,\"server_state\":%u,\"tls\":%s,\"server_error\":%d,\"trust_initialized\":%s,\"trust_known\":%s,\"trust_present\":%s,\"trust_uncertain\":%s,\"trust_error\":%d,\"clock_initialized\":%s,\"clock_known\":%s,\"clock_error\":%d}",
        control->error, control->revision, boolean(control->commissioning_open), boolean(control->usb_import_active),
        (unsigned)control->server.state, boolean(control->server.tls), control->server.error,
        boolean(control->trust.initialized), boolean(control->trust.known), boolean(control->trust.present), boolean(control->trust.uncertain), control->trust.error,
        boolean(control->clock.initialized), boolean(control->clock.known), control->clock.error);
    else { append(&w, "null"); }
    append(&w, "}");
    usb_hid_snapshot_t *usb = &scratch.usb;
    append(&w, ",\"usb_hid\":{\"initialized\":%s,\"failed\":%s,\"error\":", boolean(usb->initialized), boolean(usb->failed));
    error_value(&w, usb->error, usb->initialized || usb->failed || usb->error != ESP_OK);
    append(&w, ",\"health\":");
    if (usb->initialized) append(&w, "{\"mounted\":%s,\"suspended\":%s,\"enabled\":%s,\"ready\":%s,\"congested\":%s,\"generation\":\"%" PRIu64 "\",\"accepted\":\"%" PRIu64 "\",\"submitted\":\"%" PRIu64 "\",\"completed\":\"%" PRIu64 "\"}",
        boolean(usb->mounted), boolean(usb->suspended), boolean(usb->enabled), boolean(usb->ready), boolean(usb->congested), usb->generation, usb->accepted, usb->submitted, usb->completed);
    else { append(&w, "null"); }
    append(&w, "}");
    usb_audio_snapshot_t *uac = &scratch.uac;
    append(&w, ",\"usb_audio\":{\"initialized\":%s,\"error\":", boolean(uac->initialized));
    error_value(&w, uac->error, uac->initialized || uac->error != ESP_OK); append(&w, ",\"health\":");
    if (uac->initialized) append(&w, "{\"stack_ready\":%s,\"mounted\":%s,\"suspended\":%s,\"prepared\":%s,\"playback_selected\":%s,\"capture_selected\":%s,\"cleanup_pending\":%s,\"link_generation\":\"%" PRIu64 "\",\"audio_generation\":\"%" PRIu64 "\"}",
        boolean(uac->stack_ready), boolean(uac->mounted), boolean(uac->suspended), boolean(uac->prepared), boolean(uac->playback_selected), boolean(uac->capture_selected), boolean(uac->cleanup_pending), uac->link_generation, uac->audio_generation);
    else { append(&w, "null"); }
    append(&w, "}");
    ble_hid_snapshot_t *ble = &scratch.bluetooth; ble_hid_failure_t *failure = &scratch.bluetooth_failure;
    append(&w, ",\"ble\":{\"initialized\":%s,\"failed\":%s,\"health\":", boolean(ble->initialized), boolean(ble->failed));
    if (ble->initialized) append(&w, "{\"connected\":%s,\"ready\":%s,\"congested\":%s,\"encrypted\":%s,\"advertising\":%s,\"pairing_open\":%s,\"generation\":\"%" PRIu64 "\"}",
        boolean(ble->connected), boolean(ble->ready), boolean(ble->congested), boolean(ble->encrypted), boolean(ble->advertising), boolean(ble->pairing_open), ble->generation);
    else { append(&w, "null"); }
    append(&w, ",\"first_failure\":");
    if (failure->recorded) {
        bool stage_known = failure->stage && cj_escape(failure->stage, scratch.stage, sizeof(scratch.stage));
        append(&w, "{\"stage\":%s,\"error\":%d,\"uptime_ms\":\"%" PRIu64 "\",\"sdk_status\":", stage_known ? scratch.stage : "null", failure->error, failure->uptime_ms);
        if (failure->sdk_status_known) append(&w, "%u", failure->sdk_status); else append(&w, "null");
        append(&w, "}");
    } else { append(&w, "null"); }
    append(&w, "}");
    ae_snapshot_t *audio = &scratch.audio;
    append(&w, ",\"audio\":{\"snapshot_available\":%s,\"health\":", boolean(audio_known));
    if (audio_known) append(&w, "{\"state\":%u,\"route\":%u,\"generation\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\",\"microphone_enabled\":%s,\"speaker_enabled\":%s,\"microphone_muted\":%s,\"speaker_muted\":%s,\"capture_gate_open\":%s,\"playback_gate_open\":%s,\"hardware_off_confirmed\":%s,\"quarantined\":%s,\"error\":%d}",
        (unsigned)audio->state, (unsigned)audio->route, audio->audio_generation, audio->revision,
        boolean(audio->microphone_enabled), boolean(audio->speaker_enabled), boolean(audio->microphone_muted), boolean(audio->speaker_muted),
        boolean(audio->capture_gate_open), boolean(audio->playback_gate_open), boolean(audio->hardware_off_confirmed), boolean(audio->quarantined), (int)audio->error);
    else append(&w, "null");
    board_audio_status_t *codec = &scratch.codec;
    append(&w, ",\"codec\":{\"worker_started\":%s,\"probe_complete\":%s,\"probe_result\":", boolean(codec->worker_started), boolean(codec->probe_complete));
    error_value(&w, (int)codec->probe_result, codec->probe_complete);
    append(&w, ",\"hardware_observed\":%s,\"cleanup_uncertain\":%s,\"first_sdk_error\":", boolean(codec->hardware_observed), boolean(codec->cleanup_uncertain));
    error_value(&w, codec->first_sdk_error, codec->first_sdk_error != 0); append(&w, "}}");
    app_update_snapshot_t *update = &scratch.update;
    append(&w, ",\"update\":{\"available\":%s,\"state\":%u,\"error\":%d,\"revision\":\"%" PRIu64 "\",\"cleanup_pending\":%s,\"running_state_known\":%s,\"running_confirmed\":",
        boolean(update->available), (unsigned)update->state, update->error, update->revision, boolean(update->cleanup_pending), boolean(update->running_state_known));
    if (update->running_state_known) append(&w, "%s", boolean(update->running_confirmed)); else append(&w, "null");
    append(&w, ",\"running_state_error\":%d,\"probation_active\":%s,\"health_confirmation_attempted\":%s,\"health_confirmation_error\":",
        update->running_state_error, boolean(update->probation_active), boolean(update->health_confirmation_attempted));
    error_value(&w, update->health_confirmation_error, update->health_confirmation_attempted); append(&w, "}}");
    result = w.valid ? CP_OK : CP_TOO_LARGE;
    if (w.valid) *length = w.used;
done:
    /* Copied SSIDs/addresses and audio job metadata never remain retained. */
    for (volatile unsigned char *p = (volatile unsigned char *)&scratch; p < (volatile unsigned char *)&scratch + sizeof(scratch); ++p) *p = 0;
    if (result != CP_OK) { *length = 0; json[0] = 0; }
    portENTER_CRITICAL(&diagnostics_lock); diagnostics_busy = false; portEXIT_CRITICAL(&diagnostics_lock);
    return result;
}
