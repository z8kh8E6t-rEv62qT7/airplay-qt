#include "VstAudioInput.h"
#include "app/Message.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace vst3 {
VstAudioInput::~VstAudioInput() {
  stop();
  monitored_.store(nullptr);
  while (readers_.load() != 0)
    std::this_thread::yield();
}
void VstAudioInput::fail(InputFault value) noexcept {
  changes_.fetch_add(1);
  auto expected = InputFault::None;
  fault_.compare_exchange_strong(expected, value);
  readers_.fetch_add(1);
  if (auto *run = monitored_.load())
    run->stream.queue->fault.store(100 + int(fault_.load()));
  published_.store(nullptr);
  readers_.fetch_sub(1);
}
void VstAudioInput::interrupt() noexcept {
  changes_.fetch_add(1);
  readers_.fetch_add(1);
  if (auto *run = monitored_.load()) {
    int64_t zero = 0;
    run->interruptedAt.compare_exchange_strong(zero, monotonicNs());
    run->resumedAt.store(0);
    run->sequence.fetch_add(1);
    run->stream.queue->interrupted.store(true);
  }
  published_.store(nullptr);
  readers_.fetch_sub(1);
}
HostInterruption VstAudioInput::interruption() const noexcept {
  if (!owned_)
    return {};
  const auto sequence = owned_->sequence.load();
  HostInterruption value{owned_->interruptedAt.load(), owned_->resumedAt.load(),
                         sequence};
  if (owned_->sequence.load() != sequence)
    value.resumed = 0;
  return value;
}
void VstAudioInput::configure(double rate, int maxBlock,
                              bool doubles) noexcept {
  if (rate_.load() != rate || doubles_.load() != doubles)
    fail(InputFault::SampleRate);
  rate_.store(rate);
  maxBlock_.store(maxBlock);
  doubles_.store(doubles);
}
void VstAudioInput::setActive(bool active) noexcept {
  if (active_.exchange(active) && !active)
    interrupt();
}
void VstAudioInput::setProcessing(bool processing) noexcept {
  if (processing_.exchange(processing) && !processing)
    interrupt();
}
void VstAudioInput::setBypass(bool bypass) noexcept {
  bypass_.store(bypass);
  if (bypass)
    fail(InputFault::Bypass);
}
void VstAudioInput::setRealtime(bool realtime) noexcept {
  realtime_.store(realtime);
  if (!realtime)
    fail(InputFault::NonRealtime);
}
i18n::Message VstAudioInput::unavailable() const {
  if (rate_.load() != 44100)
    return i18n::text(i18n::Id::AirPlayRequiresAHzProjectLocalAudio);
  if (!active_.load() || !processing_.load())
    return i18n::text(i18n::Id::HostHasNotEnabledRealtimeAudioProcessing);
  if (!realtime_.load())
    return i18n::text(
        i18n::Id::AirPlayCannotStreamDuringPreprocessingOrOffline);
  if (bypass_.load())
    return i18n::text(i18n::Id::PluginIsBypassedDisableBypassAndStart);
  if (maxBlock_.load() <= 0)
    return i18n::text(i18n::Id::InvalidHostBlockLength);
  return {};
}
audio::CaptureStream VstAudioInput::prepare(double backlog, bool resuming) {
  const auto changes = changes_.load();
  if (resuming && fault_.load() != InputFault::None)
    throw i18n::MessageError(
        i18n::text(i18n::Id::HostAudioConditionsChangedStartManually));
  stop();
  monitored_.store(nullptr);
  // A new queue is never published until the prior callback has relinquished
  // its Run. The network retains its own shared ownership of the old queue.
  if (readers_.load() != 0)
    throw i18n::MessageError(
        i18n::text(i18n::Id::AudioCallbackHandoffInProgressTryAgain));
  if (const auto reason = unavailable(); !reason.isEmpty())
    throw i18n::MessageError(reason);
  if (!std::isfinite(backlog) || backlog < .01 || backlog > 1)
    throw i18n::MessageError(i18n::text(i18n::Id::InvalidCaptureBacklogLimit));
  auto run = std::make_unique<Run>();
  run->doubles = doubles_.load();
  const int bytes = run->doubles ? 8 : 4;
  run->stream = {std::make_shared<audio::CaptureQueue>(
                     352, 352 * bytes, 352 * bytes,
                     size_t(std::ceil(backlog * 44100 / 352)) + 2),
                 {bytes, bytes * 8, false, true},
                 {bytes, bytes * 8, false, true},
                 352};
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
  run->stream.rateDiagnostics = true;
#endif
  owned_ = std::move(run);
  if (!resuming)
    fault_.store(InputFault::None);
  monitored_.store(owned_.get());
  if (changes_.load() != changes) {
    monitored_.store(nullptr);
    throw i18n::MessageError(
        i18n::text(i18n::Id::HostAudioConditionsChangedDuringPreparationStart));
  }
  return owned_->stream;
}
bool VstAudioInput::start() noexcept {
  if (!owned_ || owned_->stream.queue->interrupted.load() ||
      fault_.load() != InputFault::None || rate_.load() != 44100 ||
      !active_.load() || !processing_.load() || bypass_.load() ||
      !realtime_.load())
    return false;
  published_.store(owned_.get());
  if (owned_->stream.queue->interrupted.load() ||
      fault_.load() != InputFault::None || !active_.load() ||
      !processing_.load()) {
    published_.store(nullptr);
    return false;
  }
  return true;
}
void VstAudioInput::stop() noexcept { published_.store(nullptr); }
template <class Sample>
void VstAudioInput::processSamples(Sample *const *input, Sample *const *output,
                                   int frames, uint64_t silence,
                                   bool realtime) noexcept {
  setRealtime(realtime);
  if (frames < 0) {
    fail(InputFault::InvalidBlock);
    return;
  }
  if (!frames)
    return;
  // Local output is independent of the AirPlay gate and all its failures.
  for (int c = 0; c < 2; ++c) {
    if (!output || !output[c])
      continue;
    if (!input || !input[c] || (silence & (uint64_t{1} << c)))
      std::memset(output[c], 0, size_t(frames) * sizeof(Sample));
    else if (output[c] != input[c])
      std::memmove(output[c], input[c], size_t(frames) * sizeof(Sample));
  }
  // Sequential consistency makes pointer retirement and this read-side
  // critical section ordered; there is no lock, reference count or allocation.
  readers_.fetch_add(1);
  // Validate resumed callbacks even after the old producer gate has closed.
  // No old partial block is completed or queued during this observation.
  if (auto *monitor = monitored_.load();
      monitor && fault_.load() == InputFault::None &&
      monitor->stream.queue->interrupted.load() && active_.load() &&
      processing_.load()) {
    if (frames > maxBlock_.load() ||
        monitor->doubles != (sizeof(Sample) == 8)) {
      fail(InputFault::InvalidBlock);
    } else {
      for (int i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c)
          if (input && input[c] && !(silence & (uint64_t{1} << c)) &&
              !std::isfinite(input[c][i]))
            fail(InputFault::NonFinite);
      if (fault_.load() == InputFault::None && rate_.load() == 44100 &&
          realtime_.load() && !bypass_.load()) {
        int64_t zero = 0;
        monitor->resumedAt.compare_exchange_strong(zero, monotonicNs());
      }
    }
  }
  Run *run = published_.load();
  if (run && fault_.load() == InputFault::None && active_.load() &&
      processing_.load() && !run->stream.queue->interrupted.load()) {
    if (frames > maxBlock_.load() || run->doubles != (sizeof(Sample) == 8)) {
      fail(InputFault::InvalidBlock);
    } else {
      for (int i = 0; i < frames; ++i) {
        Sample values[2]{};
        for (int c = 0; c < 2; ++c)
          if (input && input[c] && !(silence & (uint64_t{1} << c)))
            values[c] = input[c][i];
        if (!std::isfinite(values[0]) || !std::isfinite(values[1])) {
          fail(InputFault::NonFinite);
          break;
        }
        std::memcpy(run->left.data() + run->filled * sizeof(Sample), &values[0],
                    sizeof(Sample));
        std::memcpy(run->right.data() + run->filled * sizeof(Sample),
                    &values[1], sizeof(Sample));
        if (++run->filled == 352) {
          if (!run->stream.queue->push(run->left.data(), run->right.data())) {
            fail(InputFault::Overflow);
            break;
          }
          run->filled = 0;
        }
      }
    }
  }
  readers_.fetch_sub(1);
}
void VstAudioInput::process(float *const *in, float *const *out, int n,
                            uint64_t silence, bool realtime) noexcept {
  processSamples(in, out, n, silence, realtime);
}
void VstAudioInput::process(double *const *in, double *const *out, int n,
                            uint64_t silence, bool realtime) noexcept {
  processSamples(in, out, n, silence, realtime);
}
} // namespace vst3
