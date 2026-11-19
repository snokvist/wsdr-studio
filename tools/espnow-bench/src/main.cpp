// SPDX-License-Identifier: GPL-2.0-only
// ESP-NOW bench for wsdr-studio: a sender of numbered frames and a receiver that counts them, driven
// by line commands on the serial console (115200; "help"). Payload: "WSDR", u32 counter
// (little-endian), u8 mode, u8 rate index, then filler bytes (index & 0xff) to the set size.
// wsdr-studio's `test_core --wifi <recording>` reads the counters back, so what it missed is exact.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace {

enum Mode { MODE_BCAST, MODE_UCAST, MODE_ABSENT, MODE_ENC };
const char *const kModes[] = {"bcast", "ucast", "absent", "enc"};
struct Rate {
  const char *name;
  wifi_phy_rate_t rate;
};
const Rate kRates[] = {
    {"1L", WIFI_PHY_RATE_1M_L}, // ESP-NOW's default
    {"2L", WIFI_PHY_RATE_2M_L}, {"2S", WIFI_PHY_RATE_2M_S}, {"11L", WIFI_PHY_RATE_11M_L},
    {"6M", WIFI_PHY_RATE_6M},   {"MCS0", WIFI_PHY_RATE_MCS0_LGI},
};
constexpr int kNRates = sizeof kRates / sizeof kRates[0];
#ifdef ESP_NOW_MAX_DATA_LEN_V2
constexpr int kMaxSize = ESP_NOW_MAX_DATA_LEN_V2; // (v2 sends it when the peer is v2 too)
#else
constexpr int kMaxSize = ESP_NOW_MAX_DATA_LEN;
#endif

const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t kAbsent[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}; // nobody: every frame retried
const uint8_t kLmk[ESP_NOW_KEY_LEN] = {'w', 's', 'd', 'r', '-', 'b', 'e', 'n', 'c', 'h', '-', 'l', 'm', 'k', '!', '!'};

bool tx = true;
volatile bool running = false; // (set by the console loop, read by the sender task)
int chan = 6, mode = MODE_BCAST, rate = 0, size = 32, hz = 20, count = 0;
uint8_t peer[6];
bool have_peer = false;

volatile uint32_t sent, send_ok, send_fail, send_err;
uint32_t counter;

struct {
  uint8_t mac[6];
  uint32_t n, lo, hi;
  bool any;
} rx; // the receiver's count of one sender's frames

void apply_rate(const uint8_t *to) {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_now_rate_config_t rc = {};
  rc.phymode = kRates[rate].rate == WIFI_PHY_RATE_MCS0_LGI ? WIFI_PHY_MODE_HT20 : WIFI_PHY_MODE_11G;
  rc.rate = kRates[rate].rate;
  const esp_err_t e = esp_now_set_peer_rate_config(to, &rc);
#else
  (void)to;
  const esp_err_t e = esp_wifi_config_espnow_rate(WIFI_IF_STA, kRates[rate].rate);
#endif
  if (e != ESP_OK)
    Serial.printf("rate: %s\n", esp_err_to_name(e));
}

void add_peer(const uint8_t *mac, bool encrypt) {
  esp_now_del_peer(mac);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.ifidx = WIFI_IF_STA;
  p.encrypt = encrypt;
  if (encrypt)
    memcpy(p.lmk, kLmk, ESP_NOW_KEY_LEN);
  const esp_err_t e = esp_now_add_peer(&p);
  if (e != ESP_OK)
    Serial.printf("add peer: %s\n", esp_err_to_name(e));
}

#if ESP_IDF_VERSION_MAJOR >= 5 && ESP_IDF_VERSION_MINOR >= 5
void on_sent(const esp_now_send_info_t *, esp_now_send_status_t status) {
#else
void on_sent(const uint8_t *, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS)
    send_ok++;
  else
    send_fail++;
}

void count_rx(const uint8_t *src, const uint8_t *d, int n) {
  if (tx || n < 8 || memcmp(d, "WSDR", 4) != 0)
    return;
  const uint32_t c = d[4] | d[5] << 8 | d[6] << 16 | static_cast<uint32_t>(d[7]) << 24;
  if (!rx.any || memcmp(rx.mac, src, 6) != 0) { // (a new sender: start again)
    memcpy(rx.mac, src, 6);
    rx.n = 0;
    rx.lo = rx.hi = c;
    rx.any = true;
  }
  rx.n++;
  rx.lo = min(rx.lo, c);
  rx.hi = max(rx.hi, c);
}
#if ESP_IDF_VERSION_MAJOR >= 5
void on_recv(const esp_now_recv_info_t *info, const uint8_t *d, int n) { count_rx(info->src_addr, d, n); }
#else
void on_recv(const uint8_t *src, const uint8_t *d, int n) { count_rx(src, d, n); }
#endif

void sender(void *) {
  static uint8_t buf[kMaxSize];
  for (;;) {
    if (!tx || !running) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const uint8_t *to = mode == MODE_BCAST ? kBroadcast : mode == MODE_ABSENT ? kAbsent : peer;
    add_peer(to, mode == MODE_ENC);
    apply_rate(to);
    Serial.printf("sending: %s to %02x:%02x:%02x:%02x:%02x:%02x, rate %s, %d bytes, %d Hz, channel %d, %s\n", kModes[mode], to[0], to[1], to[2],
                  to[3], to[4], to[5], kRates[rate].name, size, hz, chan, count ? "a count" : "until stop");
    int64_t next = esp_timer_get_time();
    int left = count;
    while (running && tx) {
      memcpy(buf, "WSDR", 4);
      memcpy(buf + 4, &counter, 4);
      buf[8] = static_cast<uint8_t>(mode);
      buf[9] = static_cast<uint8_t>(rate);
      for (int i = 10; i < size; i++)
        buf[i] = static_cast<uint8_t>(i);
      if (esp_now_send(to, buf, size) == ESP_OK) {
        counter++;
        sent++;
      } else {
        send_err++;
      }
      if (count && --left <= 0)
        running = false;
      next += 1000000 / hz;
      const int64_t wait = next - esp_timer_get_time();
      vTaskDelay(wait > 1000 ? pdMS_TO_TICKS(wait / 1000) : 1);
    }
    Serial.printf("stopped: counter %lu, sent %lu (acked/ok %lu, failed %lu, not queued %lu)\n", (unsigned long)counter, (unsigned long)sent,
                  (unsigned long)send_ok, (unsigned long)send_fail, (unsigned long)send_err);
  }
}

bool parse_mac(const String &s, uint8_t *m) {
  unsigned v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++)
    m[i] = static_cast<uint8_t>(v[i]);
  return true;
}

void set_channel(int c) {
  esp_wifi_set_promiscuous(true); // (the channel can be set while not connected this way)
  esp_wifi_set_channel(static_cast<uint8_t>(c), WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

void stats() {
  uint32_t v = 1;
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_now_get_version(&v);
#endif
  Serial.printf("me %s (ESP-NOW v%lu), %s, channel %d, mode %s, rate %s, %d bytes, %d Hz\n", WiFi.macAddress().c_str(), (unsigned long)v,
                tx ? "tx" : "rx", chan, kModes[mode], kRates[rate].name, size, hz);
  if (tx)
    Serial.printf("tx: counter %lu, sent %lu (acked/ok %lu, failed %lu, not queued %lu)%s\n", (unsigned long)counter, (unsigned long)sent,
                  (unsigned long)send_ok, (unsigned long)send_fail, (unsigned long)send_err, running ? ", sending" : "");
  else if (rx.any)
    Serial.printf("rx from %02x:%02x:%02x:%02x:%02x:%02x: %lu frames, counters %lu..%lu (%lu of %lu)\n", rx.mac[0], rx.mac[1], rx.mac[2],
                  rx.mac[3], rx.mac[4], rx.mac[5], (unsigned long)rx.n, (unsigned long)rx.lo, (unsigned long)rx.hi, (unsigned long)rx.n,
                  (unsigned long)(rx.hi - rx.lo + 1));
  else
    Serial.println("rx: nothing yet");
}

void command(String line) {
  line.trim();
  const int sp = line.indexOf(' ');
  const String c = sp < 0 ? line : line.substring(0, sp), a = sp < 0 ? String() : line.substring(sp + 1);
  if (c == "role" && a.length()) {
    running = false;
    tx = a == "tx";
    memset(&rx, 0, sizeof rx);
  } else if (c == "chan" && a.length()) {
    chan = constrain(a.toInt(), 1, 13);
    set_channel(chan);
  } else if (c == "mode" && a.length()) {
    for (int i = 0; i < 4; i++)
      if (a == kModes[i])
        mode = i;
    if ((mode == MODE_UCAST || mode == MODE_ENC) && !have_peer)
      Serial.println("set the receiver first: peer <mac> (its \"stats\" prints it)");
  } else if (c == "peer" && parse_mac(a, peer)) {
    have_peer = true;
    if (!tx)
      add_peer(peer, true); // the receiver: the sender, to decrypt "enc"
  } else if (c == "rate" && a.length()) {
    for (int i = 0; i < kNRates; i++)
      if (a.equalsIgnoreCase(kRates[i].name))
        rate = i;
  } else if (c == "size" && a.length()) {
    size = constrain(static_cast<int>(a.toInt()), 10, kMaxSize);
  } else if (c == "hz" && a.length()) {
    hz = constrain(static_cast<int>(a.toInt()), 1, 500);
  } else if (c == "go") {
    count = a.length() ? a.toInt() : 0;
    running = tx;
  } else if (c == "stop") {
    running = false;
  } else if (c == "reset") {
    counter = sent = send_ok = send_fail = send_err = 0;
    memset(&rx, 0, sizeof rx);
  } else if (c == "stats") {
    stats();
    return;
  } else if (c == "help" || c.isEmpty()) {
    Serial.println("role tx|rx · chan N · mode bcast|ucast|absent|enc · peer MAC · rate 1L|2L|2S|11L|6M|MCS0 · size N · hz N · go [N] · stop · "
                   "reset · stats");
    return;
  } else {
    Serial.println("?");
    return;
  }
  Serial.println("ok");
}

} // namespace

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  set_channel(chan);
  if (esp_now_init() != ESP_OK)
    Serial.println("esp_now_init failed");
  esp_now_register_send_cb(on_sent);
  esp_now_register_recv_cb(on_recv);
  xTaskCreate(sender, "sender", 6144, nullptr, 5, nullptr);
  delay(500);
  Serial.println("espnow-bench ready (help)");
  stats();
}

void loop() {
  static String line;
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\n' || ch == '\r') {
      if (line.length())
        command(line);
      line = "";
    } else if (line.length() < 80) {
      line += ch;
    }
  }
  delay(5);
}
