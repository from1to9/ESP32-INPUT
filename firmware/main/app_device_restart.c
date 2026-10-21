// SPDX-License-Identifier: Apache-2.0
#include "app_device_restart.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESTART_REPLY_MS 200U
#define RESTART_AUTHORITY_MAX_AGE_MS 50U
#define RESTART_MAX_JOB_MS 5000U
static portMUX_TYPE s_restart_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_restart_worker;
static app_device_restart_status_t s_restart_status;
static uint64_t s_restart_requested_ms, s_restart_deadline_ms, s_restart_authorized_ms;
static bool s_restart_authorized, s_restart_maintenance;

static bool restart_now(uint64_t *now)
{
    int64_t us = esp_timer_get_time();
    if (us < 0) return false;
    *now = (uint64_t)us / 1000U; return true;
}
static void restart_service(void)
{
    uint64_t now = 0;
    bool clock_known = restart_now(&now), dispatch = false;
    portENTER_CRITICAL(&s_restart_lock);
    if (s_restart_status.state == APP_DEVICE_RESTART_PENDING) {
        if (!clock_known || now < s_restart_requested_ms || now >= s_restart_deadline_ms ||
            (s_restart_authorized && (now < s_restart_authorized_ms || now - s_restart_authorized_ms > RESTART_AUTHORITY_MAX_AGE_MS))) {
            s_restart_status.state = APP_DEVICE_RESTART_CANCELLED;
            s_restart_status.error = ESP_ERR_TIMEOUT;
        } else if (s_restart_authorized && s_restart_maintenance && now - s_restart_requested_ms >= RESTART_REPLY_MS) {
            s_restart_status.state = APP_DEVICE_RESTART_DISPATCHED;
            dispatch = true;
        }
    }
    portEXIT_CRITICAL(&s_restart_lock);
    if (dispatch) {
        esp_restart();
        /* Only a broken SDK boundary can return. Never label it a reboot. */
        portENTER_CRITICAL(&s_restart_lock);
        s_restart_status.state = APP_DEVICE_RESTART_UNCERTAIN;
        s_restart_status.error = ESP_FAIL;
        portEXIT_CRITICAL(&s_restart_lock);
    }
}
static void restart_worker(void *argument)
{
    (void)argument;
    for (;;) { restart_service(); vTaskDelay(pdMS_TO_TICKS(10)); }
}
esp_err_t app_device_restart_init(void)
{
    if (s_restart_worker) return ESP_OK;
    return xTaskCreate(restart_worker, "device_restart", 2048, NULL, 2, &s_restart_worker) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t app_device_restart_request(uint64_t deadline_ms, uint64_t *request_id)
{
    uint64_t now;
    if (!request_id || !restart_now(&now) || deadline_ms <= now || deadline_ms - now > RESTART_MAX_JOB_MS) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_restart_lock);
    if (!s_restart_worker || s_restart_status.request_id == UINT64_MAX ||
        s_restart_status.state == APP_DEVICE_RESTART_PENDING || s_restart_status.state == APP_DEVICE_RESTART_DISPATCHED ||
        s_restart_status.state == APP_DEVICE_RESTART_UNCERTAIN) {
        portEXIT_CRITICAL(&s_restart_lock); return ESP_ERR_INVALID_STATE;
    }
    s_restart_status = (app_device_restart_status_t){.request_id = s_restart_status.request_id + 1,
        .state = APP_DEVICE_RESTART_PENDING, .error = ESP_OK};
    s_restart_requested_ms = now; s_restart_deadline_ms = deadline_ms;
    s_restart_authorized = s_restart_maintenance = false;
    *request_id = s_restart_status.request_id;
    portEXIT_CRITICAL(&s_restart_lock); return ESP_OK;
}
void app_device_restart_authorize(uint64_t request_id, bool authorized, bool maintenance_ready)
{
    uint64_t now;
    if (!restart_now(&now)) authorized = false;
    portENTER_CRITICAL(&s_restart_lock);
    if (request_id == s_restart_status.request_id && s_restart_status.state == APP_DEVICE_RESTART_PENDING) {
        if (!authorized) {
            s_restart_status.state = APP_DEVICE_RESTART_CANCELLED;
            s_restart_status.error = ESP_ERR_INVALID_STATE;
        } else {
            s_restart_authorized = true; s_restart_maintenance = maintenance_ready;
            s_restart_authorized_ms = now;
        }
    }
    portEXIT_CRITICAL(&s_restart_lock);
}
esp_err_t app_device_restart_cancel(uint64_t request_id)
{
    portENTER_CRITICAL(&s_restart_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (request_id == s_restart_status.request_id && s_restart_status.state == APP_DEVICE_RESTART_PENDING) {
        s_restart_status.state = APP_DEVICE_RESTART_CANCELLED; s_restart_status.error = ESP_OK; error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_restart_lock); return error;
}
app_device_restart_status_t app_device_restart_status(void)
{
    portENTER_CRITICAL(&s_restart_lock);
    app_device_restart_status_t status = s_restart_status;
    portEXIT_CRITICAL(&s_restart_lock); return status;
}
