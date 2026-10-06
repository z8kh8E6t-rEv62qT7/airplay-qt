#pragma once
#include <QtGlobal>
#include <cstdint>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace airplay {
// One waiting thread; wake() may be called by its control thread. All resources
// are created before the worker and destroyed only after it has joined.
class RealtimeWait {
public:
  RealtimeWait();
#ifdef Q_OS_WIN
  struct WindowsCalls {
    decltype(&CreateWaitableTimerExW) createTimer = CreateWaitableTimerExW;
    decltype(&CreateEventW) createEvent = CreateEventW;
    decltype(&CloseHandle) close = CloseHandle;
    decltype(&SetWaitableTimer) arm = SetWaitableTimer;
    decltype(&WaitForMultipleObjects) wait = WaitForMultipleObjects;
  };
  explicit RealtimeWait(WindowsCalls);
#endif
  ~RealtimeWait();
  RealtimeWait(const RealtimeWait &) = delete;
  RealtimeWait &operator=(const RealtimeWait &) = delete;
  int64_t now() const noexcept;
  bool until(int64_t absoluteNs) noexcept;
  void wake() noexcept;
  // Read immediately on the thread whose native operation failed.
  static int lastError() noexcept;

private:
#ifdef Q_OS_WIN
  WindowsCalls calls_;
  void *timer_ = nullptr, *wake_ = nullptr;
  int64_t frequency_ = 0, origin_ = 0;
#else
  int queue_ = -1, timer_ = -1, wake_ = -1;
  uint32_t numer_ = 1, denom_ = 1;
#endif
};
} // namespace airplay
