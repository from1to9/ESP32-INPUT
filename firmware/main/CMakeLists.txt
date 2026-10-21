idf_component_register(SRCS "main.c" "app_ui.c" "ble_hid.c" "board_mosaico.c" "usb_console.c" "usb_hid.c"
                           "settings_store.c" "app_wifi.c" "app_presenter.c" "app_boot_ui.c" "usb_audio.c" "board_touch_mosaico.c"
                           "app_audio.c" "app_media_runtime.c" "board_display_transfer.c" "bt_audio.c" "board_audio_mosaico.c"
                           "board_recording_flash.c" "board_recording_nand.c" "app_spoken_letters.c"
                           "app_recording_store.c" "ble_hid_store.c" "app_command_bridge.c" "app_remote_input.c"
                           "app_media_controls.c" "app_control_runtime.c" "app_control_server.c" "app_control_trust.c" "app_clock.c" "app_firmware_update.c"
                           "app_connectivity_controls.c"
                           "app_remote_device.c" "app_device_restart.c"
                           "app_remote_media.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-mosaico-bsp hid_engine interaction device_settings haptics bt audio_engine control_protocol spoken_letters esp-sr
                                nvs_flash esp_tinyusb esp_timer mbedtls esp_wifi esp_netif
                                esp_event esp_http_server esp_https_server esp_http_client app_update esp_app_format esp_driver_i2c esp_driver_spi esp_partition spi_flash)
