idf_component_register(
    SRCS "dp_mesh.c" "dp_mesh_sensor.c" "dp_mesh_provisioner.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES dp_common dp_prov bt nvs_flash esp_hw_support esp_timer mbedtls
)
