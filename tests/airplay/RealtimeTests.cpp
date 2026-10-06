#include "airplay/AudioSendCore.h"
#include "airplay/Crypto.h"
#include "airplay/RealtimeAudioSender.h"
#include "airplay/ThreadScheduling.h"
#include "airplay/RealtimeWait.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtTest>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <new>
#include <openssl/crypto.h>
#include <thread>
#include <system_error>

// Count C++ and OpenSSL allocations on only the measured thread. Install the
// OpenSSL hooks before Qt or any crypto API initializes its allocator.
namespace {
thread_local bool measuring = false;
thread_local size_t allocations = 0;
void *allocate(size_t size) {
  if (measuring)
    ++allocations;
  if (auto *p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc();
}
void *cryptoAllocate(size_t size, const char *, int) {
  if (measuring)
    ++allocations;
  return std::malloc(size);
}
void *cryptoReallocate(void *p, size_t size, const char *, int) {
  if (measuring)
    ++allocations;
  return std::realloc(p, size);
}
void cryptoFree(void *p, const char *, int) { std::free(p); }
struct Measurement {
  Measurement() {
    allocations = 0;
    measuring = true;
  }
  ~Measurement() { measuring = false; }
};
using namespace airplay;
struct Sink {
  uint64_t packets = 0, retransmits = 0;
  bool fail = false;
  std::array<unsigned char, audioPacketCapacity + 4> last{};
  size_t length = 0;
  static bool send(void *context, size_t, bool control, uint16_t,
                   std::span<const unsigned char> data) noexcept {
    auto &self = *static_cast<Sink *>(context);
    if (self.fail)
      return false;
    if (control)
      ++self.retransmits;
    else
      ++self.packets;
    self.length = data.size();
    std::copy(data.begin(), data.end(), self.last.begin());
    return true;
  }
};
audio::CaptureStream streamFor(int frames = 64) {
  return {std::make_shared<audio::CaptureQueue>(frames, size_t(frames) * 2,
                                                size_t(frames) * 2, 1024),
          audio::format(16), audio::format(16), frames};
}
// Reproduce the pre-change per-packet allocation path for a comparable
// baseline.
QByteArray baselinePacket(const QByteArray &key, std::span<const int16_t> pcm,
                          uint16_t sequence, uint32_t rtp, uint64_t counter) {
  QByteArray header;
  header.append(char(0x80));
  header.append(char(counter ? 0x60 : 0xe0));
  appendBe(header, sequence, 2);
  appendBe(header, rtp, 4);
  appendBe(header, 0, 4);
  const auto iv = nonce(counter);
  return header + seal(key, iv, alac(pcm), header.mid(4, 8)) + iv.mid(4);
}
#ifdef Q_OS_WIN
struct WindowsFailures {
  static inline bool registerFailure = false, priorityFailure = false;
  static inline int registered = 0, priorities = 0, reverted = 0, closed = 0;
  static inline DWORD owner = 0;
  static inline bool wrongThread = false, wrongRequest = false;
  static HANDLE WINAPI registerTask(LPCWSTR name, LPDWORD index) {
    ++registered;
    owner = GetCurrentThreadId();
    wrongRequest |= QString::fromWCharArray(name) != "Pro Audio";
    *index = 9;
    SetLastError(ERROR_SERVICE_NOT_ACTIVE);
    return registerFailure ? nullptr : reinterpret_cast<HANDLE>(uintptr_t(1));
  }
  static BOOL WINAPI priority(HANDLE, AVRT_PRIORITY value) {
    ++priorities;
    wrongThread |= GetCurrentThreadId() != owner;
    wrongRequest |= value != AVRT_PRIORITY_HIGH;
    SetLastError(ERROR_ACCESS_DENIED);
    return !priorityFailure;
  }
  static BOOL WINAPI revert(HANDLE) {
    ++reverted;
    wrongThread |= GetCurrentThreadId() != owner;
    return TRUE;
  }
  static HANDLE WINAPI failTimer(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
  }
  static HANDLE WINAPI failEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  static BOOL WINAPI close(HANDLE handle) {
    ++closed;
    return CloseHandle(handle);
  }
  static BOOL WINAPI failArm(HANDLE, const LARGE_INTEGER *, LONG,
                             PTIMERAPCROUTINE, LPVOID, BOOL) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  static DWORD WINAPI failWait(DWORD, const HANDLE *, BOOL, DWORD) {
    SetLastError(ERROR_INVALID_HANDLE);
    return WAIT_FAILED;
  }
};
#endif
} // namespace
void *operator new(size_t size) { return allocate(size); }
void *operator new[](size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

class RealtimeTests : public QObject {
  Q_OBJECT
private slots:
  void kernelDetection() {
    QVERIFY(classifyKernelRealtime(QString("1\n"), "custom").realtime);
    QVERIFY(!classifyKernelRealtime(QString("0\n"), "6.12-rt12").realtime);
    for (const auto &release : {"6.12-rt12", "6.12.1-rt-arch1", "6.12-rt"})
      QVERIFY(classifyKernelRealtime(std::nullopt, release).realtime);
    for (const auto &release : {"6.12-arch1-1", "custom-rtdebug", "rt-6.12"})
      QVERIFY(!classifyKernelRealtime(std::nullopt, release).realtime);
    const auto fallback =
        classifyKernelRealtime(QString("invalid"), "6.12-rt1");
    QVERIFY(fallback.realtime);
    QVERIFY(fallback.source.contains("invalid"));
  }
  void schedulingFailureClassification() {
    QVERIFY(realtimeSchedulingAccepted(true, 0, 0, true, 80));
    QVERIFY(!realtimeSchedulingAccepted(true, EPERM, 0, false, 0));
    QVERIFY(!realtimeSchedulingAccepted(true, 0, EINVAL, true, 80));
    QVERIFY(!realtimeSchedulingAccepted(true, 0, 0, true, 79));
    QVERIFY(!realtimeSchedulingAccepted(true, 0, 0, false, 80));
    QVERIFY(realtimeSchedulingAccepted(false, EPERM, EINVAL, false, 0));
  }
  void queueBoundariesAndConcurrentPublication() {
    RealtimeQueue<uint64_t, 64> queue;
    for (uint64_t i = 0; i < 64; ++i)
      QVERIFY(queue.push(i));
    QVERIFY(!queue.push(64));
    uint64_t value = 0;
    for (uint64_t i = 0; i < 64; ++i) {
      QVERIFY(queue.pop(value));
      QCOMPARE(value, i);
    }
    QVERIFY(!queue.pop(value));
    std::thread producer([&] {
      for (uint64_t i = 0; i < 100000; ++i)
        while (!queue.push(i))
          std::this_thread::yield();
    });
    bool ordered = true;
    for (uint64_t i = 0; i < 100000; ++i) {
      while (!queue.pop(value))
        std::this_thread::yield();
      ordered &= value == i;
    }
    producer.join();
    QVERIFY(ordered);
  }
  void pcmBoundaries() {
    std::array<float, 4> left{0.f, .5f, -2.f, 2.f}, right{1.f, -1.f, 0.f, .25f};
    std::array<int16_t, 8> result{};
    const auto l = std::as_bytes(std::span(left)),
               r = std::as_bytes(std::span(right));
    QVERIFY(audio::convert(l, audio::format(19), r, audio::format(19), result));
    QCOMPARE(result, (std::array<int16_t, 8>{0, 32767, 16384, -32768, -32768, 0,
                                             32767, 8192}));
    QVERIFY(
        !audio::convert(l, {0, 0, false, false}, r, audio::format(19), result));
    QVERIFY(!audio::convert(l, audio::format(19), r, audio::format(19),
                            std::span(result).first(7)));
    left[3] = std::numeric_limits<float>::quiet_NaN();
    QVERIFY(
        !audio::convert(l, audio::format(19), r, audio::format(19), result));
  }
  void encoderMatchesIndependentCrypto() {
    const QByteArray key(32, 'K');
    AudioPacketEncoder encoder;
    QVERIFY(encoder.prepare(
        {reinterpret_cast<const unsigned char *>(key.constData()), 32}));
    std::array<unsigned char, audioPacketCapacity> output{};
    QVERIFY(!encoder.encode({}, 0, 0, 0, output));
    for (int frames : {1, 64, 352}) {
      std::array<int16_t, 704> samples{};
      for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = int16_t(i * 123);
      const auto pcm = std::span(samples).first(size_t(frames) * 2);
      for (uint64_t counter :
           {uint64_t(0), uint64_t(1), uint64_t(65536), UINT64_MAX - 1}) {
        const auto expected =
            baselinePacket(key, pcm, uint16_t(65535 + counter),
                           uint32_t(UINT32_MAX + counter), counter);
        const auto size =
            encoder.encode(pcm, uint16_t(65535 + counter),
                           uint32_t(UINT32_MAX + counter), counter, output);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(output.data()),
                            qsizetype(size)),
                 expected);
      }
    }
    QVERIFY(!encoder.prepare({}));
    std::array<int16_t, 2> sample{};
    QVERIFY(!encoder.encode(sample, 0, 0, 0, output));
  }
  void coreBudgetGenerationAndFailures() {
    app::Timing timing;
    timing.prebufferSamples = 1;
    auto stream = streamFor();
    stream.gapPolicy = audio::GapPolicy::Silence;
    AudioSendCore core(timing, stream, 65535, UINT32_MAX, 1);
    std::array<unsigned char, 32> key{};
    QVERIFY(core.preparePeer(0, key));
    core.begin(1000000000);
    Sink sink;
    QCOMPARE(core.step(1000000000, 2000000000, Sink::send, &sink).code,
             SendFailure::None);
    QCOMPARE(sink.packets, 1ULL);
    RetransmitRequest request{0, 1, 3, 65535, 64};
    int budget = 32;
    QCOMPARE(core.retransmit(request, budget, Sink::send, &sink).code,
             SendFailure::None);
    QCOMPARE(budget, 0);
    QCOMPARE(request.count, uint16_t(32));
    QCOMPARE(sink.retransmits, 1ULL);
    QCOMPARE(core.report().expired, 31ULL);
    stream.queue->generation.fetch_add(1);
    QCOMPARE(core.step(1002000000, 2002000000, Sink::send, &sink).code,
             SendFailure::None);
    sink.fail = true;
    QCOMPARE(core.step(1004000000, 2004000000, Sink::send, &sink).code,
             SendFailure::Send);
    sink.fail = false;
    QCOMPARE(core.step(1005000000, 9000000000, Sink::send, &sink).code,
             SendFailure::ClockJump);
    stream.queue->interrupted.store(true);
    QCOMPARE(core.step(1005000000, 2005000000, Sink::send, &sink).code,
             SendFailure::Interrupted);
  }
  void noAllocationsAfterPreparation() {
    app::Timing timing;
    timing.prebufferSamples = 1;
    auto stream = streamFor();
    AudioSendCore core(timing, stream, 0, 0, 2);
    const std::array<unsigned char, 32> key{};
    QVERIFY(core.preparePeer(0, key));
    QVERIFY(core.preparePeer(1, key));
    core.begin(0);
    Sink sink;
    std::array<int16_t, 64> input{};
    SendFailure failure = SendFailure::None;
    {
      Measurement measurement;
      for (uint64_t i = 0; i < 10000; ++i) {
        if (!stream.queue->push(input.data(), input.data())) {
          failure = SendFailure::Backlog;
          break;
        }
        const auto now = int64_t(i * 64 * 1000000000 / 44100);
        failure = core.step(now, now + 1000000000, Sink::send, &sink).code;
        if (failure != SendFailure::None)
          break;
      }
    }
    QCOMPARE(failure, SendFailure::None);
    QCOMPARE(allocations, size_t(0));
    QCOMPARE(sink.packets, 20000ULL);
  }
  void coreFailureAndCapacityEdges() {
    const std::array<unsigned char, 32> key{};
    app::Timing timing;
    timing.prebufferSamples = 1;
    timing.backlogSamples = 64;
    auto stream = streamFor();
    std::array<int16_t, 64> input{};
    AudioSendCore core(timing, stream, 0, 0, 1);
    QVERIFY(core.preparePeer(0, key));
    core.begin(0);
    Sink sink;
    QCOMPARE(core.step(1000000001, 1000000001, Sink::send, &sink).code,
             SendFailure::InputTimeout);
    stream.queue->fault.store(2);
    const auto fault = core.step(0, 0, Sink::send, &sink);
    QCOMPARE(fault.code, SendFailure::InputFault);
    QCOMPARE(fault.detail, 2);
    stream.queue->fault.store(0);
    QVERIFY(stream.queue->push(input.data(), input.data()));
    QVERIFY(stream.queue->push(input.data(), input.data()));
    QCOMPARE(core.step(0, 0, Sink::send, &sink).code, SendFailure::Backlog);
    stream.queue->pop();
    QCOMPARE(core.step(0, 0, Sink::send, &sink).code, SendFailure::None);
    QCOMPARE(core.step(200000000, 200000000, Sink::send, &sink).code,
             SendFailure::Late);

    auto continuous = streamFor();
    continuous.gapPolicy = audio::GapPolicy::Silence;
    AudioSendCore recovery(timing, continuous, 0, 0, 1);
    QVERIFY(recovery.preparePeer(0, key));
    recovery.begin(0);
    QVERIFY(continuous.queue->push(input.data(), input.data()));
    QVERIFY(continuous.queue->push(input.data(), input.data()));
    QCOMPARE(recovery.step(0, 0, Sink::send, &sink).code, SendFailure::None);
    QCOMPARE(continuous.queue->generation.load(), 1ULL);
    QVERIFY(!continuous.queue->playbackActive.load());
  }
  void deadlineHistogramAndResume() {
    const std::array<int64_t, 6> delays{0, 100000, 500000, 1000000, 5000000, 6000000};
    for (size_t bucket = 0; bucket < delays.size(); ++bucket) {
      app::Timing timing;
      timing.packetSamples = 352;
      auto stream = streamFor();
      stream.gapPolicy = audio::GapPolicy::Silence;
      AudioSendCore core(timing, stream, 0, 0, 1);
      const std::array<unsigned char, 32> key{};
      QVERIFY(core.preparePeer(0, key));
      core.begin(0);
      Sink sink;
      QCOMPARE(core.step(0, 0, Sink::send, &sink).code, SendFailure::None);
      const auto now = int64_t(352) * 1000000000 / 44100 + delays[bucket];
      QCOMPARE(core.step(now, now, Sink::send, &sink).code, SendFailure::None);
      const auto report = core.report();
      QCOMPARE(report.maxLatenessNs, uint64_t(delays[bucket]));
      QCOMPARE(report.deadlineHistogram[bucket], bucket == 0 ? 2ULL : 1ULL);
      // Resume with both clocks advanced retains the existing late-session
      // failure; a wall-only jump retains the clock discontinuity failure.
      QCOMPARE(core.step(60000000000LL, 60000000000LL, Sink::send, &sink).code,
               SendFailure::Late);
      QCOMPARE(core.step(now, 60000000000LL, Sink::send, &sink).code,
               SendFailure::ClockJump);
    }
  }
  void fixedCapacityStress_data() {
    QTest::addColumn<int>("frames");
    for (int frames : {1, 64, 352})
      QTest::newRow(qPrintable(QString::number(frames))) << frames;
  }
  void fixedCapacityStress() {
    QFETCH(int, frames);
    app::Timing timing;
    timing.packetSamples = frames;
    timing.prebufferSamples = 1;
    auto stream = streamFor(frames);
    AudioSendCore core(timing, stream, 65500, UINT32_MAX - 1000, 2);
    std::array<unsigned char, 32> key{};
    QVERIFY(core.preparePeer(0, key));
    QVERIFY(core.preparePeer(1, key));
    core.begin(0);
    std::array<int16_t, 352> input{};
    Sink sink;
    SendFailure error = SendFailure::None;
    QElapsedTimer timer;
    timer.start();
    {
      Measurement measure;
      for (uint64_t i = 0; i < 100000; ++i) {
        if (!stream.queue->push(input.data(), input.data())) {
          error = SendFailure::Backlog;
          break;
        }
        const auto now = int64_t(i * uint64_t(frames) * 1000000000 / 44100);
        error = core.step(now, now + 1000000000, Sink::send, &sink).code;
        if (error != SendFailure::None)
          break;
        if (i % 64 == 0) {
          RetransmitRequest request{0, 1, 0, uint16_t(65500 + i), 1};
          int budget = 32;
          error = core.retransmit(request, budget, Sink::send, &sink).code;
          if (error != SendFailure::None)
            break;
        }
      }
    }
    const auto work = timer.nsecsElapsed();
    QCOMPARE(error, SendFailure::None);
    QCOMPARE(allocations, size_t(0));
    QCOMPARE(sink.packets, 200000ULL);
    const auto report = core.report();
    qInfo("STRESS frames=%d packets=%llu retransmits=%llu total_work_ns=%lld "
          "max_gap_ns=%llu max_buffered_frames=%llu allocations=%zu",
          frames, static_cast<unsigned long long>(sink.packets),
          static_cast<unsigned long long>(sink.retransmits),
          static_cast<long long>(work),
          static_cast<unsigned long long>(report.maxGapNs),
          static_cast<unsigned long long>(report.maxBuffered), allocations);
  }
#ifdef Q_OS_WIN
  void windowsSchedulingFailures_data() {
    QTest::addColumn<bool>("registerFailure");
    QTest::addColumn<bool>("priorityFailure");
    QTest::newRow("register") << true << false;
    QTest::newRow("priority") << false << true;
    QTest::newRow("accepted") << false << false;
  }
  void windowsSchedulingFailures() {
    QFETCH(bool, registerFailure);
    QFETCH(bool, priorityFailure);
    using F = WindowsFailures;
    F::registerFailure = registerFailure;
    F::priorityFailure = priorityFailure;
    F::registered = F::priorities = F::reverted = 0;
    F::wrongThread = F::wrongRequest = false;
    SchedulingResult result;
    std::thread worker([&] {
      AudioThreadScheduling scope({F::registerTask, F::priority, F::revert});
      result = scope.result();
    });
    worker.join();
    QCOMPARE(result.ready, !registerFailure && !priorityFailure);
    QCOMPARE(F::registered, 1);
    QCOMPARE(F::priorities, registerFailure ? 0 : 1);
    QCOMPARE(F::reverted, registerFailure ? 0 : 1);
    QVERIFY(!F::wrongThread);
    QVERIFY(!F::wrongRequest);
    QVERIFY(result.description.contains("Pro Audio/HIGH"));
    if (registerFailure)
      QVERIFY(result.description.contains("registrationError=1062"));
    if (priorityFailure)
      QVERIFY(result.description.contains("priorityError=5"));
  }
  void windowsWaitFailures() {
    using F = WindowsFailures;
    RealtimeWait::WindowsCalls calls;
    calls.close = F::close;
    calls.createTimer = F::failTimer;
    F::closed = 0;
    try {
      RealtimeWait wait(calls);
      QFAIL("Unsupported high resolution timer must fail initialization");
    } catch (const std::system_error &error) {
      QCOMPARE(error.code().value(), int(ERROR_NOT_SUPPORTED));
    }
    QCOMPARE(F::closed, 0);
    calls.createTimer = CreateWaitableTimerExW;
    calls.createEvent = F::failEvent;
    try {
      RealtimeWait wait(calls);
      QFAIL("Failed wake event must fail initialization");
    } catch (const std::system_error &error) {
      QCOMPARE(error.code().value(), int(ERROR_NOT_ENOUGH_MEMORY));
    }
    QCOMPARE(F::closed, 1); // Timer from partial initialization was closed.
    calls.createEvent = CreateEventW;
    calls.arm = F::failArm;
    {
      RealtimeWait wait(calls);
      QVERIFY(!wait.until(wait.now() + 1000000000));
      QCOMPARE(RealtimeWait::lastError(), int(ERROR_INVALID_HANDLE));
    }
    QCOMPARE(F::closed, 3);
    calls.arm = SetWaitableTimer;
    calls.wait = F::failWait;
    {
      RealtimeWait wait(calls);
      QVERIFY(!wait.until(wait.now() + 1000000000));
      QCOMPARE(RealtimeWait::lastError(), int(ERROR_INVALID_HANDLE));
    }
    QCOMPARE(F::closed, 5);
  }
#endif
  void senderQueueOverflowAndEarlyStop() {
    RealtimeAudioSender sender({}, streamFor(), 0, 0, 1);
    for (int i = 0; i < 64; ++i)
      QVERIFY(sender.retransmit({0, 1234, 0, 0, 1}));
    QVERIFY(!sender.retransmit({0, 1234, 0, 0, 1}));
    QCOMPARE(sender.error().code, SendFailure::QueueFull);
    QCOMPARE(sender.poll().requestHighWater, uint64_t(64));
    QElapsedTimer elapsed;
    elapsed.start();
    sender.stop();
    sender.stop();
    QVERIFY(elapsed.elapsed() < 500);
    QVERIFY_THROWS_EXCEPTION(std::logic_error, sender.begin());
  }
  void nativeSendFailureAndBindingCleanup() {
    QUdpSocket control;
    QVERIFY(control.bind(QHostAddress::LocalHost, 0));
    const auto port = control.localPort();
    {
      auto stream = streamFor();
      stream.gapPolicy = audio::GapPolicy::Silence;
      RealtimeAudioSender sender({}, stream, 0, 0, 1);
      if (!sender.ready()) {
#ifdef Q_OS_WIN
        QFAIL(qPrintable(sender.schedulingLog()));
#else
        QVERIFY(kernelRealtime().realtime);
        QSKIP("RT scheduling permission unavailable");
#endif
      }
      // Broadcast without SO_BROADCAST is rejected by both Winsock and POSIX.
      sender.bindPeer(0, QHostAddress::LocalHost, QHostAddress::Broadcast, {},
                      control.socketDescriptor());
      sender.preparePeer(0, QByteArray(32, 'k'), port);
      sender.begin();
      QTRY_COMPARE_WITH_TIMEOUT(sender.error().code, SendFailure::Send, 2000);
      QVERIFY(sender.error().detail != 0);
      sender.stop();
    }
    // Destroying the sender must not close Qt's control socket.
    QCOMPARE(control.localPort(), port);
    QCOMPARE(control.writeDatagram("x", 1, QHostAddress::LocalHost, port), qint64(1));
    QTRY_VERIFY_WITH_TIMEOUT(control.hasPendingDatagrams(), 1000);
    QCOMPARE(control.receiveDatagram().data(), QByteArray("x"));
    {
      RealtimeAudioSender sender({}, streamFor(), 0, 0, 1);
      QVERIFY_THROWS_EXCEPTION(std::exception, sender.bindPeer(0, QHostAddress::LocalHost,
          QHostAddress::LocalHost, {}, -1));
      sender.stop();
    }
  }
  void absoluteTimerAndStopWake() {
    RealtimeWait wait;
    auto now = wait.now();
    QVERIFY(wait.until(now - 1));
    QVERIFY(wait.now() - now < 500000000);
    QVERIFY(wait.until(now + 5000000));
    QVERIFY(wait.now() >= now + 5000000);
    wait.wake(); // Wake-before-wait must never be lost.
    now = wait.now();
    QVERIFY(wait.until(now + 2000000000));
    QVERIFY(wait.now() - now < 500000000);
    std::thread waker([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      wait.wake();
    });
    now = wait.now();
    const bool result = wait.until(now + 2000000000);
    const auto elapsed = wait.now() - now;
    waker.join();
    QVERIFY(result);
    QVERIFY(elapsed < 500000000);
  }
  void nativeSendAndStop() {
    QUdpSocket receiver, control;
    QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
    QVERIFY(control.bind(QHostAddress::LocalHost, 0));
    app::Timing timing;
    auto stream = streamFor();
    stream.gapPolicy = audio::GapPolicy::Silence;
    RealtimeAudioSender sender(timing, stream, 42, 7, 1);
    if (!sender.ready()) {
#ifdef Q_OS_WIN
      QFAIL(qPrintable(sender.schedulingLog()));
#else
      QVERIFY(kernelRealtime().realtime);
#endif
      QVERIFY(!sender.schedulingLog().isEmpty());
      QVERIFY(!receiver.hasPendingDatagrams());
      return; // RT-without-permission is a mandatory negative case.
    }
    const auto sourcePort =
        sender.bindPeer(0, QHostAddress::LocalHost, QHostAddress::LocalHost, {},
                        control.socketDescriptor());
    sender.preparePeer(0, QByteArray(32, 'k'), receiver.localPort());
    sender.begin();
    QTRY_VERIFY_WITH_TIMEOUT(receiver.hasPendingDatagrams(), 2000);
    const auto first = receiver.receiveDatagram();
    QCOMPARE(first.senderPort(), sourcePort);
    QCOMPARE(readBe(first.data(), 2, 2), uint64_t(42));
    QVERIFY(sender.retransmit({0, receiver.localPort(), 5, 42, 1}));
    bool found = false;
    QElapsedTimer timer;
    timer.start();
    while (!found && timer.elapsed() < 1000) {
      QTest::qWait(1);
      while (receiver.hasPendingDatagrams()) {
        const auto packet = receiver.receiveDatagram();
        if (packet.data().size() > 4 && uint8_t(packet.data()[1]) == 0xd6) {
          QCOMPARE(packet.senderPort(), control.localPort());
          QCOMPARE(packet.data().mid(4), first.data());
          found = true;
        }
      }
    }
    QVERIFY(found);
    timer.restart();
    sender.stop();
    sender.stop();
    QVERIFY(timer.elapsed() < 500);
    QVERIFY_THROWS_EXCEPTION(std::logic_error, sender.begin());
    while (receiver.hasPendingDatagrams())
      receiver.receiveDatagram();
    QTest::qWait(20);
    QVERIFY(!receiver.hasPendingDatagrams());
    QCOMPARE(sender.error().code, SendFailure::None);
    const auto report = sender.poll();
    qInfo("SENDER packets=%llu retransmits=%llu max_gap_ns=%llu "
          "max_work_ns=%llu max_buffered_frames=%llu",
          static_cast<unsigned long long>(report.packets),
          static_cast<unsigned long long>(report.retransmitted),
          static_cast<unsigned long long>(report.maxGapNs),
          static_cast<unsigned long long>(report.maxWorkNs),
          static_cast<unsigned long long>(report.maxBuffered));
  }
  void nativeSenderSurvivesDelayedController_data() {
    QTest::addColumn<int>("peers");
    QTest::newRow("single") << 1;
    QTest::newRow("stereo") << 2;
  }
  void nativeSenderSurvivesDelayedController() {
    QFETCH(int, peers);
    std::array<QUdpSocket, 2> receivers, controls;
    app::Timing timing;
    timing.lateMs = 500;
    auto stream = streamFor();
    stream.gapPolicy = audio::GapPolicy::Silence;
    RealtimeAudioSender sender(timing, stream, 0, 0, size_t(peers));
    if (!sender.ready()) {
#ifdef Q_OS_WIN
      QFAIL(qPrintable(sender.schedulingLog()));
#else
      QVERIFY(kernelRealtime().realtime);
#endif
      QSKIP("RT scheduling permission unavailable; negative path covered "
            "separately");
    }
    for (int i = 0; i < peers; ++i) {
      QVERIFY(receivers[i].bind(QHostAddress::LocalHost, 0));
      QVERIFY(controls[i].bind(QHostAddress::LocalHost, 0));
      sender.bindPeer(size_t(i), QHostAddress::LocalHost,
                      QHostAddress::LocalHost, {},
                      controls[i].socketDescriptor());
      sender.preparePeer(size_t(i), QByteArray(32, 'x'),
                         receivers[i].localPort());
    }
    sender.begin();
    // Model an expensive controller callback: no Qt events or sender polling.
    // This also fills the bounded report queue without backpressuring audio.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    sender.stop();
    const auto report = sender.poll();
    QCOMPARE(sender.error().code, SendFailure::None);
    QVERIFY(report.packets >= 20);
    for (int i = 0; i < peers; ++i) {
      QTRY_VERIFY_WITH_TIMEOUT(receivers[i].hasPendingDatagrams(), 1000);
      uint64_t received = 0;
      while (receivers[i].hasPendingDatagrams()) {
        receivers[i].receiveDatagram();
        ++received;
      }
      QVERIFY(received >= 20);
    }
    qInfo("NATIVE peers=%d packets=%llu max_gap_ns=%llu max_work_ns=%llu "
          "max_buffered_frames=%llu",
          peers, static_cast<unsigned long long>(report.packets),
          static_cast<unsigned long long>(report.maxGapNs),
          static_cast<unsigned long long>(report.maxWorkNs),
          static_cast<unsigned long long>(report.maxBuffered));
  }
  void encodingBenchmark() {
    const QByteArray key(32, 'k');
    std::array<int16_t, 128> pcm{};
    constexpr int iterations = 10000;
    for (int peers : {1, 2}) {
      QElapsedTimer timer;
      baselinePacket(key, pcm, 0, 0, 0);
      timer.start();
      for (int i = 0; i < iterations; ++i)
        for (int p = 0; p < peers; ++p)
          baselinePacket(key, pcm, uint16_t(i), uint32_t(i * 64), uint64_t(i));
      const auto baseline = timer.nsecsElapsed();
      AudioPacketEncoder first, second;
      QVERIFY(first.prepare(
          {reinterpret_cast<const unsigned char *>(key.constData()), 32}));
      QVERIFY(second.prepare(
          {reinterpret_cast<const unsigned char *>(key.constData()), 32}));
      std::array<unsigned char, audioPacketCapacity> output{};
      size_t bytes = 0;
      timer.restart();
      {
        Measurement measurement;
        for (int i = 0; i < iterations; ++i) {
          bytes += first.encode(pcm, uint16_t(i), uint32_t(i * 64), uint64_t(i),
                                output);
          if (peers == 2)
            bytes += second.encode(pcm, uint16_t(i), uint32_t(i * 64),
                                   uint64_t(i), output);
        }
      }
      const auto optimized = timer.nsecsElapsed();
      QCOMPARE(allocations, size_t(0));
      QVERIFY(bytes > 0);
      qInfo("BENCH peers=%d packets=%d baseline_ns=%lld optimized_ns=%lld "
            "allocations=%zu",
            peers, iterations, static_cast<long long>(baseline),
            static_cast<long long>(optimized), allocations);
    }
  }
  void pcmBenchmark() {
    std::array<float, 352> left{}, right{};
    for (size_t i = 0; i < left.size(); ++i) {
      left[i] = float(i) / 352;
      right[i] = -left[i];
    }
    const auto l = std::as_bytes(std::span(left)),
               r = std::as_bytes(std::span(right));
    const auto format = audio::format(19);
    constexpr int iterations = 10000;
    int64_t checksum = 0;
    QElapsedTimer timer;
    timer.start();
    size_t baselineAllocations = 0;
    {
      Measurement measure;
      for (int iteration = 0; iteration < iterations; ++iteration) {
        std::vector<int16_t> pcm(left.size() * 2);
        for (size_t i = 0; i < left.size(); ++i) {
          pcm[2 * i] = audio::sample(l.data() + i * sizeof(float), format);
          pcm[2 * i + 1] = audio::sample(r.data() + i * sizeof(float), format);
        }
        checksum += pcm[size_t(iteration) % pcm.size()];
      }
      baselineAllocations = allocations;
    }
    const auto baseline = timer.nsecsElapsed();
    std::array<int16_t, 704> pcm{};
    int64_t optimizedChecksum = 0;
    bool valid = true;
    timer.restart();
    {
      Measurement measure;
      for (int iteration = 0; iteration < iterations; ++iteration) {
        valid &= audio::convert(l, format, r, format, pcm);
        optimizedChecksum += pcm[size_t(iteration) % pcm.size()];
      }
    }
    const auto optimized = timer.nsecsElapsed();
    QVERIFY(valid);
    QCOMPARE(checksum, optimizedChecksum);
    QCOMPARE(allocations, size_t(0));
    QCOMPARE(baselineAllocations, size_t(iterations));
    qInfo("PCM frames=352 iterations=%d baseline_ns=%lld optimized_ns=%lld "
          "baseline_allocations=%zu optimized_allocations=%zu",
          iterations, static_cast<long long>(baseline),
          static_cast<long long>(optimized), baselineAllocations, allocations);
  }
};
int main(int argc, char **argv) {
  if (!CRYPTO_set_mem_functions(cryptoAllocate, cryptoReallocate, cryptoFree))
    return 2;
  QCoreApplication app(argc, argv);
  RealtimeTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "RealtimeTests.moc"
