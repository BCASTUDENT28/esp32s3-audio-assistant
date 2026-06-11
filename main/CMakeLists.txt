# CMakeLists.txt for main component
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES storage oled_display wifi_manager backend_client telegram ota esp_driver_gpio)
