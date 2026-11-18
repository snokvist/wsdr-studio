// SPDX-License-Identifier: GPL-2.0-only
#include "video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

#include "../third_party/kepyur/video.h"

namespace studio {

using json = nlohmann::json;

namespace {
double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
template <typename T> void put(std::vector<uint8_t> &b, size_t off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }
template <typename T> T get(const uint8_t *p, size_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}
uint8_t u8(float v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }
} // namespace

std::vector<uint8_t> encode_video(const kp::Frame &f, int slot, uint32_t seq, double t, float lock, bool colour) {
  const int w = f.width & ~1, h = (f.height / 2) & ~1; // a field: the even lines of the line-doubled picture
  const size_t ny = static_cast<size_t>(w) * h, nc = ny / 4;
  std::vector<uint8_t> b(kVideoHeader + ny + 2 * nc);
  put<uint32_t>(b, 0, kVideoMagic);
  put<uint16_t>(b, 4, 1);
  put<uint8_t>(b, 6, static_cast<uint8_t>(slot));
  put<uint8_t>(b, 7, static_cast<uint8_t>((colour ? 1 : 0) | (f.std == &kp::kPal ? 2 : 0)));
  put<uint32_t>(b, 8, 0); // (the watch: the Tap writes it)
  put<uint16_t>(b, 12, static_cast<uint16_t>(w));
  put<uint16_t>(b, 14, static_cast<uint16_t>(h));
  put<double>(b, 16, t);
  put<float>(b, 24, lock);
  put<uint32_t>(b, 28, seq);
  uint8_t *Y = b.data() + kVideoHeader, *U = Y + ny, *V = U + nc;
  const size_t stride = static_cast<size_t>(f.width) * 4;
  for (int r = 0; r < h; r += 2) {
    const uint8_t *l0 = f.data + (2 * r) * stride, *l1 = f.data + (2 * r + 2) * stride; // field lines r, r + 1
    for (int x = 0; x < w; x += 2) {
      float su = 0, sv = 0;
      for (const uint8_t *l : {l0, l1})
        for (int k = 0; k < 2; k++) {
          const uint8_t *p = l + (x + k) * 4;
          const float R = p[0], G = p[1], B = p[2];
          Y[static_cast<size_t>(r + (l == l1)) * w + x + k] = u8(0.299f * R + 0.587f * G + 0.114f * B);
          su += -0.168736f * R - 0.331264f * G + 0.5f * B;
          sv += 0.5f * R - 0.418688f * G - 0.081312f * B;
        }
      const size_t c = static_cast<size_t>(r / 2) * (w / 2) + x / 2;
      U[c] = colour ? u8(su / 4 + 128) : 128;
      V[c] = colour ? u8(sv / 4 + 128) : 128;
    }
  }
  return b;
}

bool decode_video(const uint8_t *p, size_t n, DecodedVideo &o) {
  if (n < kVideoHeader || get<uint32_t>(p, 0) != kVideoMagic || get<uint16_t>(p, 4) != 1)
    return false;
  o.slot = get<uint8_t>(p, 6);
  const uint8_t fl = get<uint8_t>(p, 7);
  o.colour = fl & 1;
  o.pal = fl & 2;
  o.watch = get<uint32_t>(p, 8);
  o.seq = get<uint32_t>(p, 28);
  o.width = get<uint16_t>(p, 12);
  o.height = get<uint16_t>(p, 14);
  o.t = get<double>(p, 16);
  o.lock = get<float>(p, 24);
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
  VideoDecoder(double fs, double shift_hz, int slot, const BinarySink &out)
      : _slot(slot), _out(out), _t0(now_s()),
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
    _out(encode_video(f, _slot, _seq++, now_s(), s.lock_recent, s.colour));
  }

  int _slot;
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
  k.rate_ok = [](double fs) { return std::abs(fs - 40e6) < 1e3 || std::abs(fs - 20e6) < 1e3; };
  k.margin_hz = 4e6; // the sync tip (~4 MHz below the mean) must be inside the span
  k.make = [](double fs, double shift, const json &, int slot, const BinarySink &out) -> std::unique_ptr<TapDecoder> {
    return std::make_unique<VideoDecoder>(fs, shift, slot, out);
  };
  return k;
}

} // namespace studio
