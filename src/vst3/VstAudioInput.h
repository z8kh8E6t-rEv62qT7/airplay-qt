#pragma once
#include "audio/CaptureStream.h"
#include <QString>
#include <array>
#include <atomic>

namespace vst3 {
enum class InputFault : int {
  None,
  Inactive,
  SampleRate,
  Bypass,
  NonRealtime,
  Overflow,
  InvalidBlock,
  NonFinite,
  StateLoad
};
// Host callbacks own staging; the UI owns allocation and retirement.
class VstAudioInput {
public:
  VstAudioInput() = default;
  ~VstAudioInput();
  void configure(double rate, int maxBlock, bool doublePrecision) noexcept;
  void setActive(bool) noexcept;
  void setProcessing(bool) noexcept;
  void setBypass(bool) noexcept;
  void setRealtime(bool) noexcept;
  QString unavailable() const;
  audio::CaptureStream prepare(double backlog);
  bool start() noexcept;
  void stop() noexcept;
  void stateLoad() noexcept { fail(InputFault::StateLoad); }
  InputFault fault() const noexcept { return fault_.load(); }
  bool bypass() const noexcept { return bypass_.load(); }
  void process(float *const *input, float *const *output, int frames,
               uint64_t silence, bool realtime) noexcept;
  void process(double *const *input, double *const *output, int frames,
               uint64_t silence, bool realtime) noexcept;

private:
  struct Run {
    audio::CaptureStream stream;
    alignas(double) std::array<std::byte, 352 * sizeof(double)> left{}, right{};
    size_t filled = 0;
    bool doubles = false;
  };
  template <class Sample>
  void processSamples(Sample *const *, Sample *const *, int, uint64_t,
                      bool) noexcept;
  void fail(InputFault) noexcept;
  std::unique_ptr<Run> owned_;
  std::atomic<Run *> published_{nullptr};
  std::atomic<unsigned> readers_{0};
  std::atomic<InputFault> fault_{InputFault::None};
  std::atomic<double> rate_{0};
  std::atomic<int> maxBlock_{0};
  std::atomic<bool> active_{false}, processing_{false}, bypass_{false},
      realtime_{false}, doubles_{false};
};
static_assert(std::atomic<double>::is_always_lock_free);
} // namespace vst3
