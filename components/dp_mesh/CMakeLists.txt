idf_component_register(
    SRCS "dp_mesh.c"
    INCLUDE_DIRS "include"
    REQUIRES dp_common bt nvs_flash esp_hw_support mbedtls
)
