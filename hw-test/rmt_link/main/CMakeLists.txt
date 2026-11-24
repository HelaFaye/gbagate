idf_component_register(SRCS "main.c" "rl_codec.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_rmt esp_driver_gpio)
