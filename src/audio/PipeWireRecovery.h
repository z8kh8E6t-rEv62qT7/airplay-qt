#pragma once
#include "PipeWireCatalog.h"
#include <chrono>

namespace audio {
// Source format metadata can arrive after capture has already negotiated its
// own format. Only a change of target or codec requires a new capture stream.
inline bool samePipeWireTarget(const PipeWireSource &a, const PipeWireSource &b) {
  return a.id == b.id && a.serial == b.serial && a.node == b.node && a.codec == b.codec;
}
inline bool pipeWireStateInvalidates(pw_stream_state previous, pw_stream_state next) {
  const auto terminal = [](pw_stream_state state) {
    return state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED;
  };
  return terminal(next) && !terminal(previous);
}
// Control-thread retry gate. Missing/replaced sources must not inherit a
// failed target's cooldown. Time advances even when no source is available.
class PipeWireRetry {
public:
  using Clock = std::chrono::steady_clock;
  void clear() { target_ = {}; deadline_ = {}; }
  void failed(const PipeWireSource &target, Clock::time_point now = Clock::now()) {
    target_ = target;
    deadline_ = now + std::chrono::seconds(2);
  }
  bool waiting(const PipeWireSource &target, Clock::time_point now = Clock::now()) const {
    return samePipeWireTarget(target_, target) && now < deadline_;
  }
private:
  PipeWireSource target_;
  Clock::time_point deadline_{};
};
} // namespace audio
