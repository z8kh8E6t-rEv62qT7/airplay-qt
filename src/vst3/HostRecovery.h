#pragma once
#include "VstAudioInput.h"

namespace vst3 {
// Owner-thread policy, independent of Qt/network scheduling. The deadline is
// measured at the callback, so a delayed UI pump cannot extend the window.
class HostRecovery {
public:
  enum class Phase { Stopped, Starting, Streaming, Waiting, Reconnecting };
  enum class Action { None, Interrupt, Reconnect, Cancel };
  static constexpr int64_t window = 5'000'000'000LL;
  void start() { phase_ = Phase::Starting; }
  void cancel() { phase_ = Phase::Stopped; }
  void streaming() {
    if (phase_ == Phase::Starting || phase_ == Phase::Reconnecting)
      phase_ = Phase::Streaming;
  }
  bool waiting() const { return phase_ == Phase::Waiting; }
  bool active() const { return phase_ != Phase::Stopped; }
  Action update(HostInterruption input, bool ready, bool sessionStopped,
                bool failed, int64_t now) {
    if (!active())
      return Action::None;
    if (failed)
      return abort();
    if (!input.began)
      return Action::None;
    if (phase_ == Phase::Starting || phase_ == Phase::Reconnecting)
      return abort();
    if (phase_ == Phase::Streaming) {
      phase_ = Phase::Waiting;
      deadline_ = input.began + window;
      return Action::Interrupt;
    }
    const bool resumed =
        ready && input.resumed > 0 && input.resumed < deadline_;
    if (!resumed && now >= deadline_)
      return abort();
    if (resumed && sessionStopped) {
      phase_ = Phase::Reconnecting;
      return Action::Reconnect;
    }
    return Action::None;
  }

private:
  Action abort() {
    cancel();
    return Action::Cancel;
  }
  Phase phase_ = Phase::Stopped;
  int64_t deadline_ = 0;
};
} // namespace vst3
