// SPDX-License-Identifier: GPL-2.0-only
#include "signals.h"

#include <fftw3.h>

#include "dsp.h"
#include "wifi.h" // (ssid_text: names made safe to show)

#include <algorithm>
#include <cmath>

namespace sig {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kNarrow = 4;      // bins (2.5 MHz) within 10 dB of a run's peak: wider is not narrowband
constexpr int kAround = 8;      // bins (5 MHz) either side that must stay 10 dB under it (not a shoulder)
constexpr uint64_t kWarm = 2000; // columns (~13 ms) before anything is reported: the floors settle
constexpr int kFine = 8192;     // the fine spectrum for steady carriers (4.9 kHz bins at 40 MS/s)
constexpr float kOcc = 45;      // occupied: 45x the bin's 20% quantile (~10 dB over its mean noise)
constexpr int kMsCols = 156;    // columns per "ms" (6.4 us each: 0.998 ms)
constexpr int kWinMs = 200;     // the oven is judged over 200 ms
} // namespace

Finder::~Finder() { release(); }

void Finder::release() {
  std::lock_guard<std::mutex> l(studio::fftw_plan_mutex());
  if (_plan)
    fftwf_destroy_plan(_plan);
  if (_fplan)
    fftwf_destroy_plan(_fplan);
  fftwf_free(_in);
  fftwf_free(_out);
  fftwf_free(_fin);
  fftwf_free(_fout);
  _plan = _fplan = nullptr;
  _in = _out = _fin = _fout = nullptr;
}

void Finder::reset(double fs, double centre_hz) {
  release();
  _fs = fs;
  _centre = centre_hz;
  _nf = std::abs(fs - 40e6) < 1e3 ? 64 : std::abs(fs - 20e6) < 1e3 ? 32 : 0;
  _hop = 4 * _nf;
  _abs_end = _col = _carry_at = 0;
  _steady.clear();
  _oven = false;
  _adv.clear();
  _advch.clear();
  _held.clear();
  _decoded.clear();
  _carry.clear();
  _tracks.clear();
  _narrow.clear();
  _busy.clear();
  _ms_cols = _ms_loud = 0;
  _win_t0 = 0;
  _tfloor = 0;
  _videos.clear();
  _vid_due = 0;
  _vid_rng = 1;
  if (_halfband.empty()) { // 31 taps (centre 15), Hamming: flat to ~+-8 MHz of a sub-band at 40 MS/s, ~50 dB down past +-12
    for (int i = 0; i < 31; i++) {
      const double m = i - 15;
      _halfband.push_back(static_cast<float>((m == 0 ? 0.5 : std::sin(kPi * m / 2) / (kPi * m)) * (0.54 - 0.46 * std::cos(2 * kPi * i / 30))));
    }
    float sum = 0;
    for (float t : _halfband)
      sum += t;
    for (float &t : _halfband)
      t /= sum;
  }
  if (!_nf)
    return;
  _win.resize(_nf);
  for (int k = 0; k < _nf; k++)
    _win[k] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * k / _nf));
  {
    std::lock_guard<std::mutex> l(studio::fftw_plan_mutex());
    _in = reinterpret_cast<cf *>(fftwf_alloc_complex(static_cast<size_t>(_nf)));
    _out = reinterpret_cast<cf *>(fftwf_alloc_complex(static_cast<size_t>(_nf)));
    _plan = fftwf_plan_dft_1d(_nf, reinterpret_cast<fftwf_complex *>(_in), reinterpret_cast<fftwf_complex *>(_out), FFTW_FORWARD,
                              FFTW_ESTIMATE);
    _fin = reinterpret_cast<cf *>(fftwf_alloc_complex(kFine));
    _fout = reinterpret_cast<cf *>(fftwf_alloc_complex(kFine));
    _fplan = fftwf_plan_dft_1d(kFine, reinterpret_cast<fftwf_complex *>(_fin), reinterpret_cast<fftwf_complex *>(_fout), FFTW_FORWARD,
                               FFTW_ESTIMATE);
  }
  _fwin.resize(kFine);
  for (int k = 0; k < kFine; k++)
    _fwin[k] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * k / kFine));
  _p.assign(_nf, 0);
  _floor.assign(_nf, 0);
  _occ.assign(_nf, 0);
  _loudsum.assign(_nf, 0);
  // BLE: a windowed sinc (cutoff 600 kHz, 8 bits long) to 4 MS/s, mixed per channel into its taps
  _bdec = static_cast<int>(std::lround(_fs / 4e6));
  const int nt = 8 * _bdec + 1;
  std::vector<double> lp(nt);
  double sum = 0;
  for (int i = 0; i < nt; i++) {
    const double m = i - (nt - 1) / 2.0, fc = 0.6e6 / _fs;
    lp[i] = (m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m)) * (0.54 - 0.46 * std::cos(2 * kPi * i / (nt - 1)));
    sum += lp[i];
  }
  for (const auto &[mhz, chan] : {std::pair{2402.0, 37}, {2426.0, 38}, {2480.0, 39}}) {
    const int k = static_cast<int>(std::lround((mhz * 1e6 - _centre) / (_fs / _nf) + 0.5 * _nf));
    if (k < 2 || k > _nf - 3)
      continue;
    AdvChan a;
    a.chan = chan;
    a.bin = k;
    a.mhz = mhz;
    a.step = -2 * kPi * (mhz * 1e6 - _centre) / _fs;
    for (int i = 0; i < nt; i++)
      a.taps.push_back(std::polar(static_cast<float>(lp[i] / sum), static_cast<float>(a.step * i)));
    a.white.resize(8 * 260);
    for (unsigned i = 0, reg = (static_cast<unsigned>(chan) & 0x3F) | 0x40; i < a.white.size(); i++) {
      a.white[i] = reg & 1;
      reg = (reg >> 1) ^ ((reg & 1) ? 0x44 : 0);
    }
    _advch.push_back(std::move(a));
  }
}

void Finder::feed(const cf *x, size_t n, double t_end, std::vector<Burst> &out) {
  if (!_nf)
    return;
  _carry.insert(_carry.end(), x, x + n);
  _abs_end += n;
  _t_end = t_end;
  const uint64_t hop = static_cast<uint64_t>(_hop);
  while (_col * hop + static_cast<uint64_t>(_nf) <= _abs_end) {
    column(&_carry[_col * hop - _carry_at], out);
    _col++;
  }
  // activity on an advertising channel, once 420 us from its start are in (the longest advertising
  // packet is 376 us), from 30 us before it: a packet that decodes is reported (activity that does not
  // is often Wi-Fi covering the channel: the narrow-burst tracker reports what is narrowband there)
  const uint64_t m = static_cast<uint64_t>(30e-6 * _fs), span = static_cast<uint64_t>(420e-6 * _fs);
  while (!_adv.empty() && _adv.front().s0 + span <= _abs_end) {
    Adv a = _adv.front();
    _adv.erase(_adv.begin());
    if (a.s0 >= _carry_at + m && ble(a.s0 - m, a.s0 + span, _advch[a.ch], a.b)) { // (the samples are kept 1 ms)
      a.b.kind = 'E';
      out.push_back(a.b);
      _decoded.emplace_back(a.b.t, a.b.t + a.b.dur_us * 1e-6);
    }
  }
  // the narrow bursts held there: dropped when a decoded packet covers them, once its decode is done
  const double t_done = _t_end - (_adv.empty() ? 0 : static_cast<double>(_abs_end - _adv.front().s0) / _fs) - 450e-6;
  while (!_held.empty() && _held.front().t < t_done) {
    const Burst h = _held.front();
    _held.erase(_held.begin());
    bool dup = false;
    for (const auto &[t0, t1] : _decoded)
      dup = dup || (h.t < t1 && h.t + h.dur_us * 1e-6 > t0);
    if (!dup)
      out.push_back(h);
  }
  while (!_decoded.empty() && _decoded.front().second < t_done - 0.01)
    _decoded.erase(_decoded.begin());
  // the last ~1 ms stays (a BLE packet is decoded from it once its burst has ended); trimmed in steps
  const uint64_t keep = static_cast<uint64_t>(_fs * 1e-3);
  const uint64_t start = _col * hop > keep ? _col * hop - keep : 0;
  if (start > _carry_at + 4 * keep) {
    const uint64_t drop = std::min<uint64_t>(start - _carry_at, _carry.size());
    _carry.erase(_carry.begin(), _carry.begin() + static_cast<long>(drop));
    _carry_at += drop;
  }
}

void Finder::column(const cf *x, std::vector<Burst> &out) {
  // the spectrum, its power fftshifted
  for (int k = 0; k < _nf; k++)
    _in[k] = x[k] * _win[k];
  fftwf_execute(_plan);
  float total = 0;
  for (int k = 0; k < _nf; k++) {
    const float p = std::norm(_out[(k + _nf / 2) % _nf]);
    _p[k] = p;
    // each bin's floor: its 20% quantile over time (up 0.4% when above, down 1.6% when below)
    float &f = _floor[k];
    f = _col == 0 ? p : std::max(1e-30f, f * (p > f ? 1.004f : 0.984f));
    _occ[k] = k > 0 && k < _nf - 1 && p > kOcc * f; // (not the edge bins: the anti-alias filter)
    if (k > 0 && k < _nf - 1)
      total += p;
  }
  _tfloor = _col == 0 ? total : std::max(1e-30f, _tfloor * (total > _tfloor ? 1.004f : 0.984f));

  // the oven's evidence: is the span loud, and is what is loud narrow (a magnetron's wobbling
  // carrier, not a 20 MHz Wi-Fi frame)
  int occ = 0;
  for (int k = 0; k < _nf; k++)
    occ += _occ[k];
  if (total > 3 * _tfloor && occ <= 10) {
    _ms_loud++;
    for (int k = 0; k < _nf; k++)
      if (_occ[k])
        _loudsum[k] += _p[k];
  }
  if (_busy.empty() && _ms_cols == 0)
    _win_t0 = time_of(_col * static_cast<uint64_t>(_hop));
  if (++_ms_cols == kMsCols) {
    _busy.push_back(static_cast<float>(_ms_loud) / kMsCols);
    _ms_cols = _ms_loud = 0;
    if (static_cast<int>(_busy.size()) == kWinMs) {
      carriers(out);
      oven(out);
      _vid_rng = _vid_rng * 1664525u + 1013904223u; // (its next look: within the next ~20 ms, a field)
      _vid_due = _col + 1 + (_vid_rng >> 8) % 3125;
    }
  }
  // analog video: as the warm-up ends (its band is then known before any narrow burst, which parts of
  // it would pass for, can be reported), then once a window
  if (_col == kWarm || _col == _vid_due)
    video(out);

  // the advertising channels: active while their three bins hold 6x their noise (gaps up to 10
  // columns bridged); an active stretch of 50-512 us is decoded (below; longer is not a packet)
  for (size_t ci = 0; ci < _advch.size(); ci++) {
    AdvChan &a = _advch[ci];
    float p = 0, fl = 0;
    for (int k = a.bin - 1; k <= a.bin + 1; k++) {
      p += _p[k];
      fl += _floor[k] / 0.223f;
    }
    if (p > 6 * fl && _col >= kWarm) {
      if (!a.on)
        a.c0 = _col;
      a.on = true;
      a.c1 = _col;
    } else if (a.on && _col > a.c1 + 10) {
      a.on = false;
      if (a.c1 - a.c0 + 1 >= 8 && a.c1 - a.c0 + 1 <= 80 && !_oven) {
        Burst b;
        b.mhz = a.mhz;
        b.bw_mhz = 2;
        _adv.push_back({a.c0 * static_cast<uint64_t>(_hop), ci, b});
      }
    }
  }

  // narrow runs: occupied bins whose core (within 10 dB of their peak) is at most kNarrow bins, with
  // nothing within kAround bins either side reaching that core (a strong Wi-Fi frame's shoulder is not
  // narrowband); measured from the run's own peak, so a strong burst's skirts do not widen it
  for (int a0 = 1; a0 < _nf - 1;) {
    if (!_occ[a0]) {
      a0++;
      continue;
    }
    int b0 = a0;
    while (b0 + 1 < _nf - 1 && _occ[b0 + 1])
      b0++;
    float pk = 0;
    for (int k = a0; k <= b0; k++)
      pk = std::max(pk, _p[k]);
    int a = b0, b = a0; // the core
    for (int k = a0; k <= b0; k++)
      if (_p[k] > 0.1f * pk) {
        a = std::min(a, k);
        b = std::max(b, k);
      }
    // (neighbours count by their power over their usual level: a steady spur, such as the adapter's
    // DC at the centre, is no neighbour)
    bool alone = true;
    for (int k = std::max(0, a0 - kAround); alone && k <= std::min(_nf - 1, b0 + kAround); k++)
      alone = (k >= a0 && k <= b0) || _p[k] - _floor[k] / 0.223f <= 0.1f * pk;
    if (b - a + 1 <= kNarrow && b0 - a0 + 1 <= 3 * kNarrow && alone && _col >= kWarm) {
      size_t t = 0;
      while (t < _tracks.size() && !(_tracks[t].c1 + 2 >= _col && a <= _tracks[t].b + 1 && b >= _tracks[t].a - 1))
        t++;
      if (t == _tracks.size())
        _tracks.push_back({_col, _col, a, b, 0, 0});
      Track &tr = _tracks[t];
      tr.c1 = _col;
      tr.a = std::min(tr.a, a);
      tr.b = std::max(tr.b, b);
      for (int k = a; k <= b; k++) {
        tr.kw += static_cast<double>(k) * _p[k];
        tr.w += _p[k];
      }
    }
    a0 = b0 + 1;
  }
  for (size_t t = 0; t < _tracks.size();)
    if (_tracks[t].c1 + 2 < _col)
      end_track(t, out);
    else
      t++;
}

void Finder::end_track(size_t i, std::vector<Burst> &out) {
  const Track tr = _tracks[i];
  _tracks.erase(_tracks.begin() + static_cast<long>(i));
  const uint64_t cols = tr.c1 - tr.c0 + 1;
  // (under ~50 us: Wi-Fi spilling over the span's edge leaves preamble-length fragments; the shortest
  // Bluetooth packet is 68 us) or it grew wide; or an oven is on (its sweep is not narrowband)
  if (cols < 8 || tr.b - tr.a + 1 > kNarrow || tr.w <= 0 || _oven)
    return;
  Burst b;
  b.t = time_of(tr.c0 * static_cast<uint64_t>(_hop));
  b.mhz = bin_hz(tr.kw / tr.w) / 1e6;
  b.bw_mhz = (tr.b - tr.a + 1) * _fs / _nf / 1e6;
  b.dur_us = static_cast<double>(cols * static_cast<uint64_t>(_hop)) / _fs * 1e6;
  // (an analog video signal's carrier is narrow at any instant and rests on a level for a whole line
  // where the picture is even: it is not a burst of its own)
  if (in_video(b.t, b.mhz * 1e6))
    return;
  // Bluetooth hops: at 2.4 GHz, bursts of at most 3 ms, at least 4 within 100 ms on 3 frequencies
  b.kind = 'N';
  bool adv = false; // on an advertising channel: held until its decodes are in (it may be one of them)
  for (const auto &a : _advch)
    adv = adv || std::abs(b.mhz - a.mhz) < 1.2;
  if (b.mhz > 2400 && b.mhz < 2484 && b.dur_us <= 3000) {
    if (!adv) // (advertising bursts would make one advertiser look like hopping)
      _narrow.push_back({b.t, b.mhz});
    while (!_narrow.empty() && _narrow.front().t < b.t - 0.1)
      _narrow.erase(_narrow.begin());
    std::vector<long> f;
    for (const auto &s : _narrow)
      f.push_back(std::lround(s.mhz));
    std::sort(f.begin(), f.end());
    if (_narrow.size() >= 4 && std::unique(f.begin(), f.end()) - f.begin() >= 3)
      b.kind = 'T';
  }
  if (adv)
    _held.push_back(b);
  else
    out.push_back(b);
}

// A microwave oven's magnetron runs on half-wave rectified mains: at 2.4 GHz the span is loud with a
// narrow, wobbling carrier for about half of every 20 ms (16.7 ms at 60 Hz) and quiet for the rest.
// The span's loud share per ms then repeats at the mains period and alternates at half of it.
void Finder::oven(std::vector<Burst> &out) {
  const int n = static_cast<int>(_busy.size());
  double mean = 0;
  for (float v : _busy)
    mean += v;
  mean /= n;
  const auto r = [&](int lag) { // autocorrelation, normalised
    double num = 0, den = 0;
    for (int m = 0; m < n; m++) {
      den += (_busy[m] - mean) * (_busy[m] - mean);
      if (m + lag < n)
        num += (_busy[m] - mean) * (_busy[m + lag] - mean);
    }
    return den > 0 ? num / den * n / (n - lag) : 0.0;
  };
  const bool band = _centre > 2.38e9 && _centre < 2.51e9;
  const bool hz50 = std::max({r(19), r(20), r(21)}) > 0.6 && std::min({r(9), r(10), r(11)}) < -0.3;
  const bool hz60 = std::max({r(16), r(17), r(18)}) > 0.6 && std::min({r(8), r(9)}) < -0.3;
  _oven = band && mean > 0.1 && mean < 0.9 && (hz50 || hz60);
  if (_oven) {
    // where: the bins its loud columns occupied
    float mx = 0;
    for (float v : _loudsum)
      mx = std::max(mx, v);
    int a = _nf, b = -1;
    double kw = 0, w = 0;
    for (int k = 0; k < _nf; k++)
      if (mx > 0 && _loudsum[k] > 0.05f * mx) {
        a = std::min(a, k);
        b = std::max(b, k);
        kw += k * static_cast<double>(_loudsum[k]);
        w += _loudsum[k];
      }
    if (b >= a) {
      Burst o;
      o.t = _win_t0;
      o.mhz = bin_hz(kw / w) / 1e6;
      o.bw_mhz = (b - a + 1) * _fs / _nf / 1e6;
      o.dur_us = kWinMs * 1e3;
      o.kind = 'M';
      out.push_back(o);
    }
  }
  _busy.clear();
  std::fill(_loudsum.begin(), _loudsum.end(), 0.f);
}

// BLE advertising (1M PHY): the channel mixed to 0 and filtered (~600 kHz), 4 samples a bit; the FM
// discriminator integrated over each bit, for each of the 4 phases; the access address 0x8E89BED6
// found with at most one bit wrong (sliced at 0, and midway between its ones and zeros for a carrier
// offset: it has 18 ones and 14 zeros); then the bits dewhitened
// (x^7 + x^4 + 1 seeded from the channel), the PDU header's length, CRC-24 (0x00065B from 0x555555, sent
// MSB first). A good CRC gives the advertiser's address and its name (AD types 0x08, 0x09).
bool Finder::ble(uint64_t s0, uint64_t s1, const AdvChan &a, Burst &b) const {
  const cf *x = &_carry[s0 - _carry_at];
  const size_t n = s1 - s0, nt = a.taps.size(), dec = static_cast<size_t>(_bdec);
  std::vector<cf> z; // (the taps mix relative to each output's first sample: a rotator per output)
  for (size_t o = 0; o + nt <= n; o += dec) {
    cf acc = 0;
    for (size_t k = 0; k < nt; k++)
      acc += x[o + k] * a.taps[k];
    z.push_back(acc * std::polar(1.f, static_cast<float>(std::remainder(a.step * static_cast<double>(o), 2 * kPi))));
  }
  std::vector<float> d(z.size() - 1); // (the window is 450 us: z is never empty)
  for (size_t i = 0; i + 1 < z.size(); i++)
    d[i] = std::arg(z[i + 1] * std::conj(z[i]));
  static const uint32_t kAA = 0x8E89BED6;
  for (int ph = 0; ph < 4; ph++) {
    std::vector<float> s; // one soft value per bit
    for (size_t i = static_cast<size_t>(ph); i + 4 <= d.size(); i += 4)
      s.push_back(d[i] + d[i + 1] + d[i + 2] + d[i + 3]);
    // (the access address within the first 80 bits: the activity started there, not a later packet)
    for (size_t k = 0; k + 32 + 16 <= s.size() && k < 80; k++) {
      float m1 = 0, m0 = 0;
      for (int i = 0; i < 32; i++)
        (((kAA >> i) & 1) ? m1 : m0) += s[k + i];
      // the threshold: 0, or (a carrier offset) midway between the address's ones and zeros; the
      // address may have one bit wrong, the CRC decides
      for (const float mean : {0.f, (m1 / 18 + m0 / 14) / 2}) {
        int wrong = 0;
        for (int i = 0; i < 32; i++)
          wrong += (s[k + i] > mean) != (((kAA >> i) & 1) != 0);
        if (wrong > 1)
          continue;
        const size_t p = k + 32;
        auto bit = [&](size_t i) { return static_cast<uint8_t>((s[p + i] > mean ? 1 : 0) ^ a.white[i]); };
        int len = 0;
        for (int j = 0; j < 8; j++)
          len |= bit(8 + static_cast<size_t>(j)) << j;
        const size_t nb = 16 + 8 * static_cast<size_t>(len);
        if (len < 6 || len > 37 || p + nb + 24 > s.size())
          continue;
        unsigned reg = 0x555555;
        for (size_t i = 0; i < nb; i++) {
          const unsigned fb = ((reg >> 23) & 1) ^ bit(i);
          reg = (reg << 1) & 0xFFFFFF;
          if (fb)
            reg ^= 0x00065B;
        }
        bool ok = true;
        for (int i = 0; i < 24 && ok; i++)
          ok = bit(nb + static_cast<size_t>(i)) == ((reg >> (23 - i)) & 1);
        if (!ok)
          continue;
        std::vector<uint8_t> by(nb / 8);
        for (size_t i = 0; i < nb; i++)
          by[i / 8] |= static_cast<uint8_t>(bit(i) << (i % 8));
        // ADV_IND, ADV_DIRECT_IND, ADV_NONCONN_IND, SCAN_RSP, ADV_SCAN_IND carry AdvA first; others not
        const int type = by[0] & 0x0F;
        if (type != 0 && type != 1 && type != 2 && type != 4 && type != 6)
          return false;
        char addr[18];
        std::snprintf(addr, sizeof addr, "%02x:%02x:%02x:%02x:%02x:%02x", by[7], by[6], by[5], by[4], by[3], by[2]);
        b.addr = addr;
        b.random = (by[0] >> 6) & 1; // TxAdd
        // the packet: from its preamble (8 bits before the address; the filter delays by half its
        // length) for 8 us a byte (preamble, address, header, payload, CRC)
        const int64_t sym = ph + 4 * (static_cast<int64_t>(k) - 8);
        const uint64_t at = s0 + static_cast<uint64_t>(std::max<int64_t>(0, sym)) * dec + nt / 2;
        b.t = time_of(at);
        b.dur_us = 8.0 * (len + 10);
        for (size_t i = 8; type != 1 && i + 1 < by.size();) { // the AD structures
          const size_t l = by[i];
          if (l == 0 || i + 1 + l > by.size())
            break;
          if (by[i + 1] == 0x08 || by[i + 1] == 0x09)
            b.name = wifi::ssid_text(&by[i + 2], l - 1);
          i += 1 + l;
        }
        return true;
      }
    }
  }
  return false;
}

// Analog video, FM (an analog FPV camera's transmitter, a video sender), looked for in the last 0.8-2 ms
// once a window, at a pseudo-random point in it. The stretch's mean is taken off first (the adapter's own
// DC; an FM carrier averages out). At 40 MS/s three 20 MHz sub-bands (centred -10, 0 and +10 MHz: mixed
// by quarter turns, halfband-filtered, to 20 MS/s) are looked at apart: Wi-Fi in the next channel
// stays out of the demodulator, the noise is half, and two transmitters 20 MHz apart are both found.
// A signal found in two of them is the one with the most sync pulses. Each one found is followed (its
// frequency averaged; its band held 2 s, so a miss does not let its parts through as narrow bursts).
void Finder::video(std::vector<Burst> &out) {
  const size_t n = std::min(_carry.size(), static_cast<size_t>(_fs * 2e-3));
  if (n < static_cast<size_t>(_fs * 0.8e-3))
    return;
  const cf *x = &_carry[_carry.size() - n];
  cf dc = 0;
  for (size_t i = 0; i < n; i++)
    dc += x[i];
  dc /= static_cast<float>(n);
  const bool wide = _fs > 30e6;
  const size_t nt = _halfband.size();
  std::vector<VideoHit> hits;
  std::vector<cf> z(n), y;
  static const cf kJ[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}; // j^k
  for (int c = wide ? -1 : 0; c <= (wide ? 1 : 0); c++) {
    // mixed by -c fs/4: e^(-j c pi/2 i) = j^((4 - c) i)
    for (size_t i = 0; i < n; i++)
      z[i] = (x[i] - dc) * kJ[(static_cast<size_t>(4 - c) * i) & 3];
    if (wide) { // halfband to 20 MS/s (its taps an even distance from the centre are zero, but the centre's)
      y.resize((n - nt) / 2 + 1);
      for (size_t k = 0; k < y.size(); k++) {
        cf acc = z[2 * k + nt / 2] * _halfband[nt / 2];
        for (size_t t = 0; t < nt; t += 2) // (nt / 2 is odd: these are the odd distances)
          acc += z[2 * k + t] * _halfband[t];
        y[k] = acc;
      }
    }
    // (the polarity of a signal already followed here: a transmitter's sync does not change ends)
    float prefer = 0;
    for (const auto &v : _videos)
      if (v.until > _t_end && std::abs(v.hz - _centre - c * _fs / 4) < 8e6)
        prefer = v.sgn;
    VideoHit h;
    if (video_in(wide ? y : z, wide ? _fs / 2 : _fs, prefer, h)) {
      h.band = c * _fs / 4;
      h.hz += h.band;
      hits.push_back(h);
    }
  }
  // one signal found in two sub-bands (the same sync tip, or about the same frequency): from the one it
  // is nearest the middle of (the other cuts it: its picture's part, its mean frequency, may be missing)
  std::sort(hits.begin(), hits.end(), [](const VideoHit &a, const VideoHit &b) { return std::abs(a.hz - a.band) < std::abs(b.hz - b.band); });
  std::vector<VideoHit> kept;
  const auto tip_hz = [](const VideoHit &h) { return h.hz + h.sgn * h.tip; };
  for (const auto &h : hits)
    if (std::none_of(kept.begin(), kept.end(), [&](const VideoHit &k) { return std::abs(k.hz - h.hz) < 8e6 || std::abs(tip_hz(k) - tip_hz(h)) < 3e6; }))
      kept.push_back(h);
  for (const auto &h : kept) {
    // its band: from the sync tip to where white is (3.4 times the sync's depth: the swing whatever the
    // picture holds), and 7 MHz either side (its sound subcarrier, 6-6.5 MHz from the carrier, puts
    // steady lines there: about the occupied band by Carson's rule)
    const double swing = 3.4 * (h.blank - h.tip), hz = _centre + h.hz;
    const double e0 = hz + h.sgn * h.tip, e1 = e0 + h.sgn * swing;
    size_t k = 0;
    while (k < _videos.size() && !(_videos[k].until > _t_end && std::abs(_videos[k].hz - hz) < 6e6))
      k++;
    if (k == _videos.size())
      _videos.push_back({hz, 0, 0, 0, h.sgn});
    else
      _videos[k].hz += 0.2 * (hz - _videos[k].hz);
    Video &v = _videos[k];
    v.lo = std::min(e0, e1) - 7e6;
    v.hi = std::max(e0, e1) + 7e6;
    v.until = _t_end + 2;
    v.sgn = h.sgn;
    Burst b;
    b.t = _t_end - static_cast<double>(n) / _fs; // (the stretch looked at)
    b.mhz = v.hz / 1e6;
    b.bw_mhz = swing / 1e6;
    b.dur_us = static_cast<double>(n) / _fs * 1e6;
    b.kind = 'A';
    b.line_khz = 1e-3 / h.line;
    out.push_back(b);
  }
  _videos.erase(std::remove_if(_videos.begin(), _videos.end(), [&](const Video &v) { return v.until < _t_end - 1; }), _videos.end());
}

// One sub-band (or span): FM-demodulated about its centre (a sub-band holds ±8 MHz: the sub-bands
// overlap, so a signal is within 5 MHz of one's centre) and averaged to 2.5 MS/s, with a 3-point median (an FM click, a one-sample spike under ~10 dB
// CNR, is no pulse edge). Line sync is a plateau of 4.7 us at one end of the frequency swing (the low or
// the high end, depending on the transmitter) once a line. Pulses of 2.5-7.5 us beyond a fifth of the
// way from that end (2% quantile) to the median, in a chain of whole line periods (misses and the
// vertical interval's half-line pulses skipped), on 60% of the lines or more, fitted to within 0.4 us,
// at 15.0-16.5 kHz (15.625 kHz: 625 lines, PAL/SECAM; 15.734: 525, NTSC); at the same power during the
// pulses as between them (FM: a carrier keyed on and off is no video signal); the level after them (the
// back porch, blanking) the same on every line. A bright detail in the picture (a pole, an edge) makes
// a chain at the other end: that of a signal already followed here (`prefer`) is kept to, else the end
// whose sync is on more of the lines, with the steadier blanking, nearer 30% of the way from its tip to
// the far end (blanking's place in either standard).
// Noise, Wi-Fi and steady or keyed carriers leave no chain.
bool Finder::video_in(const std::vector<cf> &y, double fs, float prefer, VideoHit &h) const {
  const size_t n = y.size();
  const size_t D = static_cast<size_t>(std::lround(fs / 2.5e6)); // 8 at 20 MS/s
  const double fv = fs / static_cast<double>(D);
  const float to_hz = static_cast<float>(fv / (2 * kPi)); // (a sum of D steps, averaged)
  std::vector<float> u((n - 1) / D), p(u.size());
  for (size_t m = 0; m < u.size(); m++) {
    float s = 0, e = 0;
    for (size_t i = m * D + 1; i <= m * D + D; i++) {
      s += std::arg(y[i] * std::conj(y[i - 1]));
      e += std::norm(y[i]);
    }
    u[m] = s * to_hz; // Hz from the sub-band's centre
    p[m] = e / static_cast<float>(D);
  }
  if (u.size() < 3)
    return false;
  std::vector<float> v(u);
  for (size_t m = 1; m + 1 < u.size(); m++)
    v[m] = std::max(std::min(u[m - 1], u[m]), std::min(std::max(u[m - 1], u[m]), u[m + 1]));
  std::vector<float> srt = v;
  const auto q = [&](double pr) {
    const auto k = srt.begin() + static_cast<long>(pr * static_cast<double>(srt.size() - 1));
    std::nth_element(srt.begin(), k, srt.end());
    return *k;
  };
  const float lo = q(0.02), med = q(0.5), hi = q(0.98);
  const double line_min = fv / 16.5e3, line_max = fv / 15.0e3; // a line, in samples (152-167)
  const int wmin = static_cast<int>(std::lround(2.5e-6 * fv)), wmax = static_cast<int>(std::lround(7.5e-6 * fv));
  const double tol = 1.2e-6 * fv;
  const auto median = [](std::vector<float> &a) {
    std::nth_element(a.begin(), a.begin() + static_cast<long>(a.size() / 2), a.end());
    return a[a.size() / 2];
  };
  bool found = false;
  float best_score = 0;
  for (const float sgn : {1.f, -1.f}) { // sync at the low end, at the high end
    const float tip = sgn > 0 ? lo : -hi, mid = sgn * med, far = sgn > 0 ? hi : -lo;
    if (mid - tip < 0.5e6f || (prefer != 0 && sgn != prefer)) // (no swing to speak of: a carrier; a signal
      continue;                                               //  followed here keeps its end)
    const float th = tip + 0.2f * (mid - tip);
    const auto below = [&](size_t m) { return sgn * v[m] < th; };
    std::vector<double> c; // the pulses' centres (samples)
    for (size_t m = 0; m < v.size();) {
      if (!below(m)) {
        m++;
        continue;
      }
      size_t e = m; // the run, a one-sample excursion over the threshold bridged
      while (e + 1 < v.size() && (below(e + 1) || (e + 2 < v.size() && below(e + 2))))
        e++;
      const int len = static_cast<int>(e - m + 1);
      if (len >= wmin && len <= wmax && m > 0 && e + 1 < v.size()) // (not cut by the stretch's ends)
        c.push_back(0.5 * static_cast<double>(m + e));
      m = e + 1;
    }
    // the line period: the median spacing of neighbours a line apart
    std::vector<double> d;
    for (size_t i = 1; i < c.size(); i++)
      if (c[i] - c[i - 1] > line_min && c[i] - c[i - 1] < line_max)
        d.push_back(c[i] - c[i - 1]);
    if (d.size() < 3)
      continue;
    std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() / 2), d.end());
    const double L0 = d[d.size() / 2];
    // the longest chain of pulses whole lines apart (each step from the last pulse taken, up to 10
    // lines: the vertical interval has none at a line's start for ~8)
    std::vector<std::pair<double, double>> chain; // (line number, centre)
    for (size_t j = 0; j < c.size() && c.size() - j > chain.size(); j++) {
      std::vector<std::pair<double, double>> ch{{0, c[j]}};
      for (size_t i = j + 1; i < c.size(); i++) {
        const double dd = c[i] - ch.back().second, k = std::round(dd / L0);
        if (k >= 1 && k <= 10 && std::abs(dd - k * L0) < tol)
          ch.emplace_back(ch.back().first + k, c[i]);
      }
      if (ch.size() > chain.size())
        chain.swap(ch);
    }
    if (chain.size() < 6 || static_cast<double>(chain.size()) < 0.6 * static_cast<double>(v.size()) / L0)
      continue;
    // least squares: centre = a + line * number
    double sn = 0, sc = 0, snn = 0, snc = 0;
    for (const auto &[ln, cc] : chain) {
      sn += ln;
      sc += cc;
      snn += ln * ln;
      snc += ln * cc;
    }
    const double N = static_cast<double>(chain.size()), den = N * snn - sn * sn;
    if (den <= 0)
      continue;
    const double line = (N * snc - sn * sc) / den, a = (sc - line * sn) / N;
    double r2 = 0;
    for (const auto &[ln, cc] : chain)
      r2 += (cc - a - line * ln) * (cc - a - line * ln);
    if (std::sqrt(r2 / N) > 0.4e-6 * fv || line < line_min || line > line_max)
      continue;
    // the sync tip (at the pulses' centres) and blanking (the back porch: 1-4 us after the pulse ends,
    // before the picture in either standard), each the median over the pulses; the power in both
    std::vector<float> tips, porch, ratio, levels, line_porch;
    const size_t r1 = static_cast<size_t>(1e-6 * fv), p0 = static_cast<size_t>(3.35e-6 * fv), p1 = static_cast<size_t>(6.35e-6 * fv);
    for (const auto &[ln, cc] : chain) {
      const size_t m = static_cast<size_t>(cc); // (>= wmin / 2 > r1: a pulse starts after sample 0)
      float pp = 0, pb = 0;
      for (size_t k = m - r1; k <= m + r1 && k < v.size(); k++) {
        tips.push_back(sgn * v[k]);
        pp += p[k];
      }
      line_porch.clear();
      for (size_t k = m + p0; k <= m + p1 && k < v.size(); k++) {
        porch.push_back(sgn * v[k]);
        line_porch.push_back(sgn * v[k]);
        pb += p[k];
      }
      if (m + p1 < v.size() && pb > 0) {
        ratio.push_back(pp / static_cast<float>(2 * r1 + 1) / (pb / static_cast<float>(p1 - p0 + 1)));
        levels.push_back(median(line_porch));
      }
    }
    if (tips.empty() || porch.empty() || ratio.empty())
      continue;
    const float t = median(tips), bl = median(porch), pr = median(ratio);
    // (sync is 30% of the way from its tip to white in either standard: a shallower one is none)
    if (pr < 0.5f || pr > 2.f || bl - t < 0.1f * (hi - lo) || !(far > t))
      continue;
    // blanking is one level on every line; what follows a bright detail in the picture (a pole, an
    // edge: they line up from line to line too) is whatever the picture holds there
    for (float &l : levels)
      l = std::abs(l - bl);
    const float spread = median(levels) / (bl - t);
    if (spread > 0.25f)
      continue;
    // the end with the better sync: on more of the lines, blanking steadier, at blanking's place
    const float cover = static_cast<float>(N * line / static_cast<double>(v.size()));
    const float score = (1 - std::min(cover, 1.f)) + 2 * spread + std::abs((bl - t) / (far - t) - 0.3f) - (sgn == prefer ? 1.f : 0.f);
    if (!found || score < best_score) {
      found = true;
      best_score = score;
      h = {0, 0, sgn, t, bl, line / fv, chain.size()};
    }
  }
  if (!found)
    return false;
  // its mean frequency: the demodulated samples' mean where the power is its own (another signal over
  // it, such as Wi-Fi in a sub-band's edge, raises the power)
  std::vector<float> pw(p);
  const float pm = median(pw);
  double sum = 0;
  size_t k = 0;
  for (size_t m = 0; m < v.size(); m++)
    if (p[m] > 0.5f * pm && p[m] < 1.5f * pm) {
      sum += v[m];
      k++;
    }
  const float mv = k ? static_cast<float>(sum / static_cast<double>(k)) : 0.f;
  h.hz = mv;
  h.tip -= h.sgn * mv; // (from hz now)
  h.blank -= h.sgn * mv;
  return true;
}

// Steady carriers: a fine spectrum (the last ~0.8 ms, 8192-point FFTs averaged) every 200 ms; a narrow
// line 10 dB over its 1.25 MHz block's median that is there in 3 of the last 4 windows is reported,
// with the spacing of evenly spaced lines around it (a carrier switched at a steady rate: a display's
// pixel clock harmonics carry its line rate). Not the DC at the centre, nor n x 40 MHz (the adapter's
// own crystal: the display labels those).
void Finder::carriers(std::vector<Burst> &out) {
  const int K = static_cast<int>(_fs * 1e-3 / kFine); // 4 at 40 MS/s, 2 at 20 (~0.82 ms)
  if (K < 1 || _carry.size() < static_cast<size_t>(K * kFine))
    return;
  std::vector<float> P(kFine, 0.f);
  const cf *x = &_carry[_carry.size() - static_cast<size_t>(K * kFine)];
  for (int r = 0; r < K; r++) {
    for (int k = 0; k < kFine; k++)
      _fin[k] = x[r * kFine + k] * _fwin[k];
    fftwf_execute(_fplan);
    for (int k = 0; k < kFine; k++)
      P[k] += std::norm(_fout[(k + kFine / 2) % kFine]);
  }
  constexpr int kBlk = 256;
  std::vector<float> med(kFine / kBlk), tmp(kBlk);
  for (size_t b = 0; b < med.size(); b++) {
    std::copy(P.begin() + static_cast<long>(b * kBlk), P.begin() + static_cast<long>((b + 1) * kBlk), tmp.begin());
    std::nth_element(tmp.begin(), tmp.begin() + kBlk / 2, tmp.end());
    med[b] = tmp[kBlk / 2];
  }
  const double bin = _fs / kFine;
  const auto hz = [&](int k) { return _centre + (k - kFine / 2) * bin; };
  const auto over = [&](int k) { return P[k] / (med[static_cast<size_t>(k / kBlk)] + 1e-30f); };
  std::vector<int> hits;
  for (int k = 10; k < kFine - 10; k++) {
    const double off = hz(k) - _centre, h40 = hz(k) - std::round(hz(k) / 40e6) * 40e6;
    if (over(k) < 10 || std::abs(off) < 200e3 || std::abs(off) > 0.45 * _fs || std::abs(h40) < 60e3)
      continue;
    bool peak = true;
    for (int j = k - 10; peak && j <= k + 10; j++)
      peak = j == k || P[j] < P[k] || (P[j] == P[k] && j > k);
    if (peak)
      hits.push_back(k);
  }
  // one line per 1.2 MHz (a comb's teeth are lines too): the strongest
  std::sort(hits.begin(), hits.end(), [&](int a, int b) { return P[a] > P[b]; });
  std::vector<int> kept;
  for (int k : hits)
    if (std::none_of(kept.begin(), kept.end(), [&](int j) { return std::abs(j - k) * bin < 600e3; }))
      kept.push_back(k);
  hits.swap(kept);
  // each line's history over the last 4 windows (it may wander a bin or two)
  for (auto &l : _steady)
    l.seen = (l.seen << 1) & 0xF;
  for (int k : hits) {
    auto it = std::find_if(_steady.begin(), _steady.end(), [&](const Line &l) { return std::abs(l.k - k) <= 2; });
    if (it == _steady.end())
      _steady.push_back({k, 1u, 0, 0});
    else
      it->k = k, it->seen |= 1u;
  }
  _steady.erase(std::remove_if(_steady.begin(), _steady.end(), [](const Line &l) { return l.seen == 0; }), _steady.end());
  for (auto &l : _steady) {
    const int k = l.k;
    if (!(l.seen & 1) || __builtin_popcount(l.seen) < 3 || in_video(_win_t0, hz(k))) // (a video signal's sync and
      continue;                                                                     //  blanking levels are lines too)
    Burst b;
    b.t = _win_t0;
    b.mhz = hz(k) / 1e6;
    b.dur_us = kWinMs * 1e3;
    b.kind = 'C';
    // the comb: local maxima 6 dB over their block within +-600 kHz, evenly spaced, with the spectrum
    // dipping 10 dB between neighbours (else they are bumps on one line's noisy skirt)
    std::vector<int> at;
    const int span = static_cast<int>(600e3 / bin);
    for (int j = std::max(3, k - span); j <= std::min(kFine - 4, k + span); j++)
      if (over(j) > 4 && P[j] >= P[j - 1] && P[j] >= P[j + 1] && P[j] >= P[j - 2] && P[j] >= P[j + 2])
        at.push_back(j);
    if (at.size() >= 5) {
      std::vector<double> d; // the spacings between teeth that are apart (Hz); 0 where they are not
      for (size_t i = 1; i < at.size(); i++) {
        const float lo = *std::min_element(P.begin() + at[i - 1], P.begin() + at[i]);
        d.push_back(lo < 0.1f * std::min(P[at[i - 1]], P[at[i]]) ? (at[i] - at[i - 1]) * bin : 0);
      }
      std::vector<double> s = d;
      std::nth_element(s.begin(), s.begin() + static_cast<long>(s.size() / 2), s.end());
      const double m = s[s.size() / 2];
      int near = 0;
      for (double v : d)
        near += std::abs(v - m) < 0.1 * m;
      if (near >= 4 && m > 4 * bin) { // fitted to the teeth on its grid around the carrier (finer than a bin)
        double num = 0, den = 0;
        for (int j : at) {
          const double df = (j - k) * bin, n = std::round(df / m);
          if (n != 0 && std::abs(df - n * m) < 0.2 * m) {
            num += n * df;
            den += n * n;
          }
        }
        if (den > 0) {
          l.comb_khz = num / den / 1e3;
          l.miss = -1;
        }
      }
    }
    if (++l.miss >= 5) // (a window may miss it: the last one found holds a while)
      l.comb_khz = 0;
    b.comb_khz = l.comb_khz;
    b.bw_mhz = 0.5;
    out.push_back(b);
  }
}

} // namespace sig
