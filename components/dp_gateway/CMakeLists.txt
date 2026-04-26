idf_component_register(
    SRCS "dp_gateway.c"
    INCLUDE_DIRS "include"
    REQUIRES dp_common
    PRIV_REQUIRES log
)
