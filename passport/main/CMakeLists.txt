idf_component_register(
    SRCS "main.c" "passport_board.c"
    PRIV_REQUIRES bootloader_support bt esp_event esp_http_server esp_netif esp_wifi nvs_flash
)
