#include "audio/BluezCatalog.h"
#include "audio/PipeWireBuffer.h"
#include "audio/PipeWireCapture.h"
#include "audio/PipeWireCatalog.h"
#include "audio/PipeWireRecovery.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <cstring>

using namespace audio;
class Process : public QProcess {
public:
  ~Process() override {
    if (state() != NotRunning) { terminate(); if (!waitForFinished(2000)) { kill(); waitForFinished(); } }
  }
};
class Tests : public QObject {
  Q_OBJECT
private slots:
  void recoveryTracksTargetsAndElapsedTime() {
    PipeWireSource source{"bluez:adapter/phone", "Phone", "50", "aac", 10, 0, 0};
    PipeWireRetry retry;
    const auto now = PipeWireRetry::Clock::now();
    QVERIFY(!retry.waiting(source, now));
    retry.failed(source, now);
    auto metadata = source;
    metadata.rate = 44100; metadata.channels = 2; metadata.name = "Renamed phone";
    QVERIFY(samePipeWireTarget(source, metadata));
    QVERIFY(retry.waiting(metadata, now + std::chrono::milliseconds(1999)));
    QVERIFY(!retry.waiting(metadata, now + std::chrono::seconds(2)));
    metadata.rate = 48000;
    QVERIFY(samePipeWireTarget(source, metadata));
    // A replacement node or codec can recover immediately, even if the old
    // failed source never disappeared from a catalog snapshot.
    auto replaced = metadata;
    replaced.serial = "51";
    QVERIFY(!retry.waiting(replaced, now));
    replaced = metadata; replaced.node = 11;
    QVERIFY(!retry.waiting(replaced, now));
    replaced = metadata; replaced.codec = "sbc";
    QVERIFY(!retry.waiting(replaced, now));
    replaced = metadata; replaced.id = "bluez:adapter/other";
    QVERIFY(!retry.waiting(replaced, now));
    retry.clear(); // Observed disappearance, followed by the same target.
    QVERIFY(!retry.waiting(source, now));
  }
  void streamStateTransitions_data() {
    QTest::addColumn<int>("previous");
    QTest::addColumn<int>("next");
    QTest::addColumn<bool>("invalidates");
    QTest::newRow("connect") << int(PW_STREAM_STATE_UNCONNECTED) << int(PW_STREAM_STATE_CONNECTING) << false;
    QTest::newRow("negotiate") << int(PW_STREAM_STATE_CONNECTING) << int(PW_STREAM_STATE_PAUSED) << false;
    QTest::newRow("pause") << int(PW_STREAM_STATE_STREAMING) << int(PW_STREAM_STATE_PAUSED) << false;
    QTest::newRow("resume") << int(PW_STREAM_STATE_PAUSED) << int(PW_STREAM_STATE_STREAMING) << false;
    QTest::newRow("error") << int(PW_STREAM_STATE_STREAMING) << int(PW_STREAM_STATE_ERROR) << true;
    QTest::newRow("disconnect") << int(PW_STREAM_STATE_PAUSED) << int(PW_STREAM_STATE_UNCONNECTED) << true;
    QTest::newRow("already-invalid") << int(PW_STREAM_STATE_ERROR) << int(PW_STREAM_STATE_UNCONNECTED) << false;
  }
  void streamStateTransitions() {
    QFETCH(int, previous); QFETCH(int, next); QFETCH(bool, invalidates);
    QCOMPARE(pipeWireStateInvalidates(pw_stream_state(previous), pw_stream_state(next)), invalidates);
  }
  void channelMapping_data() {
    QTest::addColumn<int>("left");
    QTest::addColumn<int>("right");
    QTest::newRow("normal") << 0 << 1;
    QTest::newRow("reversed") << 1 << 0;
    QTest::newRow("same-first") << 0 << 0;
    QTest::newRow("same-second") << 1 << 1;
  }
  void channelMapping() {
    QFETCH(int, left);
    QFETCH(int, right);
    CaptureQueue queue(352, 1408, 1408, 8);
    PipeWireBuffer buffer(queue, left, right);
    std::array<float, 704> samples{};
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = float(i);
    QVERIFY(buffer.append(samples.data(), 127));
    QVERIFY(buffer.append(samples.data() + 254, 225));
    std::span<const std::byte> l, r;
    QVERIFY(queue.peek(l, r));
    for (size_t i = 0; i < 352; ++i) {
      float a, b;
      std::memcpy(&a, l.data() + i * sizeof(float), sizeof(float));
      std::memcpy(&b, r.data() + i * sizeof(float), sizeof(float));
      QCOMPARE(a, samples[2 * i + left]);
      QCOMPARE(b, samples[2 * i + right]);
    }
  }
  void variableBlocksAndDiscontinuities() {
    CaptureQueue queue(352, 1408, 1408, 8);
    PipeWireBuffer buffer(queue, 1, 0);
    std::array<float, 2048> samples;
    for (size_t i = 0; i < samples.size() / 2; ++i) { samples[i*2] = .1f; samples[i*2+1] = -.2f; }
    QVERIFY(buffer.append(samples.data(), 127));
    QCOMPARE(queue.queuedFrames(), 0ULL);
    QVERIFY(buffer.append(samples.data(), 577));
    QCOMPARE(queue.queuedFrames(), 704ULL);
    std::span<const std::byte> l, r;
    uint64_t epoch;
    QVERIFY(queue.peek(l, r, &epoch));
    float value;
    std::memcpy(&value, l.data(), sizeof(value)); QCOMPARE(value, -.2f);
    std::memcpy(&value, r.data(), sizeof(value)); QCOMPARE(value, .1f);
    queue.pop(); queue.pop();
    QVERIFY(buffer.append(samples.data(), 351));
    queue.generation.fetch_add(1);
    QVERIFY(buffer.append(samples.data(), 1));
    QCOMPARE(queue.queuedFrames(), 0ULL);
    QVERIFY(buffer.append(samples.data(), 351));
    QVERIFY(queue.peek(l, r, &epoch));
    QCOMPARE(epoch, 1ULL);
  }
  void overflowAndInvalidSamplesInvalidateInput() {
    CaptureQueue queue(352, 1408, 1408, 2);
    PipeWireBuffer buffer(queue, 0, 1);
    std::array<float, 704> samples{};
    QVERIFY(buffer.append(samples.data(), 352));
    QVERIFY(buffer.append(samples.data(), 352));
    QVERIFY(!buffer.append(samples.data(), 352));
    QCOMPARE(queue.fault.load(), 0);
    QCOMPARE(queue.generation.load(), 1ULL);
    queue.pop(); queue.pop();
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    QVERIFY(!buffer.append(samples.data(), 352));
    QCOMPARE(queue.generation.load(), 2ULL);
    QCOMPARE(queue.queuedFrames(), 0ULL);
  }
  void adaptiveClockConverges_data() {
    QTest::addColumn<double>("ppm");
    QTest::newRow("fast") << 500.;
    QTest::newRow("slow") << -500.;
  }
  void adaptiveClockConverges() {
    QFETCH(double, ppm);
    CaptureQueue queue(352, 1408, 1408, 8);
    PipeWireBuffer buffer(queue, 0, 1);
    constexpr double target = 1764;
    double backlog = target, correction = 1.;
    // Two simulated hours; model the documented resampler output/input ratio.
    for (int i = 0; i < int(7200 * 44100 / 352); ++i) {
      correction = buffer.rate(backlog, target, 352, true);
      QVERIFY(correction >= .999 && correction <= 1.001);
      backlog += 352 * ((1. + ppm / 1e6) * correction - 1.);
      QVERIFY(backlog > 352 && backlog < 4410);
    }
    QVERIFY(std::abs(backlog - target) < 1.);
    QVERIFY(std::abs(correction * (1. + ppm / 1e6) - 1.) < 1e-7);
    QCOMPARE(buffer.rate(backlog, target, 352, false), 1.);
  }
  void bluetoothIdentitySurvivesObjectPathChanges() {
    DBusObjects objects;
    const QDBusObjectPath adapter("/org/bluez/hci0"), phone("/org/bluez/hci0/dev_AA");
    objects[adapter]["org.bluez.Adapter1"] = {{"Address", "00:11:22:33:44:55"}};
    objects[phone]["org.bluez.Device1"] = {
      {"Address", "AA:BB:CC:DD:EE:FF"}, {"Alias", "Phone"}, {"Paired", true},
      {"Connected", false}, {"Adapter", QVariant::fromValue(adapter)},
      {"UUIDs", QStringList{"0000110a-0000-1000-8000-00805f9b34fb"}}};
    const auto first = BluezCatalog::sourcesFromObjects(objects);
    QCOMPARE(first.size(), 1);
    QVERIFY(!first[0].connected);
    const QDBusObjectPath nextAdapter("/org/bluez/hci2"), nextPhone("/org/bluez/hci2/dev_AA");
    objects[nextAdapter] = objects.take(adapter);
    objects[nextPhone] = objects.take(phone);
    objects[nextPhone]["org.bluez.Device1"]["Adapter"] = QVariant::fromValue(nextAdapter);
    objects[nextPhone]["org.bluez.Device1"]["Connected"] = true;
    const auto second = BluezCatalog::sourcesFromObjects(objects);
    QCOMPARE(second[0].id, first[0].id);
    QVERIFY(second[0].connected);
    objects[nextPhone]["org.bluez.Device1"]["UUIDs"] = QStringList{"0000110b-0000-1000-8000-00805f9b34fb"};
    QVERIFY(BluezCatalog::sourcesFromObjects(objects).isEmpty());
  }
  void bluetoothSourcesDoNotFallbackOrUseMicrophones() {
    QList<BluetoothSource> phones{{"bluez:adapter/A", "/org/bluez/hci0/dev_A", "A", true},
                                  {"bluez:adapter/B", "/org/bluez/hci0/dev_B", "B", true}};
    QVariantMap props{{"node.name", "bluez_input.A.1"}, {"object.serial", "50"},
      {"api.bluez5.path", phones[0].path}, {"api.bluez5.profile", "a2dp-source"},
      {"api.bluez5.codec", "aac"}};
    auto source = pipeWireSource(props, 10, 48000, 2, phones);
    QVERIFY(source); QCOMPARE(source->id, phones[0].id); QCOMPARE(source->codec, "aac");
    QCOMPARE(source->rate, 48000u);
    props["object.serial"] = "80";
    QCOMPARE(pipeWireSource(props, 70, 44100, 2, phones)->id, source->id);
    phones[0].connected = false;
    QVERIFY(!pipeWireSource(props, 70, 44100, 2, phones));
    phones[0].connected = true;
    props["api.bluez5.profile"] = "headset-audio-gateway";
    QVERIFY(!pipeWireSource(props, 70, 16000, 1, phones));
    props["api.bluez5.profile"] = "a2dp-source";
    props["api.bluez5.codec"] = "sbc";
    QCOMPARE(pipeWireSource(props, 70, 0, 2, phones)->rate, 0u);
    QCOMPARE(pipeWireSource(props, 70, 0, 2, phones)->codec, "sbc");
  }
  void nativeServerRecovery_data() {
    QTest::addColumn<int>("sourceRate");
    QTest::newRow("44100") << 44100;
    QTest::newRow("48000") << 48000;
  }
  void nativeServerRecovery() {
    QFETCH(int, sourceRate);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.path();
    QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    auto write = [&](const QString &path, const QByteArray &data) {
      QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    };
    const auto configuration = QByteArray(R"(
context.properties = { core.daemon = true core.name = airplayqt-test default.clock.rate = 48000 }
context.spa-libs = {
  support.* = support/libspa-support
  audio.convert.* = audioconvert/libspa-audioconvert
  audiotestsrc = audiotestsrc/libspa-audiotestsrc
}
context.modules = [
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-client-node }
  { name = libpipewire-module-adapter }
  { name = libpipewire-module-link-factory }
  { name = libpipewire-module-spa-node-factory }
  { name = libpipewire-module-metadata }
  { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
  { factory = metadata args = { metadata.name = default } }
  { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 } }
  { factory = adapter args = { factory.name = audiotestsrc node.name = test-source node.description = TestSource media.class = Audio/Source audio.format = F32 audio.rate = 48000 audio.channels = 2 audio.position = [ FL FR ] } }
]
)").replace("48000", QByteArray::number(sourceRate));
    QVERIFY(write(root + "/pipewire.conf", configuration));
    QVERIFY(QFile::copy("/usr/share/wireplumber/wireplumber.conf", root + "/wireplumber.conf"));
    QVERIFY(QDir().mkpath(root + "/wireplumber.conf.d"));
    QVERIFY(write(root + "/wireplumber.conf.d/test.conf", R"(
wireplumber.profiles = { main = { monitor.alsa = disabled monitor.bluez = disabled monitor.bluez-midi = disabled monitor.v4l2 = disabled monitor.libcamera = disabled } }
)"));
    const auto oldRuntime = qgetenv("XDG_RUNTIME_DIR"), oldRemote = qgetenv("PIPEWIRE_REMOTE");
    qputenv("XDG_RUNTIME_DIR", root.toUtf8()); qputenv("PIPEWIRE_REMOTE", "airplayqt-test");
    auto restore = qScopeGuard([&] {
      if (oldRuntime.isNull()) qunsetenv("XDG_RUNTIME_DIR"); else qputenv("XDG_RUNTIME_DIR", oldRuntime);
      if (oldRemote.isNull()) qunsetenv("PIPEWIRE_REMOTE"); else qputenv("PIPEWIRE_REMOTE", oldRemote);
    });
    Process server, policy;
    server.setProcessChannelMode(QProcess::MergedChannels);
    policy.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PIPEWIRE_CONFIG_DIR", root);
    server.setProcessEnvironment(environment);
    auto startServer = [&] { server.start("pipewire", {"-c", "pipewire.conf"}); return server.waitForStarted(); };
    QVERIFY(startServer());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(root + "/airplayqt-test"), 3000);
    environment.remove("PIPEWIRE_CONFIG_DIR");
    environment.insert("WIREPLUMBER_CONFIG_DIR", root);
    policy.setProcessEnvironment(environment);
    policy.start("wireplumber", {});
    QVERIFY(policy.waitForStarted());
    // Each data row uses a different server/socket. Do not inherit a catalog
    // snapshot (or a pending server-loss event) from the previous row.
    auto *catalog = &PipeWireCatalog::instance();
    const auto disposeCatalog = qScopeGuard([&] { delete catalog; });
    auto available = [] {
      const auto devices = inputDevices();
      return std::any_of(devices.begin(), devices.end(), [](const auto &d) { return d.id == "pipewire:test-source"; });
    };
    QTRY_VERIFY_WITH_TIMEOUT(available(), 5000);
    PipeWireCapture capture;
    QSignalSpy logs(&capture, &InputCapture::log);
    QCOMPARE(capture.open("pipewire:test-source", nullptr).size(), 2);
    auto stream = capture.prepare(0, 1, 1.);
    capture.start();
    QTRY_VERIFY_WITH_TIMEOUT(stream.queue->capturedFrames() >= 352, 6000);
    std::span<const std::byte> l, r;
    uint64_t epoch;
    QVERIFY(stream.queue->peek(l, r, &epoch));
    QCOMPARE(l.size(), size_t(1408));
    bool nonzero = false;
    for (size_t offset = 0; offset < l.size(); offset += sizeof(float)) {
      float sample;
      std::memcpy(&sample, l.data() + offset, sizeof(sample));
      QVERIFY(std::isfinite(sample));
      nonzero |= sample != 0.f;
    }
    QVERIFY(nonzero);
    const auto logged = [&](const QString &text) {
      return std::any_of(logs.begin(), logs.end(), [&](const auto &entry) {
        return i18n::Message(entry[0].toJsonArray()).render().contains(text);
      });
    };
    QTRY_VERIFY_WITH_TIMEOUT(logged("obtained=44100 Hz, 2 channels, Float32"), 3000);
    // Drain at device speed so the test itself does not cause queue overflow.
    // Let catalog format updates settle: metadata must not reopen the stream.
    const auto initialEpoch = stream.queue->generation.load();
    QElapsedTimer stable;
    stable.start();
    while (stable.elapsed() < 400) {
      while (stream.queue->peek(l, r)) stream.queue->pop();
      QTest::qWait(5);
    }
    QCOMPARE(stream.queue->generation.load(), initialEpoch);
    QVERIFY(logged("source PCM rate=" + QString::number(sourceRate)));
    const auto initialNegotiations = std::count_if(logs.begin(), logs.end(), [](const auto &entry) {
      return i18n::Message(entry[0].toJsonArray()).render().contains("Negotiating ");
    });
    QCOMPARE(initialNegotiations, 1);
    const auto oldEpoch = stream.queue->generation.load();
    server.terminate(); QVERIFY(server.waitForFinished(3000));
    QTRY_VERIFY_WITH_TIMEOUT(stream.queue->generation.load() > oldEpoch, 3000);
    while (stream.queue->peek(l, r)) stream.queue->pop();
    const auto before = stream.queue->capturedFrames();
    // WirePlumber exits on server loss; restart it in the isolated environment.
    policy.terminate(); policy.waitForFinished(3000);
    QVERIFY(startServer());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(root + "/airplayqt-test"), 3000);
    policy.start("wireplumber", {}); QVERIFY(policy.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(stream.queue->capturedFrames() > before, 8000);
    QVERIFY(capture.stop().isEmpty());
    const auto stopped = stream.queue->capturedFrames();
    QTest::qWait(150);
    QCOMPARE(stream.queue->capturedFrames(), stopped);
    auto restarted = capture.prepare(1, 0, 1.);
    capture.start();
    QTRY_VERIFY_WITH_TIMEOUT(restarted.queue->capturedFrames() >= 352, 6000);
    QCOMPARE(stream.queue->capturedFrames(), stopped);
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(capture.stop().isEmpty());
    QVERIFY(!logs.isEmpty());
  }
};
QTEST_GUILESS_MAIN(Tests)
#include "PipeWireTests.moc"
