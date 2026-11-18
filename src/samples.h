// SPDX-License-Identifier: GPL-2.0-only
// Path A's samples of a bank, unpacked once on the DSP thread and shared, read-only, with every thread
// that decodes them (the Wi-Fi finder, the taps): no copy per consumer. A buffer goes back to its pool
// when the last of them lets go of it (on that thread), so a steady stream reuses its buffers: what it
// allocates a bank is the shared_ptr's control block (a few dozen bytes).
#pragma once

#include <complex>
#include <memory>
#include <mutex>
#include <vector>

namespace studio {

using Samples = std::shared_ptr<const std::vector<std::complex<float>>>;

class SamplePool {
public:
  explicit SamplePool(size_t keep = 32) : _free(std::make_shared<Free>()) { _free->keep = keep; }

  // A buffer of n samples to fill (what it holds is stale), then to hand out as Samples.
  std::shared_ptr<std::vector<std::complex<float>>> get(size_t n) {
    std::unique_ptr<std::vector<std::complex<float>>> v;
    {
      std::lock_guard<std::mutex> l(_free->m);
      if (!_free->v.empty()) {
        v = std::move(_free->v.back());
        _free->v.pop_back();
      }
    }
    if (!v)
      v = std::make_unique<std::vector<std::complex<float>>>();
    v->resize(n);
    // (the deleter holds the free list: a buffer may outlive the pool)
    return {v.release(), [f = _free](std::vector<std::complex<float>> *p) {
              std::unique_ptr<std::vector<std::complex<float>>> back(p);
              std::lock_guard<std::mutex> l(f->m);
              if (f->v.size() < f->keep)
                f->v.push_back(std::move(back));
            }};
  }

private:
  struct Free {
    std::mutex m;
    std::vector<std::unique_ptr<std::vector<std::complex<float>>>> v; // (whole vectors: their storage too)
    size_t keep = 0;
  };
  std::shared_ptr<Free> _free;
};

} // namespace studio
