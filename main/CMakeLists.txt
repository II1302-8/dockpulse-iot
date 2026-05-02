set(srcs "app_main.c")

if(CONFIG_DOCKPULSE_ROLE_SENSOR)
    list(APPEND srcs "sensor_main.c")
endif()

if(CONFIG_DOCKPULSE_ROLE_GATEWAY)
    list(APPEND srcs "gateway_main.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES dp_common dp_mesh dp_radar dp_gateway dp_prov dp_io nvs_flash
)
