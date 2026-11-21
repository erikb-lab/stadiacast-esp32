idf_component_register(
    SRCS "main.c" "wifi_ap.c" "net_services.c" "https_boot.c" "stadia_signaling.c" "stadia_transport.c" "usb_gamepad.c" "stadia_srtp.c"
    INCLUDE_DIRS "."
    REQUIRES nanopb stadia_proto
    PRIV_REQUIRES esp_https_server esp_http_server esp-tls esp_netif lwip esp_wifi esp_event esp_netif_stack esp_timer nvs_flash heap log esp_psram
    EMBED_TXTFILES "certs/fullchain.pem" "certs/privkey.pem"
)