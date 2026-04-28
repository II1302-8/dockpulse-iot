idf_component_register(
    SRCS "dp_radar.c" "dp_radar_filter.c"
    INCLUDE_DIRS "include"
    REQUIRES dp_common
    PRIV_REQUIRES esp_driver_uart esp_timer log freertos
)
