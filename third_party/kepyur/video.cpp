// kepyur video decoder. See video.h. The algorithm follows vtx-spike/viewer/vtxview.cpp (measured on a
// real R4 NTSC VTX: 93-96 % of lines locked); changes are marked "kepyur:".
#include "video.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace kp {

const VideoStd kNtsc{63.5556, 9.4, 52.6, 240, 18, "NTSC"};
const VideoStd kPal{64.0, 10.5, 52.0, 288, 22, "PAL"};

namespace {

inline float fast_atan2(float y, float x) {
  const float ax = std::fabs(x), ay = std::fabs(y);
  const float mx = std::max(ax, ay), mn = std::min(ax, ay);
  const float a = mn / (mx + 1e-20f);
  const float s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  if (y < 0) r = -r;
  return r;
}

struct Biquad {  // RBJ low-pass
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void design(double fc, double fs, double q = 0.7071) {
    const double w = 2 * M_PI * fc / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
    const double a0 = 1 + al;
    b0 = static_cast<float>((1 - c) / 2 / a0);
    b1 = static_cast<float>((1 - c) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2 * c / a0);
    a2 = static_cast<float>((1 - al) / a0);
  }
  void notch(double fc, double fs, double q) {
    const double w = 2 * M_PI * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    const double a0 = 1 + al;
    b0 = static_cast<float>(1 / a0);
    b1 = static_cast<float>(-2 * c / a0);
    b2 = b0;
    a1 = b1;
    a2 = static_cast<float>((1 - al) / a0);
  }
  inline float run(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

}  // namespace

// ---- the decoder (streaming, one sample of discriminator output at a time, MHz) ----------------------
class Decoder {
public:
  Decoder(double fs, FrameSink sink, const DecoderOpts &o) : fs(fs), sink(std::move(sink)), o(o) {
    luma_ring.assign(8192, 0);
    chroma_ring.assign(8192, cf{});
    ring_mask = 8191;
    // chroma: mixed down by the subcarrier, 4th-order Butterworth low-pass at 1.2 MHz (U/V bandwidth)
    for (Biquad *b : {&cl_r1, &cl_i1}) b->design(1.2e6, fs, 0.54);
    for (Biquad *b : {&cl_r2, &cl_i2}) b->design(1.2e6, fs, 1.31);
    lp_luma.design(3.0e6, fs, 0.54);
    lp_luma2.design(3.0e6, fs, 1.31);
    lp_sync.design(1.0e6, fs);
    acq.resize(static_cast<size_t>(fs * 0.06));  // 60 ms
    hist.assign(800, 0);
    set_std(o.std_force == 2 ? kPal : kNtsc);
  }

  DecoderStats stats() const {
    DecoderStats s;
    s.acquiring = acquiring;
    s.sign = sign;
    s.tip = tip;
    s.blank = blank;
    s.lines = lines;
    s.lines_locked = lines_locked;
    s.fields = fields_seen;
    s.frames = frames_out;
    s.acquisitions = acquisitions;
    s.losses = losses;
    s.line_us = T / fs * 1e6;
    s.lock_recent = recent_n ? static_cast<float>(recent_locked) / recent_n : 0.f;
    s.std_name = std_->name;
    s.rephases = rephases;
    s.colour = colour_on;
    s.burst_err = c_err;
    return s;
  }

  inline void push(float d) {
    // sync path: 5 MHz LP + 3-point median; luma path: 3 MHz LP (two sections)
    const float s = lp_sync.run(d);
    const float m = std::max(std::min(med_a, med_b), std::min(std::max(med_a, med_b), s));
    med_b = med_a;
    med_a = s;
    luma_ring[n & ring_mask] = luma_notch.run(lp_luma2.run(lp_luma.run(d)));
    if (o.colour) {
      // the oscillator's phase belongs to the absolute sample index: burst and picture share it
      const cf c = d * std::conj(lo);
      lo *= lo_step;
      if ((n & 4095) == 0) lo /= std::abs(lo);
      chroma_ring[n & ring_mask] = cf(cl_r2.run(cl_r1.run(c.real())), cl_i2.run(cl_i1.run(c.imag())));
    }
    const uint64_t now = n++;
    if (acquiring) {
      if (acq_n < acq.size())
        acq[acq_n++] = m;
      else
        acquire();
      return;
    }
    if (gated_lines < 64) track_levels(m);  // until the sync-gated measurement has settled
    const float w = sign * m;
    // pulse tracking
    if (!low && w < thr) {
      low = true;
      low_start = now;
      longrun = false;
    } else if (low) {
      if (!longrun && now - low_start > 0.3 * T) {
        longrun = true;
        if (!line_has_longrun) {
          line_has_longrun = true;
          line_longrun_start = low_start;
        }
      }
      if (w >= thr) {
        low = false;
        const double wid = (now - low_start) / fs * 1e6;
        if (wid > 3.0 && wid < 6.5) on_hsync(static_cast<double>(low_start));
      }
      line_low++;
    }
    // freewheel: line overdue by > 8 % with no edge
    if (line_start >= 0 && now > line_start + T * 1.08) {
      const double ls = line_start;
      miss_run++;
      count_line(false);
      render_line(ls, false);
      close_line(ls + T);
      check_lost();
    }
  }

private:
  double fs;
  FrameSink sink;
  DecoderOpts o;
  const VideoStd *std_ = &kNtsc;
  float sign = +1, tip = -1, blank = 0, thr = -0.5f;
  uint64_t n = 0;
  std::vector<float> luma_ring;
  std::vector<cf> chroma_ring;  // chroma at baseband (U/V before the burst rotation)
  // kepyur: NTSC chroma comb (DecoderOpts::comb). The subcarrier is 227.5 x the line rate, so at the same
  // place one line away chroma comes back with the subcarrier's sign flipped while luma detail does not:
  // combing cancels the luma near the subcarrier that the 1.2 MHz chroma filter lets through as rainbows.
  // It works on the baseband chroma already in chroma_ring, inside render_line: each line is drawn one line
  // late, once the line below is in, and combed with both neighbours (centred, so chroma stays on luma's
  // line). Adaptive: per pixel a neighbour only counts as far as its luma there matches this line's, so the
  // top and bottom edges of OSD glyphs fall back to the plain filter instead of keeping half the rainbow.
  // Luma keeps the notch. PAL (2H subcarrier pattern) is unchanged.
  struct CombLine {
    double ls = -1;
    int64_t k = -1;
    int field = -1;
    bool col = false;  // drawn in colour
    bool ok = false;   // usable as a neighbour: locked, in colour, burst on the PLL's predicted phase
    uint64_t epoch = 0;
    double phase = 0;  // c_phase and c_mag after this line's burst: it is drawn after the next line's
    float mag = 0;
  };
  static constexpr float kCombFull = 0.08f, kCombNone = 0.25f;  // luma mismatch, of black-to-white
  static constexpr float kCombMaxErr = 0.35f;  // rad: a burst further off means the stream jumped
  bool comb = false;
  double comb_w = 0;           // the mixing oscillator's frequency, rad per sample
  uint64_t comb_epoch = 0;     // bumped where the sample stream may have jumped: lines across it don't mix
  CombLine comb_up, comb_mid;  // the two lines before the current one
  float burst_e = 0;           // |phase error| of the last burst against the PLL's prediction
  std::vector<float> comb_m;   // draw_line: the comb on every other pixel (see there)
  std::vector<cf> comb_o;
  size_t ring_mask;
  // kepyur: colour. A free-running oscillator at the nominal subcarrier mixes chroma to baseband; a
  // per-line PLL on the burst tracks the transmitter's subcarrier phase (and its small frequency error);
  // PAL: the burst's +-45 degree swing gives the V switch, and adjacent lines are averaged (PAL-D).
  Biquad luma_notch, cl_r1, cl_r2, cl_i1, cl_i2;
  cf lo{1, 0}, lo_step{1, 0};
  double c_phase = 0, c_dphi = 0;  // burst reference phase (rad) and its drift per line
  int64_t c_last_line = -1;
  float c_mag = 0, c_err = 1;      // burst amplitude (MHz), mean |phase error| (rad)
  bool colour_on = false;
  int c_good = 0;
  std::vector<cf> prev_uv;         // PAL-D: the previous line's U/V
  float med_a = 0, med_b = 0;
  Biquad lp_luma, lp_luma2, lp_sync;
  bool low = false, longrun = false;
  uint64_t low_start = 0;
  double T = 0, line_start = -1;
  uint64_t lines = 0, lines_locked = 0, line_idx = 0, acquisitions = 0, losses = 0;
  uint64_t line_low = 0;
  bool line_has_longrun = false;
  uint64_t line_longrun_start = 0;
  bool prev_line_vsync = false;
  std::vector<float> acq;
  size_t acq_n = 0;
  bool acquiring = true;
  std::vector<uint8_t> fieldA, fieldB, frame;
  bool haveA = false, haveB = false;
  int cur_field = -1;
  int64_t field_line0 = 0;
  uint64_t frames_out = 0, fields_seen = 0;
  std::vector<uint32_t> hist;
  double hlo = -8, hhi = 8;
  int hdec = 0;
  uint64_t hist_count = 0;
  // kepyur: sync-gated levels: sync tip from the middle of each hsync pulse, blanking from the back porch,
  // measured on locked active lines and smoothed over ~64 lines. Picture content cannot move them (the
  // spike's histogram peaks could, and the threshold wandered with the scene).
  uint64_t gated_lines = 0;
  // kepyur: re-phase instead of re-acquire. After a jump in the signal (stream restart, a dropped bank)
  // the hsyncs keep coming, one line apart, but outside the PLL's window. A run of them on a steady grid
  // while the PLL freewheels moves the line grid onto them within a few lines.
  int miss_run = 0, stray_run = 0;
  double last_stray = -1;
  uint64_t rephases = 0;
  // kepyur: lock quality over a sliding window of lines (the spike counted since acquisition)
  static constexpr int kRecent = 1024;
  uint8_t recent[kRecent] = {};
  int recent_pos = 0, recent_n = 0, recent_locked = 0;

  void set_std(const VideoStd &s) {
    std_ = &s;
    T = s.T_us * 1e-6 * fs;
    fieldA.assign(static_cast<size_t>(o.width) * s.active * 4, 0);
    fieldB = fieldA;
    frame.assign(static_cast<size_t>(o.width) * s.active * 2 * 4, 0);
    const double fsc = &s == &kPal ? 4.43361875e6 : 315e6 / 88;
    const double a = 2 * M_PI * fsc / fs;
    lo_step = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    luma_notch.notch(fsc, fs, 1.5);  // the subcarrier out of the picture (also the B/W herringbone)
    comb = o.comb && &s != &kPal;
    comb_w = a;
    comb_epoch++;
    prev_uv.assign(o.width, cf{});
    colour_on = false;
    c_good = 0;
    c_last_line = -1;
  }

  void count_line(bool locked) {
    lines++;
    if (locked) lines_locked++;
    if (recent_n == kRecent) recent_locked -= recent[recent_pos];
    else recent_n++;
    recent[recent_pos] = locked;
    recent_locked += locked;
    recent_pos = (recent_pos + 1) % kRecent;
  }

  // kepyur: lost when fewer than 40 % of the last ~2 fields' lines found their hsync (a healthy picture
  // locks ~90 %: only the vertical interval freewheels). The spike used the ratio since acquisition.
  void check_lost() {
    const int window = std::min(kRecent, 2 * (std_ == &kPal ? 312 : 262));
    if (recent_n < window) return;
    if (recent_locked * 10 < recent_n * 4) {
      losses++;
      KP_LOGI("video: lost lock (%d/%d recent lines), re-acquiring", recent_locked, recent_n);
      restart_acquisition();
    }
  }

  void restart_acquisition() {
    acquiring = true;
    acq_n = 0;
    lines = lines_locked = 0;
    cur_field = -1;
    haveA = haveB = false;
    recent_n = recent_locked = recent_pos = 0;
    line_start = -1;
    gated_lines = 0;
    miss_run = stray_run = 0;
    last_stray = -1;
    comb_epoch++;
  }

  void track_levels(float m) {
    // level tracking histogram (every 8th sample), refreshed every ~40 ms
    if ((++hdec & 7) != 0) return;
    const float v = sign * m;
    const int nb = static_cast<int>(hist.size());
    const int b = static_cast<int>((v - hlo) / (hhi - hlo) * nb);
    if (b >= 0 && b < nb) {
      hist[b]++;
      hist_count++;
    }
    if (hist_count >= static_cast<uint64_t>(fs * 0.04 / 8)) {
      float t, bl;
      if (levels_from_hist(static_cast<uint32_t>(hist_count), t, bl)) {
        tip = 0.8f * tip + 0.2f * t;
        blank = 0.8f * blank + 0.2f * bl;
        thr = 0.5f * (tip + blank);
      }
      std::fill(hist.begin(), hist.end(), 0);
      hist_count = 0;
    }
  }

  bool levels_from(const std::vector<float> &buf, size_t cnt, float sgn, float &tip_o, float &blank_o) {
    std::fill(hist.begin(), hist.end(), 0);
    const int nb = static_cast<int>(hist.size());
    uint32_t total = 0;
    for (size_t i = 0; i < cnt; i += 4) {
      const float v = sgn * buf[i];
      const int b = static_cast<int>((v - hlo) / (hhi - hlo) * nb);
      if (b < 0 || b >= nb) continue;
      hist[b]++;
      total++;
    }
    const bool ok = levels_from_hist(total, tip_o, blank_o);
    std::fill(hist.begin(), hist.end(), 0);
    return ok;
  }

  bool levels_from_hist(uint32_t total, float &tip_o, float &blank_o) {
    const int nb = static_cast<int>(hist.size());
    if (total < 1000) return false;
    uint32_t c = 0;
    int p0 = 0, p1 = nb - 1;
    for (int i = 0; i < nb; i++) {
      c += hist[i];
      if (c >= total * 0.005) { p0 = i; break; }
    }
    c = 0;
    for (int i = nb - 1; i >= 0; i--) {
      c += hist[i];
      if (c >= total * 0.005) { p1 = i; break; }
    }
    if (p1 - p0 < 20) return false;
    std::vector<float> sm(nb, 0);
    for (int i = 0; i < nb; i++) {
      float s = 0;
      for (int k = -4; k <= 4; k++) {
        const int j = i + k;
        if (j >= 0 && j < nb) s += hist[j];
      }
      sm[i] = s / 9;
    }
    float mx = 0;
    for (int i = p0; i <= p1; i++) mx = std::max(mx, sm[i]);
    std::vector<int> peaks;
    const int dist = std::max(3, (p1 - p0) / 40);
    for (int i = p0; i <= p1; i++) {
      if (sm[i] < mx * 0.02) continue;
      bool ok = true;
      for (int k = -dist; k <= dist && ok; k++) {
        const int j = i + k;
        if (k != 0 && j >= 0 && j < nb && sm[j] > sm[i]) ok = false;
      }
      if (ok && (peaks.empty() || i - peaks.back() > dist)) peaks.push_back(i);
    }
    if (peaks.size() < 2) return false;
    const float bw = static_cast<float>((hhi - hlo) / nb);
    const float tipv = static_cast<float>(hlo) + (peaks[0] + 0.5f) * bw;
    const float span = (p1 - p0) * bw;
    for (size_t k = 1; k < peaks.size(); k++) {
      const float v = static_cast<float>(hlo) + (peaks[k] + 0.5f) * bw;
      if (v - tipv > 0.12f * span) {
        tip_o = tipv;
        blank_o = v;
        return true;
      }
    }
    return false;
  }

  // acquisition: pick polarity + levels + line period from a 60 ms buffer
  void acquire() {
    float best = 0, bt = 0, bb = 0, bs = 0;
    for (float sgn : {+1.f, -1.f}) {
      if (o.force_invert && sgn > 0) continue;
      float t, b;
      if (!levels_from(acq, acq_n, sgn, t, b)) continue;
      const float th = 0.5f * (t + b);
      std::vector<double> edges;
      bool lo = false;
      size_t ls = 0;
      for (size_t i = 0; i < acq_n; i++) {
        const float v = sgn * acq[i];
        if (!lo && v < th) {
          lo = true;
          ls = i;
        } else if (lo && v >= th) {
          lo = false;
          const double w = (i - ls) / fs * 1e6;
          if (w > 3.0 && w < 6.5) edges.push_back(static_cast<double>(ls));
        }
      }
      if (edges.size() < 30) continue;
      std::vector<double> good;
      for (size_t k = 1; k < edges.size(); k++) {
        const double iv = (edges[k] - edges[k - 1]) / fs * 1e6;
        if (iv > 62.5 && iv < 65.0) good.push_back(iv);
      }
      const float score = static_cast<float>(good.size()) / (edges.size() - 1);
      KP_LOGD("video acq: sign %+.0f tip %.2f blank %.2f, %zu pulses, score %.2f", sgn, t, b, edges.size(), score);
      if (score > best && !good.empty()) {
        best = score;
        bs = sgn;
        bt = t;
        bb = b;
        std::sort(good.begin(), good.end());
        const double Tus = good[good.size() / 2];
        if (!o.std_force) {
          const VideoStd &s = std::fabs(Tus - kNtsc.T_us) < std::fabs(Tus - kPal.T_us) ? kNtsc : kPal;
          if (&s != std_) set_std(s);
        }
        T = Tus * 1e-6 * fs;
      }
    }
    if (best > 0.3f) {
      sign = bs;
      tip = bt;
      blank = bb;
      thr = 0.5f * (tip + blank);
      acquiring = false;
      line_start = -1;
      acquisitions++;
      KP_LOGI("video: acquired, sync %s end, tip %.2f blank %.2f MHz, line %.3f us -> %s, score %.2f",
              sign > 0 ? "low" : "high", tip, blank, T / fs * 1e6, std_->name, best);
    }
    acq_n = 0;
  }

  // Burst: chroma over the burst window (NTSC 5.3-7.8 us, PAL 5.6-7.85 us after the hsync edge; the
  // chroma filter delays it ~0.35 us). Updates the reference phase; returns false when there is none.
  bool burst(double ls, bool &pal_v) {
    const double us = 1e-6 * fs;
    const int64_t i0 = static_cast<int64_t>(ls + 6.0 * us), i1 = static_cast<int64_t>(ls + 7.8 * us);
    if (i1 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - i0 > static_cast<int64_t>(ring_mask)) return false;
    cf b{};
    for (int64_t i = i0; i < i1; i++) b += chroma_ring[i & ring_mask];
    b /= static_cast<float>(i1 - i0);
    const float mag = std::abs(b);
    const int64_t li = static_cast<int64_t>(line_idx);
    const double pred = c_last_line < 0 ? std::arg(b) : c_phase + c_dphi * (li - c_last_line);
    double e = std::arg(b * std::polar(1.f, static_cast<float>(-pred)));
    pal_v = false;
    if (std_ == &kPal) {
      // The burst sits 45 degrees either side of the reference: at baseband a normal line's burst (135 deg
      // in U/V) lands at -45, a V-switched line's (225 deg) at +45.
      pal_v = e > 0;
      e += pal_v ? -M_PI / 4 : M_PI / 4;
    }
    burst_e = static_cast<float>(std::fabs(e));
    if (c_last_line < 0) {
      c_phase = pred + e;
      c_dphi = 0;
    } else {
      c_phase = pred + 0.2 * e;
      c_dphi += 0.01 * e / std::max<int64_t>(1, li - c_last_line);
    }
    c_phase = std::remainder(c_phase, 2 * M_PI);
    c_last_line = li;
    c_mag += 0.05f * (mag - c_mag);
    c_err += 0.05f * (static_cast<float>(std::fabs(e)) - c_err);
    // colour killer, with hysteresis: a burst near the expected size (~1/4 of the sync depth) and steady
    const bool ok = c_mag > 0.08f * (blank - tip) && c_err < 0.5f;
    c_good = ok ? std::min(c_good + 1, 60) : std::max(c_good - 2, 0);
    if (!colour_on && c_good >= 40) colour_on = true;
    if (colour_on && c_good == 0) colour_on = false;
    return colour_on;
  }

  void render_line(double ls, bool locked_line) {
    if (cur_field < 0) return;
    const int64_t k = static_cast<int64_t>(line_idx) - field_line0 - std_->skip;
    if (comb) {
      comb_line(ls, k, locked_line);
      return;
    }
    if (k < 0 || k >= std_->active) return;
    if (locked_line) measure_levels(ls);
    bool pal_v = false;
    const bool col = o.colour && locked_line && burst(ls, pal_v);
    draw_line(ls, k, col, pal_v, false, false, c_phase, c_mag);
  }

  // NTSC comb: line k is measured now (levels, burst) and the previous one, k - 1, is drawn, combed with
  // k - 2 and k. A neighbour is used only if both lines are locked and in colour, in the same field and on
  // the same side of any jump in the stream (comb_epoch, and a burst on its predicted phase).
  void comb_line(double ls, int64_t k, bool locked_line) {
    CombLine cur;
    cur.ls = ls;
    cur.k = k;
    cur.field = cur_field;
    cur.epoch = comb_epoch;
    if (k >= 0 && k < std_->active) {
      if (locked_line) measure_levels(ls);
      bool pal_v = false;
      cur.col = o.colour && locked_line && burst(ls, pal_v);
      cur.ok = cur.col && burst_e < kCombMaxErr;
      cur.phase = c_phase;
      cur.mag = c_mag;
    }
    const CombLine &m = comb_mid;
    if (m.field == cur_field && m.k == k - 1 && m.k >= 0 && m.k < std_->active) {
      const auto pair = [&](const CombLine &x, int64_t dk) {
        return m.ok && x.ok && x.field == m.field && x.k == m.k + dk && x.epoch == m.epoch;
      };
      draw_line(m.ls, m.k, m.col, false, pair(comb_up, -1), pair(cur, 1), m.phase, m.mag);
    }
    comb_up = comb_mid;
    comb_mid = cur;
  }

  // up / dn: comb this line's chroma with the line above / below (see comb_line)
  void draw_line(double ls, int64_t k, bool col, bool pal_v, bool up, bool dn, double phase, float mag) {
    uint32_t *row = reinterpret_cast<uint32_t *>((cur_field == 0 ? fieldA : fieldB).data()) + static_cast<size_t>(k) * o.width;
    const double a0 = std_->a0_us * 1e-6 * fs, aw = std_->aw_us * 1e-6 * fs;
    const float white = blank + 2.5f * (blank - tip);
    const float yscale = 1.f / (white - blank);
    // chroma -> U/V in luma units: the burst is 20/92.5 (NTSC) or 0.15/0.7 (PAL) of black-to-white, and
    // demodulated chroma comes out as -(U + jV)/2 against a burst of +1/2 (see fpv_sim.py's composite())
    const float k_uv = 0.215f / std::max(mag, 1e-6f);
    const cf rot = std::polar(1.f, static_cast<float>(-phase));
    const double cdel = 0.21e-6 * fs;  // chroma filter delay minus luma filter delay
    const bool pal = std_ == &kPal;
    // the comb's neighbours sit one line period T (Ti whole samples and Tf more) away, turned onto this
    // line's subcarrier phase: the oscillator runs on the absolute sample index, so what is mixed down one
    // line away comes out turned by w*T
    if (dn && ls + a0 + aw + cdel + T + 2 >= static_cast<double>(n)) dn = false;
    if (up && static_cast<double>(n) - (ls + a0 - T) > static_cast<double>(ring_mask) - 8) up = false;
    // The comb, on every other pixel (chroma is 1.2 MHz wide; per pixel cost twice as much for nothing
    // visible) and interpolated in between below: chroma becomes c * (1 - comb_m) - comb_o.
    const bool comb_on = col && (up || dn);
    if (comb_on) {
      const int64_t Ti = static_cast<int64_t>(T);
      const float Tf = static_cast<float>(T - Ti);
      const cf turn = std::polar(1.f, static_cast<float>(-comb_w * T));  // the line above; below: conj
      const float wk = std::fabs(yscale) / (kCombNone - kCombFull), w1 = kCombNone / (kCombNone - kCombFull);
      const int nc = (o.width + 1) / 2;
      comb_m.resize(nc + 1);
      comb_o.resize(nc + 1);
      const double step = 2 * aw / o.width;
      for (int h = 0; h < nc; h++) {
        comb_m[h] = 0;
        comb_o[h] = cf{};
        const double p = ls + a0 + step * h, pc = p + cdel;
        const int64_t i0 = static_cast<int64_t>(p + 0.5), j0 = static_cast<int64_t>(pc);
        if (j0 + Ti + 3 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - (i0 - Ti) > static_cast<int64_t>(ring_mask) - 8) continue;
        // a neighbour's weight: 1 while its luma here is within kCombFull of this line's, 0 past kCombNone
        const float y0 = luma_ring[i0 & ring_mask];
        const float wu = up ? std::clamp(w1 - std::fabs(luma_ring[(i0 - Ti) & ring_mask] - y0) * wk, 0.f, 1.f) : 0.f;
        const float wd = dn ? std::clamp(w1 - std::fabs(luma_ring[(i0 + Ti) & ring_mask] - y0) * wk, 0.f, 1.f) : 0.f;
        if (wu + wd <= 0) continue;
        // A neighbour turned onto this line has chroma -c and the same luma detail, so c + the neighbours' mean
        // is twice the detail: take out half of it per unit of the best weight (all of it at 1).
        const float m = 0.5f * std::max(wu, wd), sc = m / (wu + wd);
        const float fc = static_cast<float>(pc - j0);
        // chroma_ring at j + t (0 <= t < 2), in plain floats: branch-free, and std::complex cost more here
        float orr = 0, oi = 0;
        const auto take = [&](int64_t j, float t, float rr, float ri) {
          const int iw = static_cast<int>(t);
          j += iw;
          t -= static_cast<float>(iw);
          const cf a = chroma_ring[j & ring_mask], b = chroma_ring[(j + 1) & ring_mask];
          const float xr = a.real() + (b.real() - a.real()) * t, xi = a.imag() + (b.imag() - a.imag()) * t;
          orr += xr * rr - xi * ri;
          oi += xr * ri + xi * rr;
        };
        if (wu > 0) take(j0 - Ti - 1, fc + 1 - Tf, turn.real() * sc * wu, turn.imag() * sc * wu);
        if (wd > 0) take(j0 + Ti, fc + Tf, turn.real() * sc * wd, -turn.imag() * sc * wd);
        comb_m[h] = m;
        comb_o[h] = cf(orr, oi);
      }
      comb_m[nc] = comb_m[nc - 1];
      comb_o[nc] = comb_o[nc - 1];
    }
    for (int x = 0; x < o.width; x++) {
      const double p = ls + a0 + aw * x / o.width;
      const int64_t i0 = static_cast<int64_t>(p);
      if (i0 + 8 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - i0 > static_cast<int64_t>(ring_mask) - 8) {
        row[x] = 0xff000000u;
        continue;
      }
      const float f = static_cast<float>(p - i0);
      const float v0 = luma_ring[i0 & ring_mask], v1 = luma_ring[(i0 + 1) & ring_mask];
      const float y = sign * (v0 + (v1 - v0) * f - sign * blank) * yscale;
      float r = y, g = y, bl = y;
      if (col) {
        const double pc = p + cdel;
        const int64_t j0 = static_cast<int64_t>(pc);
        const float fc = static_cast<float>(pc - j0);
        const cf c0 = chroma_ring[j0 & ring_mask], c1 = chroma_ring[(j0 + 1) & ring_mask];
        cf c = c0 + (c1 - c0) * fc;
        if (comb_on) {
          const int h = x >> 1;
          if (x & 1) c = c * (1 - 0.5f * (comb_m[h] + comb_m[h + 1])) - 0.5f * (comb_o[h] + comb_o[h + 1]);
          else c = c * (1 - comb_m[h]) - comb_o[h];
        }
        c = c * rot * k_uv;
        float u = -c.real(), v = -c.imag();
        if (pal) {
          if (pal_v) v = -v;
          const cf cur(u, v);
          const cf avg = 0.5f * (cur + prev_uv[x]);  // PAL-D: hue errors cancel between lines
          prev_uv[x] = cur;
          u = avg.real();
          v = avg.imag();
        }
        r = y + 1.140f * v;
        g = y - 0.395f * u - 0.581f * v;
        bl = y + 2.032f * u;
      }
      const auto q = [](float z) { return static_cast<uint32_t>(std::clamp(z * 255.f, 0.f, 255.f)); };
      row[x] = 0xff000000u | q(bl) << 16 | q(g) << 8 | q(r);  // bytes R, G, B, X
    }
  }

  float ring_mean(double from, double to) {
    const int64_t i0 = static_cast<int64_t>(from), i1 = static_cast<int64_t>(to);
    if (i1 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - i0 > static_cast<int64_t>(ring_mask)) return NAN;
    float s = 0;
    for (int64_t i = i0; i < i1; i++) s += luma_ring[i & ring_mask];
    return sign * s / static_cast<float>(i1 - i0);
  }

  void measure_levels(double ls) {
    const double us = 1e-6 * fs;
    const float t = ring_mean(ls + 1.2 * us, ls + 3.6 * us);  // hsync is 4.7 us
    const float b = ring_mean(ls + 5.8 * us, ls + 8.8 * us);  // back porch (burst averages out)
    if (std::isnan(t) || std::isnan(b) || b - t < 0.05f) return;
    const float a = gated_lines < 64 ? 0.1f : 1.f / 64;
    tip += a * (t - tip);
    blank += a * (b - blank);
    thr = 0.5f * (tip + blank);
    gated_lines++;
  }

  void emit(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    const size_t rowb = static_cast<size_t>(o.width) * 4;
    for (int r = 0; r < std_->active; r++) {
      std::memcpy(frame.data() + (2 * r) * rowb, a.data() + r * rowb, rowb);
      std::memcpy(frame.data() + (2 * r + 1) * rowb, b.data() + r * rowb, rowb);
    }
    frames_out++;
    if (sink) sink(Frame{frame.data(), o.width, 2 * std_->active, std_});
  }

  void close_line(double next_start) {
    const bool vs = line_low > 0.5 * T;
    if (vs && !prev_line_vsync && line_has_longrun) {
      // field start: phase of the broad-pulse start inside this line
      double ph = (static_cast<double>(line_longrun_start) - line_start) / T;
      ph -= std::floor(ph + 0.25);  // -> [-0.25, 0.75)
      const bool odd = std::fabs(ph) < 0.25;
      if (cur_field == 0) {
        haveA = true;
        if (o.fields) emit(fieldA, fieldA);
      }
      if (cur_field == 1) {
        haveB = true;
        if (o.fields) emit(fieldB, fieldB);
      }
      if (!o.fields && haveA && haveB) {
        emit(fieldA, fieldB);
        haveA = haveB = false;
      }
      cur_field = odd ? 0 : 1;
      field_line0 = static_cast<int64_t>(line_idx);
      fields_seen++;
      if (odd) haveB = false;  // a frame is A then B
    }
    prev_line_vsync = vs;
    line_idx++;
    line_low = 0;
    line_has_longrun = false;
    line_start = next_start;
  }

  void on_hsync(double edge) {
    if (line_start < 0) {
      line_start = edge;
      return;
    }
    const double pred = line_start + T;
    if (std::fabs(edge - pred) <= 0.08 * T) {
      const double ns = 0.6 * edge + 0.4 * pred;
      T = 0.995 * T + 0.005 * (ns - line_start);
      miss_run = 0;
      stray_run = 0;
      count_line(true);
      render_line(line_start, true);
      close_line(ns);
      check_lost();
      return;
    }
    // an hsync-width pulse off the grid: noise, an equalising pulse, or the picture has jumped
    if (last_stray >= 0 && std::fabs(edge - last_stray - T) < 0.03 * T) stray_run++;
    else stray_run = 1;
    last_stray = edge;
    if (stray_run >= 6 && miss_run >= 4) {
      rephases++;
      comb_epoch++;
      KP_LOGD("video: re-phased the line grid by %.1f us", (edge - line_start) / fs * 1e6);
      line_start = edge;
      line_low = 0;
      line_has_longrun = false;
      stray_run = 0;
      miss_run = 0;
    }
  }
};

// ---- front end ----------------------------------------------------------------------------------------
// kepyur: block-wise and branch-free so the compiler vectorises it (the spike's per-sample loop used a whole
// phone core at 40 MS/s). Per chunk of <= kTab samples: DC removal (block mean, smoothed), mix by a
// precomputed rotation table, halfband /2 over a work array with 22 samples of history, discriminator.
FrontEnd::FrontEnd(double fs_in, double shift_hz, FrameSink sink, const DecoderOpts &o)
    : _fs_in(fs_in), _fs_out(fs_in >= 40e6 ? fs_in / 2 : fs_in), _decimate(fs_in >= 40e6) {
  for (int i = 0; i < kHb; i++) {
    const int k = i - kHb / 2;
    const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kHb);
    _h[i] = k == 0 ? 0.5f : static_cast<float>(std::sin(M_PI * k / 2) / (M_PI * k) * w);
  }
  _wr.assign(kHb - 1 + kTab, 0.f);
  _wi.assign(kHb - 1 + kTab, 0.f);
  _dec = new Decoder(_fs_out, std::move(sink), o);
  set_shift(shift_hz);
}

FrontEnd::~FrontEnd() { delete _dec; }

void FrontEnd::set_shift(double s) {
  _shift = s;
  _tr.resize(kTab);
  _ti.resize(kTab);
  for (int i = 0; i < kTab; i++) {
    const double a = -2 * M_PI * s / _fs_in * i;
    _tr[i] = static_cast<float>(std::cos(a));
    _ti[i] = static_cast<float>(std::sin(a));
  }
  const double a = -2 * M_PI * s / _fs_in * kTab;
  _stepN = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
  _rot = cf(1, 0);
}

DecoderStats FrontEnd::stats() const { return _dec->stats(); }

namespace {
// branch-free atan2 (same polynomial as fast_atan2) so the discriminator loop vectorises
inline float atan2_bf(float y, float x) {
  const float ax = std::fabs(x), ay = std::fabs(y);
  const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  const float a = mn / (mx + 1e-20f);
  const float s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  r = ay > ax ? 1.57079637f - r : r;
  r = x < 0 ? 3.14159274f - r : r;
  return y < 0 ? -r : r;
}
}  // namespace

void FrontEnd::demod(const cf *xs, size_t n, std::vector<float> &out) {
  out.clear();
  const float k = static_cast<float>(_fs_out / (2 * M_PI) / 1e6);  // rad/sample -> MHz
  constexpr int H = kHb - 1;
  while (n > 0) {
    const int m = static_cast<int>(std::min<size_t>(n, kTab));
    // DC: block mean, smoothed across blocks (the adapter's DC line)
    float sr = 0, si = 0;
    for (int i = 0; i < m; i++) {
      sr += xs[i].real();
      si += xs[i].imag();
    }
    _dc += (cf(sr / m, si / m) - _dc) * 0.3f;
    const float dr = _dc.real(), di = _dc.imag();
    const float pr = _rot.real(), pi = _rot.imag();
    float *wr = _wr.data() + H, *wi = _wi.data() + H;
    const float *tr = _tr.data(), *ti = _ti.data();
    for (int i = 0; i < m; i++) {
      const float xr = xs[i].real() - dr, xi = xs[i].imag() - di;
      const float cr = pr * tr[i] - pi * ti[i], ci = pr * ti[i] + pi * tr[i];  // rot * step^i
      wr[i] = xr * cr - xi * ci;
      wi[i] = xr * ci + xi * cr;
    }
    _rot *= _stepN;
    if (m < kTab) {  // a short chunk: the next one starts at step^m, not step^kTab
      _rot = cf(pr, pi) * cf(tr[m], ti[m]);
    }
    _rot /= std::abs(_rot);
    // halfband /2 (or pass through), then the discriminator
    const size_t o0 = out.size();
    if (_decimate) {
      const float *r = _wr.data(), *q = _wi.data();
      int j0 = _phase;
      const int nout = m - 1 - j0 >= 0 ? (m - 1 - j0) / 2 + 1 : 0;  // windows that end inside this chunk
      _yr.resize(nout);
      _yi.resize(nout);
      for (int j = 0; j < nout; j++) {
        const int s0 = j0 + 2 * j;
        float a = 0.5f * r[s0 + kHb / 2], b = 0.5f * q[s0 + kHb / 2];
        for (int t = 0; t < kHb; t += 2) {
          a += _h[t] * r[s0 + t];
          b += _h[t] * q[s0 + t];
        }
        _yr[j] = a;
        _yi[j] = b;
      }
      _phase = j0 + 2 * nout - m;  // where the next window starts, relative to the next chunk
      std::memmove(_wr.data(), _wr.data() + m, H * sizeof(float));
      std::memmove(_wi.data(), _wi.data() + m, H * sizeof(float));
    } else {
      _yr.assign(wr, wr + m);
      _yi.assign(wi, wi + m);
    }
    const int ny = static_cast<int>(_yr.size());
    out.resize(o0 + ny);
    float *d = out.data() + o0;
    float lr = _prev.real(), li = _prev.imag();
    if (ny > 0) {
      d[0] = atan2_bf(_yi[0] * lr - _yr[0] * li, _yr[0] * lr + _yi[0] * li) * k;
      for (int j = 1; j < ny; j++) {
        const float re = _yr[j] * _yr[j - 1] + _yi[j] * _yi[j - 1];
        const float im = _yi[j] * _yr[j - 1] - _yr[j] * _yi[j - 1];
        d[j] = atan2_bf(im, re) * k;
      }
      _prev = cf(_yr[ny - 1], _yi[ny - 1]);
    }
    xs += m;
    n -= m;
  }
}

void FrontEnd::decode(const float *d, size_t n) {
  for (size_t i = 0; i < n; i++) _dec->push(d[i]);
}

void FrontEnd::feed(const cf *xs, size_t n) {
  demod(xs, n, _dbuf);
  decode(_dbuf.data(), _dbuf.size());
}

}  // namespace kp
