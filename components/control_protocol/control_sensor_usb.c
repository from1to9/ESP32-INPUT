// SPDX-License-Identifier: Apache-2.0
#include "control_sensor_usb.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool hex_span(const char *p, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}
static bool decimal(const char *p, size_t length, uint64_t *out)
{
    if (!length || length > 20 || (length > 1 && p[0] == '0')) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < length; ++i) {
        if (p[i] < '0' || p[i] > '9' || n > (UINT64_MAX - (unsigned)(p[i] - '0')) / 10U) return false;
        n = n * 10U + (unsigned)(p[i] - '0');
    }
    *out = n; return true;
}
static size_t base64(const uint8_t *in, size_t n, char *out, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((n + 2U) / 3U) * 4U;
    if (capacity <= needed) return 0;
    size_t used = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[used++] = alphabet[v >> 18]; out[used++] = alphabet[(v >> 12) & 63U];
        out[used++] = i + 1 < n ? alphabet[(v >> 6) & 63U] : '=';
        out[used++] = i + 2 < n ? alphabet[v & 63U] : '=';
    }
    out[used] = 0; return used;
}
void cp_sensor_usb_cancel(cp_sensor_usb_t *s)
{
    if (!s->retained && !s->pending && !s->length && !s->command[0]) return;
    uint64_t generation = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = generation == UINT64_MAX ? UINT64_MAX : generation + 1U;
}
static void error_body(cp_sensor_usb_t *s, cp_error_t error)
{
    s->binary = false;
    int n = snprintf(s->body, sizeof(s->body), "{\"error\":\"%s\"}", cp_error_name(error));
    s->length = n > 0 && (size_t)n < sizeof(s->body) ? (size_t)n : 0;
}
bool cp_sensor_usb_receive(cp_sensor_usb_t *s, const char *line, uint64_t now,
                           const cp_sensor_usb_port_t *port)
{
    if (!line || strncmp(line, "ST1 ", 4)) return false;
    if (!s || !port || !port->allowed || !port->allowed(port->user)) { if (s) cp_sensor_usb_cancel(s); return true; }
    size_t n = strlen(line);
    if (n >= sizeof(s->command)) return true;
    const char *id = NULL, *rest = NULL; unsigned kind = 0;
    if (!strncmp(line, "ST1 READ ", 9)) { id = line + 9; kind = 3; }
    else if (!strncmp(line, "ST1 CATALOG ", 12)) { id = line + 12; kind = 1; }
    else if (!strncmp(line, "ST1 LATEST ", 11)) { id = line + 11; kind = 2; }
    if (!id || strlen(id) < 16 || !hex_span(id, 16)) return true;
    if (s->retained && now >= s->started_ms && now < s->deadline_ms) {
        if (!memcmp(id, s->request_id, 16)) {
            if (!strcmp(line, s->command)) { s->offset = 0; s->pending = true; }
            return true; /* A reused ID cannot change a retained response. */
        }
        if (s->pending) return true; /* Never replace admitted work. */
    }
    cp_sensor_usb_cancel(s);
    if (s->generation == UINT64_MAX || now > UINT64_MAX - CP_SENSOR_USB_DEADLINE_MS) return true;
    memcpy(s->request_id, id, 16); s->request_id[16] = 0;
    memcpy(s->command, line, n + 1U); s->started_ms = now; s->deadline_ms = now + CP_SENSOR_USB_DEADLINE_MS;
    s->retained = s->pending = true;
    cp_error_t error = CP_INVALID;
    if (kind == 1 || kind == 2) {
        if (!id[16] && port->read) error = port->read(port->user,
            kind == 1 ? "/api/v1/sensors/catalog" : "/api/v1/sensors/latest", s->body, sizeof(s->body), &s->length);
    } else if (id[16] == ' ') {
        rest = id + 17; char boot[33]; uint64_t after = 0, request_id = 0; unsigned limit = 0;
        if (strlen(rest) >= 35 && hex_span(rest, 32) && rest[32] == ' ') {
            memcpy(boot, rest, 32); boot[32] = 0; rest += 33;
            const char *end = strchr(rest, ' ');
            if (end && decimal(rest, (size_t)(end - rest), &after) && end[1] >= '1' && end[1] <= '3' && !end[2]) {
                limit = (unsigned)(end[1] - '0');
                for (size_t i = 0; i < 16; ++i) request_id = request_id * 16U +
                    (uint64_t)(id[i] <= '9' ? id[i] - '0' : id[i] - 'a' + 10);
                if (port->batch) error = port->batch(port->user, boot, after, limit, request_id,
                    (uint8_t *)s->body, CP_SENSOR_USB_BATCH_BYTES, &s->length);
                if (error == CP_OK) s->binary = true;
            }
        }
    }
    if (error == CP_OK && (!s->length || s->length > (s->binary ? CP_SENSOR_USB_BATCH_BYTES : CP_MAX_RESPONSE_BYTES) ||
        (!s->binary && s->body[s->length]))) error = CP_INTERNAL;
    if (error != CP_OK) error_body(s, error);
    if (!s->binary) {
        uint8_t digest[32]; cp_sha256(s->body, s->length, digest);
        for (size_t i = 0; i < 32; ++i) (void)snprintf(s->digest + i * 2U, 3, "%02x", digest[i]);
    }
    if (!port->allowed(port->user)) cp_sensor_usb_cancel(s);
    return true;
}
void cp_sensor_usb_service(cp_sensor_usb_t *s, uint64_t now, const cp_sensor_usb_port_t *port)
{
    if (!s || !port || !port->allowed || !port->allowed(port->user) ||
        (s->retained && (now < s->started_ms || now >= s->deadline_ms))) { if (s) cp_sensor_usb_cancel(s); return; }
    if (!s->pending || !port->write) return;
    size_t count = s->binary ? s->length : s->length - s->offset;
    if (!s->binary && count > CP_SENSOR_USB_FRAGMENT_BYTES) count = CP_SENSOR_USB_FRAGMENT_BYTES;
    int prefix = s->binary ? snprintf(s->frame, sizeof(s->frame), "\nST1 R %s %zu ", s->request_id, s->length) :
        snprintf(s->frame, sizeof(s->frame), "\nST1 DATA %s %zu %zu %zu ", s->request_id, s->offset, s->length, count);
    if (prefix <= 0 || (size_t)prefix >= sizeof(s->frame)) { cp_sensor_usb_cancel(s); return; }
    size_t used = (size_t)prefix;
    size_t encoded = base64((const uint8_t *)s->body + s->offset, count, s->frame + used, sizeof(s->frame) - used);
    if (!encoded) { cp_sensor_usb_cancel(s); return; }
    used += encoded; s->frame[used++] = '\n'; s->frame[used] = 0;
    bool final = s->binary || s->offset + count == s->length;
    if (final && !s->binary) {
        int n = snprintf(s->frame + used, sizeof(s->frame) - used, "ST1 END %s %zu %s\n", s->request_id, s->length, s->digest);
        if (n <= 0 || (size_t)n >= sizeof(s->frame) - used) { cp_sensor_usb_cancel(s); return; }
        used += (size_t)n;
    }
    if (used > CP_SENSOR_USB_FRAME_BYTES || !port->allowed(port->user)) { cp_sensor_usb_cancel(s); return; }
    if (port->write(port->user, s->frame, used) == used) {
        s->offset += count; if (final) s->pending = false;
    }
}
