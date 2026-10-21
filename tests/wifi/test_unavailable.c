// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include "../../firmware/main/app_wifi.c"

int main(void)
{
    app_wifi_snapshot_t snapshot;
    app_wifi_setup_info_t info;
    assert(app_wifi_init() == ESP_ERR_NOT_SUPPORTED);
    assert(app_wifi_start_setup() == ESP_ERR_NOT_SUPPORTED);
    assert(app_wifi_stop_setup() == ESP_ERR_NOT_SUPPORTED);
    assert(app_wifi_forget_network() == ESP_ERR_NOT_SUPPORTED);
    assert(app_wifi_verify_storage() == ESP_ERR_NOT_SUPPORTED);
    app_wifi_get_snapshot(&snapshot);
    assert(!snapshot.initialized && !snapshot.configured && !snapshot.rssi_valid);
    assert(snapshot.state == APP_WIFI_UNAVAILABLE && snapshot.last_error == ESP_ERR_NOT_SUPPORTED);
    memset(&info, 42, sizeof(info));
    assert(app_wifi_get_setup_info(&info) == ESP_ERR_NOT_SUPPORTED && info.password[0] == 0);
    puts("PASS: Wi-Fi unavailable capability fails closed");
    return 0;
}
