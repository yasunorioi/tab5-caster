idf_component_register(
    SRCS
        "app_main.c"
        "usb_cdc_source.c"
        "mosaic_config.c"
        "rtcm_sink.c"
        "rtcm_monitor.c"
        "debug_console.c"
        "net_mdns.c"
        "board_power.c"
        "wifi_sta.c"
        "upstream.c"
    INCLUDE_DIRS "."
    REQUIRES console esp_timer mdns esp_driver_i2c ntripcaster esp_netif esp_event
             esp_wifi esp_wifi_remote nvs_flash lwip
)
