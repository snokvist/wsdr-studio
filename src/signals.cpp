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
    }
  }

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
    if (!(l.seen & 1) || __builtin_popcount(l.seen) < 3)
      continue;
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
