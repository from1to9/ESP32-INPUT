#!/usr/bin/env python3
"""Exercise the bounded HCI and OLM observer at its host and controller seams."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    'sdkconfig.h': '#define CONFIG_BT_CTRL_PINNED_TO_CORE 0\n',
    'esp_attr.h': '#define EXT_RAM_BSS_ATTR\n#define DRAM_ATTR\n',
    'esp_cpu.h': 'int esp_cpu_get_core_id(void);\n',
    'esp_log.h': 'void ESP_LOGI(const char *, const char *, ...);\n',
    'esp_timer.h': '#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
    'freertos/FreeRTOS.h': '#include <stdbool.h>\nbool xPortInIsrContext(void);\n',
}

FIXTURE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "bt_classic_connect_diag.c"

static const uint8_t create[] = {1,5,4,13,1,2,3,4,5,6,0,0,0,0,0,0,0};
static const uint8_t denied[] = {4,0x0f,4,9,1,5,4};
static const uint8_t accepted[] = {4,0x0f,4,0,1,5,4};
static const uint8_t up[] = {4,3,11,0,1,8,1,2,3,4,5,6,1,0};
static const uint8_t request[] = {4,4,10,1,2,3,4,5,6,0,0,0,1};
static const uint8_t down[] = {4,5,4,0,1,8,0x13};
static const uint8_t scan[] = {1,0x1a,0x0c,1,2};

static uint8_t cfg[] = {0,0,1}, entry[56], real_result;
static uint16_t slot_state;
static unsigned logs, real_calls, end_calls, slot_reads, clock_reads;
static int64_t clock_us;
static bool isr;
static int core;

int esp_cpu_get_core_id(void) { return core; }
bool xPortInIsrContext(void) { return isr; }
int64_t esp_timer_get_time(void) { ++clock_reads; return clock_us; }
const uint8_t *r_olm_cfg_get(void) { ++slot_reads; return cfg; }
const uint8_t *r_olm_get_conn_info(uint8_t slot) { assert(slot == 0); return entry; }
uint16_t r_olm_conn_get_state(uint8_t slot) { assert(slot == 0); return slot_state; }
uint8_t __real_r_olm_conn_alloc(const uint8_t *addr, uint8_t *link)
{
    assert(addr == create + 4 && link);
    ++real_calls;
    if (!real_result) *link = 0;
    return real_result;
}
void __real_r_olm_pscan_end(uint8_t link, const void *event)
{
    assert(!link && event == entry);
    ++end_calls;
    entry[32] = 0;
}

void ESP_LOGI(const char *tag, const char *fmt, ...)
{
    assert(!strcmp(tag, "classic_trace"));
    char line[256];
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(line));
    ++logs;
}

static void failure(void)
{
    uint8_t handle = 255;
    real_result = 9;
    assert(__wrap_r_olm_conn_alloc(create + 4, &handle) == 9 && slot_reads == 0);
    bt_classic_connect_diag_tx(scan, sizeof(scan));
    bt_classic_connect_diag_tx(create, sizeof(create));
    assert(atomic_load(&trace.armed) && hci.attempt == 2);
    entry[32] = 1;
    slot_state = 4;
    assert(__wrap_r_olm_conn_alloc(create + 4, &handle) == 9 && handle == 255 && real_calls == 2);
    assert(atomic_load(&trace.published) == 1);
    const olm_sample_t *sample = &trace.samples[0];
    assert(sample->capacity == 1 && sample->before_bits == 1 && sample->before_state == 4);
    assert(sample->after_bits == 1 && sample->after_state == 4 && sample->relation == ADDRESSLESS);
    bt_classic_connect_diag_rx(denied, sizeof(denied));
    assert(hci.terminal == 3 && !hci.overlap);
    slot_state = 5;
    __wrap_r_olm_pscan_end(0, entry);
    assert(end_calls == 1 && atomic_load(&trace.published) == 2);
    assert(trace.samples[1].before_state == 5 && trace.samples[1].after_bits == 0);
    bt_classic_connect_diag_rx(request, sizeof(request));
    bt_classic_connect_diag_rx(up, sizeof(up));
    assert(hci.records[4].peer == hci.attempt_peer && hci.records[4].handle == 0x801);
    bt_classic_connect_diag_poll(1999);
    assert(!logs);
    bt_classic_connect_diag_poll(2000);
    assert(logs == 3 && hci.printed && atomic_load(&trace.state) == FROZEN);
    bt_classic_connect_diag_rx(down, sizeof(down));
    bt_classic_connect_diag_poll(3000);
    assert(hci.sequence == 5 && logs == 3);
}

static void loss(void)
{
    for (unsigned i = 0; i < RECORDS + 5; ++i) bt_classic_connect_diag_tx(scan, sizeof(scan));
    assert(hci.count == RECORDS && hci.overwritten == 5);
    bt_classic_connect_diag_tx(create, sizeof(create));
    entry[32] = 3;
    memcpy(entry + 18, create + 4, 6);
    slot_state = 6;
    for (unsigned i = 0; i < SAMPLES + 2; ++i) __wrap_r_olm_pscan_end(0, entry);
    assert(atomic_load(&trace.published) == SAMPLES - 1);
    uint8_t id = 255;
    isr = true;
    real_result = 9;
    unsigned reads = slot_reads;
    assert(__wrap_r_olm_conn_alloc(create + 4, &id) == 9 && slot_reads == reads);
    assert(atomic_load(&trace.unavailable));
    isr = false;
    entry[32] = 3;
    assert(__wrap_r_olm_conn_alloc(create + 4, &id) == 9);
    assert(atomic_load(&trace.published) == SAMPLES);
    assert(trace.samples[SAMPLES - 1].relation == SAME_PEER);
    bt_classic_connect_diag_rx(denied, sizeof(denied));
    uint32_t terminal = hci.terminal;
    for (unsigned i = 0; i < POST_RECORDS + 3; ++i) bt_classic_connect_diag_rx(request, sizeof(request));
    assert(hci.sequence == terminal + POST_RECORDS && atomic_load(&trace.state) == FROZEN);
    assert(!atomic_load(&trace.armed) && atomic_load(&trace.omitted) == 3);
    bt_classic_connect_diag_poll(0);
    assert(logs == 13);
}

static void filter(void)
{
    const uint8_t acl[] = {2,1,8,0,0};
    const uint8_t key[] = {4,0x3e,2,5,0};
    const uint8_t malformed[] = {4,3,10,0,1,8,1,2,3,4,5,6,1};
    bt_classic_connect_diag_rx(acl, sizeof(acl));
    bt_classic_connect_diag_rx(key, sizeof(key));
    bt_classic_connect_diag_rx(malformed, sizeof(malformed));
    bt_classic_connect_diag_tx(create, sizeof(create) - 1);
    assert(clock_reads == 0 && hci.sequence == 0 && !atomic_load(&trace.armed));
    bt_classic_connect_diag_tx(create, sizeof(create));
    bt_classic_connect_diag_rx(accepted, sizeof(accepted));
    assert(hci.create_accepted && !hci.terminal);
    uint8_t other[sizeof(up)];
    memcpy(other, up, sizeof(other));
    other[6] = 9;
    bt_classic_connect_diag_rx(other, sizeof(other));
    assert(!hci.terminal);
    bt_classic_connect_diag_rx(up, sizeof(up));
    assert(hci.terminal == 4);
}

static void timeout(void)
{
    bt_classic_connect_diag_tx(create, sizeof(create));
    bt_classic_connect_diag_poll(19999);
    assert(!logs);
    clock_us = 20000000;
    bt_classic_connect_diag_rx(denied, sizeof(denied));
    assert(hci.sequence == 1 && !hci.terminal && atomic_load(&trace.state) == FROZEN);
    bt_classic_connect_diag_poll(20000);
    assert(logs == 2);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "failure")) failure();
    else if (!strcmp(argv[1], "loss")) loss();
    else if (!strcmp(argv[1], "filter")) filter();
    else if (!strcmp(argv[1], "timeout")) timeout();
    else assert(false);
    puts(argv[1]);
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        for name, content in STUBS.items():
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (tmp / 'fixture.c').write_text(FIXTURE)
        compiler = shutil.which('clang') or 'cc'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(tmp), '-I',
                        str(ROOT / 'firmware/main'), str(tmp / 'fixture.c'),
                        '-o', str(tmp / 'fixture')], check=True)
        for case in ('failure', 'loss', 'filter', 'timeout'):
            subprocess.run([str(tmp / 'fixture'), case], check=True)


if __name__ == '__main__':
    main()
