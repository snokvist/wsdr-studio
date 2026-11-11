// SPDX-License-Identifier: GPL-2.0-only
// wsdr-zero's unit tests (no hardware, no screen): the UI's commands and picture against scripted
// events, the framebuffer copy, and the key map.
#include <linux/input-event-codes.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "platform.h"
#include "ui.h"

using namespace zero;

static int g_fail = 0;
#define CHECK(c)                                                                                   \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                     \
      g_fail++;                                                                                    \
    }                                                                                              \
  } while (0)

static KeyEvent ch(char c, bool shift = false) {
  KeyEvent k;
  k.key = Key::Char;
  k.ch = c;
  k.shift = shift;
  return k;
}
static KeyEvent key(Key k) {
  KeyEvent e;
  e.key = k;
  return e;
}

static json devices(bool can_open = true, bool five_only = false) {
  return {{"type", "devices"},
          {"devices", json::array({{{"pid", five_only ? 0xa81a : 0xc812}, {"model", five_only ? "RTL8812EU" : "RTL8812CU"},
                                    {"bus", 1}, {"port", "1.2"}, {"speed_mbps", 480}, {"can_open", can_open},
                                    {"max_msps", 5.0}, {"gapped_msps", 40.0},
                                    {"bands", five_only ? json::array({"5"}) : json::array({"2.4", "5"})}}})}};
}
static json caps_open() {
  json rates = json::array();
  for (int r = 5; r >= 1; r--)
    rates.push_back({{"rate_sel", r}, {"fs", 80e6 / (1 << r)}, {"gapped", r < 4}});
  return {{"type", "caps"}, {"open", true}, {"paths", 2}, {"rates", rates}, {"gain_max", 45.0}, {"gain_step", 1.0}};
}

struct Rig {
  std::vector<json> sent;
  Ui ui;
  double t = 0;
  explicit Rig(UiOptions o = {}) : ui([this](json c) { sent.push_back(std::move(c)); }, o) {}
  void tick() { ui.tick(t += 0.1); }
  const json *last(const char *cmd) const {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      if (it->value("cmd", "") == cmd)
        return &*it;
    return nullptr;
  }
  int count(const char *cmd) const {
    int n = 0;
    for (const auto &c : sent)
      n += c.value("cmd", "") == cmd;
    return n;
  }
  void open() { // a device shows up, the UI starts it, the engine says it is open
    tick();
    ui.on_event(devices());
    tick();
    ui.on_event(caps_open());
    ui.on_event({{"type", "ack"}, {"cmd", "open"}});
    tick();
  }
};

static void test_channels() {
  CHECK(channel_centre(1) == 2412e6);
  CHECK(channel_centre(14) == 2484e6);
  CHECK(channel_centre(36) == 5180e6);
  CHECK(channel_centre(15) == 0 && channel_centre(0) == 0 && channel_centre(234) == 0);
  CHECK(channel_name(5180e6) == "ch36");
  CHECK(channel_name(2437e6) == "ch6");
  CHECK(channel_name(2484e6) == "ch14");
  CHECK(channel_name(5181e6) == "" && channel_name(5180.5e6) == "");
}

static void test_autostart_and_set() {
  Rig r;
  r.tick();
  CHECK(r.count("scan") == 1 && r.count("dsp") == 1 && r.count("open") == 0);
  r.ui.on_event(devices());
  r.tick();
  const json *o = r.last("open");
  CHECK(o && (*o)["pid"] == 0xc812 && (*o)["bus"] == 1 && (*o)["port"] == "1.2" && (*o)["freq"] == 5180e6);
  CHECK(o && (*o)["rate_sel"] == 2 && (*o)["allow_gapped"] == true && (*o)["usb3"] == false); // 20 MHz: gapped on USB 2
  r.tick();
  r.tick();
  CHECK(r.count("open") == 1); // not sent again while it is opening
  r.ui.on_event(caps_open());
  r.ui.on_event({{"type", "ack"}, {"cmd", "open"}});
  r.tick();
  CHECK(r.ui.running() && r.count("set") == 0);

  // tuning: one "set" at a time, the latest values in the next one
  r.ui.on_key(key(Key::Right));
  r.tick();
  CHECK(r.count("set") == 1 && (*r.last("set"))["freq"] == 5185e6);
  r.ui.on_key(key(Key::Right));
  r.ui.on_key(ch('c'));       // C is the right arrow
  r.ui.on_key(ch('c', true)); // Shift: 20 MHz
  r.tick();
  CHECK(r.count("set") == 1); // the first is not answered yet
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});
  r.tick();
  CHECK(r.count("set") == 2 && (*r.last("set"))["freq"] == 5215e6);
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});

  // span: S goes to the next (40 MHz gapped), wraps to 2.5 MHz continuous
  r.ui.on_key(ch('s'));
  r.tick();
  CHECK((*r.last("set"))["rate_sel"] == 1 && (*r.last("set"))["allow_gapped"] == true);
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});
  r.ui.on_key(ch('s'));
  r.tick();
  CHECK((*r.last("set"))["rate_sel"] == 5 && (*r.last("set"))["allow_gapped"] == false && r.ui.span() == 2.5e6);
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});
  r.ui.on_key(ch('s', true)); // back
  r.tick();
  CHECK((*r.last("set"))["rate_sel"] == 1);
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});

  // gain and AGC
  r.ui.on_key(ch('f'));
  r.tick();
  CHECK((*r.last("set"))["gain_db"] == 32.0 && (*r.last("set"))["agc"] == false);
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});
  for (int i = 0; i < 20; i++)
    r.ui.on_key(key(Key::Up));
  r.tick();
  CHECK((*r.last("set"))["gain_db"] == 45.0); // the adapter's maximum
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});
  r.ui.on_key(ch('g'));
  r.tick();
  CHECK((*r.last("set"))["agc"] == true);
  r.ui.on_event({{"type", "ack"}, {"cmd", "set"}});

  // Space stops, and it stays stopped; Space starts it again
  r.ui.on_key(key(Key::Space));
  CHECK(r.count("close") == 1);
  r.ui.on_event({{"type", "caps"}, {"open", false}});
  r.ui.on_event({{"type", "ack"}, {"cmd", "close"}});
  for (int i = 0; i < 30; i++)
    r.tick();
  CHECK(r.count("open") == 1 && !r.ui.running());
  r.ui.on_key(key(Key::Space));
  CHECK(r.count("open") == 2 && (*r.last("open"))["agc"] == true && (*r.last("open"))["freq"] == 5215e6);
}

static void test_entry_and_bands() {
  Rig r;
  r.open();
  for (const char c : {'1', '4', '9'})
    r.ui.on_key(ch(c));
  r.ui.on_key(ch('s')); // letters do nothing while typing
  r.ui.on_key(key(Key::Enter));
  CHECK(r.ui.freq() == 5745e6);
  for (const char c : {'2', '4', '3', '7'})
    r.ui.on_key(ch(c));
  r.ui.on_key(key(Key::Enter));
  CHECK(r.ui.freq() == 2437e6);
  for (const char c : {'9', '9', '9', '9'})
    r.ui.on_key(ch(c));
  r.ui.on_key(key(Key::Enter));
  CHECK(r.ui.freq() == 2437e6); // refused
  r.ui.on_key(ch('7'));
  r.ui.on_key(key(Key::Esc)); // cancelled
  r.ui.on_key(key(Key::Enter)); // (Enter without digits is Start/Stop)
  CHECK(r.ui.freq() == 2437e6 && r.count("close") == 1);

  Rig b;
  b.open();
  b.ui.on_key(ch('b'));
  CHECK(b.ui.freq() == 2437e6);
  b.ui.on_key(key(Key::Left));
  CHECK(b.ui.freq() == 2432e6);
  for (int i = 0; i < 10; i++)
    b.ui.on_key(key(Key::Left));
  CHECK(b.ui.freq() == 2412e6); // the band's edge
  for (int i = 0; i < 12; i++)
    b.ui.on_key(key(Key::Right));
  CHECK(b.ui.freq() == 2472e6);
  b.ui.on_key(key(Key::Right));
  CHECK(b.ui.freq() == 2484e6); // channel 14
  b.ui.on_key(key(Key::Left));
  CHECK(b.ui.freq() == 2472e6);
  b.ui.on_key(ch('b'));
  CHECK(b.ui.freq() == 5180e6);
  b.ui.on_key(ch('z', true));
  CHECK(b.ui.freq() == 5160e6);
}

static void test_failures() {
  { // no permission: not started; started once the device can be opened
    Rig r;
    r.tick();
    r.ui.on_event(devices(false));
    r.tick();
    CHECK(r.count("open") == 0);
    r.ui.on_event(devices(true));
    r.tick();
    CHECK(r.count("open") == 1);
    // a failed bring-up is not retried by itself
    r.ui.on_event({{"type", "error"}, {"cmd", "open"}, {"msg", "bring-up failed"}});
    for (int i = 0; i < 40; i++)
      r.tick();
    CHECK(r.count("open") == 1);
  }
  { // unplugged: started again when it is back
    Rig r;
    r.open();
    r.ui.on_event({{"type", "caps"}, {"open", false}});
    r.ui.on_event({{"type", "alert"}, {"kind", "lost"}, {"msg", "Adapter disconnected."}});
    r.ui.on_event({{"type", "devices"}, {"devices", json::array()}});
    for (int i = 0; i < 30; i++)
      r.tick();
    CHECK(r.count("open") == 1 && r.count("scan") >= 2); // it keeps looking
    r.ui.on_event(devices());
    r.tick();
    CHECK(r.count("open") == 2);
  }
  { // a 5 GHz-only adapter is not tuned to 2.4 GHz
    UiOptions o;
    o.freq = 2437e6;
    Rig r(o);
    r.tick();
    r.ui.on_event(devices(true, true));
    r.tick();
    CHECK(r.last("open") && (*r.last("open"))["freq"] == 5180e6);
    r.ui.on_key(ch('b'));
    CHECK(r.ui.freq() == 5180e6);
  }
  { // --no-autostart
    UiOptions o;
    o.autostart = false;
    Rig r(o);
    r.tick();
    r.ui.on_event(devices());
    for (int i = 0; i < 10; i++)
      r.tick();
    CHECK(r.count("open") == 0);
  }
}

static uint32_t at(const Canvas &c, int x, int y) { return c.px[static_cast<size_t>(y) * kW + x]; }

static void test_picture() {
  Rig r;
  r.open();
  Canvas blank;
  r.ui.draw(blank);
  // a row with one strong signal in the upper half of a 20 MHz band
  SpecRow row;
  row.f0 = 5170e6;
  row.df = 20e6 / 1024;
  row.avg.assign(1024, -95.f);
  row.max.assign(1024, -90.f);
  for (int i = 700; i < 720; i++)
    row.avg[i] = row.max[i] = -50.f;
  for (int i = 0; i < 5; i++)
    r.ui.on_row(row);
  Canvas c;
  r.ui.draw(c);
  const int xs = 710 * kW / 1024; // the signal's column
  // spectrum: the trace is high at the signal and low elsewhere (the plot is rows 12..75)
  int top_sig = -1, top_noise = -1;
  for (int y = 12; y < 76 && top_sig < 0; y++)
    if (at(c, xs, y) == 0x4fc3f7)
      top_sig = y;
  for (int y = 12; y < 76 && top_noise < 0; y++)
    if (at(c, 100, y) == 0x4fc3f7)
      top_noise = y;
  CHECK(top_sig >= 12 && top_noise > top_sig + 30);
  // waterfall: the newest rows (from y = 87) are brighter at the signal
  const auto lum = [&](int x, int y) { return (at(c, x, y) >> 16 & 255) + (at(c, x, y) >> 8 & 255) + (at(c, x, y) & 255); };
  CHECK(lum(xs, 88) > lum(100, 88) + 100);
  CHECK(lum(xs, 87 + 20) == lum(100, 87 + 20)); // older rows: nothing yet
  // another band: the picture starts again
  row.f0 = 5190e6;
  r.ui.on_row(row);
  r.ui.draw(c);
  CHECK(lum(xs, 89) == lum(100, 89));
  // help covers the screen; any key leaves it without acting
  const int sets = r.count("set");
  r.ui.on_key(ch('h'));
  r.ui.draw(c);
  CHECK(lum(xs, 88) < 60);
  r.ui.on_key(ch('c'));
  r.tick();
  CHECK(r.count("set") == sets && r.ui.freq() == 5180e6);
}

static void test_blit() {
  Canvas c;
  c.put(0, 0, 0xff0000);            // top left: red
  c.put(kW - 1, 0, 0x00ff00);       // top right: green
  c.put(0, kH - 1, 0x0000ff);       // bottom left: blue
  c.put(kW - 1, kH - 1, 0xffffff);  // bottom right: white
  { // RGB565, the panel's own size
    PixelFormat f;
    f.w = kW, f.h = kH, f.stride = kW * 2;
    std::vector<uint8_t> b(static_cast<size_t>(f.stride) * f.h, 0xaa);
    blit(c, b.data(), f);
    const auto px = [&](int x, int y) { uint16_t v; std::memcpy(&v, &b[static_cast<size_t>(y) * f.stride + 2 * x], 2); return v; };
    CHECK(px(0, 0) == 0xf800 && px(kW - 1, 0) == 0x07e0 && px(0, kH - 1) == 0x001f && px(kW - 1, kH - 1) == 0xffff);
    CHECK(px(5, 5) == 0);
  }
  { // 32 bpp XRGB, twice the size plus a margin: doubled and centred, the margin untouched
    PixelFormat f;
    f.w = 2 * kW + 10, f.h = 2 * kH + 6, f.bpp = 32, f.stride = f.w * 4 + 8;
    f.r_off = 16, f.g_off = 8, f.b_off = 0, f.r_len = f.g_len = f.b_len = 8;
    std::vector<uint8_t> b(static_cast<size_t>(f.stride) * f.h, 0xaa);
    blit(c, b.data(), f);
    const auto px = [&](int x, int y) { uint32_t v; std::memcpy(&v, &b[static_cast<size_t>(y) * f.stride + 4 * x], 4); return v; };
    CHECK(px(5, 3) == 0xff0000 && px(6, 4) == 0xff0000 && px(7, 3) == 0);
    CHECK(px(5 + 2 * kW - 1, 3 + 2 * kH - 1) == 0xffffff);
    CHECK(px(4, 3) == 0xaaaaaaaa && px(5, 2) == 0xaaaaaaaa && px(5 + 2 * kW, 3) == 0xaaaaaaaa);
  }
  { // a portrait buffer: turned clockwise (the canvas's top left ends up top right)
    PixelFormat f;
    f.w = kH, f.h = kW, f.stride = kH * 2;
    std::vector<uint8_t> b(static_cast<size_t>(f.stride) * f.h, 0);
    blit(c, b.data(), f);
    const auto px = [&](int x, int y) { uint16_t v; std::memcpy(&v, &b[static_cast<size_t>(y) * f.stride + 2 * x], 2); return v; };
    CHECK(px(kH - 1, 0) == 0xf800 && px(kH - 1, kW - 1) == 0x07e0 && px(0, 0) == 0x001f && px(0, kW - 1) == 0xffff);
    blit(c, b.data(), f, 270); // the other way round
    CHECK(px(0, kW - 1) == 0xf800 && px(0, 0) == 0x07e0 && px(kH - 1, kW - 1) == 0x001f && px(kH - 1, 0) == 0xffff);
  }
  { // a buffer smaller than the picture: cropped, nothing written outside it
    PixelFormat f;
    f.w = 100, f.h = 50, f.stride = 200;
    std::vector<uint8_t> b(static_cast<size_t>(f.stride) * f.h + 16, 0x55);
    blit(c, b.data(), f, 0);
    CHECK(b[0] == 0x00 && b[1] == 0xf8);
    for (size_t i = static_cast<size_t>(f.stride) * f.h; i < b.size(); i++)
      CHECK(b[i] == 0x55);
  }
}

static void test_keys() {
  KeyEvent k;
  CHECK(map_key(KEY_Z, false, k) && k.key == Key::Char && k.ch == 'z' && !k.shift);
  CHECK(map_key(KEY_S, true, k) && k.ch == 's' && k.shift);
  CHECK(map_key(KEY_LEFT, false, k) && k.key == Key::Left);
  CHECK(map_key(KEY_ENTER, false, k) && k.key == Key::Enter);
  CHECK(map_key(KEY_ESC, false, k) && k.key == Key::Esc);
  CHECK(map_key(KEY_0, false, k) && k.ch == '0');
  CHECK(map_key(KEY_HELP, false, k) && k.ch == 'h'); // Fn+H on the CardputerZero
  CHECK(!map_key(KEY_F1, false, k) && !map_key(KEY_VOLUMEUP, false, k));
}

static void test_text() {
  Canvas c;
  const int x = c.text(10, 10, "A", 0xffffff);
  CHECK(x == 10 + kCharW);
  // 'A' in the 5 x 7 font: the crossbar is row 4 (bit 4 of every column), the top row has the middle 3
  CHECK(at(c, 10, 10) == 0 && at(c, 11, 10) == 0xffffff && at(c, 13, 10) == 0xffffff && at(c, 14, 10) == 0);
  for (int i = 0; i < 5; i++)
    CHECK(at(c, 10 + i, 14) == 0xffffff);
  CHECK(at(c, 10, 16) == 0xffffff && at(c, 12, 16) == 0 && at(c, 14, 16) == 0xffffff);
  c.text(kW - 3, kH - 3, "W", 0xffffff); // clipped at the edges, no crash
  c.text(-3, -3, "W", 0xffffff);
}

int main() {
  test_channels();
  test_autostart_and_set();
  test_entry_and_bands();
  test_failures();
  test_picture();
  test_blit();
  test_keys();
  test_text();
  std::printf(g_fail ? "%d FAILED\n" : "test_zero: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
