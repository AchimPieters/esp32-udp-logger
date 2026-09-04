if(CONFIG_ESP32_UDP_LOGGER_ENABLED)
    idf_component_register(SRCS "src/esp32_udp_logger.c"
                           INCLUDE_DIRS "include"
                           REQUIRES lwip
                           PRIV_REQUIRES esp_event esp_netif mdns)
else()
    idf_component_register(INCLUDE_DIRS "include")
endif()
