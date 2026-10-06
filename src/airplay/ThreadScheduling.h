#pragma once
#include <QString>
#include <optional>
#ifdef Q_OS_WIN
#include <windows.h>

#include <avrt.h>
#endif

namespace airplay {
struct KernelRealtime {
  bool realtime = false;
  QString release, source;
  QString description() const;
};
KernelRealtime classifyKernelRealtime(const std::optional<QString> &sysfs,
                                      const QString &release);
KernelRealtime kernelRealtime();
struct SchedulingResult {
  bool ready = true;
  QString description;
};
// Called on the target thread, before the realtime loop starts.
SchedulingResult configureNetworkScheduling();
// Construct and destroy on the audio worker. MMCSS registration is
// thread-bound.
class AudioThreadScheduling {
public:
  AudioThreadScheduling();
#ifdef Q_OS_WIN
  // Native call seam for deterministic failure/cleanup tests; no runtime
  // option.
  struct WindowsCalls {
    decltype(&AvSetMmThreadCharacteristicsW) registerTask =
        AvSetMmThreadCharacteristicsW;
    decltype(&AvSetMmThreadPriority) setPriority = AvSetMmThreadPriority;
    decltype(&AvRevertMmThreadCharacteristics) revert =
        AvRevertMmThreadCharacteristics;
  };
  explicit AudioThreadScheduling(WindowsCalls);
#endif
  ~AudioThreadScheduling();
  AudioThreadScheduling(const AudioThreadScheduling &) = delete;
  AudioThreadScheduling &operator=(const AudioThreadScheduling &) = delete;
  const SchedulingResult &result() const noexcept { return result_; }

private:
  SchedulingResult result_;
#ifdef Q_OS_WIN
  WindowsCalls calls_;
  void *task_ = nullptr;
#endif
};
// Pure result classification also exercises permission/readback failures in CI.
bool realtimeSchedulingAccepted(bool required, int setError, int readError,
                                bool fifo, int priority) noexcept;
} // namespace airplay
