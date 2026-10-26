file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
idf_component_register(SRCS "main.c" "app_ui.c" "ble_hid.c" "board_mosaico.c" "usb_console.c" "usb_hid.c"
                           "settings_store.c" "app_wifi.c" "app_presenter.c" "app_boot_ui.c" "usb_audio.c" "board_touch_mosaico.c"
                           "app_audio.c" "app_media_runtime.c" "app_audio_diagnostics.c" "board_display_transfer.c" "board_display_resources.c" "bt_audio.c" "board_audio_mosaico.c"
                           "board_recording_flash.c" "board_recording_nand.c" "app_spoken_letters.c"
                           "app_recording_store.c" "ble_hid_store.c" "app_command_bridge.c" "app_remote_input.c"
                           "app_media_controls.c" "app_control_runtime.c" "app_control_server.c" "app_control_trust.c" "app_clock.c" "app_firmware_update.c"
                           "app_connectivity_controls.c" "app_bluetooth_management.c" "board_imu_queue.c" "motion_capture.c"
                           "board_sensor_journal.c" "sensor_wire.c" "app_sensor_api.c"
                           "app_remote_device.c" "app_device_restart.c"
                           "app_remote_media.c"
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}/generated"
                       REQUIRES esp-mosaico-bsp hid_engine interaction device_settings haptics bt audio_engine control_protocol spoken_letters esp-sr
                                nvs_flash esp_tinyusb esp_timer mbedtls esp_wifi esp_netif
                                esp_event esp_http_server esp_https_server esp_http_client app_update esp_app_format esp_driver_i2c esp_driver_spi esp_partition spi_flash)

if(CONFIG_APP_LOCAL_SPEAKER_TEST)
    target_sources(${COMPONENT_LIB} PRIVATE "app_local_speaker_test.c")
endif()

# Run on every build, including incremental builds after a commit or local edit.
# The generator changes the header only when identity changes, so no-op builds
# do not recompile main. The generated file is never committed to source.
get_filename_component(input_repository_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_custom_target(input_build_identity
    COMMAND "${PYTHON}" "${input_repository_root}/tools/build_identity.py"
            --root "${input_repository_root}"
            --config "${SDKCONFIG}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/generated/app_build_identity.h"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/app_build_identity.h"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} input_build_identity)

# Apply board resource policy at the BSP's public IO factory boundary without
# changing vendor sources or the display-transfer observer.
# Force extraction of the otherwise unreferenced wrapper from libmain even
# when the BSP factory reference is encountered later in the archive scan.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_lcd_new_panel_io_spi"
    "-Wl,--undefined=__wrap_esp_lcd_new_panel_io_spi")
