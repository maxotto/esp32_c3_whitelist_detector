idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES web_server
                             nvs_flash
                             esp_wifi
                             esp_event
                             esp_netif
                             lwip
                             led_strip)
