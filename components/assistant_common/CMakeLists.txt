idf_component_register(SRCS "assistant_common.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client
                       PRIV_REQUIRES freertos)
