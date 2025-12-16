#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Zero-config UDP logger for ESP-IDF.
 *
 * Normal usage: add only this line somewhere in your firmware:
 *     #include "esp32_udp_logger.h"
 *
 * That's it. The logger will autostart once the device has an IP address.
 *
 * Default behavior:
 *   - Hooks ESP_LOGx() output (keeps original output too).
 *   - Sends logs to UDP broadcast on port CONFIG_ESP32_UDP_LOGGER_PORT (default 9999).
 *   - Listens for commands on UDP port CONFIG_ESP32_UDP_LOGGER_RX_PORT (default 9998).
 *   - Sets a unique mDNS hostname: esp32-udp-logger-XXXX.local
 */

void esp32_udp_logger_autostart(void);
void esp32_udp_logger_stop(void);

bool esp32_udp_logger_bind(const char *ipv4, uint16_t port);
void esp32_udp_logger_unbind(void);
void esp32_udp_logger_set_broadcast(bool enable);

uint32_t esp32_udp_logger_get_drop_count(void);
const char *esp32_udp_logger_get_hostname(void);

#ifdef __cplusplus
}
#endif

// ---------- Zero-config autostart ----------
// Include this header once anywhere and it will "just work".
#ifndef ESP32_UDP_LOGGER_NO_AUTOSTART
__attribute__((constructor))
static void esp32_udp_logger__ctor(void) {
  esp32_udp_logger_autostart();
}
#endif
