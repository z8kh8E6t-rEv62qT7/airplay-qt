#pragma once
#include <windows.h>

namespace audio {
// Main-thread RAII owner for one capture's process timer request and policy.
// AsioCapture's single-session guard prevents overlapping policy snapshots.
class CaptureTiming {
public:
  CaptureTiming();
  ~CaptureTiming();
  CaptureTiming(const CaptureTiming &) = delete;
  CaptureTiming &operator=(const CaptureTiming &) = delete;
  void restore();
  void verifyRestored() const;

private:
  void restoreNoThrow() noexcept;
  PROCESS_POWER_THROTTLING_STATE original_{};
  bool policyChanged_ = false, timerActive_ = false;
};
} // namespace audio
