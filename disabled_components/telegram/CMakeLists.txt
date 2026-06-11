# CMakeLists.txt for telegram component
idf_component_register(SRCS "telegram.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client esp_crt_bundle json storage ota)
