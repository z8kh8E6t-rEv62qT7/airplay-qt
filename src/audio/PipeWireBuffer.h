#pragma once
#include "CaptureQueue.h"
#include <spa/utils/dll.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace audio {
// Realtime-owned assembler/controller. Control threads only update the queue's
// atomic generation. A new generation drops a partial block and PLL history.
class PipeWireBuffer {
public:
  explicit PipeWireBuffer(CaptureQueue &queue, int left) : queue_(queue), left_(left) { reset(); }
  void reset() noexcept {
    generation_ = queue_.generation.load(std::memory_order_acquire);
    count_ = 0;
    spa_dll_init(&dll_);
    spa_dll_set_bw(&dll_, SPA_DLL_BW_MIN, 352, 44100);
    correction_ = 1.;
  }
  bool append(const float *interleaved, size_t frames) noexcept {
    if (generation_ != queue_.generation.load(std::memory_order_acquire)) reset();
    for (size_t i = 0; i < frames; ++i) {
      const float l = interleaved[2 * i + left_], r = interleaved[2 * i + 1 - left_];
      if (!std::isfinite(l) || !std::isfinite(r)) { invalidate(); return false; }
      leftSamples_[count_] = l; rightSamples_[count_] = r;
      if (++count_ != 352) continue;
      count_ = 0;
      if (!queue_.push(leftSamples_.data(), rightSamples_.data(), nullptr, generation_)) {
        invalidate(); return false;
      }
    }
    return true;
  }
  void invalidate() noexcept {
    queue_.generation.fetch_add(1, std::memory_order_acq_rel);
    reset();
  }
  double rate(double buffered, double target, uint32_t frames, bool active) noexcept {
    if (generation_ != queue_.generation.load(std::memory_order_acquire)) reset();
    if (!active) {
      spa_dll_init(&dll_); correction_ = 1.; return correction_;
    }
    spa_dll_set_bw(&dll_, SPA_DLL_BW_MIN, std::max(1u, frames), 44100);
    const auto error = std::clamp(buffered - target, -target, target);
    correction_ = std::clamp(spa_dll_update(&dll_, error), .999, 1.001);
    return correction_;
  }
private:
  CaptureQueue &queue_;
  int left_;
  uint64_t generation_ = 0;
  size_t count_ = 0;
  std::array<float, 352> leftSamples_{}, rightSamples_{};
  spa_dll dll_{};
  double correction_ = 1.;
};
} // namespace audio
