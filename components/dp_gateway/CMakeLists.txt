idf_component_register(
    SRCS
        "dp_gateway.c"
        "dp_gateway_wifi.c"
        "dp_gateway_mqtt.c"
        "dp_gateway_adopt.c"
        "dp_gateway_decom.c"
    INCLUDE_DIRS "include"
    REQUIRES dp_common dp_mesh dp_prov
    PRIV_REQUIRES log esp_event esp_netif esp_wifi mqtt json esp-tls mbedtls
)

if(CONFIG_DOCKPULSE_ROLE_GATEWAY AND
   NOT CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB AND
   CONFIG_DOCKPULSE_MQTT_TLS)
    set(_dp_cert "${CMAKE_SOURCE_DIR}/certs/client.crt")
    set(_dp_key  "${CMAKE_SOURCE_DIR}/certs/client.key")
    if(NOT EXISTS "${_dp_cert}" OR NOT EXISTS "${_dp_key}")
        message(FATAL_ERROR
            "DOCKPULSE_MQTT_TLS is on but certs/client.crt or certs/client.key "
            "is missing. Issue a device cert with `gen_certs.sh device <id>` "
            "in the dockpulse repo and copy the .crt + .key into "
            "${CMAKE_SOURCE_DIR}/certs/.")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${_dp_cert}" TEXT)
    target_add_binary_data(${COMPONENT_LIB} "${_dp_key}"  TEXT)
endif()
