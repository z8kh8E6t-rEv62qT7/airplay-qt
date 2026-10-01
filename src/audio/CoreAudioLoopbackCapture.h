#pragma once
#include "CoreAudioCapture.h"
namespace audio {
class CoreAudioLoopbackCapture final : public InputCapture {
public:
  explicit CoreAudioLoopbackCapture(
      const CoreAudioApi & = defaultCoreAudioApi());
  ~CoreAudioLoopbackCapture() override;
  QList<ChannelInfo> open(const QString &, void *) override;
  void controlPanel() override;
  CaptureStream prepare(int, int, double) override;
  void start() override;
  i18n::Message stop() noexcept override;
  i18n::Message close() noexcept override;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace audio
