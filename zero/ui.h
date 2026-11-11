// SPDX-License-Identifier: GPL-2.0-only
// wsdr-zero's screen and keys: one view (spectrum above a waterfall) on 320 x 170, driven from a
// keyboard. It talks to a Backend (backend.h) in the engine's own JSON commands and events, and knows
// nothing about the display or the input device (platform.h). Everything here runs on the UI thread.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "canvas.h"

namespace zero {

using json = nlohmann::json;

enum class Key { Left, Right, Up, Down, Enter, Esc, Space, Backspace, Char };
struct KeyEvent {
  Key key = Key::Char;
  char ch = 0;         // Key::Char: lower-case letter, digit or punctuation
  bool shift = false;
  bool repeat = false; // auto-repeat of a held key
};

// One spectrum row from the backend: bin 0 at f0 (Hz), bins df apart, dBFS.
struct SpecRow {
  int path = 0; // 0 = antenna A, 1 = B
  int gen = 0;
  bool gapped = false;
  double f0 = 0, df = 0;
  std::vector<float> avg, max;
};

struct UiOptions {
  double freq = 5180e6;
  double span_msps = 20; // the span picked at start (the nearest one the adapter offers)
  bool autostart = true; // start the first adapter found, and again when it is plugged back in
};

class Ui {
public:
  using Send = std::function<void(json)>;
  Ui(Send send, const UiOptions &o);

  void on_row(const SpecRow &r);
  void on_event(const json &e); // status, settings, caps, devices, notice, alert, recorded, ack, error
  void on_key(const KeyEvent &k);
  void tick(double now);        // call every frame (seconds, monotonic)
  void draw(Canvas &c) const;

  // tests
  bool running() const { return _open; }
  double freq() const { return _freq; }
  double span() const { return _spans.empty() ? 0 : _spans[_span].fs; }

private:
  struct Span {
    int rate_sel = 4;
    double fs = 5e6;
    bool gapped = false;
  };
  struct Dev {
    int pid = 0, bus = 0, speed = 0;
    std::string model, port, driver;
    bool can_open = false, five_only = false;
    double max_msps = 5, gapped_msps = 40;
  };

  void tune(double f);
  void pick_span(double fs, bool prefer_gapped);
  void spans_for_device();
  void start();
  void stop();
  void note(const std::string &s, bool error = false);
  void apply_entry();
  const Dev *dev() const { return _dev < _devs.size() ? &_devs[_dev] : nullptr; }
  void draw_header(Canvas &c) const;
  void draw_plot(Canvas &c) const;
  void draw_axis(Canvas &c) const;
  void draw_waterfall(Canvas &c) const;
  void draw_help(Canvas &c) const;
  void clear_traces();

  Send _send;
  double _now = 0;

  // the adapter
  std::vector<Dev> _devs;
  size_t _dev = 0;
  bool _scanned = false;   // a device list has arrived
  double _next_scan = 0;
  bool _want_run = true;   // the user wants it streaming (Space toggles)
  bool _open = false;      // caps say an adapter is open
  bool _opening = false;   // an open command is out
  std::string _state = "closed", _rx_state, _adapter, _error;
  double _restarts = 0, _restart_seen_at = -1e9;
  bool _rec = false;
  double _rec_seconds = 0;

  // what the user asked for
  double _freq, _freq24 = 2437e6, _freq5 = 5180e6;
  double _want_span;
  std::vector<Span> _spans;
  size_t _span = 0;
  double _gain = 30, _gain_max = 45, _gain_step = 1;
  bool _agc = false;
  int _paths = 1, _dev_paths = 2; // bit 0 = A, bit 1 = B
  bool _dirty = false;            // a "set" is owed
  bool _await_set = false;        // a "set" is out
  bool _dsp_sent = false;

  // the picture
  static constexpr int kPlotY = 12, kPlotH = 64, kAxisY = 78, kFallY = 87, kFallH = 74, kFootY = 162;
  double _f0 = 0, _df = 0; // band of the rows shown: column 0 at _f0, _df per column
  int _gen = -1;
  bool _gapped = false, _have = false;
  std::array<std::vector<float>, 2> _avg, _peak; // per antenna, kW columns (dBFS)
  std::array<bool, 2> _has{false, false};
  bool _peak_hold = true;
  double _floor = -100; // smoothed noise floor (dBFS): the plot shows _floor - 8 .. + 56
  bool _floor_set = false;
  std::vector<uint8_t> _fall; // kFallH rows of kW palette indexes, a ring
  int _fall_head = 0;         // the row written next
  std::array<uint32_t, 256> _pal{};

  std::string _entry;         // digits being typed (a channel or MHz)
  bool _help = false;
  std::string _note;
  bool _note_error = false;
  double _note_until = 0;
};

// A channel's centre (Hz); 0 for a number that is no channel.
double channel_centre(int ch);
// "ch36" for a frequency on a channel centre, else "".
std::string channel_name(double freq);

} // namespace zero
