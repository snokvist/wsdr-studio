// SPDX-License-Identifier: GPL-2.0-only
#include "platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>

namespace zero {

void blit(const Canvas &c, uint8_t *dst, const PixelFormat &f, int rotate) {
  if (rotate < 0)
    rotate = f.w < kW && f.h >= kW ? 90 : 0;
  const bool turned = rotate == 90 || rotate == 270;
  const int sw = turned ? kH : kW, sh = turned ? kW : kH; // the picture's size as the buffer sees it
  const int k = std::max(1, std::min(f.w / sw, f.h / sh));
  const int ox = std::max(0, (f.w - sw * k) / 2), oy = std::max(0, (f.h - sh * k) / 2);
  const int bytes = f.bpp / 8;
  const auto pack = [&](uint32_t rgb) -> uint32_t {
    const uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    return (r >> (8 - f.r_len)) << f.r_off | (g >> (8 - f.g_len)) << f.g_off | (b >> (8 - f.b_len)) << f.b_off;
  };
  const int xs = std::min(sw * k, f.w - ox), ys = std::min(sh * k, f.h - oy);
  for (int y = 0; y < ys; y++) {
    uint8_t *line = dst + static_cast<size_t>(oy + y) * f.stride + static_cast<size_t>(ox) * bytes;
    const int v = y / k;
    for (int x = 0; x < xs; x++) {
      const int u = x / k;
      // (u, v) in the buffer's picture -> the canvas pixel there
      const int cx = rotate == 90 ? v : rotate == 270 ? kW - 1 - v : u;
      const int cy = rotate == 90 ? kH - 1 - u : rotate == 270 ? u : v;
      const uint32_t p = pack(c.px[static_cast<size_t>(cy) * kW + cx]);
      if (bytes == 2) {
        const uint16_t q = static_cast<uint16_t>(p);
        std::memcpy(line + 2 * x, &q, 2);
      } else if (bytes == 4) {
        std::memcpy(line + 4 * x, &p, 4);
      } else if (bytes == 3) {
        line[3 * x] = p & 255, line[3 * x + 1] = p >> 8 & 255, line[3 * x + 2] = p >> 16 & 255;
      }
    }
  }
}

namespace {

class FbDisplay : public Display {
public:
  FbDisplay(int fd, std::string dev, PixelFormat f, uint8_t *map, size_t len, int rotate)
      : _fd(fd), _dev(std::move(dev)), _f(f), _map(map), _len(len), _rotate(rotate), _buf(len, 0) {}
  ~FbDisplay() override {
    std::fill(_buf.begin(), _buf.end(), 0); // leave a black screen for the launcher to draw on
    flush();
    if (_map)
      munmap(_map, _len);
    close(_fd);
  }
  void present(const Canvas &c) override {
    blit(c, _buf.data(), _f, _rotate);
    flush();
  }
  std::string describe() const override {
    return _dev + " " + std::to_string(_f.w) + "x" + std::to_string(_f.h) + " " + std::to_string(_f.bpp) + " bpp" +
           (_map ? "" : " (no mmap: write)");
  }

private:
  void flush() {
    if (_map)
      std::memcpy(_map, _buf.data(), _len);
    else if (pwrite(_fd, _buf.data(), _len, 0) < 0) {
    }
  }
  int _fd;
  std::string _dev;
  PixelFormat _f;
  uint8_t *_map;
  size_t _len;
  int _rotate;
  std::vector<uint8_t> _buf; // drawn here, then copied in one go (no half-drawn frames on the panel)
};

// "/dev/fbN" of the ST7789 panel when /proc/fb lists one ("1 fb_st7789v"), else /dev/fb0
std::string find_fb() {
  std::ifstream f("/proc/fb");
  int n;
  std::string name;
  while (f >> n && std::getline(f, name)) {
    std::string low = name;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (low.find("st7789") != std::string::npos || low.find("mipi_dbi") != std::string::npos ||
        low.find("mipi-dbi") != std::string::npos)
      return "/dev/fb" + std::to_string(n);
  }
  return "/dev/fb0";
}

double mono() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool has_bit(const unsigned long *bits, int n) { return bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long))) & 1; }

class EvdevInput : public Input {
public:
  EvdevInput(std::vector<int> fds, std::string names) : _fds(std::move(fds)), _names(std::move(names)) {}
  ~EvdevInput() override {
    for (const int fd : _fds)
      close(fd);
  }
  void poll(std::vector<KeyEvent> &out) override {
    for (const int fd : _fds) {
      input_event ev[32];
      for (;;) {
        const ssize_t n = read(fd, ev, sizeof ev);
        if (n < static_cast<ssize_t>(sizeof(input_event)))
          break;
        for (size_t i = 0; i < n / sizeof(input_event); i++)
          key(ev[i], out);
      }
    }
    // a held tuning or gain key repeats (done here: not every keypad driver repeats by itself)
    const double t = mono();
    if (_held >= 0 && t >= _next_repeat) {
      KeyEvent k;
      if (map_key(_held, _shift, k)) {
        k.repeat = true;
        out.push_back(k);
      }
      _next_repeat = t + 0.12;
    }
  }
  bool esc_held() const override { return _esc; }
  std::string describe() const override { return _names; }

private:
  void key(const input_event &e, std::vector<KeyEvent> &out) {
    if (e.type != EV_KEY || e.value == 2) // (the kernel's own repeats: see poll)
      return;
    const bool down = e.value == 1;
    if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT) {
      _shift = down;
      return;
    }
    if (e.code == KEY_ESC)
      _esc = down;
    if (!down) {
      if (e.code == _held)
        _held = -1;
      return;
    }
    KeyEvent k;
    if (!map_key(e.code, _shift, k))
      return;
    out.push_back(k);
    const bool repeats = k.key == Key::Left || k.key == Key::Right || k.key == Key::Up || k.key == Key::Down ||
                         (k.key == Key::Char && (k.ch == 'z' || k.ch == 'c' || k.ch == 'f' || k.ch == 'x'));
    _held = repeats ? e.code : -1;
    _next_repeat = mono() + 0.45;
  }
  std::vector<int> _fds;
  std::string _names;
  bool _shift = false, _esc = false;
  int _held = -1;
  double _next_repeat = 0;
};

} // namespace

bool map_key(int code, bool shift, KeyEvent &out) {
  out = KeyEvent{};
  out.shift = shift;
  switch (code) {
  case KEY_LEFT: out.key = Key::Left; return true;
  case KEY_RIGHT: out.key = Key::Right; return true;
  case KEY_UP: out.key = Key::Up; return true;
  case KEY_DOWN: out.key = Key::Down; return true;
  case KEY_ENTER:
  case KEY_KPENTER: out.key = Key::Enter; return true;
  case KEY_ESC: out.key = Key::Esc; return true;
  case KEY_SPACE: out.key = Key::Space; return true;
  case KEY_BACKSPACE:
  case KEY_DELETE: out.key = Key::Backspace; return true;
  default: break;
  }
  static const struct {
    int code;
    char ch;
  } chars[] = {
      {KEY_1, '1'}, {KEY_2, '2'}, {KEY_3, '3'}, {KEY_4, '4'}, {KEY_5, '5'}, {KEY_6, '6'}, {KEY_7, '7'},
      {KEY_8, '8'}, {KEY_9, '9'}, {KEY_0, '0'}, {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'},
      {KEY_T, 't'}, {KEY_Y, 'y'}, {KEY_U, 'u'}, {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_A, 'a'},
      {KEY_S, 's'}, {KEY_D, 'd'}, {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'},
      {KEY_L, 'l'}, {KEY_Z, 'z'}, {KEY_X, 'x'}, {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'}, {KEY_N, 'n'},
      {KEY_M, 'm'}, {KEY_DOT, '.'}, {KEY_KPDOT, '.'}, {KEY_SLASH, '/'}, {KEY_HELP, 'h'},
      {KEY_KP0, '0'}, {KEY_KP1, '1'}, {KEY_KP2, '2'}, {KEY_KP3, '3'}, {KEY_KP4, '4'}, {KEY_KP5, '5'},
      {KEY_KP6, '6'}, {KEY_KP7, '7'}, {KEY_KP8, '8'}, {KEY_KP9, '9'},
  };
  for (const auto &c : chars)
    if (c.code == code) {
      out.key = Key::Char;
      out.ch = c.ch;
      return true;
    }
  return false;
}

std::unique_ptr<Display> open_fbdev(const std::string &device, int rotate, std::string &err) {
  const std::string dev = device.empty() ? find_fb() : device;
  const int fd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    err = "cannot open " + dev + ": " + std::strerror(errno);
    return nullptr;
  }
  fb_var_screeninfo v{};
  fb_fix_screeninfo x{};
  if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &x) < 0) {
    err = dev + " is no framebuffer: " + std::strerror(errno);
    close(fd);
    return nullptr;
  }
  if (v.bits_per_pixel != 16 && v.bits_per_pixel != 24 && v.bits_per_pixel != 32) {
    err = dev + ": " + std::to_string(v.bits_per_pixel) + " bits per pixel is not supported";
    close(fd);
    return nullptr;
  }
  PixelFormat f;
  f.w = static_cast<int>(v.xres);
  f.h = static_cast<int>(v.yres);
  f.bpp = static_cast<int>(v.bits_per_pixel);
  f.stride = x.line_length ? static_cast<int>(x.line_length) : f.w * f.bpp / 8;
  f.r_off = v.red.offset, f.r_len = v.red.length;
  f.g_off = v.green.offset, f.g_len = v.green.length;
  f.b_off = v.blue.offset, f.b_len = v.blue.length;
  if (f.r_len <= 0 || f.r_len > 8 || f.g_len <= 0 || f.g_len > 8 || f.b_len <= 0 || f.b_len > 8) {
    err = dev + ": unexpected colour layout";
    close(fd);
    return nullptr;
  }
  const size_t len = static_cast<size_t>(f.stride) * f.h;
  void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  return std::make_unique<FbDisplay>(fd, dev, f, m == MAP_FAILED ? nullptr : static_cast<uint8_t *>(m), len, rotate);
}

std::unique_ptr<Input> open_evdev(const std::string &device, std::string &err) {
  std::vector<int> fds;
  std::string names;
  const auto add = [&](const std::string &path, bool check) {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      return;
    if (check) { // a keyboard: it has Enter and the letter Z
      unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = {};
      if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 || !has_bit(bits, KEY_ENTER) || !has_bit(bits, KEY_Z)) {
        close(fd);
        return;
      }
    }
    fds.push_back(fd);
    names += (names.empty() ? "" : ", ") + path;
  };
  if (!device.empty()) {
    add(device, false);
    if (fds.empty())
      err = "cannot open " + device + ": " + std::strerror(errno);
  } else {
    if (DIR *d = opendir("/dev/input")) {
      while (const dirent *e = readdir(d))
        if (std::strncmp(e->d_name, "event", 5) == 0)
          add(std::string("/dev/input/") + e->d_name, true);
      closedir(d);
    }
    if (fds.empty())
      err = "no keyboard under /dev/input (is this user in the 'input' group?)";
  }
  if (fds.empty())
    return nullptr;
  return std::make_unique<EvdevInput>(std::move(fds), names);
}

} // namespace zero
