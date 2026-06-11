# CMakeLists.txt for storage component
idf_component_register(SRCS "storage.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash spiffs)
