// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "freertos/idf_additions.h"
#define APP_NETWORK_EXTERNAL_STACK 1
#define APP_NETWORK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define APP_NETWORK_EXTERNAL_STACK 0
#define APP_NETWORK_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

/* Only network/clock tasks whose current call chains do not execute flash
 * operations use this policy. Main, native Wi-Fi/NVS, runtime and OTA retain
 * their internal stacks. Stack capacities are specified by each owner.
 * Supported builds never fall back to internal RAM on PSRAM exhaustion.
 * TCBs remain internal in the SDK's WithCaps implementation.
 *
 * The pinned SDK self-delete path creates an internal cleanup task. Reserve
 * configMINIMAL_STACK_SIZE (currently 1536 bytes) plus one internal TCB for
 * each simultaneous self-deletion. One shutdown wave has the server owner,
 * HTTPS task and download (4608 cleanup-stack bytes plus three TCBs with the
 * current SDK configuration). The clock remains resident. Admission can occur
 * before a previous task's cleanup finishes, so three is a planning allowance,
 * not a proven global concurrency ceiling. This policy does not reserve that
 * memory; the SDK aborts if it cannot create its cleanup task. */
static inline BaseType_t app_network_task_create(TaskFunction_t entry, const char *name,
                                                 uint32_t stack_bytes, void *argument,
                                                 UBaseType_t priority, TaskHandle_t *handle)
{
#if APP_NETWORK_EXTERNAL_STACK
    return xTaskCreateWithCaps(entry, name, stack_bytes, argument, priority, handle,
                               APP_NETWORK_STACK_CAPS);
#else
    return xTaskCreate(entry, name, stack_bytes, argument, priority, handle);
#endif
}

static inline void app_network_task_delete_self(void)
{
#if APP_NETWORK_EXTERNAL_STACK
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}
