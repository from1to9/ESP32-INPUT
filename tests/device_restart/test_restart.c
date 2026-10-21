// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app_device_restart.c"

static int64_t clock_us;
static unsigned lock_depth, dispatches, creates;
static bool create_failure;
int64_t esp_timer_get_time(void) { return clock_us; }
void audio_test_enter(portMUX_TYPE *lock) { (void)lock; assert(!lock_depth++); }
void audio_test_leave(portMUX_TYPE *lock) { (void)lock; assert(lock_depth-- == 1); }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *argument, unsigned priority, TaskHandle_t *task)
{
    assert(!lock_depth && function == restart_worker && !strcmp(name,"device_restart") && stack == 2048 && !argument && priority == 2);
    ++creates; if (create_failure) return 0; *task = &creates; return pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; assert(false); }
void esp_restart(void)
{
    assert(!lock_depth && app_device_restart_status().state == APP_DEVICE_RESTART_DISPATCHED);
    ++dispatches;
}
static void reset(void)
{
    s_restart_worker = NULL; memset(&s_restart_status,0,sizeof(s_restart_status));
    s_restart_requested_ms = s_restart_deadline_ms = s_restart_authorized_ms = 0;
    s_restart_authorized = s_restart_maintenance = false;
    clock_us = 1000000; dispatches = creates = lock_depth = 0; create_failure = false;
}
static void at(uint64_t ms) { clock_us = (int64_t)(ms * 1000U); }
static uint64_t request(void)
{
    uint64_t id; assert(app_device_restart_request(2000,&id) == ESP_OK); return id;
}
static void admission_and_no_implicit_restart(void)
{
    reset(); uint64_t id = 9;
    assert(app_device_restart_request(2000,&id) == ESP_ERR_INVALID_STATE && id == 9);
    create_failure = true; assert(app_device_restart_init() == ESP_ERR_NO_MEM && !dispatches);
    create_failure = false; assert(app_device_restart_init() == ESP_OK && app_device_restart_init() == ESP_OK && creates == 2);
    assert(app_device_restart_request(1000,&id) == ESP_ERR_INVALID_ARG);
    assert(app_device_restart_request(6001,&id) == ESP_ERR_INVALID_ARG);
    id = request(); restart_service(); assert(!dispatches && app_device_restart_status().state == APP_DEVICE_RESTART_PENDING);
    assert(app_device_restart_request(2000,&id) == ESP_ERR_INVALID_STATE);
    at(1300); restart_service(); assert(!dispatches); /* No authorization, no work. */
    at(2000); restart_service(); assert(app_device_restart_status().state == APP_DEVICE_RESTART_CANCELLED && !dispatches);
}
static void fresh_authority_actual_maintenance_and_irreversible_receipt(void)
{
    reset(); assert(app_device_restart_init() == ESP_OK); uint64_t id = request();
    app_device_restart_authorize(id,true,true); at(1199); app_device_restart_authorize(id,true,true); restart_service(); assert(!dispatches);
    at(1200); app_device_restart_authorize(id,true,false); restart_service(); assert(!dispatches);
    app_device_restart_authorize(id,true,true); restart_service(); assert(dispatches == 1);
    assert(app_device_restart_status().state == APP_DEVICE_RESTART_UNCERTAIN); /* Stub returns; no observed reboot. */
    restart_service(); assert(dispatches == 1 && app_device_restart_cancel(id) == ESP_ERR_INVALID_STATE);
    assert(app_device_restart_request(2000,&id) == ESP_ERR_INVALID_STATE);
}
static void cancellation_staleness_and_clock_fail_closed(void)
{
    reset(); assert(app_device_restart_init() == ESP_OK); uint64_t first = request();
    app_device_restart_authorize(first,true,true); at(1051); restart_service();
    assert(app_device_restart_status().state == APP_DEVICE_RESTART_CANCELLED && !dispatches);
    uint64_t second = request(); assert(second > first);
    assert(app_device_restart_cancel(first) == ESP_ERR_INVALID_STATE);
    app_device_restart_authorize(first,true,true); assert(!s_restart_authorized);
    app_device_restart_authorize(second,false,false); app_device_restart_authorize(second,true,true);
    at(1400); restart_service(); assert(app_device_restart_status().state == APP_DEVICE_RESTART_CANCELLED && !dispatches);
    uint64_t third = request(); app_device_restart_authorize(third,true,true); at(1399); restart_service();
    assert(app_device_restart_status().state == APP_DEVICE_RESTART_CANCELLED && !dispatches);
    at(1500); uint64_t fourth = request(); assert(app_device_restart_cancel(fourth) == ESP_OK);
    s_restart_status.request_id = UINT64_MAX;
    assert(app_device_restart_request(2000,&fourth) == ESP_ERR_INVALID_STATE && !dispatches);
}
int main(void)
{
    admission_and_no_implicit_restart(); fresh_authority_actual_maintenance_and_irreversible_receipt(); cancellation_staleness_and_clock_fail_closed();
    puts("Restart worker: bounded admission, fresh authority, actual maintenance, cancellation, stale identity, clock failure and truthful irreversible receipt passed.");
}
