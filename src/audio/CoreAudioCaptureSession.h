#pragma once
#include "CoreAudioApi.h"
#include "InputCapture.h"
#include <vector>

namespace audio {
namespace coreaudio {
inline void check(OSStatus status, const i18n::Message &operation) {
  if (status != noErr)
    throw i18n::MessageError(
        i18n::text(i18n::Id::CoreAudioFailed).arg(operation).arg(status));
}
inline AudioObjectPropertyAddress
address(AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  return {selector, scope, kAudioObjectPropertyElementMain};
}
template <class T>
T readProperty(const CoreAudioApi &api, AudioObjectID object,
               AudioObjectPropertyAddress key) {
  T value{};
  UInt32 bytes = sizeof(value);
  check(api.get(object, &key, 0, nullptr, &bytes, &value),
        i18n::text(i18n::Id::ReadProperty));
  if (bytes != sizeof(value))
    throw i18n::MessageError(
        i18n::text(i18n::Id::InvalidCoreAudioPropertyLength));
  return value;
}
template <class T>
std::vector<T> list(const CoreAudioApi &api, AudioObjectID object,
                    AudioObjectPropertyAddress key) {
  UInt32 bytes = 0;
  check(api.size(object, &key, 0, nullptr, &bytes),
        i18n::text(i18n::Id::ReadPropertySize));
  if (bytes > 1024 * 1024 || bytes % sizeof(T))
    throw i18n::MessageError(
        i18n::text(i18n::Id::InvalidCoreAudioPropertyListLength));
  std::vector<T> values(bytes / sizeof(T));
  if (bytes) {
    check(api.get(object, &key, 0, nullptr, &bytes, values.data()),
          i18n::text(i18n::Id::ReadPropertyList));
    if (bytes > values.size() * sizeof(T) || bytes % sizeof(T))
      throw i18n::MessageError(
          i18n::text(i18n::Id::CoreAudioPropertyListChanged));
    values.resize(bytes / sizeof(T));
  }
  return values;
}
inline QString stringProperty(const CoreAudioApi &api, AudioObjectID object,
                              AudioObjectPropertySelector selector) {
  const auto value = readProperty<CFStringRef>(api, object, address(selector));
  if (!value)
    return {};
  const auto text = QString::fromCFString(value);
  CFRelease(value);
  return text;
}

struct BufferFormat {
  UInt32 channels;
  PcmFormat pcm;
  UInt32 stride;
};
struct Layout {
  std::vector<AudioStreamID> streams;
  std::vector<BufferFormat> buffers;
  int channels = 0;
};
Layout layout(const CoreAudioApi &, AudioDeviceID, AudioObjectPropertyScope);
AudioDeviceID findDevice(const CoreAudioApi &, const QString &uid);
QList<ChannelInfo> channels(const CoreAudioApi &, AudioDeviceID,
                            AudioObjectPropertyScope);
QList<DriverInfo> enumerate(const CoreAudioApi &, CaptureKind);
void controlPanel();
void validateSelection(int left, int right, double backlog, int channels);
} // namespace coreaudio

// One HAL input callback, shared by physical inputs and private tap aggregates.
// The owner stops this session before destroying any of its native devices.
class CoreAudioCaptureSession final {
public:
  explicit CoreAudioCaptureSession(const CoreAudioApi &);
  ~CoreAudioCaptureSession();
  void observe(AudioDeviceID, AudioObjectPropertyScope);
  CaptureStream prepare(AudioDeviceID, int left, int right, double backlog);
  void start();
  i18n::Message stop() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace audio
