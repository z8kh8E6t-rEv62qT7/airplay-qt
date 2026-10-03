#include "CoreAudioCaptureSession.h"
#include <QDebug>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>
#include <array>
#include <cmath>

namespace audio {
namespace coreaudio {
Layout layout(const CoreAudioApi &api, AudioDeviceID device,
              AudioObjectPropertyScope scope) {
  Layout result;
  result.streams = list<AudioStreamID>(
      api, device, address(kAudioDevicePropertyStreams, scope));
  for (auto stream : result.streams) {
    const auto f = readProperty<AudioStreamBasicDescription>(
        api, stream, address(kAudioStreamPropertyVirtualFormat));
    const bool planar = f.mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    const bool floating = f.mFormatFlags & kAudioFormatFlagIsFloat;
    const bool big = f.mFormatFlags & kAudioFormatFlagIsBigEndian;
    if (f.mFormatID != kAudioFormatLinearPCM || !f.mChannelsPerFrame ||
        f.mChannelsPerFrame > 256 ||
        !(f.mFormatFlags & kAudioFormatFlagIsPacked) ||
        (f.mFormatFlags & kAudioFormatFlagIsAlignedHigh) ||
        (floating ? (f.mBitsPerChannel != 32 && f.mBitsPerChannel != 64)
                  : (!(f.mFormatFlags & kAudioFormatFlagIsSignedInteger) ||
                     (f.mBitsPerChannel != 16 && f.mBitsPerChannel != 24 &&
                      f.mBitsPerChannel != 32))) ||
        f.mFramesPerPacket != 1 ||
        f.mBytesPerFrame !=
            (f.mBitsPerChannel / 8) * (planar ? 1 : f.mChannelsPerFrame) ||
        f.mBytesPerPacket != f.mBytesPerFrame || f.mSampleRate != 44100)
      throw i18n::MessageError(
          i18n::text(i18n::Id::InputMustUseKHzAndASupported));
    const PcmFormat pcm{int(f.mBitsPerChannel / 8), int(f.mBitsPerChannel), big,
                        floating};
    for (UInt32 n = 0; n < (planar ? f.mChannelsPerFrame : 1); ++n)
      result.buffers.push_back(
          {planar ? 1u : f.mChannelsPerFrame, pcm, f.mBytesPerFrame});
    result.channels += int(f.mChannelsPerFrame);
    if (result.channels > 256)
      throw i18n::MessageError(i18n::text(i18n::Id::InputChannelCountExceeds));
  }
  return result;
}
AudioDeviceID findDevice(const CoreAudioApi &api, const QString &uid) {
  for (auto device :
       list<AudioDeviceID>(api, kAudioObjectSystemObject,
                           address(kAudioHardwarePropertyDevices))) {
    try {
      if (stringProperty(api, device, kAudioDevicePropertyDeviceUID) == uid)
        return device;
    } catch (const std::exception &error) {
      qWarning() << "Device unavailable:" << error.what();
    }
  }
  throw i18n::MessageError(i18n::text(i18n::Id::InputDeviceUnavailable));
}
QList<ChannelInfo> channels(const CoreAudioApi &api, AudioDeviceID device,
                            AudioObjectPropertyScope scope) {
  QList<ChannelInfo> channels;
  const auto streams = list<AudioStreamID>(
      api, device, address(kAudioDevicePropertyStreams, scope));
  for (auto stream : streams) {
    const auto f = readProperty<AudioStreamBasicDescription>(
        api, stream, address(kAudioStreamPropertyVirtualFormat));
    if (f.mChannelsPerFrame > 256 ||
        channels.size() + f.mChannelsPerFrame > 256)
      throw i18n::MessageError(i18n::text(i18n::Id::InvalidInputChannelCount));
    for (UInt32 n = 0; n < f.mChannelsPerFrame; ++n)
      channels.append({int(channels.size()),
                       i18n::text(scope == kAudioDevicePropertyScopeOutput
                                      ? i18n::Id::OutputChannel
                                      : i18n::Id::Input)
                           .arg(channels.size() + 1),
                       long(f.mBitsPerChannel)});
  }
  if (channels.isEmpty())
    throw i18n::MessageError(i18n::text(i18n::Id::DeviceHasNoInputChannels));
  return channels;
}
QList<DriverInfo> enumerate(const CoreAudioApi &api, CaptureKind kind) {
  QList<DriverInfo> devices;
  const auto scope = kind == CaptureKind::Loopback
                         ? kAudioDevicePropertyScopeOutput
                         : kAudioDevicePropertyScopeInput;
  for (auto device :
       list<AudioDeviceID>(api, kAudioObjectSystemObject,
                           address(kAudioHardwarePropertyDevices))) {
    try {
      if (list<AudioStreamID>(api, device,
                              address(kAudioDevicePropertyStreams, scope))
              .empty())
        continue;
      const auto uid =
          stringProperty(api, device, kAudioDevicePropertyDeviceUID);
      if (!uid.isEmpty())
        devices.append(
            {(kind == CaptureKind::Loopback ? "loopback:" : "") + uid,
             stringProperty(api, device, kAudioObjectPropertyName), kind});
    } catch (const std::exception &error) {
      qWarning() << "Device unavailable:" << error.what();
    }
  }
  return devices;
}
void validateSelection(int left, int right, double backlog, int count) {
  if (left < 0 || right < 0 || left == right || left >= count ||
      right >= count || !std::isfinite(backlog) || backlog < .01 || backlog > 1)
    throw i18n::MessageError(
        i18n::text(i18n::Id::InvalidInputDeviceChannelsOrBacklogSetting));
}
void controlPanel() {
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(
          "/System/Applications/Utilities/Audio MIDI Setup.app")))
    throw i18n::MessageError(i18n::text(i18n::Id::CannotOpenAudioMIDISetup));
}
} // namespace coreaudio
using namespace coreaudio;
namespace {
constexpr size_t packetFrames = 352;
}
struct CoreAudioCaptureSession::State {
  CoreAudioApi api;
  AudioDeviceID device = kAudioObjectUnknown;
  AudioDeviceIOProcID proc = nullptr;
  Layout format;
  std::shared_ptr<CaptureQueue> queue;
  std::array<std::array<std::byte, packetFrames * 8>, 2> samples{};
  std::array<size_t, 2> bufferIndex{}, channelOffset{};
  std::array<PcmFormat, 2> selected{};
  size_t filled = 0;
  std::atomic<bool> accepting{false}, changed{false};
  std::atomic_flag inCallback = ATOMIC_FLAG_INIT;
  bool running = false;
  std::vector<std::pair<AudioObjectID, AudioObjectPropertyAddress>> listeners;
  QTimer health;
  explicit State(const CoreAudioApi &value) : api(value) {
    health.setInterval(20);
    QObject::connect(&health, &QTimer::timeout, &health, [this] {
      if (changed.load() && queue) {
        accepting = false;
        queue->fault.store(7); // Device or stream properties changed; never
                               // restart automatically.
      }
    });
  }
  static OSStatus changedProperty(AudioObjectID, UInt32,
                                  const AudioObjectPropertyAddress *,
                                  void *context) {
    static_cast<State *>(context)->changed.store(true);
    return noErr;
  }
  void listen(AudioObjectID object, AudioObjectPropertyAddress key) {
    check(api.listen(object, &key, changedProperty, this),
          i18n::text(i18n::Id::ObserveDeviceChanges));
    listeners.emplace_back(object, key);
  }
  static OSStatus process(AudioObjectID, const AudioTimeStamp *,
                          const AudioBufferList *input, const AudioTimeStamp *,
                          AudioBufferList *, const AudioTimeStamp *,
                          void *context) {
    auto &s = *static_cast<State *>(context);
    if (!s.accepting.load(std::memory_order_acquire))
      return noErr;
    if (s.inCallback.test_and_set(std::memory_order_acquire)) {
      s.queue->fault.store(8);
      return noErr;
    }
    struct Release {
      std::atomic_flag &flag;
      ~Release() { flag.clear(std::memory_order_release); }
    } release{s.inCallback};
    if (!s.accepting.load() || s.changed.load() || s.queue->fault.load())
      return noErr;
    auto fault = [&] {
      s.queue->fault.store(9);
      s.accepting = false;
    };
    if (!input || input->mNumberBuffers != s.format.buffers.size()) {
      fault();
      return noErr;
    }
    size_t frames = 0;
    for (size_t n = 0; n < s.format.buffers.size(); ++n) {
      const auto &buffer = input->mBuffers[n];
      const auto &f = s.format.buffers[n];
      if (buffer.mNumberChannels != f.channels ||
          buffer.mDataByteSize % f.stride ||
          (!buffer.mData && buffer.mDataByteSize)) {
        fault();
        return noErr;
      }
      const auto count = buffer.mDataByteSize / f.stride;
      if (n && frames != count) {
        fault();
        return noErr;
      }
      frames = count;
    }
    if (frames > 44100) {
      fault();
      return noErr;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
      for (size_t c = 0; c < 2; ++c) {
        const auto b = s.bufferIndex[c];
        const auto *source =
            static_cast<const std::byte *>(input->mBuffers[b].mData);
        std::memcpy(s.samples[c].data() + s.filled * s.selected[c].bytes,
                    source + frame * s.format.buffers[b].stride +
                        s.channelOffset[c],
                    s.selected[c].bytes);
      }
      if (++s.filled == packetFrames) {
        if (!s.queue->push(s.samples[0].data(), s.samples[1].data())) {
          s.queue->fault.store(1);
          s.accepting = false;
          return noErr;
        }
        s.filled = 0;
      }
    }
    return noErr;
  }
};
CoreAudioCaptureSession::CoreAudioCaptureSession(const CoreAudioApi &api)
    : state_(std::make_unique<State>(api)) {}
CoreAudioCaptureSession::~CoreAudioCaptureSession() {
  const auto error = stop();
  if (!error.isEmpty()) {
    // Never free a context still registered with an OS callback. Stop remains
    // retryable during normal operation; this last-resort quarantine is logged.
    qCritical().noquote() << error;
    state_.release();
  }
}
void CoreAudioCaptureSession::observe(AudioDeviceID device,
                                      AudioObjectPropertyScope scope) {
  auto &s = *state_;
  s.listeners.reserve(s.listeners.size() + 3);
  s.listen(device, address(kAudioDevicePropertyNominalSampleRate));
  s.listen(device, address(kAudioDevicePropertyDeviceIsAlive));
  s.listen(device, address(kAudioDevicePropertyStreams, scope));
  const auto streams = list<AudioStreamID>(
      s.api, device, address(kAudioDevicePropertyStreams, scope));
  s.listeners.reserve(s.listeners.size() + streams.size());
  for (auto stream : streams)
    s.listen(stream, address(kAudioStreamPropertyVirtualFormat));
}
CaptureStream CoreAudioCaptureSession::prepare(AudioDeviceID device, int left,
                                               int right, double backlog) {
  auto &s = *state_;
  if (s.proc || s.queue)
    throw i18n::MessageError(i18n::text(i18n::Id::AudioInputIsNotReady));
  s.device = device;
  try {
    if (device == kAudioObjectUnknown)
      throw i18n::MessageError(i18n::text(i18n::Id::InputDeviceUnavailable));
    observe(device, kAudioDevicePropertyScopeInput);
    const auto rate = readProperty<Float64>(
        s.api, s.device, address(kAudioDevicePropertyNominalSampleRate));
    if (rate != 44100)
      throw i18n::MessageError(
          i18n::text(i18n::Id::InputDeviceSampleRateMustBeKHz));
    s.format = layout(s.api, s.device, kAudioDevicePropertyScopeInput);
    validateSelection(left, right, backlog, s.format.channels);
    for (size_t c = 0; c < 2; ++c) {
      size_t index = c ? right : left;
      for (size_t b = 0; b < s.format.buffers.size(); ++b) {
        const auto &f = s.format.buffers[b];
        if (index < f.channels) {
          s.bufferIndex[c] = b;
          s.channelOffset[c] = index * f.pcm.bytes;
          s.selected[c] = f.pcm;
          break;
        }
        index -= f.channels;
      }
    }
    s.queue = std::make_shared<CaptureQueue>(
        packetFrames, packetFrames * s.selected[0].bytes,
        packetFrames * s.selected[1].bytes,
        size_t(std::ceil(backlog * 44100 / packetFrames)) + 2);
    s.filled = 0;
    check(s.api.create(s.device, State::process, &s, &s.proc),
          i18n::text(i18n::Id::CreateInputCallback));
    if (!s.proc)
      throw i18n::MessageError(
          i18n::text(i18n::Id::CoreAudioDidNotReturnAnInput));
    if (readProperty<Float64>(s.api, s.device,
                              address(kAudioDevicePropertyNominalSampleRate)) !=
            44100 ||
        s.changed.load())
      throw i18n::MessageError(
          i18n::text(i18n::Id::InputDeviceFormatChangedDuringPreparation));
    s.health.start();
    return {s.queue, s.selected[0], s.selected[1], long(packetFrames)};
  } catch (const std::exception &error) {
    const auto failure = i18n::fromException(error);
    const auto cleanup = stop();
    throw i18n::MessageError(failure + (cleanup.isEmpty() ? "" : "\n") +
                             cleanup);
  }
}
void CoreAudioCaptureSession::start() {
  auto &s = *state_;
  if (!s.proc || s.running || s.changed.load())
    throw i18n::MessageError(i18n::text(i18n::Id::InputIsNotReadyOrTheDevice));
  s.accepting = true;
  const auto result = s.api.start(s.device, s.proc);
  if (result != noErr) {
    s.accepting = false;
    check(result, i18n::text(i18n::Id::StartInput));
  }
  s.running = true;
}
i18n::Message CoreAudioCaptureSession::stop() noexcept {
  auto &s = *state_;
  s.accepting = false;
  s.health.stop();
  i18n::Message errors;
  if (s.running) {
    const auto result = s.api.stop(s.device, s.proc);
    if (result == noErr || result == kAudioHardwareBadDeviceError)
      s.running = false;
    else
      errors = errors + (errors.isEmpty() ? "" : "\n") +
               i18n::text(i18n::Id::FailedToStopCoreAudio).arg(result);
  }
  if (s.proc && !s.running) {
    const auto result = s.api.destroy(s.device, s.proc);
    if (result == noErr || result == kAudioHardwareBadDeviceError)
      s.proc = nullptr;
    else
      errors =
          errors + (errors.isEmpty() ? "" : "\n") +
          i18n::text(i18n::Id::FailedToReleaseCoreAudioCallback).arg(result);
  }
  if (s.proc)
    return errors;
  for (auto it = s.listeners.begin(); it != s.listeners.end();) {
    const auto result =
        s.api.unlisten(it->first, &it->second, State::changedProperty, &s);
    if (result == noErr || result == kAudioHardwareBadObjectError)
      it = s.listeners.erase(it);
    else {
      errors =
          errors + (errors.isEmpty() ? "" : "\n") +
          i18n::text(i18n::Id::FailedToRemoveCoreAudioListener).arg(result);
      ++it;
    }
  }
  if (!s.proc && s.listeners.empty()) {
    s.queue.reset();
    s.filled = 0;
    s.changed = false;
    s.device = kAudioObjectUnknown;
  }
  return errors;
}
} // namespace audio
