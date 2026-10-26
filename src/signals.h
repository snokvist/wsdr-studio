// SPDX-License-Identifier: GPL-2.0-only
// Signals other than Wi-Fi in the raw I/Q, tagged by cheap rules (after the Airshark idea): narrow
// bursts from a short-time spectrum (Bluetooth when they hop, else "narrowband"), and a microwave
// oven from the span's activity switching with the mains. A narrow burst on a BLE advertising channel
// is decoded (its device address and name). A narrow line that stays (a steady carrier) is reported
// with the spacing of the comb around it, if any (a video line rate: a display clock's harmonic).
// Receive only.
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

typedef struct fftwf_plan_s *fftwf_plan;

namespace sig {

using cf = std::complex<float>;

struct Burst {
  double t = 0;      // start (s), on the clock of the samples fed in
  double mhz = 0;    // centre
  double bw_mhz = 0; // width
  double dur_us = 0;
  // 'T' Bluetooth (narrow bursts hopping), 'N' other narrowband, 'M' microwave oven, 'E' BLE advertising
  // (decoded: CRC good), 'C' a steady carrier (every 200 ms while it lasts)
  char kind = 'N';
  double comb_khz = 0; // 'C': the spacing of evenly spaced lines around it (0: none)
  std::string addr;  // 'E': the advertiser's address (aa:bb:..., as shown by phones)
  bool random = false; // 'E': a random (often rotating) address rather than a public one
  std::string name;  // 'E': its name, if it sends one (UTF-8 checked)
};

class Finder {
public:
  Finder() = default;
  Finder(const Finder &) = delete;
  Finder &operator=(const Finder &) = delete;
  ~Finder();
  // fs: 20 or 40 MS/s (anything else finds nothing); centre_hz: the samples' centre frequency.
  void reset(double fs, double centre_hz);
  // Contiguous samples, in order; t_end: the time (s) just after the last one. Bursts are appended to
  // `out` once they have ended (an oven once per 200 ms while it runs).
  void feed(const cf *x, size_t n, double t_end, std::vector<Burst> &out);

private:
  void release();
  void column(const cf *x, std::vector<Burst> &out);
  void end_track(size_t i, std::vector<Burst> &out);
  // the advertising channels inside the span: activity in their three bins, gaps up to 64 us bridged
  struct AdvChan {
    int chan, bin;                   // 37..39; the bin at its centre
    double mhz;
    bool on = false;
    uint64_t c0 = 0, c1 = 0;         // first and last active column
    std::vector<cf> taps;            // the channel to 0 Hz and ~600 kHz wide (mixer and filter in one)
    double step = 0;                 // the mixer's phase step per sample
    std::vector<uint8_t> white;      // its whitening sequence
  };
  std::vector<AdvChan> _advch;
  int _bdec = 0;                     // to 4 MS/s: 10 or 5
  struct Adv {
    uint64_t s0;                     // where the activity started
    size_t ch;                       // its channel (_advch)
    Burst b;
  };
  std::vector<Adv> _adv;             // activity on an advertising channel, waiting for its samples
  bool ble(uint64_t s0, uint64_t s1, const AdvChan &a, Burst &b) const; // decode samples [s0, s1)
  std::vector<Burst> _held;          // narrow bursts on an advertising channel, until its decodes are in
  std::vector<std::pair<double, double>> _decoded; // decoded packets' times (s): a burst over one is it
  void oven(std::vector<Burst> &out); // every 200 ms
  void carriers(std::vector<Burst> &out); // every 200 ms
  double time_of(uint64_t sample) const { return _t_end - static_cast<double>(_abs_end - sample) / _fs; }
  double bin_hz(double k) const { return _centre + (k - 0.5 * _nf) * _fs / _nf; }

  double _fs = 0, _centre = 0, _t_end = 0;
  int _nf = 0, _hop = 0;             // FFT size (625 kHz bins), a column every _hop samples (6.4 us)
  uint64_t _abs_end = 0, _col = 0;   // samples fed; columns done (column c starts at sample c * _hop)
  std::vector<cf> _carry;            // samples from _carry_at on, not yet in a column
  uint64_t _carry_at = 0;
  std::vector<float> _win, _p, _floor; // window; this column's power; each bin's noise floor (20% quantile)
  fftwf_plan _plan = nullptr, _fplan = nullptr;
  cf *_in = nullptr, *_out = nullptr, *_fin = nullptr, *_fout = nullptr; // FFTW's buffers (the fine one 8192)
  std::vector<float> _fwin;          // its window
  struct Line {
    int k;                           // its bin in the fine spectrum
    unsigned seen;                   // in which of the last 4 windows
    double comb_khz;                 // the last comb found around it (kept while it lasts)
  };
  std::vector<Line> _steady;
  std::vector<uint8_t> _occ;

  struct Track {
    uint64_t c0, c1;                 // first and last column
    int a, b;                        // bins it covered
    double kw, w;                    // power-weighted bin sum, power sum (its centre)
  };
  std::vector<Track> _tracks;
  struct Seen { double t, mhz; };
  std::vector<Seen> _narrow;         // narrow bursts of the last 100 ms (hopping)

  // the oven: the span's activity per ms over 200 ms, and where the power was while loud
  float _tfloor = 0;
  std::vector<float> _busy;
  int _ms_cols = 0, _ms_loud = 0;
  double _win_t0 = 0;
  std::vector<float> _loudsum;
  bool _oven = false;                // the last 200 ms had an oven: its sweep is not narrowband
};

} // namespace sig
