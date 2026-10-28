// SPDX-License-Identifier: GPL-2.0-only
// Spectrum DSP: LA banks -> per-path complex samples (unpack, DC block) -> windowed FFT (FFTW3f) -> rows
// for the display.
//
// The rows always cover the HARDWARE band (centre +- fs/2). The requested frequency may sit off the
// 5 MHz synthesizer grid; it is not shifted to the middle, because at 2.5/5 MS/s a residual of up to
// +-2.5 MHz would wrap the band around (the UI marks the requested frequency instead).
//
// Power is accumulated LINEAR per row interval (sum for the average, max for the peak trace) and
// converted to dB once per row. Above kMaxBins FFT bins, each display bin carries the summed power of
// the FFT bins it covers (noise in display units is then independent of the FFT size; a tone reads
// up to +1.8 dB high, the Hann leakage into the neighbour bin) and the max of their peaks.
// dBFS: a full-scale complex sine (|x| = 2048 LSB) reads 0 dBFS (coherent-gain normalised window).
//
// Gapped runs (BankMeta::gapped, 8812CU wide spans): every bank is a separate capture. The FFT is
// capped at one bank (8192, a power of two, so frames never straddle a gap), and rows are timed by
// the clock (samples arrive at only a fraction of fs).
#pragma once

#include <chrono>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "iqdsp.h"
#include "receiver.h"

namespace studio {

// FFTW's planner is not thread-safe: every plan is made and destroyed under this lock.
std::mutex &fftw_plan_mutex();

constexpr int kMaxBins = 4096;
constexpr int kGappedMaxFft = 8192; // one LA bank (BankMeta::gapped)

struct DspConfig {
  int fft = 8192;         // 1024 .. 16384, power of two
  double rows_per_s = 25; // display rows per second (1..30)
  int paths = 1;          // bit 0 = path A, bit 1 = path B
  bool dc_remove = true;
  bool wifi = false;      // find Wi-Fi frames in the samples (engine.cpp: its own thread)
};

struct Row {
  int path = 0;           // 0 = A, 1 = B
  uint32_t seq = 0;
  uint8_t gen = 0;        // low byte of the stream run (BankMeta.gen): rows of a new run differ
  bool gapped = false;    // from a gapped run (BankMeta::gapped)
  bool offset = false;    // cropped from an offset-tuned capture (the hardware centre is outside)
  int slot = 0;           // the view it belongs to (0 = A, 1 = B; the engine sets it when encoding)
  double f0 = 0, df = 0;  // Hz of bin 0 (lowest frequency) and bin spacing
  std::vector<float> avg, max; // dBFS, size bins
};

class Spectrum {
public:
  Spectrum();
  ~Spectrum();
  void configure(const DspConfig &c);
  // Measure FFT plans for every size once (a second or so, on a thread of its own at startup): the
  // paths then use measured plans. Without it they use estimated ones, about twice as slow.
  static void warm_plans();
  // One raw LA bank. Resets state when meta.gen changes (new tuning / rate).
  void process(const uint8_t *bank, size_t bytes, const wsdr::BankMeta &meta,
               const std::function<void(const Row &)> &emit);
  uint64_t ffts() const { return _ffts; }
  int fft_size() const { return _c.fft; } // in use (gapped runs cap it at one bank)
  // Emit only [lo, hi] Hz of the band (offset tuning: the window the user asked for); 0, 0 = all.
  void set_crop(double lo, double hi) {
    _crop_lo = lo;
    _crop_hi = hi;
  }
  // Time source for gapped rows (seconds; tests replace it).
  std::function<double()> clock = [] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };

  // Unpack one path (0 = A, 1 = B) of a bank; cu: the 8822C's word layout (BankMeta::cu), whose path B
  // sits elsewhere than the 8822B's (wifi-sdr path_b_i/path_b_q).
  static void unpack(const uint8_t *bank, size_t samples, int path, bool cu, wsdr::cf *out);

private:
  struct Path;
  void reset(const wsdr::BankMeta &m);
  void rebuild();

  DspConfig _c;
  int _req_fft = 8192;    // the configured size (_c.fft may be capped for a gapped run)
  double _crop_lo = 0, _crop_hi = 0;
  bool _gapped = false;
  uint32_t _gen = ~0u;
  double _fs = 0, _freq = 0;
  std::vector<float> _win;
  float _norm = 1;
  std::vector<std::unique_ptr<Path>> _paths;
  uint64_t _ffts = 0;
  uint32_t _seq = 0;
  std::vector<wsdr::cf> _tmp;
};

} // namespace studio
