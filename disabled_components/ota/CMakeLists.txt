# CMakeLists.txt for ota component
idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client esp_https_ota esp_crt_bundle oled_display)
