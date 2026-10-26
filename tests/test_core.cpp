// SPDX-License-Identifier: GPL-2.0-only
// Unit tests without hardware, plus `--bench`: the DSP at 40 MS/s on synthetic banks, and
// `--wifi <recording.sigmf-data>`: the Wi-Fi frame finder over a recording, summarised.
#include <array>
#include <chrono>
#include <complex>
#include <fstream>
#include <map>
#include <random>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dsp.h"
#include "protocol.h"
#include "receiver.h"
#include "signals.h"
#include "wifi.h"

#include <nlohmann/json.hpp>

using namespace studio;

static int g_fail = 0;
#define CHECK(c)                                                                                   \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                     \
      g_fail++;                                                                                    \
    }                                                                                              \
  } while (0)

// LA bank with a complex tone at f_hz (amplitude amp LSB) on path A, and path B = path A * j.
static std::vector<uint8_t> tone_bank(double f_hz, double fs, double amp, uint64_t start) {
  std::vector<uint8_t> b(65536);
  for (size_t i = 0; i < 8192; i++) {
    const double ph = 2 * M_PI * f_hz * static_cast<double>(start + i) / fs;
    const int I = static_cast<int>(std::lround(amp * std::cos(ph))), Q = static_cast<int>(std::lround(amp * std::sin(ph)));
    const uint64_t w = (static_cast<uint64_t>(I & 0xFFF)) | (static_cast<uint64_t>(Q & 0xFFF) << 12) |
                       (static_cast<uint64_t>((-Q) & 0xFFF) << 44) | (static_cast<uint64_t>(I & 0xFFF) << 32);
    std::memcpy(b.data() + 8 * i, &w, 8);
  }
  return b;
}

static void test_channels() {
  using R = wsdr::Receiver;
  CHECK(R::channel_for(5180e6, 20) == 36);
  CHECK(R::channel_for(5190e6, 40) == 36);
  CHECK(std::abs(R::centre_of(36, 40) - 5190e6) < 1);
  CHECK(R::channel_for(5230e6, 40) == 44);
  CHECK(R::channel_for(2412e6, 20) == 1);
  CHECK(R::channel_for(2484e6, 20) == 14);
  CHECK(R::channel_for(5182e6, 20) == 36); // residual 2 MHz is shifted digitally
  CHECK(R::igi_for(0, false) == 0x50);
  CHECK(R::igi_for(100, false) == 0x1C);
  CHECK(R::width_of(wsdr::RxSettings{5190e6, 1, true, false, 30}, false) == 40);
  CHECK(R::width_of(wsdr::RxSettings{5190e6, 1, true, false, 30}, true) == 40); // 8822C gapped 40 MS/s
  CHECK(R::width_of(wsdr::RxSettings{5190e6, 2, true, false, 30}, false) == 20);
}

static void test_spectrum_tone() {
  // fs 20 MS/s, centre 5180 MHz, tone on bin 614 of 4096 (~ +3 MHz), half scale (-6.02 dBFS).
  // (Off a bin centre the Hann window reads up to 1.4 dB low: 3.000 MHz exactly gives -6.93.)
  const double fs = 20e6, f = 614 * fs / 4096;
  Spectrum sp;
  DspConfig c;
  c.fft = 4096;
  c.rows_per_s = 30;
  c.paths = 3;
  c.dc_remove = false;
  sp.configure(c);
  wsdr::BankMeta m;
  m.gen = 1;
  m.fs = fs;
  m.centre_hz = 5180e6;
  m.paths = 2;
  std::vector<Row> rows;
  for (int k = 0; k < 160; k++) { // >= 2 rows at the 30 rows/s cap
    auto b = tone_bank(f, fs, 1024, static_cast<uint64_t>(k) * 8192);
    m.seq = k;
    sp.process(b.data(), b.size(), m, [&](const Row &r) { rows.push_back(r); });
  }
  CHECK(!rows.empty());
  for (const Row &r : rows) {
    size_t pk = 0;
    for (size_t i = 1; i < r.avg.size(); i++)
      if (r.avg[i] > r.avg[pk])
        pk = i;
    const double fpk = r.f0 + r.df * pk;
    CHECK(std::abs(fpk - (5180e6 + f)) <= r.df);
    if (getenv("DUMP") || std::abs(r.avg[pk] - (-6.02f)) >= 0.5f)
      std::printf("  row seq %u path %d peak %.2f dBFS at %.0f Hz\n", r.seq, r.path, r.avg[pk], r.f0 + r.df * pk);
    CHECK(std::abs(r.avg[pk] - (-6.02f)) < 0.5f); // half scale, tone on a bin centre
    CHECK(r.avg.size() == 4096);
  }
  // path B = j * A: same spectrum
  int a = 0, bcount = 0;
  for (const Row &r : rows)
    (r.path ? bcount : a)++;
  CHECK(a > 0 && bcount > 0);
}

static void test_offgrid() {
  // Requested 5182 MHz at 5 MS/s: the hardware sits at 5180 (5 MHz grid). The rows must show the
  // hardware band 5177.5..5182.5 (no wrap-around), with a tone at the hardware's +2 MHz at 5182.
  Spectrum sp;
  DspConfig c;
  c.fft = 2048;
  c.rows_per_s = 25;
  c.dc_remove = false;
  sp.configure(c);
  wsdr::BankMeta m;
  m.gen = 7;
  m.fs = 5e6;
  m.centre_hz = 5180e6;
  m.shift_hz = 2e6;
  std::vector<Row> rows;
  for (int k = 0; k < 64; k++) {
    auto b = tone_bank(2e6, 5e6, 1024, static_cast<uint64_t>(k) * 8192);
    sp.process(b.data(), b.size(), m, [&](const Row &r) { rows.push_back(r); });
  }
  CHECK(!rows.empty());
  if (rows.empty())
    return;
  const Row &r = rows.back();
  size_t pk = 0;
  for (size_t i = 1; i < r.avg.size(); i++)
    if (r.avg[i] > r.avg[pk])
      pk = i;
  CHECK(std::abs(r.f0 + r.df * pk - 5182e6) <= r.df);
  CHECK(std::abs(r.f0 - r.df / 2 - 5177.5e6) <= r.df); // band starts at centre - fs/2
}

static void test_level_vs_fft() {
  // A tone on a bin centre reads -6.02 dBFS at FFT 4096. At 16384 (reduced to 4096 display bins by
  // summing power) the display bin also holds the Hann window's leakage: up to +1.76 dB.
  for (int n : {4096, 16384}) {
    Spectrum sp;
    DspConfig c;
    c.fft = n;
    c.rows_per_s = 25;
    c.dc_remove = false;
    sp.configure(c);
    wsdr::BankMeta m;
    m.gen = 1;
    m.fs = 20e6;
    m.centre_hz = 5180e6;
    std::vector<Row> rows;
    const double f = 512 * 20e6 / 4096; // on a bin centre at both sizes
    for (int k = 0; k < 120; k++) {
      auto b = tone_bank(f, 20e6, 1024, static_cast<uint64_t>(k) * 8192);
      sp.process(b.data(), b.size(), m, [&](const Row &r) { rows.push_back(r); });
    }
    CHECK(!rows.empty());
    if (rows.empty())
      continue;
    float pk = -200, pkmax = -200;
    for (float v : rows.back().avg)
      pk = std::max(pk, v);
    for (float v : rows.back().max)
      pkmax = std::max(pkmax, v);
    CHECK(std::abs(pkmax - pk) < 0.3f); // a steady tone: peak trace = average trace (same units)
    const float hi = n > 4096 ? -6.02f + 1.8f : -6.02f + 0.3f;
    if (pk < -6.32f || pk > hi)
      std::printf("  fft %d peak %.2f dBFS\n", n, pk);
    CHECK(pk >= -6.32f && pk <= hi);
  }
}

static void test_reduce_and_frame() {
  Spectrum sp;
  DspConfig c;
  c.fft = 16384;
  c.rows_per_s = 50; // 800k samples per row at 40 MS/s: ~98 banks
  sp.configure(c);
  wsdr::BankMeta m;
  m.gen = 1;
  m.fs = 40e6;
  m.centre_hz = 5190e6;
  std::vector<Row> rows;
  for (int k = 0; k < 200; k++) {
    auto b = tone_bank(5e6, 40e6, 512, static_cast<uint64_t>(k) * 8192);
    sp.process(b.data(), b.size(), m, [&](const Row &r) { rows.push_back(r); });
  }
  CHECK(!rows.empty());
  if (rows.empty())
    return;
  CHECK(rows[0].avg.size() == kMaxBins);
  const auto frame = encode_row(rows[0], 1.25);
  DecodedRow d;
  CHECK(decode_row(frame.data(), frame.size(), d));
  CHECK(d.bins == kMaxBins && d.seq == rows[0].seq && d.f0 == rows[0].f0 && d.df == rows[0].df);
  CHECK(d.gen == 1); // the run byte (meta.gen & 0xff) travels with every row
  for (size_t i = 0; i < d.bins; i++) {
    if (rows[0].avg[i] > kDbMin && rows[0].avg[i] < kDbMin + 255 * kDbStep)
      CHECK(std::abs(d.avg[i] - rows[0].avg[i]) <= kDbStep / 2 + 1e-3f);
  }
  CHECK(!decode_row(frame.data(), frame.size() - 1, d));
}

static int bench() {
  // 40 MS/s for 2 s of signal: 9766 banks, FFT 8192, 25 rows/s.
  Spectrum sp;
  DspConfig c;
  c.fft = 8192;
  c.rows_per_s = 25;
  sp.configure(c);
  wsdr::BankMeta m;
  m.gen = 1;
  m.fs = 40e6;
  m.centre_hz = 5190e6;
  m.shift_hz = 1.5e6;
  auto b = tone_bank(3e6, 40e6, 800, 0);
  const int banks = 9766;
  int rows = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int k = 0; k < banks; k++) {
    m.seq = k;
    sp.process(b.data(), b.size(), m, [&](const Row &) { rows++; });
  }
  const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double sig = banks * 8192 / 40e6;
  std::printf("{\"bench\":\"dsp\",\"fs\":40e6,\"fft\":8192,\"signal_s\":%.3f,\"cpu_s\":%.3f,\"load\":%.3f,\"ffts\":%llu,"
              "\"rows\":%d}\n", sig, el, el / sig, static_cast<unsigned long long>(sp.ffts()), rows);
  return el / sig < 0.5 ? 0 : 1;
}

// Gapped runs: the FFT is capped at one bank, a tone still lands in its bin, and rows follow the clock
// (not the sample count, which is only a fraction of fs).
static void test_gapped() {
  Spectrum sp;
  double now = 0;
  sp.clock = [&] { return now; };
  DspConfig c;
  c.fft = 16384;
  c.rows_per_s = 10;
  sp.configure(c);
  wsdr::BankMeta m;
  m.gen = 1;
  m.fs = 40e6;
  m.centre_hz = 5190e6;
  m.gapped = true;
  std::vector<Row> rows;
  for (int k = 0; k < 600; k++) { // 555 banks/s, as measured at 40 MS/s: ~1.08 s
    now = k / 555.0;
    // each bank its own capture: the phase restarts (a gap), the tone is the same
    auto b = tone_bank(5e6, 40e6, 512, 0);
    sp.process(b.data(), b.size(), m, [&](const Row &r) { rows.push_back(r); });
  }
  CHECK(sp.fft_size() == kGappedMaxFft);
  if (!rows.empty()) { // the gapped flag travels with the row
    DecodedRow d;
    const auto fr = encode_row(rows[0], 0);
    CHECK(rows[0].gapped && decode_row(fr.data(), fr.size(), d) && d.gapped && d.path == 0);
  }
  CHECK(rows.size() >= 9 && rows.size() <= 11); // 10 rows/s by the clock
  if (rows.empty())
    return;
  const Row &r = rows.back();
  int best = 0;
  for (int i = 1; i < static_cast<int>(r.avg.size()); i++)
    if (r.avg[i] > r.avg[best])
      best = i;
  CHECK(std::abs(r.f0 + best * r.df - (5190e6 + 5e6)) <= r.df);
  // back to a continuous run: the configured size again
  m.gen = 2;
  m.gapped = false;
  auto b = tone_bank(5e6, 40e6, 512, 0);
  sp.process(b.data(), b.size(), m, [](const Row &) {});
  CHECK(sp.fft_size() == 16384);
}

// Offset tuning: a crop window emits only its bins, with the right f0, and flags the rows.
static void test_crop() {
  Spectrum sp;
  DspConfig c;
  c.fft = 8192;
  c.rows_per_s = 50;
  sp.configure(c);
  sp.set_crop(5795e6, 5815e6); // channel 161 viewed from a radio centred at 5815 MHz (40 MS/s)
  wsdr::BankMeta m;
  m.gen = 1;
  m.fs = 40e6;
  m.centre_hz = 5815e6;
  std::vector<Row> rows;
  for (int k = 0; k < 200; k++) {
    auto b = tone_bank(-12e6, 40e6, 512, static_cast<uint64_t>(k) * 8192); // 5803 MHz
    sp.process(b.data(), b.size(), m, [&](const Row &r) { rows.push_back(r); });
  }
  CHECK(!rows.empty());
  if (rows.empty())
    return;
  const Row &r = rows.back();
  CHECK(r.offset && r.avg.size() >= 2040 && r.avg.size() <= 2050); // 20 of 40 MHz at 4096 bins
  CHECK(r.f0 >= 5795e6 - r.df && r.f0 <= 5795e6 + r.df);
  int best = 0;
  for (int i = 1; i < static_cast<int>(r.avg.size()); i++)
    if (r.avg[i] > r.avg[best])
      best = i;
  CHECK(std::abs(r.f0 + best * r.df - 5803e6) <= r.df);
  DecodedRow d;
  const auto fr = encode_row(r, 0);
  CHECK(decode_row(fr.data(), fr.size(), d) && d.offset && !d.gapped && d.path == 0);
}

// An 802.11a PPDU header (STF, LTF, L-SIG, then `extra` symbols) synthesised directly at 40 MS/s on
// the 20 MHz channel `off_hz` from the centre (+-10 MHz: the 312.5 kHz subcarriers land on bins of a
// 128-point IFFT). ht: the two symbols after L-SIG are QBPSK (HT-SIG), else BPSK.
static std::vector<std::complex<float>> ofdm_frame(int rate_code, int length, double off_hz, bool ht, int extra, std::mt19937 &rng) {
  using C = std::complex<float>;
  const int shift = static_cast<int>(std::lround(off_hz / 312.5e3));
  auto sym = [&](const std::array<C, 64> &sc, int cp) { // sc by subcarrier -32..31 (index s + 32)
    std::array<C, 128> f{};
    for (int s = -32; s < 32; s++)
      f[(s + shift + 128) % 128] = sc[s + 32];
    std::vector<C> t(128);
    for (int n = 0; n < 128; n++) {
      C acc = 0;
      for (int k = 0; k < 128; k++)
        acc += f[k] * std::polar(1.f, static_cast<float>(2 * M_PI * k * n / 128));
      t[n] = acc / 128.f;
    }
    std::vector<C> out(t.end() - cp, t.end());
    out.insert(out.end(), t.begin(), t.end());
    return out;
  };
  std::vector<C> x;
  // STF: 12 subcarriers, 0.8 us period (32 samples here), 8 us
  std::array<C, 64> stf{};
  const int stf_sc[12] = {-24, -20, -16, -12, -8, -4, 4, 8, 12, 16, 20, 24};
  const float stf_v[12] = {1, -1, 1, -1, -1, 1, -1, -1, 1, 1, 1, 1};
  for (int i = 0; i < 12; i++)
    stf[stf_sc[i] + 32] = C(stf_v[i], stf_v[i]) * std::sqrt(13.f / 6.f);
  auto st = sym(stf, 0);
  for (int r = 0; r < 2; r++)
    x.insert(x.end(), st.begin(), st.end()); // 256
  x.insert(x.end(), st.begin(), st.begin() + 64); // 320 = 8 us
  const int ltf[53] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 0,
                       1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};
  std::array<C, 64> lf{};
  for (int i = 0; i < 53; i++)
    lf[i - 26 + 32] = static_cast<float>(ltf[i]);
  auto lt = sym(lf, 0);
  x.insert(x.end(), lt.end() - 64, lt.end()); // GI2
  x.insert(x.end(), lt.begin(), lt.end());
  x.insert(x.end(), lt.begin(), lt.end());
  // L-SIG bits, convolutional code, interleaver, BPSK
  int bits[24] = {};
  for (int i = 0; i < 4; i++)
    bits[i] = (rate_code >> (3 - i)) & 1;
  for (int i = 0; i < 12; i++)
    bits[5 + i] = (length >> i) & 1;
  int par = 0;
  for (int i = 0; i < 17; i++)
    par ^= bits[i];
  bits[17] = par;
  int coded[48], reg = 0;
  for (int i = 0; i < 24; i++) {
    const int r = (bits[i] << 6) | reg;
    coded[2 * i] = __builtin_parity(r & 0133);
    coded[2 * i + 1] = __builtin_parity(r & 0171);
    reg = r >> 1;
  }
  std::vector<int> dsc;
  for (int s = -26; s <= 26; s++)
    if (s != 0 && s != -21 && s != -7 && s != 7 && s != 21)
      dsc.push_back(s);
  const int pol[3] = {1, 1, 1};
  auto data_sym = [&](const int *cb, int p, C rot) {
    std::array<C, 64> sc{};
    for (int k = 0; k < 48; k++)
      sc[dsc[3 * (k % 16) + k / 16] + 32] = rot * static_cast<float>(2 * cb[k] - 1);
    const int ps[4] = {-21, -7, 7, 21}, pv[4] = {1, 1, 1, -1};
    for (int i = 0; i < 4; i++)
      sc[ps[i] + 32] = static_cast<float>(pv[i] * p);
    return sym(sc, 32);
  };
  auto sg = data_sym(coded, pol[0], 1);
  x.insert(x.end(), sg.begin(), sg.end());
  std::uniform_int_distribution<int> bit(0, 1);
  for (int e = 0; e < extra; e++) {
    int cb[48];
    for (int &b : cb)
      b = bit(rng);
    const int p = e < 2 ? pol[e + 1] : 1;
    auto d = data_sym(cb, p, ht && e < 2 ? C(0, 1) : C(1, 0));
    x.insert(x.end(), d.begin(), d.end());
  }
  return x;
}

static void test_wifi_frames() {
  std::mt19937 rng(7);
  std::normal_distribution<float> g(0, 1);
  const double fs = 40e6, centre = 5190e6;
  wifi::Decoder dec;
  dec.reset(fs, centre);
  std::vector<std::complex<float>> x;
  auto noise = [&](size_t n, float sd) {
    for (size_t i = 0; i < n; i++)
      x.emplace_back(sd * g(rng), sd * g(rng));
  };
  // three frames: 24 Mb/s legacy on 5200 (+10 MHz), an HT one on 5180 (-10), 6 Mb/s legacy on 5200
  struct Want { int code, rate, len; double off; bool ht; size_t at; };
  std::vector<Want> want = {{0b1001, 24, 100, 10e6, false, 0}, {0b1101, 6, 1500, -10e6, true, 0}, {0b1101, 6, 14, 10e6, false, 0}};
  noise(30000, 0.05f);
  for (auto &w : want) {
    if (&w == &want[1]) { // a long burst at the frame's power just before (an A-MPDU): the noise floor must not rise to it
      for (size_t i = 0; i < 120000; i++) // 3 ms
        x.emplace_back(0.7f * g(rng), 0.7f * g(rng));
      noise(800, 0.05f); // 20 us
    }
    w.at = x.size();
    auto f = ofdm_frame(w.code, w.len, w.off, w.ht, 6, rng);
    const double cfo = 60e3; // like two adapters' crystals apart
    for (size_t i = 0; i < f.size(); i++)
      x.push_back(f[i] * std::polar(1.f, static_cast<float>(2 * M_PI * cfo * static_cast<double>(i) / fs)) * 8.9f + // ~17 dB SNR
                  std::complex<float>(0.05f * g(rng), 0.05f * g(rng)));
    noise(20000, 0.05f);
  }
  std::vector<wifi::Frame> got;
  for (size_t i = 0; i < x.size(); i += 8192) {
    const size_t n = std::min<size_t>(8192, x.size() - i);
    dec.feed(&x[i], n, static_cast<double>(i + n) / fs, got);
  }
  CHECK(got.size() == want.size());
  if (got.size() != want.size())
    for (const auto &f : got)
      std::printf("  got t %.6f c %.0f %c rate %d len %d snr %.1f\n", f.t, f.centre_hz / 1e6, f.fmt, f.rate_mbps, f.length, f.snr_db);
  for (size_t i = 0; i < std::min(got.size(), want.size()); i++) {
    const auto &f = got[i];
    const auto &w = want[i];
    CHECK(f.rate_mbps == w.rate && f.length == w.len);
    CHECK(std::abs(f.centre_hz - (centre + w.off)) < 1);
    CHECK(f.fmt == (w.ht ? 'H' : 'L'));
    CHECK(std::abs(f.t - static_cast<double>(w.at) / fs) < 3e-6); // where its preamble starts
    CHECK(f.snr_db > 15);
    if (w.len == 14)
      CHECK(std::abs(f.dur_us - (20 + 4 * 6)) < 1e-9); // an ACK at 6 Mb/s: 44 us
  }
}

// An 802.11b beacon (long preamble; payload at 1 Mb/s DBPSK or 2 Mb/s DQPSK; Barker) synthesised at
// 40 MS/s 10 MHz below the centre: its clock `ppm` off, its carrier `cfo` Hz off, noise of `sigma`, a DC
// offset `dc`, and (if lead_us) another signal that starts lead_us before it and lasts past it: a
// strong tone 12 MHz above the centre, or (`inoise`) white noise 8 dB below the frame. With `ack`, an
// ACK (1 Mb/s, long preamble) follows a SIFS (10 us) after it. Returns the samples; `at` is where the
// frame starts, `nbytes` its MAC frame's length.
static std::vector<std::complex<float>> dsss_frame(int rate, size_t pad, double ppm, double cfo, double lead_us, size_t &at, size_t &nbytes,
                                                   const std::string &ssid, float sigma = 0.05f, float dc = 0, bool inoise = false,
                                                   bool ack = false) {
  std::mt19937 rng(11);
  std::normal_distribution<float> g(0, 1);
  std::vector<uint8_t> mac = {0x80, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2, 0x11, 0x22, 0x33, 0x44, 0x55,
                              2, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0};
  for (int i = 0; i < 12; i++)
    mac.push_back(0); // timestamp, interval, capabilities
  mac.push_back(0);
  mac.push_back(static_cast<uint8_t>(ssid.size()));
  mac.insert(mac.end(), ssid.begin(), ssid.end());
  mac.insert(mac.end(), {3, 1, 1});
  while (pad > 0) { // vendor elements to make it longer
    const size_t l = std::min<size_t>(pad, 200);
    mac.push_back(221);
    mac.push_back(static_cast<uint8_t>(l));
    for (size_t i = 0; i < l; i++)
      mac.push_back(static_cast<uint8_t>(i * 7));
    pad -= l;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : mac) {
    c ^= b;
    for (int k = 0; k < 8; k++)
      c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  c ^= 0xFFFFFFFFu;
  for (int k = 0; k < 4; k++)
    mac.push_back(static_cast<uint8_t>(c >> (8 * k)));
  nbytes = mac.size();
  // the PHY: phases, one per symbol (1 us)
  const auto ppdu = [](const std::vector<uint8_t> &mac, int rate) {
  std::vector<uint8_t> bits(128, 1); // SYNC
  for (int j = 0; j < 16; j++)
    bits.push_back((0xF3A0 >> j) & 1);
  const unsigned length_us = static_cast<unsigned>(mac.size() * 8 / static_cast<size_t>(rate));
  std::vector<uint8_t> hdr;
  for (int j = 0; j < 8; j++)
    hdr.push_back(((rate == 1 ? 0x0A : 0x14) >> j) & 1);
  for (int j = 0; j < 8; j++)
    hdr.push_back(0);
  for (int j = 0; j < 16; j++)
    hdr.push_back((length_us >> j) & 1);
  unsigned reg = 0xFFFF;
  for (uint8_t b : hdr) {
    const unsigned fb = ((reg >> 15) & 1) ^ b;
    reg = (reg << 1) & 0xFFFF;
    if (fb)
      reg ^= 0x1021;
  }
  reg ^= 0xFFFF;
  for (int j = 15; j >= 0; j--)
    hdr.push_back((reg >> j) & 1);
  bits.insert(bits.end(), hdr.begin(), hdr.end());
  const size_t nhead = bits.size();
  for (uint8_t b : mac)
    for (int j = 0; j < 8; j++)
      bits.push_back((b >> j) & 1);
  // scramble (self-synchronising, seed 1101100)
  std::vector<uint8_t> sc(bits.size());
  unsigned st = 0b1101100;
  for (size_t i = 0; i < bits.size(); i++) {
    const unsigned o = bits[i] ^ ((st >> 3) & 1) ^ ((st >> 6) & 1);
    sc[i] = static_cast<uint8_t>(o);
    st = ((st << 1) | o) & 0x7f;
  }
  // phases: one symbol per bit (header), or per two bits at 2 Mb/s (Gray: 00 0, 01 pi/2, 11 pi, 10 3pi/2)
  std::vector<double> phase;
  double ph = 0;
  for (size_t i = 0; i < nhead; i++) {
    if (sc[i])
      ph += M_PI;
    phase.push_back(ph);
  }
  for (size_t i = nhead; i < sc.size(); i += static_cast<size_t>(rate)) {
    if (rate == 1)
      ph += sc[i] ? M_PI : 0;
    else
      ph += sc[i] ? (sc[i + 1] ? M_PI : 1.5 * M_PI) : (sc[i + 1] ? 0.5 * M_PI : 0);
    phase.push_back(ph);
  }
  return phase;
  };
  static const int barker[11] = {1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1};
  const double fs = 40e6, off = -10e6;
  std::vector<std::complex<float>> x;
  const std::complex<float> dco(dc, -0.7f * dc);
  auto noise = [&] { return std::complex<float>(sigma * g(rng), sigma * g(rng)) + dco; };
  auto other = [&](size_t i) {
    return inoise ? std::complex<float>(0.14f * g(rng), 0.14f * g(rng)) : std::polar(0.5f, static_cast<float>(2 * M_PI * 12e6 * static_cast<double>(i) / fs));
  };
  const size_t lead = static_cast<size_t>(lead_us * 40);
  bool on = false; // the other signal, once started, lasts to the end
  for (size_t i = 0; i < 20000; i++) {
    on = on || 20000 - i <= lead;
    x.push_back(noise() + (on ? other(i) : std::complex<float>(0)));
  }
  at = x.size();
  const auto put = [&](const std::vector<double> &phase) {
    const size_t a0 = x.size();
    const double frame_s = static_cast<double>(phase.size()) * 1e-6 / (1 + ppm * 1e-6);
    for (size_t n = 0; static_cast<double>(n) / fs < frame_s; n++) {
      const double t = static_cast<double>(n) / fs, u = t * 1e6 * (1 + ppm * 1e-6); // in the sender's microseconds
      const size_t k = std::min(phase.size() - 1, static_cast<size_t>(u));
      const int chip = barker[std::min(10, static_cast<int>((u - static_cast<double>(k)) * 11))];
      const double tt = static_cast<double>(a0 - at + n) / fs;
      x.push_back(static_cast<float>(chip) * 0.5f * std::polar(1.f, static_cast<float>(phase[k] + 2 * M_PI * (off + cfo) * tt)) + noise() +
                  (on ? other(x.size()) : std::complex<float>(0)));
    }
  };
  put(ppdu(mac, rate));
  if (ack) { // a SIFS, then an ACK to 02:11:22:33:44:55
    for (int i = 0; i < 400; i++)
      x.push_back(noise() + (on ? other(x.size()) : std::complex<float>(0)));
    std::vector<uint8_t> a = {0xd4, 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t ca = 0xFFFFFFFFu;
    for (uint8_t b : a) {
      ca ^= b;
      for (int k = 0; k < 8; k++)
        ca = ca & 1 ? 0xEDB88320u ^ (ca >> 1) : ca >> 1;
    }
    ca ^= 0xFFFFFFFFu;
    for (int k = 0; k < 4; k++)
      a.push_back(static_cast<uint8_t>(ca >> (8 * k)));
    put(ppdu(a, 1));
  }
  for (int i = 0; i < 40000; i++)
    x.push_back(noise() + (on ? other(x.size()) : std::complex<float>(0)));
  return x;
}

static void test_dsss_beacon() {
  struct Case { int rate; size_t pad; double ppm, cfo, lead; float sigma, dc; bool inoise, ack; };
  const Case cases[] = {
      {1, 0, 0, 50e3, 0, 0.05f, 0, false, false},      // plain
      {1, 450, 20, 50e3, 0, 0.05f, 0, false, false},   // a 500-byte frame, clocks 20 ppm apart (timing tracked)
      {2, 300, -20, 100e3, 0, 0.05f, 0, false, false}, // 2 Mb/s DQPSK with a 100 kHz offset (removed)
      {1, 0, 0, 50e3, 300, 0.05f, 0, false, false},    // a tone on the air from 300 us before it
      {1, 0, 0, 50e3, 300, 0.05f, 0, true, false},     // noise 8 dB below it, from 300 us before it
      {1, 0, 0, 50e3, 0, 0.2f, 0, false, false},       // weak (~4 dB over the noise in 40 MHz)
      {1, 0, 0, 50e3, 0, 0.05f, 0.05f, false, false},  // the adapter's DC offset
      {1, 0, 0, 50e3, 0, 0.05f, 0, false, true},       // its ACK a SIFS later (two frames)
      {1, 0, 0, 50e3, 300, 0.05f, 0, false, true},     // both under a tone that holds the detector on
  };
  for (const auto &cs : cases) {
    size_t at = 0, nbytes = 0;
    const auto x = dsss_frame(cs.rate, cs.pad, cs.ppm, cs.cfo, cs.lead, at, nbytes, "test-ssid", cs.sigma, cs.dc, cs.inoise, cs.ack);
    const double fs = 40e6;
    wifi::Decoder dec;
    dec.reset(fs, 2422e6);
    std::vector<wifi::Frame> got;
    for (size_t i = 0; i < x.size(); i += 8192) {
      const size_t n = std::min<size_t>(8192, x.size() - i);
      dec.feed(&x[i], n, static_cast<double>(i + n) / fs, got);
    }
    int b = 0, whole = 0, acks = 0;
    for (const auto &f : got) {
      if (f.fmt != 'B')
        continue;
      if (cs.ack && f.length == 14) { // the ACK: no sender, from the 2nd frame's place
        acks += f.mac && f.type == 1 && f.subtype == 13 && !f.has_ta;
        continue;
      }
      b++;
      whole += f.mac;
      CHECK(f.rate_mbps == cs.rate && f.length == static_cast<int>(nbytes));
      CHECK(std::abs(f.centre_hz - 2412e6) < 1);
      CHECK(f.mac && f.beacon && f.ssid == "test-ssid" && f.channel == 1);
      CHECK(std::abs(f.t - static_cast<double>(at) / fs) < 3e-6);
      CHECK(f.snr_db > (cs.sigma > 0.1f ? 5 : 10));
    }
    CHECK(b == 1);
    CHECK(acks == (cs.ack ? 1 : 0));
    if (b != 1 || whole != 1 || acks != (cs.ack ? 1 : 0))
      std::printf("  (802.11b case: %d Mb/s, %zu bytes, %g ppm, %g Hz, lead %g us%s, sigma %g, dc %g, ack %d: %d frames, %d whole, %d acks)\n", cs.rate,
                  nbytes, cs.ppm, cs.cfo, cs.lead, cs.inoise ? " (noise)" : "", cs.sigma, cs.dc, cs.ack, b, whole, acks);
  }
}

// Signals other than Wi-Fi, at 40 MS/s around 2422 MHz (2452 for the oven): Bluetooth's hops,
// a device fixed on one frequency, a microwave oven, and Wi-Fi switching with the same rhythm (not an
// oven).
static void test_signals() {
  std::mt19937 rng(5);
  std::normal_distribution<float> g(0, 1);
  std::uniform_int_distribution<int> bit(0, 1);
  const double fs = 40e6;
  auto run = [&](double centre, const std::vector<std::complex<float>> &x) {
    sig::Finder f;
    f.reset(fs, centre);
    std::vector<sig::Burst> out;
    for (size_t i = 0; i < x.size(); i += 8192) {
      const size_t n = std::min<size_t>(8192, x.size() - i);
      f.feed(&x[i], n, static_cast<double>(i + n) / fs, out);
    }
    return out;
  };
  auto noise = [&](std::vector<std::complex<float>> &x, size_t n) {
    for (size_t i = 0; i < n; i++)
      x.emplace_back(0.05f * g(rng), 0.05f * g(rng));
  };
  // GFSK-like bursts (1 Mb/s, +-250 kHz), 366 us (one slot), every 1250 us, on `chans` (MHz) in turn
  auto bursts = [&](const std::vector<double> &chans, double centre) {
    std::vector<std::complex<float>> x;
    noise(x, 800000); // 20 ms (the finder reports nothing for its first ~13 ms)
    double ph = 0;
    for (double c : chans) {
      const size_t n0 = x.size();
      int b = 0;
      for (size_t i = 0; i < 14640; i++) { // 366 us
        if (i % 40 == 0)
          b = bit(rng);
        ph = std::remainder(ph + 2 * M_PI * ((c * 1e6 - centre) + (b ? 250e3 : -250e3)) / fs, 2 * M_PI);
        x.push_back(std::polar(0.3f, static_cast<float>(ph)) + std::complex<float>(0.05f * g(rng), 0.05f * g(rng)));
      }
      noise(x, 50000 - (x.size() - n0)); // to 1250 us
    }
    noise(x, 40000);
    return x;
  };
  {
    std::vector<double> hop;
    for (int k = 0; k < 24; k++)
      hop.push_back(2404 + (k * 13) % 35); // 2404..2438
    const auto got = run(2422e6, bursts(hop, 2422e6));
    int t = 0, near = 0;
    for (const auto &b : got) {
      t += b.kind == 'T';
      near += std::abs(b.dur_us - 366) < 25 && b.bw_mhz <= 2.5;
    }
    CHECK(got.size() == hop.size());
    CHECK(t >= 20 && near >= 20); // (the first few are only "narrowband": not yet seen hopping)
  }
  {
    const auto got = run(2422e6, bursts(std::vector<double>(24, 2433), 2422e6));
    int n = 0;
    for (const auto &b : got)
      n += b.kind == 'N' && std::abs(b.mhz - 2433) < 0.5;
    CHECK(n == 24 && got.size() == 24);
  }
  // an oven: a carrier wobbling +-3 MHz around 2455 MHz, on 10 ms of every 20 ms, for 1 s
  {
    std::vector<std::complex<float>> x;
    double ph = 0;
    for (size_t i = 0; i < 40000000; i++) {
      const double t = static_cast<double>(i) / fs;
      const bool on = std::fmod(t, 0.02) < 0.01;
      ph = std::remainder(ph + 2 * M_PI * (3e6 + 3e6 * std::sin(2 * M_PI * 700 * t)) / fs, 2 * M_PI);
      x.push_back((on ? std::polar(0.5f, static_cast<float>(ph)) : std::complex<float>(0)) +
                  std::complex<float>(0.05f * g(rng), 0.05f * g(rng)));
    }
    const auto got = run(2452e6, x);
    int m = 0, other = 0;
    for (const auto &b : got) {
      if (b.kind == 'M') {
        m++;
        CHECK(b.mhz > 2451 && b.mhz < 2459);
      } else {
        other++;
      }
    }
    CHECK(m >= 4 && other == 0); // (5 windows; the first has the warm-up in it)
    if (m < 4 || other)
      std::printf("  (oven: %d windows tagged, %d other bursts)\n", m, other);
  }
  // a carrier switched on and off at 67.5 kHz (a 1080p60 display's line rate) at 5773.1 MHz, centre
  // 5770: a steady carrier with that comb, at 40 and at 20 MS/s; a steady line at 5760 MHz (n x 40
  // MHz: the adapter's own crystal) is not reported. A strong clean carrier with phase noise at
  // 5778.3 MHz is, without a comb (its noisy skirt is not one).
  {
    sig::Finder f40, f20;
    f40.reset(fs, 5770e6);
    f20.reset(20e6, 5770e6);
    std::vector<sig::Burst> o40, o20;
    std::vector<std::complex<float>> x(8192), h(4096);
    double ph = 0, ph2 = 0, ph3 = 0;
    for (size_t i = 0; i < 40000000; i += x.size()) {
      for (size_t j = 0; j < x.size(); j++) {
        ph = std::remainder(ph + 2 * M_PI * 3.1e6 / fs, 2 * M_PI);
        ph2 = std::remainder(ph2 + 2 * M_PI * -10e6 / fs, 2 * M_PI);
        ph3 = std::remainder(ph3 + 2 * M_PI * 8.3e6 / fs + 0.01 * g(rng), 2 * M_PI);
        const bool on = std::fmod(static_cast<double>(i + j) / fs * 67.5e3, 1.0) < 0.3;
        x[j] = (on ? std::polar(0.05f, static_cast<float>(ph)) : std::complex<float>(0)) + std::polar(0.03f, static_cast<float>(ph2)) +
               std::polar(2.5f, static_cast<float>(ph3)) + std::complex<float>(0.05f * g(rng), 0.05f * g(rng));
      }
      for (size_t j = 0; j < h.size(); j++)
        h[j] = x[2 * j];
      f40.feed(x.data(), x.size(), static_cast<double>(i + x.size()) / fs, o40);
      f20.feed(h.data(), h.size(), static_cast<double>(i + x.size()) / fs, o20);
    }
    for (const auto *o : {&o40, &o20}) {
      int c = 0, clean = 0, wrong = 0;
      for (const auto &b : *o) {
        if (b.kind != 'C')
          continue;
        if (std::abs(b.mhz - 5773.1) < 0.01 && std::abs(b.comb_khz - 67.5) < 1)
          c++;
        else if (std::abs(b.mhz - 5778.3) < 0.01 && b.comb_khz == 0)
          clean++;
        else
          wrong++;
      }
      CHECK(c >= 2 && clean >= 2 && wrong == 0); // (1 s: about three windows past the 3-of-4 rule)
      if (c < 2 || clean < 2 || wrong)
        std::printf("  (carriers at %s MS/s: %d right, %d clean, %d wrong)\n", o == &o40 ? "40" : "20", c, clean, wrong);
    }
  }
  // Wi-Fi frames (6 Mb/s, ~1.5 ms each) back to back for 10 ms of every 20: loud and switching like
  // an oven, but 20 MHz wide: no oven, no narrow bursts
  {
    std::vector<std::complex<float>> x;
    while (x.size() < 30000000) {
      const size_t n0 = x.size();
      while (x.size() - n0 < 400000) {
        const auto fr = ofdm_frame(0b1101, 1100, -10e6, false, 0, rng);
        for (const auto &v : fr)
          x.push_back(v * 8.9f + std::complex<float>(0.05f * g(rng), 0.05f * g(rng)));
        noise(x, 640); // 16 us
      }
      noise(x, 800000 - (x.size() - n0));
    }
    const auto got = run(2422e6, x);
    CHECK(got.empty());
    if (!got.empty())
      std::printf("  (Wi-Fi: %zu bursts, first '%c' %.1f MHz %.0f us)\n", got.size(), got[0].kind, got[0].mhz, got[0].dur_us);
  }
}

// A BLE advertisement (ADV_IND from a random address c0:ff:ee:12:34:56 with the name "test-ble"),
// GFSK (BT 0.5, +-250 kHz) on channel 38 (2426 MHz) 40 kHz off, at 40 MS/s around 2422 MHz: decoded.
static void test_ble() {
  std::mt19937 rng(9);
  std::normal_distribution<float> g(0, 1);
  const std::string name = "test-ble";
  std::vector<uint8_t> pdu = {0x40, 0, 0x56, 0x34, 0x12, 0xee, 0xff, 0xc0, 2, 0x01, 0x06, static_cast<uint8_t>(name.size() + 1), 0x09};
  pdu.insert(pdu.end(), name.begin(), name.end());
  pdu[1] = static_cast<uint8_t>(pdu.size() - 2);
  std::vector<uint8_t> bits;
  for (uint8_t b : pdu)
    for (int j = 0; j < 8; j++)
      bits.push_back((b >> j) & 1);
  unsigned reg = 0x555555; // CRC-24, sent MSB first
  for (uint8_t b : bits) {
    const unsigned fb = ((reg >> 23) & 1) ^ b;
    reg = (reg << 1) & 0xFFFFFF;
    if (fb)
      reg ^= 0x00065B;
  }
  for (int i = 23; i >= 0; i--)
    bits.push_back((reg >> i) & 1);
  for (unsigned i = 0, w = (38 & 0x3F) | 0x40; i < bits.size(); i++) { // whitening
    bits[i] ^= w & 1;
    w = (w >> 1) ^ ((w & 1) ? 0x44 : 0);
  }
  std::vector<uint8_t> air = {1, 0, 1, 0, 1, 0, 1, 0}; // preamble, then the access address (LSB first)
  for (int i = 0; i < 32; i++)
    air.push_back((0x8E89BED6u >> i) & 1);
  air.insert(air.end(), bits.begin(), bits.end());
  // GFSK: NRZ at 40 samples a bit through a Gaussian (BT 0.5), the phase integrated
  const double fs = 40e6;
  std::vector<double> nrz(air.size() * 40), gs;
  for (size_t i = 0; i < nrz.size(); i++)
    nrz[i] = air[i / 40] ? 1 : -1;
  const double sigma = std::sqrt(std::log(2.0)) / (2 * M_PI * 0.5) * 40;
  for (int k = -80; k <= 80; k++)
    gs.push_back(std::exp(-0.5 * k * k / (sigma * sigma)));
  double gsum = 0;
  for (double v : gs)
    gsum += v;
  std::vector<std::complex<float>> x;
  for (int i = 0; i < 800000; i++) // 20 ms (the finder's warm-up)
    x.emplace_back(0.05f * g(rng), 0.05f * g(rng));
  double ph = 0;
  for (size_t i = 0; i < nrz.size(); i++) {
    double f = 0;
    for (size_t k = 0; k < gs.size(); k++) {
      const long j = static_cast<long>(i) + static_cast<long>(k) - 80;
      f += gs[k] * (j < 0 ? -1 : j >= static_cast<long>(nrz.size()) ? 1 : nrz[static_cast<size_t>(j)]);
    }
    ph = std::remainder(ph + 2 * M_PI * (4e6 + 40e3 + 250e3 * f / gsum) / fs, 2 * M_PI);
    x.push_back(std::polar(0.3f, static_cast<float>(ph)) + std::complex<float>(0.05f * g(rng), 0.05f * g(rng)));
  }
  for (int i = 0; i < 80000; i++)
    x.emplace_back(0.05f * g(rng), 0.05f * g(rng));
  sig::Finder f;
  f.reset(fs, 2422e6);
  std::vector<sig::Burst> out;
  for (size_t i = 0; i < x.size(); i += 8192) {
    const size_t n = std::min<size_t>(8192, x.size() - i);
    f.feed(&x[i], n, static_cast<double>(i + n) / fs, out);
  }
  int e = 0;
  for (const auto &b : out)
    if (b.kind == 'E') {
      e++;
      CHECK(b.addr == "c0:ff:ee:12:34:56" && b.random && b.name == name && std::abs(b.mhz - 2426) < 0.1);
    }
  CHECK(e == 1 && out.size() == 1);
  if (e != 1 || out.size() != 1)
    std::printf("  (BLE: %zu reported, %d decoded)\n", out.size(), e);
  // 10 ms of a carrier on that channel (longer than the samples kept): no packet, no crash
  std::vector<std::complex<float>> y;
  for (int i = 0; i < 800000; i++)
    y.emplace_back(0.05f * g(rng), 0.05f * g(rng));
  for (int i = 0; i < 400000; i++)
    y.push_back(std::polar(0.3f, static_cast<float>(std::remainder(2 * M_PI * 4e6 * i / fs, 2 * M_PI))) + std::complex<float>(0.05f * g(rng), 0.05f * g(rng)));
  for (int i = 0; i < 80000; i++)
    y.emplace_back(0.05f * g(rng), 0.05f * g(rng));
  f.reset(fs, 2422e6);
  out.clear();
  for (size_t i = 0; i < y.size(); i += 8192) {
    const size_t n = std::min<size_t>(8192, y.size() - i);
    f.feed(&y[i], n, static_cast<double>(i + n) / fs, out);
  }
  for (const auto &b : out)
    CHECK(b.kind != 'E');
}

// Network names: whatever bytes a beacon carries, the text is valid UTF-8 the JSON writer accepts.
static void test_ssid_text() {
  auto t = [](const std::string &b) { return wifi::ssid_text(reinterpret_cast<const uint8_t *>(b.data()), b.size()); };
  CHECK(t("homelab") == "homelab");
  CHECK(t("Kaffe\xc3\xa5") == "Kaffe\xc3\xa5");          // valid UTF-8 kept
  CHECK(t(std::string("\0\0\0", 3)).empty());             // hidden network
  CHECK(t("\xc0\x80") == "??");                            // overlong
  CHECK(t("\xed\xa0\x80") == "???");                      // surrogate
  CHECK(t("\xf4\x90\x80\x80") == "????");                // past U+10FFFF
  CHECK(t("a\xe2\x80\xae" "b") == "a???b");                  // right-to-left override
  CHECK(t("a\x01" "b") == "a?b");                              // control character
  CHECK(t(std::string("abc\0\0", 5)) == "abc");             // zero padding
  for (int i = 0; i < 2000; i++) { // random bytes: dump() must not throw
    std::string b;
    for (int k = 0; k < 32; k++)
      b += static_cast<char>((i * 131 + k * 197 + (i >> 3) * k) & 0xff);
    bool ok = true;
    try {
      (void)nlohmann::json(t(b)).dump();
    } catch (...) {
      ok = false;
    }
    CHECK(ok);
  }
}

// --wifi <file.sigmf-data>: the frame finder over a recording (ci16, rate and centre from its meta).
static int wifi_scan(const char *path) {
  std::string p(path), mp = p.substr(0, p.rfind(".sigmf-data")) + ".sigmf-meta";
  std::ifstream m(mp);
  std::string meta((std::istreambuf_iterator<char>(m)), std::istreambuf_iterator<char>());
  auto num = [&](const char *key) {
    const auto k = meta.find(key);
    return k == std::string::npos ? 0.0 : std::atof(meta.c_str() + meta.find(':', k + std::strlen(key)) + 1);
  };
  const double fs = num("\"core:sample_rate\""), fc = num("\"core:frequency\"");
  std::ifstream f(p, std::ios::binary);
  wifi::Decoder dec;
  dec.reset(fs, fc);
  std::vector<int16_t> raw(2 * 8192);
  std::vector<std::complex<float>> x(8192);
  std::vector<wifi::Frame> got;
  sig::Finder sf;
  sf.reset(fs, fc);
  std::vector<sig::Burst> other;
  uint64_t done = 0;
  double secs = 0, ssecs = 0;
  while (f.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * 2))) {
    for (size_t i = 0; i < 8192; i++)
      x[i] = {raw[2 * i] / 16.f, raw[2 * i + 1] / 16.f};
    done += 8192;
    const auto t0 = std::chrono::steady_clock::now();
    dec.feed(x.data(), 8192, static_cast<double>(done) / fs, got);
    const auto t1 = std::chrono::steady_clock::now();
    sf.feed(x.data(), 8192, static_cast<double>(done) / fs, other);
    secs += std::chrono::duration<double>(t1 - t0).count();
    ssecs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  }
  std::map<std::string, int> kinds;
  for (const auto &o : other) {
    char k[64];
    std::snprintf(k, sizeof k, "%c %.0f MHz %s%s%s", o.kind, o.mhz, o.addr.c_str(), o.random ? " (random)" : "", o.name.empty() ? "" : (" " + o.name).c_str());
    if (o.kind == 'C')
      std::snprintf(k, sizeof k, "C %.3f MHz comb %.1f kHz", o.mhz, o.comb_khz);
    kinds[k]++;
  }
  std::printf("other signals (found in %.2f s): %zu\n", ssecs, other.size());
  for (const auto &[k, n] : kinds)
    std::printf("  %-14s %d\n", k.c_str(), n);
  std::map<std::string, int> by;
  for (const auto &fr : got) {
    char k[64];
    std::snprintf(k, sizeof k, "%.0f MHz %d MHz %c %2d Mb/s", fr.centre_hz / 1e6, fr.bw_mhz, fr.fmt, fr.rate_mbps);
    by[k]++;
  }
  std::printf("%.2f s of %.0f MS/s at %.1f MHz: %zu frames (decoded in %.2f s, %.1fx real time)\n", static_cast<double>(done) / fs,
              fs / 1e6, fc / 1e6, got.size(), secs, static_cast<double>(done) / fs / secs);
  for (const auto &[k, n] : by)
    std::printf("  %-28s %d\n", k.c_str(), n);
  int six = 0, mac = 0;
  std::map<std::string, int> nets, senders;
  auto hex = [](const std::array<uint8_t, 6> &a) {
    char b[18];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
    return std::string(b);
  };
  int b = 0, bmac = 0;
  for (const auto &fr : got) {
    if (fr.fmt == 'B') {
      b++;
      bmac += fr.mac;
    } else if (fr.rate_mbps == 6 && fr.fmt == 'L') {
      six++;
      mac += fr.mac;
    }
    if (!fr.mac)
      continue;
    if (fr.beacon)
      nets["\"" + fr.ssid + "\" " + hex(fr.bssid) + " ch " + std::to_string(fr.channel)]++;
    if (fr.has_ta)
      senders[hex(fr.ta)]++;
  }
  std::printf("6 Mb/s legacy frames: %d, decoded whole (CRC good): %d; 802.11b frames: %d, whole: %d\n", six, mac, b, bmac);
  for (const auto &[k, n] : nets)
    std::printf("  network %s  x%d\n", k.c_str(), n);
  for (const auto &[k, n] : senders)
    if (n >= 3)
      std::printf("  sender %s  x%d\n", k.c_str(), n);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 2 && std::string(argv[1]) == "--wifi")
    return wifi_scan(argv[2]);
  if (argc > 1 && std::string(argv[1]) == "--bench")
    return bench();
  test_channels();
  test_spectrum_tone();
  test_offgrid();
  test_level_vs_fft();
  test_reduce_and_frame();
  test_gapped();
  test_crop();
  test_wifi_frames();
  test_ssid_text();
  test_dsss_beacon();
  test_signals();
  test_ble();
  std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
  return g_fail ? 1 : 0;
}
