// SPDX-License-Identifier: GPL-2.0-only
// wsdr-zero: the 320 x 170 picture the app draws (0xRRGGBB per pixel), with a 5 x 7 font. The display
// backends (platform.h) copy it to a framebuffer, a window or a file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zero {

constexpr int kW = 320, kH = 170; // the CardputerZero's panel, landscape
constexpr int kCharW = 6, kCharH = 8; // text cell (5 x 7 glyph and a gap)

class Canvas {
public:
  Canvas() : px(kW * kH, 0) {}
  void clear(uint32_t c) { px.assign(px.size(), c); }
  void put(int x, int y, uint32_t c) {
    if (x >= 0 && x < kW && y >= 0 && y < kH)
      px[y * kW + x] = c;
  }
  void fill(int x, int y, int w, int h, uint32_t c);
  void hline(int x, int y, int w, uint32_t c) { fill(x, y, w, 1, c); }
  void vline(int x, int y, int h, uint32_t c) { fill(x, y, 1, h, c); }
  // Text at (x, y) = the cell's top left; returns the x after the last character.
  int text(int x, int y, const std::string &s, uint32_t c);
  int text_right(int x_right, int y, const std::string &s, uint32_t c) { // ends at x_right
    return text(x_right - static_cast<int>(s.size()) * kCharW, y, s, c);
  }
  static int width(const std::string &s) { return static_cast<int>(s.size()) * kCharW; }

  std::vector<uint32_t> px;
};

} // namespace zero
