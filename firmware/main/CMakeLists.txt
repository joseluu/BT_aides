set(srcs "main.c" "audio_capture.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS ".")
