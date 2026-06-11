# CMakeLists.txt for oled_display component
idf_component_register(SRCS "oled_display.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver)
