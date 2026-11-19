idf_component_register(SRCS "main.cpp"
                       REQUIRES esp_driver_i2c esp_driver_gpio esp_timer max30102 pulse_ox net)
