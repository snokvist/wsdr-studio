// SPDX-License-Identifier: GPL-2.0-only
// WebSocket protocol v1 between wsdr-studiod and the web UI.
//
// Text frames are JSON objects with a "type" (server -> client) or a "cmd" plus a numeric "id"
// (client -> server, answered by {"type":"ack","id"} or {"type":"error","id","msg"}).
// Binary frames carry spectrum rows, little-endian:
//   off  0 u32 magic 'WSRW' (0x57525357)   4 u16 version (1)   6 u8 path (bits 0-3: antenna 0 = A, 1 = B; bit 4: view slot B; bit 6: offset crop; bit 7: gapped)   7 u8 run (low byte
//   of the stream run: a new value = new tuning/gain/rate)
//   off  8 u32 seq    12 u32 bins    16 f64 t (s, server monotonic)    24 f64 f0 (Hz, bin 0)
//   off 32 f64 df (Hz)   40 f32 db_min   44 f32 db_step   48 u8[bins] avg   48+bins u8[bins] max
// A dB value v is sent as clamp(round((v - db_min) / db_step), 0, 255).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsp.h"

namespace studio {

constexpr int kProtocolVersion = 1;
constexpr uint32_t kRowMagic = 0x57525357u; // "WSRW" little-endian
constexpr size_t kRowHeader = 48;
constexpr float kDbMin = -150.f, kDbStep = 0.6f; // u8 0..255 covers -150 .. +3 dBFS

// slot: the view slot written in the frame (-1: r.slot)
std::vector<uint8_t> encode_row(const Row &r, double t, int slot = -1);

struct DecodedRow {
  int path = 0, gen = 0;
  bool gapped = false, offset = false;
  int slot = 0; // view slot (0 = A, 1 = B)
  uint32_t seq = 0, bins = 0;
  double t = 0, f0 = 0, df = 0;
  float db_min = 0, db_step = 0;
  std::vector<float> avg, max;
};
bool decode_row(const uint8_t *p, size_t n, DecodedRow &out); // tests

} // namespace studio
