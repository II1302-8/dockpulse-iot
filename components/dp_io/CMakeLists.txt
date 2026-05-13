idf_component_register(
    SRCS "dp_io.c"
    INCLUDE_DIRS "include"
    #    REQUIRES driver esp_driver_rmt
#ADDED: esp_adc required for battery voltage reading
    REQUIRES driver esp_driver_rmt esp_adc

)
