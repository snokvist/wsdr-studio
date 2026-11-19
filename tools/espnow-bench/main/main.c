// SPDX-License-Identifier: GPL-2.0-only
// ESP-NOW bench for wsdr-studio: a sender of numbered frames, and a receiver that counts them, driven
// from the serial console. Payload: "WSDR", u32 counter (little-endian), u8 mode, u8 rate code, then
// filler bytes (index & 0xff) to the set size. wsdr-studio's `test_core --wifi <recording>` reads the
// counters back, so what it missed is known exactly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

enum { MODE_BCAST, MODE_UCAST, MODE_ABSENT, MODE_ENC };
static const char *const kModes[] = {"bcast", "ucast", "absent", "enc"};
static const struct {
  const char *name;
  wifi_phy_mode_t phy;
  wifi_phy_rate_t rate;
} kRates[] = {
    {"1L", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_1M_L},   // ESP-NOW's default
    {"2L", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_2M_L},   {"2S", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_2M_S},
    {"11L", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_11M_L}, {"6M", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M},
    {"MCS0", WIFI_PHY_MODE_HT20, WIFI_PHY_RATE_MCS0_LGI},
};
#define N_RATES (sizeof kRates / sizeof kRates[0])

static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t kAbsent[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}; // nobody: every frame retried
static const uint8_t kLmk[ESP_NOW_KEY_LEN] = "wsdr-bench-lmk!"; // (16 bytes with its NUL)

static struct {
  bool tx;            // sender (else receiver)
  int chan, mode, rate, size, hz, count;
  uint8_t peer[6];
  bool have_peer;
} cfg = {.tx = true, .chan = 6, .mode = MODE_BCAST, .rate = 0, .size = 32, .hz = 20, .count = 0};

static volatile bool running; // (the console task sets it, the sender reads it)
static volatile uint32_t sent, send_ok, send_fail, send_err;
static uint32_t counter;

// receiver: the frames from one sender
static struct {
  uint8_t mac[6];
  uint32_t n, lo, hi;
  bool any;
} rx;

static void set_rate_on(const uint8_t *addr) {
  esp_now_rate_config_t rc = {.phymode = kRates[cfg.rate].phy, .rate = kRates[cfg.rate].rate, .ersu = false, .dcm = false};
  esp_err_t e = esp_now_set_peer_rate_config(addr, &rc);
  if (e != ESP_OK)
    printf("rate config: %s\n", esp_err_to_name(e));
}

// The peer the current mode sends to, added fresh (encrypted for "enc")
static const uint8_t *target(void) {
  const uint8_t *to = cfg.mode == MODE_BCAST ? kBroadcast : cfg.mode == MODE_ABSENT ? kAbsent : cfg.peer;
  esp_now_del_peer(to);
  esp_now_peer_info_t p = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = cfg.mode == MODE_ENC};
  memcpy(p.peer_addr, to, 6);
  if (p.encrypt)
    memcpy(p.lmk, kLmk, ESP_NOW_KEY_LEN);
  ESP_ERROR_CHECK(esp_now_add_peer(&p));
  set_rate_on(to);
  return to;
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  (void)info;
  if (status == ESP_NOW_SEND_SUCCESS)
    send_ok++;
  else
    send_fail++;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *d, int n) {
  if (cfg.tx || n < 8 || memcmp(d, "WSDR", 4) != 0)
    return;
  const uint32_t c = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
  if (!rx.any || memcmp(rx.mac, info->src_addr, 6) != 0) { // (a new sender: start again)
    memcpy(rx.mac, info->src_addr, 6);
    rx.n = 0;
    rx.lo = rx.hi = c;
    rx.any = true;
  }
  rx.n++;
  if (c < rx.lo)
    rx.lo = c;
  if (c > rx.hi)
    rx.hi = c;
}

static void sender(void *arg) {
  (void)arg;
  uint8_t buf[ESP_NOW_MAX_DATA_LEN_V2];
  for (;;) {
    if (!cfg.tx || !running) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const uint8_t *to = target();
    printf("sending: %s to " MACSTR ", rate %s, %d bytes, %d Hz, channel %d%s\n", kModes[cfg.mode], MAC2STR(to), kRates[cfg.rate].name,
           cfg.size, cfg.hz, cfg.chan, cfg.count ? "" : " (until stop)");
    int64_t next = esp_timer_get_time();
    int left = cfg.count;
    while (running && cfg.tx) {
      memcpy(buf, "WSDR", 4);
      memcpy(buf + 4, &counter, 4);
      buf[8] = (uint8_t)cfg.mode;
      buf[9] = (uint8_t)cfg.rate;
      for (int i = 10; i < cfg.size; i++)
        buf[i] = (uint8_t)i;
      const esp_err_t e = esp_now_send(to, buf, (size_t)cfg.size);
      if (e == ESP_OK) {
        counter++;
        sent++;
      } else {
        send_err++;
      }
      if (cfg.count && --left <= 0)
        running = false;
      next += 1000000 / cfg.hz;
      const int64_t wait = next - esp_timer_get_time();
      vTaskDelay(wait > 1000 ? pdMS_TO_TICKS(wait / 1000) : 1);
    }
    printf("stopped: counter now %lu, sent %lu (callback: %lu ok, %lu failed; %lu not queued)\n", (unsigned long)counter, (unsigned long)sent,
           (unsigned long)send_ok, (unsigned long)send_fail, (unsigned long)send_err);
  }
}

static int parse_mac(const char *s, uint8_t *m) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return -1;
  for (int i = 0; i < 6; i++)
    m[i] = (uint8_t)v[i];
  return 0;
}

static int cmd(int argc, char **argv) {
  const char *c = argv[0];
  if (!strcmp(c, "role") && argc == 2) {
    running = false;
    cfg.tx = !strcmp(argv[1], "tx");
    memset(&rx, 0, sizeof rx);
  } else if (!strcmp(c, "chan") && argc == 2) {
    cfg.chan = atoi(argv[1]);
    ESP_ERROR_CHECK(esp_wifi_set_channel((uint8_t)cfg.chan, WIFI_SECOND_CHAN_NONE));
  } else if (!strcmp(c, "mode") && argc == 2) {
    for (int i = 0; i < 4; i++)
      if (!strcmp(argv[1], kModes[i]))
        cfg.mode = i;
    if ((cfg.mode == MODE_UCAST || cfg.mode == MODE_ENC) && !cfg.have_peer)
      printf("set the receiver first: peer <mac> (its \"stats\" prints it)\n");
  } else if (!strcmp(c, "peer") && argc == 2) {
    if (parse_mac(argv[1], cfg.peer) == 0) {
      cfg.have_peer = true;
      if (!cfg.tx) { // the receiver: the sender as an encrypted peer, to decrypt "enc"
        esp_now_del_peer(cfg.peer);
        esp_now_peer_info_t p = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = true};
        memcpy(p.peer_addr, cfg.peer, 6);
        memcpy(p.lmk, kLmk, ESP_NOW_KEY_LEN);
        ESP_ERROR_CHECK(esp_now_add_peer(&p));
      }
    }
  } else if (!strcmp(c, "rate") && argc == 2) {
    for (int i = 0; i < (int)N_RATES; i++)
      if (!strcasecmp(argv[1], kRates[i].name))
        cfg.rate = i;
  } else if (!strcmp(c, "size") && argc == 2) {
    int s = atoi(argv[1]);
    uint32_t v = 0;
    esp_now_get_version(&v);
    const int max = v >= 2 ? ESP_NOW_MAX_DATA_LEN_V2 : ESP_NOW_MAX_DATA_LEN;
    cfg.size = s < 10 ? 10 : s > max ? max : s;
  } else if (!strcmp(c, "hz") && argc == 2) {
    int h = atoi(argv[1]);
    cfg.hz = h < 1 ? 1 : h > 500 ? 500 : h;
  } else if (!strcmp(c, "go")) {
    cfg.count = argc == 2 ? atoi(argv[1]) : 0;
    running = cfg.tx;
  } else if (!strcmp(c, "stop")) {
    running = false;
  } else if (!strcmp(c, "reset")) {
    counter = sent = send_ok = send_fail = send_err = 0;
    memset(&rx, 0, sizeof rx);
  } else if (!strcmp(c, "stats")) {
    uint8_t me[6];
    uint32_t v = 0;
    esp_wifi_get_mac(WIFI_IF_STA, me);
    esp_now_get_version(&v);
    printf("me " MACSTR " (ESP-NOW v%lu), %s, channel %d, mode %s, rate %s, %d bytes, %d Hz\n", MAC2STR(me), (unsigned long)v, cfg.tx ? "tx" : "rx",
           cfg.chan, kModes[cfg.mode], kRates[cfg.rate].name, cfg.size, cfg.hz);
    if (cfg.tx)
      printf("tx: counter %lu, sent %lu (callback: %lu ok, %lu failed; %lu not queued)\n", (unsigned long)counter, (unsigned long)sent,
             (unsigned long)send_ok, (unsigned long)send_fail, (unsigned long)send_err);
    else if (rx.any)
      printf("rx from " MACSTR ": %lu frames, counters %lu..%lu (%lu of %lu)\n", MAC2STR(rx.mac), (unsigned long)rx.n, (unsigned long)rx.lo,
             (unsigned long)rx.hi, (unsigned long)rx.n, (unsigned long)(rx.hi - rx.lo + 1));
    else
      printf("rx: nothing yet\n");
    return 0;
  } else {
    printf("?\n");
    return 1;
  }
  printf("ok\n");
  return 0;
}

void app_main(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    e = nvs_flash_init();
  }
  ESP_ERROR_CHECK(e);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wc));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel((uint8_t)cfg.chan, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
  xTaskCreate(sender, "sender", 6144, NULL, 5, NULL);

  esp_console_repl_t *repl = NULL;
  esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  rc.prompt = "espnow>";
  static const char *const kCmds[][2] = {
      {"role", "role tx|rx"},
      {"chan", "chan <1-13>"},
      {"mode", "mode bcast|ucast|absent|enc"},
      {"peer", "peer <mac>: the receiver (on the sender), the sender (on the receiver, for enc)"},
      {"rate", "rate 1L|2L|2S|11L|6M|MCS0"},
      {"size", "size <bytes> (10..250, or 1470 on ESP-NOW v2)"},
      {"hz", "hz <frames/s>"},
      {"go", "go [count]: send (count frames, or until stop)"},
      {"stop", "stop sending"},
      {"reset", "zero the counters"},
      {"stats", "this board's MAC, settings and counts"},
  };
  for (size_t i = 0; i < sizeof kCmds / sizeof kCmds[0]; i++) {
    const esp_console_cmd_t c = {.command = kCmds[i][0], .help = kCmds[i][1], .func = cmd};
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
  }
  ESP_ERROR_CHECK(esp_console_register_help_command());
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
  esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
  esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
#else
#error unsupported console
#endif
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
