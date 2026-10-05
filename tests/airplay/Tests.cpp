#include "TestReceiver.h"
#include "airplay/AirPlaySession.h"
#include "airplay/EventChannel.h"
#include "airplay/NowPlaying.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTcpServer>
#include <QtTest>
#include <openssl/bn.h>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <QSocketNotifier>
#include <QtEndian>
#ifdef Q_OS_MACOS
#include <dns_sd.h>
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using namespace airplay;
namespace {
using test::Receiver;
QVariantMap mediaCommand(const QString &value) {
  return {{"type", "sendMediaRemoteCommand"}, {"value", value}};
}
QByteArray dacpRequest(quint16 port, const QByteArray &path, const QByteArray &token) {
  QTcpSocket socket;
  socket.connectToHost(QHostAddress::LocalHost, port);
  if (!socket.waitForConnected(1000))
    return {};
  socket.write("GET /ctrl-int/1/" + path +
               " HTTP/1.1\r\nActive-Remote: " + token + "\r\n\r\n");
  QByteArray response;
  QElapsedTimer timer;
  timer.start();
  while (socket.state() != QAbstractSocket::UnconnectedState && timer.elapsed() < 1000) {
    QTest::qWait(1);
    response += socket.readAll();
  }
  return response;
}
struct SessionFixture {
  Receiver left{"left"}, right{"right"};
  app::Timing timing;
  audio::CaptureStream stream;
  SessionEnvironment environment;
  QList<ReceiverEndpoint> endpoints;
  QTimer input;
  QElapsedTimer captureElapsed;
  uint64_t captureRate = 44100;
  std::array<int16_t, 176> l{}, r{};
  SessionFixture() {
    timing.settleMs = 0;
    timing.packetSamples = 352;
    timing.prebufferSamples = 512;
    timing.backlogSamples = 32768;
    timing.lateMs = 500;
    timing.requestTimeoutMs = 200;
    timing.connectTimeoutMs = 200;
    timing.teardownTimeoutMs = 100;
    stream = {std::make_shared<audio::CaptureQueue>(176, 352, 352, 128),
              audio::format(16), audio::format(16), 176};
    endpoints = {ReceiverEndpoint{QHostAddress::LocalHost, left.port()},
                 ReceiverEndpoint{QHostAddress::LocalHost, right.port()}};
    environment.startClock = [] {};
    environment.stopClock = [] {};
    environment.advertiseRemote = false;
    for (size_t i = 0; i < l.size(); ++i) {
      l[i] = int16_t(i + 1);
      r[i] = -int16_t(i + 1);
    }
    input.setTimerType(Qt::PreciseTimer);
    input.setInterval(4);
    QObject::connect(&input, &QTimer::timeout, &input, [this] {
      // QTest processes events in batches; model a 44100 Hz device clock,
      // rather than assuming every 4 ms timer notification is delivered.
      const auto target =
          uint64_t(captureElapsed.nsecsElapsed()) * captureRate / 1000000000;
      while (stream.queue->capturedFrames() + 176 <= target) {
        if (!stream.queue->push(l.data(), r.data())) {
          stream.queue->fault = 2;
          break;
        }
      }
    });
  }
  void attach(AirPlaySession &session) {
    QObject::connect(&session, &AirPlaySession::startCapture, &session,
                     [this, &session] {
                       captureElapsed.start();
                       input.start();
                       session.captureStarted();
                     });
    QObject::connect(&session, &AirPlaySession::stopCapture, &input,
                     &QTimer::stop);
  }
};
} // namespace
class ProtocolTests : public QObject {
  Q_OBJECT
private slots:
  void nowPlayingRejectsOldFieldsAndTypes() {
    for (auto state : {PlaybackState::Playing, PlaybackState::Paused, PlaybackState::Stopped}) {
      const auto command = plistDecode(plistEncode(nowPlayingInfo(state))).toMap();
      QVERIFY(Receiver::validNowPlaying(command));
      QCOMPARE(command["params"].toMap()["params"].toMap()
                   ["kMRMediaRemoteNowPlayingInfoPlaybackRate"].toDouble(),
               state == PlaybackState::Playing ? 1. : 0.);
      QCOMPARE(playbackState(state)["params"].toMap()["mrPlaybackState"].toInt(), int(state));
    }
    auto command = nowPlayingInfo(PlaybackState::Playing);
    auto params = command["params"].toMap();
    auto info = params["params"].toMap();
    info["Title"] = info.take("kMRMediaRemoteNowPlayingInfoTitle");
    params["params"] = info;
    command["params"] = params;
    QVERIFY(!Receiver::validNowPlaying(command));
    info["kMRMediaRemoteNowPlayingInfoTitle"] = info.take("Title");
    info["kMRMediaRemoteNowPlayingInfoMediaType"] = 1;
    params["params"] = info;
    command["params"] = params;
    QVERIFY(!Receiver::validNowPlaying(command));
  }
  void remoteControlLifecycle() {
    RemoteControl remote;
    QSignalSpy changed(&remote, &RemoteControl::playbackChanged);
    QSignalSpy ready(&remote, &RemoteControl::ready);
    remote.handleEventCommand(mediaCommand("paus"));
    QVERIFY(changed.isEmpty());
    remote.start("test", QHostAddress::LocalHost, {}, {QHostAddress::LocalHost}, false, 101);
    remote.stop();
    remote.start("test", QHostAddress::LocalHost, {}, {QHostAddress::LocalHost}, false, 102);
    QTRY_COMPARE(ready.size(), 1); // Old deferred ready cannot affect the new session.
    QCOMPARE(remote.playbackState(), PlaybackState::Playing);
    QVERIFY(dacpRequest(remote.port(), "pause", "102").contains("503"));
    remote.setEnabled(true);
    QVERIFY(dacpRequest(remote.port(), "pause", "101").contains("403"));
    QVERIFY(dacpRequest(remote.port(), "pause?unexpected=1", "102").contains("400"));
    QVERIFY(dacpRequest(remote.port(), "stop", "102").contains("501"));
    remote.handleEventCommand({{"type", "sendMediaRemoteCommand"}, {"value", QByteArray("paus")}});
    remote.handleEventCommand({{"type", "updateInfo"}, {"value", "paus"}});
    remote.handleEventCommand(mediaCommand("nitm"));
    QVERIFY(changed.isEmpty());
    for (const auto &action : {QByteArray("pause"), QByteArray("pause"),
                               QByteArray("play"), QByteArray("play"),
                               QByteArray("playpause"), QByteArray("playpause"),
                               QByteArray("pause"), QByteArray("playresume")})
      QVERIFY(dacpRequest(remote.port(), action, "102").contains("204"));
    QCOMPARE(changed.size(), 6);
    QCOMPARE(remote.playbackState(), PlaybackState::Playing);
    remote.stop();
    remote.handleEventCommand(mediaCommand("paus"));
    QCOMPARE(changed.size(), 6);
    QCOMPARE(remote.playbackState(), PlaybackState::Stopped);
    QCOMPARE(remote.port(), quint16(0));
  }
  void protocolPausePreservesAudio_data() {
    QTest::addColumn<bool>("stereo");
    QTest::newRow("single") << false;
    QTest::newRow("stereo") << true;
  }
  void protocolPausePreservesAudio() {
    QFETCH(bool, stereo);
    SessionFixture f;
    if (!stereo) f.endpoints.removeLast();
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    QSignalSpy capture(&session, &AirPlaySession::startCapture);
    QSignalSpy stoppedCapture(&session, &AirPlaySession::stopCapture);
    QSignalSpy streaming(&session, &AirPlaySession::streamingChanged);
    QSignalSpy volume(&session, &AirPlaySession::volumeApplied);
    session.start();
    QTRY_VERIFY(f.left.packets.size() >= 4);
    auto *remote = session.findChild<RemoteControl *>();
    QVERIFY(remote);
    const auto statusCount = streaming.size();
    f.left.sendEvent(mediaCommand("paus"), 11);
    QTRY_COMPARE(f.left.protocolState, 2);
    if (stereo) QTRY_COMPARE(f.right.protocolState, 2);
    QTRY_COMPARE(f.left.eventResponses.size(), 1);
    QCOMPARE(f.left.eventResponses.last().status, 200);
    QCOMPARE(f.left.eventResponses.last().headers["cseq"], QByteArray("11"));
    const auto commands = f.left.commands.size();
    f.left.sendEvent(mediaCommand("paus"), 12);
    f.left.sendEvent(mediaCommand("nitm"), 13);
    QTRY_COMPARE(f.left.eventResponses.size(), 3);
    QCOMPARE(f.left.commands.size(), commands);
    const auto pausedAt = f.left.packets.size();
    QVERIFY(dacpRequest(remote->port(), "devicevolume=-21", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(volume.last()[0].toDouble(), -21.);
    if (stereo) QCOMPARE(f.right.volumes.last(), -21.);
    QTRY_VERIFY(f.left.packets.size() > pausedAt + 8);
    QCOMPARE(f.left.protocolState, 2);
    QCOMPARE(capture.size(), 1);
    QVERIFY(stoppedCapture.isEmpty());
    QCOMPARE(streaming.size(), statusCount);
    std::vector<int16_t> expected;
    for (int i = 0; i < f.timing.packetSamples; ++i) {
      expected.push_back(f.l[size_t(i) % f.l.size()]);
      expected.push_back(f.r[size_t(i) % f.r.size()]);
    }
    const auto first = f.left.packets.first();
    for (qsizetype i = 0; i < f.left.packets.size(); ++i) {
      const auto &packet = f.left.packets[i];
      QCOMPARE(uint16_t(readBe(packet, 2, 2)), uint16_t(readBe(first, 2, 2) + i));
      QCOMPARE(uint32_t(readBe(packet, 4, 4)), uint32_t(readBe(first, 4, 4) + i * 352));
      // Packet nonce is the little-endian monotonically increasing counter.
      quint64 nonce = 0;
      for (int j = 7; j >= 0; --j) nonce = (nonce << 8) | uint8_t(packet.right(8)[j]);
      QCOMPARE(nonce, quint64(i));
      QCOMPARE(unseal(f.left.srp.key.left(32), QByteArray(4, '\0') + packet.right(8),
                      packet.mid(12, packet.size() - 20), packet.mid(4, 8)), alac(expected));
    }
    f.left.sendEvent(mediaCommand("play"), 14);
    QTRY_COMPARE(f.left.protocolState, 1);
    f.left.sendEvent(mediaCommand("plps"), 15);
    QTRY_COMPARE(f.left.protocolState, 2);
    f.left.sendEvent(mediaCommand("plps"), 16);
    QTRY_COMPARE(f.left.protocolState, 1);
    if (stereo) QTRY_COMPARE(f.right.protocolState, 1);
    QTRY_COMPARE(f.left.eventResponses.size(), 6);
    QVERIFY(dacpRequest(remote->port(), "pause", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(f.left.protocolState, 2);
    if (stereo) QTRY_COMPARE(f.right.protocolState, 2);
    QVERIFY(dacpRequest(remote->port(), "playpause", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(f.left.protocolState, 1);
    if (stereo) QTRY_COMPARE(f.right.protocolState, 1);
    QTest::qWait(10); // Allow the receiver's last RTSP acknowledgment to arrive.
    QVERIFY(done.isEmpty());
    QVERIFY(f.left.error.isEmpty());
    QVERIFY(f.right.error.isEmpty());
    session.stop();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(f.left.protocolState, 3);
    if (stereo) QCOMPARE(f.right.protocolState, 3);
  }
  void controlUpdatesCoalesceAcrossReceivers() {
    SessionFixture f;
    f.timing.requestTimeoutMs = 500;
    f.timing.keepAliveMs = 1000;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    QSignalSpy applied(&session, &AirPlaySession::volumeApplied);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    auto *remote = session.findChild<RemoteControl *>();
    QVERIFY(remote);
    f.left.commandDelay = 10;
    f.right.commandDelay = 60;
    f.left.sendEvent(mediaCommand("paus"));
    QTRY_COMPARE(f.right.commands.size(), 6); // Paused info is in flight.
    f.left.sendEvent(mediaCommand("play"));
    f.left.sendEvent(mediaCommand("paus"));
    f.left.sendEvent(mediaCommand("play"));
    session.volume(-22);
    session.volume(-18);
    QTRY_COMPARE(f.left.commands.size(), 9);
    QTRY_COMPARE(f.right.commands.size(), 9);
    QTRY_COMPARE(applied.last()[0].toDouble(), -18.);
    QCOMPARE(f.left.protocolState, 1);
    QCOMPARE(f.right.protocolState, 1);
    QCOMPARE(f.left.volumes.last(), -18.);
    QCOMPARE(f.right.volumes.last(), -18.);
    QVERIFY(f.left.error.isEmpty());
    QVERIFY(f.right.error.isEmpty());
    QTest::qWait(100);
    QCOMPARE(f.left.commands.size(), 9);
    QCOMPARE(f.right.commands.size(), 9);
    QVERIFY(done.isEmpty());
    session.stop();
    QTRY_COMPARE(done.size(), 1);
  }
  void runtimeMetadataFailure_data() {
    QTest::addColumn<QString>("type");
    QTest::addColumn<bool>("timeout");
    for (const auto &type : {QString("updateMRNowPlayingInfo"), QString("updateMRPlaybackState")}) {
      QTest::newRow(qPrintable(type + "-reject")) << type << false;
      QTest::newRow(qPrintable(type + "-timeout")) << type << true;
    }
  }
  void runtimeMetadataFailure() {
    QFETCH(QString, type);
    QFETCH(bool, timeout);
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    if (timeout) f.right.holdCommandType = type;
    else f.right.rejectCommandType = type;
    f.left.sendEvent(mediaCommand("paus"));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::Failure));
    QVERIFY(!done[0][0].toJsonArray().isEmpty());
    auto *remote = session.findChild<RemoteControl *>();
    QVERIFY(remote);
    QCOMPARE(remote->port(), quint16(0));
    const auto count = f.left.packets.size();
    QTest::qWait(20);
    QCOMPARE(f.left.packets.size(), count);
  }
  void stoppingDuringControlUpdate() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    f.right.holdCommandType = "updateMRNowPlayingInfo";
    f.left.sendEvent(mediaCommand("paus"));
    QTRY_COMPARE(f.right.commands.size(), 6);
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::Stopped));
    auto *remote = session.findChild<RemoteControl *>();
    remote->handleEventCommand(mediaCommand("play"));
    QCOMPARE(remote->playbackState(), PlaybackState::Stopped);
    QCOMPARE(remote->port(), quint16(0));
  }
  void replacementSessionRejectsOldControl() {
    SessionFixture oldFixture;
    AirPlaySession oldSession(oldFixture.timing, oldFixture.stream, oldFixture.endpoints,
                              nullptr, oldFixture.environment);
    oldFixture.attach(oldSession);
    QSignalSpy oldDone(&oldSession, &AirPlaySession::finished);
    oldSession.start();
    QTRY_VERIFY(!oldFixture.left.packets.isEmpty());
    oldFixture.left.sendEvent(mediaCommand("paus"));
    QTRY_COMPARE(oldFixture.left.protocolState, 2);
    const auto oldToken = oldFixture.left.activeRemote;
    oldSession.stop();
    QTRY_COMPARE(oldDone.size(), 1);
    auto *oldRemote = oldSession.findChild<RemoteControl *>();
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    auto *remote = session.findChild<RemoteControl *>();
    QVERIFY(remote && oldRemote);
    // Simulate an already queued delivery from the old authenticated event channel.
    oldRemote->handleEventCommand(mediaCommand("plps"));
    QCOMPARE(oldRemote->playbackState(), PlaybackState::Stopped);
    QCOMPARE(remote->playbackState(), PlaybackState::Playing);
    QVERIFY(dacpRequest(remote->port(), "pause", oldToken).contains("403"));
    QCOMPARE(f.left.protocolState, 1);
    QCOMPARE(f.right.protocolState, 1);
    QCOMPARE(f.left.commands.size(), 5);
    QCOMPARE(f.right.commands.size(), 5);
    QVERIFY(done.isEmpty());
    session.stop();
    QTRY_COMPARE(done.size(), 1);
  }
  void inputVolumeUsesLatestTargetAndIgnoresInactiveSessions() {
    SessionFixture f;
    f.stream.gapPolicy = audio::GapPolicy::Silence;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    connect(&session, &AirPlaySession::startCapture, &session, &AirPlaySession::captureStarted);
    QSignalSpy done(&session, &AirPlaySession::finished);
    QSignalSpy applied(&session, &AirPlaySession::volumeApplied);
    session.inputVolume(-10);
    session.inputVolumeStep(1);
    session.start();
    session.inputVolume(-5);
    session.inputVolumeStep(-1);
    QTRY_VERIFY(!f.left.packets.isEmpty());
    QCOMPARE(f.left.volumes.first(), -30.);
    session.inputVolume(-25);
    for (int i = 0; i < 8; ++i) session.inputVolumeStep(1);
    session.inputVolumeStep(-1);
    session.inputVolumeStep(-1);
    QTRY_COMPARE(applied.last()[0].toDouble(), -19.);
    QCOMPARE(f.left.volumes.last(), -19.);
    QCOMPARE(f.right.volumes.last(), -19.);
    // Unlike a DACP echo, a new input request can intentionally return to an
    // earlier in-flight value.
    session.inputVolume(-25);
    session.inputVolume(-20);
    session.inputVolume(-25);
    QTRY_COMPARE(applied.last()[0].toDouble(), -25.);
    session.inputVolume(-144);
    QTRY_COMPARE(applied.last()[0].toDouble(), -144.);
    session.inputVolumeStep(-1);
    session.inputVolumeStep(1);
    QTRY_COMPARE(applied.last()[0].toDouble(), -24.);
    session.inputVolume(0);
    session.inputVolumeStep(1);
    session.inputVolumeStep(-1);
    QTRY_COMPARE(applied.last()[0].toDouble(), -1.);
    const auto count = f.left.volumeRequests;
    session.inputVolume(1);
    session.inputVolume(-145);
    session.inputVolume(std::numeric_limits<double>::quiet_NaN());
    session.inputVolumeStep(0);
    session.inputVolumeStep(2);
    session.stop();
    session.inputVolume(-10);
    session.inputVolumeStep(1);
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(f.left.volumeRequests, count);
    QVERIFY(i18n::Message(done[0][0].toJsonArray()).isEmpty());
  }
  void continuousInputKeepsSessionAndDiscardsOldGenerations() {
    SessionFixture f;
    f.timing.inputTimeoutMs = 100;
    f.timing.lateMs = 1000;
    f.timing.prebufferSamples = 1024;
    f.timing.backlogSamples = 8192;
    f.stream = {std::make_shared<audio::CaptureQueue>(352, 704, 704, 128),
                audio::format(16), audio::format(16), 352};
    f.stream.gapPolicy = audio::GapPolicy::Silence;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr, f.environment);
    connect(&session, &AirPlaySession::startCapture, &session, &AirPlaySession::captureStarted);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(f.left.packets.size() > 20);
    QVERIFY(done.isEmpty());
    const auto decoded = [&](const QByteArray &packet) {
      return unseal(f.left.srp.key.left(32), QByteArray(4, '\0') + packet.right(8),
                    packet.mid(12, packet.size() - 20), packet.mid(4, 8));
    };
    const auto silence = alac(std::vector<int16_t>(704, 0));
    for (const auto &packet : f.left.packets) QCOMPARE(decoded(packet), silence);
    std::array<int16_t, 352> samples;
    samples.fill(123);
    auto feed = [&](int count, uint64_t generation) {
      for (int i = 0; i < count; ++i)
        QVERIFY(f.stream.queue->push(samples.data(), samples.data(), nullptr, generation));
    };
    feed(8, f.stream.queue->generation.load());
    const auto audio = alac(std::vector<int16_t>(704, 123));
    QTRY_VERIFY(std::any_of(f.left.packets.begin(), f.left.packets.end(),
                            [&](const auto &p) { return decoded(p) == audio; }));
    const auto old = f.stream.queue->generation.fetch_add(1);
    samples.fill(777);
    feed(5, old); // A queued callback from a removed PipeWire node.
    const auto forbidden = alac(std::vector<int16_t>(704, 777));
    QTest::qWait(180);
    QVERIFY(done.isEmpty());
    for (const auto &packet : f.left.packets) QVERIFY(decoded(packet) != forbidden);
    samples.fill(456);
    feed(8, f.stream.queue->generation.load());
    const auto resumed = alac(std::vector<int16_t>(704, 456));
    QTRY_VERIFY(std::any_of(f.left.packets.begin(), f.left.packets.end(),
                            [&](const auto &p) { return decoded(p) == resumed; }));
    // Excess input resets buffering, not the AirPlay session.
    feed(50, f.stream.queue->generation.load());
    QTest::qWait(180);
    QVERIFY(done.isEmpty());
    QCOMPARE(f.left.teardowns, 0);
    const auto first = f.left.packets.first();
    for (qsizetype i = 0; i < f.left.packets.size(); ++i) {
      const auto &p = f.left.packets[i];
      QCOMPARE(uint16_t(readBe(p, 2, 2)), uint16_t(readBe(first, 2, 2) + i));
      QCOMPARE(uint32_t(readBe(p, 4, 4)), uint32_t(readBe(first, 4, 4) + i * 352));
      QCOMPARE(p.right(8), nonce(uint64_t(i)).mid(4));
    }
    session.stop();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::Stopped));
  }
  void pausedTelemetryCountsOnlyEnabledIntervals_data() {
    QTest::addColumn<bool>("stereo");
    QTest::newRow("single") << false;
    QTest::newRow("stereo") << true;
  }
  void pausedTelemetryCountsOnlyEnabledIntervals() {
    QFETCH(bool, stereo);
    SessionFixture f;
    QList<Receiver *> receivers{&f.left};
    if (stereo)
      receivers.append(&f.right);
    else
      f.endpoints.removeLast();
    // Feed exact packet-sized blocks, leaving room for telemetry waits without
    // turning the deliberately idle input into a transport timeout.
    f.timing.lateMs = 1000;
    f.timing.inputTimeoutMs = 5000;
    f.stream = {std::make_shared<audio::CaptureQueue>(352, 704, 704, 128),
                audio::format(16), audio::format(16), 352};
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    connect(&session, &AirPlaySession::startCapture, &session,
            &AirPlaySession::captureStarted);
    QSignalSpy telemetry(&session, &AirPlaySession::telemetry);
    QSignalSpy done(&session, &AirPlaySession::finished);
    QVERIFY(telemetry.isValid() && done.isValid());
    session.setTelemetryEnabled(false, 1);
    session.start();
    std::array<int16_t, 352> samples{};
    quint64 countedPackets = 0, countedFeedback = 0;
    for (int phase = 0; phase < 5; ++phase) {
      const bool enabled = phase % 2 != 0;
      const quint64 revision = phase + 1;
      session.setTelemetryEnabled(enabled, revision);
      telemetry.clear();
      for (int i = 0; i < 8; ++i)
        QVERIFY(f.stream.queue->push(samples.data(), samples.data()));
      for (auto *receiver : receivers) {
        QTRY_COMPARE(receiver->packets.size(), (phase + 1) * 8);
        const auto first = uint16_t(readBe(receiver->packets.first(), 2, 2));
        // The valid response also confirms the preceding expired request has
        // been processed before changing the telemetry gate.
        receiver->retransmit(uint16_t(first - 1), 1);
        receiver->retransmit(first, 1);
      }
      for (auto *receiver : receivers) {
        QTRY_COMPARE(receiver->retransmits.size(), phase + 1);
        QCOMPARE(receiver->retransmits.last().mid(4), receiver->packets.first());
      }
      if (enabled) {
        countedPackets += 8;
        countedFeedback += receivers.size();
        QTRY_VERIFY(!telemetry.isEmpty() &&
                    telemetry.last()[3].toULongLong() == countedPackets &&
                    telemetry.last()[4].toULongLong() == countedFeedback &&
                    telemetry.last()[5].toULongLong() == countedFeedback);
        QCOMPARE(telemetry.last()[6].toULongLong(), revision);
      } else {
        QVERIFY(telemetry.isEmpty());
      }
      QVERIFY(done.isEmpty());
    }
    session.setTelemetryEnabled(true, 6);
    QTRY_VERIFY(!telemetry.isEmpty());
    QCOMPARE(telemetry.first()[3].toULongLong(), countedPackets);
    QCOMPARE(telemetry.first()[4].toULongLong(), countedFeedback);
    QCOMPARE(telemetry.first()[5].toULongLong(), countedFeedback);
    for (auto *receiver : receivers) {
      const auto first = uint16_t(readBe(receiver->packets.first(), 2, 2));
      const auto firstRtp = uint32_t(readBe(receiver->packets.first(), 4, 4));
      for (int i = 0; i < receiver->packets.size(); ++i) {
        const auto &packet = receiver->packets[i];
        QCOMPARE(readBe(packet, 2, 2), uint64_t(uint16_t(first + i)));
        QCOMPARE(readBe(packet, 4, 4), uint64_t(uint32_t(firstRtp + i * 352)));
        QCOMPARE(packet.right(8), nonce(uint64_t(i)).mid(4));
      }
    }
    session.stop();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done.first()[1].toInt(), int(SessionEnd::Stopped));
  }
  void pausedTelemetryPreservesTransportAndDiagnostics() {
    SessionFixture f;
    f.l.fill(16384);
    f.r.fill(-8192);
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy telemetry(&session, &AirPlaySession::telemetry);
    QSignalSpy logs(&session, &AirPlaySession::log);
    QSignalSpy volume(&session, &AirPlaySession::volumeApplied);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.setTelemetryEnabled(false, 1);
    session.start();
    QTRY_VERIFY(f.left.packets.size() > 20);
    QVERIFY(telemetry.isEmpty());
    QVERIFY(!logs.isEmpty());

    session.setTelemetryEnabled(true, 2);
    QTRY_VERIFY(!telemetry.isEmpty());
    QCOMPARE(telemetry.last()[0].toDouble(), .5);
    QCOMPARE(telemetry.last()[1].toDouble(), .25);
    QCOMPARE(telemetry.last()[6].toULongLong(), quint64(2));
    const auto packets = telemetry.last()[3].toULongLong();
    session.setTelemetryEnabled(false, 3);
    telemetry.clear();
    const auto received = f.left.packets.size();
    const auto seq = uint16_t(readBe(f.left.packets.last(), 2, 2));
    f.left.retransmit(seq, 1);
    f.left.retransmit(uint16_t(readBe(f.left.packets.first(), 2, 2) - 1), 1);
    QTRY_COMPARE(f.left.retransmits.size(), 1);
    const auto applied = volume.size();
    session.volume(-25);
    QTRY_COMPARE(volume.size(), applied + 1);
    QTRY_VERIFY(f.left.packets.size() > received + 20);
    QVERIFY(telemetry.isEmpty());
    QVERIFY(done.isEmpty());

    f.l.fill(0);
    f.r.fill(0);
    // Drain samples captured before switching the test input to silence.
    QTest::qWait(150);
    session.setTelemetryEnabled(true, 4);
    QTest::qWait(40);
    QVERIFY(telemetry.isEmpty());
    QTRY_VERIFY(!telemetry.isEmpty());
    QCOMPARE(telemetry.first()[0].toDouble(), 0.);
    QCOMPARE(telemetry.first()[1].toDouble(), 0.);
    QVERIFY(telemetry.first()[3].toULongLong() > packets);
    QCOMPARE(telemetry.first()[4].toULongLong(), quint64(0));
    QCOMPARE(telemetry.first()[5].toULongLong(), quint64(0));
    QCOMPARE(telemetry.first()[6].toULongLong(), quint64(4));

    session.setTelemetryEnabled(false, 5);
    telemetry.clear();
    const auto logCount = logs.size();
    f.stream.queue->fault = 2;
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done.first()[1].toInt(), int(SessionEnd::Failure));
    QVERIFY(!i18n::Message(done.first()[0].toJsonArray()).isEmpty());
    QVERIFY(logs.size() > logCount);
    QVERIFY(telemetry.isEmpty());
  }
#ifdef Q_OS_MACOS
  void nativeDacpPublication() {
    NetworkRoute route;
    for (const auto &binding : NetworkBinding::available())
      if (!QHostAddress(binding.ipv4).isLoopback()) {
        route = NetworkRoute::resolve(binding);
        break;
      }
    if (route.local.isNull())
      QSKIP("No active LAN IPv4 for native Bonjour test");
    DacpServer server;
    const auto id = QString::fromLatin1(randomBytes(8).toHex().toUpper());
    QSignalSpy ready(&server, &DacpServer::ready);
    QSignalSpy failed(&server, &DacpServer::failed);
    server.start(id, route.local, route, {route.local});
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 6000);
    QVERIFY2(failed.isEmpty(),
             failed.isEmpty()
                 ? ""
                 : qPrintable(i18n::Message(failed[0][0].toJsonArray())
                                  .render(i18n::Language::Chinese)));
    struct Result {
      bool done = false;
      int error = 0;
      quint16 port = 0;
      QString host;
    } result;
    DNSServiceRef ref = nullptr;
    const auto name = ("iTunes_Ctrl_" + id).toLatin1();
    QCOMPARE(DNSServiceResolve(
                 &ref, 0, route.index, name.constData(), "_dacp._tcp", "local.",
                 [](DNSServiceRef, DNSServiceFlags, uint32_t,
                    DNSServiceErrorType error, const char *, const char *host,
                    uint16_t port, uint16_t, const unsigned char *, void *ctx) {
                   auto &r = *static_cast<Result *>(ctx);
                   r.done = true;
                   r.error = error;
                   if (!error) {
                     r.port = qFromBigEndian(port);
                     r.host = QString::fromUtf8(host);
                   }
                 },
                 &result),
             0);
    const auto release = qScopeGuard([&] { DNSServiceRefDeallocate(ref); });
    QSocketNotifier notifier(DNSServiceRefSockFD(ref), QSocketNotifier::Read);
    connect(&notifier, &QSocketNotifier::activated, this, [&] {
      const int error = DNSServiceProcessResult(ref);
      if (error) {
        result.error = error;
        result.done = true;
      }
    });
    QTRY_VERIFY_WITH_TIMEOUT(result.done, 5000);
    QCOMPARE(result.error, 0);
    QCOMPARE(result.port, server.port());
    QCOMPARE(result.host.toLower(), "airplayqt-" + id.toLower() + ".local.");
    struct AddressResult {
      bool done = false;
      int error = 0;
      uint index = 0;
      QHostAddress address;
    } address;
    DNSServiceRef addressRef = nullptr;
    const auto hostname = result.host.toUtf8();
    QCOMPARE(DNSServiceGetAddrInfo(
                 &addressRef, 0, route.index, kDNSServiceProtocol_IPv4,
                 hostname.constData(),
                 [](DNSServiceRef, DNSServiceFlags, uint32_t index,
                    DNSServiceErrorType error, const char *, const sockaddr *sa,
                    uint32_t, void *ctx) {
                   auto &r = *static_cast<AddressResult *>(ctx);
                   r.done = true;
                   r.error = error;
                   r.index = index;
                   if (!error && sa && sa->sa_family == AF_INET)
                     r.address = QHostAddress(
                         ntohl(reinterpret_cast<const sockaddr_in *>(sa)
                                   ->sin_addr.s_addr));
                 },
                 &address),
             0);
    const auto releaseAddress =
        qScopeGuard([&] { DNSServiceRefDeallocate(addressRef); });
    QSocketNotifier addressNotifier(DNSServiceRefSockFD(addressRef),
                                    QSocketNotifier::Read);
    connect(&addressNotifier, &QSocketNotifier::activated, this, [&] {
      const int error = DNSServiceProcessResult(addressRef);
      if (error) {
        address.error = error;
        address.done = true;
      }
    });
    QTRY_VERIFY_WITH_TIMEOUT(address.done, 5000);
    QCOMPARE(address.error, 0);
    QCOMPARE(address.address, route.local);
    QCOMPARE(address.index, route.index);
    server.stop();
    QVERIFY(!server.port());
  }
#endif
  void encryptedEventsAndRemoteVolume() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    QSignalSpy applied(&session, &AirPlaySession::volumeApplied);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    QVERIFY(f.left.metadata.contains("AirPlayQt"));
    QCOMPARE(f.left.metadataRtpInfo,
             QByteArray("rtptime=") +
                 QByteArray::number(readBe(f.left.packets.first(), 4, 4)));
    QCOMPARE(f.left.commands.size(), 5);
    const auto npi = f.left.commands[1]["params"].toMap()["params"].toMap();
    QCOMPARE(npi["kMRMediaRemoteNowPlayingInfoTitle"].toString(), QString("AirPlayQt"));
    QVERIFY(!npi.contains("kMRMediaRemoteNowPlayingInfoDuration"));
    QCOMPARE(f.left.commands[2]["params"]
                .toMap()["mrSupportedCommandsFromSender"]
                .toList().size(), 3);
    const auto body = plistEncode(QVariantMap{
        {"type", "updateInfo"},
        {"params", QVariantMap{{"diagnostic", QByteArray(2500, 'x')}}}});
    auto request =
        QByteArray("POST /command RTSP/1.0\r\nCSeq: 73\r\nContent-Length: ") +
        QByteArray::number(body.size()) + "\r\n\r\n" + body;
    auto wire = f.left.eventRecords->encode(request);
    // Split a record header, then deliver multiple encrypted records together.
    f.left.eventSocket->write(wire.left(1));
    QTest::qWait(10);
    f.left.eventSocket->write(wire.mid(1));
    QTRY_COMPARE(f.left.eventResponses.size(), 1);
    QCOMPARE(f.left.eventResponses[0].status, 200);
    QCOMPARE(f.left.eventResponses[0].headers["cseq"], QByteArray("73"));
    QVERIFY(f.left.error.isEmpty());
    auto *remote = session.findChild<DacpServer *>();
    QVERIFY(remote);
    auto send = [&](const QByteArray &path, const QByteArray &token) {
      return dacpRequest(remote->port(), path, token);
    };
    QVERIFY(send("volumeup", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(f.left.volumes.last(), -29.);
    QTRY_COMPARE(f.right.volumes.last(), -29.);
    QTRY_COMPARE(applied.last()[0].toDouble(), -29.);
    const auto property =
        send("getproperty?properties=dmcp.volume,dacp.volumecontrollable",
             f.left.activeRemote);
    QVERIFY(property.contains("200"));
    QVERIFY(property.endsWith(volumeProperties(-29., true, true)));
    auto count = f.left.volumeRequests;
    QVERIFY(send("setproperty?dmcp.device-volume=-29", f.left.activeRemote)
                .contains("204"));
    QTest::qWait(20);
    QCOMPARE(f.left.volumeRequests, count); // echo does not loop
    QVERIFY(send("volumeup", "stale-token").contains("403"));
    QVERIFY(send("nextitem", f.left.activeRemote).contains("501"));
    QVERIFY(send("setproperty?dmcp.device-volume=nan", f.left.activeRemote)
                .contains("400"));
    QVERIFY(send("setproperty?dmcp.device-volume=1", f.left.activeRemote)
                .contains("400"));
    QVERIFY(send("setproperty?dmcp.device-volume=-18&dmcp.device-volume=-12",
                 f.left.activeRemote)
                .contains("400"));
    QVERIFY(send("devicevolume=-24", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(f.right.volumes.last(), -24.);
    QVERIFY(send("devicevolume=-29", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(applied.last()[0].toDouble(), -29.);
    QVERIFY(send("mutetoggle", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(f.left.volumes.last(), -144.);
    QVERIFY(send("mutetoggle", f.left.activeRemote).contains("204"));
    QTRY_COMPARE(f.left.volumes.last(), -29.);
    // Rapid desired values coalesce while an older RTSP write is in flight.
    session.volume(-25);
    session.volume(-22);
    session.volume(-18);
    QTRY_COMPARE(applied.last()[0].toDouble(), -18.);
    QCOMPARE(f.left.volumes.last(), -18.);
    QCOMPARE(f.right.volumes.last(), -18.);
    QVERIFY(done.isEmpty());
    session.stop();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(
        f.left.commands.last()["params"].toMap()["mrPlaybackState"].toInt(), 3);
    QCOMPARE(f.left.teardowns, 1);
    QCOMPARE(remote->port(), quint16(0));
  }
  void eventAuthenticationFailureStopsGroup() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    auto bad = f.left.eventRecords->encode(
        "POST /command RTSP/1.0\r\nContent-Length: 0\r\n\r\n");
    bad[bad.size() - 1] ^= 1;
    f.left.eventSocket->write(bad);
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::Failure));
    QVERIFY(!i18n::Message(done[0][0].toJsonArray())
                 .render(i18n::Language::Chinese)
                 .isEmpty());
  }
  void eventReplySurvivesReceiverDeadline() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(!f.left.packets.isEmpty());
    const auto body = plistEncode(QVariantMap{{"type", "updateInfo"}});
    const auto request =
        QByteArray("POST /command RTSP/1.0\r\nCSeq: 1\r\nContent-Length: ") +
        QByteArray::number(body.size()) + "\r\n\r\n" + body;
    f.left.eventSocket->write(f.left.eventRecords->encode(request));
    // Reproduce the receiver's observed 30-second response deadline. This is
    // real elapsed time, including normal audio pacing and RTSP keepalives.
    bool checked = false;
    QTimer::singleShot(30000, &session, [&] {
      checked = true;
      if (f.left.eventResponses.isEmpty())
        f.left.eventSocket->disconnectFromHost();
    });
    QTRY_COMPARE_WITH_TIMEOUT(f.left.eventResponses.size(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(checked, 32000);
    QVERIFY(done.isEmpty());
    QVERIFY(f.left.eventSocket);
    QCOMPARE(f.left.eventSocket->state(), QAbstractSocket::ConnectedState);
    QVERIFY(f.left.packets.size() > 3000);
    session.stop();
    QTRY_COMPARE(done.size(), 1);
  }
  void metadataRejectionStopsBeforeAudio() {
    SessionFixture f;
    f.right.rejectMetadata = true;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(i18n::Message(done[0][0].toJsonArray())
                .render(i18n::Language::Chinese)
                .contains("播放信息／外部控制未就绪"));
    QVERIFY(f.left.packets.isEmpty());
    QVERIFY(f.right.packets.isEmpty());
  }
  void boundedControlFraming() {
    QByteArray part = "POST /command RTSP/1.0\r\nContent-Length: 4\r\n\r\nab";
    QVERIFY(!parseControlMessage(part));
    part += "cdGET /x HTTP/1.1\r\n\r\n";
    QCOMPARE(parseControlMessage(part)->body, QByteArray("abcd"));
    QCOMPARE(parseControlMessage(part)->line, QByteArray("GET /x HTTP/1.1"));
    QVERIFY(part.isEmpty());
    for (auto bad :
         {QByteArray("GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n"),
          QByteArray("GET / HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n"),
          QByteArray("GET / HTTP/1.1\r\nContent-Length: 1\r\ncontent-length: "
                     "1\r\n\r\n"),
          QByteArray("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
          QByteArray(16385, 'x')})
      QVERIFY_THROWS_EXCEPTION(Error, parseControlMessage(bad));
  }
  void networkBindingAndNativeSockets() {
    QVERIFY(NetworkBinding{}.json().isNull());
    QVERIFY(NetworkBinding::fromJson(QJsonValue(QJsonValue::Null)).automatic());
    for (const auto value :
         {QJsonValue(QJsonValue::Undefined), QJsonValue(true),
          QJsonValue(QJsonObject{}),
          QJsonValue(QJsonObject{{"interfaceName", "x"}, {"ipv4", "127.1"}})})
      QVERIFY_THROWS_EXCEPTION(Error, NetworkBinding::fromJson(value));
    QVERIFY_THROWS_EXCEPTION(
        Error, NetworkRoute::resolve({"missing-airplayqt", "192.0.2.1"}));
    const auto choices = NetworkBinding::available();
    const auto it =
        std::find_if(choices.begin(), choices.end(),
                     [](const auto &v) { return v.ipv4 == "127.0.0.1"; });
    QVERIFY(it != choices.end());
    const auto route = NetworkRoute::resolve(*it);
    QCOMPARE(NetworkBinding::fromJson(it->json()), *it);
    auto stale = route;
    ++stale.index;
    QVERIFY_THROWS_EXCEPTION(Error, stale.validate());
    stale = route;
    stale.local = QHostAddress("192.0.2.250");
    QVERIFY_THROWS_EXCEPTION(Error, stale.validate());
    auto checkOption = [&](QAbstractSocket &socket) {
#ifdef Q_OS_WIN
      DWORD actual = 0;
      int size = sizeof(actual);
      QCOMPARE(getsockopt(SOCKET(socket.socketDescriptor()), IPPROTO_IP,
                          IP_UNICAST_IF, reinterpret_cast<char *>(&actual),
                          &size),
               0);
      QCOMPARE(ntohl(actual), route.index);
#elif defined(Q_OS_LINUX)
      char actual[256]{};
      socklen_t size = sizeof(actual);
      QCOMPARE(getsockopt(int(socket.socketDescriptor()), SOL_SOCKET,
                          SO_BINDTODEVICE, actual, &size), 0);
      QCOMPARE(QString::fromLocal8Bit(actual), route.binding.interfaceName);
#else
      int actual = 0;
      socklen_t size = sizeof(actual);
      QCOMPARE(getsockopt(int(socket.socketDescriptor()), IPPROTO_IP,
                          IP_BOUND_IF, &actual, &size),
               0);
      QCOMPARE(uint(actual), route.index);
#endif
      QCOMPARE(socket.proxy().type(), QNetworkProxy::NoProxy);
    };
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket tcp;
    route.bind(tcp);
    checkOption(tcp);
    tcp.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QTRY_VERIFY(server.hasPendingConnections());
    std::unique_ptr<QTcpSocket> accepted(server.nextPendingConnection());
    QCOMPARE(accepted->peerAddress(), route.local);
    QUdpSocket udp, receiver;
    QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
    route.bind(udp);
    checkOption(udp);
    QCOMPARE(udp.writeDatagram("test", QHostAddress::LocalHost,
                               receiver.localPort()),
             4);
    QTRY_VERIFY(receiver.hasPendingDatagrams());
    QCOMPARE(receiver.receiveDatagram().senderAddress(), route.local);
    QUdpSocket multicastReceiver;
    route.bind(multicastReceiver, 0, true);
    checkOption(multicastReceiver);
    QNetworkDatagram packet("ptp-source", QHostAddress::LocalHost,
                            receiver.localPort());
    packet.setSender(route.local, multicastReceiver.localPort());
    QCOMPARE(multicastReceiver.writeDatagram(packet), 10);
    QTRY_VERIFY(receiver.hasPendingDatagrams());
    QCOMPARE(receiver.receiveDatagram().senderAddress(), route.local);
    QUdpSocket collision;
    QVERIFY_THROWS_EXCEPTION(Error, route.bind(collision, udp.localPort()));
    QCOMPARE(collision.state(), QAbstractSocket::UnconnectedState);
  }
  void boundSession() {
    NetworkBinding binding;
    for (const auto &v : NetworkBinding::available())
      if (v.ipv4 == "127.0.0.1")
        binding = v;
    QVERIFY(!binding.automatic());
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment, NetworkRoute::resolve(binding));
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        f.left.packets.size() >= 4 && f.right.packets.size() >= 4, 4000);
    QCOMPARE(f.left.socket->peerAddress(), QHostAddress(binding.ipv4));
    QCOMPARE(f.right.socket->peerAddress(), QHostAddress(binding.ipv4));
    for (const auto *receiver : {&f.left, &f.right}) {
      QCOMPARE(receiver->eventSource, QHostAddress(binding.ipv4));
      QCOMPARE(receiver->dataSource, QHostAddress(binding.ipv4));
      QTRY_COMPARE(receiver->controlSource, QHostAddress(binding.ipv4));
    }
    session.stop();
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(i18n::Message(done[0][0].toJsonArray())
                .render(i18n::Language::Chinese)
                .isEmpty());
  }
  void boundPtpNative() {
    NetworkBinding binding;
    for (const auto &v : NetworkBinding::available())
      if (v.ipv4 == "127.0.0.1")
        binding = v;
    QVERIFY(!binding.automatic());
    QUdpSocket eventProbe, generalProbe;
    for (auto [probe, port] : {std::pair{&eventProbe, 319}, std::pair{&generalProbe, 320}})
      if (!probe->bind(QHostAddress::AnyIPv4, quint16(port), QUdpSocket::DontShareAddress))
        QSKIP(qPrintable(QString("Cannot bind PTP UDP %1: %2; native PTP success path not verified")
                             .arg(port).arg(probe->errorString())));
    eventProbe.close();
    generalProbe.close();
    PtpClock clock;
    QSignalSpy errors(&clock, &PtpClock::failed);
    clock.start(QHostAddress(binding.ipv4), {QHostAddress::LocalHost}, 42,
                app::Timing{}, NetworkRoute::resolve(binding));
    QTest::qWait(150);
    QCOMPARE(errors.size(), 0);
    clock.stop();
    // The real PTP sockets must release both fixed ports on stop.
    QUdpSocket event, general;
    QVERIFY(
        event.bind(QHostAddress::AnyIPv4, 319, QUdpSocket::DontShareAddress));
    QVERIFY(
        general.bind(QHostAddress::AnyIPv4, 320, QUdpSocket::DontShareAddress));
  }
  void failureDuringInterruptionCleanup() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(f.left.packets.size() >= 4, 4000);
    session.stop({}, SessionEnd::HostInterrupted);
    session.stop("指定网卡已失效");
    session.stop("后续故障");
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(
        i18n::Message(done[0][0].toJsonArray()).render(i18n::Language::Chinese),
        QString("指定网卡已失效"));
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::Failure));
  }
  void invalidBindingNeverConnects() {
    SessionFixture f;
    AirPlaySession session(
        f.timing, f.stream, f.endpoints, nullptr, f.environment,
        {{"missing-airplayqt", "192.0.2.1"}, 1, QHostAddress("192.0.2.1")});
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::Failure));
    QVERIFY(i18n::Message(done[0][0].toJsonArray())
                .render(i18n::Language::Chinese)
                .contains("网卡"));
    QVERIFY(!f.left.socket && !f.right.socket);
  }
  void endpointValidation() {
    QCOMPARE(parseReceiverEndpoint("192.168.8.9").text(),
             QString("192.168.8.9:7000"));
    QCOMPARE(parseReceiverEndpoint(" 127.0.0.1:1234 ").port, quint16(1234));
    for (const auto &value :
         {"", "127.1", "host.local", "::1", "256.0.0.1", "1.2.3.4:0",
          "1.2.3.4:65536", "1.2.3.4:-1", "1.2.3.4:abc", "1.2.3.4:", "0.0.0.0",
          "255.255.255.255", "224.0.0.1", "+1.2.3.4"})
      QVERIFY_THROWS_EXCEPTION(Error, parseReceiverEndpoint(value));
    const auto one = parseReceiverEndpoint("192.168.8.9");
    const auto two = parseReceiverEndpoint("192.168.8.10:7001");
    validateEndpoints({one});
    validateEndpoints({one, two});
    QVERIFY_THROWS_EXCEPTION(Error, validateEndpoints({}));
    QVERIFY_THROWS_EXCEPTION(Error, validateEndpoints({one, one}));
    QVERIFY_THROWS_EXCEPTION(Error, validateEndpoints({one, two, one}));
    QVERIFY_THROWS_EXCEPTION(
        Error, validateEndpoints({{QHostAddress::LocalHostIPv6, 7000}}));
  }
  void singleReceiverSession() {
    SessionFixture f;
    f.endpoints.removeLast();
    f.left.stereo.clear();
    f.timing.keepAliveMs = 1000;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished),
        volume(&session, &AirPlaySession::volumeApplied),
        logs(&session, &AirPlaySession::log);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(f.left.packets.size() >= 4, 4000);
    QVERIFY(f.right.socket == nullptr);
    QVERIFY(!f.left.sessionSetup.contains("senderPerceivedClusterType"));
    QCOMPARE(f.left.sessionSetup["timingProtocol"].toString(), QString("PTP"));
    QCOMPARE(f.left.volumes[0], -20.);
    session.volume(-144);
    QTRY_COMPARE_WITH_TIMEOUT(volume.size(), 2, 1000);
    session.volume(-20);
    QTRY_COMPARE_WITH_TIMEOUT(volume.size(), 3, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(f.left.options > 0, 2000);
    const auto seq = uint16_t(readBe(f.left.packets.last(), 2, 2));
    f.left.retransmit(seq, 1);
    QTRY_COMPARE_WITH_TIMEOUT(f.left.retransmits.size(), 1, 1000);
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QCOMPARE(
        i18n::Message(done[0][0].toJsonArray()).render(i18n::Language::Chinese),
        QString{});
    QCOMPARE(f.left.teardowns, 1);
    QVERIFY(!f.input.isActive());
    for (const auto &entry : logs)
      QVERIFY(!i18n::Message(entry[0].toJsonArray())
                   .render(i18n::Language::Chinese)
                   .startsWith("[VST速率"));
  }
  void vstRateDiagnostics() {
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
    SessionFixture f;
    f.stream.rateDiagnostics = true;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy logs(&session, &AirPlaySession::log),
        done(&session, &AirPlaySession::finished);
    const auto periodic = [&] {
      for (const auto &entry : logs)
        if (i18n::Message(entry[0].toJsonArray())
                .render(i18n::Language::Chinese)
                .startsWith("[VST速率] 窗口"))
          return i18n::Message(entry[0].toJsonArray())
              .render(i18n::Language::Chinese);
      return QString{};
    };
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(!periodic().isEmpty(), 4000);
    const auto match =
        QRegularExpression("输入 ([0-9.]+) 帧/s；发送 ([0-9.]+) 帧/s")
            .match(periodic());
    QVERIFY(match.hasMatch());
    QVERIFY(match.captured(1).toDouble() > 42000);
    QVERIFY(match.captured(1).toDouble() < 46200);
    QVERIFY(match.captured(2).toDouble() > 38000);
    QVERIFY(periodic().contains("poll 最大间隔"));
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QVERIFY(i18n::Message(done[0][0].toJsonArray())
                .render(i18n::Language::Chinese)
                .isEmpty());
    QVERIFY(std::any_of(logs.begin(), logs.end(), [](const auto &entry) {
      return i18n::Message(entry[0].toJsonArray())
          .render(i18n::Language::Chinese)
          .startsWith("[VST速率·停止]");
    }));
#else
    QSKIP("VST rate diagnostics disabled in this build");
#endif
  }
  void vstRateDiagnosticsBeforeEarlyOverflow() {
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
    SessionFixture f;
    f.stream.rateDiagnostics = true;
    f.captureRate = 88200;
    f.timing.backlogSamples = 16384;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy logs(&session, &AirPlaySession::log),
        done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QVERIFY(i18n::Message(done[0][0].toJsonArray())
                .render(i18n::Language::Chinese)
                .contains("积压"));
    QString final;
    for (const auto &entry : logs)
      if (i18n::Message(entry[0].toJsonArray())
              .render(i18n::Language::Chinese)
              .startsWith("[VST速率·停止]"))
        final = i18n::Message(entry[0].toJsonArray())
                    .render(i18n::Language::Chinese);
    QVERIFY(!final.isEmpty());
    const auto match =
        QRegularExpression("输入 ([0-9.]+) 帧/s；发送 ([0-9.]+) 帧/s")
            .match(final);
    QVERIFY(match.hasMatch());
    QVERIFY(match.captured(1).toDouble() > match.captured(2).toDouble() * 1.5);
#else
    QSKIP("VST rate diagnostics disabled in this build");
#endif
  }
  void invalidGroupIdentity_data() {
    QTest::addColumn<bool>("sameDevice");
    QTest::newRow("different-group") << false;
    QTest::newRow("same-device") << true;
  }
  void invalidGroupIdentity() {
    QFETCH(bool, sameDevice);
    SessionFixture f;
    if (sameDevice)
      f.right.id = "LEFT";
    else
      f.right.stereo = "different";
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
    QVERIFY(!i18n::Message(done[0][0].toJsonArray())
                 .render(i18n::Language::Chinese)
                 .isEmpty());
    QVERIFY(f.left.sessionSetup.isEmpty());
    QVERIFY(f.right.sessionSetup.isEmpty());
    QVERIFY(f.left.packets.isEmpty());
  }
  void keepAliveSerializesVolume() {
    SessionFixture f;
    f.timing.keepAliveMs = 1000;
    f.left.optionsDelay = f.right.optionsDelay = 100;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished),
        volume(&session, &AirPlaySession::volumeApplied);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(f.left.options >= 1 && f.right.options >= 1, 3000);
    session.volume(-18);
    QTRY_COMPARE_WITH_TIMEOUT(volume.size(), 2, 1000);
    QCOMPARE(f.left.volumes.last(), -18.);
    QCOMPARE(f.right.volumes.last(), -18.);
    QTRY_VERIFY_WITH_TIMEOUT(f.left.options >= 2 && f.right.options >= 2, 3000);
    QVERIFY(done.isEmpty());
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QCOMPARE(
        i18n::Message(done[0][0].toJsonArray()).render(i18n::Language::Chinese),
        QString{});
  }
  void keepAliveFailureStopsGroup() {
    SessionFixture f;
    f.timing.keepAliveMs = 1000;
    f.right.failure = Receiver::Failure::KeepAlive;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QVERIFY(!i18n::Message(done[0][0].toJsonArray())
                 .render(i18n::Language::Chinese)
                 .isEmpty());
    QVERIFY(!f.input.isActive());
  }
  void pocVectors() {
    QFile file(QString(QT_TESTCASE_SOURCEDIR) + "/tests/airplay/vectors.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    auto value = [&](const char *key) {
      return QByteArray::fromHex(json.value(key).toString().toLatin1());
    };
    const auto proof = srp(value("salt"), value("server"), value("private"));
    QCOMPARE(proof.publicKey, value("public"));
    QCOMPARE(proof.proof, value("proof"));
    QCOMPARE(proof.expected, value("expected"));
    QCOMPARE(proof.key, value("shared"));
    QCOMPARE(hkdf(proof.key, "Control-Salt", "Control-Write-Encryption-Key"),
             value("writeKey"));
    QCOMPARE(hkdf(proof.key, "Control-Salt", "Control-Read-Encryption-Key"),
             value("readKey"));
    QByteArray key;
    for (int i = 0; i < 32; ++i)
      key.append(char(i));
    HapRecords records(key, key);
    QCOMPARE(records.encode(value("hapPlain")), value("hapWire"));
    const auto bytes = value("pcm");
    std::array<int16_t, 704> pcm{};
    for (size_t i = 0; i < pcm.size(); ++i) {
      const uint16_t word =
          uint16_t(uint8_t(bytes[qsizetype(i * 2)])) |
          (uint16_t(uint8_t(bytes[qsizetype(i * 2 + 1)])) << 8);
      pcm[i] = std::bit_cast<int16_t>(word);
    }
    QCOMPARE(alac(pcm), value("alac"));
    QCOMPARE(audioPacket(key, pcm, 65535, UINT32_MAX, 0, true), value("audio"));
    QCOMPARE(audioPacket(key, pcm, 0, 0x15f, 1, false), value("audioWrapped"));
    QCOMPARE(ptpHeader(8, 96, 0x456789ABCDEF0123ULL, 65535),
             value("ptpHeader"));
    QCOMPARE(ptpTime(1790723456123456789ULL), value("ptpTime"));
    QCOMPARE(syncPacket(0x456789ABCDEF0123ULL, 0xffffff00,
                        1790723456123456789LL, 1790723454123456788LL, true),
             value("syncBefore"));
    QCOMPARE(syncPacket(0x456789ABCDEF0123ULL, 0xffffff00,
                        1790723456123456789LL, 1790723456234567890LL, false),
             value("syncAfter"));
  }
  void tlvBoundaries() {
    const QByteArray value(511, 'x');
    QCOMPARE(tlvDecode(tlvEncode({{3, value}})).value(3), value);
    QCOMPARE(tlvEncode({{7, {}}}), QByteArray::fromHex("0700"));
    QVERIFY_THROWS_EXCEPTION(Error, tlvDecode(QByteArray::fromHex("01")));
    QVERIFY_THROWS_EXCEPTION(Error, tlvDecode(QByteArray::fromHex("010201")));
    QVERIFY_THROWS_EXCEPTION(Error, tlvEncode({{256, {}}}));
  }
  void recordsAuthenticationAndExhaustion() {
    const QByteArray key(32, 'k');
    HapRecords tx(key, key), rx(key, key);
    const auto record = tx.encode(QByteArray(1025, 'a'));
    QCOMPARE(rx.decode(record.left(2), record.mid(2, 1040)),
             QByteArray(1024, 'a'));
    QCOMPARE(rx.decode(record.mid(1042, 2), record.mid(1044)),
             QByteArray(1, 'a'));
    auto next = tx.encode("hello");
    auto bad = next;
    bad[4] = char(uint8_t(bad[4]) ^ 1);
    QVERIFY_THROWS_EXCEPTION(Error, rx.decode(bad.left(2), bad.mid(2)));
    QCOMPARE(rx.rx, 2ULL);
    QCOMPARE(rx.decode(next.left(2), next.mid(2)), QByteArray("hello"));
    QVERIFY_THROWS_EXCEPTION(Error, rx.decode(QByteArray::fromHex("0000"), {}));
    tx.tx = UINT64_MAX;
    rx.rx = UINT64_MAX;
    next = tx.encode("last");
    QCOMPARE(rx.decode(next.left(2), next.mid(2)), QByteArray("last"));
    QVERIFY_THROWS_EXCEPTION(Error, tx.encode("reuse"));
    QVERIFY_THROWS_EXCEPTION(Error, rx.decode(next.left(2), next.mid(2)));
  }
  void parserFraming() {
    QByteArray partial =
        "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\nab";
    QVERIFY(!parseResponse(partial));
    partial += 'c';
    auto result = parseResponse(partial);
    QVERIFY(result);
    QCOMPARE(result->body, QByteArray("abc"));
    QVERIFY(partial.isEmpty());
    for (auto value :
         {QByteArray("RTSP/1.0 200 OK\r\nContent-Length: 1\r\ncontent-length: "
                     "1\r\n\r\nx"),
          QByteArray("RTSP/1.0 200 OK\r\nContent-Length: -1\r\n\r\n"),
          QByteArray("RTSP/1.0 200 OK\r\nContent-Length: 1048577\r\n\r\n"),
          QByteArray("RTSP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"),
          QByteArray(16385, 'x')})
      QVERIFY_THROWS_EXCEPTION(Error, parseResponse(value));
  }
  void groupValidation() {
    ReceiverInfo a{"a", "A", "pair", {}, -20}, b{"b", "B", "pair", {}, -30};
    validateGroup(a, b);
    b.stereoId = "other";
    QVERIFY_THROWS_EXCEPTION(Error, validateGroup(a, b));
    b.stereoId = a.stereoId;
    b.deviceId = "a";
    QVERIFY_THROWS_EXCEPTION(Error, validateGroup(a, b));
    QVERIFY_THROWS_EXCEPTION(
        Error, receiverInfo(plistEncode(QVariantMap{{"initialVolume", true}})));
  }
  void alacBoundaries() {
    QVERIFY_THROWS_EXCEPTION(Error, alac({}));
    std::array<int16_t, 705> odd{};
    QVERIFY_THROWS_EXCEPTION(Error, alac(odd));
    std::array<int16_t, 706> tooMany{};
    QVERIFY_THROWS_EXCEPTION(Error, alac(tooMany));
    for (int frames : {1, 64, 352}) {
      std::vector<int16_t> pcm(size_t(frames) * 2);
      for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = int16_t(i % 2 ? -int(i) : int(i));
      const auto encoded = alac(pcm);
      size_t position = 0;
      const auto bits = [&](int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++position)
          value = (value << 1) | ((uint8_t(encoded[qsizetype(position / 8)]) >>
                                   (7 - position % 8)) &
                                  1);
        return value;
      };
      QCOMPARE(bits(3), 1u);
      QCOMPARE(bits(4), 0u);
      QCOMPARE(bits(12), 0u);
      QCOMPARE(bits(1), 1u);
      QCOMPARE(bits(2), 0u);
      QCOMPARE(bits(1), 1u);
      QCOMPARE(bits(32), uint32_t(frames));
      for (int16_t value : pcm)
        QCOMPARE(bits(16), uint32_t(uint16_t(value)));
      QCOMPARE(bits(3), 7u);
      const QByteArray key(32, 'k');
      const auto first = audioPacket(key, pcm, 65535, UINT32_MAX, 0, true);
      const auto next = audioPacket(
          key, pcm, 0, uint32_t(UINT32_MAX + uint32_t(frames)), 1, false);
      QCOMPARE(readBe(first, 2, 2), uint64_t(65535));
      QCOMPARE(readBe(next, 2, 2), uint64_t(0));
      QCOMPARE(readBe(next, 4, 4), uint64_t(frames - 1));
      QCOMPARE(unseal(key, QByteArray(4, '\0') + next.right(8),
                      next.mid(12, next.size() - 20), next.mid(4, 8)),
               encoded);
    }
  }
  void configuredPacketTimeline_data() {
    QTest::addColumn<int>("packetSamples");
    QTest::addColumn<bool>("continuous");
    for (int packet : {1, 64, 352})
      for (bool continuous : {false, true})
        QTest::newRow(qPrintable(QString("%1-%2").arg(packet).arg(continuous)))
            << packet << continuous;
  }
  void configuredPacketTimeline() {
    QFETCH(int, packetSamples);
    QFETCH(bool, continuous);
    SessionFixture f;
    f.timing.packetSamples = packetSamples;
    f.timing.prebufferSamples = 1;
    f.stream = {std::make_shared<audio::CaptureQueue>(
                    packetSamples, size_t(packetSamples) * 2,
                    size_t(packetSamples) * 2, 512),
                audio::format(16), audio::format(16), packetSamples};
    if (continuous)
      f.stream.gapPolicy = audio::GapPolicy::Silence;
    std::vector<int16_t> left(packetSamples, 123), right(packetSamples, -234);
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    QSignalSpy done(&session, &AirPlaySession::finished);
    connect(&session, &AirPlaySession::startCapture, &session, [&] {
      if (!continuous)
        for (int i = 0; i < 32; ++i)
          QVERIFY(f.stream.queue->push(left.data(), right.data()));
      session.captureStarted();
    });
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        f.left.packets.size() >= 4 && f.right.packets.size() >= 4, 4000);
    QCOMPARE(f.left.error,
             QString{}); // Test receiver independently requires SETUP spf=352.
    QCOMPARE(f.right.error, QString{});
    const uint32_t initial = uint32_t(readBe(f.left.packets[0], 4, 4));
    for (int i = 0; i < 4; ++i) {
      const auto a = f.left.packets[i], b = f.right.packets[i];
      QCOMPARE(a.left(12), b.left(12));
      QCOMPARE(readBe(a, 4, 4),
               uint64_t(uint32_t(initial + uint32_t(i * packetSamples))));
      const auto decoded =
          unseal(f.left.srp.key.left(32), QByteArray(4, '\0') + a.right(8),
                 a.mid(12, a.size() - 20), a.mid(4, 8));
      uint32_t actualFrames = 0;
      for (int bit = 23; bit < 55; ++bit)
        actualFrames = (actualFrames << 1) |
                       ((uint8_t(decoded[bit / 8]) >> (7 - bit % 8)) & 1);
      QCOMPARE(actualFrames, uint32_t(packetSamples));
      std::vector<int16_t> expected;
      for (int j = 0; j < packetSamples; ++j) {
        expected.push_back(continuous ? 0 : 123);
        expected.push_back(continuous ? 0 : -234);
      }
      QCOMPARE(decoded, alac(expected));
    }
    const auto retransmitted = f.left.packets.last();
    f.left.retransmit(uint16_t(readBe(retransmitted, 2, 2)), 1);
    QTRY_COMPARE_WITH_TIMEOUT(f.left.retransmits.size(), 1, 1000);
    QCOMPARE(f.left.retransmits[0].mid(4), retransmitted);
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    const auto count = f.left.packets.size();
    QTest::qWait(10);
    QCOMPARE(f.left.packets.size(), count);
  }
  void successfulGroupAndRetransmit() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished),
        volume(&session, &AirPlaySession::volumeApplied);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        f.left.packets.size() >= 4 && f.right.packets.size() >= 4, 4000);
    QCOMPARE(f.left.error, QString{});
    QCOMPARE(f.right.error, QString{});
    QCOMPARE(f.left.volumes[0], -30.);
    QCOMPARE(f.right.volumes[0], -30.);
    QCOMPARE(f.left.sessionSetup["senderPerceivedClusterType"].toInt(), 1);
    QCOMPARE(f.right.sessionSetup["senderPerceivedClusterType"].toInt(), 1);
    QCOMPARE(f.left.sessionSetup["groupUUID"],
             f.right.sessionSetup["groupUUID"]);
    QCOMPARE(f.left.sessionSetup["timingPeerInfo"].toMap()["ClockID"],
             f.right.sessionSetup["timingPeerInfo"].toMap()["ClockID"]);
    QCOMPARE(f.left.sessionSetup["timingPeerInfo"].toMap()["Addresses"],
             f.right.sessionSetup["timingPeerInfo"].toMap()["Addresses"]);
    for (int i = 0; i < 4; ++i) {
      const auto a = f.left.packets[i], b = f.right.packets[i];
      QCOMPARE(a.left(12), b.left(12));
      QCOMPARE(a.right(8), b.right(8));
      const auto decoded =
          unseal(f.left.srp.key.left(32), QByteArray(4, '\0') + a.right(8),
                 a.mid(12, a.size() - 20), a.mid(4, 8));
      std::vector<int16_t> expected;
      for (int block = 0; block < 2; ++block)
        for (int j = 0; j < 176; ++j) {
          expected.push_back(j + 1);
          expected.push_back(-j - 1);
        }
      QCOMPARE(decoded, alac(expected));
    }
    const auto seq = uint16_t(readBe(f.left.packets[0], 2, 2));
    f.left.retransmit(seq, 1);
    QTRY_COMPARE_WITH_TIMEOUT(f.left.retransmits.size(), 1, 1000);
    QCOMPARE(f.left.retransmits[0].mid(4), f.left.packets[0]);
    f.left.retransmit(seq, 0);
    f.left.retransmit(seq, 1025);
    f.left.retransmit(uint16_t(seq - 1), 1);
    session.volume(-25);
    QTRY_COMPARE_WITH_TIMEOUT(volume.size(), 2, 1000);
    QCOMPARE(f.left.volumes.last(), -25.);
    QCOMPARE(f.right.volumes.last(), -25.);
    session.volume(-144);
    QTRY_COMPARE_WITH_TIMEOUT(volume.size(), 3, 1000);
    session.volume(-25);
    QTRY_COMPARE_WITH_TIMEOUT(volume.size(), 4, 1000);
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QCOMPARE(
        i18n::Message(done[0][0].toJsonArray()).render(i18n::Language::Chinese),
        QString{});
    QCOMPARE(f.left.teardowns, 1);
    QCOMPARE(f.right.teardowns, 1);
  }
  void initializationFailure_data() {
    QTest::addColumn<int>("failure");
    QTest::newRow("setup") << int(Receiver::Failure::SessionSetup);
    QTest::newRow("proof") << int(Receiver::Failure::BadProof);
    QTest::newRow("timeout") << int(Receiver::Failure::InfoTimeout);
  }
  void destroyActiveSession() {
    SessionFixture f;
    auto session = std::make_unique<AirPlaySession>(
        f.timing, f.stream, f.endpoints, nullptr, f.environment);
    f.attach(*session);
    session->start();
    QTRY_VERIFY_WITH_TIMEOUT(
        !f.left.packets.isEmpty() && !f.right.packets.isEmpty(), 3000);
    f.input.stop();
    session.reset();
    QTRY_COMPARE(f.left.socket->state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(f.right.socket->state(), QAbstractSocket::UnconnectedState);
  }
  void initializationFailure() {
    QFETCH(int, failure);
    SessionFixture f;
    f.right.failure = Receiver::Failure(failure);
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QVERIFY(!i18n::Message(done[0][0].toJsonArray())
                 .render(i18n::Language::Chinese)
                 .isEmpty());
    QVERIFY(!f.input.isActive());
    QVERIFY(f.left.packets.isEmpty());
  }
  void volumeFailureStopsGroup() {
    SessionFixture f;
    f.right.failure = Receiver::Failure::Volume;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(!f.left.packets.isEmpty(), 3000);
    session.volume(-15);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
    QVERIFY(!i18n::Message(done[0][0].toJsonArray())
                 .render(i18n::Language::Chinese)
                 .isEmpty());
    QVERIFY(!f.input.isActive());
  }
  void cancelAndRestart() {
    for (int i = 0; i < 3; ++i) {
      SessionFixture f;
      f.right.failure = Receiver::Failure::InfoTimeout;
      AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                             f.environment);
      f.attach(session);
      QSignalSpy done(&session, &AirPlaySession::finished);
      session.start();
      QTest::qWait(10);
      session.stop();
      session.stop();
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
      QCOMPARE(i18n::Message(done[0][0].toJsonArray())
                   .render(i18n::Language::Chinese),
               QString{});
      QVERIFY(!f.input.isActive());
    }
  }
  void captureFaultStopsGroup_data() {
    QTest::addColumn<int>("fault");
    QTest::newRow("overflow") << 2;
    QTest::newRow("rate-change") << 4;
    QTest::newRow("position") << 5;
    QTest::newRow("reset") << 6;
  }
  void captureFaultStopsGroup() {
    QFETCH(int, fault);
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(!f.left.packets.isEmpty(), 3000);
    f.stream.queue->fault = fault;
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QVERIFY(!i18n::Message(done[0][0].toJsonArray())
                 .render(i18n::Language::Chinese)
                 .isEmpty());
    QVERIFY(!f.input.isActive());
  }
  void hostInterruptionIsNotAnInputError() {
    SessionFixture f;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(!f.left.packets.isEmpty(), 3000);
    f.stream.queue->interrupted = true;
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QCOMPARE(
        i18n::Message(done[0][0].toJsonArray()).render(i18n::Language::Chinese),
        QString{});
    QCOMPARE(done[0][1].toInt(), int(SessionEnd::HostInterrupted));
    QVERIFY(!f.input.isActive());
  }
  void inputTimeout() {
    SessionFixture f;
    f.timing.inputTimeoutMs = 100;
    f.timing.lateMs = 1000;
    AirPlaySession session(f.timing, f.stream, f.endpoints, nullptr,
                           f.environment);
    f.attach(session);
    QSignalSpy done(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(!f.left.packets.isEmpty(), 3000);
    f.input.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
    QVERIFY(i18n::Message(done[0][0].toJsonArray())
                .render(i18n::Language::Chinese)
                .contains("断流"));
  }
};
QTEST_GUILESS_MAIN(ProtocolTests)
#include "Tests.moc"
