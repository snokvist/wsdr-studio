// SPDX-License-Identifier: GPL-2.0-only
// --demo: a made-up adapter and band, so the screen and the keys can be tried (and tested) without a
// dongle. It answers the commands the UI sends the way the engine does.
#include "backend.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>

namespace zero {
namespace {

class DemoBackend : public Backend {
public:
  explicit DemoBackend(Sink sink) : _sink(std::move(sink)), _t([this] { loop(); }) {}
  ~DemoBackend() override {
    _quit = true;
    _t.join();
  }
  void submit(json c) override {
    const std::string cmd = c.value("cmd", "");
    json r = {{"type", "ack"}, {"cmd", cmd}};
    std::lock_guard<std::mutex> l(_m);
    if (cmd == "scan") {
      _sink.event(devices());
    } else if (cmd == "open") {
      set(c);
      _open_at = now() + 1.0; // a bring-up takes a while
      _reply_open = true;
      return;
    } else if (cmd == "close") {
      _open = false;
      _open_at = -1;
      _sink.event(json{{"type", "caps"}, {"open", false}});
    } else if (cmd == "set") {
      if (!_open)
        r = {{"type", "error"}, {"cmd", cmd}, {"kind", "closed"}, {"msg", "no adapter open"}};
      else
        set(c);
    } else if (cmd == "dsp") {
      const std::string p = c.value("paths", "");
      if (!p.empty())
        _paths = p == "a" ? 1 : p == "b" ? 2 : 3;
    } else if (cmd == "record") {
      r = {{"type", "error"}, {"cmd", cmd}, {"kind", "unsupported"}, {"msg", "the demo adapter cannot record"}};
    }
    _sink.event(std::move(r));
  }
  void poll() override {
    std::lock_guard<std::mutex> l(_m);
    json st = {{"type", "status"}, {"state", _open ? "open" : _open_at >= 0 ? "opening" : "closed"}, {"error", ""},
               {"record", {{"active", false}, {"seconds", 0.0}}}};
    if (_open)
      st["rx"] = {{"state", "streaming"}, {"restarts", 0}, {"gapped", _rate < 4}};
    _sink.event(std::move(st));
  }

private:
  static double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  static json devices() {
    return {{"type", "devices"},
            {"devices", json::array({{{"pid", 0xb812}, {"model", "RTL8812BU"}, {"chip", "RTL8822B"}, {"bus", 1},
                                      {"port", "1"}, {"speed_mbps", 480}, {"driver", ""}, {"can_open", true},
                                      {"max_msps", 5.0}, {"gapped_msps", 80.0}, {"bands", {"2.4", "5"}}}})}};
  }
  void set(const json &c) {
    _freq = c.value("freq", _freq);
    _rate = c.value("rate_sel", _rate);
    _gain = c.value("gain_db", _gain);
  }
  // dBFS at an absolute frequency: a floor, three Wi-Fi networks, a carrier and a hopper
  double level(double f, double t, std::mt19937 &rng) {
    std::normal_distribution<double> n(0, 1.2);
    double p = std::pow(10, (-98 + (_gain - 30)) / 10);
    const auto wifi = [&](double fc, double db, double duty_hz) {
      if (std::abs(f - fc) < 8.3e6 && std::fmod(t * duty_hz, 1.0) < 0.7)
        p += std::pow(10, (db + (_gain - 30) - 3 * std::pow((f - fc) / 8.3e6, 6)) / 10);
    };
    wifi(5180e6, -62, 1.3);
    wifi(5200e6, -78, 0.7);
    wifi(5240e6, -70, 2.1);
    wifi(2437e6, -58, 1.1);
    wifi(2462e6, -74, 0.5);
    if (std::abs(f - 5191.3e6) < 30e3 || std::abs(f - 2420e6) < 30e3)
      p += std::pow(10, (-66 + (_gain - 30)) / 10);
    const double hop = 2402e6 + std::fmod(std::floor(t * 12) * 37, 79) * 1e6;
    if (std::abs(f - hop) < 0.5e6)
      p += std::pow(10, (-64 + (_gain - 30)) / 10);
    return 10 * std::log10(p) + n(rng);
  }
  void loop() {
    std::mt19937 rng(1);
    while (!_quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::lock_guard<std::mutex> l(_m);
      const double t = now();
      if (!_open && _open_at >= 0 && t >= _open_at) {
        _open = true;
        _open_at = -1;
        json rates = json::array();
        for (int r = 5; r >= 0; r--)
          rates.push_back({{"rate_sel", r}, {"fs", 80e6 / (1 << r)}, {"gapped", r < 4}});
        _sink.event(json{{"type", "caps"}, {"open", true}, {"adapter", "RTL8812BU (demo)"}, {"paths", 2},
                         {"rates", rates}, {"gain_max", 45.0}, {"gain_step", 1.0}});
        if (_reply_open)
          _sink.event(json{{"type", "ack"}, {"cmd", "open"}});
        _reply_open = false;
      }
      if (!_open)
        continue;
      const double fs = 80e6 / (1 << _rate);
      const int bins = 1024;
      for (int path = 0; path < 2; path++) {
        if (!(_paths >> path & 1))
          continue;
        SpecRow r;
        r.path = path;
        r.gapped = _rate < 4;
        r.f0 = _freq - fs / 2;
        r.df = fs / bins;
        r.avg.resize(bins);
        r.max.resize(bins);
        for (int i = 0; i < bins; i++) {
          const double v = level(r.f0 + (i + 0.5) * r.df, t, rng) - (path ? 4 : 0);
          r.avg[i] = static_cast<float>(v);
          r.max[i] = static_cast<float>(v + 5);
        }
        _sink.row(std::move(r));
      }
    }
  }

  Sink _sink;
  std::mutex _m;
  bool _open = false, _reply_open = false;
  double _open_at = -1, _freq = 5180e6, _gain = 30;
  int _rate = 4, _paths = 1;
  std::atomic<bool> _quit{false};
  std::thread _t; // last
};

} // namespace

std::unique_ptr<Backend> make_demo_backend(Sink sink) { return std::make_unique<DemoBackend>(std::move(sink)); }

} // namespace zero
