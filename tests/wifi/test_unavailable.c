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
    assert(app_wifi_scan_start() == ESP_ERR_NOT_SUPPORTED);
    assert(app_wifi_scan_cancel() == ESP_ERR_NOT_SUPPORTED);
    assert(app_wifi_join(NULL) == ESP_ERR_NOT_SUPPORTED);
    app_wifi_selection_t selection;
    memset(&selection, 42, sizeof(selection));
    assert(app_wifi_select_scan_record(1, 0, &selection) == ESP_ERR_NOT_SUPPORTED && !selection.id);
    assert(app_wifi_release_selection(1) == ESP_ERR_NOT_SUPPORTED);
    app_wifi_scan_snapshot_t scan;
    app_wifi_scan_record_t record;
    app_wifi_get_scan_snapshot(&scan);
    assert(scan.state == APP_WIFI_SCAN_ERROR && scan.error == ESP_ERR_NOT_SUPPORTED);
    memset(&record, 42, sizeof(record));
    assert(app_wifi_get_scan_record(1, 0, &record) == ESP_ERR_NOT_SUPPORTED && !record.label[0]);
    app_wifi_get_snapshot(&snapshot);
    assert(!snapshot.initialized && !snapshot.configured && !snapshot.rssi_valid);
    assert(snapshot.state == APP_WIFI_UNAVAILABLE && snapshot.last_error == ESP_ERR_NOT_SUPPORTED);
    assert(!snapshot.lan_ready && !snapshot.backend_available && snapshot.backend_error == ESP_ERR_NOT_SUPPORTED);
    memset(&info, 42, sizeof(info));
    assert(app_wifi_get_setup_info(&info) == ESP_ERR_NOT_SUPPORTED && info.password[0] == 0);
    puts("PASS: Wi-Fi unavailable capability fails closed");
    return 0;
}
