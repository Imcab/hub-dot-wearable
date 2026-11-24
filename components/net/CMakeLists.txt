idf_component_register(SRCS "wifi.cpp" "db_upload.cpp" "mqtt_pub.cpp"
                    INCLUDE_DIRS "include"
                    # ArduinoJson es publico: los main.cpp arman el payload con el
                    REQUIRES bblanchon__arduinojson
                    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_client wpa_supplicant
                                  espressif__mqtt)
