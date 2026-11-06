// SPDX-License-Identifier: Apache-2.0
#include "bt_classic_connect_diag.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

enum {
    RECORDS = 48, GROUP = 5, PEERS = 4, SAMPLES = 8,
    POST_RECORDS = 8, POST_MS = 2000, ATTEMPT_MS = 20000,
    CREATE = 0x0405, CREATE_CANCEL = 0x0408, NO_ENTRY = 0x09,
    UNKNOWN = 0xff, NO_HANDLE = 0xffff,
};

enum capture_state { OPEN, WRITING, FROZEN };
enum event_kind { TX = 1, STATUS, COMPLETE, LINK_UP, LINK_REQUEST, LINK_DOWN, NAME, LE_UP, SCO_UP };
enum sample_kind { ALLOC_FAILURE = 1, SCAN_END };
enum peer_relation { ADDRESSLESS, SAME_PEER, OTHER_PEER, PEER_UNKNOWN = UNKNOWN };

typedef struct {
    uint32_t at_ms, sequence;
    uint16_t code, handle;
    uint8_t kind, status, peer, detail;
} hci_record_t;

typedef struct {
    uint32_t at_ms;
    uint8_t kind, result, capacity, relation;
    uint8_t before_bits, before_state, after_bits, after_state;
} olm_sample_t;

typedef struct {
    hci_record_t records[RECORDS];
    uint8_t peers[PEERS][6];
    char line[GROUP * sizeof(hci_record_t) * 2 + 1];
    uint32_t sequence, overwritten, attempt, terminal;
    uint8_t next, count, peer_count, attempt_peer, post_count;
    bool create_accepted, overlap, peer_limit, printed;
} hci_trace_t;

typedef struct {
    olm_sample_t samples[SAMPLES];
    atomic_uint published, omitted, unavailable;
    atomic_uint state, deadline, busy_loss, armed;
} trace_control_t;

_Static_assert(sizeof(hci_record_t) == 16 && sizeof(olm_sample_t) == 12, "Trace wire layout");
_Static_assert(sizeof(hci_trace_t) <= 1024, "Classic trace PSRAM budget");
_Static_assert(sizeof(trace_control_t) <= 128, "Classic trace internal budget");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Trace publications must be lock free");

static EXT_RAM_BSS_ATTR hci_trace_t hci;
static DRAM_ATTR trace_control_t trace;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t now_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000U);
}

/* These counters have one producer each. A reader only observes the value. */
static void count_loss(atomic_uint *counter)
{
    unsigned n = atomic_load_explicit(counter, memory_order_relaxed);
    if (n != UINT32_MAX) atomic_store_explicit(counter, n + 1U, memory_order_relaxed);
}

static bool command(uint16_t code)
{
    switch (code) {
    case CREATE: case 0x0406: case CREATE_CANCEL: case 0x0409: case 0x040a:
    case 0x0419: case 0x041a: case 0x043f: case 0x0440:
    case 0x0c17: case 0x0c18: case 0x0c19: case 0x0c1a:
        return true;
    default:
        return false;
    }
}

static uint8_t peer_tag(const uint8_t *addr)
{
    if (!addr) return 0;
    for (uint8_t i = 0; i < hci.peer_count; ++i) {
        if (memcmp(hci.peers[i], addr, 6) == 0) return i + 1U;
    }
    if (hci.peer_count == PEERS) {
        hci.peer_limit = true;
        return UNKNOWN;
    }
    memcpy(hci.peers[hci.peer_count], addr, 6);
    return ++hci.peer_count;
}

/* TX and dequeued RX run on the HCI host task. The coordinator can freeze
 * between records. A writer never waits for an exporter. */
static bool due(uint32_t now, unsigned deadline)
{
    return deadline && (int32_t)(now - deadline) >= 0;
}

static void freeze(void)
{
    atomic_store_explicit(&trace.armed, false, memory_order_relaxed);
    atomic_store_explicit(&trace.state, FROZEN, memory_order_release);
}

static void retain(hci_record_t *record, const uint8_t *addr)
{
    unsigned expected = OPEN;
    if (!atomic_compare_exchange_strong_explicit(&trace.state, &expected, WRITING,
            memory_order_acquire, memory_order_relaxed)) {
        if (expected == WRITING) count_loss(&trace.busy_loss);
        return;
    }
    record->at_ms = now_ms();
    if (hci.sequence == UINT32_MAX ||
        due(record->at_ms, atomic_load_explicit(&trace.deadline, memory_order_relaxed))) {
        freeze();
        return;
    }
    record->sequence = ++hci.sequence;
    record->peer = peer_tag(addr);
    hci.records[hci.next] = *record;
    hci.next = (uint8_t)((hci.next + 1U) % RECORDS);
    if (hci.count < RECORDS) ++hci.count;
    else if (hci.overwritten != UINT32_MAX) ++hci.overwritten;

    if (hci.terminal) ++hci.post_count;
    if (record->kind == TX && record->code == CREATE) {
        if (!hci.attempt) {
            hci.attempt = record->sequence;
            hci.attempt_peer = record->peer;
            atomic_store_explicit(&trace.deadline, record->at_ms + ATTEMPT_MS, memory_order_release);
            atomic_store_explicit(&trace.armed, true, memory_order_release);
        } else if (!hci.terminal) {
            hci.overlap = true;
        }
    } else if (hci.attempt && !hci.terminal) {
        bool terminal = false;
        if (record->kind == STATUS && record->code == CREATE) {
            hci.create_accepted = record->status == 0;
            terminal = !hci.create_accepted;
        } else if (record->kind == LINK_UP && record->detail == 1 && hci.create_accepted) {
            terminal = record->peer != UNKNOWN && record->peer == hci.attempt_peer;
        } else if (record->kind == COMPLETE && record->code == CREATE_CANCEL && record->status == 0) {
            terminal = record->peer != UNKNOWN && record->peer == hci.attempt_peer;
        }
        if (terminal) {
            hci.terminal = record->sequence;
            atomic_store_explicit(&trace.deadline, record->at_ms + POST_MS, memory_order_release);
        }
    }
    if (hci.post_count == POST_RECORDS) {
        freeze();
    } else {
        atomic_store_explicit(&trace.state, OPEN, memory_order_release);
    }
}

void bt_classic_connect_diag_tx(const uint8_t *p, size_t len)
{
    if (len < 4 || p[0] != 1 || p[3] != len - 4) return;
    uint16_t code = le16(p + 1);
    if (!command(code)) return;

    hci_record_t r = {.code = code, .handle = NO_HANDLE, .kind = TX, .status = UNKNOWN, .detail = UNKNOWN};
    const uint8_t *addr = NULL;
    switch (code) {
    case CREATE:
        if (p[3] != 13) return;
        addr = p + 4;
        break;
    case CREATE_CANCEL: case 0x0409: case 0x040a: case 0x0419: case 0x041a: case 0x043f: case 0x0440:
        if (p[3] < 6) return;
        addr = p + 4;
        break;
    case 0x0406:
        if (p[3] != 3) return;
        r.handle = le16(p + 4) & 0x0fffU;
        r.detail = p[6];
        break;
    case 0x0c1a:
        if (p[3] != 1) return;
        r.detail = p[4];
        break;
    case 0x0c18:
        if (p[3] != 2) return;
        r.handle = le16(p + 4); /* Page timeout in slots. */
        break;
    default:
        break;
    }
    retain(&r, addr);
}

void bt_classic_connect_diag_rx(const uint8_t *p, size_t len)
{
    if (len < 3 || p[0] != 4 || p[2] != len - 3) return;
    uint8_t bytes = p[2];
    hci_record_t r = {.code = p[1], .handle = NO_HANDLE, .status = UNKNOWN, .detail = UNKNOWN};
    const uint8_t *addr = NULL;
    const uint8_t *data = p + 3;
    switch (p[1]) {
    case 0x0f:
        if (bytes != 4) return;
        r.code = le16(data + 2);
        if (!command(r.code)) return;
        r.kind = STATUS;
        r.status = data[0];
        break;
    case 0x0e:
        if (bytes < 4) return;
        r.code = le16(data + 1);
        if (!command(r.code)) return;
        r.kind = COMPLETE;
        r.status = data[3];
        if (r.status == 0) {
            if (r.code == 0x0c19 && bytes >= 5) r.detail = data[4];
            if (r.code == 0x0c17 && bytes >= 6) r.handle = le16(data + 4);
            if ((r.code == CREATE_CANCEL || r.code == 0x041a || r.code == 0x0440) && bytes >= 10) addr = data + 4;
        }
        break;
    case 0x03: case 0x2c:
        if (bytes != (p[1] == 0x03 ? 11 : 17)) return;
        r.kind = p[1] == 0x03 ? LINK_UP : SCO_UP;
        r.status = data[0];
        r.detail = data[9];
        addr = data + 3;
        if (r.status == 0) r.handle = le16(data + 1) & 0x0fffU;
        break;
    case 0x04:
        if (bytes != 10) return;
        r.kind = LINK_REQUEST;
        r.detail = data[9];
        addr = data;
        break;
    case 0x05:
        if (bytes != 4) return;
        r.kind = LINK_DOWN;
        r.status = data[0];
        r.handle = le16(data + 1) & 0x0fffU;
        r.detail = data[3];
        break;
    case 0x07:
        if (bytes < 7) return;
        r.kind = NAME;
        r.status = data[0];
        addr = data + 1;
        break;
    case 0x3e:
        if (!bytes) return;
        if ((data[0] == 1 && bytes != 19) || (data[0] == 0x0a && bytes != 31) ||
            (data[0] == 0x29 && bytes != 34) || (data[0] != 1 && data[0] != 0x0a && data[0] != 0x29)) return;
        r.kind = LE_UP;
        r.status = data[1];
        r.detail = data[0];
        if (r.status == 0) {
            r.handle = le16(data + 2) & 0x0fffU;
            addr = data + 6;
        }
        break;
    default:
        return;
    }
    retain(&r, addr);
}

/* Private ABI verified against the controller archive pinned in CMake.
 * OLM's message dispatcher owns these entries. Sample in that caller,
 * with the original arguments and return value left intact. */
extern const uint8_t *r_olm_cfg_get(void);
extern const uint8_t *r_olm_get_conn_info(uint8_t index);
extern uint16_t r_olm_conn_get_state(uint8_t index);
extern uint8_t __real_r_olm_conn_alloc(const uint8_t *addr, uint8_t *link);
extern void __real_r_olm_pscan_end(uint8_t link, const void *event);

typedef struct { uint8_t capacity, bits, state, relation; } slot_t;

static bool observing_slot(void)
{
    if (!atomic_load_explicit(&trace.armed, memory_order_acquire)) return false;
    if (xPortInIsrContext() || esp_cpu_get_core_id() != CONFIG_BT_CTRL_PINNED_TO_CORE) {
        /* This path may have a different producer. It only sets a flag. */
        atomic_store_explicit(&trace.unavailable, true, memory_order_relaxed);
        return false;
    }
    return true;
}

static slot_t read_slot(const uint8_t *addr)
{
    slot_t view = {UNKNOWN, UNKNOWN, UNKNOWN, PEER_UNKNOWN};
    const uint8_t *cfg = r_olm_cfg_get();
    if (!cfg) return view;
    view.capacity = cfg[2];
    if (view.capacity != 1) return view;
    const uint8_t *entry = r_olm_get_conn_info(0);
    if (!entry) return view;
    view.bits = entry[32] & 3U;
    if ((view.bits & 1U) == 0) return view;
    uint16_t state = r_olm_conn_get_state(0);
    if (state < UNKNOWN) view.state = (uint8_t)state;
    if ((view.bits & 2U) == 0) view.relation = ADDRESSLESS;
    else if (addr) view.relation = memcmp(entry + 18, addr, 6) == 0 ? SAME_PEER : OTHER_PEER;
    return view;
}

static void retain_olm(uint8_t kind, uint8_t result, slot_t before, slot_t after, uint32_t at_ms)
{
    if (!atomic_load_explicit(&trace.armed, memory_order_relaxed)) return;
    unsigned index = atomic_load_explicit(&trace.published, memory_order_relaxed);
    if (index == SAMPLES || (kind == SCAN_END && index >= SAMPLES - 1U)) {
        count_loss(&trace.omitted);
        return;
    }
    trace.samples[index] = (olm_sample_t){
        .at_ms = at_ms, .kind = kind, .result = result, .capacity = before.capacity,
        .relation = before.relation, .before_bits = before.bits, .before_state = before.state,
        .after_bits = after.bits, .after_state = after.state,
    };
    atomic_store_explicit(&trace.published, index + 1U, memory_order_release);
}

uint8_t __wrap_r_olm_conn_alloc(const uint8_t *addr, uint8_t *link)
{
    bool observe = observing_slot();
    slot_t before = {0};
    if (observe) before = read_slot(addr);
    uint8_t result = __real_r_olm_conn_alloc(addr, link);
    if (observe && result == NO_ENTRY) {
        slot_t after = read_slot(addr);
        retain_olm(ALLOC_FAILURE, result, before, after, now_ms());
    }
    return result;
}

void bt_classic_connect_diag_scan_end(uint8_t link, const void *event)
{
    bool observe = observing_slot();
    slot_t before = {0};
    if (observe) before = read_slot(NULL);
    __real_r_olm_pscan_end(link, event);
    if (observe) {
        slot_t after = read_slot(NULL);
        retain_olm(SCAN_END, UNKNOWN, before, after, now_ms());
    }
}

#if !CONFIG_APP_CLASSIC_SCAN_COORDINATION
void __wrap_r_olm_pscan_end(uint8_t link, const void *event)
{
    bt_classic_connect_diag_scan_end(link, event);
}
#endif

static char *hex_record(char *out, const void *data, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        *out++ = hex[bytes[i] >> 4];
        *out++ = hex[bytes[i] & 15U];
    }
    *out = '\0';
    return out;
}

void bt_classic_connect_diag_poll(uint64_t now)
{
    if (hci.printed) return;
    unsigned state = atomic_load_explicit(&trace.state, memory_order_acquire);
    if (state != FROZEN) {
        unsigned deadline = atomic_load_explicit(&trace.deadline, memory_order_acquire);
        if (state != OPEN || !due((uint32_t)now, deadline)) return;
        if (!atomic_compare_exchange_strong_explicit(&trace.state, &state, WRITING,
                memory_order_acquire, memory_order_relaxed)) return;
        /* The HCI owner may have committed a terminal and a new deadline
         * between the first read and acquiring the lease. */
        if (!due((uint32_t)now, atomic_load_explicit(&trace.deadline, memory_order_relaxed))) {
            atomic_store_explicit(&trace.state, OPEN, memory_order_release);
            return;
        }
        freeze();
    }
    hci.printed = true;
    unsigned samples = atomic_load_explicit(&trace.published, memory_order_acquire);
    unsigned omitted = atomic_load_explicit(&trace.omitted, memory_order_relaxed);
    unsigned unavailable = atomic_load_explicit(&trace.unavailable, memory_order_relaxed);
    unsigned busy_loss = atomic_load_explicit(&trace.busy_loss, memory_order_relaxed);
    ESP_LOGI("classic_trace", "v=1 attempt=%" PRIu32 " terminal=%" PRIu32
             " retained=%u overwritten=%" PRIu32 " overlap=%u peer_limit=%u busy_loss=%u"
             " samples=%u slot_omitted=%u slot_context_unavailable=%u",
             hci.attempt, hci.terminal, (unsigned)hci.count, hci.overwritten,
             (unsigned)hci.overlap, (unsigned)hci.peer_limit, busy_loss, samples, omitted, unavailable);
    unsigned oldest = (hci.next + RECORDS - hci.count) % RECORDS;
    for (unsigned i = 0; i < hci.count;) {
        char *out = hci.line;
        for (unsigned j = 0; j < GROUP && i < hci.count; ++j, ++i) {
            out = hex_record(out, &hci.records[(oldest + i) % RECORDS], sizeof(hci_record_t));
        }
        ESP_LOGI("classic_trace", "hci=%s", hci.line);
    }
    for (unsigned i = 0; i < samples;) {
        char *out = hci.line;
        for (unsigned j = 0; j < GROUP && i < samples; ++j, ++i) {
            out = hex_record(out, &trace.samples[i], sizeof(olm_sample_t));
        }
        ESP_LOGI("classic_trace", "olm=%s", hci.line);
    }
    memset(hci.peers, 0, sizeof(hci.peers));
}
