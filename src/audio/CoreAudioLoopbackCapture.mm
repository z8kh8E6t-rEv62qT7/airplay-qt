#include "CoreAudioLoopbackCapture.h"
#include "CoreAudioCaptureSession.h"
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>
#include <QDebug>

namespace audio {
using namespace coreaudio;
const CoreAudioApi &defaultCoreAudioApi() {
  static const CoreAudioApi api;
  return api;
}
OSStatus createCoreAudioTap(const QString &uid, UInt32 stream,
                            AudioObjectID *tap) {
  @autoreleasepool {
    auto *description =
        [[CATapDescription alloc] initExcludingProcesses:@[]
                                            andDeviceUID:uid.toNSString()
                                              withStream:stream];
    description.name = @"AirPlayQt auto loopback";
    description.privateTap = YES;
    description.muteBehavior = CATapMutedWhenTapped;
    const auto result = AudioHardwareCreateProcessTap(description, tap);
    [description release];
    return result;
  }
}
OSStatus destroyCoreAudioTap(AudioObjectID tap) {
  return AudioHardwareDestroyProcessTap(tap);
}
struct CoreAudioLoopbackCapture::State {
  CoreAudioApi api;
  CoreAudioCaptureSession session;
  QString uid;
  AudioDeviceID source = kAudioObjectUnknown, aggregate = kAudioObjectUnknown;
  std::vector<AudioObjectID> taps;
  explicit State(const CoreAudioApi &value) : api(value), session(value) {}
};
CoreAudioLoopbackCapture::CoreAudioLoopbackCapture(const CoreAudioApi &api)
    : state_(std::make_unique<State>(api)) {}
CoreAudioLoopbackCapture::~CoreAudioLoopbackCapture() {
  const auto error = close();
  if (!error.isEmpty()) {
    // Preserve the complete ownership graph if HAL still owns a callback.
    qCritical().noquote() << error.render();
    state_.release();
  }
}
QList<ChannelInfo> CoreAudioLoopbackCapture::open(const QString &id, void *) {
  if (const auto error = close(); !error.isEmpty())
    throw i18n::MessageError(error);
  if (!id.startsWith("loopback:") || id.size() == 9)
    throw i18n::MessageError(i18n::text(i18n::Id::InputDeviceUnavailable));
  auto &s = *state_;
  const auto uid = id.mid(9);
  const auto device = findDevice(s.api, uid);
  auto result = channels(s.api, device, kAudioDevicePropertyScopeOutput);
  s.uid = uid;
  s.source = device;
  return result;
}
void CoreAudioLoopbackCapture::controlPanel() { coreaudio::controlPanel(); }
CaptureStream CoreAudioLoopbackCapture::prepare(int left, int right,
                                                int packetSamples,
                                                int backlogSamples) {
  if (const auto error = stop(); !error.isEmpty())
    throw i18n::MessageError(error);
  auto &s = *state_;
  try {
    if (s.source == kAudioObjectUnknown || findDevice(s.api, s.uid) != s.source)
      throw i18n::MessageError(i18n::text(i18n::Id::InputDeviceUnavailable));
    // Observe the source before taking the format snapshot or creating taps.
    s.session.observe(s.source, kAudioDevicePropertyScopeOutput);
    if (readProperty<Float64>(s.api, s.source,
                              address(kAudioDevicePropertyNominalSampleRate)) !=
        44100)
      throw i18n::MessageError(i18n::text(i18n::Id::LoopbackRequires44100));
    const auto sourceLayout =
        layout(s.api, s.source, kAudioDevicePropertyScopeOutput);
    validateSelection(left, right, packetSamples, backlogSamples,
                      sourceLayout.channels);
    s.taps.reserve(sourceLayout.streams.size());
    @autoreleasepool {
      auto *tapList = [NSMutableArray array];
      for (size_t index = 0; index < sourceLayout.streams.size(); ++index) {
        AudioObjectID tap = kAudioObjectUnknown;
        const auto status = s.api.createTap(s.uid, UInt32(index), &tap);
        if (tap != kAudioObjectUnknown)
          s.taps.push_back(tap);
        check(status, i18n::text(i18n::Id::CreateLoopbackTap));
        if (tap == kAudioObjectUnknown)
          throw i18n::MessageError(i18n::text(i18n::Id::InvalidLoopbackTap));
        const auto tapUID = stringProperty(s.api, tap, kAudioTapPropertyUID);
        if (tapUID.isEmpty())
          throw i18n::MessageError(i18n::text(i18n::Id::InvalidLoopbackTap));
        [tapList addObject:@{@kAudioSubTapUIDKey : tapUID.toNSString()}];
      }
      // A tap-only aggregate has no physical input channels to leak into
      // capture.
      NSDictionary *description = @{
        @kAudioAggregateDeviceNameKey : @"AirPlayQt auto loopback",
        @kAudioAggregateDeviceUIDKey : [[NSUUID UUID] UUIDString],
        @kAudioAggregateDeviceIsPrivateKey : @YES,
        @kAudioAggregateDeviceTapAutoStartKey : @NO,
        @kAudioAggregateDeviceTapListKey : tapList
      };
      check(s.api.createAggregate((CFDictionaryRef)description, &s.aggregate),
            i18n::text(i18n::Id::CreateLoopbackAggregate));
    }
    if (s.aggregate == kAudioObjectUnknown)
      throw i18n::MessageError(i18n::text(i18n::Id::InvalidLoopbackAggregate));
    // Never silently remap saved channel indices if HAL exposes another layout.
    const auto captured =
        layout(s.api, s.aggregate, kAudioDevicePropertyScopeInput);
    if (captured.channels != sourceLayout.channels ||
        captured.streams.size() != sourceLayout.streams.size())
      throw i18n::MessageError(i18n::text(i18n::Id::LoopbackLayoutChanged));
    for (size_t index = 0; index < captured.streams.size(); ++index) {
      const auto source = readProperty<AudioStreamBasicDescription>(
          s.api, sourceLayout.streams[index],
          address(kAudioStreamPropertyVirtualFormat));
      const auto target = readProperty<AudioStreamBasicDescription>(
          s.api, captured.streams[index],
          address(kAudioStreamPropertyVirtualFormat));
      if (source.mChannelsPerFrame != target.mChannelsPerFrame)
        throw i18n::MessageError(i18n::text(i18n::Id::LoopbackLayoutChanged));
    }
    return s.session.prepare(s.aggregate, left, right, packetSamples,
                             backlogSamples);
  } catch (const std::exception &error) {
    const auto failure = i18n::fromException(error);
    const auto cleanup = stop();
    throw i18n::MessageError(failure + (cleanup.isEmpty() ? "" : "\n") +
                             cleanup);
  }
}
void CoreAudioLoopbackCapture::start() {
  try {
    state_->session.start();
  } catch (const std::exception &error) {
    const auto failure = i18n::fromException(error);
    const auto cleanup = stop();
    throw i18n::MessageError(failure + (cleanup.isEmpty() ? "" : "\n") +
                             cleanup);
  }
}
i18n::Message CoreAudioLoopbackCapture::stop() noexcept {
  auto &s = *state_;
  auto errors = s.session.stop();
  if (!errors.isEmpty())
    return errors;
  if (s.aggregate != kAudioObjectUnknown) {
    const auto status = s.api.destroyAggregate(s.aggregate);
    if (status != noErr && status != kAudioHardwareBadDeviceError &&
        status != kAudioHardwareBadObjectError)
      return i18n::text(i18n::Id::DestroyLoopbackAggregateFailed).arg(status);
    s.aggregate = kAudioObjectUnknown;
  }
  for (auto it = s.taps.begin(); it != s.taps.end();) {
    const auto status = s.api.destroyTap(*it);
    if (status == noErr || status == kAudioHardwareBadObjectError)
      it = s.taps.erase(it);
    else {
      errors = errors + (errors.isEmpty() ? "" : "\n") +
               i18n::text(i18n::Id::DestroyLoopbackTapFailed).arg(status);
      ++it;
    }
  }
  return errors;
}
i18n::Message CoreAudioLoopbackCapture::close() noexcept {
  const auto error = stop();
  if (error.isEmpty()) {
    state_->source = kAudioObjectUnknown;
    state_->uid.clear();
  }
  return error;
}
} // namespace audio
