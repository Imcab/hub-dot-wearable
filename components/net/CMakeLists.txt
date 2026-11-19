idf_component_register(SRCS "wifi.cpp" "db_upload.cpp"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_client wpa_supplicant)
