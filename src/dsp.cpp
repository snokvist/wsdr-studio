// SPDX-License-Identifier: GPL-2.0-only
#include "dsp.h"
#include "stream_cu.h" // la_i/la_q, path_b_i/path_b_q

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace studio {

std::mutex &fftw_plan_mutex() {
  static std::mutex m;
  return m;
}

struct Spectrum::Path {
  int index = 0;
  wsdr::DcBlock dc;
  fftwf_complex *in = nullptr, *out = nullptr;
  fftwf_plan plan = nullptr;
  int fill = 0;                   // samples in `in`
  std::vector<float> sum, max;    // linear power per DISPLAY bin (fftshifted, <= kMaxBins)
  std::vector<float> one;         // this FFT's power per display bin
  int count = 0;                  // FFTs in the current row
  double row_samples = 0;         // samples consumed in the current row
  double row_t0 = 0;              // gapped runs: when the current row started (clock)

  Path(int idx, int n) : index(idx) {
    std::lock_guard<std::mutex> l(fftw_plan_mutex());
    in = fftwf_alloc_complex(n);
    out = fftwf_alloc_complex(n);
    plan = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    const int bins = std::min(n, kMaxBins);
    sum.assign(bins, 0.f);
    max.assign(bins, 0.f);
    one.assign(bins, 0.f);
  }
  ~Path() {
    std::lock_guard<std::mutex> l(fftw_plan_mutex());
    fftwf_destroy_plan(plan);
    fftwf_free(in);
    fftwf_free(out);
  }
};

Spectrum::Spectrum() { rebuild(); }
Spectrum::~Spectrum() = default;

void Spectrum::configure(const DspConfig &c) {
  DspConfig n = c;
  int f = 1024;
  while (f < n.fft && f < 16384)
    f <<= 1;
  n.fft = f;
  n.rows_per_s = std::clamp(n.rows_per_s, 1.0, 30.0);
  if (!(n.paths & 3))
    n.paths = 1;
  _c = n;
  _req_fft = n.fft;
  if (_gapped)
    _c.fft = std::min(_c.fft, kGappedMaxFft);
  rebuild();
}

void Spectrum::rebuild() {
  const int n = _c.fft;
  _win.resize(n);
  double s = 0;
  for (int i = 0; i < n; i++) {
    _win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / n));
    s += _win[i];
  }
  // Full-scale complex sine (amplitude 2048 LSB) -> |X| = 2048 * sum(w) -> 0 dBFS.
  _norm = static_cast<float>(1.0 / (2048.0 * 2048.0 * s * s));
  _paths.clear();
  for (int p = 0; p < 2; p++)
    if (_c.paths & (1 << p))
      _paths.push_back(std::make_unique<Path>(p, n));
  _gen = ~0u; // re-init on the next bank
}

void Spectrum::reset(const wsdr::BankMeta &m) {
  _gapped = m.gapped;
  const int want = _gapped ? std::min(_req_fft, kGappedMaxFft) : _req_fft;
  if (want != _c.fft) {
    _c.fft = want;
    rebuild();
  }
  const double now = clock();
  _gen = m.gen;
  _fs = m.fs;
  _freq = m.centre_hz; // hardware band; see the header
  // (the DC block keeps its state across a gapped run's gaps: the DC offset drifts far more slowly)
  for (auto &p : _paths) {
    p->dc = wsdr::DcBlock();
    p->fill = 0;
    p->count = 0;
    p->row_samples = 0;
    p->row_t0 = now;
    std::fill(p->sum.begin(), p->sum.end(), 0.f);
    std::fill(p->max.begin(), p->max.end(), 0.f);
  }
}

void Spectrum::unpack(const uint8_t *bank, size_t samples, int path, bool cu, wsdr::cf *out) {
  for (size_t i = 0; i < samples; i++) {
    uint64_t w;
    std::memcpy(&w, bank + 8 * i, 8);
    if (path == 0)
      out[i] = wsdr::cf(static_cast<float>(wsdr::la_i(w)), static_cast<float>(wsdr::la_q(w)));
    else
      out[i] = wsdr::cf(static_cast<float>(wsdr::path_b_i(w, cu)), static_cast<float>(wsdr::path_b_q(w, cu)));
  }
}

void Spectrum::process(const uint8_t *bank, size_t bytes, const wsdr::BankMeta &meta,
                       const std::function<void(const Row &)> &emit) {
  if (meta.gen != _gen)
    reset(meta);
  const size_t ns = bytes / 8;
  _tmp.resize(ns);
  const int n = _c.fft;
  const double row_len = _fs / _c.rows_per_s;
  for (auto &pp : _paths) {
    Path &p = *pp;
    if (p.index == 1 && meta.paths < 2)
      continue; // no path B on this chip
    unpack(bank, ns, p.index, meta.cu, _tmp.data());
    if (_c.dc_remove)
      p.dc.process(_tmp.data(), ns);
    size_t i = 0;
    while (i < ns) {
      const size_t take = std::min<size_t>(ns - i, static_cast<size_t>(n - p.fill));
      for (size_t k = 0; k < take; k++) {
        const float w = _win[p.fill + k];
        p.in[p.fill + k][0] = _tmp[i + k].real() * w;
        p.in[p.fill + k][1] = _tmp[i + k].imag() * w;
      }
      p.fill += static_cast<int>(take);
      i += take;
      p.row_samples += static_cast<double>(take);
      if (p.fill < n)
        continue;
      p.fill = 0;
      fftwf_execute(p.plan);
      _ffts++;
      // Power per display bin for this FFT (fftshift; above kMaxBins, `dec` FFT bins summed), then the
      // row's average (sum) and peak (max) both from those same values: one unit for both traces.
      const int bins = std::min(n, kMaxBins);
      const int dec = n / bins;
      float *one = p.one.data(), *sum = p.sum.data(), *mx = p.max.data();
      std::fill(p.one.begin(), p.one.end(), 0.f);
      for (int k = 0; k < n; k++) {
        const float re = p.out[k][0], im = p.out[k][1];
        one[((k + n / 2) % n) / dec] += re * re + im * im;
      }
      for (int b = 0; b < bins; b++) {
        sum[b] += one[b];
        if (one[b] > mx[b])
          mx[b] = one[b];
      }
      p.count++;
      if (_gapped ? clock() - p.row_t0 < 1.0 / _c.rows_per_s : p.row_samples < row_len)
        continue;
      // Emit: dB once per display bin.
      Row r;
      r.path = p.index;
      r.seq = _seq++;
      r.gen = static_cast<uint8_t>(_gen & 0xff);
      r.gapped = _gapped;
      r.df = _fs / n * dec;
      r.f0 = _freq - _fs / 2 + (_fs / n) * (dec - 1) / 2.0;
      int b0 = 0, b1 = bins; // [b0, b1): the whole band, or the crop window
      if (_crop_hi > _crop_lo) {
        b0 = std::clamp(static_cast<int>(std::ceil((_crop_lo - r.f0) / r.df - 1e-6)), 0, bins);
        b1 = std::clamp(static_cast<int>(std::floor((_crop_hi - r.f0) / r.df + 1e-6)) + 1, b0, bins);
        if (b1 - b0 < 2) { // the window is not in this band: send the band (never an empty row)
          b0 = 0;
          b1 = bins;
        } else {
          r.f0 += b0 * r.df;
          r.offset = true;
        }
      }
      r.avg.resize(b1 - b0);
      r.max.resize(b1 - b0);
      const float inv = _norm / static_cast<float>(p.count);
      for (int b = b0; b < b1; b++) {
        r.avg[b - b0] = 10.f * std::log10(sum[b] * inv + 1e-20f);
        r.max[b - b0] = 10.f * std::log10(mx[b] * _norm + 1e-20f);
      }
      emit(r);
      std::fill(p.sum.begin(), p.sum.end(), 0.f);
      std::fill(p.max.begin(), p.max.end(), 0.f);
      p.count = 0;
      p.row_samples = 0;
      if (_gapped)
        p.row_t0 = clock();
    }
  }
}

} // namespace studio
