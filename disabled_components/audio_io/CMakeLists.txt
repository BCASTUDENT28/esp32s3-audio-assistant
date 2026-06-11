# CMakeLists.txt for audio_io component
idf_component_register(SRCS "audio_io.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver)
