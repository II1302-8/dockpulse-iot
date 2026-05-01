idf_component_register(
    SRCS "dp_mesh.c"
    INCLUDE_DIRS "include"
    REQUIRES dp_common dp_prov bt nvs_flash esp_hw_support esp_timer mbedtls
)
