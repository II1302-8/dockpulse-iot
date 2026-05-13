idf_component_register(
    SRCS "dp_io.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_rmt esp_adc
    PRIV_REQUIRES dp_common
)
