// SPDX-License-Identifier: GPL-2.0-only
#include "engine.h"
#include "usbmode.h"
#include "signals.h"
#include "wifi.h"

#include <set>

#include <optional>
#include <sys/statvfs.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "logger.h"
#include "protocol.h"

namespace studio {

namespace {
using clk = std::chrono::steady_clock;
const clk::time_point g_t0 = clk::now();
double now_s() { return std::chrono::duration<double>(clk::now() - g_t0).count(); }

const char *open_kind(wsdr::OpenError::Kind k) {
  switch (k) {
  case wsdr::OpenError::NotFound: return "not_found";
  case wsdr::OpenError::Permission: return "permission";
  case wsdr::OpenError::Busy: return "busy";
  default: return "other";
  }
}

struct CmdError : std::runtime_error {
  std::string kind;
  CmdError(std::string k, const std::string &m) : std::runtime_error(m), kind(std::move(k)) {}
};

// Validated field access for client JSON: wrong types and out-of-range values are request errors.
int integer(const json &c, const char *k, int lo, int hi) {
  const json &v = c.at(k);
  if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi)
    throw CmdError("bad_request", std::string(k) + " must be an integer in [" + std::to_string(lo) + ", " +
                                      std::to_string(hi) + "]");
  return static_cast<int>(v.get<int64_t>());
}
double num(const json &c, const char *k, double lo, double hi) {
  const json &v = c.at(k);
  if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < lo || v.get<double>() > hi)
    throw CmdError("bad_request", std::string(k) + " must be a number in [" + std::to_string(lo) + ", " +
                                      std::to_string(hi) + "]");
  return v.get<double>();
}
bool flag(const json &c, const char *k) {
  if (!c.at(k).is_boolean())
    throw CmdError("bad_request", std::string(k) + " must be true or false");
  return c.at(k).get<bool>();
}
int paths_from(const json &c) {
  const json &v = c.at("paths");
  const std::string s = v.is_string() ? v.get<std::string>() : "";
  if (s == "a") return 1;
  if (s == "b") return 2;
  if (s == "ab") return 3;
  throw CmdError("bad_request", "paths must be \"a\", \"b\" or \"ab\"");
}
const char *paths_name(int p) { return p == 2 ? "b" : p == 3 ? "ab" : "a"; }
} // namespace

Engine::Engine(Callbacks cb, std::string record_dir, int slot, std::shared_ptr<Claims> claims)
    : _cb(std::move(cb)), _slot(slot), _claims(claims ? std::move(claims) : std::make_shared<Claims>()),
      _rec(std::move(record_dir)) {
  _cmd = std::thread([this] { cmd_loop(); });
  _dsp = std::thread([this] { dsp_loop(); });
  _wifi = std::thread([this] { wifi_loop(); });
  // free disk space every 2 s, on a thread of its own: a slow or hung file system must stall neither
  // the I/O thread (status) nor the DSP thread (the recording)
  _disk = std::thread([this] {
    std::unique_lock<std::mutex> l(_qm);
    while (!_quit) {
      l.unlock();
      _disk_free = disk_free();
      l.lock();
      _diskcv.wait_for(l, std::chrono::seconds(2), [&] { return _quit; });
    }
  });
}

Engine::~Engine() {
  {
    std::lock_guard<std::mutex> l(_qm);
    _quit = true;
  }
  _qcv.notify_all();
  _diskcv.notify_all();
  if (_cmd.joinable())
    _cmd.join();
  close_adapter("the app closed"); // stops the recorder and the stream; the DSP thread then idles
  if (_disk.joinable())
    _disk.join(); // (after the recording is saved: a slow file system may hold it)
  _dsp_quit = true;
  _ring.wake();
  if (_dsp.joinable())
    _dsp.join();
  {
    std::lock_guard<std::mutex> l(_wm);
    _wquit = true;
  }
  _wcv.notify_all();
  if (_wifi.joinable())
    _wifi.join();
}

void Engine::submit(json cmd, std::function<void(const json &)> reply) {
  {
    std::lock_guard<std::mutex> l(_qm);
    _q.emplace_back(std::move(cmd), std::move(reply));
  }
  _qcv.notify_one();
}

void Engine::poll() {
  const auto r = rx();
  if (!r)
    return;
  // Fatal only when the worker has ended (a retune error is retried and passes).
  const wsdr::RxStatus st = r->status();
  if (!st.running && (st.state == wsdr::RxState::Lost || st.state == wsdr::RxState::Error) &&
      !_lost_requested.exchange(true))
    submit({{"cmd", "_lost"}}, nullptr);
}

void Engine::cmd_loop() {
  for (;;) {
    std::pair<json, std::function<void(const json &)>> item;
    {
      std::unique_lock<std::mutex> l(_qm);
      _qcv.wait(l, [&] { return !_q.empty() || _quit; });
      if (_quit)
        return;
      item = std::move(_q.front());
      _q.pop_front();
    }
    const json &c = item.first;
    json r;
    try {
      r = run(c);
      r["type"] = "ack";
    } catch (const CmdError &e) {
      r = {{"type", "error"}, {"kind", e.kind}, {"msg", e.what()}};
    } catch (const json::exception &e) {
      r = {{"type", "error"}, {"kind", "bad_request"}, {"msg", e.what()}};
    } catch (const std::exception &e) {
      r = {{"type", "error"}, {"kind", "other"}, {"msg", e.what()}};
    }
    if (c.contains("id"))
      r["id"] = c["id"];
    if (c.contains("cmd"))
      r["cmd"] = c["cmd"];
    if (item.second)
      item.second(r);
  }
}

json Engine::run(const json &c) {
  if (!c.contains("cmd") || !c["cmd"].is_string())
    throw CmdError("bad_request", "missing cmd");
  const std::string cmd = c["cmd"].get<std::string>();
  if (cmd == "scan") {
    json d = devices_json(scan_devices());
    _cb.event(d);
    return {{"devices", d["devices"]}};
  }
  if (cmd == "open") {
    open_adapter(static_cast<uint16_t>(integer(c, "pid", 0, 65535)), c);
    return {};
  }
  if (cmd == "close") {
    close_adapter("the view was stopped");
    _cb.event(caps());
    return {};
  }
  if (cmd == "set") {
    const auto r = rx();
    if (!r || !r->is_open())
      throw CmdError("closed", "no adapter open");
    wsdr::RxSettings s;
    {
      std::lock_guard<std::mutex> l(_sm);
      s = _view;
    }
    if (c.contains("freq"))
      s.freq = num(c, "freq", 2.3e9, 6.2e9);
    if (c.contains("rate_sel"))
      s.rate_sel = integer(c, "rate_sel", 1, 5);
    // a gapped span only when asked for: a span change without it is a continuous one (the widest the
    // adapter carries, if it asks for more); a change of anything else keeps what the view has
    if (c.contains("allow_gapped"))
      s.allow_gapped = flag(c, "allow_gapped");
    else if (c.contains("rate_sel"))
      s.allow_gapped = false;
    // (applied once every field has passed: a rejected command changes nothing)
    const std::optional<bool> offset = c.contains("offset_tuning") ? std::optional<bool>(flag(c, "offset_tuning")) : std::nullopt;
    if (c.contains("gain_db"))
      s.gain_db = num(c, "gain_db", 0, 100);
    if (c.contains("agc"))
      s.agc = flag(c, "agc");
    if (c.contains("wide"))
      s.wide = flag(c, "wide");
    const bool caps_changed = offset && _offset_tuning.exchange(*offset) != *offset;
    const wsdr::RxSettings hw = hw_for(s);
    r->set(hw);
    set_view(s, r->settings());
    _cb.event(settings());
    if (caps_changed)
      _cb.event(caps()); // the spans' captured rates (hw_fs) changed
    return {};
  }
  if (cmd == "dsp") {
    DspConfig n;
    {
      std::lock_guard<std::mutex> l(_dm);
      n = _dsp_cfg;
    }
    // validate everything first, then apply
    if (c.contains("fft")) {
      n.fft = integer(c, "fft", 1024, 16384);
      if (n.fft & (n.fft - 1))
        throw CmdError("bad_request", "fft must be a power of two");
    }
    if (c.contains("rows_per_s"))
      n.rows_per_s = num(c, "rows_per_s", 1, 30);
    if (c.contains("paths")) {
      n.paths = paths_from(c);
      std::lock_guard<std::mutex> l(_sm);
      if (n.paths != 1 && _caps.paths < 2)
        throw CmdError("bad_request", "this adapter has one receive path (A)");
    }
    if (c.contains("dc_remove"))
      n.dc_remove = flag(c, "dc_remove");
    if (c.contains("wifi"))
      n.wifi = flag(c, "wifi");
    {
      std::lock_guard<std::mutex> l(_dm);
      _dsp_cfg = n;
      _dsp_dirty = true;
    }
    _wifi_on = n.wifi;
    _cb.event(settings());
    return {};
  }
  if (cmd == "record") {
    const std::string a = c.value("action", "");
    if (a == "stop") {
      const std::string file = _rec.info().path;
      stop_recording("");
      return {{"file", file}};
    }
    if (a != "start")
      throw CmdError("bad_request", "record action must be start or stop");
    const auto r = rx();
    if (!r || !r->is_open())
      throw CmdError("closed", "no adapter open");
    if (_rec.info().active)
      throw CmdError("busy", "already recording");
    const wsdr::RxStatus st = r->status();
    if (st.state != wsdr::RxState::Streaming)
      throw CmdError("not_streaming", "not streaming");
    if (st.settling)
      throw CmdError("busy", "settings are being applied; try again in a moment");
    if (st.gapped)
      throw CmdError("unsupported", "recording needs a continuous rate (gapped spans sample only part of the time)");
    const wsdr::RxSettings s = r->settings();
    int path;
    {
      std::lock_guard<std::mutex> l(_dm);
      path = _dsp_cfg.paths == 2 ? 1 : 0; // B only when B alone is shown; A otherwise
    }
    std::string adapter;
    {
      std::lock_guard<std::mutex> l(_sm);
      adapter = _adapter;
    }
    _recording = false;
    const std::string file = _rec.start(st.fs, st.centre_hz, adapter, path, s.gain_db, s.agc);
    _rec_path = path;
    _rec_gen = st.gen; // only banks of this run (centre/fs as in st) go into the file
    _rec_cfg = st.cfg;
    _rec_id++;
    _rec_stop_requested = false;
    _recording = true;
    _cb.event(status());
    return {{"file", file}};
  }
  if (cmd == "_rec_stop") { // from the DSP thread; ignore requests about an earlier recording
    if (c.value("rec", uint64_t{0}) == _rec_id.load())
      stop_recording(c.value("reason", ""));
    return {};
  }
  if (cmd == "_lost") { // from poll(); re-check: the user may have pressed Stop meanwhile
    const auto r = rx();
    const wsdr::RxStatus st = r ? r->status() : wsdr::RxStatus{};
    _lost_requested = false;
    if (!r || st.running || (st.state != wsdr::RxState::Lost && st.state != wsdr::RxState::Error))
      return {};
    _lost_requested = true;
    const bool lost = st.state == wsdr::RxState::Lost;
    stop_recording(lost ? "adapter disconnected" : "stream failed");
    close_adapter("");
    {
      std::lock_guard<std::mutex> l(_sm);
      _error = lost ? "Adapter disconnected. Plug it back in, then press Start."
                    : "The stream kept failing (" + st.error + "). Press Start to try again.";
    }
    _cb.event(caps());
    // kind "lost": the app waits for the adapter and starts it again when it is back
    _cb.event({{"type", "alert"}, {"kind", lost ? "lost" : "failed"}, {"msg", lost ? "Adapter disconnected. Plug it back in, then press Start."
                                                : "The stream kept failing. Press Start to try again."}});
    _cb.event(devices_json(scan_devices()));
    return {};
  }
  throw CmdError("bad_request", "unknown command '" + cmd + "'");
}

void Engine::stop_recording(const std::string &why) {
  _recording = false;
  if (!_rec.stop())
    return; // nothing was recording
  const RecordInfo ri = _rec.info();
  json e = {{"type", "recorded"}, {"file", ri.path}, {"samples", ri.samples},
            {"seconds", ri.fs > 0 ? ri.samples / ri.fs : 0.0}, {"bytes", ri.samples * 4}};
  if (!why.empty())
    e["reason"] = why;
  if (!ri.error.empty())
    e["error"] = ri.error;
  _cb.event(e);
  _cb.event(status());
}

void Engine::open_adapter(uint16_t pid, const json &c) {
  if (pid != wsdr::kPid8812bu && pid != wsdr::kPid8812cu)
    throw CmdError("bad_request", "pid must be 0xb812 or 0xc812");
  if (rx()) { // a second open (another window, a script): tell clients the old session is gone
    close_adapter("the view was started again (another window?)");
    _cb.event(caps());
  }
  wsdr::RxSettings s;
  s.freq = c.contains("freq") ? num(c, "freq", 2.3e9, 6.2e9) : 5180e6;
  s.rate_sel = c.contains("rate_sel") ? integer(c, "rate_sel", 1, 5) : (pid == wsdr::kPid8812cu ? 4 : 2);
  int bus = c.contains("bus") ? integer(c, "bus", 0, 255) : -1;
  std::string port;
  if (c.contains("port")) {
    if (!c["port"].is_string() || c["port"].get<std::string>().size() > 32)
      throw CmdError("bad_request", "port must be a string like \"1\" or \"1.2\"");
    port = c["port"].get<std::string>();
  }
  // Always open one specific device (the first match when bus/port are not given), so caps can say
  // which one is open (a client that did not start it shows the right adapter).
  // (an adapter the other view has open is skipped, or refused when asked for by bus/port)
  const auto key = [](int b, const std::string &p) { return std::to_string(b) + "-" + p; };
  std::unique_lock<std::mutex> serial(_claims->open_mutex); // until this slot has claimed its adapter
  // a failure once 'opening' is shown: back to closed, with the reason (the view must not stay 'starting')
  const auto fail = [&](const char *kind, const std::string &msg) {
    _claims->drop(_slot);
    {
      std::lock_guard<std::mutex> l(_sm);
      _state = "closed";
      _error = msg;
    }
    _cb.event(status());
    throw CmdError(kind, msg);
  };
  DeviceInfo dev;
  bool skipped = false;
  for (const auto &d : scan_devices())
    if (d.pid == pid && (bus < 0 || d.bus == bus) && (port.empty() || d.port == port)) {
      if (_claims->held_by_other(key(d.bus, d.port), _slot)) {
        if (bus >= 0)
          throw CmdError("busy", "this adapter is open in the other view");
        skipped = true;
        continue;
      }
      dev = d;
      bus = d.bus;
      port = d.port;
      break;
    }
  if (bus < 0 && skipped)
    throw CmdError("busy", "every adapter of this kind is open in the other view");
  if (bus < 0)
    fail("not_found", "no such adapter is plugged in");
  if (!_claims->take(key(bus, port), _slot))
    throw CmdError("busy", "this adapter is open in the other view");
  {
    std::lock_guard<std::mutex> l(_sm);
    _state = "opening";
    _error.clear();
  }
  _cb.event(status());
  // An 8812BU that came up at USB 2 (it does after a cold plug) is switched to USB 3 first, like the
  // kernel driver does: it disconnects and re-enumerates on the SuperSpeed side of its port (a new
  // device: one that was not there before the switch).
  const bool usb3 = c.contains("usb3") ? flag(c, "usb3") : _usb3_default.load();
  if (usb3 && dev.pid == wsdr::kPid8812bu && dev.speed_mbps > 0 && dev.speed_mbps < 5000) {
    std::string why;
    const wsdr::Usb3Switch sw = wsdr::request_usb3(pid, bus, port, &why);
    std::fprintf(stderr, "wsdr-studiod: USB 3 switch: %s %s\n", wsdr::usb3_switch_name(sw), why.c_str());
    if (sw == wsdr::Usb3Switch::Requested) {
      _cb.event({{"type", "notice"}, {"text", "Switching the 8812BU to USB 3…"}});
      std::set<std::string> before; // every 8812BU present before (bus, port, device number)
      const auto key = [](const DeviceInfo &d) { return std::to_string(d.bus) + "-" + d.port + "#" + std::to_string(d.devnum); };
      for (const auto &d : scan_devices())
        if (d.pid == pid && key(d) != key(dev))
          before.insert(key(d));
      bool super = false, back = false;
      for (int i = 0; i < 40 && !super && !back; i++) { // up to 8 s
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (const auto &d : scan_devices()) {
          // new: not seen before and not the device we switched; usable: udev has set its permissions
          if (d.pid != pid || before.count(key(d)) || key(d) == key(dev) || !d.can_open ||
              _claims->held_by_other(std::to_string(d.bus) + "-" + d.port, _slot))
            continue;
          if (d.speed_mbps >= 5000 && (!super || d.controller == dev.controller)) {
            bus = d.bus;
            port = d.port;
            super = true;
          } else if (d.speed_mbps < 5000 && d.bus == dev.bus && d.port == dev.port) {
            back = true; // re-enumerated at USB 2 on the same port: no USB 3 here
          }
        }
      }
      if (!super && !back)
        fail("not_found", "the 8812BU did not come back after the USB 3 switch; replug it");
      _cb.event({{"type", "notice"},
                 {"text", super ? "The 8812BU now runs at USB 3 (until it is unplugged)"
                                : "The 8812BU stayed at USB 2: this port or cable may not carry USB 3"}});
      _cb.event(devices_json(scan_devices()));
    } else if (sw == wsdr::Usb3Switch::AlreadyTried) {
      _cb.event({{"type", "notice"},
                 {"text", "The 8812BU is at USB 2; switching to USB 3 already failed once on this plug-in "
                          "(USB 2 port or cable?). Replug it in a USB 3 port to try again."}});
    }
  }
  s.gain_db = c.contains("gain_db") ? num(c, "gain_db", 0, 100) : 30.0;
  s.agc = c.contains("agc") ? flag(c, "agc") : false;
  s.wide = c.contains("wide") ? flag(c, "wide") : true;
  // gapped spans only when asked for (the user picked one); the command line opens without them
  s.allow_gapped = c.contains("allow_gapped") ? flag(c, "allow_gapped") : false;
  if (c.contains("offset_tuning"))
    _offset_tuning = flag(c, "offset_tuning");
  auto r = std::make_shared<wsdr::Receiver>(std::make_shared<Logger>());
  {
    std::lock_guard<std::mutex> l(_sm);
    _cu = pid == wsdr::kPid8812cu;
  }
  if (!_claims->take(key(bus, port), _slot)) // its bus may have changed (USB 3 switch)
    fail("busy", "this adapter is open in the other view");
  serial.unlock();
  try {
    r->open(pid, hw_for(s), bus, port);
  } catch (const wsdr::OpenError &e) {
    fail(open_kind(e.kind), e.what());
  } catch (const std::exception &e) {
    fail("bringup", std::string("bring-up failed: ") + e.what());
  }
  {
    std::lock_guard<std::mutex> l(_dm);
    if (r->caps().paths < 2)
      _dsp_cfg.paths = 1; // an adapter with one path (both supported ones have two)
    _dsp_dirty = true;
  }
  set_view(s, r->settings());
  _ring.clear();
  _lost_requested = false;
  r->start([this](const uint8_t *b, size_t n, const wsdr::BankMeta &m) { _ring.push(b, n, m); });
  {
    std::lock_guard<std::mutex> l(_sm);
    _caps = r->caps();
    _adapter = (_caps.cu ? std::string("RTL8812CU (") : std::string("RTL8812BU (")) + _caps.chip + ")";
    _open_bus = bus;
    _open_port = port;
    _state = "open";
  }
  {
    std::lock_guard<std::mutex> l(_rxm);
    _rx = std::move(r);
  }
  _cb.event(caps());
  _cb.event(settings());
}

// why: a recording still running ends with this reason (it is always saved)
void Engine::close_adapter(const std::string &why) {
  stop_recording(why);
  std::shared_ptr<wsdr::Receiver> old;
  {
    std::lock_guard<std::mutex> l(_rxm);
    old.swap(_rx);
  }
  if (old)
    old->close();
  _ring.clear();
  _claims->drop(_slot);
  std::lock_guard<std::mutex> l(_sm);
  _state = "closed";
  _caps = {};
}

wsdr::RxSettings Engine::hw_for(const wsdr::RxSettings &v) const {
  bool cu;
  {
    std::lock_guard<std::mutex> l(_sm);
    cu = _cu;
  }
  if (!cu || !_offset_tuning || !v.allow_gapped || (v.rate_sel != 2 && v.rate_sel != 3))
    return v;
  if (std::abs(v.freq - std::round(v.freq / 5e6) * 5e6) > 1)
    return v; // off the 5 MHz grid (the UI never sends that): no offset, the notch stays in view
  // 40 MHz capture (the 8822C's BW40 filter is flat to +-17 MHz), radio 10 MHz above the view (below
  // at the top of a band): the view is [-20, 0] or [-15, -5] MHz from the radio's centre, the notch
  // sits at the channel edge or outside. 40 MHz centres: 2422-2462 and 5090-6155 MHz.
  const auto ok = [](double f) { return (f >= 2422e6 && f <= 2462e6) || (f >= 5090e6 && f <= 6155e6); };
  const double up = std::round((v.freq + 10e6) / 5e6) * 5e6, dn = std::round((v.freq - 10e6) / 5e6) * 5e6;
  const double c = ok(up) ? up : ok(dn) ? dn : 0;
  if (c == 0)
    return v; // no room (2.4 GHz channel 14): the notch stays in view
  wsdr::RxSettings h = v;
  h.rate_sel = 1;
  h.wide = true;
  h.freq = c;
  return h;
}

void Engine::set_view(const wsdr::RxSettings &view, const wsdr::RxSettings &hw) {
  // offset-tuned when the view maps to other radio settings (not when the receiver merely clamped)
  const wsdr::RxSettings want = hw_for(view);
  const bool offset = want.freq != view.freq || want.rate_sel != view.rate_sel;
  {
    std::lock_guard<std::mutex> l(_sm);
    _view = offset ? view : hw; // not offset: the radio's own (clamped) settings are the view
  }
  const int w = wsdr::Receiver::width_of(hw, false);
  Crop c;
  c.hw_centre = wsdr::Receiver::centre_of(wsdr::Receiver::channel_for(hw.freq, w), w);
  c.hw_fs = wsdr::Receiver::rate_of(hw.rate_sel);
  if (offset) {
    const double half = wsdr::Receiver::rate_of(view.rate_sel) / 2;
    c.lo = view.freq - half;
    c.hi = view.freq + half;
  }
  std::lock_guard<std::mutex> l(_dm);
  _crop = c;
  _crop_dirty = true;
}

json Engine::settings() const {
  json j = {{"type", "settings"}};
  if (const auto r = rx(); r && r->is_open()) {
    std::lock_guard<std::mutex> l(_sm);
    const wsdr::RxSettings &s = _view;
    j["rx"] = {{"freq", s.freq}, {"rate_sel", s.rate_sel}, {"wide", s.wide}, {"agc", s.agc}, {"gain_db", s.gain_db},
               {"allow_gapped", s.allow_gapped}};
  }
  std::lock_guard<std::mutex> l(_dm);
  j["dsp"] = {{"fft", _dsp_cfg.fft}, {"rows_per_s", _dsp_cfg.rows_per_s}, {"paths", paths_name(_dsp_cfg.paths)},
              {"dc_remove", _dsp_cfg.dc_remove}, {"wifi", _dsp_cfg.wifi}};
  return j;
}

json Engine::caps() const {
  const bool open = rx() != nullptr;
  std::lock_guard<std::mutex> l(_sm);
  json j = {{"type", "caps"}, {"open", open}};
  if (open) {
    json rates = json::array();
    for (int r = 5; r >= std::min(_caps.min_rate_sel, _caps.gapped_min_rate_sel); r--)
      // hw_fs: what the radio captures (10/20 MHz gapped views are cut from an offset-tuned 40 MHz capture)
      rates.push_back({{"rate_sel", r}, {"fs", wsdr::Receiver::rate_of(r)}, {"gapped", r < _caps.min_rate_sel},
                       {"hw_fs", _caps.cu && _offset_tuning && (r == 2 || r == 3) ? 40e6 : wsdr::Receiver::rate_of(r)}});
    j["adapter"] = _adapter;
    j["pid"] = _caps.pid;
    j["bus"] = _open_bus;
    j["port"] = _open_port;
    j["chip"] = _caps.chip;
    j["superspeed"] = _caps.superspeed;
    j["paths"] = _caps.paths;
    j["rates"] = rates;
    j["gain_max"] = _caps.gain_max;
    j["gain_step"] = _caps.gain_step;
    j["wide_capable"] = std::min(_caps.min_rate_sel, _caps.gapped_min_rate_sel) <= 1; // 40 MHz width at 40 MS/s
  }
  return j;
}

json Engine::status() const {
  json j = {{"type", "status"}};
  {
    std::lock_guard<std::mutex> l(_sm);
    j["state"] = _state;
    j["adapter"] = _adapter;
    j["error"] = _error;
  }
  if (const auto r = rx(); r && r->is_open()) {
    const wsdr::RxStatus st = r->status();
    j["rx"] = {{"state", wsdr::rx_state_name(st.state)}, {"banks", st.banks}, {"restarts", st.restarts},
               {"channel", st.channel}, {"width", st.width}, {"centre", st.centre_hz}, {"fs", st.fs},
               {"gapped", st.gapped}, {"error", st.error}};
    wsdr::RxSettings v;
    {
      std::lock_guard<std::mutex> l(_sm);
      v = _view;
    }
    const wsdr::RxSettings hw = hw_for(v);
    if (hw.freq != v.freq || hw.rate_sel != v.rate_sel) { // offset-tuned: report the view, and the radio
      j["rx"]["offset"] = true;
      const int w = wsdr::Receiver::width_of(hw, false); // the radio's settings (not the run still ending)
      j["rx"]["hw_centre"] = wsdr::Receiver::centre_of(wsdr::Receiver::channel_for(hw.freq, w), w);
      j["rx"]["hw_fs"] = wsdr::Receiver::rate_of(hw.rate_sel);
      j["rx"]["centre"] = v.freq;
      j["rx"]["fs"] = wsdr::Receiver::rate_of(v.rate_sel);
    }
  }
  j["ring_dropped"] = _ring.dropped();
  j["rows"] = _rows.load();
  j["dsp_load"] = _dsp_load.load();
  const RecordInfo ri = _rec.info();
  j["record"] = {{"active", ri.active}, {"file", ri.path}, {"samples", ri.samples},
                 {"seconds", ri.fs > 0 ? ri.samples / ri.fs : 0.0}, {"bytes", ri.samples * 4}, {"error", ri.error}};
  if (const double f = _disk_free; f >= 0) // (sampled on its own thread)
    j["disk_free"] = f;
  j["t"] = now_s();
  return j;
}

// free space where recordings go (the nearest existing directory); -1 when unknown
double Engine::disk_free() const {
  std::error_code ec;
  std::filesystem::path d = _rec.dir();
  while (!d.empty() && !std::filesystem::exists(d, ec) && d.has_parent_path() && d != d.parent_path())
    d = d.parent_path();
  struct statvfs v {};
  return statvfs(d.c_str(), &v) == 0 ? static_cast<double>(v.f_bavail) * v.f_frsize : -1;
}

json Engine::devices_json(const std::vector<DeviceInfo> &d) {
  json arr = json::array();
  for (const auto &x : d)
    arr.push_back({{"pid", x.pid}, {"model", x.model}, {"chip", x.chip}, {"bus", x.bus}, {"port", x.port},
                   {"speed_mbps", x.speed_mbps}, {"controller", x.controller}, {"driver", x.driver},
                   {"can_open", x.can_open}, {"max_msps", x.max_msps}, {"gapped_msps", x.gapped_msps}});
  return {{"type", "devices"}, {"devices", arr}};
}

// Wi-Fi frames from the samples the DSP thread hands over: found frames go out in batches as
// {"type":"wifi","frames":[[t, centre MHz, width MHz, format, rate Mb/s, length, airtime us, SNR dB
// (, sender, BSSID, network name, channel: 6 Mb/s frames decoded whole)]...],
// "other":[[t, centre MHz, width MHz, kind (T Bluetooth, N narrowband, M microwave oven, E BLE
// advertising, C steady carrier), us (, E: address, random, name; C: null, null, null, comb kHz)]...],
// "dropped": banks skipped because this thread fell behind}.
void Engine::wifi_loop() {
  wifi::Decoder dec;
  sig::Finder sf; // other signals in the same samples (Bluetooth, narrowband, a microwave oven)
  std::vector<sig::Burst> other;
  uint32_t gen = ~0u;
  uint64_t next = 0;
  double fs = 0, centre = 0, sent = 0;
  std::vector<wifi::Frame> found;
  uint64_t dropped = 0;
  for (;;) {
    WifiChunk w;
    {
      std::unique_lock<std::mutex> l(_wm);
      _wcv.wait_for(l, std::chrono::milliseconds(100), [&] { return _wquit || !_wq.empty(); });
      if (_wquit)
        return;
      if (!_wq.empty()) {
        w = std::move(_wq.front());
        _wq.pop_front();
      }
    }
    if (!w.x.empty()) {
      // a new run, or a gap in the samples (banks dropped): no frame spans it
      if (w.gen != gen || w.fs != fs || w.centre != centre || w.seq != next) {
        gen = w.gen;
        fs = w.fs;
        centre = w.centre;
        dec.reset(fs, centre);
        sf.reset(fs, centre);
      }
      next = w.seq + 1;
      dec.feed(w.x.data(), w.x.size(), w.t_end, found);
      sf.feed(w.x.data(), w.x.size(), w.t_end, other);
      std::lock_guard<std::mutex> l(_wm);
      if (_wpool.size() < 8)
        _wpool.push_back(std::move(w.x));
    }
    const double now = now_s();
    const uint64_t d = _wifi_dropped.load();
    if ((!found.empty() || !other.empty() || d != dropped) && (now - sent > 0.2 || found.size() > 400)) {
      json arr = json::array();
      auto hex = [](const std::array<uint8_t, 6> &a) {
        char b[18];
        std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
        return std::string(b);
      };
      for (const auto &f : found) {
        json e = {std::round(f.t * 1e6) / 1e6, f.centre_hz / 1e6, f.bw_mhz, std::string(1, f.fmt), f.rate_mbps, f.length,
                  std::round(f.dur_us), std::round(f.snr_db * 10) / 10};
        if (f.mac) { // decoded whole: sender, and for beacons the network
          e.push_back(f.has_ta ? json(hex(f.ta)) : json(nullptr));
          e.push_back(f.beacon ? json(hex(f.bssid)) : json(nullptr));
          e.push_back(f.beacon ? json(f.ssid) : json(nullptr));
          e.push_back(f.channel);
        }
        arr.push_back(e);
      }
      json oth = json::array();
      for (const auto &o : other) {
        json e = {std::round(o.t * 1e6) / 1e6, std::round(o.mhz * 100) / 100, std::round(o.bw_mhz * 100) / 100, std::string(1, o.kind),
                  std::round(o.dur_us)};
        if (o.kind == 'E') { // a decoded BLE advertisement: who sent it
          e.push_back(o.addr);
          e.push_back(o.random);
          e.push_back(o.name);
        } else if (o.kind == 'C') { // a steady carrier: the comb around it
          e.insert(e.end(), {nullptr, nullptr, nullptr, std::round(o.comb_khz * 10) / 10});
        }
        oth.push_back(e);
      }
      try {
        _cb.event({{"type", "wifi"}, {"frames", arr}, {"other", oth}, {"dropped", d}});
      } catch (const std::exception &) {
        // a frame's text the JSON writer refused: this batch is lost, the thread goes on
      }
      found.clear();
      other.clear();
      dropped = d;
      sent = now;
    }
  }
}

void Engine::dsp_loop() {
  Spectrum sp;
  uint32_t gen = ~0u, cfg = ~0u;
  std::vector<int16_t> iq(2 * 8192);
  std::vector<wsdr::cf> tmp(8192);
  double busy = 0;
  auto win = clk::now();
  // Ask the command thread to stop the recording (once); stop feeding it right away.
  uint64_t rec_seen = 0; // the recording rec_next belongs to
  auto request_stop = [&](const char *reason) {
    _recording = false;
    if (!_rec_stop_requested.exchange(true))
      submit({{"cmd", "_rec_stop"}, {"reason", reason}, {"rec", rec_seen}}, nullptr); // the file the bank was checked for
  };
  uint64_t rec_next = 0; // next bank seq expected in the file (0 = not started)
  Crop applied;          // the crop window in use, and the hardware run it belongs to
  while (!_dsp_quit) {
    Bank *b = _ring.pop(std::chrono::milliseconds(100));
    const auto t0 = clk::now();
    if (b) {
      {
        std::lock_guard<std::mutex> l(_dm);
        if (_dsp_dirty) {
          sp.configure(_dsp_cfg);
          _dsp_dirty = false;
        }
        // A crop window belongs to one hardware run (centre, fs): it is taken over from the first bank
        // of that run, and banks of any other run are not cropped (fast retunes: no stale window).
        const auto same = [&](const Crop &c) {
          return std::abs(b->meta.centre_hz - c.hw_centre) < 1 && std::abs(b->meta.fs - c.hw_fs) < 1;
        };
        if (_crop_dirty && same(_crop)) {
          applied = _crop;
          _crop_dirty = false;
        }
        const bool crop = applied.hi > applied.lo && same(applied);
        sp.set_crop(crop ? applied.lo : 0, crop ? applied.hi : 0);
      }
      gen = b->meta.gen;
      cfg = b->meta.cfg;
      sp.process(b->data.data(), b->bytes, b->meta, [&](const Row &r) {
        _rows++;
        _cb.row(encode_row(r, now_s()));
      });
      // Wi-Fi frames: a copy of path A, stamped on the rows' clock (a row is stamped when its last
      // samples are processed, here, so a frame lands in the row that holds it)
      if (_wifi_on && !b->meta.gapped && (std::abs(b->meta.fs - 40e6) < 1e3 || std::abs(b->meta.fs - 20e6) < 1e3)) {
        WifiChunk w;
        bool room = true;
        {
          std::lock_guard<std::mutex> l(_wm);
          room = _wq.size() < 256; // ~50 ms at 40 MS/s
          if (room && !_wpool.empty()) {
            w.x = std::move(_wpool.back());
            _wpool.pop_back();
          }
        }
        if (!room) {
          _wifi_dropped++;
          goto wifi_done;
        }
        w.x.resize(b->bytes / 8);
        Spectrum::unpack(b->data.data(), w.x.size(), 0, b->meta.cu, w.x.data());
        w.t_end = now_s();
        w.fs = b->meta.fs;
        w.centre = b->meta.centre_hz;
        w.gen = b->meta.gen;
        w.seq = b->meta.seq;
        std::lock_guard<std::mutex> l(_wm);
        _wq.push_back(std::move(w));
        _wcv.notify_one();
      }
    wifi_done:;
      // Every file has one centre, one rate and no gaps. Banks of the run the recording belongs to go
      // in; older ones (still queued from before a retune) are skipped; a newer run, or a gap in the
      // bank sequence (the DSP fell behind and the ring dropped banks), ends the file.
      // read _recording before _rec_id: start() bumps the id before setting _recording, so a bank that
      // sees the recording also sees its id
      const bool recording = _recording;
      if (_rec_id != rec_seen) { // a new recording: its first bank starts the sequence
        rec_seen = _rec_id;
        rec_next = 0;
      }
      if (!recording) {
        rec_next = 0;
      } else if (gen > _rec_gen) {
        request_stop(cfg != _rec_cfg ? "settings changed" : "the stream restarted after a fault");
      } else if (gen == _rec_gen && rec_next && b->meta.seq != rec_next) {
        request_stop("samples were lost (the computer could not keep up); the file ends before the gap");
      } else if (gen == _rec_gen) {
        rec_next = b->meta.seq + 1;
        const int path = b->meta.paths > 1 ? _rec_path.load() : 0;
        const size_t ns = b->bytes / 8;
        Spectrum::unpack(b->data.data(), ns, path, b->meta.cu, tmp.data());
        for (size_t i = 0; i < ns; i++) {
          iq[2 * i] = static_cast<int16_t>(static_cast<int>(tmp[i].real()) * 16);
          iq[2 * i + 1] = static_cast<int16_t>(static_cast<int>(tmp[i].imag()) * 16);
        }
        _rec.push(iq.data(), ns);
        if (_rec.failed())
          request_stop("the recording could not be written");
        else if (_rec.dropped())
          request_stop("the disk could not keep up; the file ends before the first lost sample");
      }
      _ring.release(b);
      busy += std::chrono::duration<double>(clk::now() - t0).count();
    }
    const double el = std::chrono::duration<double>(clk::now() - win).count();
    if (el >= 1.0) {
      _dsp_load = busy / el;
      busy = 0;
      win = clk::now();
    }
  }
}

} // namespace studio
