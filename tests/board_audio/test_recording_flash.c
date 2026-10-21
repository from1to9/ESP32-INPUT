// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/main/board_recording_flash.c"

#define PARTITION_BYTES 0xd0000U
static uint8_t memory[PARTITION_BYTES];
static esp_flash_t chip;
static esp_partition_t partition;
static TaskFunction_t task_function;
static unsigned lock_depth, calls, reads, programs, erases, current_task;
static unsigned fail_program, fail_read;
static bool partition_missing, task_failure, discard_program, discard_erase;
static bool partial_program;
static board_recording_flash_port_t port;
static const uint8_t volume_id[AE_ID_BYTES] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static unsigned notifications;
static void completion_notice(void *context)
{
    assert(context == &notifications && !lock_depth && current_task == 1);
    board_recording_flash_status_t status = board_recording_flash_status();
    assert(status.state != BOARD_RECORDING_FLASH_CLOSED && (f.slot == FLASH_DONE || f.request.automatic));
    ++notifications;
}

void audio_test_enter(portMUX_TYPE *lock) { (void)lock; assert(lock_depth++ == 0); }
void audio_test_leave(portMUX_TYPE *lock) { (void)lock; assert(lock_depth-- == 1); }
static void sdk(void) { assert(lock_depth == 0 && current_task == 1); ++calls; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task ? (void *)&chip : (void *)&partition; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *argument, unsigned priority, TaskHandle_t *task)
{
    assert(!lock_depth && function && name && stack == 4096 && !argument && priority == 3 && task);
    if (task_failure) return 0;
    task_function = function; *task = &chip; return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == &chip && !lock_depth); }
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear; (void)wait; assert(false); return 0; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    sdk(); assert(type == ESP_PARTITION_TYPE_DATA && subtype == 0x40 && !strcmp(label, "recordings"));
    return partition_missing ? NULL : &partition;
}
esp_err_t esp_flash_get_size(esp_flash_t *flash, uint32_t *size)
{
    sdk(); assert(flash == &chip); *size = 16U * 1024U * 1024U; return ESP_OK;
}
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *output, size_t bytes)
{
    sdk(); assert(part == &partition && offset <= sizeof(memory) && bytes <= sizeof(memory) - offset);
    ++reads;
    if (fail_read == reads) { if (bytes) memset(output, 0x5a, bytes / 2); return ESP_FAIL; }
    memcpy(output, memory + offset, bytes); return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset, const void *input, size_t bytes)
{
    sdk(); assert(part == &partition && offset <= sizeof(memory) && bytes <= sizeof(memory) - offset);
    ++programs;
    const uint8_t *data = input;
    if (fail_program == programs) {
        if (partial_program) for (size_t i = 0; i < bytes / 2; ++i) memory[offset + i] &= data[i];
        return ESP_FAIL;
    }
    if (!discard_program) for (size_t i = 0; i < bytes; ++i) memory[offset + i] &= data[i];
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t bytes)
{
    sdk(); assert(part == &partition && offset >= 4096 && offset % 4096 == 0 && bytes == 4096);
    assert(offset <= sizeof(memory) && bytes <= sizeof(memory) - offset);
    ++erases; if (!discard_erase) memset(memory + offset, 255, bytes); return ESP_OK;
}
static void drain(void)
{
    current_task = 1;
    unsigned count = 0;
    while (flash_service()) assert(++count < 3);
    current_task = 0;
}
static void reset(void)
{
    memset(&f, 0, sizeof(f));
    f.status.result = AE_UNAVAILABLE;
    memset(memory, 255, sizeof(memory));
    partition = (esp_partition_t){.flash_chip = &chip, .type = ESP_PARTITION_TYPE_DATA, .subtype = 0x40,
        .address = 0xf30000, .size = sizeof(memory), .erase_size = 4096, .label = "recordings"};
    calls = reads = programs = erases = current_task = fail_program = fail_read = 0;
    partition_missing = task_failure = discard_program = discard_erase = partial_program = false;
    task_function = NULL; port = board_recording_flash_port();
}
static void boot(void)
{
    unsigned before = calls;
    assert(board_recording_flash_init() == AE_OK && task_function && calls == before);
    assert(board_recording_flash_init() == AE_OK && calls == before);
    drain();
}
static ae_result_t reopen(void)
{
    unsigned before = calls;
    assert(port.open(NULL) == AE_AGAIN && calls == before); drain();
    return port.open(NULL);
}
static ae_result_t make_volume(void)
{
    unsigned before = calls;
    assert(port.provision(NULL, volume_id) == AE_AGAIN && calls == before); drain();
    return port.provision(NULL, volume_id);
}

static void persistence_and_bounds(void)
{
    reset(); boot();
    board_recording_flash_status_t status = board_recording_flash_status();
    assert(status.state == BOARD_RECORDING_FLASH_ERASED && status.geometry_verified && status.result == AE_PRIVACY);
    assert(!programs && !erases && status.partition_address == 0xf30000 && status.partition_bytes == PARTITION_BYTES);
    assert(status.capacity_bytes == PARTITION_BYTES - 4096);
    ae_storage_device_t device = board_recording_flash_device();
    assert(device.geometry_verified && !device.explicitly_owned && !device.key_protection_verified && !device.key && !device.nonce_range);
    uint8_t data[32]; memset(data, 0xa5, sizeof(data)); size_t actual;
    assert(port.program(NULL, 0, data, sizeof(data), &actual) == AE_AGAIN); drain();
    assert(port.program(NULL, 0, data, sizeof(data), &actual) == AE_PRIVACY && !actual && !programs);
    assert(make_volume() == AE_OK && programs == 2 && !erases);
    status = board_recording_flash_status();
    assert(status.state == BOARD_RECORDING_FLASH_OWNED && !memcmp(status.volume_id, volume_id, 16));
    uint8_t ownership[64]; memcpy(ownership, memory, sizeof(ownership));
    device = board_recording_flash_device();
    assert(device.explicitly_owned && device.region_bytes == sizeof(memory) - 4096 && device.erase_bytes == 4096);
    assert(device.storage == AE_STORAGE_RECORDS_PARTITION && !device.key_protection_verified);
    assert(device.program(NULL, UINT64_MAX, data, sizeof(data)) == AE_INVALID);
    assert(device.erase(NULL, 0, 8192) == AE_INVALID);
    unsigned before = calls;
    assert(device.program(NULL, 7, data, sizeof(data)) == AE_AGAIN && calls == before);
    assert(device.program(NULL, 7, data, sizeof(data)) == AE_AGAIN && calls == before);
    uint8_t different[32] = {0};
    assert(device.program(NULL, 7, different, sizeof(different)) == AE_BUSY);
    drain(); assert(device.program(NULL, 7, data, sizeof(data)) == AE_OK);
    assert(!memcmp(memory + 4096 + 7, data, sizeof(data)));
    uint8_t output[32] = {0};
    assert(device.read(NULL, 7, output, sizeof(output)) == AE_AGAIN); drain();
    assert(device.read(NULL, 7, output, sizeof(output)) == AE_OK && !memcmp(data, output, sizeof(data)));
    assert(port.program(NULL, status.capacity_bytes - 1, data, 2, &actual) == AE_AGAIN); drain();
    assert(port.program(NULL, status.capacity_bytes - 1, data, 2, &actual) == AE_INVALID && actual == 0);
    assert(port.erase(NULL, 1) == AE_AGAIN); drain(); assert(port.erase(NULL, 1) == AE_INVALID);
    assert(port.sync(NULL) == AE_AGAIN); drain(); assert(port.sync(NULL) == AE_OK);
    assert(reopen() == AE_OK && !memcmp(memory, ownership, sizeof(ownership)));
    before = programs; assert(make_volume() == AE_OK && programs == before && !erases);
    assert(device.erase(NULL, 0, 4096) == AE_AGAIN); drain(); assert(device.erase(NULL, 0, 4096) == AE_OK);
    assert(erases == 1 && filled(memory + 4096, 4096, 255) && !memcmp(memory, ownership, sizeof(ownership)));
    /* SDK calls are restricted to the private worker, not the manager task. */
    assert(open_volume(NULL) == AE_BUSY);
}

static void unknown_and_faults(void)
{
    reset(); memory[PARTITION_BYTES - 1] = 0; boot();
    assert(board_recording_flash_status().state == BOARD_RECORDING_FLASH_UNKNOWN);
    assert(make_volume() == AE_DENIED && !programs && !erases && memory[PARTITION_BYTES - 1] == 0);
    reset(); partition.address = UINT32_MAX - 4095; boot();
    assert(board_recording_flash_status().result == AE_INTEGRITY && !programs && !reads);
    reset(); partition.readonly = true; boot(); assert(board_recording_flash_status().result == AE_INTEGRITY && !reads);
    reset(); partition.encrypted = true; boot(); assert(board_recording_flash_status().result == AE_NOT_SUPPORTED && !reads);
    reset(); partition_missing = true; boot(); assert(board_recording_flash_status().result == AE_UNAVAILABLE && !reads);
    reset(); task_failure = true; assert(board_recording_flash_init() == AE_EXHAUSTED && !calls);
    reset(); boot(); fail_program = 1; partial_program = true;
    assert(make_volume() == AE_UNCERTAIN && board_recording_flash_status().state == BOARD_RECORDING_FLASH_FAULT);
    assert(reopen() == AE_INTEGRITY && !erases);
    unsigned before = programs; assert(make_volume() == AE_DENIED && programs == before);
    reset(); boot(); fail_program = 2;
    assert(make_volume() == AE_UNCERTAIN); assert(reopen() == AE_INTEGRITY && !erases);
    reset(); boot(); assert(make_volume() == AE_OK);
    uint8_t data[16]; memset(data, 0xa5, sizeof(data)); size_t actual;
    discard_program = true;
    assert(port.program(NULL, 0, data, sizeof(data), &actual) == AE_AGAIN); drain();
    assert(port.program(NULL, 0, data, sizeof(data), &actual) == AE_UNCERTAIN && actual == 0);
    assert(board_recording_flash_status().state == BOARD_RECORDING_FLASH_FAULT);
    discard_program = false; assert(reopen() == AE_OK);
    assert(port.program(NULL, 0, data, sizeof(data), &actual) == AE_AGAIN); drain();
    assert(port.program(NULL, 0, data, sizeof(data), &actual) == AE_OK && actual == sizeof(data));
    discard_erase = true;
    assert(port.erase(NULL, 0) == AE_AGAIN); drain(); assert(port.erase(NULL, 0) == AE_UNCERTAIN);
    discard_erase = false; assert(reopen() == AE_OK);
    uint8_t output[16]; memset(output, 0x33, sizeof(output)); fail_read = reads + 1;
    assert(port.read(NULL, 0, output, sizeof(output), &actual) == AE_AGAIN); drain();
    assert(port.read(NULL, 0, output, sizeof(output), &actual) == AE_IO_ERROR && actual == 0);
    assert(filled(output, sizeof(output), 0x33));
    reset(); boot(); assert(make_volume() == AE_OK);
    memory[20] ^= 1;
    assert(reopen() == AE_INTEGRITY && !erases);
}

static void completion_wakeup_after_actual_publish(void)
{
    reset(); notifications = 0;
    ae_storage_device_t device = board_recording_flash_device(); assert(device.completion_notify);
    device.completion_notify(device.context, completion_notice, &notifications);
    assert(!notifications); boot(); assert(notifications == 1);
    assert(port.provision(NULL, volume_id) == AE_AGAIN && notifications == 1);
    drain(); assert(notifications == 2 && port.provision(NULL, volume_id) == AE_OK && notifications == 2);
    uint8_t data[4] = {1,2,3,4};
    assert(device.program(NULL, 0, data, sizeof(data)) == AE_AGAIN && notifications == 2);
    drain(); assert(notifications == 3 && device.program(NULL, 0, data, sizeof(data)) == AE_OK);
    fail_read = reads + 1;
    assert(device.read(NULL, 0, data, sizeof(data)) == AE_AGAIN); drain();
    assert(notifications == 4 && device.read(NULL, 0, data, sizeof(data)) == AE_IO_ERROR);
    device.completion_notify(NULL, NULL, NULL);
    assert(port.open(NULL) == AE_AGAIN); drain(); assert(notifications == 4);
}

int main(void)
{
    reset(); assert(board_recording_flash_status().result == AE_UNAVAILABLE);
    persistence_and_bounds(); unknown_and_faults();
    completion_wakeup_after_actual_publish();
    assert(!lock_depth);
    puts("Recording flash: explicit ownership, nonblocking copied IO, persistence, bounds, readback, interrupted provisioning and preservation passed.");
    return 0;
}
