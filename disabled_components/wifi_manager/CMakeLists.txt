# CMakeLists.txt for wifi_manager component
idf_component_register(SRCS "wifi_manager.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_server storage oled_display)
