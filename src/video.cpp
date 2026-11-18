// SPDX-License-Identifier: GPL-2.0-only
#include "video.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "../third_party/kepyur/video.h"
#include "protocol.h"

namespace studio {

using json = nlohmann::json;

// One field of the line-doubled RGBX picture (its even lines) as Y and 4:2:0 U, V: BT.601 full range, in
// 8-bit fixed point (Y's weights sum to 256: no clamp needed).
std::vector<uint8_t> encode_video(const kp::Frame &f, uint32_t seq, double t, float lock, bool colour) {
  const int w = f.width & ~1, h = (f.height / 2) & ~1;
  const size_t ny = static_cast<size_t>(w) * h, nc = ny / 4;
  std::vector<uint8_t> b(kVideoHeader + ny + 2 * nc);
  put_le<uint32_t>(b, 0, kVideoMagic);
  put_le<uint16_t>(b, 4, 1);
  // (6: the view slot and 8: the watch, the Tap writes them)
  put_le<uint8_t>(b, 7, static_cast<uint8_t>((colour ? 1 : 0) | (f.std == &kp::kPal ? 2 : 0)));
  put_le<uint16_t>(b, 12, static_cast<uint16_t>(w));
  put_le<uint16_t>(b, 14, static_cast<uint16_t>(h));
  put_le<double>(b, 16, t);
  put_le<float>(b, 24, lock);
  put_le<uint32_t>(b, 28, seq);
  uint8_t *Y = b.data() + kVideoHeader, *U = Y + ny, *V = U + nc;
  const size_t stride = static_cast<size_t>(f.width) * 4;
  for (int r = 0; r < h; r++) {
    const uint8_t *p = f.data + 2 * r * stride; // field line r
    uint8_t *y = Y + static_cast<size_t>(r) * w;
    for (int x = 0; x < w; x++, p += 4)
      y[x] = static_cast<uint8_t>((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
  }
  if (!colour) {
    std::memset(U, 128, 2 * nc);
    return b;
  }
  const auto c8 = [](int v) { return static_cast<uint8_t>(std::clamp(128 + ((v + 512) >> 10), 0, 255)); };
  for (int r = 0; r < h; r += 2) {
    const uint8_t *l0 = f.data + 2 * r * stride, *l1 = l0 + 2 * stride; // field lines r, r + 1
    for (int x = 0; x < w; x += 2) {
      int su = 0, sv = 0; // 256 x the sum of four pixels' Cb, Cr
      for (const uint8_t *p : {l0 + x * 4, l0 + x * 4 + 4, l1 + x * 4, l1 + x * 4 + 4}) {
        su += -43 * p[0] - 85 * p[1] + 128 * p[2];
        sv += 128 * p[0] - 107 * p[1] - 21 * p[2];
      }
      const size_t c = static_cast<size_t>(r / 2) * (w / 2) + x / 2;
      U[c] = c8(su);
      V[c] = c8(sv);
    }
  }
  return b;
}

bool decode_video(const uint8_t *p, size_t n, DecodedVideo &o) {
  if (n < kVideoHeader || get_le<uint32_t>(p, 0) != kVideoMagic || get_le<uint16_t>(p, 4) != 1)
    return false;
  o.slot = get_le<uint8_t>(p, 6);
  const uint8_t fl = get_le<uint8_t>(p, 7);
  o.colour = fl & 1;
  o.pal = fl & 2;
  o.watch = get_le<uint32_t>(p, 8);
  o.seq = get_le<uint32_t>(p, 28);
  o.width = get_le<uint16_t>(p, 12);
  o.height = get_le<uint16_t>(p, 14);
  o.t = get_le<double>(p, 16);
  o.lock = get_le<float>(p, 24);
  const size_t ny = static_cast<size_t>(o.width) * o.height;
  if (n != kVideoHeader + ny + ny / 2)
    return false;
  o.y = p + kVideoHeader;
  o.u = o.y + ny;
  o.v = o.u + ny / 4;
  return true;
}

namespace {
// kepyur's FrontEnd: DC block, mixer to the carrier, halfband to 20 MS/s (from 40), FM discriminator,
// sync and colour decoder; each field goes to the page as it is finished.
class VideoDecoder : public TapDecoder {
public:
  VideoDecoder(double fs, double shift_hz, const BinarySink &out)
      : _out(out), _t0(now_s()),
        _fe(fs, shift_hz, [this](const kp::Frame &f) { field(f); }, kp::DecoderOpts{}) {}

  void feed(const std::complex<float> *x, size_t n) override { _fe.feed(x, n); }

  json stats() const override {
    const kp::DecoderStats s = _fe.stats();
    const double now = now_s();
    json j = {{"state", s.acquiring ? "searching" : "locked"},
              {"std", s.acquiring ? "" : s.std_name},
              {"colour", !s.acquiring && s.colour},
              {"lock", s.acquiring ? 0.0 : s.lock_recent},
              {"fields", now > _t0 ? (_fields - _fields_at) / (now - _t0) : 0.0}};
    _fields_at = _fields;
    _t0 = now;
    return j;
  }

private:
  void field(const kp::Frame &f) {
    const kp::DecoderStats s = _fe.stats();
    _fields++;
    _out(encode_video(f, _seq++, now_s(), s.lock_recent, s.colour));
  }

  BinarySink _out;
  uint32_t _seq = 0;
  uint64_t _fields = 0;
  mutable uint64_t _fields_at = 0;
  mutable double _t0;
  kp::FrontEnd _fe; // (last: its sink uses the members above)
};
} // namespace

TapKind video_tap() {
  TapKind k;
  k.name = "video";
  k.rate_ok = continuous_rate;
  k.margin_hz = 4e6; // the sync tip (~4 MHz below the mean) must be inside the span
  k.make = [](double fs, double shift, const json &, const BinarySink &out) -> std::unique_ptr<TapDecoder> {
    return std::make_unique<VideoDecoder>(fs, shift, out);
  };
  return k;
}

} // namespace studio
