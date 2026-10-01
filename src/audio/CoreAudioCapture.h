#pragma once
#include "InputCapture.h"
#include "app/Message.h"
namespace audio {
struct CoreAudioApi;
const CoreAudioApi &defaultCoreAudioApi();
class CoreAudioCapture final : public InputCapture {
public:
  explicit CoreAudioCapture(const CoreAudioApi & = defaultCoreAudioApi());
  ~CoreAudioCapture() override;
  static QList<DriverInfo>
  enumerate(const CoreAudioApi & = defaultCoreAudioApi());
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
