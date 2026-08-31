set(private_dependencies
    app_update
    esp_app_format
    esp_driver_gpio
    esp_eth
    esp_event
    esp_http_server
    esp_https_server
    esp_netif
    esp_partition
    esp_system
    esp_timer
    lwip
)

if(DEFINED IDF_VERSION_MAJOR AND IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND private_dependencies ip101)
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "bacnet_codec.c"
        "bacnet_server.c"
        "ota_auth.c"
        "ota_server.c"
        "switch_inputs.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${private_dependencies}
    EMBED_TXTFILES
        "ota_server_cert.pem"
        "ota_server_key.pem"
)
