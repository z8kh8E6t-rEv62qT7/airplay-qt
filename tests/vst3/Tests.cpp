#include "airplay/Crypto.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/alignedalloc.h"
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
      QVERIFY(input.fault() != vst3::InputFault::None);
      QVERIFY(stream.queue->fault.load() >=
              101); // Network observes it without a UI pump.
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
    const auto priority = GetPriorityClass(GetCurrentProcess());
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Test", 0, 0, 0, 900, 900, nullptr,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
    QVERIFY(window);
    {
      vst3::Processor processor;
      ready(processor.state()->input);
      auto &runtime = vst3::PluginRuntime::acquire(window);
      airplay::DiscoveryApi api;
      api.browse = [](auto *, auto *) {
        return DNS_STATUS(ERROR_NOT_SUPPORTED);
      };
      auto *panel = runtime.open(processor.state(), api);
      auto timing = panel->timing();
      timing.lead = .75;
      emit panel->timingChanged(timing);
      runtime.close(processor.state()->id);
      panel = runtime.open(processor.state(), api);
      QCOMPARE(panel->timing().lead, .75);
      panel->findChild<QTabWidget *>("receiverModes")->setCurrentIndex(1);
      panel->findChild<QLineEdit *>("manualFirst")->setText("127.0.0.1:7000");
      MemoryStream saved;
      QCOMPARE(processor.getState(&saved), kResultOk);
      saved.seek(0, IBStream::kIBSeekSet, nullptr);
      QCOMPARE(processor.setState(&saved), kResultOk);
      runtime.pump();
      QVERIFY(panel->findChild<QLineEdit *>("manualFirst")->text().isEmpty());
      QVERIFY(!panel->findChild<QPushButton *>("start")->isEnabled());
      QVERIFY(panel->grab().save(QCoreApplication::applicationDirPath() +
                                 "/VstStreamingPanel.png"));
      runtime.close(processor.state()->id);
      QCOMPARE(GetPriorityClass(GetCurrentProcess()), priority);
    }
    QVERIFY(!vst3::PluginRuntime::exists());
    QCOMPARE(QCoreApplication::instance(), original);
    DestroyWindow(window);
  }
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
  void multipleInstancesAndActiveUnload() {
    HWND window =
        CreateWindowExW(0, L"STATIC", L"Test", 0, 0, 0, 900, 900, nullptr,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
    QVERIFY(window);
    QTcpServer local;
    QVERIFY(local.listen(QHostAddress::LocalHost, 0));
    airplay::DiscoveryApi api;
    api.browse = [](auto *, auto *) { return DNS_STATUS(ERROR_NOT_SUPPORTED); };
    {
      vst3::Processor first, second;
      ready(first.state()->input);
      ready(second.state()->input);
      auto &runtime = vst3::PluginRuntime::acquire(window);
      auto *one = runtime.open(first.state(), api);
      auto *two = runtime.open(second.state(), api);
      const QList<airplay::ReceiverEndpoint> endpoints{
          {QHostAddress::LocalHost, local.serverPort()}};
      emit one->startRequested(app::Timing{}, endpoints);
      QVERIFY(!one->findChild<QTabWidget *>("receiverModes")->isEnabled());
      emit two->startRequested(app::Timing{}, endpoints);
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
    DestroyWindow(window);
  }
};
QTEST_MAIN(VstTests)
#include "Tests.moc"
