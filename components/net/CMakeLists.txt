idf_component_register(SRCS "wifi.cpp" "alert.cpp"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES vitals esp_wifi esp_netif esp_event esp_timer nvs_flash
                                  esp_http_server esp_http_client wpa_supplicant lwip mbedtls)
