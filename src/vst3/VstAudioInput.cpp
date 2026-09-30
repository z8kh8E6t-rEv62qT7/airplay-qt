#include "VstAudioInput.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace vst3 {
VstAudioInput::~VstAudioInput() {
  stop();
  while (readers_.load() != 0)
    std::this_thread::yield();
}
void VstAudioInput::fail(InputFault value) noexcept {
  auto expected = InputFault::None;
  fault_.compare_exchange_strong(expected, value);
  readers_.fetch_add(1);
  if (auto *run = published_.load())
    run->stream.queue->fault.store(100 + int(fault_.load()));
  published_.store(nullptr);
  readers_.fetch_sub(1);
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
  active_.store(active);
  if (!active)
    fail(InputFault::Inactive);
}
void VstAudioInput::setProcessing(bool processing) noexcept {
  processing_.store(processing);
  if (!processing)
    fail(InputFault::Inactive);
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
QString VstAudioInput::unavailable() const {
  if (rate_.load() != 44100)
    return "AirPlay 需要 44,100 Hz 工程；本地音频仍原样透传。";
  if (!active_.load() || !processing_.load())
    return "宿主尚未启用实时音频处理。";
  if (!realtime_.load())
    return "预处理／离线导出期间不能发送 AirPlay。";
  if (bypass_.load())
    return "插件已旁路；取消旁路后请手动开始。";
  if (maxBlock_.load() <= 0)
    return "宿主块长度无效。";
  return {};
}
audio::CaptureStream VstAudioInput::prepare(double backlog) {
  stop();
  // A new queue is never published until the prior callback has relinquished
  // its Run. The network retains its own shared ownership of the old queue.
  if (readers_.load() != 0)
    throw std::runtime_error("音频回调交接中，请稍后重试。");
  if (const auto reason = unavailable(); !reason.isEmpty())
    throw std::runtime_error(reason.toStdString());
  if (!std::isfinite(backlog) || backlog < .01 || backlog > 1)
    throw std::runtime_error("无效的采集积压上限");
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
  fault_.store(InputFault::None);
  return owned_->stream;
}
bool VstAudioInput::start() noexcept {
  if (!owned_ || fault_.load() != InputFault::None || rate_.load() != 44100 ||
      !active_.load() || !processing_.load() || bypass_.load() ||
      !realtime_.load())
    return false;
  published_.store(owned_.get());
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
  Run *run = published_.load();
  if (run && fault_.load() == InputFault::None) {
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
