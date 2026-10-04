#include "CoreAudioCapture.h"
#include "CoreAudioCaptureSession.h"
#include "CoreAudioLoopbackCapture.h"

namespace audio {
struct CoreAudioCapture::State {
  CoreAudioApi api;
  CoreAudioCaptureSession session;
  AudioDeviceID device = kAudioObjectUnknown;
  explicit State(const CoreAudioApi &value) : api(value), session(value) {}
};
CoreAudioCapture::CoreAudioCapture(const CoreAudioApi &api)
    : state_(std::make_unique<State>(api)) {}
CoreAudioCapture::~CoreAudioCapture() = default;
QList<DriverInfo> CoreAudioCapture::enumerate(const CoreAudioApi &api) {
  return coreaudio::enumerate(api, CaptureKind::Input);
}
QList<ChannelInfo> CoreAudioCapture::open(const QString &uid, void *) {
  if (const auto error = close(); !error.isEmpty())
    throw i18n::MessageError(error);
  auto &s = *state_;
  const auto device = coreaudio::findDevice(s.api, uid);
  auto result =
      coreaudio::channels(s.api, device, kAudioDevicePropertyScopeInput);
  s.device = device;
  return result;
}
void CoreAudioCapture::controlPanel() { coreaudio::controlPanel(); }
CaptureStream CoreAudioCapture::prepare(int left, int right, int packetSamples,
                                        int backlogSamples) {
  if (const auto error = stop(); !error.isEmpty())
    throw i18n::MessageError(error);
  return state_->session.prepare(state_->device, left, right, packetSamples,
                                 backlogSamples);
}
void CoreAudioCapture::start() { state_->session.start(); }
i18n::Message CoreAudioCapture::stop() noexcept {
  return state_->session.stop();
}
i18n::Message CoreAudioCapture::close() noexcept {
  auto error = stop();
  if (error.isEmpty())
    state_->device = kAudioObjectUnknown;
  return error;
}
QList<DriverInfo> inputDevices() {
  auto devices = CoreAudioCapture::enumerate();
  devices.append(
      coreaudio::enumerate(defaultCoreAudioApi(), CaptureKind::Loopback));
  return devices;
}
std::unique_ptr<InputCapture> createInputCapture(CaptureKind kind) {
  if (kind == CaptureKind::Loopback)
    return std::make_unique<CoreAudioLoopbackCapture>();
  return std::make_unique<CoreAudioCapture>();
}
} // namespace audio
