// SPDX-License-Identifier: GPL-2.0-only
// wsdr-zero — wsdr-studio for the M5Stack CardputerZero (and other small Linux handhelds): the same
// receive engine (one adapter, spectrum rows), shown as a spectrum and waterfall on a 320 x 170
// framebuffer and driven from the keyboard. No Wi-Fi frame analysis: it needs 20 or 40 MS/s
// continuous, which USB 2 does not carry.
//
//   wsdr-zero [--fb /dev/fb0] [--rotate 0|90|270] [--input /dev/input/eventN]
//             [--freq MHz] [--span MHz] [--no-autostart] [--record-dir DIR]
//             [--sdl [SCALE]]   a desktop window instead of the framebuffer (builds with SDL2)
//             [--demo]          a simulated adapter (no dongle needed)
//   tests:    [--script KEYS] [--shot FILE.ppm]   KEYS: comma-separated: a key (s, S = Shift+s, left,
//             right, up, down, enter, esc, space, bksp), wait:SECONDS, shot:FILE.ppm, quit
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "backend.h"
#include "canvas.h"
#include "platform.h"
#include "ui.h"

using namespace zero;

namespace {

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

double mono() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool write_ppm(const Canvas &c, const std::string &path) {
  std::ofstream f(path, std::ios::binary);
  f << "P6\n" << kW << " " << kH << "\n255\n";
  for (const uint32_t p : c.px) {
    const char rgb[3] = {static_cast<char>(p >> 16), static_cast<char>(p >> 8), static_cast<char>(p)};
    f.write(rgb, 3);
  }
  return static_cast<bool>(f);
}

// --script: keys and waits played back in place of a keyboard
class Script {
public:
  explicit Script(const std::string &s) {
    size_t i = 0;
    while (i <= s.size()) {
      size_t j = s.find(',', i);
      if (j == std::string::npos)
        j = s.size();
      if (j > i)
        _steps.push_back(s.substr(i, j - i));
      i = j + 1;
    }
  }
  bool active() const { return !_steps.empty(); }
  // Runs the steps that are due; false once "quit" is reached.
  bool run(double now, Ui &ui, const Canvas &c) {
    while (!_steps.empty() && now >= _wait_until) {
      const std::string t = _steps.front();
      _steps.pop_front();
      KeyEvent k;
      if (t == "quit")
        return false;
      if (t.rfind("wait:", 0) == 0) {
        _wait_until = now + std::atof(t.c_str() + 5);
        continue;
      }
      if (t.rfind("shot:", 0) == 0) {
        _shot = t.substr(5); // taken after the next frame is drawn
        return true;
      }
      if (t == "left") k.key = Key::Left;
      else if (t == "right") k.key = Key::Right;
      else if (t == "up") k.key = Key::Up;
      else if (t == "down") k.key = Key::Down;
      else if (t == "enter") k.key = Key::Enter;
      else if (t == "esc") k.key = Key::Esc;
      else if (t == "space") k.key = Key::Space;
      else if (t == "bksp") k.key = Key::Backspace;
      else if (t.size() == 1) {
        k.key = Key::Char;
        k.shift = t[0] >= 'A' && t[0] <= 'Z';
        k.ch = static_cast<char>(k.shift ? t[0] - 'A' + 'a' : t[0]);
      } else {
        std::fprintf(stderr, "wsdr-zero: --script: unknown step '%s'\n", t.c_str());
        continue;
      }
      ui.on_key(k);
    }
    (void)c;
    return true;
  }
  void after_draw(const Canvas &c) {
    if (_shot.empty())
      return;
    if (!write_ppm(c, _shot))
      std::fprintf(stderr, "wsdr-zero: cannot write %s\n", _shot.c_str());
    _shot.clear();
  }

private:
  std::deque<std::string> _steps;
  double _wait_until = 0;
  std::string _shot;
};

std::string data_dir() {
  const char *x = std::getenv("XDG_DATA_HOME");
  const char *h = std::getenv("HOME");
  return x && *x ? std::string(x) + "/wsdr-studio" : std::string(h ? h : "/tmp") + "/.local/share/wsdr-studio";
}

void usage() {
  std::puts("wsdr-zero " WSDR_ZERO_VERSION " - spectrum and waterfall from an RTL8812BU/CU/EU Wi-Fi dongle, on a small screen\n"
            "  --fb DEV          framebuffer (default: the ST7789 panel, else /dev/fb0; or $LV_LINUX_FBDEV_DEVICE)\n"
            "  --rotate 0|90|270 turn the picture (default: by the framebuffer's shape)\n"
            "  --input DEV       keyboard event device (default: every keyboard; or $LV_LINUX_KEYBOARD_DEVICE)\n"
            "  --freq MHZ        start frequency (default 5180)\n"
            "  --span MHZ        start span: 2.5, 5, or gapped 10, 20, 40, 80 (default 20)\n"
            "  --no-autostart    wait for Space before starting the adapter\n"
            "  --record-dir DIR  where R saves IQ (default ~/.local/share/wsdr-studio/recordings)\n"
            "  --sdl [SCALE]     show a desktop window instead (default scale 3)\n"
            "  --demo            a simulated adapter\n"
            "  --script KEYS, --shot FILE.ppm   scripted keys and screenshots (tests)");
}

} // namespace

int main(int argc, char **argv) {
  std::string fb, input, script_s, shot, record_dir;
  int rotate = -1, sdl = 0;
  bool demo = false;
  UiOptions uo;
  if (const char *e = std::getenv("LV_LINUX_FBDEV_DEVICE"))
    fb = e;
  if (const char *e = std::getenv("LV_LINUX_KEYBOARD_DEVICE"))
    input = e;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto val = [&]() -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "wsdr-zero: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--fb") fb = val();
    else if (a == "--rotate") rotate = std::atoi(val());
    else if (a == "--input") input = val();
    else if (a == "--freq") {
      const double f = std::atof(val());
      uo.freq = f < 1e5 ? f * 1e6 : f; // MHz, or Hz
    } else if (a == "--span") uo.span_msps = std::atof(val());
    else if (a == "--no-autostart") uo.autostart = false;
    else if (a == "--record-dir") record_dir = val();
    else if (a == "--sdl") sdl = i + 1 < argc && std::atoi(argv[i + 1]) > 0 ? std::atoi(argv[++i]) : 3;
    else if (a == "--demo") demo = true;
    else if (a == "--script") script_s = val();
    else if (a == "--shot") shot = val();
    else if (a == "--version") {
      std::puts(WSDR_ZERO_VERSION);
      return 0;
    } else {
      usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  if (rotate != -1 && rotate != 0 && rotate != 90 && rotate != 270) {
    std::fprintf(stderr, "wsdr-zero: --rotate must be 0, 90 or 270\n");
    return 2;
  }
  if ((uo.freq < 2400e6 || uo.freq > 2500e6) && (uo.freq < 5000e6 || uo.freq > 6170e6)) {
    std::fprintf(stderr, "wsdr-zero: --freq must be 2400-2500 or 5000-6170 MHz\n");
    return 2;
  }

  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr); // (the launcher's force-close sends SIGINT)
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  Script script(script_s);
  const bool headless = script.active() || !shot.empty(); // a test run: no screen or keyboard needed
  std::unique_ptr<Display> display;
  std::unique_ptr<Input> in;
  std::string err;
  if (sdl) {
    if (!open_sdl(sdl, display, in, err)) {
      std::fprintf(stderr, "wsdr-zero: %s\n", err.c_str());
      return 1;
    }
  } else if (!headless) {
    display = open_fbdev(fb, rotate, err);
    if (!display) {
      std::fprintf(stderr, "wsdr-zero: %s\n", err.c_str());
      return 1;
    }
    in = open_evdev(input, err);
    if (!in) {
      std::fprintf(stderr, "wsdr-zero: %s\n", err.c_str());
      return 1;
    }
  }
  if (display)
    std::fprintf(stderr, "wsdr-zero: display %s; keys %s\n", display->describe().c_str(), in->describe().c_str());

  // what the backend's threads hand to the UI thread
  std::mutex qm;
  std::deque<std::variant<SpecRow, json>> queue;
  Sink sink;
  sink.row = [&](SpecRow &&r) {
    std::lock_guard<std::mutex> l(qm);
    if (queue.size() < 256) // (a stalled UI thread must not grow it without end)
      queue.emplace_back(std::move(r));
  };
  sink.event = [&](json &&e) {
    std::lock_guard<std::mutex> l(qm);
    queue.emplace_back(std::move(e));
  };
  std::unique_ptr<Backend> backend =
      demo ? make_demo_backend(sink) : make_engine_backend(sink, record_dir.empty() ? data_dir() + "/recordings" : record_dir);
  Ui ui([&](json c) { backend->submit(std::move(c)); }, uo);

  Canvas canvas;
  std::vector<KeyEvent> keys;
  double next_poll = 0, esc_since = -1;
  const double t_end = headless && !script.active() ? mono() + 3 : 0; // --shot alone: three seconds, then the picture
  while (!g_quit) {
    const double now = mono();
    std::deque<std::variant<SpecRow, json>> got;
    {
      std::lock_guard<std::mutex> l(qm);
      got.swap(queue);
    }
    for (auto &m : got) {
      if (auto *r = std::get_if<SpecRow>(&m))
        ui.on_row(*r);
      else
        ui.on_event(std::get<json>(m));
    }
    if (in) {
      keys.clear();
      in->poll(keys);
      for (const auto &k : keys)
        ui.on_key(k);
      // Esc held for a second leaves the app (a short press only goes back)
      if (!in->esc_held())
        esc_since = -1;
      else if (esc_since < 0)
        esc_since = now;
      else if (now - esc_since >= 1.0)
        break;
      if (in->closed())
        break;
    }
    if (script.active() && !script.run(now, ui, canvas))
      break;
    if (now >= next_poll) {
      backend->poll();
      next_poll = now + 0.5;
    }
    ui.tick(now);
    ui.draw(canvas);
    if (display)
      display->present(canvas);
    script.after_draw(canvas);
    if (t_end > 0 && now >= t_end)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(headless ? 20 : 66)); // ~15 frames a second
  }
  if (!shot.empty() && !write_ppm(canvas, shot))
    std::fprintf(stderr, "wsdr-zero: cannot write %s\n", shot.c_str());
  display.reset(); // clears the screen first: closing the adapter can take a moment
  backend.reset(); // closes the adapter and finishes a recording
  return 0;
}
