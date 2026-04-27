idf_component_register(
    SRCS
        "dp_gateway.c"
        "dp_gateway_wifi.c"
        "dp_gateway_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES dp_common
    PRIV_REQUIRES log esp_event esp_netif esp_wifi mqtt json
)
