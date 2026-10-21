idf_component_register(SRCS "main.c" "app_ui.c" "ble_hid.c" "board_mosaico.c" "usb_console.c" "usb_hid.c"
                           "settings_store.c" "app_wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-mosaico-bsp hid_engine interaction device_settings haptics bt
                                nvs_flash esp_tinyusb esp_timer mbedtls esp_wifi esp_netif
                                esp_event esp_http_server esp_app_format esp_driver_i2c esp_driver_spi)
