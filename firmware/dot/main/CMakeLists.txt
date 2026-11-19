idf_component_register(SRCS "main.cpp"
                    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer mpu6050 tmp117 fall_detect axillary_temp net)
