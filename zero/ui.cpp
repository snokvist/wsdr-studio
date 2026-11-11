// SPDX-License-Identifier: GPL-2.0-only
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace zero {
namespace {

constexpr uint32_t kBg = 0x0b0e13, kPanel = 0x141a22, kGrid = 0x232c38, kText = 0xc8d0d8, kDim = 0x6b7785,
                   kTraceA = 0x4fc3f7, kTraceB = 0xffb74d, kFillA = 0x10293a, kPeak = 0x3d6f8c, kGreen = 0x66d17a,
                   kRed = 0xff5a5a, kYellow = 0xffd54f;
constexpr double kDbRange = 64; // the plot is 64 px high: 1 dB a pixel
constexpr double kBelowFloor = 8;

std::string fmt(const char *f, double v) {
  char b[48];
  std::snprintf(b, sizeof b, f, v);
  return b;
}

// the channels that get a tick: 2.4 GHz 1-14; 5 GHz every fourth, 68-96 included (the radio receives there)
std::vector<int> tick_channels(bool five) {
  std::vector<int> v;
  if (!five) {
    for (int c = 1; c <= 14; c++)
      v.push_back(c);
    return v;
  }
  for (int c = 36; c <= 144; c += 4)
    v.push_back(c);
  for (int c = 149; c <= 177; c += 4)
    v.push_back(c);
  return v;
}

std::vector<std::string> wrap(const std::string &s, size_t width) {
  std::vector<std::string> out;
  std::string line;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string::npos)
      j = s.size();
    std::string word = s.substr(i, j - i);
    while (word.size() > width) { // a word longer than a line is cut
      if (!line.empty())
        out.push_back(line), line.clear();
      out.push_back(word.substr(0, width));
      word.erase(0, width);
    }
    if (!line.empty() && line.size() + 1 + word.size() > width)
      out.push_back(line), line.clear();
    if (!word.empty())
      line += (line.empty() ? "" : " ") + word;
    i = j + 1;
  }
  if (!line.empty())
    out.push_back(line);
  return out;
}

uint32_t mix(uint32_t a, uint32_t b, double t) {
  const auto ch = [&](int s) {
    const double x = (a >> s & 255) + ((b >> s & 255) - double(a >> s & 255)) * t;
    return static_cast<uint32_t>(std::lround(x)) << s;
  };
  return ch(16) | ch(8) | ch(0);
}

} // namespace

double channel_centre(int ch) {
  if (ch >= 1 && ch <= 13)
    return (2407 + 5 * ch) * 1e6;
  if (ch == 14)
    return 2484e6;
  if (ch >= 16 && ch <= 233)
    return (5000 + 5 * ch) * 1e6;
  return 0;
}

std::string channel_name(double freq) {
  const double mhz = freq / 1e6;
  if (std::abs(mhz - std::round(mhz)) > 1e-3)
    return "";
  const int m = static_cast<int>(std::lround(mhz));
  if (m == 2484)
    return "ch14";
  if (m >= 2412 && m <= 2472 && (m - 2407) % 5 == 0)
    return "ch" + std::to_string((m - 2407) / 5);
  if (m >= 5080 && m <= 6165 && m % 5 == 0)
    return "ch" + std::to_string((m - 5000) / 5);
  return "";
}

Ui::Ui(Send send, const UiOptions &o)
    : _send(std::move(send)), _want_run(o.autostart), _freq(o.freq), _want_span(o.span_msps * 1e6),
      _fall(static_cast<size_t>(kW) * kFallH, 0) {
  (_freq < 3e9 ? _freq24 : _freq5) = _freq;
  for (auto &v : _avg)
    v.assign(kW, -200.f);
  for (auto &v : _peak)
    v.assign(kW, -200.f);
  // waterfall colours: dark blue, teal, yellow, orange, white
  const std::pair<double, uint32_t> stops[] = {{0, 0x05060f}, {0.25, 0x1a2f8f}, {0.5, 0x1aa0a8},
                                               {0.7, 0xd8d040}, {0.87, 0xff6030}, {1, 0xffffff}};
  for (int i = 0; i < 256; i++) {
    const double t = i / 255.0;
    size_t k = 0;
    while (k + 2 < std::size(stops) && t > stops[k + 1].first)
      k++;
    _pal[i] = mix(stops[k].second, stops[k + 1].second, (t - stops[k].first) / (stops[k + 1].first - stops[k].first));
  }
  spans_for_device();
}

void Ui::note(const std::string &s, bool error) {
  _note = s;
  _note_error = error;
  _note_until = _now + (error ? 8 : 4);
}

void Ui::clear_traces() {
  for (auto &v : _avg)
    v.assign(kW, -200.f);
  for (auto &v : _peak)
    v.assign(kW, -200.f);
  _has = {false, false};
  std::fill(_fall.begin(), _fall.end(), 0);
  _have = false;
  _floor_set = false;
}

// The spans an adapter offers before it is open (as the engine's caps will say once it is): continuous
// up to what its link carries, gapped above that.
void Ui::spans_for_device() {
  const Dev *d = dev();
  const int min_cont = d && d->max_msps >= 40 ? 1 : 4;
  const int min_gap = d && d->gapped_msps >= 80 ? 0 : 1;
  _spans.clear();
  for (int r = 5; r >= std::min(min_cont, min_gap); r--)
    _spans.push_back({r, 80e6 / (1 << r), r < min_cont});
  pick_span(_want_span, false);
}

void Ui::pick_span(double fs, bool) {
  size_t best = 0;
  for (size_t i = 0; i < _spans.size(); i++)
    if (std::abs(_spans[i].fs - fs) < std::abs(_spans[best].fs - fs))
      best = i;
  _span = best;
}

void Ui::tune(double f) {
  const bool five = f > 3e9;
  f = five ? std::clamp(f, 5080e6, 6165e6) : std::clamp(f, 2412e6, 2484e6);
  if (!five && dev() && dev()->five_only) {
    note(dev()->model + " receives 5 GHz only", true);
    return;
  }
  _freq = f;
  (five ? _freq5 : _freq24) = f;
  _dirty = true;
}

void Ui::start() {
  const Dev *d = dev();
  if (!d) {
    note("No adapter plugged in", true);
    return;
  }
  if (!d->can_open) {
    note("No USB permission for the adapter: replug it", true);
    return;
  }
  if (d->five_only && _freq < 3e9)
    _freq = _freq5;
  if (_spans.empty())
    spans_for_device();
  const Span &s = _spans[_span];
  _send({{"cmd", "open"}, {"pid", d->pid}, {"bus", d->bus}, {"port", d->port}, {"freq", _freq},
         {"rate_sel", s.rate_sel}, {"allow_gapped", s.gapped}, {"gain_db", _gain}, {"agc", _agc}, {"usb3", false}});
  _opening = true;
  _want_run = true;
  _dirty = false;
  _error.clear();
}

void Ui::stop() {
  _want_run = false;
  if (_open || _opening)
    _send({{"cmd", "close"}});
}

void Ui::apply_entry() {
  const std::string e = _entry;
  _entry.clear();
  if (e.empty())
    return;
  const double n = std::atof(e.c_str());
  double f = 0;
  if (n <= 233 && e.find('.') == std::string::npos)
    f = channel_centre(static_cast<int>(n));
  else if ((n >= 2400 && n <= 2500) || (n >= 5000 && n <= 6170))
    f = n * 1e6;
  if (f <= 0) {
    note("'" + e + "' is no channel (1-14, 16-233) or MHz", true);
    return;
  }
  tune(f);
}

void Ui::on_key(const KeyEvent &k) {
  if (_help) { // any key closes the help
    if (!k.repeat)
      _help = false;
    return;
  }
  const bool digit = k.key == Key::Char && ((k.ch >= '0' && k.ch <= '9') || (k.ch == '.' && !_entry.empty()));
  if (digit) {
    if (_entry.size() < 6)
      _entry += k.ch;
    return;
  }
  if (!_entry.empty()) { // typing a channel or a frequency
    if (k.key == Key::Enter)
      apply_entry();
    else if (k.key == Key::Backspace)
      _entry.pop_back();
    else if (k.key == Key::Esc)
      _entry.clear();
    return;
  }
  // the launcher's own navigation keys double as arrows: F up, X down, Z left, C right
  Key key = k.key;
  if (key == Key::Char) {
    if (k.ch == 'z') key = Key::Left;
    else if (k.ch == 'c') key = Key::Right;
    else if (k.ch == 'f') key = Key::Up;
    else if (k.ch == 'x') key = Key::Down;
  }
  const double step = k.shift ? 20e6 : 5e6;
  switch (key) {
  case Key::Left:
  case Key::Right: {
    double f = _freq + (key == Key::Right ? step : -step);
    if (f > 2472e6 && f < 2484e6) // 2.4 GHz: channel 14 is 12 MHz above channel 13
      f = key == Key::Right ? 2484e6 : 2472e6;
    else if (f < 3e9)
      f = 2412e6 + std::round((f - 2412e6) / 5e6) * 5e6;
    tune(f);
    return;
  }
  case Key::Up:
  case Key::Down:
    if (_agc) {
      note("Gain is automatic (G turns AGC off)");
      return;
    }
    _gain = std::clamp(_gain + (key == Key::Up ? 2 : -2) * std::max(_gain_step, 0.5), 0.0, _gain_max);
    _dirty = true;
    return;
  default:
    break;
  }
  if (k.repeat)
    return; // only tuning and gain repeat
  if (key == Key::Space || key == Key::Enter) {
    if (_open || _opening)
      stop();
    else
      start();
    return;
  }
  if (key != Key::Char)
    return;
  switch (k.ch) {
  case 's': // the next span; Shift: the one before
    if (!_spans.empty()) {
      _span = (_span + (k.shift ? _spans.size() - 1 : 1)) % _spans.size();
      _want_span = _spans[_span].fs;
      _dirty = true;
    }
    break;
  case 'b':
    tune(_freq < 3e9 ? _freq5 : _freq24);
    break;
  case 'g':
    _agc = !_agc;
    _dirty = true;
    note(_agc ? "AGC on" : "AGC off: fixed gain");
    break;
  case 'a': {
    if (_dev_paths < 2) {
      note("This adapter has one antenna path");
      break;
    }
    _paths = _paths == 1 ? 2 : _paths == 2 ? 3 : 1;
    _send({{"cmd", "dsp"}, {"paths", _paths == 1 ? "a" : _paths == 2 ? "b" : "ab"}});
    clear_traces();
    break;
  }
  case 'p':
    _peak_hold = !_peak_hold;
    for (auto &v : _peak)
      v.assign(kW, -200.f);
    note(_peak_hold ? "Peak hold on" : "Peak hold off");
    break;
  case 'r':
    if (!_open)
      note("Start the adapter first", true);
    else
      _send({{"cmd", "record"}, {"action", _rec ? "stop" : "start"}});
    break;
  case 'd':
    if (_open || _opening)
      note("Stop (Space) before changing the adapter");
    else if (_devs.size() > 1) {
      _dev = (_dev + 1) % _devs.size();
      spans_for_device();
      note("Adapter: " + _devs[_dev].model + " on USB " + std::to_string(_devs[_dev].bus) + "-" + _devs[_dev].port);
    } else
      note("No other adapter");
    break;
  case 'h':
  case '?':
  case '/':
    _help = true;
    break;
  default:
    break;
  }
}

void Ui::on_event(const json &e) {
  const std::string type = e.value("type", "");
  if (type == "devices") {
    const std::string held = dev() ? std::to_string(dev()->bus) + "-" + dev()->port : "";
    _devs.clear();
    for (const auto &d : e.value("devices", json::array())) {
      Dev x;
      x.pid = d.value("pid", 0);
      x.bus = d.value("bus", 0);
      x.speed = d.value("speed_mbps", 0);
      x.model = d.value("model", "");
      x.port = d.value("port", "");
      x.driver = d.value("driver", "");
      x.can_open = d.value("can_open", false);
      x.max_msps = d.value("max_msps", 5.0);
      x.gapped_msps = d.value("gapped_msps", 40.0);
      const json bands = d.value("bands", json::array());
      x.five_only = bands.size() == 1;
      _devs.push_back(x);
    }
    _dev = 0;
    for (size_t i = 0; i < _devs.size(); i++)
      if (std::to_string(_devs[i].bus) + "-" + _devs[i].port == held)
        _dev = i;
    _scanned = true;
    if (!_open && !_opening)
      spans_for_device();
  } else if (type == "caps") {
    const bool was = _open;
    _open = e.value("open", false);
    if (_open) {
      _spans.clear();
      for (const auto &r : e.value("rates", json::array()))
        _spans.push_back({r.value("rate_sel", 4), r.value("fs", 5e6), r.value("gapped", false)});
      _gain_max = e.value("gain_max", 45.0);
      _gain_step = e.value("gain_step", 1.0);
      _dev_paths = e.value("paths", 1);
      _adapter = e.value("adapter", "");
      pick_span(_want_span, false);
      if (_dev_paths < 2)
        _paths = 1;
    } else if (was) {
      _rec = false;
      _next_scan = 0;
    }
  } else if (type == "settings") {
    if (e.contains("rx") && !_dirty && !_await_set) {
      _gain = e["rx"].value("gain_db", _gain);
      _agc = e["rx"].value("agc", _agc);
    }
  } else if (type == "status") {
    _state = e.value("state", "closed");
    _error = e.value("error", "");
    if (e.contains("rx")) {
      _rx_state = e["rx"].value("state", "");
      const double r = e["rx"].value("restarts", 0.0);
      if (r > _restarts)
        _restart_seen_at = _now;
      _restarts = r;
    } else {
      _rx_state.clear();
      _restarts = 0;
    }
    if (e.contains("record")) {
      _rec = e["record"].value("active", false);
      _rec_seconds = e["record"].value("seconds", 0.0);
    }
  } else if (type == "notice") {
    note(e.value("text", ""));
  } else if (type == "alert") {
    note(e.value("msg", ""), true);
    if (e.value("kind", "") != "lost")
      _want_run = false; // (a lost adapter is started again when it is back)
  } else if (type == "recorded") {
    note(e.contains("error") && !e.value("error", "").empty()
             ? "Recording failed: " + e.value("error", "")
             : fmt("Saved %.0f s of IQ", e.value("seconds", 0.0)) + (e.contains("reason") ? " (" + e.value("reason", "") + ")" : ""));
  } else if (type == "ack" || type == "error") {
    const std::string cmd = e.value("cmd", "");
    const bool err = type == "error";
    if (cmd == "open") {
      _opening = false;
      if (err) {
        _want_run = false;
        note(e.value("msg", "could not start"), true);
      } else if (_paths != 1) {
        _send({{"cmd", "dsp"}, {"paths", _paths == 2 ? "b" : "ab"}});
      }
    } else if (cmd == "set") {
      _await_set = false;
      if (err)
        note(e.value("msg", "could not apply"), true);
    } else if (cmd == "close") {
      _opening = false;
    } else if (err && cmd != "scan") {
      note(e.value("msg", "failed"), true);
    }
  }
}

void Ui::tick(double now) {
  _now = now;
  if (!_dsp_sent) {
    _send({{"cmd", "dsp"}, {"fft", 2048}, {"rows_per_s", 10}});
    _dsp_sent = true;
  }
  if (!_open && !_opening && now >= _next_scan) { // (a scan would queue behind a bring-up)
    _send({{"cmd", "scan"}});
    _next_scan = now + 2;
  }
  if (_want_run && !_open && !_opening && dev() && dev()->can_open)
    start();
  if (_open && _dirty && !_await_set && !_spans.empty()) {
    const Span &s = _spans[_span];
    _send({{"cmd", "set"}, {"freq", _freq}, {"rate_sel", s.rate_sel}, {"allow_gapped", s.gapped},
           {"gain_db", _gain}, {"agc", _agc}});
    _dirty = false;
    _await_set = true;
  }
  if (!_note.empty() && now > _note_until)
    _note.clear();
}

void Ui::on_row(const SpecRow &r) {
  const int bins = static_cast<int>(r.avg.size());
  if (bins <= 0 || r.df <= 0 || r.path < 0 || r.path > 1 || r.max.size() != r.avg.size())
    return;
  const double df = bins * r.df / kW;
  if (!_have || std::abs(r.f0 - _f0) > 1 || std::abs(df - _df) > 1e-3) { // another band: a new picture
    clear_traces();
    _f0 = r.f0;
    _df = df;
  }
  _gen = r.gen;
  _gapped = r.gapped;
  _have = true;
  _has[r.path] = true;
  std::vector<float> &avg = _avg[r.path], &peak = _peak[r.path];
  for (int x = 0; x < kW; x++) { // a column shows the strongest of the bins it covers
    const int lo = static_cast<int>(static_cast<int64_t>(x) * bins / kW);
    const int hi = std::max(lo + 1, static_cast<int>(static_cast<int64_t>(x + 1) * bins / kW));
    float a = -200.f, m = -200.f;
    for (int i = lo; i < hi && i < bins; i++) {
      a = std::max(a, r.avg[i]);
      m = std::max(m, r.max[i]);
    }
    avg[x] = a;
    peak[x] = std::max(peak[x] - 0.3f, m); // the hold decays ~3 dB a second
  }
  const int primary = _paths == 2 ? 1 : 0; // the antenna the waterfall shows
  if (r.path != primary)
    return;
  std::vector<float> sorted = avg;
  std::nth_element(sorted.begin(), sorted.begin() + kW / 4, sorted.end());
  const double floor = sorted[kW / 4];
  _floor = _floor_set ? _floor + 0.15 * (floor - _floor) : floor;
  _floor_set = true;
  const double lo = _floor - kBelowFloor;
  uint8_t *row = &_fall[static_cast<size_t>(_fall_head) * kW];
  for (int x = 0; x < kW; x++)
    row[x] = static_cast<uint8_t>(std::clamp((avg[x] - lo) / kDbRange * 255.0, 0.0, 255.0));
  _fall_head = (_fall_head + 1) % kFallH;
}

void Ui::draw_header(Canvas &c) const {
  c.fill(0, 0, kW, 10, kPanel);
  if (!_entry.empty())
    c.text(2, 1, "> " + _entry + "_  ch or MHz, Enter", kYellow);
  else {
    const std::string ch = channel_name(_freq);
    int x = c.text(2, 1, fmt("%.1f MHz", _freq / 1e6), kText);
    if (!ch.empty())
      c.text(x + 4, 1, ch, kDim);
    if (!_spans.empty()) {
      const Span &s = _spans[_span];
      x = c.text(110, 1, fmt(s.fs < 5e6 ? "%.1fM" : "%.0fM", s.fs / 1e6), kText);
      if (s.gapped)
        c.text(x + 3, 1, "gap", kYellow);
    }
    c.text(166, 1, _agc ? "AGC" : fmt("%.0fdB", _gain), kText);
    c.text(198, 1, _paths == 1 ? "A" : _paths == 2 ? "B" : "A+B", _paths == 2 ? kTraceB : kTraceA);
  }
  std::string st = "STOP";
  uint32_t col = kDim;
  if (_rec) {
    const int s = static_cast<int>(_rec_seconds);
    char b[24];
    std::snprintf(b, sizeof b, "REC %d:%02d", s / 60, s % 60);
    st = b;
    col = kRed;
  } else if (_opening || _state == "opening") {
    st = "START";
    col = kYellow;
  } else if (_open && _now - _restart_seen_at < 3) {
    st = "FAULT";
    col = kYellow;
  } else if (_open) {
    st = "RUN";
    col = kGreen;
  } else if (_scanned && _devs.empty()) {
    st = "NO DEV";
  }
  c.text_right(kW - 2, 1, st, col);
}

void Ui::draw_plot(Canvas &c) const {
  const double lo = _floor - kBelowFloor;
  for (int k = 1; k < 4; k++) { // a line every 16 dB
    const int y = kPlotY + kPlotH - 16 * k;
    for (int x = 0; x < kW; x += 4)
      c.put(x, y, kGrid);
  }
  if (!_have)
    return;
  const auto y_of = [&](float v) { return kPlotY + kPlotH - 1 - static_cast<int>(std::clamp(v - lo, 0.0, kDbRange - 1)); };
  const int primary = _paths == 2 ? 1 : 0;
  if (_has[primary])
    for (int x = 0; x < kW; x++) {
      const int y = y_of(_avg[primary][x]);
      c.vline(x, y, kPlotY + kPlotH - y, kFillA);
    }
  for (int k = 1; k < 4; k++) // the grid again, over the fill
    for (int x = 0; x < kW; x += 4)
      c.put(x, kPlotY + kPlotH - 16 * k, kGrid);
  if (_peak_hold && _has[primary])
    for (int x = 0; x < kW; x++)
      c.put(x, y_of(_peak[primary][x]), kPeak);
  for (int p = 1; p >= 0; p--) { // A over B
    if (!_has[p] || !(_paths >> p & 1))
      continue;
    const uint32_t col = p ? kTraceB : kTraceA;
    int prev = y_of(_avg[p][0]);
    for (int x = 0; x < kW; x++) {
      const int y = y_of(_avg[p][x]);
      c.vline(x, std::min(y, prev), std::abs(y - prev) + 1, col);
      prev = y;
    }
  }
  for (int k = 1; k < 4; k++)
    c.text(1, kPlotY + kPlotH - 16 * k - 8, fmt("%.0f", lo + 16 * k), kDim);
  // where the requested frequency is (it can sit off the radio's centre)
  const double xf = (_freq - _f0) / _df;
  if (xf >= 0 && xf < kW)
    for (int i = 0; i < 3; i++)
      c.hline(static_cast<int>(xf) - (2 - i), kPlotY + i, 2 * (2 - i) + 1, kYellow);
}

void Ui::draw_axis(Canvas &c) const {
  c.hline(0, kPlotY + kPlotH, kW, kGrid);
  if (!_have)
    return;
  const double lo = _f0, hi = _f0 + _df * kW;
  const char *f = hi - lo < 10e6 ? "%.2f" : "%.0f";
  const std::string l = fmt(f, lo / 1e6), r = fmt(f, hi / 1e6);
  c.text(1, kAxisY, l, kDim);
  c.text_right(kW - 1, kAxisY, r, kDim);
  int free_from = Canvas::width(l) + 6;                // labels must not touch the edge labels
  const int free_to = kW - Canvas::width(r) - 6;
  bool any = false;
  for (const int ch : tick_channels(lo > 3e9)) {
    const double fc = channel_centre(ch);
    if (fc <= lo || fc >= hi)
      continue;
    const int x = static_cast<int>((fc - lo) / _df);
    c.vline(x, kPlotY + kPlotH + 1, 2, kText);
    const std::string s = std::to_string(ch);
    const int x0 = x - Canvas::width(s) / 2;
    if (x0 >= free_from && x0 + Canvas::width(s) <= free_to) {
      c.text(x0, kAxisY, s, kText);
      free_from = x0 + Canvas::width(s) + 5;
      any = true;
    }
  }
  if (!any) { // no channel centre in view: the middle frequency
    const std::string m = fmt("%.2f MHz", (lo + hi) / 2e6);
    c.text(kW / 2 - Canvas::width(m) / 2, kAxisY, m, kText);
  }
}

void Ui::draw_waterfall(Canvas &c) const {
  for (int y = 0; y < kFallH; y++) { // newest row on top
    const int src = ((_fall_head - 1 - y) % kFallH + kFallH) % kFallH;
    const uint8_t *row = &_fall[static_cast<size_t>(src) * kW];
    uint32_t *dst = &c.px[static_cast<size_t>(kFallY + y) * kW];
    for (int x = 0; x < kW; x++)
      dst[x] = _pal[row[x]];
  }
}

void Ui::draw_help(Canvas &c) const {
  c.clear(kBg);
  c.fill(0, 0, kW, 10, kPanel);
  c.text(2, 1, "wsdr-zero keys", kText);
  c.text_right(kW - 2, 1, "any key: back", kDim);
  const std::pair<const char *, const char *> rows[] = {
      {"Z / C", "tune 5 MHz down / up (Shift: 20)"},
      {"0-9, Enter", "go to a channel, or to a MHz"},
      {"B", "2.4 GHz / 5 GHz"},
      {"S", "next span (Shift: previous)"},
      {"F / X", "gain up / down"},
      {"G", "automatic gain (AGC) on / off"},
      {"A", "antenna A, B, or both"},
      {"P", "peak hold on / off"},
      {"R", "record IQ (continuous spans)"},
      {"D", "next adapter (while stopped)"},
      {"Space", "start / stop"},
      {"Hold Esc", "exit"},
  };
  int y = 15;
  for (const auto &r : rows) {
    c.text(4, y, r.first, kTraceA);
    c.text(76, y, r.second, kText);
    y += 11;
  }
  c.text(4, y + 3, "Arrow keys (Fn+F/X/Z/C) work like F X Z C.", kDim);
}

void Ui::draw(Canvas &c) const {
  if (_help) {
    draw_help(c);
    return;
  }
  c.clear(kBg);
  draw_header(c);
  draw_plot(c);
  draw_axis(c);
  draw_waterfall(c);

  // what is going on, while there is no live picture
  std::vector<std::string> msg;
  const Dev *d = dev();
  if (_opening || _state == "opening")
    msg = {"Starting " + (d ? d->model : std::string("the adapter")) + "...", "(this can take up to 20 s)"};
  else if (_open && !_have)
    msg = {"Waiting for samples..."};
  else if (!_open) {
    if (!_scanned)
      msg = {"Looking for adapters..."};
    else if (!d)
      msg = {"No adapter found", "Plug in an RTL8812BU, 8812CU or 8812EU"};
    else if (!d->can_open)
      msg = {"Cannot open the " + d->model, "No USB permission: unplug it and plug it", "back in (see the README if that fails)"};
    else {
      if (!_error.empty())
        msg = wrap(_error, 46);
      msg.push_back(d->model + " stopped. Space: start");
    }
  }
  if (!msg.empty()) {
    size_t w = 0;
    for (const auto &s : msg)
      w = std::max(w, s.size());
    const int bw = static_cast<int>(w) * kCharW + 12, bh = static_cast<int>(msg.size()) * 10 + 8;
    const int bx = (kW - bw) / 2, by = kPlotY + (kFootY - kPlotY - bh) / 2;
    c.fill(bx - 1, by - 1, bw + 2, bh + 2, kGrid);
    c.fill(bx, by, bw, bh, kPanel);
    for (size_t i = 0; i < msg.size(); i++)
      c.text(kW / 2 - Canvas::width(msg[i]) / 2, by + 5 + static_cast<int>(i) * 10, msg[i], kText);
  }

  if (!_note.empty())
    c.text(2, kFootY, _note.substr(0, 52), _note_error ? kRed : kYellow);
  else
    c.text(2, kFootY, "Z/C tune  S span  F/X gain  B band  H help", kDim);
}

} // namespace zero
