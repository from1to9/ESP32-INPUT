// SPDX-License-Identifier: Apache-2.0
#include "audio_store.h"
#define main store_fixture_main
#define service fixture_store_service
#include "test_store.c"
#undef main
#undef service
#include "../../firmware/main/app_recording_store.c"

static board_recording_flash_status_t physical;
static uint8_t nvs_value[64], nvs_staged[64];
static bool nvs_present, commit_uncertain;
static unsigned commits, provision_calls;
void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{ assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return calloc(n, size); }
void heap_caps_free(void *p) { free(p); }
int64_t esp_timer_get_time(void) { return 1000000; }
esp_err_t nvs_flash_init_partition(const char *name) { assert(!strcmp(name, "control")); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle)
{ assert(!strcmp(partition, "control") && !strcmp(name, "record_audio") && mode == NVS_READWRITE); *handle = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *name, void *value, size_t *bytes)
{ assert(handle == 1 && !strcmp(name, "custody_v1")); if (!nvs_present) return ESP_ERR_NVS_NOT_FOUND; assert(*bytes >= 64); memcpy(value, nvs_value, 64); *bytes = 64; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *name, const void *value, size_t bytes)
{ assert(handle == 1 && !strcmp(name, "custody_v1") && bytes == 64); memcpy(nvs_staged, value, 64); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle == 1); memcpy(nvs_value, nvs_staged, 64); nvs_present = true; ++commits; return commit_uncertain ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
ae_result_t board_recording_flash_init(void) { return AE_OK; }
static ae_result_t physical_open(void *context)
{ (void)context; return physical.state == BOARD_RECORDING_FLASH_OWNED ? AE_OK : AE_PRIVACY; }
static ae_result_t physical_provision(void *context, const uint8_t id_bytes[16])
{
    (void)context; ++provision_calls;
    if (physical.state == BOARD_RECORDING_FLASH_ERASED) {
        memcpy(physical.volume_id, id_bytes, 16); physical.state = BOARD_RECORDING_FLASH_PROVISIONING; return AE_AGAIN;
    }
    assert(!memcmp(physical.volume_id, id_bytes, 16)); physical.state = BOARD_RECORDING_FLASH_OWNED; return AE_OK;
}
board_recording_flash_port_t board_recording_flash_port(void)
{ return (board_recording_flash_port_t){.open = physical_open, .provision = physical_provision}; }
board_recording_flash_status_t board_recording_flash_status(void) { return physical; }
ae_storage_device_t board_recording_flash_device(void)
{ ae_storage_device_t d = device(); d.explicitly_owned = physical.state == BOARD_RECORDING_FLASH_OWNED; return d; }
static void tick(void) { (void)port.service(port.context); }
static void boot(void)
{
    if (state) {
        if (state->store) ae_store_deinit(state->store);
        free(state->store_memory); ae_secure_zero(state, sizeof(*state)); free(state); state = NULL;
    }
    app_recording_store_config_t config = {.wall_time = utc};
    assert(app_recording_store_init(&config) == AE_OK); port = app_recording_store_port();
}
#undef RETRY
#define RETRY(expression) do { ae_result_t result_ = AE_AGAIN; unsigned loops_ = 0; \
    while ((result_ = (expression)) == AE_AGAIN && loops_++ < 20000) tick(); \
    if (result_ != AE_OK) fprintf(stderr, "runtime failure line %d result %d\\n", __LINE__, result_); \
    assert(result_ == AE_OK); } while (0)
int main(void)
{
    memset(disk.bytes, 255, sizeof(disk.bytes)); disk.time = 100000; disk.async = true;
    physical = (board_recording_flash_status_t){.state = BOARD_RECORDING_FLASH_ERASED, .geometry_verified = true,
        .capacity_bytes = DISK_BYTES, .erase_bytes = 4096};
    boot(); for (unsigned i = 0; i < 50; ++i) tick();
    assert(!disk.writes && !disk.erases && !commits && !provision_calls);
    RETRY(port.provision(port.context, 8, 512U * 1024U));
    app_recording_store_snapshot_t view; assert(app_recording_store_snapshot(&view) && view.store.mounted);
    assert(commits == 2 && provision_calls == 2 && physical.state == BOARD_RECORDING_FLASH_OWNED);
    ae_record_metadata_t m = metadata(1), evidence;
    RETRY(port.reserve(port.context, &m));
    uint8_t frame[320]; memset(frame, 0x4a, sizeof(frame)); size_t actual;
    for (unsigned i = 0; i < 20; ++i) {
        RETRY(port.append(port.context, &m.object_id, m.pcm_bytes, frame, sizeof(frame), &actual));
        assert(actual == sizeof(frame)); m.pcm_bytes += actual;
    }
    RETRY(port.checkpoint(port.context, &m.object_id, &evidence)); m = evidence;
    m.state = AE_RECORD_COMPLETED; ++m.revision; uint8_t header[44]; assert(ae_wav_header(&m.format, m.pcm_bytes, header) == AE_OK);
    RETRY(port.finish(port.context, &m, header, &evidence)); m = evidence;
    assert(commits == 2); unsigned before = disk.writes;
    boot(); for (unsigned i = 0; i < 10000 && !state->view.store.mounted; ++i) tick();
    assert(state->view.store.mounted && commits == 2 && disk.writes == before && provision_calls == 2);
    assert(port.get(port.context, &m.object_id, &evidence) == AE_OK && evidence.state == AE_RECORD_COMPLETED && !memcmp(evidence.sha256, m.sha256, 32));
    /* The next lease is durably burned even when commit acknowledgement is
     * uncertain. No page write may use the unacknowledged range. */
    commit_uncertain = true; ae_record_metadata_t another = metadata(2);
    assert(port.reserve(port.context, &another) == AE_AGAIN); tick();
    assert(state->view.store.storage_uncertain && disk.writes == before && commits == 3);
    uint64_t durable_counter = read64(nvs_value + 56); assert(durable_counter == 1 + 2U * AE_STORE_NONCE_LEASE);
    commit_uncertain = false; boot();
    for (unsigned i = 0; i < 10000 && !state->view.store.mounted; ++i) tick();
    assert(state->view.store.mounted); RETRY(port.reserve(port.context, &another));
    assert(read64(nvs_value + 56) == durable_counter + AE_STORE_NONCE_LEASE);
    ae_store_deinit(state->store); free(state->store_memory); ae_secure_zero(state, sizeof(*state)); free(state); state = NULL;
    puts("PASS: actual recording runtime and encrypted store, explicit ownership, blank boot preservation, NVS custody, no per-frame commits, reboot and ambiguous nonce burn");
    return 0;
}
