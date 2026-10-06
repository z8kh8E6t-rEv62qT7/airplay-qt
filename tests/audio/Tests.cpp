#include "audio/AsioCapture.h"
#include "audio/CaptureQueue.h"
#include "audio/CaptureTiming.h"
#include "audio/PcmConverter.h"
#include <QtTest>
#include <array>
#include <bit>
#include <thread>

namespace audio {
class AsioCaptureTestAccess {
public:
  static void attach(AsioCapture &capture, IASIO &driver) {
    capture.close();
    capture.driver_ = &driver;
    for (int channel = 0; channel < 32; ++channel)
      capture.channels_.append(
          {channel, QString::number(channel + 1), ASIOSTFloat32LSB});
  }
  static bool ownsTiming(const AsioCapture &capture) {
    return bool(capture.timing_);
  }
};
} // namespace audio
namespace {
PROCESS_POWER_THROTTLING_STATE timerPolicy() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                             &state, sizeof(state)))
    throw std::runtime_error("Cannot query timer policy");
  return state;
}
bool honorsTimerRequest() {
  const auto state = timerPolicy();
  constexpr auto flag = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  return (state.ControlMask & flag) && !(state.StateMask & flag);
}
bool samePolicy(const PROCESS_POWER_THROTTLING_STATE &before) {
  const auto after = timerPolicy();
  return after.ControlMask == before.ControlMask &&
         (after.StateMask & before.ControlMask) ==
             (before.StateMask & before.ControlMask);
}
class FakeAsio final : public IASIO {
public:
  static constexpr long frames = 64;
  ASIOCallbacks callbacks{};
  std::array<std::array<std::array<float, frames>, 2>, 32> data{};
  std::array<int, 2> selected{};
  bool running = false, allocated = false;
  ASIOError rateResult = ASE_OK, createResult = ASE_OK, startResult = ASE_OK;
  bool nullBuffer = false;
  bool timedDuringPrepare = false, timedDuringStop = false,
       timedDuringDispose = false;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {
    if (object)
      *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 0; }
  ASIOBool init(void *) override { return ASIOTrue; }
  void getDriverName(char *name) override { std::strcpy(name, "Fake ASIO"); }
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char *text) override {
    std::strcpy(text, "Fake ASIO error");
  }
  ASIOError start() override {
    running = true;
    return startResult;
  }
  ASIOError stop() override {
    timedDuringStop = honorsTimerRequest();
    running = false;
    return ASE_OK;
  }
  ASIOError getChannels(long *input, long *output) override {
    *input = 32;
    *output = 0;
    return ASE_OK;
  }
  ASIOError getLatencies(long *input, long *output) override {
    *input = frames;
    *output = 0;
    return ASE_OK;
  }
  ASIOError getBufferSize(long *minimum, long *maximum, long *preferred,
                          long *granularity) override {
    *minimum = *maximum = *preferred = frames;
    *granularity = 0;
    return ASE_OK;
  }
  ASIOError canSampleRate(ASIOSampleRate rate) override {
    timedDuringPrepare = honorsTimerRequest();
    return rate == 44100 ? rateResult : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate *rate) override {
    *rate = 44100;
    return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    return canSampleRate(rate);
  }
  ASIOError getClockSources(ASIOClockSource *, long *) override {
    return ASE_NotPresent;
  }
  ASIOError setClockSource(long) override { return ASE_NotPresent; }
  ASIOError getSamplePosition(ASIOSamples *, ASIOTimeStamp *) override {
    return ASE_NotPresent;
  }
  ASIOError getChannelInfo(ASIOChannelInfo *info) override {
    if (!info->isInput || info->channel < 0 || info->channel >= 32)
      return ASE_InvalidParameter;
    info->type = ASIOSTFloat32LSB;
    info->isActive = allocated;
    return ASE_OK;
  }
  ASIOError createBuffers(ASIOBufferInfo *info, long count, long size,
                          ASIOCallbacks *cb) override {
    if (createResult != ASE_OK)
      return createResult;
    if ((count != 1 && count != 2) || size != frames ||
        (count == 2 && info[0].channelNum == info[1].channelNum))
      return ASE_InvalidParameter;
    for (int i = 0; i < count; ++i) {
      if (!info[i].isInput || info[i].channelNum < 0 ||
          info[i].channelNum >= 32)
        return ASE_InvalidParameter;
      selected[i] = int(info[i].channelNum);
      for (int half = 0; half < 2; ++half)
        info[i].buffers[half] = data[selected[i]][half].data();
    }
    if (count == 1) selected[1] = selected[0];
    callbacks = *cb;
    allocated = true;
    if (nullBuffer)
      info[count - 1].buffers[1] = nullptr;
    return ASE_OK;
  }
  ASIOError disposeBuffers() override {
    timedDuringDispose = honorsTimerRequest();
    allocated = false;
    return ASE_OK;
  }
  ASIOError controlPanel() override { return ASE_NotPresent; }
  ASIOError future(long, void *) override { return ASE_NotPresent; }
  ASIOError outputReady() override { return ASE_NotPresent; }
  static float marker(int ordinal, int channel, int sample) {
    return float(ordinal * 1000 + channel * 100 + sample);
  }
  void deliver(int ordinal, bool timed = true, int positionOffset = 0) {
    const long half = ordinal % 2;
    for (int channel : selected)
      for (int i = 0; i < frames; ++i)
        data[channel][half][i] = marker(ordinal, channel, i);
    if (timed) {
      ASIOTime time{};
      time.timeInfo.flags =
          kSamplePositionValid | kSampleRateValid | kSystemTimeValid;
      time.timeInfo.sampleRate = 44100;
      time.timeInfo.samplePosition.lo =
          unsigned(ordinal * frames + positionOffset);
      time.timeInfo.systemTime.lo = unsigned(
          1000000000ULL + uint64_t(ordinal) * frames * 1000000000 / 44100);
      callbacks.bufferSwitchTimeInfo(&time, half, ASIOTrue);
    } else
      callbacks.bufferSwitch(half, ASIOFalse);
    // An ASIO buffer belongs to the driver and may be reused after the
    // callback.
    for (int channel : selected)
      data[channel][half].fill(-999999);
  }
};
} // namespace
class AudioTests : public QObject {
  Q_OBJECT
private slots:
  void asioReblocksCallbacks() {
    for (int packet : {1, 64, 352}) {
      FakeAsio driver;
      audio::AsioCapture capture;
      audio::AsioCaptureTestAccess::attach(capture, driver);
      auto stream = capture.prepare(30, 31, packet, 8193);
      audio::CaptureTrace trace(20);
      capture.setTrace(&trace);
      capture.start();
      size_t consumed = 0;
      for (int callback = 0; callback < 20; ++callback) {
        driver.deliver(callback);
        std::span<const std::byte> left, right;
        while (stream.queue->peek(left, right)) {
          QCOMPARE(left.size(), size_t(packet) * 4);
          for (int i = 0; i < packet; ++i, ++consumed) {
            float l, r;
            std::memcpy(&l, left.data() + i * 4, 4);
            std::memcpy(&r, right.data() + i * 4, 4);
            QCOMPARE(l, FakeAsio::marker(int(consumed / FakeAsio::frames), 30,
                                         int(consumed % FakeAsio::frames)));
            QCOMPARE(r, FakeAsio::marker(int(consumed / FakeAsio::frames), 31,
                                         int(consumed % FakeAsio::frames)));
          }
          stream.queue->pop();
        }
        QCOMPARE(trace.entries[callback].sourceBefore,
                 trace.entries[callback].copied);
        QCOMPARE(trace.entries[callback].sourceAfter,
                 trace.entries[callback].copied);
      }
      QCOMPARE(consumed, size_t(20 * FakeAsio::frames / packet * packet));
      capture.stop();
      auto next = capture.prepare(30, 31, packet, 8192);
      QCOMPARE(next.queue->queuedFrames(), uint64_t(0));
      capture.stop();
    }
  }

  void timerOwnerRestoresOnException() {
    const auto original = timerPolicy();
    try {
      audio::CaptureTiming timing;
      QVERIFY(honorsTimerRequest());
      throw std::runtime_error("Simulate initialization failure");
    } catch (const std::runtime_error &) {
    }
    QVERIFY(samePolicy(original));
    audio::CaptureTiming timing;
    QVERIFY(honorsTimerRequest());
    timing.restore();
    timing.verifyRestored();
    timing.restore();
    timing.verifyRestored();
    QVERIFY(samePolicy(original));
  }
  void asioTimerLifecycle() {
    const auto original = timerPolicy();
    FakeAsio driver;
    {
      audio::AsioCapture capture;
      audio::AsioCaptureTestAccess::attach(capture, driver);
      QVERIFY(!audio::AsioCaptureTestAccess::ownsTiming(capture));
      for (int cycle = 0; cycle < 10; ++cycle) {
        capture.prepare(30, 31, 64, 8192);
        QVERIFY(driver.timedDuringPrepare);
        QVERIFY(audio::AsioCaptureTestAccess::ownsTiming(capture));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 capture.prepare(30, 31, 64, 8192));
        QVERIFY(honorsTimerRequest());
        if (cycle % 2)
          capture.start();
        QVERIFY(capture.stop().isEmpty());
        QVERIFY(driver.timedDuringDispose);
        if (cycle % 2)
          QVERIFY(driver.timedDuringStop);
        QVERIFY(!audio::AsioCaptureTestAccess::ownsTiming(capture));
        QVERIFY(samePolicy(original));
        QVERIFY(capture.stop().isEmpty());
        QVERIFY(samePolicy(original));
      }
      capture.prepare(30, 31, 64, 8192);
      capture.start();
      // Destruction must stop the driver before restoring process policy.
    }
    QVERIFY(!driver.running);
    QVERIFY(!driver.allocated);
    QVERIFY(driver.timedDuringStop);
    QVERIFY(driver.timedDuringDispose);
    QVERIFY(samePolicy(original));
  }
  void asioTimerPartialFailure_data() {
    QTest::addColumn<int>("stage");
    QTest::newRow("sample-rate") << 0;
    QTest::newRow("create-buffers") << 1;
    QTest::newRow("null-buffer-after-create") << 2;
    QTest::newRow("driver-start") << 3;
  }
  void asioTimerPartialFailure() {
    QFETCH(int, stage);
    const auto original = timerPolicy();
    FakeAsio driver;
    audio::AsioCapture capture;
    audio::AsioCaptureTestAccess::attach(capture, driver);
    if (stage == 0)
      driver.rateResult = ASE_NoClock;
    if (stage == 1)
      driver.createResult = ASE_NoMemory;
    if (stage == 2)
      driver.nullBuffer = true;
    if (stage == 3) {
      driver.startResult = ASE_HWMalfunction;
      capture.prepare(30, 31, 64, 8192);
      QVERIFY_THROWS_EXCEPTION(std::runtime_error, capture.start());
    } else {
      QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                               capture.prepare(30, 31, 64, 8192));
    }
    QVERIFY(driver.timedDuringPrepare);
    QVERIFY(!driver.allocated);
    QVERIFY(!driver.running);
    QVERIFY(!audio::AsioCaptureTestAccess::ownsTiming(capture));
    QVERIFY(samePolicy(original));
    driver.rateResult = driver.createResult = driver.startResult = ASE_OK;
    driver.nullBuffer = false;
    capture.prepare(30, 31, 64, 8192);
    capture.start();
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(samePolicy(original));
  }
  void asioTimerRejectsSecondOwner() {
    const auto original = timerPolicy();
    FakeAsio firstDriver, secondDriver;
    audio::AsioCapture first, second;
    audio::AsioCaptureTestAccess::attach(first, firstDriver);
    audio::AsioCaptureTestAccess::attach(second, secondDriver);
    first.prepare(30, 31, 64, 8192);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             second.prepare(30, 31, 64, 8192));
    QVERIFY(!audio::AsioCaptureTestAccess::ownsTiming(second));
    QVERIFY(second.stop().isEmpty());
    QVERIFY(honorsTimerRequest());
    QVERIFY(first.stop().isEmpty());
    QVERIFY(samePolicy(original));
    second.prepare(30, 31, 64, 8192);
    QVERIFY(second.stop().isEmpty());
    QVERIFY(samePolicy(original));
  }
  void asioCallbackCopiesOwnedBytes_data() {
    QTest::addColumn<bool>("timed");
    QTest::addColumn<int>("left");
    QTest::addColumn<int>("right");
    QTest::newRow("legacy-31-32") << false << 30 << 31;
    QTest::newRow("timed-31-32") << true << 30 << 31;
    QTest::newRow("timed-32-31") << true << 31 << 30;
    QTest::newRow("timed-1-2") << true << 0 << 1;
    QTest::newRow("legacy-same-first") << false << 0 << 0;
    QTest::newRow("timed-same-last") << true << 31 << 31;
  }
  void asioCallbackCopiesOwnedBytes() {
    QFETCH(bool, timed);
    QFETCH(int, left);
    QFETCH(int, right);
    FakeAsio driver;
    audio::AsioCapture capture;
    audio::AsioCaptureTestAccess::attach(capture, driver);
    auto stream = capture.prepare(left, right, 64, 8192);
    constexpr int blocks = 10000;
    audio::CaptureTrace trace(blocks);
    capture.setTrace(&trace);
    capture.start();
    for (int base = 0; base < blocks; base += 5) {
      for (int i = base; i < base + 5; ++i)
        driver.deliver(i, timed);
      for (int i = base; i < base + 5; ++i) {
        std::span<const std::byte> l, r;
        QVERIFY(stream.queue->peek(l, r));
        for (int j = 0; j < FakeAsio::frames; ++j) {
          float a, b;
          std::memcpy(&a, l.data() + j * 4, 4);
          std::memcpy(&b, r.data() + j * 4, 4);
          QCOMPARE(a, FakeAsio::marker(i, left, j));
          QCOMPARE(b, FakeAsio::marker(i, right, j));
        }
        QCOMPARE(audio::captureChecksum(l, r), trace.entries[i].copied);
        stream.queue->pop();
      }
    }
    capture.stop();
    QCOMPARE(stream.queue->fault.load(), 0);
    QCOMPARE(trace.used, size_t(blocks));
    QCOMPARE(trace.overflow, size_t(0));
    for (size_t i = 0; i < trace.used; ++i) {
      const auto &entry = trace.entries[i];
      QVERIFY(entry.queued);
      QCOMPARE(entry.bufferIndex, long(i % 2));
      QCOMPARE(entry.timeCallback, timed);
      QCOMPARE(entry.sourceBefore, entry.copied);
      QCOMPARE(entry.sourceAfter, entry.copied);
      QVERIFY(entry.endTicks >= entry.beginTicks);
      if (timed)
        QCOMPARE(entry.samplePosition, uint64_t(i * FakeAsio::frames));
    }
    QVERIFY(!driver.running);
    QVERIFY(!driver.allocated);
  }
  void asioCallbackConcurrentConsumer() {
    FakeAsio driver;
    audio::AsioCapture capture;
    audio::AsioCaptureTestAccess::attach(capture, driver);
    auto stream = capture.prepare(30, 31, 64, 8192);
    constexpr int blocks = 20000;
    audio::CaptureTrace trace(blocks);
    std::vector<uint64_t> hashes(blocks);
    std::atomic<int> consumed{0};
    capture.setTrace(&trace);
    capture.start();
    std::thread producer([&] {
      for (int i = 0; i < blocks; ++i) {
        while (i - consumed.load(std::memory_order_acquire) >= 8)
          std::this_thread::yield();
        driver.deliver(i);
      }
    });
    bool exact = true;
    for (int i = 0; i < blocks; ++i) {
      std::span<const std::byte> left, right;
      while (!stream.queue->peek(left, right))
        std::this_thread::yield();
      for (int j = 0; j < FakeAsio::frames; ++j) {
        float l, r;
        std::memcpy(&l, left.data() + j * 4, 4);
        std::memcpy(&r, right.data() + j * 4, 4);
        exact &=
            l == FakeAsio::marker(i, 30, j) && r == FakeAsio::marker(i, 31, j);
      }
      hashes[i] = audio::captureChecksum(left, right);
      stream.queue->pop();
      consumed.store(i + 1, std::memory_order_release);
    }
    producer.join();
    capture.stop();
    QVERIFY(exact);
    QCOMPARE(stream.queue->fault.load(), 0);
    QCOMPARE(trace.used, size_t(blocks));
    for (int i = 0; i < blocks; ++i)
      QCOMPARE(trace.entries[i].copied, hashes[i]);
  }
  void asioTraceCapacityAndDiscontinuity() {
    FakeAsio driver;
    audio::AsioCapture capture;
    audio::AsioCaptureTestAccess::attach(capture, driver);
    auto stream = capture.prepare(30, 31, 64, 8192);
    audio::CaptureTrace trace(1);
    capture.setTrace(&trace);
    capture.start();
    driver.deliver(0);
    driver.deliver(1, true, 64);
    capture.stop();
    QCOMPARE(stream.queue->fault.load(), 5);
    QCOMPARE(trace.used, size_t(1));
    QCOMPARE(trace.overflow, size_t(1));
  }
  void asioCallbackRejectsInvalidBuffer() {
    FakeAsio driver;
    audio::AsioCapture capture;
    audio::AsioCaptureTestAccess::attach(capture, driver);
    auto stream = capture.prepare(30, 31, 64, 8192);
    audio::CaptureTrace trace(2);
    capture.setTrace(&trace);
    capture.start();
    driver.callbacks.bufferSwitch(2, ASIOTrue);
    capture.stop();
    QCOMPARE(stream.queue->fault.load(), 1);
    QCOMPARE(stream.queue->capturedFrames(), uint64_t(0));
    QCOMPARE(trace.used, size_t(1));
    QVERIFY(!trace.entries[0].queued);
  }
  void pcmTypes() {
    for (int endian : {0, 16})
      for (int base : {0, 1, 2, 3, 4, 8, 9, 10, 11}) {
        const auto format = audio::format(endian + base);
        for (int expected : {-32768, -16384, 0, 16384}) {
          uint64_t value;
          if (format.floating) {
            const double sample = double(expected) / 32768;
            value = format.bytes == 4
                        ? uint64_t(std::bit_cast<uint32_t>(float(sample)))
                        : std::bit_cast<uint64_t>(sample);
          } else
            value = uint64_t(int64_t(expected) *
                             (int64_t(1) << (format.bits - 16)));
          std::array<std::byte, 8> bytes{};
          for (int i = 0; i < format.bytes; ++i)
            bytes[i] = std::byte(
                value >> (8 * (format.bigEndian ? format.bytes - 1 - i : i)));
          QCOMPARE(audio::sample(bytes.data(), format), expected);
        }
      }
    for (int type : {-1, 5, 15, 21, 28, 32, 33, 40, 100})
      QVERIFY_THROWS_EXCEPTION(std::runtime_error, audio::format(type));
  }
  void clippingAndInvalidFloat() {
    auto format = audio::format(20);
    for (auto [value, expected] :
         {std::pair{-2., -32768}, std::pair{1., 32767}, std::pair{2., 32767},
          std::pair{.5 / 32768, 1}, std::pair{-.5 / 32768, -1}})
      QCOMPARE(
          audio::sample(reinterpret_cast<const std::byte *>(&value), format),
          expected);
    for (double value : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()})
      QVERIFY_THROWS_EXCEPTION(
          std::runtime_error,
          audio::sample(reinterpret_cast<const std::byte *>(&value), format));
  }
  void channelOrder() {
    std::array<int16_t, 3> left{1, 2, 3}, right{-1, -2, -3};
    std::array<int16_t, 6> result{};
    QVERIFY(audio::convert(std::as_bytes(std::span(left)), audio::format(16),
                           std::as_bytes(std::span(right)), audio::format(16), result));
    QCOMPARE(result, (std::array<int16_t, 6>{1, -1, 2, -2, 3, -3}));
    QVERIFY(!audio::convert(std::as_bytes(std::span(left)).first(3),
                            audio::format(16), std::as_bytes(std::span(right)),
                            audio::format(16), result));
  }
  void boundedQueue() {
    audio::CaptureQueue queue(2, 4, 4, 2);
    std::array<int16_t, 2> left{31, 32}, right{1, 2};
    QVERIFY(queue.push(left.data(), right.data()));
    QVERIFY(queue.push(right.data(), left.data()));
    QVERIFY(!queue.push(left.data(), right.data()));
    QCOMPARE(queue.queuedFrames(), 4ULL);
    std::span<const std::byte> l, r;
    QVERIFY(queue.peek(l, r));
    std::array<int16_t, 4> result{};
    QVERIFY(audio::convert(l, audio::format(16), r, audio::format(16), result));
    QCOMPARE(result, (std::array<int16_t, 4>{31, 1, 32, 2}));
    queue.pop();
    QVERIFY(queue.peek(l, r));
    QVERIFY(audio::convert(l, audio::format(16), r, audio::format(16), result));
    QCOMPARE(result, (std::array<int16_t, 4>{1, 31, 2, 32}));
    queue.pop();
    QVERIFY(!queue.peek(l, r));
    QVERIFY(queue.push(left.data(), right.data()));
    QCOMPARE(queue.capturedFrames(), 6ULL);
  }
  void concurrentQueue() {
    audio::CaptureQueue queue(1, 4, 4, 64);
    constexpr uint32_t count = 100000;
    std::thread producer([&] {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t opposite = ~i;
        while (!queue.push(&i, &opposite))
          std::this_thread::yield();
      }
    });
    bool matched = true;
    for (uint32_t i = 0; i < count; ++i) {
      std::span<const std::byte> l, r;
      while (!queue.peek(l, r))
        std::this_thread::yield();
      uint32_t a, b;
      std::memcpy(&a, l.data(), 4);
      std::memcpy(&b, r.data(), 4);
      matched &= a == i && b == ~i;
      queue.pop();
    }
    producer.join();
    QVERIFY(matched);
    QCOMPARE(queue.queuedFrames(), 0ULL);
  }
};
QTEST_GUILESS_MAIN(AudioTests)
#include "Tests.moc"
