idf_component_register(
    SRCS "main.c" "passport_board.c" "passport_ble.c" "passport_store.c" "passport_ui.c" "passport_widgets.c" "passport_theme.c"
    PRIV_REQUIRES bootloader_support hid_engine bt esp_event esp_http_server esp_netif esp_wifi nvs_flash
)
