# CMakeLists.txt for backend_client component
idf_component_register(SRCS "backend_client.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client mbedtls json storage oled_display)
