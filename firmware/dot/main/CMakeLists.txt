idf_component_register(SRCS "main.cpp" "db_upload.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer mpu6050 tmp117 fall_detect net vitals
                             esp_http_client esp_netif)
