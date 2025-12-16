\
#include "esp32_udp_logger.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"

#if CONFIG_ESP32_UDP_LOGGER_ENABLE_MDNS
#include "mdns.h"
#endif

#include "sdkconfig.h"

#ifndef CONFIG_ESP32_UDP_LOGGER_ENABLED
#define CONFIG_ESP32_UDP_LOGGER_ENABLED 1
#endif

#if CONFIG_ESP32_UDP_LOGGER_ENABLED

typedef struct {
  uint16_t len;
  char data[CONFIG_ESP32_UDP_LOGGER_MAX_LINE];
} log_item_t;

typedef enum {
  DEST_BROADCAST = 0,
  DEST_UNICAST = 1,
} dest_mode_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_q;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;

static int s_tx_sock = -1;
static int s_rx_sock = -1;

static struct sockaddr_in s_bcast;
static bool s_bcast_ready = false;
static bool s_bcast_enabled = true;

static struct sockaddr_in s_unicast;
static bool s_unicast_ready = false;

static dest_mode_t s_mode = DEST_BROADCAST;
static uint32_t s_drop_count = 0;

static bool s_started = false;
static bool s_autostart_inited = false;

static int (*s_prev_vprintf)(const char *fmt, va_list ap) = NULL;
static bool s_hooked = false;

static char s_hostname[32] = {0};

static void lock_take(void){ if (s_lock) (void)xSemaphoreTake(s_lock, portMAX_DELAY); }
static void lock_give(void){ if (s_lock) (void)xSemaphoreGive(s_lock); }

static void make_hostname_once(void)
{
  if (s_hostname[0] != 0) return;

  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    (void)esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
  }
  snprintf(s_hostname, sizeof(s_hostname), "esp32-udp-logger-%02X%02X", mac[4], mac[5]);
}

const char *esp32_udp_logger_get_hostname(void)
{
  return s_hostname;
}

static bool compute_broadcast_from_any_netif(struct in_addr *out)
{
  if (!out) return false;

  const char *keys[] = {"WIFI_STA_DEF", "ETH_DEF"};
  for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(keys[i]);
    if (!n) continue;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(n, &info) != ESP_OK) continue;
    if (info.ip.addr == 0 || info.netmask.addr == 0) continue;

    uint32_t ip = info.ip.addr;      // network order
    uint32_t nm = info.netmask.addr; // network order
    uint32_t bcast = (ip & nm) | (~nm);

    out->s_addr = bcast;
    return true;
  }
  return false;
}

static void mdns_start_once(void)
{
#if CONFIG_ESP32_UDP_LOGGER_ENABLE_MDNS
  static bool done = false;
  if (done) return;
  done = true;

  make_hostname_once();

  esp_err_t e = mdns_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;

  (void)mdns_hostname_set(s_hostname);
  (void)mdns_instance_name_set(s_hostname);

#if CONFIG_ESP32_UDP_LOGGER_SERVICE
  (void)mdns_service_add(NULL, "_esp32udplog", "_udp",
                         CONFIG_ESP32_UDP_LOGGER_RX_PORT, NULL, 0);
#endif
#endif
}

static void ensure_sockets(void)
{
  if (s_tx_sock < 0) {
    s_tx_sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  }
  if (s_rx_sock < 0) {
    s_rx_sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_rx_sock >= 0) {
      struct sockaddr_in bind_addr;
      memset(&bind_addr, 0, sizeof(bind_addr));
      bind_addr.sin_family = AF_INET;
      bind_addr.sin_port = htons(CONFIG_ESP32_UDP_LOGGER_RX_PORT);
      bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

      if (bind(s_rx_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(s_rx_sock);
        s_rx_sock = -1;
      } else {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        (void)setsockopt(s_rx_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      }
    }
  }
}

static void update_broadcast_dest(void)
{
  struct in_addr b;
  if (!compute_broadcast_from_any_netif(&b)) return;

  memset(&s_bcast, 0, sizeof(s_bcast));
  s_bcast.sin_family = AF_INET;
  s_bcast.sin_port = htons(CONFIG_ESP32_UDP_LOGGER_PORT);
  s_bcast.sin_addr = b;
  s_bcast_ready = true;
}

static void start_hook_if_needed(void);

static void queue_line(const char *data, size_t len)
{
  if (!data || len == 0 || !s_q) return;

  log_item_t item;
  size_t n = len;
  if (n > sizeof(item.data)) n = sizeof(item.data);

  memcpy(item.data, data, n);
  item.len = (uint16_t)n;

#if CONFIG_ESP32_UDP_LOGGER_DROP_ON_FULL
  if (xQueueSend(s_q, &item, 0) != pdTRUE) {
    s_drop_count++;
  }
#else
  (void)xQueueSend(s_q, &item, portMAX_DELAY);
#endif
}

static int udp_vprintf(const char *fmt, va_list ap)
{
  int n1 = 0;
  if (s_prev_vprintf) {
    va_list ap2;
    va_copy(ap2, ap);
    n1 = s_prev_vprintf(fmt, ap2);
    va_end(ap2);
  }

  if (!s_started) return n1;

  char tmp[CONFIG_ESP32_UDP_LOGGER_MAX_LINE];

#if CONFIG_ESP32_UDP_LOGGER_PREFIX_DEVICE
  if (s_hostname[0] == 0) make_hostname_once();
  int p = snprintf(tmp, sizeof(tmp), "[%s] ", s_hostname[0] ? s_hostname : "esp32-udp-logger");
  if (p < 0 || p >= (int)sizeof(tmp)) return n1;

  int n = vsnprintf(tmp + p, sizeof(tmp) - (size_t)p, fmt, ap);
  if (n <= 0) return n1;

  size_t to_send = (size_t)p + (size_t)n;
  if (to_send >= sizeof(tmp)) to_send = sizeof(tmp) - 1;

  queue_line(tmp, to_send);
#else
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  if (n <= 0) return n1;

  size_t to_send = (size_t)n;
  if (to_send >= sizeof(tmp)) to_send = sizeof(tmp) - 1;

  queue_line(tmp, to_send);
#endif

  return n1;
}

static void start_hook_if_needed(void)
{
  if (s_hooked) return;
  s_prev_vprintf = esp_log_set_vprintf(udp_vprintf);
  s_hooked = true;
}

static void maybe_start_tasks_locked(void)
{
  if (s_started) return;

  ensure_sockets();
  if (s_tx_sock < 0) return;

  update_broadcast_dest();
  if (!s_bcast_ready) return;

  if (!s_tx_task) {
    BaseType_t ok = xTaskCreate(
      [](void *arg){
        (void)arg;
        log_item_t item;

        for (;;) {
          if (xQueueReceive(s_q, &item, portMAX_DELAY) != pdTRUE) continue;

          lock_take();
          int sock = s_tx_sock;
          dest_mode_t mode = s_mode;
          bool bcast_ok = s_bcast_enabled && s_bcast_ready;
          struct sockaddr_in bdest = s_bcast;
          bool u_ok = s_unicast_ready;
          struct sockaddr_in udest = s_unicast;
          lock_give();

          if (sock < 0) continue;

          if (mode == DEST_UNICAST && u_ok) {
            (void)sendto(sock, item.data, item.len, 0,
                         (struct sockaddr *)&udest, sizeof(udest));
          } else if (bcast_ok) {
            int yes = 1;
            (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            (void)sendto(sock, item.data, item.len, 0,
                         (struct sockaddr *)&bdest, sizeof(bdest));
          }
        }
      },
      "udp_log_tx",
      CONFIG_ESP32_UDP_LOGGER_TASK_STACK,
      NULL,
      CONFIG_ESP32_UDP_LOGGER_TASK_PRIO,
      &s_tx_task
    );
    if (ok != pdPASS) {
      s_tx_task = NULL;
      return;
    }
  }

  if (!s_rx_task && s_rx_sock >= 0) {
    BaseType_t ok = xTaskCreate(
      [](void *arg){
        (void)arg;
        char buf[512];

        auto reply = [](const char *msg, const struct sockaddr *to, socklen_t tolen){
          if (s_rx_sock < 0 || !msg) return;
          (void)sendto(s_rx_sock, msg, strlen(msg), 0, to, tolen);
        };

        auto parse_ip_port = [](const char *ip, const char *port_str, struct sockaddr_in *out)->bool {
          if (!ip || !port_str || !out) return false;
          int port = atoi(port_str);
          if (port <= 0 || port > 65535) return false;
          struct in_addr a;
          if (inet_pton(AF_INET, ip, &a) != 1) return false;
          memset(out, 0, sizeof(*out));
          out->sin_family = AF_INET;
          out->sin_port = htons((uint16_t)port);
          out->sin_addr = a;
          return true;
        };

        for (;;) {
          struct sockaddr_in from;
          socklen_t flen = sizeof(from);
          int n = recvfrom(s_rx_sock, buf, sizeof(buf)-1, 0, (struct sockaddr *)&from, &flen);
          if (n <= 0) continue;
          buf[n] = 0;

          char *argv[4] = {0};
          int argc = 0;
          char *p = buf;
          while (*p && argc < 4) {
            while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
            if (*p) *p++ = 0;
          }
          if (argc == 0) continue;

          if (!strcmp(argv[0], "bind") && argc >= 3) {
            struct sockaddr_in u;
            if (!parse_ip_port(argv[1], argv[2], &u)) {
              reply("ERR usage: bind <ipv4> <port>\n", (struct sockaddr *)&from, flen);
              continue;
            }
            lock_take();
            s_unicast = u;
            s_unicast_ready = true;
            s_mode = DEST_UNICAST;
            lock_give();
            reply("OK bound\n", (struct sockaddr *)&from, flen);
            continue;
          }

          if (!strcmp(argv[0], "unbind")) {
            lock_take();
            s_mode = DEST_BROADCAST;
            lock_give();
            reply("OK unbound\n", (struct sockaddr *)&from, flen);
            continue;
          }

          if (!strcmp(argv[0], "broadcast") && argc >= 2) {
            bool on = (!strcmp(argv[1], "on") || !strcmp(argv[1], "1"));
            bool off = (!strcmp(argv[1], "off") || !strcmp(argv[1], "0"));
            if (!on && !off) {
              reply("ERR usage: broadcast on|off\n", (struct sockaddr *)&from, flen);
              continue;
            }
            lock_take();
            s_bcast_enabled = on;
            lock_give();
            reply(on ? "OK broadcast on\n" : "OK broadcast off\n", (struct sockaddr *)&from, flen);
            continue;
          }

          if (!strcmp(argv[0], "status")) {
            char msg[220];
            lock_take();
            const char *mode = (s_mode == DEST_UNICAST) ? "unicast" : "broadcast";
            bool b = s_bcast_enabled;
            uint32_t drops = s_drop_count;
            bool uok = s_unicast_ready;
            struct sockaddr_in u = s_unicast;
            lock_give();

            char ipbuf[16] = {0};
            if (uok) inet_ntop(AF_INET, &u.sin_addr, ipbuf, sizeof(ipbuf));

            snprintf(msg, sizeof(msg),
                     "host=%s mode=%s broadcast=%s drops=%lu unicast=%s:%u\n",
                     s_hostname[0] ? s_hostname : "(pending)",
                     mode,
                     b ? "on" : "off",
                     (unsigned long)drops,
                     uok ? ipbuf : "-",
                     uok ? (unsigned)ntohs(u.sin_port) : 0u);

            reply(msg, (struct sockaddr *)&from, flen);
            continue;
          }

          reply("ERR unknown command\n", (struct sockaddr *)&from, flen);
        }
      },
      "udp_log_rx",
      3072,
      NULL,
      CONFIG_ESP32_UDP_LOGGER_TASK_PRIO,
      &s_rx_task
    );
    if (ok != pdPASS) {
      s_rx_task = NULL;
    }
  }

  mdns_start_once();
  start_hook_if_needed();
  s_started = true;
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  (void)arg; (void)base; (void)id; (void)data;

  lock_take();
  update_broadcast_dest();
  maybe_start_tasks_locked();
  lock_give();
}

void esp32_udp_logger_autostart(void)
{
  if (s_autostart_inited) return;
  s_autostart_inited = true;

  esp_err_t e;

  e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;

  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;

  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return;

  if (!s_q) s_q = xQueueCreate(CONFIG_ESP32_UDP_LOGGER_QUEUE_DEPTH, sizeof(log_item_t));
  if (!s_q) return;

  make_hostname_once();

  (void)esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL);

  lock_take();
  ensure_sockets();
  update_broadcast_dest();
  maybe_start_tasks_locked();
  lock_give();
}

void esp32_udp_logger_stop(void)
{
  lock_take();

  if (s_hooked) {
    (void)esp_log_set_vprintf(s_prev_vprintf);
    s_hooked = false;
    s_prev_vprintf = NULL;
  }

  if (s_rx_task) { vTaskDelete(s_rx_task); s_rx_task = NULL; }
  if (s_tx_task) { vTaskDelete(s_tx_task); s_tx_task = NULL; }

  if (s_rx_sock >= 0) { close(s_rx_sock); s_rx_sock = -1; }
  if (s_tx_sock >= 0) { close(s_tx_sock); s_tx_sock = -1; }

  s_started = false;
  s_bcast_ready = false;
  s_unicast_ready = false;
  s_mode = DEST_BROADCAST;

  lock_give();
}

bool esp32_udp_logger_bind(const char *ipv4, uint16_t port)
{
  if (!ipv4 || port == 0) return false;

  struct in_addr a;
  if (inet_pton(AF_INET, ipv4, &a) != 1) return false;

  lock_take();
  memset(&s_unicast, 0, sizeof(s_unicast));
  s_unicast.sin_family = AF_INET;
  s_unicast.sin_port = htons(port);
  s_unicast.sin_addr = a;
  s_unicast_ready = true;
  s_mode = DEST_UNICAST;
  lock_give();
  return true;
}

void esp32_udp_logger_unbind(void)
{
  lock_take();
  s_mode = DEST_BROADCAST;
  lock_give();
}

void esp32_udp_logger_set_broadcast(bool enable)
{
  lock_take();
  s_bcast_enabled = enable;
  lock_give();
}

uint32_t esp32_udp_logger_get_drop_count(void)
{
  return s_drop_count;
}

#else

void esp32_udp_logger_autostart(void) {}
void esp32_udp_logger_stop(void) {}
bool esp32_udp_logger_bind(const char *ipv4, uint16_t port) { (void)ipv4; (void)port; return false; }
void esp32_udp_logger_unbind(void) {}
void esp32_udp_logger_set_broadcast(bool enable) { (void)enable; }
uint32_t esp32_udp_logger_get_drop_count(void) { return 0; }
const char *esp32_udp_logger_get_hostname(void) { return ""; }

#endif
