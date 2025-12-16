idf_component_register(
  SRCS "src/esp32_udp_logger.c"
  INCLUDE_DIRS "include"
  REQUIRES lwip
  PRIV_REQUIRES esp_event
)
