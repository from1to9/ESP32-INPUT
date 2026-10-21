// SPDX-License-Identifier: Apache-2.0
#include "board_recording_flash.h"

#include <string.h>

#include "esp_flash.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HEADER_BYTES 64U
#define BODY_BYTES 60U
#define COMMIT UINT32_C(0x314d4f43)

typedef enum { FLASH_OPEN, FLASH_PROVISION, FLASH_READ, FLASH_PROGRAM, FLASH_ERASE, FLASH_SYNC } flash_operation_t;
typedef enum { FLASH_EMPTY, FLASH_PENDING, FLASH_RUNNING, FLASH_DONE } flash_slot_t;
typedef struct {
    flash_operation_t operation;
    uint32_t offset;
    size_t bytes;
    uint8_t volume_id[AE_ID_BYTES];
    bool automatic;
} flash_request_t;

static const uint8_t magic[16] = {'M','O','S','A','I','C','O','-','R','E','C','O','R','D','1',0};
static struct {
    portMUX_TYPE lock;
    TaskHandle_t owner;
    const esp_partition_t *partition;
    board_recording_flash_status_t status;
    uint8_t scratch[BOARD_RECORDING_FLASH_IO_MAX];
    uint8_t header[HEADER_BYTES];
    TaskHandle_t task;
    bool worker_started;
    flash_slot_t slot;
    flash_request_t request;
    uint8_t payload[BOARD_RECORDING_FLASH_IO_MAX];
    ae_result_t result;
    size_t actual;
    void (*completion_notify)(void *);
    void *completion_context;
} f = {.lock = portMUX_INITIALIZER_UNLOCKED,
    .status = {.state = BOARD_RECORDING_FLASH_CLOSED, .result = AE_UNAVAILABLE}};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8U * i));
}
static uint32_t crc32(const uint8_t *data, size_t bytes)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}
static bool filled(const uint8_t *data, size_t bytes, uint8_t value)
{
    for (size_t i = 0; i < bytes; ++i) if (data[i] != value) return false;
    return true;
}
static bool valid_id(const uint8_t *id)
{
    return id && !filled(id, AE_ID_BYTES, 0) && !filled(id, AE_ID_BYTES, 255);
}
static bool worker_owner(void)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&f.lock);
    if (!f.owner) f.owner = caller;
    bool valid = caller && caller == f.owner;
    portEXIT_CRITICAL(&f.lock);
    return valid;
}
static ae_result_t publish(board_recording_flash_state_t state, ae_result_t result, esp_err_t error)
{
    portENTER_CRITICAL(&f.lock);
    f.status.state = state;
    f.status.result = result;
    f.status.sdk_error = error;
    portEXIT_CRITICAL(&f.lock);
    return result;
}
static ae_result_t fault(esp_err_t error, bool mutation)
{
    return publish(BOARD_RECORDING_FLASH_FAULT, mutation ? AE_UNCERTAIN : AE_IO_ERROR, error);
}
static bool valid_header(const uint8_t *header)
{
    return !memcmp(header, magic, sizeof(magic)) && get32(header + 16) == 1 &&
        get32(header + 20) == f.partition->address &&
        get32(header + 24) == f.partition->size &&
        get32(header + 28) == BOARD_RECORDING_FLASH_ERASE_BYTES && valid_id(header + 32) &&
        get32(header + 48) == BOARD_RECORDING_FLASH_ERASE_BYTES &&
        get32(header + 52) == crc32(header, 52) && get32(header + 56) == 0;
}
static ae_result_t scan_erased(uint32_t start)
{
    for (uint32_t offset = start; offset < f.partition->size; offset += sizeof(f.scratch)) {
        esp_err_t error = esp_partition_read(f.partition, offset, f.scratch, sizeof(f.scratch));
        if (error != ESP_OK) return fault(error, false);
        if (!filled(f.scratch, sizeof(f.scratch), 255)) return AE_DENIED;
    }
    return AE_OK;
}

static ae_result_t open_volume(void *context)
{
    (void)context;
    if (!worker_owner()) return AE_BUSY;
    f.partition = NULL;
    portENTER_CRITICAL(&f.lock);
    f.status = (board_recording_flash_status_t){.state = BOARD_RECORDING_FLASH_CLOSED, .result = AE_UNAVAILABLE};
    portEXIT_CRITICAL(&f.lock);
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        BOARD_RECORDING_FLASH_SUBTYPE, BOARD_RECORDING_FLASH_LABEL);
    if (!partition) return AE_UNAVAILABLE;
    /* The application store encrypts its own pages. Transparent partition
     * encryption changes erased-byte reads and requires a different format. */
    if (partition->encrypted) return publish(BOARD_RECORDING_FLASH_UNKNOWN, AE_NOT_SUPPORTED, ESP_OK);
    uint32_t flash_bytes = 0;
    esp_err_t error = esp_flash_get_size(partition->flash_chip, &flash_bytes);
    if (error != ESP_OK) return fault(error, false);
    if (partition->type != ESP_PARTITION_TYPE_DATA || partition->subtype != BOARD_RECORDING_FLASH_SUBTYPE ||
        strcmp(partition->label, BOARD_RECORDING_FLASH_LABEL) || partition->readonly ||
        partition->size < 2U * BOARD_RECORDING_FLASH_ERASE_BYTES ||
        partition->size % BOARD_RECORDING_FLASH_ERASE_BYTES || partition->address % BOARD_RECORDING_FLASH_ERASE_BYTES ||
        partition->erase_size != BOARD_RECORDING_FLASH_ERASE_BYTES ||
        partition->address > flash_bytes || partition->size > flash_bytes - partition->address) {
        return publish(BOARD_RECORDING_FLASH_UNKNOWN, AE_INTEGRITY, ESP_OK);
    }
    f.partition = partition;
    portENTER_CRITICAL(&f.lock);
    f.status.geometry_verified = true;
    f.status.partition_address = partition->address;
    f.status.partition_bytes = partition->size;
    f.status.capacity_bytes = partition->size - BOARD_RECORDING_FLASH_ERASE_BYTES;
    f.status.erase_bytes = BOARD_RECORDING_FLASH_ERASE_BYTES;
    f.status.write_alignment = partition->encrypted ? 16 : 1;
    portEXIT_CRITICAL(&f.lock);
    error = esp_partition_read(partition, 0, f.header, sizeof(f.header));
    if (error != ESP_OK) return fault(error, false);
    if (filled(f.header, sizeof(f.header), 255)) {
        ae_result_t result = scan_erased(0);
        if (result == AE_IO_ERROR) return result;
        return publish(result == AE_OK ? BOARD_RECORDING_FLASH_ERASED : BOARD_RECORDING_FLASH_UNKNOWN,
            result == AE_OK ? AE_PRIVACY : AE_DENIED, ESP_OK);
    }
    if (!valid_header(f.header)) return publish(BOARD_RECORDING_FLASH_UNKNOWN, AE_INTEGRITY, ESP_OK);
    uint32_t commit = get32(f.header + BODY_BYTES);
    if (commit != COMMIT && commit != UINT32_MAX)
        return publish(BOARD_RECORDING_FLASH_UNKNOWN, AE_INTEGRITY, ESP_OK);
    portENTER_CRITICAL(&f.lock);
    memcpy(f.status.volume_id, f.header + 32, AE_ID_BYTES);
    portEXIT_CRITICAL(&f.lock);
    return publish(commit == COMMIT ? BOARD_RECORDING_FLASH_OWNED : BOARD_RECORDING_FLASH_PROVISIONING,
        commit == COMMIT ? AE_OK : AE_PRIVACY, ESP_OK);
}

static ae_result_t provision(void *context, const uint8_t volume_id[AE_ID_BYTES])
{
    (void)context;
    if (!worker_owner()) return AE_BUSY;
    if (!valid_id(volume_id)) return AE_INVALID;
    board_recording_flash_status_t status = board_recording_flash_status();
    if (!f.partition || !status.geometry_verified) return AE_UNAVAILABLE;
    if (status.state == BOARD_RECORDING_FLASH_OWNED)
        return !memcmp(status.volume_id, volume_id, AE_ID_BYTES) ? AE_OK : AE_DENIED;
    if (status.state != BOARD_RECORDING_FLASH_ERASED && status.state != BOARD_RECORDING_FLASH_PROVISIONING) return AE_DENIED;
    bool completing = status.state == BOARD_RECORDING_FLASH_PROVISIONING;
    if (completing && memcmp(status.volume_id, volume_id, AE_ID_BYTES)) return AE_DENIED;
    ae_result_t result = scan_erased(completing ? BOARD_RECORDING_FLASH_ERASE_BYTES : 0);
    if (result != AE_OK) return result == AE_DENIED ? publish(BOARD_RECORDING_FLASH_UNKNOWN, result, ESP_OK) : result;
    if (!completing) {
        memset(f.header, 255, sizeof(f.header));
        memcpy(f.header, magic, sizeof(magic));
        put32(f.header + 16, 1); put32(f.header + 20, f.partition->address);
        put32(f.header + 24, f.partition->size); put32(f.header + 28, BOARD_RECORDING_FLASH_ERASE_BYTES);
        memcpy(f.header + 32, volume_id, AE_ID_BYTES);
        put32(f.header + 48, BOARD_RECORDING_FLASH_ERASE_BYTES); put32(f.header + 52, crc32(f.header, 52));
        put32(f.header + 56, 0);
        /* Program each encrypted 16-byte block once. Writing plaintext FF and
         * then overwriting its encrypted block is not a legal NOR update. */
        esp_err_t error = esp_partition_write(f.partition, 0, f.header, 48);
        if (error != ESP_OK) return fault(error, true);
        error = esp_partition_read(f.partition, 0, f.scratch, 48);
        if (error != ESP_OK || memcmp(f.header, f.scratch, 48)) return fault(error == ESP_OK ? ESP_FAIL : error, true);
    }
    put32(f.header + BODY_BYTES, COMMIT);
    esp_err_t error = esp_partition_write(f.partition, 48, f.header + 48, 16);
    if (error != ESP_OK) return fault(error, true);
    error = esp_partition_read(f.partition, 0, f.scratch, sizeof(f.header));
    if (error != ESP_OK || memcmp(f.header, f.scratch, sizeof(f.header))) return fault(error == ESP_OK ? ESP_FAIL : error, true);
    return open_volume(NULL);
}

static ae_result_t check_data(uint32_t offset, size_t bytes)
{
    if (!worker_owner()) return AE_BUSY;
    board_recording_flash_status_t status = board_recording_flash_status();
    if (!f.partition || status.state != BOARD_RECORDING_FLASH_OWNED) return AE_PRIVACY;
    if (!bytes || bytes > BOARD_RECORDING_FLASH_IO_MAX || offset > status.capacity_bytes || bytes > status.capacity_bytes - offset)
        return AE_INVALID;
    return AE_OK;
}

static ae_result_t read_data(void *context, uint32_t offset, void *data, size_t bytes, size_t *actual)
{
    (void)context;
    if (actual) *actual = 0;
    if (!data || !actual) return AE_INVALID;
    ae_result_t result = check_data(offset, bytes);
    if (result != AE_OK) return result;
    esp_err_t error = esp_partition_read(f.partition, BOARD_RECORDING_FLASH_ERASE_BYTES + offset, data, bytes);
    if (error != ESP_OK) return fault(error, false);
    *actual = bytes;
    return AE_OK;
}

static ae_result_t program_data(void *context, uint32_t offset, const void *data, size_t bytes, size_t *actual)
{
    (void)context;
    if (actual) *actual = 0;
    if (!data || !actual) return AE_INVALID;
    ae_result_t result = check_data(offset, bytes);
    if (result != AE_OK) return result;
    unsigned alignment = f.partition->encrypted ? 16U : 1U;
    if (offset % alignment || bytes % alignment) return AE_INVALID;
    uint32_t address = BOARD_RECORDING_FLASH_ERASE_BYTES + offset;
    esp_err_t error = esp_partition_write(f.partition, address, data, bytes);
    if (error != ESP_OK) return fault(error, true);
    error = esp_partition_read(f.partition, address, f.scratch, bytes);
    if (error != ESP_OK || memcmp(data, f.scratch, bytes)) return fault(error == ESP_OK ? ESP_FAIL : error, true);
    *actual = bytes;
    return AE_OK;
}

static ae_result_t erase_data(void *context, uint32_t offset)
{
    (void)context;
    ae_result_t result = check_data(offset, BOARD_RECORDING_FLASH_ERASE_BYTES);
    if (result != AE_OK) return result;
    if (offset % BOARD_RECORDING_FLASH_ERASE_BYTES) return AE_INVALID;
    esp_err_t error = esp_partition_erase_range(f.partition, BOARD_RECORDING_FLASH_ERASE_BYTES + offset,
        BOARD_RECORDING_FLASH_ERASE_BYTES);
    if (error != ESP_OK) return fault(error, true);
    error = esp_partition_read(f.partition, BOARD_RECORDING_FLASH_ERASE_BYTES + offset, f.scratch, sizeof(f.scratch));
    if (error != ESP_OK || !filled(f.scratch, sizeof(f.scratch), 255)) return fault(error == ESP_OK ? ESP_FAIL : error, true);
    return AE_OK;
}

static ae_result_t sync_data(void *context)
{
    (void)context;
    ae_result_t result = check_data(0, 1);
    if (result != AE_OK) return result;
    /* ESP partition program/erase calls are synchronous, and every mutation
     * above has already passed a readback. Recheck the committed identity as
     * the barrier; there is no buffered filesystem or invented fsync result. */
    esp_err_t error = esp_partition_read(f.partition, 0, f.scratch, HEADER_BYTES);
    if (error != ESP_OK) return fault(error, false);
    board_recording_flash_status_t status = board_recording_flash_status();
    if (!valid_header(f.scratch) || get32(f.scratch + BODY_BYTES) != COMMIT ||
        memcmp(f.scratch + 32, status.volume_id, AE_ID_BYTES)) return fault(ESP_FAIL, false);
    return AE_OK;
}

static bool flash_service(void)
{
    portENTER_CRITICAL(&f.lock);
    if (f.slot != FLASH_PENDING) { portEXIT_CRITICAL(&f.lock); return false; }
    flash_request_t request = f.request;
    f.slot = FLASH_RUNNING;
    portEXIT_CRITICAL(&f.lock);
    size_t actual = 0;
    ae_result_t result;
    switch (request.operation) {
    case FLASH_OPEN: result = open_volume(NULL); break;
    case FLASH_PROVISION: result = provision(NULL, request.volume_id); break;
    case FLASH_READ: result = read_data(NULL, request.offset, f.payload, request.bytes, &actual); break;
    case FLASH_PROGRAM: result = program_data(NULL, request.offset, f.payload, request.bytes, &actual); break;
    case FLASH_ERASE: result = erase_data(NULL, request.offset); break;
    case FLASH_SYNC: result = sync_data(NULL); break;
    default: result = AE_INVALID; break;
    }
    portENTER_CRITICAL(&f.lock);
    f.result = result;
    f.actual = actual;
    f.slot = request.automatic ? FLASH_EMPTY : FLASH_DONE;
    void (*notify)(void *) = f.completion_notify;
    void *notify_context = f.completion_context;
    portEXIT_CRITICAL(&f.lock);
    if (notify) notify(notify_context);
    return true;
}

static void flash_worker(void *unused)
{
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (flash_service()) { }
    }
}

static ae_result_t exchange(const flash_request_t *request, void *data, size_t *actual)
{
    if (actual) *actual = 0;
    portENTER_CRITICAL(&f.lock);
    if (!f.worker_started) { portEXIT_CRITICAL(&f.lock); return AE_UNAVAILABLE; }
    if (f.slot != FLASH_EMPTY) {
        if (f.request.operation != request->operation || f.request.offset != request->offset ||
            f.request.bytes != request->bytes || memcmp(f.request.volume_id, request->volume_id, AE_ID_BYTES) ||
            (request->operation == FLASH_PROGRAM && memcmp(data, f.payload, request->bytes))) {
            portEXIT_CRITICAL(&f.lock); return AE_BUSY;
        }
        if (f.slot != FLASH_DONE) { portEXIT_CRITICAL(&f.lock); return AE_AGAIN; }
        ae_result_t result = f.result;
        if (actual) *actual = f.actual;
        if (request->operation == FLASH_READ && result == AE_OK) memcpy(data, f.payload, f.actual);
        memset(f.payload, 0, sizeof(f.payload));
        f.slot = FLASH_EMPTY;
        portEXIT_CRITICAL(&f.lock);
        return result;
    }
    f.request = *request;
    if (request->operation == FLASH_PROGRAM) memcpy(f.payload, data, request->bytes);
    f.slot = FLASH_PENDING;
    portEXIT_CRITICAL(&f.lock);
    xTaskNotifyGive(f.task);
    return AE_AGAIN;
}

static ae_result_t request_open(void *context)
{
    (void)context;
    flash_request_t request = {.operation = FLASH_OPEN};
    return exchange(&request, NULL, NULL);
}

static ae_result_t request_provision(void *context, const uint8_t volume_id[AE_ID_BYTES])
{
    (void)context;
    if (!valid_id(volume_id)) return AE_INVALID;
    flash_request_t request = {.operation = FLASH_PROVISION};
    memcpy(request.volume_id, volume_id, AE_ID_BYTES);
    return exchange(&request, NULL, NULL);
}

static ae_result_t request_read(void *context, uint32_t offset, void *data, size_t bytes, size_t *actual)
{
    (void)context;
    if (actual) *actual = 0;
    if (!data || !actual || !bytes || bytes > BOARD_RECORDING_FLASH_IO_MAX) return AE_INVALID;
    flash_request_t request = {.operation = FLASH_READ, .offset = offset, .bytes = bytes};
    return exchange(&request, data, actual);
}

static ae_result_t request_program(void *context, uint32_t offset, const void *data, size_t bytes, size_t *actual)
{
    (void)context;
    if (actual) *actual = 0;
    if (!data || !actual || !bytes || bytes > BOARD_RECORDING_FLASH_IO_MAX) return AE_INVALID;
    flash_request_t request = {.operation = FLASH_PROGRAM, .offset = offset, .bytes = bytes};
    return exchange(&request, (void *)data, actual);
}

static ae_result_t request_erase(void *context, uint32_t offset)
{
    (void)context;
    flash_request_t request = {.operation = FLASH_ERASE, .offset = offset};
    return exchange(&request, NULL, NULL);
}

static ae_result_t request_sync(void *context)
{
    (void)context;
    flash_request_t request = {.operation = FLASH_SYNC};
    return exchange(&request, NULL, NULL);
}

ae_result_t board_recording_flash_init(void)
{
    if (f.worker_started) return AE_OK; /* Boot owner calls init once. */
    if (xTaskCreate(flash_worker, "record_flash", 4096, NULL, 3, &f.task) != pdPASS) return AE_EXHAUSTED;
    portENTER_CRITICAL(&f.lock);
    f.request = (flash_request_t){.operation = FLASH_OPEN, .automatic = true};
    f.slot = FLASH_PENDING;
    f.worker_started = true;
    portEXIT_CRITICAL(&f.lock);
    xTaskNotifyGive(f.task);
    return AE_OK;
}

board_recording_flash_port_t board_recording_flash_port(void)
{
    return (board_recording_flash_port_t){.open = request_open, .provision = request_provision,
        .read = request_read, .program = request_program, .erase = request_erase, .sync = request_sync};
}

static ae_result_t device_read(void *context, uint64_t offset, void *data, size_t bytes)
{
    if (offset > UINT32_MAX) return AE_INVALID;
    size_t actual = 0;
    ae_result_t result = request_read(context, (uint32_t)offset, data, bytes, &actual);
    return result == AE_OK && actual != bytes ? AE_INTEGRITY : result;
}
static ae_result_t device_program(void *context, uint64_t offset, const void *data, size_t bytes)
{
    if (offset > UINT32_MAX) return AE_INVALID;
    size_t actual = 0;
    ae_result_t result = request_program(context, (uint32_t)offset, data, bytes, &actual);
    return result == AE_OK && actual != bytes ? AE_INTEGRITY : result;
}
static ae_result_t device_erase(void *context, uint64_t offset, size_t bytes)
{
    if (offset > UINT32_MAX || bytes != BOARD_RECORDING_FLASH_ERASE_BYTES) return AE_INVALID;
    return request_erase(context, (uint32_t)offset);
}

static void device_completion_notify(void *context, void (*notify)(void *), void *notify_context)
{
    (void)context;
    portENTER_CRITICAL(&f.lock);
    f.completion_notify = notify; f.completion_context = notify_context;
    portEXIT_CRITICAL(&f.lock);
}

ae_storage_device_t board_recording_flash_device(void)
{
    board_recording_flash_status_t status = board_recording_flash_status();
    return (ae_storage_device_t){.storage = AE_STORAGE_RECORDS_PARTITION, .region_bytes = status.capacity_bytes,
        .erase_bytes = status.erase_bytes, .geometry_verified = status.geometry_verified,
        .explicitly_owned = status.state == BOARD_RECORDING_FLASH_OWNED,
        .read = device_read, .program = device_program, .erase = device_erase, .sync = request_sync,
        .completion_notify = device_completion_notify};
}

board_recording_flash_status_t board_recording_flash_status(void)
{
    portENTER_CRITICAL(&f.lock);
    board_recording_flash_status_t status = f.status;
    portEXIT_CRITICAL(&f.lock);
    return status;
}
