#include "audio/CoreAudioApi.h"
#include "audio/CoreAudioCapture.h"
#include <QtTest>
#include <array>
#include <map>
using namespace audio;
namespace {
struct FakeAudio {
  inline static FakeAudio *current;
  CoreAudioApi api;
  double rate = 44100;
  bool planar = false;
  UInt32 channels = 4;
  int startError = 0, createError = 0, stopError = 0;
  int starts = 0, stops = 0, destroys = 0;
  AudioDeviceIOProc callback = nullptr;
  void *context = nullptr;
  struct Listener {
    AudioObjectID object;
    AudioObjectPropertyAddress key;
    AudioObjectPropertyListenerProc callback;
    void *context;
  };
  std::vector<Listener> listeners;
  FakeAudio() {
    current = this;
    api.size = [](AudioObjectID, const AudioObjectPropertyAddress *, UInt32,
                  const void *, UInt32 *bytes) -> OSStatus {
      *bytes = sizeof(UInt32);
      return noErr;
    };
    api.get = [](AudioObjectID, const AudioObjectPropertyAddress *key, UInt32,
                 const void *, UInt32 *bytes, void *out) -> OSStatus {
      auto &s = *current;
      auto put = [&](auto value) -> OSStatus {
        if (*bytes < sizeof(value))
          return kAudioHardwareBadPropertySizeError;
        std::memcpy(out, &value, sizeof(value));
        *bytes = sizeof(value);
        return noErr;
      };
      switch (key->mSelector) {
      case kAudioHardwarePropertyDevices:
        return put(AudioDeviceID(2));
      case kAudioDevicePropertyStreams:
        return put(AudioStreamID(10));
      case kAudioDevicePropertyDeviceUID:
        return put(CFStringCreateWithCString(nullptr, "test-device",
                                             kCFStringEncodingUTF8));
      case kAudioObjectPropertyName:
        return put(CFStringCreateWithCString(nullptr, "Four channels",
                                             kCFStringEncodingUTF8));
      case kAudioDevicePropertyNominalSampleRate:
        return put(s.rate);
      case kAudioStreamPropertyVirtualFormat: {
        AudioStreamBasicDescription f{};
        f.mSampleRate = s.rate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat |
                         (s.planar ? kAudioFormatFlagIsNonInterleaved : 0);
        f.mBitsPerChannel = 32;
        f.mChannelsPerFrame = s.channels;
        f.mFramesPerPacket = 1;
        f.mBytesPerFrame = f.mBytesPerPacket = 4 * (s.planar ? 1 : s.channels);
        return put(f);
      }
      default:
        return kAudioHardwareUnknownPropertyError;
      }
    };
    api.listen = [](AudioObjectID object, const AudioObjectPropertyAddress *key,
                    AudioObjectPropertyListenerProc fn, void *ctx) -> OSStatus {
      current->listeners.push_back({object, *key, fn, ctx});
      return noErr;
    };
    api.unlisten = [](AudioObjectID object,
                      const AudioObjectPropertyAddress *key,
                      AudioObjectPropertyListenerProc, void *) -> OSStatus {
      auto &v = current->listeners;
      std::erase_if(v, [&](const auto &x) {
        return x.object == object && x.key.mSelector == key->mSelector;
      });
      return noErr;
    };
    api.create = [](AudioObjectID, AudioDeviceIOProc fn, void *ctx,
                    AudioDeviceIOProcID *id) -> OSStatus {
      if (current->createError)
        return current->createError;
      current->callback = fn;
      current->context = ctx;
      *id = fn;
      return noErr;
    };
    api.destroy = [](AudioObjectID, AudioDeviceIOProcID) -> OSStatus {
      ++current->destroys;
      current->callback = nullptr;
      current->context = nullptr;
      return noErr;
    };
    api.start = [](AudioObjectID, AudioDeviceIOProcID) -> OSStatus {
      ++current->starts;
      return current->startError;
    };
    api.stop = [](AudioObjectID, AudioDeviceIOProcID) -> OSStatus {
      ++current->stops;
      return current->stopError;
    };
  }
  void feed(const AudioBufferList *data) {
    AudioTimeStamp stamp{};
    AudioBufferList output{};
    callback(2, &stamp, data, &stamp, &output, &stamp, context);
  }
};
struct Buffers {
  UInt32 mNumberBuffers;
  AudioBuffer mBuffers[4];
  const AudioBufferList *get() const {
    return reinterpret_cast<const AudioBufferList *>(this);
  }
};
} // namespace
class CoreAudioTests : public QObject {
  Q_OBJECT
private slots:
  void enumerateAndRejectRate() {
    FakeAudio f;
    const auto devices = CoreAudioCapture::enumerate(f.api);
    QCOMPARE(devices.size(), 1);
    QCOMPARE(devices[0].id, QString("test-device"));
    CoreAudioCapture capture(f.api);
    QCOMPARE(capture.open("test-device", nullptr).size(), 4);
    f.rate = 48000;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    QCOMPARE(f.rate, 48000.);
    QVERIFY(!f.callback);
    QVERIFY(f.listeners.empty());
    f.rate = 44100;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 4, 352, 8192));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(-1, 1, 352, 8192));
  }
  void channelMappingAndPartialBlocks_data() {
    QTest::addColumn<bool>("planar");
    QTest::addColumn<int>("packetSamples");
    QTest::addColumn<int>("leftChannel");
    QTest::addColumn<int>("rightChannel");
    for (int packetSamples : {1, 64, 352}) {
      QTest::newRow(qPrintable(QString("interleaved-%1").arg(packetSamples)))
          << false << packetSamples << 3 << 1;
      QTest::newRow(qPrintable(QString("noninterleaved-%1").arg(packetSamples)))
          << true << packetSamples << 3 << 1;
      QTest::newRow(
          qPrintable(QString("interleaved-same-first-%1").arg(packetSamples)))
          << false << packetSamples << 0 << 0;
      QTest::newRow(
          qPrintable(QString("interleaved-same-last-%1").arg(packetSamples)))
          << false << packetSamples << 3 << 3;
      QTest::newRow(qPrintable(
          QString("noninterleaved-same-first-%1").arg(packetSamples)))
          << true << packetSamples << 0 << 0;
      QTest::newRow(
          qPrintable(QString("noninterleaved-same-last-%1").arg(packetSamples)))
          << true << packetSamples << 3 << 3;
    }
  }
  void channelMappingAndPartialBlocks() {
    QFETCH(bool, planar);
    QFETCH(int, packetSamples);
    QFETCH(int, leftChannel);
    QFETCH(int, rightChannel);
    FakeAudio f;
    f.planar = planar;
    CoreAudioCapture capture(f.api);
    capture.open("test-device", nullptr);
    auto stream =
        capture.prepare(leftChannel, rightChannel, packetSamples, 8193);
    capture.start();
    std::array<std::array<float, 400>, 4> data{};
    std::array<float, 1600> interleaved{};
    for (int i = 0; i < 400; ++i)
      for (int c = 0; c < 4; ++c)
        interleaved[i * 4 + c] = data[c][i] = float(c * .1 + i * .0001);
    for (const auto part : {std::pair{0, 100}, std::pair{100, 300}}) {
      Buffers b{};
      b.mNumberBuffers = planar ? 4 : 1;
      for (UInt32 c = 0; c < b.mNumberBuffers; ++c)
        b.mBuffers[c] = {
            planar ? 1u : 4u, UInt32(part.second * 4 * (planar ? 1 : 4)),
            planar ? static_cast<void *>(data[c].data() + part.first)
                   : static_cast<void *>(interleaved.data() + part.first * 4)};
      f.feed(b.get());
    }
    QCOMPARE(stream.queue->capturedFrames(),
             uint64_t(400 / packetSamples * packetSamples));
    std::span<const std::byte> left, right;
    size_t offset = 0;
    while (stream.queue->peek(left, right)) {
      QCOMPARE(left.size(), size_t(packetSamples) * sizeof(float));
      QCOMPARE(std::memcmp(left.data(), data[leftChannel].data() + offset,
                           left.size()),
               0);
      QCOMPARE(std::memcmp(right.data(), data[rightChannel].data() + offset,
                           right.size()),
               0);
      offset += packetSamples;
      stream.queue->pop();
    }
    QCOMPARE(offset, size_t(400 / packetSamples * packetSamples));
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(!f.callback);
    QVERIFY(f.listeners.empty());
    auto next = capture.prepare(0, 2, 352, 8192);
    QVERIFY(next.queue != stream.queue);
    QCOMPARE(next.queue->capturedFrames(), uint64_t(0));
    capture.start();
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(capture.stop().isEmpty());
    QCOMPARE(f.stops, 2);
  }
  void invalidBufferAndOverflow() {
    FakeAudio f;
    CoreAudioCapture capture(f.api);
    capture.open("test-device", nullptr);
    auto stream = capture.prepare(0, 1, 352, 512);
    capture.start();
    Buffers invalid{};
    f.feed(invalid.get());
    QVERIFY(stream.queue->fault.load());
    stream = capture.prepare(0, 1, 352, 512);
    capture.start();
    std::array<float, 352 * 4> data{};
    Buffers b{1, {{4, sizeof(data), data.data()}}};
    for (int i = 0; i < 10; ++i)
      f.feed(b.get());
    QCOMPARE(stream.queue->fault.load(), 1);
  }
  void propertyChangeAndCleanup() {
    FakeAudio f;
    CoreAudioCapture capture(f.api);
    capture.open("test-device", nullptr);
    auto stream = capture.prepare(0, 1, 352, 8192);
    capture.start();
    const auto listener = f.listeners.front();
    listener.callback(listener.object, 1, &listener.key, listener.context);
    QTRY_COMPARE(stream.queue->fault.load(), 7);
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(f.listeners.empty());
  }
  void failuresAreRetryable() {
    FakeAudio f;
    CoreAudioCapture capture(f.api);
    capture.open("test-device", nullptr);
    f.createError = -1;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    QVERIFY(f.listeners.empty());
    QVERIFY(!f.callback);
    f.createError = 0;
    capture.prepare(0, 1, 352, 8192);
    f.startError = -1;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, capture.start());
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(!f.callback);
    f.startError = 0;
    capture.prepare(0, 1, 352, 8192);
    capture.start();
    f.stopError = -1;
    QVERIFY(!capture.stop().isEmpty());
    QVERIFY(f.callback);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             capture.prepare(0, 1, 352, 8192));
    f.stopError = 0;
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(!f.callback);
  }
};
QTEST_GUILESS_MAIN(CoreAudioTests)
#include "CoreAudioTests.moc"
