// SPDX-License-Identifier: GPL-2.0-only
#include "tap.h"

#include <cstring>

#include "protocol.h"

namespace studio {

using json = nlohmann::json;

Tap::Tap(TapKind kind, int slot, BinarySink out, EventSink event)
    : _kind(std::move(kind)), _slot(slot), _out(std::move(out)), _event(std::move(event)) {
  _status = {{"type", "tap"}, {"kind", _kind.name}, {"on", false}, {"watch", 0}};
  _t = std::thread([this] { loop(); });
}

Tap::~Tap() {
  {
    std::lock_guard<std::mutex> l(_m);
    _quit = true;
  }
  _cv.notify_all();
  if (_t.joinable())
    _t.join();
}

uint32_t Tap::watch(double freq, const json &params) {
  const json p = params.is_object() ? params : json::object();
  if (freq > 0 && _kind.check)
    _kind.check(p);
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(_m);
    _watch = {freq > 0 ? freq : 0, p, freq > 0 ? _watch.id + 1 : _watch.id}; // (a stop ends the current one)
    if (freq <= 0)
      _stopped = true;
    id = _watch.id;
    _dropped = 0;
    _span_ok = true;
    _freq = _watch.freq;
  }
  _cv.notify_all();
  return id;
}

void Tap::push(const std::complex<float> *x, size_t n, const wsdr::BankMeta &m) {
  if (!on())
    return;
  const bool ok = accepts(m);
  _span_ok = ok;
  if (!ok || !x)
    return;
  Chunk c;
  {
    std::lock_guard<std::mutex> l(_m);
    if (_q.size() >= 512) { // ~100 ms at 40 MS/s
      _dropped++;
      return;
    }
    if (!_pool.empty()) {
      c.x = std::move(_pool.back());
      _pool.pop_back();
    }
    c.watch = _watch.id;
  }
  c.x.assign(x, x + n);
  c.fs = m.fs;
  c.centre = m.centre_hz;
  c.gen = m.gen;
  std::lock_guard<std::mutex> l(_m);
  _q.push_back(std::move(c));
  _cv.notify_one();
}

uint32_t Tap::stop(uint32_t id) {
  {
    std::lock_guard<std::mutex> l(_m);
    if (id != _watch.id)
      return _watch.id;
  }
  return watch(0);
}

json Tap::status() const {
  std::lock_guard<std::mutex> l(_m);
  return _status;
}

void Tap::loop() {
  std::unique_ptr<TapDecoder> dec;
  uint32_t gen = ~0u, id = 0;
  double fs = 0, centre = 0, sent = 0;
  int was = 0; // what the last status said: 0 off, 1 on, 2 failed
  std::string error;
  for (;;) {
    Chunk c;
    Watch w; // the watch this chunk is for: freq, params and id read together
    bool fresh = false; // a new watch: its status goes out now
    {
      std::unique_lock<std::mutex> l(_m);
      // (woken by a new watch, and by a stop: its off status goes out at once)
      _cv.wait_for(l, std::chrono::milliseconds(100),
                   [&] { return _quit || !_q.empty() || _watch.id != id || _stopped || (_watch.freq > 0) != (was == 1); });
      if (_quit)
        return;
      if (_watch.id != id) { // a new watch (or a stop): a new decoder
        dec.reset();
        fs = 0;
        error.clear();
        fresh = true;
      }
      if (_stopped) { // (a failed watch, stopped: its error is over)
        _stopped = false;
        error.clear();
      }
      if (!_q.empty()) {
        c = std::move(_q.front());
        _q.pop_front();
        if (c.watch != _watch.id) { // pushed for an earlier watch: not this one's to decode
          if (_pool.size() < 16)
            _pool.push_back(std::move(c.x));
          c.x = {};
        }
      }
      w = _watch;
    }
    id = w.id;
    const bool on = w.freq > 0;
    if (!on)
      dec.reset();
    try {
      if (!c.x.empty() && on) {
        if (!dec || c.gen != gen || c.fs != fs || c.centre != centre) { // a new stream run: a new decoder
          gen = c.gen;
          fs = c.fs;
          centre = c.centre;
          // (its frames get the view slot, byte 6, and this watch's id, offset 8)
          dec = _kind.make(fs, w.freq - centre, w.params, [this, wid = w.id](std::vector<uint8_t> &&f) {
            if (f.size() >= 12) {
              f[6] = static_cast<uint8_t>(_slot);
              put_le<uint32_t>(f, 8, wid);
            }
            _out(std::move(f));
          });
        }
        dec->feed(c.x.data(), c.x.size());
      }
    } catch (const std::exception &e) { // (a decoder that fails stops its tap; the backend goes on)
      error = e.what();
      dec.reset();
      std::lock_guard<std::mutex> l(_m);
      if (_watch.id == id) {
        _freq = 0;
        _watch.freq = 0;
        _q.clear();
      }
    }
    if (c.x.capacity()) {
      std::lock_guard<std::mutex> l(_m);
      if (_pool.size() < 16)
        _pool.push_back(std::move(c.x));
    }
    // the status: for every new watch, whenever it goes on / off / fails, and twice a second while on
    const double now = now_s();
    const int cur = !error.empty() ? 2 : on ? 1 : 0;
    if (fresh || cur != was || (cur == 1 && now - sent >= 0.5)) {
      json j = {{"type", "tap"}, {"kind", _kind.name}, {"on", on && error.empty()}, {"watch", id}};
      if (!error.empty()) {
        j.update({{"freq", w.freq}, {"params", w.params}, {"state", "error"}, {"error", error}});
      } else if (on) {
        const bool outside = fs > 0 && std::abs(w.freq - centre) > fs / 2 - _kind.margin_hz;
        j.update({{"freq", w.freq}, {"params", w.params}, {"dropped", _dropped.load()}});
        try {
          if (dec)
            j.update(dec->stats());
        } catch (const std::exception &) {
        }
        if (!_span_ok)
          j["state"] = "span";
        else if (outside)
          j["state"] = "outside";
        else if (!j.contains("state"))
          j["state"] = "searching";
      }
      {
        std::lock_guard<std::mutex> l(_m);
        _status = j;
      }
      _event(j);
      sent = now;
      was = cur;
    }
  }
}

} // namespace studio
