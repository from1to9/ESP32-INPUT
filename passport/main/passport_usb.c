// SPDX-License-Identifier: Apache-2.0
#include "passport_usb.h"
#include "passport_usb_protocol.h"
#include "passport_sound.h"
#include "driver/usb_serial_jtag.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef enum { USB_IDLE, USB_BLE, USB_WIFI, USB_SOUND, USB_CUE, USB_NAME } usb_owner_t;
static passport_usb_parser_t parser;
static uint32_t boot, sequence;
static bool available;
static char frame[PUSB_FRAME_CAPACITY]; // UI owner only; one write per framed reply.
static struct {
    usb_owner_t owner;
    passport_receipt_t receipt;
    uint64_t deadline;
    bool sound_enabled;
    passport_name_domain_t domain;
    uint32_t ticket;
} job;

static void emit(uint32_t id, const char *kind, const char *code, const char *format, ...)
{
    if (!usb_serial_jtag_is_connected()) return;
    int used = snprintf(frame, sizeof(frame), "\x1eP1 %08" PRIx32 " %08" PRIx32 " %s %s", boot, id, kind, code);
    if (format && *format && used > 0 && (size_t)used + 1 < sizeof(frame)) {
        frame[used++] = ' ';
        va_list args; va_start(args, format);
        int n = vsnprintf(frame + used, sizeof(frame) - (size_t)used, format, args);
        va_end(args);
        if (n < 0 || (size_t)n >= sizeof(frame) - (size_t)used) return;
        used += n;
    }
    if (used < 0) return;
    size_t length = passport_usb_seal(frame, sizeof(frame), (size_t)used);
    if (length) (void)write(STDOUT_FILENO, frame, length);
    explicit_bzero(frame, sizeof(frame));
}
static void done(uint32_t id, passport_result_t result, int32_t detail)
{ emit(id, "done", passport_result_name(result), "detail=%" PRId32, detail); }
static void finish(passport_result_t result, int32_t detail)
{
    job.receipt.result = result; job.receipt.detail = detail; job.owner = USB_IDLE;
    done(job.receipt.id, result, detail);
}
static passport_result_t name_result(passport_name_result_t result)
{
    switch (result) {
    case PN_OK: return PR_APPLIED;
    case PN_PENDING: return PR_PENDING;
    case PN_INVALID: return PR_INVALID;
    case PN_LOCKED: return PR_LOCKED;
    case PN_STALE: case PN_NOT_FOUND: return PR_STALE;
    case PN_FULL: case PN_BUSY: return PR_BUSY;
    case PN_UNCERTAIN: return PR_STORAGE_UNCERTAIN;
    case PN_RESULT_UNKNOWN: return PR_UNKNOWN;
    default: return PR_UNAVAILABLE;
    }
}
static void poll_job(const passport_ble_state_t *ble, const passport_wifi_state_t *wifi, uint64_t now)
{
    if (!job.owner) return;
    passport_receipt_t result = {0};
    if (job.owner == USB_BLE) result = ble->command;
    else if (job.owner == USB_WIFI) result = wifi->command;
    else if (job.owner == USB_SOUND || job.owner == USB_CUE) {
        passport_sound_state_t sound = passport_sound_state();
        if (job.owner == USB_CUE) result = sound.cue;
        else if (!sound.preference_known) { finish(PR_STORAGE_UNCERTAIN, 0); return; }
        else if (sound.enabled != job.sound_enabled) { finish(PR_CANCELLED, 0); return; }
        else if (sound.output == PASSPORT_SOUND_UNAVAILABLE) { finish(PR_UNAVAILABLE, 0); return; }
        else if (sound.output == (job.sound_enabled ? PASSPORT_SOUND_READY : PASSPORT_SOUND_OFF)) { finish(PR_APPLIED, 0); return; }
    } else if (job.owner == USB_NAME) {
        passport_result_t outcome = name_result(passport_names_result(job.domain, job.ticket));
        if (outcome != PR_PENDING) { finish(outcome, 0); return; }
    }
    if (result.id == job.receipt.id && result.result != PR_PENDING && result.result != PR_NONE) {
        finish(result.result, result.detail); return;
    }
    if (now >= job.deadline) finish(PR_UNKNOWN, 0);
}
static void accepted(uint32_t id, usb_owner_t owner, uint64_t now)
{
    // Safety commands may supersede a pending request. Its complete outcome is
    // no longer retained; do not assert that its side effects were cancelled.
    if (job.owner) done(job.receipt.id, PR_UNKNOWN, 0);
    job.owner = owner; job.receipt = (passport_receipt_t){id, PR_PENDING, 0};
    job.deadline = now + 70000u;
    emit(id, "ack", "accepted", "");
}
static void status(uint32_t id, const passport_ble_state_t *b, const passport_wifi_state_t *w,
                   passport_theme_t theme, bool known)
{
    emit(id, "data", "usb", "connected=1 last=%" PRIu32 " pending=%" PRIu32 " frame=256 usb_hid=0 ble_audio=0 microphone=0 motion=0",
         sequence, job.owner ? job.receipt.id : 0);
    emit(id, "data", "ble", "synced=%u storage=%u connected=%u encrypted=%u bonded=%u keyboard=%u mouse=%u ready=%u locked=%u",
         b->synced, b->storage_known, b->connected, b->encrypted, b->bonded,
         b->keyboard_subscribed, b->mouse_subscribed, b->ready, b->locked);
    emit(id, "data", "ble_revision", "generation=%" PRIu32 " bonds=%" PRIu32 " input=%" PRIu32 " peers=%u advertising=%u pairing=%u release=%u release_unknown=%u",
         b->generation, b->bond_revision, b->input_revision, b->peer_count, b->advertising, b->pairing, b->release_pending, b->release_unknown);
    emit(id, "data", "ble_target", "selected=%s updating=%u handshake=%u window=%u network=%u controller=%s",
         *b->selected_id ? b->selected_id : "-", b->updating, b->handshake, b->window_seconds,
         b->network_active, *b->controller ? b->controller : "-");
    emit(id, "data", "wifi", "initialized=%u storage=%u phase=%u ap=%u dhcp=%u client=%u lease=%u sta_ip=%s ap_ip=%s",
         w->initialized, w->storage_known, (unsigned)w->phase, w->ap_started, w->dhcp_running,
         w->ap_client, w->ap_lease, *w->sta_ip ? w->sta_ip : "-", *w->ap_ip ? w->ap_ip : "-");
    emit(id, "data", "wifi_revision", "profiles=%u revision=%" PRIu32 " active=%" PRIu32 " scan=%" PRIu32 " rows=%u scanning=%u scan_known=%u truncated=%u",
         w->profile_count, w->revision, w->active_profile, w->scan_revision, w->scan_count, w->scanning, w->scan_known, w->scan_truncated);
    emit(id, "data", "setup", "open=%u id=%" PRIu32 " pending=%u request=%" PRIu32 " state=%u seconds=%u",
         w->setup_open, w->setup_id, w->pending, w->pending_id, w->request_state, w->setup_seconds);
    passport_sound_state_t sound = passport_sound_state();
    emit(id, "data", "settings", "theme=%u theme_known=%u sound=%u sound_known=%u output=%u",
         theme, known, sound.enabled, sound.preference_known, sound.output);
    passport_name_catalog_t bn = passport_names_catalog(PN_BLE), wn = passport_names_catalog(PN_WIFI);
    emit(id, "data", "names", "ble_known=%u ble_revision=%" PRIu32 " wifi_known=%u wifi_revision=%" PRIu32,
         bn.known, bn.revision, wn.known, wn.revision);
    done(id, PR_APPLIED, 0);
}
static bool uarg(const passport_usb_request_t *r, unsigned index, uint32_t *out)
{ return index < r->count && passport_usb_u32(r->args[index], out); }
static bool iarg(const passport_usb_request_t *r, unsigned index, int32_t *out)
{ return index < r->count && passport_usb_i32(r->args[index], out) && *out >= -127 && *out <= 127; }

static bool read_request(const passport_usb_request_t *r, const passport_ble_state_t *b,
                         const passport_wifi_state_t *w, passport_theme_t theme, bool known)
{
    const char *op = r->args[0]; uint32_t revision, index;
    if (!strcmp(op, "status") && r->count == 1) { status(r->id, b, w, theme, known); return true; }
    if (!strcmp(op, "result") && r->count == 2 && uarg(r, 1, &index)) {
        if (index != job.receipt.id || !index) done(r->id, PR_UNKNOWN, 0);
        else { emit(r->id, "data", "result", "id=%" PRIu32 " result=%s detail=%" PRId32,
                    index, passport_result_name(job.receipt.result), job.receipt.detail); done(r->id, PR_APPLIED, 0); }
        return true;
    }
    if (!strcmp(op, "wifi.ap_credentials") && r->count == 1) {
        if (b->locked) done(r->id, PR_LOCKED, 0);
        else if (!w->initialized) done(r->id, PR_UNAVAILABLE, 0);
        else {
            char ssid[32], password[17];
            passport_wifi_ap_credentials(ssid, password);
            emit(r->id, "data", "ap_credentials", "ssid=%s password=%s", ssid, password);
            explicit_bzero(password, sizeof(password)); explicit_bzero(ssid, sizeof(ssid));
            done(r->id, PR_APPLIED, 0);
        }
        return true;
    }
    if (!strcmp(op, "ble.peer") && r->count == 3 && uarg(r, 1, &revision) && uarg(r, 2, &index)) {
        if (!b->storage_known) done(r->id, PR_UNAVAILABLE, 0);
        else if (revision != b->bond_revision || index >= b->peer_count) done(r->id, PR_STALE, 0);
        else {
            const passport_peer_t *p = &b->peers[index]; char address[13], bond[33];
            passport_usb_hex(p->address, 6, address); passport_usb_hex(p->bond_id, 16, bond);
            emit(r->id, "data", "peer", "index=%" PRIu32 " revision=%" PRIu32 " type=%u address=%s bond=%s", index, revision, p->type, address, bond);
            done(r->id, PR_APPLIED, 0);
        }
        return true;
    }
    bool row = !strcmp(op, "wifi.row"), profile = !strcmp(op, "wifi.profile");
    if ((row || profile) && r->count == 3 && uarg(r, 1, &revision) && uarg(r, 2, &index)) {
        if (!(row ? w->scan_known : w->storage_known)) done(r->id, PR_UNAVAILABLE, 0);
        else if (revision != (row ? w->scan_revision : w->revision) || index >= (row ? w->scan_count : w->profile_count)) done(r->id, PR_STALE, 0);
        else {
            char ssid[65]; const passport_ssid_t *s = row ? &w->rows[index].ssid : &w->profiles[index].ssid;
            passport_usb_hex(s->bytes, s->length, ssid);
            if (row) {
                const passport_wifi_row_t *p = &w->rows[index]; char address[13]; passport_usb_hex(p->bssid, 6, address);
                emit(r->id, "data", "row", "index=%" PRIu32 " revision=%" PRIu32 " ssid=%s bssid=%s auth=%u security=%u rssi=%d channel=%u profile=%" PRIu32,
                     index, revision, ssid, address, p->auth, p->security, p->rssi, p->channel, p->profile_id);
            } else emit(r->id, "data", "profile", "index=%" PRIu32 " revision=%" PRIu32 " id=%" PRIu32 " ssid=%s security=%u",
                        index, revision, w->profiles[index].id, ssid, w->profiles[index].security);
            done(r->id, PR_APPLIED, 0);
        }
        return true;
    }
    return false;
}

static void dispatch(const passport_usb_request_t *r, const passport_ble_state_t *b,
                     const passport_wifi_state_t *w, passport_theme_t theme, bool known, bool settings_busy, uint64_t now)
{
    const char *op = r->args[0];
    if (!strcmp(op, "hello") && r->count == 1 && !r->boot) {
        const esp_app_desc_t *app = esp_app_get_description(); char version[65];
        passport_usb_hex(app->version, strnlen(app->version, sizeof(app->version)), version);
        emit(r->id, "data", "hello", "last=%" PRIu32 " pending=%" PRIu32 " protocol=1 firmware=%s", sequence, job.owner ? job.receipt.id : 0, version);
        done(r->id, PR_APPLIED, 0); return;
    }
    if (r->boot != boot || !r->id) { done(r->id, PR_STALE, 0); return; }
    if (r->id == job.receipt.id) {
        if (job.owner) emit(r->id, "ack", "accepted", "");
        else done(r->id, job.receipt.result, job.receipt.detail);
        return;
    }
    if (r->id <= sequence) { done(r->id, PR_STALE, 0); return; }
    sequence = r->id;
    if (read_request(r, b, w, theme, known)) return;
    bool safety = !strcmp(op, "lock") || !strcmp(op, "cancel") || !strcmp(op, "ble.close") ||
        !strcmp(op, "wifi.scan_cancel") || !strcmp(op, "wifi.setup_close") || !strcmp(op, "wifi.reject") ||
        (!strcmp(op, "sound") && r->count == 2 && !strcmp(r->args[1], "0"));
    bool name_read = r->count == 4 && (!strcmp(op, "name.ble") || !strcmp(op, "name.profile") || !strcmp(op, "name.ap"));
    if (job.owner && !safety && !name_read) { done(r->id, PR_BUSY, 0); return; }

    passport_command_t c = {.command_id = r->id}; uint32_t value, index; int32_t x, y;
    bool ble_command = true;
    if (!strcmp(op, "lock") && r->count == 1) c.kind = PASSPORT_LOCK;
    else if (!strcmp(op, "ble.close") && r->count == 1) c.kind = PASSPORT_STOP_PAIR;
    else if ((!strcmp(op, "unlock") || !strcmp(op, "cancel") || !strcmp(op, "takeover")) && r->count == 2 && uarg(r, 1, &c.generation))
        c.kind = !strcmp(op, "unlock") ? PASSPORT_UNLOCK : !strcmp(op, "cancel") ? PASSPORT_CANCEL : PASSPORT_TAKEOVER;
    else if (!strcmp(op, "ble.pair") && r->count == 3 && uarg(r, 1, &c.generation) && uarg(r, 2, &c.window_ms) && c.window_ms >= 1000 && c.window_ms <= 60000) c.kind = PASSPORT_PAIR;
    else if ((!strcmp(op, "ble.target") || !strcmp(op, "ble.disconnect") || !strcmp(op, "ble.forget")) && r->count == 4 &&
             uarg(r, 1, &c.generation) && uarg(r, 2, &c.bond_revision) && uarg(r, 3, &index)) {
        if (!b->storage_known || c.bond_revision != b->bond_revision || index >= b->peer_count) { done(r->id, PR_STALE, 0); return; }
        c.kind = !strcmp(op, "ble.target") ? PASSPORT_ADVERTISE : !strcmp(op, "ble.forget") ? PASSPORT_FORGET : PASSPORT_DISCONNECT;
        c.peer_type = b->peers[index].type; memcpy(c.peer, b->peers[index].address, 6);
    } else if (!strcmp(op, "mouse.move") && r->count == 5 && uarg(r, 1, &c.generation) && uarg(r, 2, &c.input_revision) && iarg(r, 3, &x) && iarg(r, 4, &y)) {
        c.kind = PASSPORT_MOVE; c.x = x; c.y = y;
    } else if (!strcmp(op, "mouse.scroll") && r->count == 4 && uarg(r, 1, &c.generation) && uarg(r, 2, &c.input_revision) && iarg(r, 3, &y)) {
        c.kind = PASSPORT_SCROLL; c.y = y;
    } else if (!strcmp(op, "mouse.click") && r->count == 4 && uarg(r, 1, &c.generation) && uarg(r, 2, &c.input_revision) && uarg(r, 3, &value) && value >= 1 && value <= 7) {
        c.kind = PASSPORT_CLICK; c.buttons = value;
    } else if (!strcmp(op, "key") && r->count == 5 && uarg(r, 1, &c.generation) && uarg(r, 2, &c.input_revision) && uarg(r, 3, &value) && value <= 255 && uarg(r, 4, &index) && (index == 0 ? value != 0 : index >= 4 && index <= 0x65)) {
        c.kind = PASSPORT_KEY; c.modifier = value; c.usage = index;
    } else ble_command = false;
    if (ble_command) {
        if (!b->synced) done(r->id, PR_UNAVAILABLE, 0);
        else if (passport_ble_request(&c)) accepted(r->id, USB_BLE, now); else done(r->id, PR_BUSY, 0);
        return;
    }
    passport_wifi_command_t wc = {.command_id = r->id}; bool wifi_command = true;
    if ((!strcmp(op, "wifi.scan") || !strcmp(op, "wifi.scan_cancel")) && r->count == 2 && uarg(r, 1, &wc.revision))
        wc.action = !strcmp(op, "wifi.scan") ? PW_SCAN : PW_SCAN_CANCEL;
    else if ((!strcmp(op, "wifi.use_ap") || !strcmp(op, "wifi.setup_open") || !strcmp(op, "wifi.setup_close")) && r->count == 1)
        wc.action = !strcmp(op, "wifi.use_ap") ? PW_USE_AP : !strcmp(op, "wifi.setup_open") ? PW_SETUP_OPEN : PW_SETUP_CLOSE;
    else if ((!strcmp(op, "wifi.connect") || !strcmp(op, "wifi.disconnect") || !strcmp(op, "wifi.forget") || !strcmp(op, "wifi.confirm") || !strcmp(op, "wifi.reject")) &&
             r->count == 3 && uarg(r, 1, &wc.revision) && uarg(r, 2, &wc.id))
        wc.action = !strcmp(op, "wifi.connect") ? PW_CONNECT : !strcmp(op, "wifi.disconnect") ? PW_DISCONNECT : !strcmp(op, "wifi.forget") ? PW_FORGET : !strcmp(op, "wifi.confirm") ? PW_CONFIRM : PW_REJECT;
    else wifi_command = false;
    if (wifi_command) {
        if (!w->initialized) done(r->id, PR_UNAVAILABLE, 0);
        else if (passport_wifi_request(&wc)) accepted(r->id, USB_WIFI, now); else done(r->id, PR_BUSY, 0);
        return;
    }
    if (!strcmp(op, "wifi.join") && r->count == 4) {
        passport_wifi_submission_t submission = {.request_id = r->id}; size_t length;
        bool valid = uarg(r, 1, &submission.scan_revision) && uarg(r, 2, &submission.row) &&
            passport_usb_unhex(r->args[3], submission.password, 63, &length);
        if (valid) for (size_t i = 0; i < length; ++i) if (submission.password[i] < 32 || submission.password[i] > 126) valid = false;
        bool queued = valid && w->initialized && passport_wifi_submit_usb(&submission);
        explicit_bzero(&submission, sizeof(submission));
        if (queued) accepted(r->id, USB_WIFI, now);
        else done(r->id, !valid ? PR_INVALID : !w->initialized ? PR_UNAVAILABLE : PR_BUSY, 0);
        return;
    }
    bool name_ble = !strcmp(op, "name.ble"), name_profile = !strcmp(op, "name.profile"), name_ap = !strcmp(op, "name.ap");
    if ((name_ble || name_profile || name_ap) && (r->count == 4 || r->count == 5)) {
        passport_name_change_t change = {0}; uint32_t revision; size_t length;
        if (!uarg(r, 1, &change.revision) || !uarg(r, 2, &revision) || !uarg(r, 3, &index)) { done(r->id, PR_INVALID, 0); return; }
        if (name_ble) {
            if (!b->storage_known || revision != b->bond_revision || index >= b->peer_count) { done(r->id, PR_STALE, 0); return; }
            change.key.kind = PN_HID_BOND; memcpy(change.key.bond, b->peers[index].bond_id, PASSPORT_BOND_ID_BYTES);
        } else if (name_profile) {
            if (!w->storage_known || revision != w->revision || index >= w->profile_count) { done(r->id, PR_STALE, 0); return; }
            change.key.kind = PN_WIFI_PROFILE; change.key.profile = w->profiles[index].id;
        } else {
            if (!w->scan_known || revision != w->scan_revision || index >= w->scan_count) { done(r->id, PR_STALE, 0); return; }
            change.key = passport_wifi_ap_name_key(&w->rows[index]); change.scan_revision = revision;
        }
        if (r->count == 4) {
            char label[PASSPORT_NAME_BYTES * 2 + 1];
            passport_name_result_t rc = passport_names_lookup(&change.key, change.label);
            if (rc == PN_OK || rc == PN_NOT_FOUND) {
                passport_usb_hex(change.label, rc == PN_NOT_FOUND ? 0 : strlen(change.label), label);
                emit(r->id, "data", "name", "label=%s", label);
            }
            done(r->id, rc == PN_NOT_FOUND ? PR_APPLIED : name_result(rc), rc); return;
        }
        if (!passport_usb_unhex(r->args[4], change.label, PASSPORT_NAME_BYTES, &length) ||
            (length && !passport_names_text_valid(change.label, length))) { done(r->id, PR_INVALID, 0); return; }
        change.clear = length == 0;
        uint32_t ticket;
        passport_name_result_t rc = passport_names_submit(&change, &ticket);
        if (rc == PN_OK) { accepted(r->id, USB_NAME, now); job.domain = name_ble ? PN_BLE : PN_WIFI; job.ticket = ticket; }
        else done(r->id, name_result(rc), rc);
        return;
    }
    if (!strcmp(op, "theme") && r->count == 2 && uarg(r, 1, &value) && value <= PASSPORT_LIGHT) {
        if (b->locked) { done(r->id, PR_LOCKED, 0); return; }
        if (settings_busy) { done(r->id, PR_BUSY, 0); return; }
        accepted(r->id, USB_IDLE, now);
        esp_err_t rc = passport_ui_set_theme((passport_theme_t)value);
        finish(rc == ESP_OK ? PR_APPLIED : PR_STORAGE_UNCERTAIN, rc); return;
    }
    if ((!strcmp(op, "sound") || !strcmp(op, "sound.cue")) && r->count == 2 && uarg(r, 1, &value)) {
        bool cue = !strcmp(op, "sound.cue");
        if (value > (cue ? PASSPORT_CUE_ERROR : 1) || (cue && !value)) { done(r->id, PR_INVALID, 0); return; }
        if (b->locked) { done(r->id, PR_LOCKED, 0); return; }
        if (settings_busy) { done(r->id, PR_BUSY, 0); return; }
        if (cue) {
            if (passport_sound_cue_usb((passport_cue_t)value, r->id)) accepted(r->id, USB_CUE, now);
            else done(r->id, PR_UNAVAILABLE, 0);
        } else {
            accepted(r->id, USB_SOUND, now); job.sound_enabled = value;
            esp_err_t rc = passport_ui_set_sound(value);
            if (rc != ESP_OK) finish(passport_sound_state().preference_known ? PR_BUSY : PR_STORAGE_UNCERTAIN, rc);
        }
        return;
    }
    done(r->id, PR_INVALID, 0);
}

void passport_usb_init(void)
{
    do { boot = esp_random(); } while (!boot);
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    // The pinned no-driver rx_func polls one FIFO byte without waiting. Its VFS
    // O_NONBLOCK branch instead checks only a driver ring and skips that poll.
    // Do not use the potentially blocking driver-backed path on the UI task.
    available = !usb_serial_jtag_is_driver_installed() && flags >= 0 &&
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) == 0;
}
void passport_usb_service(const passport_ble_state_t *ble, const passport_wifi_state_t *wifi,
                          passport_theme_t theme, bool theme_known, bool settings_busy)
{
    uint64_t now = esp_timer_get_time() / 1000;
    poll_job(ble, wifi, now);
    if (!available || !usb_serial_jtag_is_connected()) { passport_usb_parser_reset(&parser); return; }
    (void)passport_usb_parser_expire(&parser, now);
    for (unsigned i = 0; i < PUSB_FRAME_CAPACITY; ++i) {
        unsigned char byte;
        if (read(STDIN_FILENO, &byte, 1) != 1) break;
        passport_usb_request_t request;
        passport_usb_parse_t result = passport_usb_feed(&parser, byte, now, &request);
        if (result == PUSB_FRAME) {
            dispatch(&request, ble, wifi, theme, theme_known, settings_busy, now);
            passport_usb_parser_reset(&parser); // Includes all submitted password bytes.
            break;
        }
    }
}
