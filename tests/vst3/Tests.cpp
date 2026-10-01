#include "../airplay/TestReceiver.h"
#include "airplay/Crypto.h"
#include "airplay/DiscoveryApi.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/alignedalloc.h"
#include "vst3/HostRecovery.h"
#include "vst3/Plugin.h"
#include "vst3/PluginRuntime.h"
#include <QFile>
#include <QJsonDocument>
#include <QTcpServer>
#include <QtTest>
#include <bit>
#include <thread>

using namespace Steinberg;
using namespace Steinberg::Vst;
class VstTests : public QObject {
  Q_OBJECT
private:
  static airplay::DiscoveryApi unavailableDiscovery() {
    airplay::DiscoveryApi api;
#ifdef Q_OS_WIN
    api.browse = [](auto *, auto *) { return DNS_STATUS(ERROR_NOT_SUPPORTED); };
#else
    api.browse = [](DNSServiceRef *, DNSServiceFlags, uint32_t, const char *,
                    const char *, DNSServiceBrowseReply,
                    void *) -> DNSServiceErrorType {
      return kDNSServiceErr_Unsupported;
    };
#endif
    return api;
  }
  static void ready(vst3::VstAudioInput &input, bool doubles = false,
                    int block = 2048) {
    input.configure(44100, block, doubles);
    input.setRealtime(true);
    input.setActive(true);
    input.setProcessing(true);
  }
  template <class T> void audio() {
    vst3::VstAudioInput input;
    ready(input, sizeof(T) == 8);
    auto stream = input.prepare(1);
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
    QVERIFY(stream.rateDiagnostics);
#endif
    QVERIFY(input.start());
    std::array<T, 2048> left{}, right{}, outLeft{}, outRight{};
    for (int i = 0; i < 2048; ++i) {
      left[i] = T((i % 100 - 50) / 64.);
      right[i] = -left[i];
    }
    T *in[]{left.data(), right.data()}, *out[]{outLeft.data(), outRight.data()};
    // Preserve negative zero and exact mantissas.
    left[0] = -T(0);
    left[1] = std::nextafter(T(.1), T(1));
    int total = 0;
    std::vector<T> expected;
    for (const int count : {0, 1, 31, 320, 511, 2048}) {
      input.process(in, out, count, 0, true);
      QCOMPARE(std::memcmp(left.data(), outLeft.data(), count * sizeof(T)), 0);
      QCOMPARE(std::memcmp(right.data(), outRight.data(), count * sizeof(T)),
               0);
      expected.insert(expected.end(), left.begin(), left.begin() + count);
      total += count;
    }
    QCOMPARE(stream.queue->capturedFrames(), uint64_t(total / 352 * 352));
    size_t cursor = 0;
    std::span<const std::byte> l, r;
    while (stream.queue->peek(l, r)) {
      QCOMPARE(std::memcmp(l.data(), expected.data() + cursor, l.size()), 0);
      const auto pcm = audio::convert(l, stream.left, r, stream.right);
      QCOMPARE(pcm.size(), size_t(704));
      QCOMPARE(pcm[0], audio::sample(l.data(), stream.left));
      cursor += 352;
      stream.queue->pop();
    }
    input.stop();
    auto before = left;
    input.process(in, in, 2048, 0, true);
    QCOMPARE(std::memcmp(left.data(), before.data(), sizeof(left)), 0);
    input.process(in, out, 352, 3, true);
    QVERIFY(std::all_of(outLeft.begin(), outLeft.begin() + 352,
                        [](T value) { return value == 0; }));
  }
private slots:
  void captureMatchesPocAlacVector() {
    QFile file(QString(AIRPLAY_TEST_SOURCE_DIR) +
               "/tests/airplay/vectors.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    const auto source = QByteArray::fromHex(json["pcm"].toString().toLatin1());
    const auto expected =
        QByteArray::fromHex(json["alac"].toString().toLatin1());
    std::array<float, 352> left{}, right{};
    for (size_t i = 0; i < 704; ++i) {
      const uint16_t word =
          uint8_t(source[i * 2]) | (uint16_t(uint8_t(source[i * 2 + 1])) << 8);
      (i % 2 ? right : left)[i / 2] = std::bit_cast<int16_t>(word) / 32768.f;
    }
    vst3::VstAudioInput input;
    ready(input);
    auto stream = input.prepare(.1);
    QVERIFY(input.start());
    float *first[]{left.data(), right.data()},
        *second[]{left.data() + 100, right.data() + 100};
    input.process(first, nullptr, 100, 0, true);
    input.process(second, nullptr, 252, 0, true);
    std::span<const std::byte> l, r;
    QVERIFY(stream.queue->peek(l, r));
    QCOMPARE(airplay::alac(audio::convert(l, stream.left, r, stream.right)),
             expected);
  }
  void sdkAlignedAllocation() {
    for (const uint32_t alignment : {0u, 8u, 16u, 32u, 64u, 256u, 4096u}) {
      for (const size_t size :
           {size_t(0), size_t(1), size_t(17), size_t(352 * 16)}) {
        void *data = Steinberg::Vst::aligned_alloc(size, alignment);
#ifdef Q_OS_MACOS
        // The SDK uses C aligned_alloc on macOS, unlike Windows' allocator.
        // Its size must be a multiple of alignment (WG14 DR 460).
        if (alignment && size % alignment) {
          QVERIFY(data == nullptr);
          continue;
        }
        if (!size && !data)
          continue; // Zero-size allocation may return null.
#endif
        QVERIFY(data);
        if (alignment)
          QCOMPARE(reinterpret_cast<uintptr_t>(data) % alignment, uintptr_t(0));
        if (size)
          std::memset(data, 0xa5, size);
        Steinberg::Vst::aligned_free(data, alignment);
      }
      Steinberg::Vst::aligned_free(nullptr, alignment);
    }
  }
  void float32() { audio<float>(); }
  void float64() { audio<double>(); }
  void overflowAndRestart() {
    vst3::VstAudioInput input;
    ready(input);
    auto old = input.prepare(.01);
    QVERIFY(input.start());
    std::array<float, 352> samples{};
    samples.fill(.25f);
    float *channels[]{samples.data(), samples.data()};
    for (int i = 0; i < 8; ++i)
      input.process(channels, nullptr, 352, 0, true);
    QCOMPARE(input.fault(), vst3::InputFault::Overflow);
    const auto captured = old.queue->capturedFrames();
    auto current = input.prepare(.1);
    QVERIFY(input.start());
    input.process(channels, channels, 352, 0, true);
    QCOMPARE(old.queue->capturedFrames(), captured);
    QCOMPARE(current.queue->capturedFrames(), uint64_t(352));
    QCOMPARE(current.queue->fault.load(), 0);
  }
  void conditionsStopOnlyAirplay() {
    for (int cause = 0; cause < 7; ++cause) {
      vst3::VstAudioInput input;
      ready(input, false, 352);
      auto stream = input.prepare(.1);
      QVERIFY(input.start());
      std::array<float, 353> source{}, output{};
      source.fill(.375f);
      float *in[]{source.data(), source.data()},
          *out[]{output.data(), output.data()};
      int count = 352;
      switch (cause) {
      case 0:
        input.setBypass(true);
        break;
      case 1:
        input.setProcessing(false);
        break;
      case 2:
        input.setActive(false);
        break;
      case 3:
        input.configure(48000, 352, false);
        break;
      case 4:
        input.setRealtime(false);
        break;
      case 5:
        count = 353;
        break;
      case 6:
        source[17] = std::numeric_limits<float>::quiet_NaN();
        break;
      }
      input.process(in, out, count, 0, cause != 4);
      QCOMPARE(std::memcmp(source.data(), output.data(), count * sizeof(float)),
               0);
      if (cause == 1 || cause == 2) {
        QCOMPARE(input.fault(), vst3::InputFault::None);
        QVERIFY(stream.queue->interrupted.load());
        QCOMPARE(stream.queue->fault.load(), 0);
      } else {
        QVERIFY(input.fault() != vst3::InputFault::None);
        QVERIFY(stream.queue->fault.load() >= 102);
      }
      QCOMPARE(stream.queue->capturedFrames(), uint64_t(0));
      input.setBypass(false);
      input.setProcessing(true);
      input.setActive(true);
      input.configure(44100, 352, false);
      input.setRealtime(true);
      QVERIFY(
          !input.start()); // Explicit prepare/start is required after a fault.
    }
  }
  void interruptionKeepsFatalFaultsAndDiscardsPartialAudio() {
    for (bool doubles : {false, true}) {
      vst3::VstAudioInput input;
      ready(input, doubles, 352);
      auto old = input.prepare(.1);
      QVERIFY(input.start());
      std::array<float, 352> f{};
      std::array<double, 352> d{};
      f.fill(.25f);
      d.fill(.25);
      float *fc[]{f.data(), f.data()};
      double *dc[]{d.data(), d.data()};
      if (doubles)
        input.process(dc, dc, 100, 0, true);
      else
        input.process(fc, fc, 100, 0, true);
      input.setProcessing(false);
      const auto first = input.interruption();
      QVERIFY(first.began > 0 && first.sequence == 1 && first.resumed == 0);
      input.setActive(false);
      QCOMPARE(input.interruption().began, first.began);
      QCOMPARE(input.interruption().sequence, uint64_t(2));
      input.setProcessing(true);
      input.setActive(true);
      if (doubles)
        input.process(dc, dc, 352, 0, true);
      else
        input.process(fc, fc, 352, 0, true);
      QVERIFY(input.interruption().resumed >= first.began);
      QVERIFY(!input.start());
      QCOMPARE(old.queue->capturedFrames(), uint64_t(0));
      auto fresh = input.prepare(.1, true);
      QVERIFY(input.start());
      if (doubles)
        input.process(dc, dc, 252, 0, true);
      else
        input.process(fc, fc, 252, 0, true);
      QCOMPARE(fresh.queue->capturedFrames(), uint64_t(0));
      input.setProcessing(false);
      input.setBypass(true);
      QCOMPARE(input.fault(), vst3::InputFault::Bypass);
      QCOMPARE(fresh.queue->fault.load(), 103);
      input.setBypass(false);
      input.setProcessing(true);
      QVERIFY_THROWS_EXCEPTION(std::runtime_error, input.prepare(.1, true));
    }
  }
  void hostRecoveryDeadlineAndCancellation() {
    using Recovery = vst3::HostRecovery;
    using Action = Recovery::Action;
    const int64_t start = 1'000'000'000;
    Recovery recovery;
    recovery.start();
    QCOMPARE(recovery.update({start, 0, 1}, false, false, false, start),
             Action::Cancel);
    recovery.start();
    recovery.streaming();
    QCOMPARE(
        recovery.update({start, start + 1, 1}, true, false, false, start + 1),
        Action::Interrupt);
    // Timely audio can be observed after a slow teardown or delayed UI pump.
    QCOMPARE(recovery.update({start, start + 1, 1}, true, false, false,
                             start + Recovery::window),
             Action::None);
    QCOMPARE(recovery.update({start, start + 1, 1}, true, true, false,
                             start + Recovery::window),
             Action::Reconnect);
    QCOMPARE(recovery.update({}, true, true, false, start + Recovery::window),
             Action::None);
    // A second pause during the reconnect never starts a retry loop.
    QCOMPARE(recovery.update({start + 2, 0, 1}, false, false, false, start + 3),
             Action::Cancel);
    for (bool lateCallback : {false, true}) {
      recovery.start();
      recovery.streaming();
      QCOMPARE(recovery.update({start, 0, 1}, false, false, false, start),
               Action::Interrupt);
      QCOMPARE(recovery.update(
                   {start, lateCallback ? start + Recovery::window : 0, 7},
                   lateCallback, true, false, start + Recovery::window),
               Action::Cancel);
    }
    recovery.start();
    recovery.streaming();
    QCOMPARE(recovery.update({start, 0, 1}, false, false, false, start),
             Action::Interrupt);
    QCOMPARE(
        recovery.update({start, start + 1, 2}, true, true, true, start + 1),
        Action::Cancel);
    recovery.start();
    recovery.streaming();
    recovery.cancel();
    recovery.streaming(); // A late signal cannot re-arm a manual stop.
    QCOMPARE(
        recovery.update({start, start + 1, 1}, true, true, false, start + 1),
        Action::None);
  }
  void runtimeRecovery_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto *name :
         {"resume", "closed-editor", "manual-stop", "bypass", "state-load",
          "invalid-state", "format", "offline", "timeout", "reconnect-failure",
          "repeat-pause", "stop-reconnect", "unload", "competing"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }
  void runtimeRecovery() {
    QFETCH(QString, scenario);
    test::Receiver receiver("left");
    receiver.stereo.clear();
#ifdef Q_OS_WIN
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Recovery Test", 0, 0, 0, 900, 900,
                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    struct WindowOwner {
      HWND value;
      ~WindowOwner() {
        if (value)
          DestroyWindow(value);
      }
    } windowOwner{window};
#else
    QWidget hostWindow;
    auto *window = reinterpret_cast<void *>(hostWindow.winId());
#endif
    QVERIFY(window);
    auto processor = std::make_unique<vst3::Processor>();
    ready(processor->state()->input, false, 1024);
    airplay::SessionEnvironment environment{[] {}, [] {}, false};
    auto &runtime = vst3::PluginRuntime::acquire(window, environment);
    auto *panel = runtime.open(processor->state(), unavailableDiscovery());
    auto timing = panel->timing();
    timing.settle = 0;
    timing.prebuffer = .008;
    timing.backlog = .5;
    timing.late = .5;
    timing.requestTimeout = .3;
    timing.teardownTimeout = .1;
    panel->setTiming(timing);
    airplay::NetworkBinding loopback;
    for (const auto &binding : airplay::NetworkBinding::available())
      if (binding.ipv4 == "127.0.0.1")
        loopback = binding;
    QVERIFY(!loopback.automatic());
    processor->state()->setNetworkBinding(loopback);
    panel->setNetworkBinding(loopback);
    panel->findChild<QTabWidget *>("receiverModes")->setCurrentIndex(1);
    panel->findChild<QLineEdit *>("manualFirst")
        ->setText(QString("127.0.0.1:%1").arg(receiver.port()));
    QElapsedTimer clock;
    QTimer producer;
    bool paused = false;
    uint64_t frames = 0;
    std::array<float, 352> samples{};
    samples.fill(.125f);
    float *channels[]{samples.data(), samples.data()};
    producer.setTimerType(Qt::PreciseTimer);
    producer.setInterval(4);
    QObject::connect(&producer, &QTimer::timeout, &producer, [&] {
      const auto target =
          uint64_t(clock.nsecsElapsed()) * 44100 / 1'000'000'000;
      if (paused || !processor) {
        frames = target;
        return;
      }
      while (frames + 352 <= target) {
        processor->state()->input.process(channels, channels, 352, 0, true);
        frames += 352;
      }
    });
    clock.start();
    producer.start();
    panel->findChild<QPushButton *>("start")->click();
    QTRY_VERIFY_WITH_TIMEOUT(receiver.packets.size() > 3, 4000);
    QCOMPARE(receiver.connections, 1);
    const auto id = processor->state()->id;
    paused = true;
    processor->setProcessing(false);
    processor->setActive(false);
    runtime.pump();
    QVERIFY(panel->findChild<QPushButton *>("stop")->isEnabled());
    QVERIFY(!panel->findChild<QTabWidget *>("receiverModes")->isEnabled());
    if (scenario == "closed-editor") {
      runtime.close(id);
      panel = nullptr;
    }
    if (scenario == "competing") {
      QTest::qWait(150); // Reservation survives completion of the old teardown.
      vst3::Processor other;
      ready(other.state()->input);
      auto *second = runtime.open(other.state(), unavailableDiscovery());
      second->findChild<QTabWidget *>("receiverModes")->setCurrentIndex(1);
      second->findChild<QLineEdit *>("manualFirst")
          ->setText(QString("127.0.0.1:%1").arg(receiver.port()));
      second->findChild<QPushButton *>("start")->click();
      const auto labels = second->findChildren<QLabel *>();
      QVERIFY(std::any_of(labels.begin(), labels.end(), [](auto *value) {
        return value->text().contains("另一个 AirPlayQt");
      }));
      runtime.close(other.state()->id);
    }
    bool resumes = scenario == "resume" || scenario == "closed-editor" ||
                   scenario == "competing";
    if (scenario == "manual-stop")
      panel->findChild<QPushButton *>("stop")->click();
    if (scenario == "bypass")
      processor->state()->input.setBypass(true);
    if (scenario == "invalid-state") {
      MemoryStream invalid;
      QCOMPARE(processor->setState(&invalid), kResultFalse);
    }
    if (scenario == "state-load") {
      MemoryStream saved;
      QCOMPARE(processor->getState(&saved), kResultOk);
      saved.seek(0, IBStream::kIBSeekSet, nullptr);
      QCOMPARE(processor->setState(&saved), kResultOk);
    }
    if (scenario == "format")
      processor->state()->input.configure(48000, 1024, false);
    if (scenario == "offline")
      processor->state()->input.setRealtime(false);
    if (scenario == "unload") {
      producer.stop();
      runtime.close(id);
      processor.reset();
      QVERIFY(!vst3::PluginRuntime::exists());
      QCOMPARE(receiver.connections, 1);
      return;
    }
    if (scenario == "timeout")
      QTRY_VERIFY_WITH_TIMEOUT(
          !panel->findChild<QPushButton *>("stop")->isEnabled(), 6000);
    runtime.pump();
    if (scenario == "reconnect-failure")
      receiver.failure = test::Receiver::Failure::SessionSetup;
    if (scenario == "repeat-pause" || scenario == "stop-reconnect")
      receiver.failure = test::Receiver::Failure::InfoTimeout;
    processor->state()->input.configure(44100, 1024, false);
    processor->state()->input.setBypass(false);
    processor->setActive(true);
    processor->setProcessing(true);
    paused = false;
    if (resumes) {
      const auto packets = receiver.packets.size();
      QTRY_COMPARE_WITH_TIMEOUT(receiver.connections, 2, 4000);
      QTRY_VERIFY_WITH_TIMEOUT(receiver.packets.size() > packets + 3, 4000);
      QVERIFY(receiver.teardowns >= 1);
      if (!panel) {
        panel = runtime.open(processor->state(), unavailableDiscovery());
        QVERIFY(panel->findChild<QPushButton *>("stop")->isEnabled());
      }
      QCOMPARE(panel->networkBinding(), loopback);
      QCOMPARE(receiver.dataSource, QHostAddress(loopback.ipv4));
    } else if (scenario == "reconnect-failure" || scenario == "repeat-pause" ||
               scenario == "stop-reconnect") {
      QTRY_COMPARE_WITH_TIMEOUT(receiver.connections, 2, 4000);
      if (scenario == "repeat-pause") {
        paused = true;
        processor->setProcessing(false);
        runtime.pump();
        processor->setProcessing(true);
        paused = false;
      }
      if (scenario == "stop-reconnect")
        panel->findChild<QPushButton *>("stop")->click();
      QTRY_VERIFY_WITH_TIMEOUT(
          !panel->findChild<QPushButton *>("stop")->isEnabled(), 2000);
      QTest::qWait(400);
      QCOMPARE(receiver.connections, 2);
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(
          !panel->findChild<QPushButton *>("stop")->isEnabled(), 2000);
      QTest::qWait(150);
      QCOMPARE(receiver.connections, 1);
    }
    producer.stop();
    panel->findChild<QPushButton *>("stop")->click();
    runtime.close(id);
    processor.reset();
    QVERIFY(!vst3::PluginRuntime::exists());
    QVERIFY2(receiver.error.isEmpty(), qPrintable(receiver.error));
  }
  void noAutomaticCapture() {
    vst3::Processor processor;
    QVERIFY(!vst3::PluginRuntime::exists());
    ProcessSetup setup{kRealtime, kSample64, 512, 48000};
    QCOMPARE(processor.setupProcessing(setup), kResultOk);
    processor.setActive(true);
    processor.setProcessing(true);
    QVERIFY(!processor.state()->input.unavailable().isEmpty());
    QCOMPARE(processor.getLatencySamples(), uint32(0));
    QCOMPARE(processor.getTailSamples(), uint32(0));
    ProcessData empty{};
    empty.symbolicSampleSize = kSample64;
    empty.processMode = kRealtime;
    QCOMPARE(processor.process(empty), kResultOk);
    QVERIFY(!vst3::PluginRuntime::exists());
  }
  void stateIsAtomicAndVersioned() {
    vst3::SavedState expected;
    expected.timing.lead = .125;
    expected.bypass = true;
    expected.networkBinding = {"en-test", "192.0.2.10"};
    MemoryStream valid;
    QVERIFY(vst3::writeState(&valid, expected));
    const QByteArray bytes(valid.getData(), int(valid.getSize()));
    QVERIFY(!bytes.contains("192.168"));
    for (int length = 0; length < bytes.size(); ++length) {
      MemoryStream truncated;
      int32 written = 0;
      truncated.write(const_cast<char *>(bytes.constData()), length, &written);
      truncated.seek(0, IBStream::kIBSeekSet, nullptr);
      vst3::SavedState result;
      result.timing.lead = 1.5;
      QVERIFY(!vst3::readState(&truncated, result));
      QCOMPARE(result.timing.lead, 1.5);
    }
    valid.seek(0, IBStream::kIBSeekSet, nullptr);
    vst3::SavedState actual;
    QVERIFY(vst3::readState(&valid, actual));
    QCOMPARE(actual.timing.lead, .125);
    QVERIFY(actual.bypass);
    QCOMPARE(actual.networkBinding, expected.networkBinding);
    auto old = bytes;
    old[4] = 1;
    MemoryStream legacy;
    int32 legacyWritten = 0;
    legacy.write(old.data(), old.size(), &legacyWritten);
    legacy.seek(0, IBStream::kIBSeekSet, nullptr);
    vst3::Processor processor;
    QCOMPARE(processor.setState(&legacy), kResultFalse);
    QVERIFY(processor.state()->invalidConfiguration.load());
    QVERIFY(processor.state()->configurationError().contains("重新添加"));
    MemoryStream rejectedSave;
    QCOMPARE(processor.getState(&rejectedSave), kResultFalse);
    for (const int offset : {0, 4, 8, 116}) {
      auto changed = bytes;
      changed[offset] = char(255);
      MemoryStream corrupt;
      int32 written = 0;
      corrupt.write(changed.data(), changed.size(), &written);
      corrupt.seek(0, IBStream::kIBSeekSet, nullptr);
      QVERIFY(!vst3::readState(&corrupt, actual));
    }
    expected.timing.prebuffer = expected.timing.backlog;
    MemoryStream invalid;
    QVERIFY(!vst3::writeState(&invalid, expected));
  }
  void registryAndDestructionOrder() {
    auto *processor = new vst3::Processor;
    auto state = processor->state();
    QCOMPARE(vst3::PluginState::find(state->id), state);
    auto *controller = new vst3::EditController;
    processor->release();
    QVERIFY(!state->processorAlive.load());
    controller->release();
    const auto id = state->id;
    state.reset();
    QVERIFY(!vst3::PluginState::find(id));
    controller = new vst3::EditController;
    processor = new vst3::Processor;
    controller->release();
    processor->release();
    QVERIFY(!vst3::PluginRuntime::exists());
  }
  void producerRetirement() {
    vst3::VstAudioInput input;
    ready(input);
    auto stream = input.prepare(1);
    QVERIFY(input.start());
    std::atomic<bool> run{true};
    std::thread producer([&] {
      float samples[16]{};
      float *channels[]{samples, samples};
      while (run.load())
        input.process(channels, channels, 16, 0, true);
    });
    input.stop();
    run = false;
    producer.join();
    auto next = input.prepare(.1);
    QVERIFY(input.start());
    QVERIFY(next.queue != stream.queue);
  }
  void borrowedQtAndEditorReopen() {
    auto *original = QCoreApplication::instance();
#ifdef Q_OS_WIN
    const auto priority = GetPriorityClass(GetCurrentProcess());
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Test", 0, 0, 0, 900, 900, nullptr,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
#else
    QWidget hostWindow;
    auto *window = reinterpret_cast<void *>(hostWindow.winId());
#endif
    QVERIFY(window);
    {
      vst3::Processor processor;
      ready(processor.state()->input);
      auto &runtime = vst3::PluginRuntime::acquire(window);
      const auto api = unavailableDiscovery();
      auto *panel = runtime.open(processor.state(), api);
      auto timing = panel->timing();
      timing.lead = .75;
      panel->setTiming(timing);
      emit panel->timingChanged();
      const airplay::NetworkBinding binding{"unavailable-test", "192.0.2.10"};
      panel->setNetworkBinding(binding);
      emit panel->networkBindingChanged();
      runtime.close(processor.state()->id);
      panel = runtime.open(processor.state(), api);
      QCOMPARE(panel->timing().lead, .75);
      QCOMPARE(panel->networkBinding(), binding);
      panel->findChild<QTabWidget *>("receiverModes")->setCurrentIndex(1);
      panel->findChild<QLineEdit *>("manualFirst")->setText("127.0.0.1:7000");
      MemoryStream saved;
      QCOMPARE(processor.getState(&saved), kResultOk);
      saved.seek(0, IBStream::kIBSeekSet, nullptr);
      panel->setNetworkBinding({});
      emit panel->networkBindingChanged();
      QCOMPARE(processor.setState(&saved), kResultOk);
      runtime.pump();
      QCOMPARE(panel->networkBinding(), binding);
      QVERIFY(panel->findChild<QLineEdit *>("manualFirst")->text().isEmpty());
      QVERIFY(!panel->findChild<QPushButton *>("start")->isEnabled());
      QVERIFY(panel->grab().save(QCoreApplication::applicationDirPath() +
                                 "/VstStreamingPanel.png"));
      MemoryStream broken;
      QCOMPARE(processor.setState(&broken), kResultFalse);
      runtime.pump();
      runtime.close(processor.state()->id);
      panel = runtime.open(processor.state(), api);
      QVERIFY(panel->findChild<QLabel *>("receiverSummary")
                  ->text()
                  .contains("重新添加"));
      QVERIFY(!panel->findChild<QPushButton *>("start")->isEnabled());
      runtime.close(processor.state()->id);
#ifdef Q_OS_WIN
      QCOMPARE(GetPriorityClass(GetCurrentProcess()), priority);
#endif
    }
    QVERIFY(!vst3::PluginRuntime::exists());
    QCOMPARE(QCoreApplication::instance(), original);
#ifdef Q_OS_WIN
    DestroyWindow(window);
#endif
  }
#ifdef Q_OS_WIN
  void deleteDuringDiscovery() {
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Test", 0, 0, 0, 900, 900, nullptr,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
    QVERIFY(window);
    DNS_SERVICE_BROWSE_REQUEST request{};
    int cancellations = 0;
    airplay::DiscoveryApi api;
    api.browse = [&](auto *r, auto *) {
      request = *r;
      return DNS_STATUS(DNS_REQUEST_PENDING);
    };
    api.cancelBrowse = [&](auto *) {
      ++cancellations;
      QTimer::singleShot(5, [=] {
        request.pBrowseCallback(ERROR_CANCELLED, request.pQueryContext,
                                nullptr);
      });
      return DNS_STATUS(ERROR_SUCCESS);
    };
    {
      vst3::Processor processor;
      auto &runtime = vst3::PluginRuntime::acquire(window);
      auto *panel = runtime.open(processor.state(), api);
      QVERIFY(panel->discoveryBusy());
      runtime.close(processor.state()->id);
    }
    QCOMPARE(cancellations, 1);
    QVERIFY(!vst3::PluginRuntime::exists());
    DestroyWindow(window);
  }
#endif
  void multipleInstancesAndActiveUnload() {
#ifdef Q_OS_WIN
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Test", 0, 0, 0, 900, 900, nullptr,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
#else
    QWidget hostWindow;
    auto *window = reinterpret_cast<void *>(hostWindow.winId());
#endif
    QVERIFY(window);
    QTcpServer local;
    QVERIFY(local.listen(QHostAddress::LocalHost, 0));
    const auto api = unavailableDiscovery();
    {
      vst3::Processor first, second;
      ready(first.state()->input);
      ready(second.state()->input);
      auto &runtime = vst3::PluginRuntime::acquire(window);
      auto *one = runtime.open(first.state(), api);
      auto *two = runtime.open(second.state(), api);
      for (auto *panel : {one, two}) {
        panel->findChild<QTabWidget *>("receiverModes")->setCurrentIndex(1);
        panel->findChild<QLineEdit *>("manualFirst")
            ->setText(QString("127.0.0.1:%1").arg(local.serverPort()));
      }
      one->findChild<QPushButton *>("start")->click();
      QVERIFY(!one->findChild<QTabWidget *>("receiverModes")->isEnabled());
      two->findChild<QPushButton *>("start")->click();
      const auto labels = two->findChildren<QLabel *>();
      QVERIFY(std::any_of(labels.begin(), labels.end(), [](auto *label) {
        return label->text().contains("另一个 AirPlayQt");
      }));
      runtime.close(first.state()->id);
      one = runtime.open(first.state(), api);
      QVERIFY(!one->findChild<QTabWidget *>("receiverModes")->isEnabled());
      // Both components exit with an initialized network controller. Its
      // bounded shutdown must join before Qt/runtime destruction returns.
      runtime.close(first.state()->id);
      runtime.close(second.state()->id);
    }
    QVERIFY(!vst3::PluginRuntime::exists());
#ifdef Q_OS_WIN
    DestroyWindow(window);
#endif
  }
};
QTEST_MAIN(VstTests)
#include "Tests.moc"
