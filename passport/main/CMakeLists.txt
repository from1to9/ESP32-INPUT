idf_component_register(
    SRCS "main.c" "passport_board.c" "passport_ble.c" "passport_store.c" "passport_ui.c" "passport_widgets.c" "passport_theme.c" "passport_wifi.c" "passport_wifi_text.c" "passport_wifi_store.c"
         "passport_setup.c" "passport_sound.c" "passport_speaker.c" "passport_names.c" "passport_names_text.c"
         "passport_control.c" "passport_control_http.c" "passport_management.c" "../../firmware/main/app_remote_input.c"
         "passport_usb.c" "passport_usb_protocol.c"
    INCLUDE_DIRS "../../firmware/main"
    PRIV_REQUIRES bootloader_support hid_engine interaction control_protocol bt esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag esp_app_format esp_event esp_http_server esp_netif esp_wifi nvs_flash json mbedtls
    EMBED_TXTFILES "web/controller.html" "web/controller.css" "web/controller.js" "web/setup.html" "web/setup.css" "web/setup.js"
)
# The pinned host repeats this initializer while selecting its default IRK.
# Passport owns the RAM-store lifecycle and its persistent callback wrappers.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_store_config_init")
