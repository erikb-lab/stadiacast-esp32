idf_component_register(
    SRCS "main.c" "wifi_ap.c" "net_services.c" "https_boot.c" "stadia_signaling.c" "stadia_transport.c" "usb_gamepad.c" "usb_audio.c"
    INCLUDE_DIRS "."
    REQUIRES nanopb stadia_proto
    PRIV_REQUIRES esp_https_server esp_http_server esp-tls esp_netif lwip esp_wifi esp_event esp_netif_stack esp_timer nvs_flash heap log esp_psram
    EMBED_TXTFILES "certs/fullchain.pem" "certs/privkey.pem"
)

if(TARGET espressif__tinyusb)
    set(_tusb_target espressif__tinyusb)
else()
    idf_component_get_property(_tusb_lib espressif__tinyusb COMPONENT_LIB)
    set(_tusb_target "${_tusb_lib}")
endif()

set(_uac_defs
    CFG_TUD_AUDIO=1
    CFG_TUD_AUDIO_FUNC_1_N_AS_INT=1
    CFG_TUD_AUDIO_CTRL_BUF_SZ=64
    CFG_TUD_AUDIO_ENABLE_EP_IN=0
    CFG_TUD_AUDIO_ENABLE_EP_OUT=1
    CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=1536
    CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=192
    CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX=2
    CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX=16
    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX=2
)

if(TARGET "${_tusb_target}")
    target_compile_definitions("${_tusb_target}" PRIVATE ${_uac_defs})
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_uac_defs})
