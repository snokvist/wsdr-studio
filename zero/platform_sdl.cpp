// SPDX-License-Identifier: GPL-2.0-only
// The desktop preview: an SDL window showing the 320 x 170 picture enlarged, keys from the window.
#include "platform.h"

#ifdef WSDR_ZERO_SDL
#include <SDL.h>

namespace zero {
namespace {

struct Sdl {
  SDL_Window *win = nullptr;
  SDL_Renderer *ren = nullptr;
  SDL_Texture *tex = nullptr;
  bool esc = false, closed = false;
  ~Sdl() {
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
  }
};

class SdlDisplay : public Display {
public:
  explicit SdlDisplay(std::shared_ptr<Sdl> s) : _s(std::move(s)) {}
  void present(const Canvas &c) override {
    SDL_UpdateTexture(_s->tex, nullptr, c.px.data(), kW * 4);
    SDL_RenderClear(_s->ren);
    SDL_RenderCopy(_s->ren, _s->tex, nullptr, nullptr);
    SDL_RenderPresent(_s->ren);
  }
  std::string describe() const override { return "SDL window"; }

private:
  std::shared_ptr<Sdl> _s;
};

class SdlInput : public Input {
public:
  explicit SdlInput(std::shared_ptr<Sdl> s) : _s(std::move(s)) {}
  void poll(std::vector<KeyEvent> &out) override {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        _s->closed = true;
      if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP)
        continue;
      const SDL_Keycode k = e.key.keysym.sym;
      if (k == SDLK_ESCAPE)
        _s->esc = e.type == SDL_KEYDOWN;
      if (e.type == SDL_KEYUP)
        continue;
      KeyEvent ev;
      ev.shift = e.key.keysym.mod & KMOD_SHIFT;
      ev.repeat = e.key.repeat != 0;
      if (k == SDLK_LEFT) ev.key = Key::Left;
      else if (k == SDLK_RIGHT) ev.key = Key::Right;
      else if (k == SDLK_UP) ev.key = Key::Up;
      else if (k == SDLK_DOWN) ev.key = Key::Down;
      else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) ev.key = Key::Enter;
      else if (k == SDLK_ESCAPE) ev.key = Key::Esc;
      else if (k == SDLK_SPACE) ev.key = Key::Space;
      else if (k == SDLK_BACKSPACE) ev.key = Key::Backspace;
      else if ((k >= SDLK_a && k <= SDLK_z) || (k >= SDLK_0 && k <= SDLK_9) || k == SDLK_PERIOD || k == SDLK_SLASH) {
        ev.key = Key::Char;
        ev.ch = static_cast<char>(k);
      } else
        continue;
      out.push_back(ev);
    }
  }
  bool esc_held() const override { return _s->esc; }
  bool closed() const override { return _s->closed; }
  std::string describe() const override { return "SDL window"; }

private:
  std::shared_ptr<Sdl> _s;
};

} // namespace

bool open_sdl(int scale, std::unique_ptr<Display> &d, std::unique_ptr<Input> &i, std::string &err) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    err = std::string("SDL: ") + SDL_GetError();
    return false;
  }
  auto s = std::make_shared<Sdl>();
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  s->win = SDL_CreateWindow("wsdr-zero", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, kW * scale, kH * scale, 0);
  s->ren = s->win ? SDL_CreateRenderer(s->win, -1, 0) : nullptr;
  s->tex = s->ren ? SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH) : nullptr;
  if (!s->tex) {
    err = std::string("SDL: ") + SDL_GetError();
    return false;
  }
  d = std::make_unique<SdlDisplay>(s);
  i = std::make_unique<SdlInput>(s);
  return true;
}

} // namespace zero
#else
namespace zero {
bool open_sdl(int, std::unique_ptr<Display> &, std::unique_ptr<Input> &, std::string &err) {
  err = "this build has no SDL window (build on a desktop with libsdl2-dev)";
  return false;
}
} // namespace zero
#endif
