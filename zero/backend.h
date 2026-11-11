// SPDX-License-Identifier: GPL-2.0-only
// What the UI talks to: the real engine (studio::Engine, one adapter) or a simulated one (--demo).
// Commands are the engine's JSON commands; every reply comes back as an event ({"type":"ack"|"error",
// "cmd": ...}) along with the engine's own events. The sink is called from the backend's threads.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "ui.h"

namespace zero {

struct Sink {
  std::function<void(SpecRow &&)> row;
  std::function<void(json &&)> event;
};

class Backend {
public:
  virtual ~Backend() = default;
  virtual void submit(json cmd) = 0;
  virtual void poll() = 0; // twice a second: notices a lost adapter, sends a status event
};

std::unique_ptr<Backend> make_engine_backend(Sink sink, const std::string &record_dir);
std::unique_ptr<Backend> make_demo_backend(Sink sink);

} // namespace zero
