// SPDX-License-Identifier: GPL-2.0-only
// Wi-Fi frames in the raw I/Q: 802.11 OFDM preambles found by their 0.8 us repetition, each frame's
// 20 MHz channel, its legacy header (L-SIG: rate and length, so its airtime) and its format (legacy,
// HT, VHT). Receive only: this reads samples the adapter already captured.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace wifi {

using cf = std::complex<float>;

// The equalised data subcarriers behind a frame (Decoder::keep_points), for a constellation: L-SIG
// (BPSK), the two HT-SIG / VHT-SIG-A symbols, then a legacy frame's first payload symbols. On 802.11's
// own scale (unit mean power per constellation), times kPointScale, as int16.
constexpr float kPointScale = 4096;
constexpr int kPointSymbols = 32; // payload symbols kept at most
struct Points {
  std::vector<std::complex<int16_t>> z;
  uint16_t n_lsig = 0, n_sig2 = 0;   // then the payload's
  uint8_t bits = 0;                  // payload bits per subcarrier: 1, 2, 4, 6 (0: no payload points)
  float evm_lsig_db = 0, evm_db = 0; // decision-directed EVM of L-SIG and of the payload (0: none)
};

struct Frame {
  double t = 0;          // start (s), on the clock of the samples fed in
  double centre_hz = 0;  // the 20 MHz channel it was decoded on (40 MHz: the pair's centre)
  int bw_mhz = 20;       // 20, or 40 when its preamble is duplicated on the paired channel (5 GHz)
  char fmt = 'L';        // 'L' legacy OFDM, 'H' HT (802.11n), 'V' VHT (802.11ac), 'B' 802.11b DSSS/CCK
  int rate_mbps = 0;     // L-SIG rate (HT/VHT frames carry 6 here); 802.11b: 1, 2, 5 (5.5) or 11
  int length = 0;        // L-SIG length (bytes)
  double dur_us = 0;     // airtime from L-SIG (covers the whole PPDU, also HT/VHT)
  float snr_db = 0;      // from the two LTF copies
  double cfo_hz = 0;     // the sender's carrier against this receiver's tuning (both crystals' errors)
  Points pts;            // with Decoder::keep_points
  // 6 Mb/s legacy frames (beacons, ACKs, RTS/CTS, ...) are decoded whole; with a good CRC their MAC
  // header is read (only addresses and, for beacons and probe responses, the network's name and
  // channel: payloads are not kept)
  bool mac = false;      // decoded, CRC good
  int type = -1, subtype = -1; // 0 management, 1 control, 2 data
  bool has_ta = false;   // the sender's address (control frames ACK and CTS carry none)
  std::array<uint8_t, 6> ta{}, bssid{};
  bool beacon = false;   // a beacon or probe response: bssid, ssid, channel
  std::string ssid;      // as sent (UTF-8 checked; empty: hidden network)
  int channel = 0;       // from its DS parameter set (0: none)
};

// A network name as received, made safe to show and to write as JSON (see wifi.cpp).
std::string ssid_text(const uint8_t *p, size_t n);

class Decoder {
public:
  // fs: 20 or 40 MS/s (anything else finds nothing); centre_hz: the samples' centre frequency.
  void reset(double fs, double centre_hz);
  // Also keep each frame's equalised subcarriers (Frame::pts); a legacy frame is then reported once
  // its first payload symbols have arrived (one still arriving at reset() is not reported).
  void keep_points(bool on) { _points = on; }
  // Contiguous samples, in order; t_end: the time (s) just after the last one. Frames found are
  // appended to `out` (a frame is reported once its header has arrived).
  void feed(const cf *x, size_t n, double t_end, std::vector<Frame> &out);
  double fs() const { return _fs; }

  // One 20 MS/s snippet starting 2 us before the detector's plateau: decode its header (tests). f.t is
  // set to the frame's start relative to where it was expected (seconds).
  // payload: also decode a 6 Mb/s legacy frame whole when the snippet holds all of it.
  // points: fill f.pts with what the snippet holds.
  static bool decode20(const cf *x, size_t n, Frame &f, bool payload = false, bool points = false);

private:
  void scan(std::vector<Frame> &out);
  void frame_at(uint64_t pos, std::vector<Frame> &out);
  // to 20 MS/s on the channel `off` Hz from the centre: len samples from absolute index `from`
  std::vector<cf> snippet(uint64_t from, uint64_t len, double off) const;
  void finish(std::vector<Frame> &out); // pending payloads whose samples have arrived
  struct Pending {
    uint64_t pos, end; // where its snippet starts (absolute), the sample after the frame
    double off;        // its channel's offset from the centre
    Frame f;
    bool whole = true; // decode it whole (a 6 Mb/s frame); else only its points
  };
  bool _points = false;
  std::vector<Pending> _pend;
  // 802.11b (2.4 GHz): x[k] * conj(x[k + 1 us]) of a DSSS signal is the same Barker waveform times
  // a phase step of 0/90/180/270 degrees, so (summed over a few samples, as a unit phasor) its 4th
  // power adds up coherently over a 10 us window, which noise and OFDM do not: that marks each
  // 802.11b frame from its start while the span is loud; it is decoded once its samples are in
  struct Job {
    uint64_t start, need, stop; // probe from start (absolute) up to stop; first wait for samples up to need
    uint32_t det;               // the detection it belongs to
    bool hit = false;           // a frame was found in it
    double chan = 0;            // waiting for a frame's samples: its channel (it starts at `start`)
  };
  // probes the job's part for frames with the samples up to `end`, decoding the first one found;
  // false: it needs more samples (j.need; j.start moved to where it found one), call again then
  bool dsss_at(Job &j, uint64_t end, std::vector<Frame> &out);
  int dsss_frame(uint64_t from, uint64_t end, double c, Job &j, std::vector<Frame> &out);
  bool _dsss = false;           // the span is at 2.4 GHz
  bool _din = false;            // inside an 802.11b frame
  uint64_t _dstart = 0;         // where it started
  uint64_t _dseg = 0;           // where its current segment started (a long one is probed in parts)
  uint64_t _bdone = 0;          // the end of the last frame decoded (absolute): later jobs start there
  uint64_t _dloud = 0;          // the last time (absolute sample) the span was loud
  std::vector<std::complex<float>> _z4; // the window's terms (a ring), to slide it
  cf _dc = 0;                   // the samples' DC offset (taken out as they arrive)
  std::vector<Job> _bjobs;      // to probe, in order
  uint32_t _ndet = 0;           // detections so far
  int _bempty = 0, _bpart = 0;  // parts of the current detection probed in vain; parts since

  double _fs = 0, _centre = 0;
  int _lag = 0, _win = 0, _dec = 1;
  std::vector<cf> _buf;      // samples not yet scanned (plus what a frame's header needs)
  uint64_t _abs0 = 0;        // absolute index of _buf[0]
  uint64_t _scan = 0;        // next absolute index to examine
  uint64_t _abs_end = 0;     // absolute index after the last sample fed
  double _t_end = 0;         // its time
  bool _in = false;          // inside a plateau of the detector
  uint64_t _pstart = 0;      // where it started
  uint64_t _quiet_until = 0; // no new detection before this (the frame just found)
  double _noise = 0;         // noise power per sample: a low quantile of _lvl
  std::vector<float> _lvl, _lvl_tmp; // window power every 256 samples, the last ~100 ms (a ring)
  size_t _lvl_at = 0, _lvl_n = 0;
  unsigned _lvl_scans = 0;
  std::vector<float> _lp;    // channel filter (40 MS/s -> 20 MS/s)
  std::vector<double> _cands; // 20 MHz channel centres inside the span
};

} // namespace wifi
