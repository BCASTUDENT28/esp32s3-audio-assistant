# CMakeLists.txt for ota component
idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client esp_https_ota mbedtls oled_display)
