// SPDX-License-Identifier: GPL-2.0-only
// The "video" tap (tap.h): analog video, FM composite (an FPV camera's transmitter, a video sender),
// decoded by kepyur's decoder (third_party/kepyur) to pictures, NTSC or PAL, in colour when it can.
// The 8812BU's continuous 20 and 40 MS/s spans only. Params: none.
//
// Binary video frame (little-endian), one field (the decoder line-doubles each field: every other line):
//   off  0 u32 magic 'WSVF' (0x46565357)   4 u16 version (1)   6 u8 slot   7 u8 flags (bit 0: colour,
//   bit 1: PAL)   8 u32 watch (tap.h)   12 u16 width   14 u16 height (this field's lines)   16 f64 t (s,
//   server monotonic)   24 f32 lock (locked fraction of the recent lines)   28 u32 seq
//   off 32 u8 Y[width * height], then U and V, each u8[(width / 2) * (height / 2)] (BT.601, full range,
//   chroma 128 = none)
// Status fields: "state" ("searching" / "locked"), "std" ("NTSC" / "PAL" once locked), "colour", "lock",
// "fields" (a second).
#pragma once

#include <cstdint>
#include <vector>

#include "tap.h"

namespace kp {
struct Frame;
}

namespace studio {

constexpr uint32_t kVideoMagic = 0x46565357u; // "WSVF" little-endian
constexpr size_t kVideoHeader = 32;

TapKind video_tap();

// One field of a decoder's picture (line-doubled, RGBX), as a binary video frame.
std::vector<uint8_t> encode_video(const kp::Frame &f, int slot, uint32_t seq, double t, float lock, bool colour);

struct DecodedVideo {
  int slot = 0, width = 0, height = 0;
  bool colour = false, pal = false;
  uint32_t seq = 0, watch = 0;
  double t = 0;
  float lock = 0;
  const uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
};
bool decode_video(const uint8_t *p, size_t n, DecodedVideo &out); // tests

} // namespace studio
