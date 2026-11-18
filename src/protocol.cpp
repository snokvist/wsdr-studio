// SPDX-License-Identifier: GPL-2.0-only
#include "protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace studio {

double now_s() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

namespace {
uint8_t q(float db) {
  const float v = std::round((db - kDbMin) / kDbStep);
  return static_cast<uint8_t>(std::clamp(v, 0.f, 255.f));
}
} // namespace

std::vector<uint8_t> encode_row(const Row &r, double t, int slot) {
  const uint32_t bins = static_cast<uint32_t>(r.avg.size());
  std::vector<uint8_t> b(kRowHeader + 2 * bins);
  put_le<uint32_t>(b, 0, kRowMagic);
  put_le<uint16_t>(b, 4, kProtocolVersion);
  if (slot < 0)
    slot = r.slot;
  put_le<uint8_t>(b, 6, static_cast<uint8_t>(r.path | (slot ? 0x10 : 0) | (r.gapped ? 0x80 : 0) | (r.offset ? 0x40 : 0)));
  put_le<uint8_t>(b, 7, r.gen);
  put_le<uint32_t>(b, 8, r.seq);
  put_le<uint32_t>(b, 12, bins);
  put_le<double>(b, 16, t);
  put_le<double>(b, 24, r.f0);
  put_le<double>(b, 32, r.df);
  put_le<float>(b, 40, kDbMin);
  put_le<float>(b, 44, kDbStep);
  for (uint32_t i = 0; i < bins; i++) {
    b[kRowHeader + i] = q(r.avg[i]);
    b[kRowHeader + bins + i] = q(r.max[i]);
  }
  return b;
}

bool decode_row(const uint8_t *p, size_t n, DecodedRow &o) {
  if (n < kRowHeader || get_le<uint32_t>(p, 0) != kRowMagic || get_le<uint16_t>(p, 4) != kProtocolVersion)
    return false;
  o.path = p[6] & 0x0f;
  o.slot = (p[6] >> 4) & 1;
  o.gapped = (p[6] & 0x80) != 0;
  o.offset = (p[6] & 0x40) != 0;
  o.gen = p[7];
  o.seq = get_le<uint32_t>(p, 8);
  o.bins = get_le<uint32_t>(p, 12);
  if (n != kRowHeader + 2 * static_cast<size_t>(o.bins))
    return false;
  o.t = get_le<double>(p, 16);
  o.f0 = get_le<double>(p, 24);
  o.df = get_le<double>(p, 32);
  o.db_min = get_le<float>(p, 40);
  o.db_step = get_le<float>(p, 44);
  o.avg.resize(o.bins);
  o.max.resize(o.bins);
  for (uint32_t i = 0; i < o.bins; i++) {
    o.avg[i] = o.db_min + o.db_step * p[kRowHeader + i];
    o.max[i] = o.db_min + o.db_step * p[kRowHeader + o.bins + i];
  }
  return true;
}

} // namespace studio
