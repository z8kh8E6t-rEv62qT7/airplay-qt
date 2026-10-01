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
#include <dns_sd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using namespace airplay;
namespace {
using test::Receiver;
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
    timing.settle = 0;
    timing.prebuffer = .008;
    timing.backlog = .5;
    timing.late = .5;
    timing.requestTimeout = .2;
    timing.connectTimeout = .2;
    timing.teardownTimeout = .1;
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
    QCOMPARE(telemetry.first()[4].toULongLong(), quint64(1));
    QCOMPARE(telemetry.first()[5].toULongLong(), quint64(1));
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
    QCOMPARE(npi["Title"].toString(), QString("AirPlayQt"));
    QVERIFY(!npi.contains("Duration"));
    QVERIFY(f.left.commands[2]["params"]
                .toMap()["mrSupportedCommandsFromSender"]
                .toList()
                .isEmpty());
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
      QTcpSocket socket;
      socket.connectToHost(QHostAddress::LocalHost, remote->port());
      if (!socket.waitForConnected(1000))
        return QByteArray{};
      socket.write("GET /ctrl-int/1/" + path +
                   " HTTP/1.1\r\nActive-Remote: " + token + "\r\n\r\n");
      QByteArray response;
      QElapsedTimer timer;
      timer.start();
      while (!response.contains("\r\n\r\n") && timer.elapsed() < 1000) {
        QTest::qWait(1);
        response += socket.readAll();
      }
      return response;
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
    QVERIFY(send("pause", f.left.activeRemote).contains("501"));
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
    if (!eventProbe.bind(QHostAddress::AnyIPv4, 319,
                         QUdpSocket::DontShareAddress) ||
        !generalProbe.bind(QHostAddress::AnyIPv4, 320,
                           QUdpSocket::DontShareAddress))
      QSKIP("PTP 319/320 occupied by another process; native PTP success path "
            "not verified");
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
    f.timing.keepAlive = 1;
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
    f.timing.backlog = .2;
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
    f.timing.keepAlive = 1;
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
    f.timing.keepAlive = 1;
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
    std::array<int16_t, 704> full{};
    QCOMPARE(alac(full).size(), qsizetype(1416));
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
    f.timing.inputTimeout = .1;
    f.timing.late = 1;
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
