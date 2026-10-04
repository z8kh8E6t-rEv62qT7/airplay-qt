#include "../airplay/TestReceiver.h"
#include "app/Settings.h"
#include "audio/CoreAudioCaptureSession.h"
#include "audio/CoreAudioLoopbackCapture.h"
#import <Foundation/Foundation.h>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <array>
#include <cmath>
#include <set>
using namespace audio;
namespace {
struct FakeAudio {
  inline static FakeAudio *current;
  CoreAudioApi api;
  double rate = 44100;
  bool planar = false, present = true, changeDuringCreate = false;
  int streamCount = 2, channelCount = 2;
  QString failure;
  int tapCalls = 0, listenCalls = 0;
  AudioDeviceIOProc callback = nullptr;
  void *context = nullptr;
  bool aggregate = false, running = false, contractValid = true;
  std::set<AudioObjectID> taps;
  QStringList events;
  struct Listener {
    AudioObjectID object;
    AudioObjectPropertyAddress key;
    AudioObjectPropertyListenerProc callback;
    void *context;
  };
  std::vector<Listener> listeners;
  std::vector<UInt32> ids(AudioObjectID object,
                          const AudioObjectPropertyAddress &key) {
    if (key.mSelector == kAudioHardwarePropertyDevices)
      return present ? std::vector<UInt32>{2, 3, 4} : std::vector<UInt32>{3, 4};
    if (key.mSelector != kAudioDevicePropertyStreams)
      return {};
    if (object == 2 && key.mScope == kAudioDevicePropertyScopeInput)
      return {10};
    if (object == 3)
      return key.mScope == kAudioDevicePropertyScopeInput
                 ? std::vector<UInt32>{11}
                 : std::vector<UInt32>{};
    if (object == 4)
      return key.mScope == kAudioDevicePropertyScopeOutput
                 ? std::vector<UInt32>{22}
                 : std::vector<UInt32>{};
    std::vector<UInt32> result;
    for (int n = 0; n < streamCount; ++n)
      result.push_back((object == 30 ? 40 : 20) + n);
    return result;
  }
  void changed() {
    const auto copy = listeners;
    for (const auto &listener : copy)
      if (listener.object == 2)
        listener.callback(listener.object, 1, &listener.key, listener.context);
  }
  FakeAudio() {
    current = this;
    api.size = [](AudioObjectID object, const AudioObjectPropertyAddress *key,
                  UInt32, const void *, UInt32 *bytes) -> OSStatus {
      *bytes = UInt32(current->ids(object, *key).size() * sizeof(UInt32));
      return noErr;
    };
    api.get = [](AudioObjectID object, const AudioObjectPropertyAddress *key,
                 UInt32, const void *, UInt32 *bytes, void *out) -> OSStatus {
      auto &s = *current;
      auto copy = [&](const void *data, size_t length) -> OSStatus {
        if (*bytes < length)
          return kAudioHardwareBadPropertySizeError;
        if (length)
          std::memcpy(out, data, length);
        *bytes = UInt32(length);
        return noErr;
      };
      auto put = [&](auto value) { return copy(&value, sizeof(value)); };
      auto string = [&](const QString &text) { return put(text.toCFString()); };
      switch (key->mSelector) {
      case kAudioHardwarePropertyDevices:
      case kAudioDevicePropertyStreams: {
        const auto values = s.ids(object, *key);
        return copy(values.data(), values.size() * sizeof(UInt32));
      }
      case kAudioDevicePropertyDeviceUID:
        return string(QString("device-%1").arg(object));
      case kAudioObjectPropertyName:
        return string("Same device name");
      case kAudioTapPropertyUID:
        return string(QString("tap-%1").arg(object));
      case kAudioDevicePropertyNominalSampleRate:
        return put(s.rate);
      case kAudioStreamPropertyVirtualFormat: {
        AudioStreamBasicDescription f{};
        f.mSampleRate = s.rate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat |
                         (s.planar ? kAudioFormatFlagIsNonInterleaved : 0);
        f.mBitsPerChannel = 32;
        f.mChannelsPerFrame = s.channelCount;
        f.mFramesPerPacket = 1;
        f.mBytesPerFrame = f.mBytesPerPacket =
            4 * (s.planar ? 1 : s.channelCount);
        if (s.failure == "layout" && object == 40)
          ++f.mChannelsPerFrame;
        return put(f);
      }
      default:
        return kAudioHardwareUnknownPropertyError;
      }
    };
    api.listen = [](AudioObjectID object, const AudioObjectPropertyAddress *key,
                    AudioObjectPropertyListenerProc callback,
                    void *context) -> OSStatus {
      auto &s = *current;
      if (++s.listenCalls == 2 && s.failure == "listen")
        return -1;
      s.listeners.push_back({object, *key, callback, context});
      return noErr;
    };
    api.unlisten = [](AudioObjectID object,
                      const AudioObjectPropertyAddress *key,
                      AudioObjectPropertyListenerProc, void *) -> OSStatus {
      auto &s = *current;
      s.events << "unlisten";
      if (s.failure == "unlisten")
        return -1;
      std::erase_if(s.listeners, [&](const auto &v) {
        return v.object == object && v.key.mSelector == key->mSelector &&
               v.key.mScope == key->mScope;
      });
      return noErr;
    };
    api.createTap = [](const QString &uid, UInt32 stream,
                       AudioObjectID *tap) -> OSStatus {
      auto &s = *current;
      s.contractValid &=
          uid == "device-2" && stream == UInt32(s.tapCalls % s.streamCount);
      ++s.tapCalls;
      if (s.failure == "tap" && stream == 1)
        return -1;
      if (s.failure == "null-tap")
        return noErr;
      *tap = 100 + stream;
      s.taps.insert(*tap);
      if (s.changeDuringCreate)
        s.changed();
      return noErr;
    };
    api.destroyTap = [](AudioObjectID tap) -> OSStatus {
      auto &s = *current;
      s.events << "tap";
      s.contractValid &= !s.aggregate && !s.callback && s.listeners.empty();
      if (s.failure == "destroy-tap")
        return -1;
      s.taps.erase(tap);
      return noErr;
    };
    api.createAggregate = [](CFDictionaryRef value,
                             AudioDeviceID *device) -> OSStatus {
      auto &s = *current;
      NSDictionary *description = (NSDictionary *)value;
      s.contractValid &=
          [description[@kAudioAggregateDeviceIsPrivateKey] boolValue];
      s.contractValid &=
          ![description[@kAudioAggregateDeviceTapAutoStartKey] boolValue];
      s.contractValid &=
          description[@kAudioAggregateDeviceSubDeviceListKey] == nil;
      NSArray *taps = description[@kAudioAggregateDeviceTapListKey];
      s.contractValid &= taps.count == NSUInteger(s.streamCount);
      for (NSUInteger n = 0; n < taps.count; ++n)
        s.contractValid &= [taps[n][@kAudioSubTapUIDKey]
            isEqualToString:[NSString stringWithFormat:@"tap-%lu", 100 + n]];
      if (s.failure == "null-aggregate")
        return noErr;
      s.aggregate = true;
      *device = 30;
      // Exercise cleanup even when HAL returns both an error and a handle.
      return s.failure == "aggregate" ? -1 : noErr;
    };
    api.destroyAggregate = [](AudioDeviceID device) -> OSStatus {
      auto &s = *current;
      s.events << "aggregate";
      s.contractValid &= device == 30 && !s.callback && s.listeners.empty();
      if (s.failure == "destroy-aggregate")
        return -1;
      s.aggregate = false;
      return noErr;
    };
    api.create = [](AudioDeviceID device, AudioDeviceIOProc fn, void *ctx,
                    AudioDeviceIOProcID *id) -> OSStatus {
      auto &s = *current;
      s.contractValid &= device == 30;
      if (s.failure == "callback")
        return -1;
      s.callback = fn;
      s.context = ctx;
      *id = fn;
      return noErr;
    };
    api.destroy = [](AudioDeviceID, AudioDeviceIOProcID) -> OSStatus {
      auto &s = *current;
      s.events << "callback";
      s.contractValid &= !s.running;
      if (s.failure == "destroy-callback")
        return -1;
      s.callback = nullptr;
      s.context = nullptr;
      return noErr;
    };
    api.start = [](AudioDeviceID, AudioDeviceIOProcID) -> OSStatus {
      if (current->failure == "start")
        return -1;
      current->running = true;
      return noErr;
    };
    api.stop = [](AudioDeviceID, AudioDeviceIOProcID) -> OSStatus {
      current->events << "stop";
      if (current->failure == "stop")
        return -1;
      current->running = false;
      return noErr;
    };
  }
  bool clean() const {
    return taps.empty() && !aggregate && !callback && listeners.empty();
  }
};
} // namespace
class LoopbackTests : public QObject {
  Q_OBJECT
private slots:
  void liveApplicationCapture() {
    const auto app = qEnvironmentVariable("AIRPLAY_LOOPBACK_LIVE_APP");
    const auto deviceId = qEnvironmentVariable("AIRPLAY_LOOPBACK_LIVE_DEVICE");
    if (app.isEmpty() || deviceId.isEmpty())
      QSKIP("Set AIRPLAY_LOOPBACK_LIVE_APP and AIRPLAY_LOOPBACK_LIVE_DEVICE "
            "for hardware acceptance.");
    const auto &api = defaultCoreAudioApi();
    const auto device = coreaudio::findDevice(api, deviceId.mid(9));
    const auto format =
        coreaudio::layout(api, device, kAudioDevicePropertyScopeOutput);
    for (const auto &buffer : format.buffers)
      QVERIFY2(buffer.pcm.floating && buffer.pcm.bytes == 4 &&
                   !buffer.pcm.bigEndian,
               "The live tone fixture requires Float32 output");
    struct Tone {
      AudioDeviceID device;
      AudioDeviceIOProcID proc = nullptr;
      double phase = 0;
      ~Tone() {
        if (proc) {
          AudioDeviceStop(device, proc);
          AudioDeviceDestroyIOProcID(device, proc);
        }
      }
      static OSStatus render(AudioObjectID, const AudioTimeStamp *,
                             const AudioBufferList *, const AudioTimeStamp *,
                             AudioBufferList *output, const AudioTimeStamp *,
                             void *context) {
        auto &tone = *static_cast<Tone *>(context);
        for (UInt32 b = 0; b < output->mNumberBuffers; ++b) {
          auto &buffer = output->mBuffers[b];
          if (!buffer.mData || !buffer.mNumberChannels)
            continue;
          auto *samples = static_cast<float *>(buffer.mData);
          const auto frames =
              buffer.mDataByteSize / sizeof(float) / buffer.mNumberChannels;
          for (UInt32 frame = 0; frame < frames; ++frame) {
            const float value = float(.02 * std::sin(tone.phase));
            tone.phase = std::fmod(tone.phase + 6.283185307179586 * 440 / 44100,
                                   6.283185307179586);
            for (UInt32 channel = 0; channel < buffer.mNumberChannels;
                 ++channel)
              samples[frame * buffer.mNumberChannels + channel] = value;
          }
        }
        return noErr;
      }
    } tone{device};
    test::Receiver receiver("loopback-live");
    receiver.stereo.clear();
    QProcess child;
    struct StopChild {
      QProcess &child;
      ~StopChild() {
        if (child.state() != QProcess::NotRunning) {
          child.terminate();
          if (!child.waitForFinished(5000)) {
            child.kill();
            child.waitForFinished(5000);
          }
        }
      }
    } stopChild{child};
    QTemporaryDir configurationDirectory;
    QVERIFY(configurationDirectory.isValid());
    const auto configurationPath = configurationDirectory.filePath("config.json");
    app::Settings{}.save(configurationPath);
    child.setProgram(app);
    child.setArguments(
        {"--cli", "--config", configurationPath, "--device", deviceId, "--receiver",
         QString("127.0.0.1:%1").arg(receiver.server.serverPort()), "--seconds",
         "2", "--startup-timeout", "30"});
    QCOMPARE(AudioDeviceCreateIOProcID(device, Tone::render, &tone, &tone.proc),
             noErr);
    QCOMPARE(AudioDeviceStart(device, tone.proc), noErr);
    qInfo("Live tone: local playback before capture");
    QTest::qWait(1500);
    child.start();
    QVERIFY(child.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, 40000);
    qInfo("Live tone: local playback restored after capture");
    QTest::qWait(1500);
    const auto output = child.readAllStandardOutput();
    qInfo().noquote() << output << child.readAllStandardError();
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    QVERIFY(receiver.error.isEmpty());
    QVERIFY(receiver.packets.size() > 100);
    bool measured = false;
    for (const auto &line : output.split('\n')) {
      const auto report = QJsonDocument::fromJson(line).object();
      if (report.value("event") == "finished") {
        measured = report.value("peak_left").toDouble() > .005 &&
                   report.value("peak_right").toDouble() > .005;
      }
    }
    QVERIFY2(measured, "Actual output audio must reach both AirPlay channels");
  }
  void enumerationAndLabels() {
    FakeAudio f;
    const auto inputs = coreaudio::enumerate(f.api, CaptureKind::Input);
    const auto outputs = coreaudio::enumerate(f.api, CaptureKind::Loopback);
    QCOMPARE(inputs.size(), 2);
    QCOMPARE(outputs.size(), 2);
    QCOMPARE(inputs[0].id, "device-2");
    QCOMPARE(outputs[0].id, "loopback:device-2");
    QCOMPARE(outputs[1].id, "loopback:device-4");
    QCOMPARE(outputs[0].displayName().render(),
             "Same device name(auto loopback)");
    QCOMPARE(outputs[0].displayName().render(i18n::Language::Chinese),
             "Same device name(自动环回)");
    QCOMPARE(inputs[0].displayName().render(), "Same device name");
    app::Settings settings;
    for (const auto &device : {inputs[0], outputs[0]}) {
      settings.driverId = device.id;
      QCOMPARE(app::Settings::fromJson(settings.json()).driverId, device.id);
    }
  }
  void rejectsInvalidSelectionsAndRate() {
    FakeAudio f;
    CoreAudioLoopbackCapture capture(f.api);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.open("device-2", nullptr));
    QCOMPARE(capture.open("loopback:device-2", nullptr).size(), 4);
    for (auto pair : {std::pair{-1, 1}, std::pair{0, -1}, std::pair{0, 4}})
      QVERIFY_THROWS_EXCEPTION(
          std::runtime_error,
          capture.prepare(pair.first, pair.second, 352, 8192));
    f.rate = 48000;
    try {
      capture.prepare(0, 1, 352, 8192);
      QFAIL("48 kHz accepted");
    } catch (const i18n::MessageError &error) {
      QVERIFY(error.message().render().contains("Audio MIDI Setup"));
    }
    QCOMPARE(f.rate, 48000.);
    QCOMPARE(f.tapCalls, 0);
    QVERIFY(f.clean());
    f.rate = 44100;
    f.streamCount = f.channelCount = 1;
    QCOMPARE(capture.open("loopback:device-2", nullptr).size(), 1);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    QVERIFY(f.clean());
    f.present = false;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    QVERIFY(f.clean());
  }
  void capturesAcrossStreams_data() {
    QTest::addColumn<bool>("planar");
    QTest::addColumn<int>("leftChannel");
    QTest::addColumn<int>("rightChannel");
    QTest::newRow("interleaved") << false << 3 << 0;
    QTest::newRow("planar") << true << 3 << 0;
    QTest::newRow("interleaved-same") << false << 3 << 3;
    QTest::newRow("planar-same") << true << 0 << 0;
  }
  void capturesAcrossStreams() {
    QFETCH(bool, planar);
    QFETCH(int, leftChannel);
    QFETCH(int, rightChannel);
    FakeAudio f;
    f.planar = planar;
    CoreAudioLoopbackCapture capture(f.api);
    capture.open("loopback:device-2", nullptr);
    auto stream = capture.prepare(leftChannel, rightChannel, 352, 8193);
    capture.start();
    std::array<std::array<float, 352>, 4> channels;
    std::array<std::array<float, 704>, 2> interleaved;
    for (int c = 0; c < 4; ++c)
      for (int n = 0; n < 352; ++n)
        interleaved[c / 2][n * 2 + c % 2] = channels[c][n] =
            c * .1f + n * .0001f;
    struct {
      UInt32 count;
      AudioBuffer buffers[4];
    } data{};
    data.count = planar ? 4 : 2;
    for (UInt32 n = 0; n < data.count; ++n)
      data.buffers[n] = {planar ? 1u : 2u, planar ? 1408u : 2816u,
                         planar ? channels[n].data() : interleaved[n].data()};
    AudioTimeStamp timestamp{};
    AudioBufferList output{};
    f.callback(30, &timestamp, reinterpret_cast<const AudioBufferList *>(&data),
               &timestamp, &output, &timestamp, f.context);
    std::span<const std::byte> left, right;
    QVERIFY(stream.queue->peek(left, right));
    QCOMPARE(std::memcmp(left.data(), channels[leftChannel].data(), left.size()), 0);
    QCOMPARE(std::memcmp(right.data(), channels[rightChannel].data(), right.size()), 0);
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(f.clean());
    QVERIFY(f.contractValid);
    QCOMPARE(f.events.front(), "stop");
    QVERIFY(f.events.indexOf("callback") < f.events.indexOf("unlisten"));
    QVERIFY(f.events.lastIndexOf("unlisten") < f.events.indexOf("aggregate"));
    QVERIFY(f.events.indexOf("aggregate") < f.events.indexOf("tap"));
    auto next = capture.prepare(0, 1, 352, 8192);
    QVERIFY(next.queue != stream.queue);
    capture.start();
    QVERIFY(capture.close().isEmpty());
    QVERIFY(capture.close().isEmpty());
    QVERIFY(f.clean());
  }
  void prepareFailures_data() {
    QTest::addColumn<QString>("failure");
    for (const char *stage : {"listen", "tap", "null-tap", "aggregate",
                              "null-aggregate", "callback", "layout"})
      QTest::newRow(stage) << QString(stage);
  }
  void prepareFailures() {
    QFETCH(QString, failure);
    FakeAudio f;
    CoreAudioLoopbackCapture capture(f.api);
    capture.open("loopback:device-2", nullptr);
    f.failure = failure;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    QVERIFY(f.clean());
    QVERIFY(f.contractValid);
  }
  void cleanupFailures_data() {
    QTest::addColumn<QString>("failure");
    for (const char *stage : {"stop", "destroy-callback", "unlisten",
                              "destroy-aggregate", "destroy-tap"})
      QTest::newRow(stage) << QString(stage);
  }
  void cleanupFailures() {
    QFETCH(QString, failure);
    FakeAudio f;
    CoreAudioLoopbackCapture capture(f.api);
    capture.open("loopback:device-2", nullptr);
    capture.prepare(0, 1, 352, 8192);
    capture.start();
    f.failure = failure;
    QVERIFY(!capture.stop().isEmpty());
    QVERIFY(!f.clean());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.open("loopback:device-4", nullptr));
    f.failure.clear();
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(f.clean());
    QVERIFY(f.contractValid);
  }
  void startFailureAndSourceChanges() {
    FakeAudio f;
    CoreAudioLoopbackCapture capture(f.api);
    capture.open("loopback:device-2", nullptr);
    capture.prepare(0, 1, 352, 8192);
    f.failure = "start";
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, capture.start());
    QVERIFY(f.clean());
    f.failure.clear();
    f.changeDuringCreate = true;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    QVERIFY(f.clean());
    f.changeDuringCreate = false;
    auto stream = capture.prepare(0, 1, 352, 8192);
    capture.start();
    f.changed();
    QTRY_COMPARE(stream.queue->fault.load(), 7);
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(f.clean());
  }
};
QTEST_GUILESS_MAIN(LoopbackTests)
#include "CoreAudioLoopbackTests.moc"
