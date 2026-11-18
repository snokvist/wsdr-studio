// SPDX-License-Identifier: GPL-2.0-only
// Taps: decoders that follow one signal in a view's samples and turn it into something to see or hear
// (analog video now; an FM/AM audio receiver would be another kind). The engine hands every bank to its
// taps; a tap that is on copies path A's samples to a thread of its own, where its decoder runs, and
// sends what the decoder makes to the page as binary frames, with a status event twice a second.
//
// What a tap does for every kind: the handoff from the DSP thread (path A's shared samples, in batches;
// dropping while its thread is behind),
// a new decoder after every retune or rate change (it is told the signal's offset from the run's centre),
// the rates it accepts, whether the signal is in the span at all, the "tap" command and the status.
// A kind adds its decoder (TapDecoder) and its binary frame (its own magic; the first 8 bytes as below).
//
// Binary tap frames (little-endian) all start with: off 0 u32 magic (the kind's), 4 u16 version, 6 u8
// view slot, 7 u8 flags (the kind's), 8 u32 watch (which watch the frame is of); the Tap writes the slot
// and the watch, the kind the rest (video.h).
//
// Status event: {"type":"tap", "kind", "on", "watch" (which watch it is of: the "tap" command's reply
// carries the id; a stop's off status carries the id of the watch it ended), and while on: "freq" (Hz), "params" (as given), "state" ("span": not a span this
// kind decodes; "outside": the signal is not inside the span; "error": the decoder failed, "error" says
// why, and the tap is off; else the decoder's, e.g. "searching" / "locked"), "dropped" (banks skipped:
// its thread fell behind), and the decoder's own fields}.
#pragma once

#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "receiver.h"
#include "samples.h"

namespace studio {

using BinarySink = std::function<void(std::vector<uint8_t> &&)>;
using EventSink = std::function<void(const nlohmann::json &)>;

// One decoder for one stream run (a retune or rate change builds a new one). Runs on the tap's thread.
class TapDecoder {
public:
  virtual ~TapDecoder() = default;
  // path A's samples at the run's rate, centred on the run's centre (the signal sits at shift_hz)
  virtual void feed(const std::complex<float> *x, size_t n) = 0;
  // its fields of the status event; "state" among them ("searching", "locked", ...)
  virtual nlohmann::json stats() const = 0;
};

struct TapKind {
  std::string name; // the command's and the status event's "kind"
  // the rates its decoder takes (continuous spans only: a gapped bank never reaches it)
  std::function<bool(double fs)> rate_ok;
  double margin_hz = 0; // how far inside the span's edge the signal's frequency must be
  // a decoder for a run at fs with the signal at shift_hz; params: the "tap" command's, checked by check
  std::function<std::unique_ptr<TapDecoder>(double fs, double shift_hz, const nlohmann::json &params,
                                            const BinarySink &out)>
      make;
  std::function<void(const nlohmann::json &params)> check; // throws std::invalid_argument; may be empty
};

class Tap {
public:
  Tap(TapKind kind, int slot, BinarySink out, EventSink event);
  ~Tap();
  Tap(const Tap &) = delete;
  Tap &operator=(const Tap &) = delete;

  const std::string &name() const { return _kind.name; }
  // Follow the signal at freq (Hz) with these params (the kind's; checked first: throws
  // std::invalid_argument); freq 0 stops. Returns this watch's id (its status events and frames carry
  // it; a stop returns the id of the watch it ended).
  uint32_t watch(double freq, const nlohmann::json &params = nlohmann::json::object());
  // Stop the watch `id` if it is still the current one (another may have replaced it: that one goes on);
  // returns the current watch's id.
  uint32_t stop(uint32_t id);
  bool on() const { return _freq.load() > 0; }
  // DSP thread, every bank: does it want this bank's path A (on, and a span its kind decodes)?
  bool accepts(const wsdr::BankMeta &m) const { return on() && !m.gapped && _kind.rate_ok(m.fs); }
  // DSP thread, every bank: path A's samples (x: may be null when accepts() was false), shared with the
  // tap's thread (no copy), or dropped while it is behind (the decoder finds the sync again after the gap).
  void push(const Samples &x, const wsdr::BankMeta &m);
  nlohmann::json status() const; // the latest status event

private:
  static constexpr size_t kBatch = 8;       // banks a wake-up of the tap's thread takes (~1.6 ms at 40 MS/s)
  static constexpr size_t kMaxTake = 32;    // banks decoded between looks at the watch and the status (~6.5 ms)
  static constexpr size_t kMaxQueued = 512; // ~100 ms at 40 MS/s waiting (plus the kMaxTake being decoded)
  struct Chunk {
    Samples x;
    double fs = 0, centre = 0;
    uint32_t gen = 0;
    uint32_t watch = 0; // the watch it was pushed under (an older watch's chunks are dropped)
  };
  void loop();

  TapKind _kind;
  int _slot;
  BinarySink _out;
  EventSink _event;
  std::atomic<double> _freq{0};       // (for on() on the DSP thread; the tap's thread reads _watch)
  std::atomic<uint32_t> _wid{0};      // _watch.id, for the tap's thread mid-batch (with _freq: a stop or a new watch ends the batch)
  struct Watch {
    double freq = 0;
    nlohmann::json params = nlohmann::json::object();
    uint32_t id = 0; // counts watch() calls: each one builds a new decoder
  } _watch;           // under _m
  std::atomic<bool> _span_ok{true};   // the last bank seen was one the kind decodes
  std::atomic<uint64_t> _dropped{0};

  mutable std::mutex _m; // queue, watch, status
  std::condition_variable _cv;
  std::deque<Chunk> _q;
  nlohmann::json _status;
  bool _stopped = false; // a stop since the loop last looked (it clears a failed watch's error)
  bool _quit = false;
  std::thread _t;
};

} // namespace studio
