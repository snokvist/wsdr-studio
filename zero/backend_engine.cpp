// SPDX-License-Identifier: GPL-2.0-only
#include "backend.h"

#include "engine.h"
#include "protocol.h"

namespace zero {
namespace {

class EngineBackend : public Backend {
public:
  EngineBackend(Sink sink, const std::string &record_dir)
      : _sink(std::move(sink)),
        _eng(studio::Engine::Callbacks{[this](std::vector<uint8_t> &&f) { row(f); },
                                       [this](const json &e) { _sink.event(json(e)); }},
             record_dir) {
    _eng.set_usb3_default(false); // the CardputerZero has no USB 3 (an open command can still ask for it)
  }
  void submit(json cmd) override {
    _eng.submit(std::move(cmd), [this](const json &r) { _sink.event(json(r)); });
  }
  void poll() override {
    _eng.poll();
    _sink.event(_eng.status());
  }

private:
  void row(const std::vector<uint8_t> &frame) {
    studio::DecodedRow d;
    if (!studio::decode_row(frame.data(), frame.size(), d))
      return;
    SpecRow r;
    r.path = d.path;
    r.gen = d.gen;
    r.gapped = d.gapped;
    r.f0 = d.f0;
    r.df = d.df;
    r.avg = std::move(d.avg);
    r.max = std::move(d.max);
    _sink.row(std::move(r));
  }
  Sink _sink;
  studio::Engine _eng; // last: its threads call back into the members above
};

} // namespace

std::unique_ptr<Backend> make_engine_backend(Sink sink, const std::string &record_dir) {
  return std::make_unique<EngineBackend>(std::move(sink), record_dir);
}

} // namespace zero
