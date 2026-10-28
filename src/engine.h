// SPDX-License-Identifier: GPL-2.0-only
// Engine: the app's model. Owns the Receiver (one adapter), the bank ring, the DSP thread and the
// recorder, and executes commands on its own command thread (bring-up blocks for ~5 s; commands that
// arrive meanwhile queue up behind it).
//
// Threads: command thread (open/close/set/record, and every recorder start/stop), stream thread
// (inside Receiver: copies banks into the ring), DSP thread (unpack, FFT, rows, recorder feed; it only
// *requests* recorder stops). Rows and events go out through callbacks, which must not block.
#pragma once

#include <atomic>
#include <complex>
#include <condition_variable>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "device_scan.h"
#include "dsp.h"
#include "receiver.h"
#include "recorder.h"
#include "ring.h"
#include "wifi.h"

namespace studio {

using json = nlohmann::json;

// Which adapter each engine (view slot) has open: two slots must never open the same dongle.
class Claims {
public:
  // Held by an opening engine from choosing its adapter until it has claimed it (USB 3 switch
  // included): two slots opening at once never pick, or re-find, the same dongle.
  std::mutex open_mutex;

  bool take(const std::string &key, int slot) { // false: another slot holds it
    std::lock_guard<std::mutex> l(_m);
    for (auto it = _held.begin(); it != _held.end();)
      it = it->second == slot ? _held.erase(it) : std::next(it); // a slot holds one adapter at most
    if (_held.count(key))
      return false;
    _held[key] = slot;
    return true;
  }
  void drop(int slot) {
    std::lock_guard<std::mutex> l(_m);
    for (auto it = _held.begin(); it != _held.end();)
      it = it->second == slot ? _held.erase(it) : std::next(it);
  }
  bool held_by_other(const std::string &key, int slot) const {
    std::lock_guard<std::mutex> l(_m);
    const auto it = _held.find(key);
    return it != _held.end() && it->second != slot;
  }

private:
  mutable std::mutex _m;
  std::map<std::string, int> _held; // "bus-port" -> slot
};

class Engine {
public:
  struct Callbacks {
    std::function<void(std::vector<uint8_t> &&frame)> row;  // binary row frame (protocol.h)
    std::function<void(const json &event)> event;           // JSON event for all clients
  };
  // slot: which view this engine serves (0 = A, 1 = B); claims: shared between the slots' engines.
  Engine(Callbacks cb, std::string record_dir, int slot = 0, std::shared_ptr<Claims> claims = nullptr);
  // Start switches an 8812BU found at USB 2 to USB 3 unless the open command says otherwise
  // (its "usb3" field) or this default is off (--no-usb3).
  void set_usb3_default(bool on) { _usb3_default = on; }
  ~Engine();

  // A command from a client ({"cmd": ..., ...}); `reply` gets {"type":"ack"|"error", ...}. Non-blocking.
  void submit(json cmd, std::function<void(const json &)> reply);
  // Call periodically (any thread): notices a lost adapter and closes it; logs a stream fault.
  void poll();
  // An open requested from the command line failed: the reason goes in status (no client was there
  // to get the reply).
  void note_failure(const std::string &msg);
  json status() const;   // {"type":"status", ...}
  json settings() const; // {"type":"settings", rx, dsp}
  json caps() const;     // {"type":"caps", ...} (open:false if closed)
  static json devices_json(const std::vector<DeviceInfo> &d);

  uint64_t rows_emitted() const { return _rows; }
  uint64_t ring_dropped() const { return _ring.dropped(); }
  double dsp_load() const { return _dsp_load; }

private:
  void cmd_loop();
  void dsp_loop();
  json run(const json &c); // on the command thread; throws -> error reply
  void open_adapter(uint16_t pid, const json &c);
  void close_adapter(const std::string &why);
  double disk_free() const;
  void stop_recording(const std::string &why);
  // Offset tuning (8812CU 10/20 MHz views): the chip's centre notch (~+-0.6 MHz, deeper at higher
  // rates) is kept out of view by capturing 40 MHz with the radio 10 MHz to the side and showing only
  // the window asked for. hw_for: the radio settings for a view; set_view: remember the view and
  // hand the crop window to the DSP thread.
  wsdr::RxSettings hw_for(const wsdr::RxSettings &view) const;
  void set_view(const wsdr::RxSettings &view, const wsdr::RxSettings &hw); // hw: the receiver's settings
  std::shared_ptr<wsdr::Receiver> rx() const {
    std::lock_guard<std::mutex> l(_rxm);
    return _rx;
  }

  Callbacks _cb;
  int _slot = 0;
  std::shared_ptr<Claims> _claims;
  mutable std::mutex _rxm;
  std::shared_ptr<wsdr::Receiver> _rx; // replaced on the command thread, read anywhere via rx()
  BankRing _ring{512};
  Recorder _rec;

  std::mutex _qm;
  std::condition_variable _qcv;
  std::deque<std::pair<json, std::function<void(const json &)>>> _q;
  bool _quit = false;
  std::condition_variable _diskcv;
  std::thread _cmd, _dsp, _disk, _wifi;

  // Wi-Fi frames: the DSP thread hands copies of path A's samples to their own thread (dropped when it
  // falls behind), which sends what it finds as "wifi" events, a few times a second
  struct WifiChunk {
    std::vector<std::complex<float>> x;
    double t_end = 0, fs = 0, centre = 0;
    uint32_t gen = 0;
    uint64_t seq = 0; // the bank's: a gap (dropped here or in the ring) starts the finder again
  };
  void wifi_loop();
  std::mutex _wm;
  std::condition_variable _wcv;
  std::deque<WifiChunk> _wq;
  std::vector<std::vector<std::complex<float>>> _wpool; // spare buffers
  bool _wquit = false;
  std::atomic<bool> _wifi_on{false};
  std::atomic<uint64_t> _wifi_dropped{0};
  // The latest frames with their equalised points, for the frame inspector ("points"): a frame's id
  // counts the frames this engine reported ("wifi" events carry their first frame's as "id0")
  static constexpr size_t kKeptPoints = 4'000'000; // ~16 MB: some thousands of frames
  static constexpr size_t kKeptMin = 64;            // what a frame without points (802.11b) counts
  std::mutex _km;
  std::deque<std::pair<uint64_t, wifi::Frame>> _kept;
  size_t _kept_points = 0;
  uint64_t _next_frame = 0;
  json points(uint64_t id);

  mutable std::mutex _dm; // DSP settings handoff (command thread -> DSP thread)
  DspConfig _dsp_cfg;
  bool _dsp_dirty = true;
  struct Crop {
    double lo = 0, hi = 0;          // 0, 0 = whole band
    double hw_centre = 0, hw_fs = 0; // applied from the first bank of this hardware run on
  } _crop;
  bool _crop_dirty = false;
  std::atomic<bool> _usb3_default{true};
  // Off by default: the view is what the adapter's own receiver sees (its centre notch included).
  // On: the 8812CU's 10/20 MHz gapped views are offset-tuned to keep the notch out (hw_for).
  std::atomic<bool> _offset_tuning{false};
  std::atomic<bool> _dsp_quit{false};

  std::atomic<bool> _recording{false}; // DSP thread feeds the recorder while set
  std::atomic<int> _rec_path{0};       // path fixed when the recording started
  std::atomic<uint32_t> _rec_gen{0}, _rec_cfg{0}; // the stream run the file belongs to
  std::atomic<uint64_t> _rec_id{0};               // counts recordings: stale stop requests are ignored
  uint64_t _seen_restarts = 0;                    // poll(): stream faults logged so far
  std::atomic<bool> _rec_stop_requested{false}, _lost_requested{false};

  mutable std::mutex _sm; // status strings
  std::string _state = "closed", _error, _adapter;
  wsdr::RxSettings _view; // what the user asked for (== the radio's settings unless offset-tuned)
  bool _cu = false;
  int _open_bus = -1;     // the open device (caps: which adapter is open)
  std::string _open_port;
  wsdr::RxCaps _caps;

  std::atomic<uint64_t> _rows{0};
  std::atomic<double> _dsp_load{0};
  std::atomic<double> _disk_free{-1}; // bytes free where recordings go (sampled every 2 s)
};

} // namespace studio
