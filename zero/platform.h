// SPDX-License-Identifier: GPL-2.0-only
// wsdr-zero's display and keyboard: the Linux framebuffer and evdev on the CardputerZero, an SDL
// window on a desktop (when built with SDL2).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "ui.h"

namespace zero {

// A frame buffer's layout (what FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO report).
struct PixelFormat {
  int w = 0, h = 0, stride = 0; // pixels, pixels, bytes per line
  int bpp = 16;                 // 16, 24 or 32
  int r_off = 11, r_len = 5, g_off = 5, g_len = 6, b_off = 0, b_len = 5;
};
// Copy the canvas into such a buffer: 1:1 or the largest whole multiple that fits, centred; turned a
// quarter when the buffer is portrait (rotate: 0 = as is, 90 / 270 = clockwise quarter turns, -1 =
// decide from the buffer's shape, turning clockwise). What the picture does not cover is left alone.
void blit(const Canvas &c, uint8_t *dst, const PixelFormat &f, int rotate = -1);

class Display {
public:
  virtual ~Display() = default;
  virtual void present(const Canvas &c) = 0;
  virtual std::string describe() const = 0;
};

class Input {
public:
  virtual ~Input() = default;
  virtual void poll(std::vector<KeyEvent> &out) = 0; // never blocks
  virtual bool esc_held() const = 0;
  virtual bool closed() const { return false; }      // the window was closed
  virtual std::string describe() const = 0;
};

// device: "" = the ST7789 panel if /proc/fb names one, else /dev/fb0. nullptr and `err` on failure.
std::unique_ptr<Display> open_fbdev(const std::string &device, int rotate, std::string &err);
// device: "" = every keyboard under /dev/input. nullptr and `err` when there is none.
std::unique_ptr<Input> open_evdev(const std::string &device, std::string &err);

// The SDL window is display and keyboard in one (false and `err` when built without SDL2).
bool open_sdl(int scale, std::unique_ptr<Display> &d, std::unique_ptr<Input> &i, std::string &err);

// evdev: a key code (linux/input-event-codes.h) as the UI's key; false for keys the app does not use.
bool map_key(int code, bool shift, KeyEvent &out);

} // namespace zero
