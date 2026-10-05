#include "TestReceiver.h"
#include "airplay/InboundControlServer.h"
#include "airplay/TransientPairing.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>

using namespace airplay;
namespace {
QByteArray request(const QByteArray &method, const QByteArray &path,
                   const QByteArray &body = {}, const QByteArray &extra = {}) {
  return method + " " + path + " RTSP/1.0\r\nCSeq: 1\r\nContent-Length: " +
         QByteArray::number(body.size()) + "\r\n" + extra + "\r\n" + body;
}
QByteArray fairPlayStart(int mode = 0) {
  auto bytes = QByteArray::fromHex("46504c590301010000000004020000bb");
  bytes[14] = char(mode);
  return bytes;
}
QByteArray fairPlayFinish(char value = 'x') {
  return QByteArray::fromHex("46504c590301030000000098") +
         QByteArray(152, value);
}
QByteArray fairPlayRequest(const QByteArray &body,
                           const QByteArray &et = "32") {
  return request("POST", "/fp-setup", body, "X-Apple-ET: " + et + "\r\n");
}
QVariantMap remoteStream(QString channel = "test-channel") {
  return {{"type", 130},
          {"controlType", 2},
          {"clientTypeUUID", "1910A70F-DBC0-4242-AF95-115DB30604E1"},
          {"clientUUID", "47172D6C-C249-4852-ADEB-6714C6E1ECF0"},
          {"channelID", channel}};
}
QByteArray streamRequest(const QVariantList &streams,
                         const QByteArray &method = "SETUP") {
  return request(method, "/", plistEncode(QVariantMap{{"streams", streams}}));
}
QByteArray remoteCommand(quint64 id,
                         QByteArray data = QByteArray::fromHex("020800")) {
  return request(
      "POST", "/command",
      plistEncode(QVariantMap{{"params", QVariantMap{{"data", data}}}}),
      "X-Apple-StreamID: " + QByteArray::number(id) + "\r\n");
}
struct Client {
  QTcpSocket socket;
  QByteArray wire, plain;
  std::unique_ptr<HapRecords> records;
  std::unique_ptr<HapRecords> eventRecords;
  explicit Client(quint16 port) {
    socket.connectToHost(QHostAddress::LocalHost, port);
  }
  RtspResponse exchange(QByteArray bytes, bool fragment = false) {
    if (records)
      bytes = records->encode(bytes);
    if (fragment) {
      for (int i = 0; i < bytes.size(); i += 7) {
        socket.write(bytes.mid(i, 7));
        QTest::qWait(1);
      }
    } else
      socket.write(bytes);
    QElapsedTimer deadline;
    deadline.start();
    do {
      QTest::qWait(1);
      wire += socket.readAll();
      if (records)
        plain += records->decodeAvailable(wire);
      else {
        plain += wire;
        wire.clear();
      }
      if (auto response = parseResponse(plain))
        return *response;
    } while (deadline.elapsed() < 2000);
    throw std::runtime_error("Timed out waiting for control response");
  }
  SrpProof challenge() {
    const auto response = exchange(request(
        "POST", "/pair-setup", QByteArray::fromHex("000100060101130110"),
        "X-Apple-HKP: 4\r\nContent-Type: "
        "application/x-apple-binary-plist\r\n"));
    if (response.status != 200)
      throw std::runtime_error("Challenge failed");
    const auto tlv = tlvDecode(response.body);
    return srp(tlv.value(2), tlv.value(3));
  }
  void pair() {
    const auto proof = challenge();
    const auto response =
        exchange(request("POST", "/pair-setup",
                         tlvEncode({{6, QByteArray(1, char(3))},
                                    {3, proof.publicKey},
                                    {4, proof.proof}}),
                         "X-Apple-HKP: 4\r\n"));
    const auto tlv = tlvDecode(response.body);
    if (response.status != 200 || tlv.value(6) != QByteArray(1, char(4)) ||
        tlv.value(4) != proof.expected || tlv.contains(7))
      throw std::runtime_error("Server proof verification failed");
    records = std::make_unique<HapRecords>(
        hkdf(proof.key, "Control-Salt", "Control-Write-Encryption-Key"),
        hkdf(proof.key, "Control-Salt", "Control-Read-Encryption-Key"));
    eventRecords = std::make_unique<HapRecords>(
        hkdf(proof.key, "Events-Salt", "Events-Read-Encryption-Key"),
        hkdf(proof.key, "Events-Salt", "Events-Write-Encryption-Key"));
  }
  void fairPlay() {
    if (exchange(fairPlayRequest(fairPlayStart())).status != 200 ||
        exchange(fairPlayRequest(fairPlayFinish())).status != 200)
      throw std::runtime_error("FairPlay setup failed");
  }
  QVariantMap setup() {
    const auto response = exchange(request(
        "SETUP", "/",
        plistEncode(QVariantMap{{"isRemoteControlOnly", true},
                                {"timingProtocol", "None"},
                                {"combinedGetInfoWithControlSetup", true}})));
    if (response.status != 200)
      throw std::runtime_error("RC SETUP failed");
    return plistDecode(response.body).toMap();
  }
  ControlMessage receiveEvent() {
    QElapsedTimer deadline;
    deadline.start();
    do {
      QTest::qWait(1);
      wire += socket.readAll();
      plain += records->decodeAvailable(wire);
      if (auto message = parseControlMessage(plain))
        return *message;
    } while (deadline.elapsed() < 2000);
    throw std::runtime_error("Timed out waiting for MRP event");
  }
  void acknowledge(const ControlMessage &event, int status = 200,
                   QByteArray sequence = {}) {
    if (sequence.isEmpty())
      sequence = event.headers.value("cseq");
    socket.write(records->encode("RTSP/1.0 " + QByteArray::number(status) +
                                 " Result\r\nCSeq: " + sequence +
                                 "\r\nContent-Length: 0\r\n\r\n"));
  }
};
} // namespace
class Tests : public QObject {
  Q_OBJECT
private slots:
  void malformedClientCannotFailAudioOwner() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    QSignalSpy failed(&server, &InboundControlServer::failed);
    QSignalSpy errors(&server, &InboundControlServer::error);
    Client first(server.port()), healthy(server.port());
    first.socket.write("RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    QTRY_COMPARE(first.socket.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(errors.size(), 1);
    QVERIFY(failed.isEmpty());
    healthy.pair();
    healthy.socket.write(healthy.records->encode("RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"));
    QTRY_COMPARE(healthy.socket.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(errors.size(), 2);
    QVERIFY(failed.isEmpty());
    Client replacement(server.port());
    QCOMPARE(replacement.exchange(request("GET", "/info")).status, 200);
  }
  void listenerLifetimeAndCapacity() {
    InboundControlServer server, other;
    for (const auto &address :
         {QHostAddress{}, QHostAddress(QHostAddress::AnyIPv4),
          QHostAddress("224.0.0.1"), QHostAddress(QHostAddress::LocalHostIPv6)})
      QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                               server.start(address, {}, {}));
    QCOMPARE(server.port(), quint16(0));
    server.start(QHostAddress::LocalHost, {}, {});
    other.start(QHostAddress::LocalHost, {}, {});
    QVERIFY(server.port() != other.port());
    std::vector<std::unique_ptr<Client>> clients;
    QByteArray firstIdentity;
    for (int i = 0; i < 8; ++i) {
      auto &client =
          *clients.emplace_back(std::make_unique<Client>(server.port()));
      const auto info =
          plistDecode(client.exchange(request("GET", "/info")).body).toMap();
      if (!i)
        firstIdentity = info.value("pi").toString().toUtf8();
      QCOMPARE(info.value("pi").toString().toUtf8(), firstIdentity);
    }
    Client rejected(server.port());
    QTRY_COMPARE(rejected.socket.state(), QAbstractSocket::UnconnectedState);
    Client independent(other.port());
    QVERIFY(plistDecode(independent.exchange(request("GET", "/info")).body)
                .toMap()
                .value("pi")
                .toString()
                .toUtf8() != firstIdentity);
    server.beginStop();
    QCOMPARE(server.port(), quint16(0));
    server.start(QHostAddress::LocalHost, {}, {});
    const auto replacementPort = server.port();
    for (const auto &client : clients)
      QTRY_COMPARE(client->socket.state(), QAbstractSocket::UnconnectedState);
    QTest::qWait(300);
    QCOMPARE(server.port(), replacementPort);
    Client replacement(replacementPort);
    const auto replacementIdentity =
        plistDecode(replacement.exchange(request("GET", "/info")).body)
            .toMap()
            .value("pi")
            .toString()
            .toUtf8();
    QVERIFY(replacementIdentity != firstIdentity);
    server.beginStop();
    QTRY_COMPARE_WITH_TIMEOUT(replacement.socket.state(),
                              QAbstractSocket::UnconnectedState, 1000);
    QCOMPARE(independent.exchange(request("GET", "/info")).status, 200);
  }
  void finalStateAndConfirmedUpdates() {
    InboundControlServer server;
    int accepted = 0;
    server.start(QHostAddress::LocalHost, {}, [&](const ControlRequest &) {
      ++accepted;
      return true;
    });
    ControlState state{true,
                       PlaybackState::Playing,
                       -30.,
                       "group",
                       {{"device", "Receiver", {}, QHostAddress("127.0.0.2")}},
                       true};
    server.updateState(state);
    Client control(server.port());
    control.pair();
    control.fairPlay();
    Client event(quint16(control.setup().value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    const auto setup = control.exchange(streamRequest({remoteStream()}));
    const auto id = plistDecode(setup.body)
                        .toMap()
                        .value("streams")
                        .toList()
                        .first()
                        .toMap()
                        .value("streamID")
                        .toULongLong();
    QList<QByteArray> received;
    connect(&event.socket, &QTcpSocket::readyRead, &server, [&] {
      event.wire += event.socket.readAll();
      event.plain += event.records->decodeAvailable(event.wire);
      while (auto message = parseControlMessage(event.plain)) {
        received.append(plistDecode(message->body)
                            .toMap()
                            .value("params")
                            .toMap()
                            .value("data")
                            .toByteArray());
        event.acknowledge(*message);
      }
    });
    QCOMPARE(
        control
            .exchange(remoteCommand(
                id, QByteArray::fromHex("10080f1203726571a20106120474657374")))
            .status,
        200);
    QCOMPARE(control
                 .exchange(remoteCommand(
                     id, QByteArray::fromHex("0b0810aa0106100118012801")))
                 .status,
             200);
    QTRY_COMPARE(received.size(), 14);
    received.clear();
    state.playback = PlaybackState::Paused;
    state.settled = false;
    server.updateState(state);
    QTest::qWait(20);
    QVERIFY(received.isEmpty());
    state.settled = true;
    server.updateState(state);
    QTRY_COMPARE(received.size(), 13);
    QVERIFY(std::ranges::any_of(received, [](const auto &frame) {
      return frame.contains(QByteArray::fromHex("30024a"));
    }));
    received.clear();
    // This phone's authenticated UID must not become an output alias.
    const auto update =
        QByteArray::fromHex("120825a2010d1204746573749a010466616b65");
    QCOMPARE(control.exchange(remoteCommand(id, update)).status, 200);
    QTest::qWait(20);
    QVERIFY(received.isEmpty());
    const auto pause = QByteArray::fromHex("0b0801120372657132020802");
    QCOMPARE(control.exchange(remoteCommand(id, pause)).status, 200);
    QTRY_COMPARE(accepted, 1);
    QTest::qWait(20);
    QVERIFY(
        received.isEmpty()); // No fabricated success before owner confirmation.
    server.beginStop();
    QTRY_VERIFY(std::ranges::any_of(received, [](const auto &frame) {
      return frame.contains(QByteArray::fromHex("30034a"));
    }));
    QTRY_VERIFY(std::ranges::any_of(received, [](const auto &frame) {
      return frame.contains(QByteArray::fromHex("080212037265712001"));
    }));
    QTRY_COMPARE_WITH_TIMEOUT(control.socket.state(),
                              QAbstractSocket::UnconnectedState, 1000);
    QCOMPARE(accepted, 1);
  }
  void mrpAudioControl_data() {
    QTest::addColumn<bool>("stereo");
    QTest::addColumn<bool>("failVolume");
    QTest::addColumn<bool>("failPublication");
    QTest::newRow("single") << false << false << false;
    QTest::newRow("stereo") << true << false << false;
    QTest::newRow("stereo-failure") << true << true << false;
    QTest::newRow("stereo-event-failure") << true << false << true;
  }
  void mrpAudioControl() {
    QFETCH(bool, stereo);
    QFETCH(bool, failVolume);
    QFETCH(bool, failPublication);
    test::Receiver left("left"), right("right");
    app::Timing timing;
    timing.settleMs = 0;
    timing.packetSamples = 352;
    timing.prebufferSamples = 352;
    timing.lateMs = 500;
    timing.teardownTimeoutMs = 100;
    audio::CaptureStream capture;
    capture.blockFrames = 352;
    capture.left = capture.right = {2, 16, false, false};
    capture.queue = std::make_shared<audio::CaptureQueue>(352, 704, 704, 8);
    capture.gapPolicy = audio::GapPolicy::Silence;
    SessionEnvironment environment{[] {}, [] {}, false};
    QList<ReceiverEndpoint> endpoints{{QHostAddress::LocalHost, left.port()}};
    if (stereo)
      endpoints.append({QHostAddress::LocalHost, right.port()});
    AirPlaySession session(timing, capture, endpoints, nullptr, environment);
    connect(&session, &AirPlaySession::startCapture, &session,
            &AirPlaySession::captureStarted);
    QSignalSpy starts(&session, &AirPlaySession::startCapture),
        stops(&session, &AirPlaySession::stopCapture);
    QSignalSpy finished(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(left.packets.size() >= 4);
    const auto controlPort = quint16(left.sessionSetup.value("uglServerInfo")
                                         .toMap()
                                         .value("Port")
                                         .toUInt());
    QVERIFY(controlPort);
    Client client(controlPort);
    client.pair();
    client.fairPlay();
    const auto initial = client.setup();
    Client event(quint16(initial.value("eventPort").toUInt()));
    event.records = std::move(client.eventRecords);
    QList<QByteArray> received;
    QList<int> receivedTypes;
    bool ackWithheld = false;
    connect(&event.socket, &QTcpSocket::readyRead, &session, [&] {
      event.wire += event.socket.readAll();
      event.plain += event.records->decodeAvailable(event.wire);
      while (auto message = parseControlMessage(event.plain)) {
        QVERIFY(!ackWithheld); // No next frame until the prior request has a
                               // transport receipt.
        received.append(plistDecode(message->body)
                            .toMap()
                            .value("params")
                            .toMap()
                            .value("data")
                            .toByteArray());
        const auto raw = received.last();
        qsizetype prefix = 0;
        quint64 length = 0;
        do {
          QVERIFY(prefix < raw.size() && prefix < 10);
          length |= quint64(quint8(raw[prefix]) & 127) << (7 * prefix);
        } while (quint8(raw[prefix++]) & 128);
        QCOMPARE(length,
                 quint64(raw.size() - prefix)); // No second frame in one event.
        QCOMPARE(quint8(raw[prefix]), quint8(8));
        receivedTypes.append(quint8(raw[prefix + 1]));
        if (received.size() == 2 && !failPublication) {
          ackWithheld = true;
          QTimer::singleShot(25, &session, [&, request = *message] {
            ackWithheld = false;
            event.acknowledge(request);
          });
        } else
          event.acknowledge(*message, failPublication ? 500 : 200);
      }
    });
    const auto setup = client.exchange(streamRequest({remoteStream()}));
    const auto id = plistDecode(setup.body)
                        .toMap()
                        .value("streams")
                        .toList()
                        .first()
                        .toMap()
                        .value("streamID")
                        .toULongLong();
    QCOMPARE(
        client
            .exchange(remoteCommand(
                id, QByteArray::fromHex("10080f1203726571a20106120474657374")))
            .status,
        200);
    if (failPublication) {
      QTRY_COMPARE(finished.size(), 1);
      QVERIFY(!finished[0][0].toJsonArray().isEmpty());
      QTRY_COMPARE(client.socket.state(), QAbstractSocket::UnconnectedState);
      QCOMPARE(stops.size(), 1);
      return;
    }
    // Subscribe to player, volume, and output-device updates without
    // identifier.
    QCOMPARE(client
                 .exchange(remoteCommand(
                     id, QByteArray::fromHex("0b0810aa0106100118012801")))
                 .status,
             200);
    QTRY_VERIFY(received.size() >= 2);
    QVERIFY(
        received[1].contains("AirPlayQt")); // Initial DeviceInfo update; later
                                            // volume frames have no name.
    QCOMPARE(client
                 .exchange(remoteCommand(
                     id, QByteArray::fromHex("0b0801120372657132020802")))
                 .status,
             200);
    QTRY_COMPARE(left.protocolState, 2);
    if (stereo)
      QTRY_COMPARE(right.protocolState, 2);
    QTRY_VERIFY(session.controlState().settled);
    QCOMPARE(int(session.controlState().playback), 2);
    const auto atPause = left.packets.size();
    if (failVolume)
      left.failure = test::Receiver::Failure::Volume;
    QCOMPARE(
        client
            .exchange(remoteCommand(
                id, QByteArray::fromHex("0f08331203726571ba03050d0000003f")))
            .status,
        200);
    if (failVolume) {
      QTRY_COMPARE(finished.size(), 1);
      QVERIFY(!finished[0][0].toJsonArray().isEmpty());
      QTRY_COMPARE(client.socket.state(), QAbstractSocket::UnconnectedState);
      return;
    }
    QTRY_VERIFY(session.controlState().settled);
    QTRY_COMPARE(left.volumes.last(), -15.);
    if (stereo)
      QTRY_COMPARE(right.volumes.last(), -15.);
    std::array<int16_t, 352> samples;
    samples.fill(321);
    QVERIFY(capture.queue->push(samples.data(), samples.data()));
    const auto expectedAudio = alac(std::vector<int16_t>(704, 321));
    const auto hasPausedAudio = [&] {
      for (qsizetype i = atPause; i < left.packets.size(); ++i) {
        const auto &packet = left.packets[i];
        if (unseal(left.srp.key.left(32), QByteArray(4, '\0') + packet.right(8),
                   packet.mid(12, packet.size() - 20),
                   packet.mid(4, 8)) == expectedAudio)
          return true;
      }
      return false;
    };
    QTRY_VERIFY(hasPausedAudio());
    QTRY_VERIFY(left.packets.size() > atPause + 8);
    QCOMPARE(left.protocolState, 2);
    const auto hasPayload = [&](const QByteArray &pattern) {
      return std::ranges::any_of(
          received, [&](const auto &data) { return data.contains(pattern); });
    };
    // Actual event messages include the correlated successful command result,
    // paused state and confirmed half-volume, rather than just RTSP receipts.
    QTRY_VERIFY(
        hasPayload(QByteArray::fromHex("0802120372657120003a0408001000")));
    QTRY_VERIFY(hasPayload(QByteArray::fromHex("30024a")));
    QTRY_VERIFY(hasPayload(QByteArray::fromHex("0d0000003f")));
    const QList<int> initialTypes =
        stereo
            ? QList<int>{15, 37, 65, 46, 47, 4,  72, 17,
                         64, 52, 64, 52, 64, 52, 64, 52}
            : QList<int>{15, 37, 65, 46, 47, 4, 72, 17, 64, 52, 64, 52, 64, 52};
    QTRY_VERIFY(receivedTypes.size() >= initialTypes.size());
    QCOMPARE(receivedTypes.first(initialTypes.size()), initialTypes);
    QCOMPARE(starts.size(), 1);
    QVERIFY(stops.isEmpty());
    const auto first = left.packets.first();
    for (qsizetype i = 0; i < left.packets.size(); ++i) {
      const auto packet = left.packets[i];
      QCOMPARE(uint16_t(readBe(packet, 2, 2)),
               uint16_t(readBe(first, 2, 2) + i));
      QCOMPARE(uint32_t(readBe(packet, 4, 4)),
               uint32_t(readBe(first, 4, 4) + i * 352));
      quint64 nonce = 0;
      for (int j = 7; j >= 0; --j)
        nonce = (nonce << 8) | uint8_t(packet.right(8)[j]);
      QCOMPARE(nonce, quint64(i));
    }
    // Interleave rapid controls with delayed receiver metadata responses.
    left.commandDelay = 20;
    right.commandDelay = 20;
    for (int i = 0; i < 4; ++i)
      QCOMPARE(client
                   .exchange(remoteCommand(
                       id, QByteArray::fromHex("0b0801120372657132020803")))
                   .status,
               200);
    QCOMPARE(
        client
            .exchange(remoteCommand(
                id, QByteArray::fromHex("0f08331203726571ba03050d0000803f")))
            .status,
        200);
    QCOMPARE(client
                 .exchange(remoteCommand(
                     id, QByteArray::fromHex("0b0801120372657132020801")))
                 .status,
             200);
    QTRY_VERIFY(session.controlState().settled);
    QTRY_COMPARE(left.protocolState, 1);
    QTRY_COMPARE(left.volumes.last(), 0.);
    if (stereo) {
      QTRY_COMPARE(right.protocolState, 1);
      QTRY_COMPARE(right.volumes.last(), 0.);
    }
    // Two noncommutative commands in the same input batch must retain wire order.
    const auto pauseThenToggle = QByteArray::fromHex("0b0801120372657132020802") +
                                 QByteArray::fromHex("0b0801120372657132020803");
    QCOMPARE(client.exchange(remoteCommand(id, pauseThenToggle)).status, 200);
    QTRY_VERIFY(session.controlState().settled);
    QCOMPARE(session.controlState().playback, PlaybackState::Playing);
    QCOMPARE(left.protocolState, 1);
    if (!stereo) {
      QTest::qWait(15500);
      QCOMPARE(client.exchange(request("POST", "/feedback")).status, 200);
      QCOMPARE(client
                   .exchange(remoteCommand(
                       id, QByteArray::fromHex("0b0801120372657132020802")))
                   .status,
               200);
      QTRY_COMPARE(left.protocolState, 2);
    }
    session.stop();
    QTRY_COMPARE(finished.size(), 1);
    QTRY_COMPARE(client.socket.state(), QAbstractSocket::UnconnectedState);
    QVERIFY(finished[0][0].toJsonArray().isEmpty());
  }
  void mrpHandshakeOverEncryptedEvents() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto initial = control.setup();
    Client event(quint16(initial.value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    const auto setup =
        control.exchange(streamRequest({remoteStream("a"), remoteStream("b")}));
    const auto streams =
        plistDecode(setup.body).toMap().value("streams").toList();
    const auto a = streams[0].toMap().value("streamID").toULongLong();
    const auto b = streams[1].toMap().value("streamID").toULongLong();
    const auto hello =
        QByteArray::fromHex("10080f1203726571a20106120474657374");
    QCOMPARE(control.exchange(remoteCommand(a, hello.first(4))).status, 200);
    QTest::qWait(5);
    QCOMPARE(event.socket.bytesAvailable(), 0);
    const auto receipt = control.exchange(remoteCommand(a, hello.mid(4)), true);
    QCOMPARE(receipt.status, 200);
    QVERIFY(receipt.body.isEmpty());
    const auto response = event.receiveEvent();
    QCOMPARE(response.line, "POST /command RTSP/1.0");
    QCOMPARE(response.headers.value("x-apple-streamid"), QByteArray::number(a));
    QCOMPARE(response.headers.value("content-type"),
             "application/x-apple-binary-plist");
    const auto payload = plistDecode(response.body)
                             .toMap()
                             .value("params")
                             .toMap()
                             .value("data");
    QCOMPARE(payload.typeId(), QMetaType::QByteArray);
    QVERIFY(payload.toByteArray().contains(
        QByteArray::fromHex("080f12037265712000")));
    QVERIFY(payload.toByteArray().contains(
        initial.value("info").toMap().value("pi").toString().toUtf8()));
    QVERIFY(payload.toByteArray().contains("AirPlayQt"));
    event.acknowledge(response);
    // A subscription on a different, unintroduced stream must not inherit
    // state.
    const auto subscribe =
        QByteArray::fromHex("0e08101203726571aa010408011001");
    QCOMPARE(control.exchange(remoteCommand(b, subscribe)).status, 200);
    const auto unintroduced = event.receiveEvent();
    QVERIFY(unintroduced.body.size() > 0);
    const auto errorData = plistDecode(unintroduced.body)
                               .toMap()
                               .value("params")
                               .toMap()
                               .value("data")
                               .toByteArray();
    QVERIFY(errorData.contains(QByteArray::fromHex("080012037265712002")));
    event.acknowledge(unintroduced);
    // MRP requests can also arrive on the event socket after it acknowledges.
    QCOMPARE(event.exchange(remoteCommand(a, subscribe)).status, 200);
    const auto subscribed = event.receiveEvent();
    QVERIFY(plistDecode(subscribed.body)
                .toMap()
                .value("params")
                .toMap()
                .value("data")
                .toByteArray()
                .contains(QByteArray::fromHex("080012037265712000")));
    event.acknowledge(subscribed);
    QCOMPARE(
        control.exchange(remoteCommand(a, QByteArray::fromHex("00"))).status,
        400);
    QCOMPARE(
        control
            .exchange(remoteCommand(a, QByteArray::fromHex("0708631203726571")))
            .status,
        200);
    const auto unsupported = event.receiveEvent();
    QVERIFY(plistDecode(unsupported.body)
                .toMap()
                .value("params")
                .toMap()
                .value("data")
                .toByteArray()
                .contains(QByteArray::fromHex("080012037265712006")));
    // A late transport ack after local stream teardown must not recreate its
    // state.
    QCOMPARE(control.exchange(streamRequest({streams[0]}, "TEARDOWN")).status,
             200);
    event.acknowledge(unsupported);
    QCOMPARE(control.exchange(remoteCommand(a, hello)).status, 454);
    QCOMPARE(control.exchange(remoteCommand(b, hello)).status, 200);
    const auto remaining = event.receiveEvent();
    QCOMPARE(remaining.headers.value("x-apple-streamid"),
             QByteArray::number(b));
    event.acknowledge(remaining);
    server.stop();
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void mrpAcknowledgementWithoutCSeq() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto initial = control.setup();
    Client event(quint16(initial.value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    const auto setup = control.exchange(streamRequest({remoteStream()}));
    const auto id = plistDecode(setup.body)
                        .toMap()
                        .value("streams")
                        .toList()
                        .first()
                        .toMap()
                        .value("streamID")
                        .toULongLong();
    const auto hello =
        QByteArray::fromHex("10080f1203726571a20106120474657374");
    const auto subscribe =
        QByteArray::fromHex("0e08101203726571aa010408011001");
    QCOMPARE(control.exchange(remoteCommand(id, hello + subscribe)).status,
             200);
    const auto first = event.receiveEvent();
    QVERIFY(plistDecode(first.body)
                .toMap()
                .value("params")
                .toMap()
                .value("data")
                .toByteArray()
                .contains(QByteArray::fromHex("080f12037265712000")));
    const auto ack =
        event.records->encode("RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    event.socket.write(ack.first(5));
    QTest::qWait(5);
    QCOMPARE(event.socket.bytesAvailable(), 0);
    event.socket.write(ack.mid(5));
    const auto second = event.receiveEvent();
    QVERIFY(second.headers.value("cseq") != first.headers.value("cseq"));
    QVERIFY(plistDecode(second.body)
                .toMap()
                .value("params")
                .toMap()
                .value("data")
                .toByteArray()
                .contains(QByteArray::fromHex("080012037265712000")));
    event.acknowledge(second);
    QCOMPARE(control.exchange(request("POST", "/feedback")).status, 200);
    // Without any pending request, another unnumbered ack must be rejected.
    event.socket.write(
        event.records->encode("RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"));
    QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void mrpEventFailure_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("sequence");
    QTest::newRow("failed-status") << 500 << QByteArray{};
    QTest::newRow("wrong-cseq") << 200 << QByteArray("999");
    QTest::newRow("invalid-cseq") << 200 << QByteArray("+1");
    QTest::newRow("empty-cseq") << 200 << QByteArray(" ");
    QTest::newRow("missing-cseq-failure") << 500 << QByteArray("omit");
    QTest::newRow("timeout") << 0 << QByteArray{};
  }
  void mrpEventFailure() {
    QFETCH(int, status);
    QFETCH(QByteArray, sequence);
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto initial = control.setup();
    Client event(quint16(initial.value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    const auto setup = control.exchange(streamRequest({remoteStream()}));
    const auto id = plistDecode(setup.body)
                        .toMap()
                        .value("streams")
                        .toList()
                        .first()
                        .toMap()
                        .value("streamID")
                        .toULongLong();
    QCOMPARE(
        control
            .exchange(remoteCommand(
                id, QByteArray::fromHex("10080f1203726571a20106120474657374")))
            .status,
        200);
    const auto request = event.receiveEvent();
    if (sequence == "omit")
      event.socket.write(event.records->encode(
          "RTSP/1.0 500 Error\r\nContent-Length: 0\r\n\r\n"));
    else if (status)
      event.acknowledge(request, status, sequence);
    QTRY_COMPARE_WITH_TIMEOUT(control.socket.state(),
                              QAbstractSocket::UnconnectedState, 6500);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void mrpDeferredEventsAndStreamCleanup() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto initial = control.setup();
    const auto setup =
        control.exchange(streamRequest({remoteStream("a"), remoteStream("b")}));
    const auto streams =
        plistDecode(setup.body).toMap().value("streams").toList();
    const auto a = streams[0].toMap().value("streamID").toULongLong();
    const auto b = streams[1].toMap().value("streamID").toULongLong();
    const auto hello =
        QByteArray::fromHex("10080f1203726571a20106120474657374");
    QCOMPARE(control.exchange(remoteCommand(a, hello)).status, 200);
    QCOMPARE(control.exchange(remoteCommand(b, hello)).status, 200);
    QCOMPARE(control.exchange(streamRequest({streams[0]}, "TEARDOWN")).status,
             200);
    Client event(quint16(initial.value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    const auto response = event.receiveEvent();
    QCOMPARE(response.headers.value("x-apple-streamid"), QByteArray::number(b));
    // Stop while an event is in flight, then ensure reconnect creates fresh
    // state.
    server.stop();
    server.start(QHostAddress::LocalHost, {}, {});
    Client next(server.port());
    next.pair();
    next.fairPlay();
    next.setup();
    QCOMPARE(next.exchange(remoteCommand(b, hello)).status, 454);
    QCOMPARE(next.exchange(request("POST", "/feedback")).status, 200);
  }
  void mrpQueueBoundAndConnectionIsolation() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port()), other(server.port());
    control.pair();
    control.fairPlay();
    control.setup();
    other.pair();
    other.fairPlay();
    other.setup();
    const auto setup = control.exchange(streamRequest({remoteStream()}));
    const auto stream =
        plistDecode(setup.body).toMap().value("streams").toList().first();
    const auto id = stream.toMap().value("streamID").toULongLong();
    const auto hello =
        QByteArray::fromHex("10080f1203726571a20106120474657374");
    QCOMPARE(control.exchange(remoteCommand(id, hello.repeated(32))).status,
             200);
    // Removing the stream frees all unsent replies before a replacement stream
    // is made.
    QCOMPARE(control.exchange(streamRequest({stream}, "TEARDOWN")).status, 200);
    const auto replacement = control.exchange(streamRequest({remoteStream()}));
    const auto newId = plistDecode(replacement.body)
                           .toMap()
                           .value("streams")
                           .toList()
                           .first()
                           .toMap()
                           .value("streamID")
                           .toULongLong();
    QCOMPARE(control.exchange(remoteCommand(newId, hello.repeated(32))).status,
             200);
    // The 33rd queued reply closes only this connection, without any unbounded
    // growth.
    control.socket.write(control.records->encode(remoteCommand(newId, hello)));
    QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(other.exchange(request("POST", "/feedback")).status, 200);
  }
  void audioSessionAssociation_data() {
    QTest::addColumn<bool>("stereo");
    QTest::addColumn<bool>("explicitRoute");
    QTest::newRow("single-auto") << false << false;
    QTest::newRow("stereo-auto") << true << false;
    QTest::newRow("single-explicit") << false << true;
    QTest::newRow("stereo-explicit") << true << true;
  }
  void audioSessionAssociation() {
    QFETCH(bool, stereo);
    QFETCH(bool, explicitRoute);
    test::Receiver left("left"), right("right");
    NetworkRoute route;
    if (explicitRoute) {
      for (const auto &binding : NetworkBinding::available())
        if (binding.ipv4 == "127.0.0.1")
          route = NetworkRoute::resolve(binding);
      QVERIFY(!route.binding.automatic());
    }
    app::Timing timing;
    timing.settleMs = 0;
    timing.packetSamples = 352;
    timing.lateMs = 500;
    timing.teardownTimeoutMs = 100;
    audio::CaptureStream stream;
    stream.blockFrames = 352;
    stream.left = stream.right = {2, 16, false, false};
    stream.queue = std::make_shared<audio::CaptureQueue>(352, 704, 704, 8);
    stream.gapPolicy = audio::GapPolicy::Silence;
    QList<ReceiverEndpoint> endpoints{{QHostAddress::LocalHost, left.port()}};
    if (stereo)
      endpoints.append({QHostAddress::LocalHost, right.port()});
    AirPlaySession session(timing, stream, endpoints, nullptr,
                           {[] {}, [] {}, false}, route);
    connect(&session, &AirPlaySession::startCapture, &session,
            &AirPlaySession::captureStarted);
    QSignalSpy finished(&session, &AirPlaySession::finished);
    session.start();
    QTRY_VERIFY(!left.packets.isEmpty());
    QCOMPARE(left.error, QString());
    const auto info = left.sessionSetup.value("uglServerInfo").toMap();
    QCOMPARE(info.size(), 2);
    QCOMPARE(info.value("Port").typeId(), QMetaType::LongLong);
    QCOMPARE(info.value("Addresses").toList(),
             QVariantList{QString("127.0.0.1")});
    const auto port = quint16(info.value("Port").toUInt());
    QVERIFY(port);
    for (auto *receiver : stereo ? QList<test::Receiver *>{&left, &right}
                                 : QList<test::Receiver *>{&left}) {
      QCOMPARE(receiver->sessionSetup.value("uglServerInfo").toMap(), info);
      QCOMPARE(receiver->sessionSetup.value("senderSupportsRelay"),
               QVariant(true));
      QCOMPARE(receiver->sessionSetup.value("groupContainsGroupLeader"),
               QVariant(false));
      QCOMPARE(receiver->sessionSetup.value("isMultiSelectAirPlay"),
               QVariant(true));
    }
    Client control(port);
    control.pair();
    control.fairPlay();
    const auto setup = control.setup();
    Client event(quint16(setup.value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    QCOMPARE(
        event.exchange(request("POST", "/command", plistEncode(QVariantMap{})))
            .status,
        200);
    const auto count = left.packets.size();
    QTRY_VERIFY(left.packets.size() > count);
    session.stop();
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(finished[0][0].toJsonArray().isEmpty());
    QCOMPARE(left.teardowns, 1);
    if (stereo)
      QCOMPARE(right.teardowns, 1);
    QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
    Client closed(port);
    QTRY_VERIFY(closed.socket.error() ==
                QAbstractSocket::ConnectionRefusedError);
  }
  void fairPlayModes_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<QByteArray>("digest");
    // SHA-256 of complete upstream wire responses, independent of our table.
    QTest::newRow("mode0") << 0
                           << QByteArray("8e1a11ea61e4c397f30480910786893aaebb5"
                                         "bff59bf2074db9d7e84fda49dbc");
    QTest::newRow("mode1") << 1
                           << QByteArray("e3730ce34481b93312c97ecbebe5b2128ee16"
                                         "a8e4d0140e89904ac9ff7509df3");
    QTest::newRow("mode2") << 2
                           << QByteArray("c6ab6f9488d29f4c7ec395981cdd12f14a5fa"
                                         "5c51ce61ae6ce43c570f7eae989");
    QTest::newRow("mode3") << 3
                           << QByteArray("8c53e03e2a08e23558e48287851d1cf392970"
                                         "39a7a60bf906a582e22c92f826b");
  }
  void fairPlayModes() {
    QFETCH(int, mode);
    QFETCH(QByteArray, digest);
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client client(server.port());
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart(mode))).status, 470);
    client.pair();
    const auto first =
        client.exchange(fairPlayRequest(fairPlayStart(mode)), true);
    QCOMPARE(first.status, 200);
    QCOMPARE(first.headers.value("content-type"),
             QByteArray("application/octet-stream"));
    QCOMPARE(first.body.size(), 142);
    QCOMPARE(QCryptographicHash::hash(first.body, QCryptographicHash::Sha256)
                 .toHex(),
             digest);
    const auto finish = fairPlayFinish(char(mode + 1));
    const auto second = client.exchange(fairPlayRequest(finish), true);
    QCOMPARE(second.status, 200);
    QCOMPARE(second.body, QByteArray::fromHex("46504c590301040000000014") +
                              finish.right(20));
    QCOMPARE(client.exchange(request("SETUP", "/")).status, 400);
  }
  void fairPlayMalformedAndOrder() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client client(server.port());
    client.pair();
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayFinish())).status, 400);
    for (const QByteArray et :
         {QByteArray("8"), QByteArray(""), QByteArray("bad")})
      QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart(), et)).status,
               501);
    // All truncated messages, trailing bytes, unsupported modes/header fields.
    for (int size = 0; size < 16; ++size)
      QCOMPARE(
          client.exchange(fairPlayRequest(fairPlayStart().first(size))).status,
          400);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart() + 'x')).status,
             400);
    for (int mode : {4, 255})
      QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart(mode))).status,
               400);
    for (int index : {0, 4, 5, 6, 7, 8, 11}) {
      auto bad = fairPlayStart();
      bad[index] ^= 0x40;
      QCOMPARE(client.exchange(fairPlayRequest(bad)).status, 400);
    }
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayFinish())).status, 400);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart())).status, 200);
    for (int size = 0; size < 164; ++size)
      QCOMPARE(
          client.exchange(fairPlayRequest(fairPlayFinish().first(size))).status,
          400);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayFinish() + 'x')).status,
             400);
    auto bad = fairPlayFinish();
    bad[11] = 0;
    QCOMPARE(client.exchange(fairPlayRequest(bad)).status, 400);
    // Invalid input never advances the exchange or resets the HAP counters.
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayFinish())).status, 200);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayFinish())).status, 400);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart(3))).status, 200);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayStart(2))).status, 200);
    QCOMPARE(client.exchange(fairPlayRequest(fairPlayFinish())).status, 200);
  }
  void fairPlayConnectionIsolationAndRestart() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client first(server.port()), second(server.port());
    first.pair();
    second.pair();
    QCOMPARE(first.exchange(fairPlayRequest(fairPlayStart())).status, 200);
    QCOMPARE(second.exchange(fairPlayRequest(fairPlayFinish())).status, 400);
    QCOMPARE(second.exchange(fairPlayRequest(fairPlayStart(1))).status, 200);
    QCOMPARE(
        first.exchange(fairPlayRequest(fairPlayFinish('a'))).body.right(20),
        QByteArray(20, 'a'));
    QCOMPARE(
        second.exchange(fairPlayRequest(fairPlayFinish('b'))).body.right(20),
        QByteArray(20, 'b'));
    QCOMPARE(first.exchange(fairPlayRequest(fairPlayStart())).status, 200);
    server.stop();
    QTRY_COMPARE(first.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(second.socket.state(), QAbstractSocket::UnconnectedState);
    server.start(QHostAddress::LocalHost, {}, {});
    Client third(server.port());
    third.pair();
    QCOMPARE(third.exchange(fairPlayRequest(fairPlayFinish())).status, 400);
    QCOMPARE(third.exchange(fairPlayRequest(fairPlayStart())).status, 200);
    QCOMPARE(third.exchange(fairPlayRequest(fairPlayFinish())).status, 200);
  }
  void setupFieldsAndEventFraming() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto setup = control.setup();
    QCOMPARE(setup.size(), 2);
    QCOMPARE(setup.value("eventPort").typeId(), QMetaType::LongLong);
    const auto port = setup.value("eventPort").toUInt();
    QVERIFY(port > 0 && port <= 65535);
    QCOMPARE(setup.value("info").typeId(), QMetaType::QVariantMap);
    QCOMPARE(
        setup.value("info").toMap(),
        plistDecode(control.exchange(request("GET", "/info")).body).toMap());
    QCOMPARE(control.setup(), setup);
    auto noInfo = control.exchange(
        request("SETUP", "/",
                plistEncode(QVariantMap{{"isRemoteControlOnly", true},
                                        {"timingProtocol", "None"}})));
    QCOMPARE(noInfo.status, 200);
    QCOMPARE(plistDecode(noInfo.body).toMap(),
             (QVariantMap{{"eventPort", qlonglong(port)}}));
    Client event{quint16(port)};
    event.records = std::move(control.eventRecords);
    auto command =
        request("POST", "/command",
                plistEncode(QVariantMap{
                    {"type", "testEvent"},
                    {"name", QString(3000, 'x')},
                    {"params", QVariantMap{{"data", QByteArray(1200, 'a')}}}}),
                "X-Test-Marker: fragmentation\r\n");
    command.replace("CSeq: 1\r\n", "CSeq: 27\r\n");
    const auto response = event.exchange(command, true);
    QCOMPARE(response.status, 200);
    QCOMPARE(response.headers.value("cseq"), QByteArray("27"));
    QVERIFY(event.records->tx >
            1); // Fragmented across HAP records as well as TCP.
    QCOMPARE(control.setup(),
             setup); // Does not replace a connected event stream.
    QCOMPARE(event.exchange(request("POST", "/unknown")).status, 501);
    QCOMPARE(
        event.exchange(request("POST", "/command", "bplist00broken")).status,
        400);
    QCOMPARE(
        event.exchange(request("POST", "/command", plistEncode(QVariantList{})))
            .status,
        400);
    QCOMPARE(event.exchange(command).status, 200);
    QCOMPARE(control.exchange(request("RECORD", "/")).status, 200);
    QCOMPARE(control.exchange(request("POST", "/feedback")).status, 200);
    QCOMPARE(
        control.exchange(request("TEARDOWN", "/", plistEncode(QVariantMap{})))
            .status,
        200);
    QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void setupRejectsInvalidAndUnsupportedRequests() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    const QVariantMap valid{{"isRemoteControlOnly", true},
                            {"timingProtocol", "None"},
                            {"combinedGetInfoWithControlSetup", true}};
    const auto send = [&](const QVariant &value) {
      return control.exchange(request("SETUP", "/", plistEncode(value))).status;
    };
    QCOMPARE(send(valid), 470);
    control.pair();
    QCOMPARE(send(valid), 455);
    QCOMPARE(control.exchange(fairPlayRequest(fairPlayStart())).status, 200);
    QCOMPARE(send(valid), 455);
    QCOMPARE(control.exchange(fairPlayRequest(fairPlayFinish())).status, 200);
    QCOMPARE(send(QVariantList{}), 400);
    QCOMPARE(control.exchange(request("SETUP", "/", "bplist00bad")).status,
             400);
    for (const auto &key :
         {"isRemoteControlOnly", "combinedGetInfoWithControlSetup",
          "updateSessionRequest"}) {
      auto bad = valid;
      bad.insert(key, 1);
      QCOMPARE(send(bad), 400);
    }
    auto bad = valid;
    bad.insert("timingProtocol", QByteArray("None"));
    QCOMPARE(send(bad), 400);
    bad = valid;
    bad.insert("qualifier", "txtAirPlay");
    QCOMPARE(send(bad), 400);
    bad.insert("qualifier", QVariantList{1});
    QCOMPARE(send(bad), 400);
    bad = valid;
    bad.insert("timingProtocol", "PTP");
    QCOMPARE(send(bad), 501);
    bad = valid;
    bad.insert("isRemoteControlOnly", false);
    QCOMPARE(send(bad), 501);
    bad = valid;
    bad.insert("updateSessionRequest", true);
    QCOMPARE(send(bad), 501);
    bad = valid;
    bad.insert("streams", QVariantMap{});
    QCOMPARE(send(bad), 455);
    bad.insert("streams", QVariantList{QVariantMap{
                              {"type", 130}, {"clientTypeUUID", "test"}}});
    QCOMPARE(send(bad), 455);
    QCOMPARE(send(valid), 200);
    const auto first = control.setup();
    QCOMPARE(send(bad), 400);
    QCOMPARE(control.setup(), first);
    QCOMPARE(control.exchange(fairPlayRequest(fairPlayStart())).status, 455);
    QCOMPARE(
        control
            .exchange(request(
                "TEARDOWN", "/",
                plistEncode(QVariantMap{
                    {"streams", QVariantList{QVariantMap{{"type", 130}}}}})))
            .status,
        400);
    QCOMPARE(control.exchange(request("TEARDOWN", "/")).status, 200);
    QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void sharedRemoteStreamMessagesAndTeardown() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto initial = control.setup();
    Client event(quint16(initial.value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    const auto response = control.exchange(
        streamRequest({remoteStream("a"), remoteStream("b")}), true);
    QCOMPARE(response.status, 200);
    QCOMPARE(response.headers.value("content-type"),
             QByteArray("application/x-apple-binary-plist"));
    const auto body = plistDecode(response.body).toMap();
    QCOMPARE(body.size(), 1);
    QCOMPARE(body.value("streams").typeId(), QMetaType::QVariantList);
    const auto streams = body.value("streams").toList();
    QCOMPARE(streams.size(), 2);
    const auto a = streams[0].toMap().value("streamID").toULongLong();
    const auto b = streams[1].toMap().value("streamID").toULongLong();
    QVERIFY(a > 0 && b > a);
    for (const auto &stream : streams) {
      QCOMPARE(stream.toMap().size(), 2); // No fabricated dataPort or keys.
      QCOMPARE(stream.toMap().value("type"), QVariant(qlonglong(130)));
      QCOMPARE(stream.toMap().value("streamID").typeId(), QMetaType::LongLong);
    }
    auto command = remoteCommand(a);
    command.replace("CSeq: 1\r\n", "CSeq: 37\r\n");
    const auto acknowledgement = control.exchange(command, true);
    QCOMPARE(acknowledgement.status, 200);
    QCOMPARE(acknowledgement.headers.value("cseq"), QByteArray("37"));
    QVERIFY(acknowledgement.body.isEmpty()); // No invented inner MRP reply.
    QCOMPARE(event.exchange(remoteCommand(b), true).status, 200);
    QCOMPARE(control.exchange(remoteCommand(999999)).status, 454);
    const auto received = control.exchange(
        request("SETUP", "/",
                plistEncode(QVariantMap{{"isRemoteControlOnly", true},
                                        {"timingProtocol", "None"}})));
    QCOMPARE(received.status, 200);
    QCOMPARE(control.exchange(remoteCommand(a)).status, 200);
    QCOMPARE(control.exchange(streamRequest({streams[0]}, "TEARDOWN")).status,
             200);
    QCOMPARE(control.exchange(remoteCommand(a)).status, 454);
    QCOMPARE(event.exchange(remoteCommand(b)).status, 200);
    QCOMPARE(control.exchange(streamRequest({streams[0]}, "TEARDOWN")).status,
             454);
    const auto replacement =
        control.exchange(streamRequest({remoteStream("a")}));
    const auto replacementId = plistDecode(replacement.body)
                                   .toMap()
                                   .value("streams")
                                   .toList()
                                   .first()
                                   .toMap()
                                   .value("streamID")
                                   .toULongLong();
    QVERIFY(replacementId > b);
    QCOMPARE(control.exchange(remoteCommand(a)).status, 454);
    QCOMPARE(control.exchange(remoteCommand(replacementId)).status, 200);
    QCOMPARE(control.exchange(request("TEARDOWN", "/")).status, 200);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void sharedRemoteStreamValidationAndCapacity() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    QCOMPARE(control.exchange(streamRequest({remoteStream()})).status, 470);
    control.pair();
    QCOMPARE(control.exchange(streamRequest({remoteStream()})).status, 455);
    control.fairPlay();
    QCOMPARE(control.exchange(streamRequest({remoteStream()})).status, 455);
    control.setup();
    QCOMPARE(control.exchange(streamRequest({})).status, 400);
    QCOMPARE(control.exchange(streamRequest({QVariant(1)})).status, 400);
    for (const auto &key :
         {"type", "controlType", "clientTypeUUID", "clientUUID", "channelID"}) {
      auto bad = remoteStream();
      bad.remove(key);
      QCOMPARE(control.exchange(streamRequest({bad})).status, 400);
    }
    for (const auto &key : {"type", "controlType"}) {
      auto bad = remoteStream();
      bad.insert(key, QString("130"));
      QCOMPARE(control.exchange(streamRequest({bad})).status, 400);
    }
    auto bad = remoteStream();
    bad.insert("controlType", 1);
    QCOMPARE(control.exchange(streamRequest({bad})).status, 501);
    bad = remoteStream();
    bad.insert("clientTypeUUID", "A6B27562-B43A-4F2D-B75F-82391E250194");
    QCOMPARE(control.exchange(streamRequest({remoteStream(), bad})).status,
             501);
    for (const auto &key : {"wantsDedicatedSocket", "sendMessageAsIs"}) {
      bad = remoteStream();
      bad.insert(key, 1);
      QCOMPARE(control.exchange(streamRequest({bad})).status, 400);
      bad.insert(key, true);
      QCOMPARE(control.exchange(streamRequest({bad})).status, 501);
    }
    bad = remoteStream();
    bad.insert("seed", qlonglong(1));
    QCOMPARE(control.exchange(streamRequest({bad})).status, 501);
    bad = remoteStream();
    bad.insert("channelID", QString(257, 'x'));
    QCOMPARE(control.exchange(streamRequest({bad})).status, 400);
    QVariantList eight;
    for (int i = 0; i < 8; ++i)
      eight.append(remoteStream(QString::number(i)));
    // Failed batches above must not consume a slot or ID.
    auto response = control.exchange(streamRequest(eight));
    QCOMPARE(response.status, 200);
    auto streams = plistDecode(response.body).toMap().value("streams").toList();
    QCOMPARE(streams.size(), 8);
    QCOMPARE(streams[0].toMap().value("streamID").toULongLong(), quint64(1));
    QCOMPARE(control.exchange(streamRequest({remoteStream()})).status, 453);
    const auto firstId = streams[0].toMap().value("streamID").toULongLong();
    auto nonexistent = streams[0].toMap();
    nonexistent.insert("streamID", qlonglong(99999));
    QCOMPARE(
        control.exchange(streamRequest({streams[0], nonexistent}, "TEARDOWN"))
            .status,
        454);
    QCOMPARE(control.exchange(remoteCommand(firstId)).status,
             200); // Atomic teardown.
    QCOMPARE(
        control.exchange(streamRequest({streams[0], streams[0]}, "TEARDOWN"))
            .status,
        400);
    QCOMPARE(control.exchange(streamRequest(streams, "TEARDOWN")).status, 200);
    response = control.exchange(streamRequest({remoteStream()}));
    QCOMPARE(response.status, 200);
    const auto id = plistDecode(response.body)
                        .toMap()
                        .value("streams")
                        .toList()[0]
                        .toMap()
                        .value("streamID")
                        .toULongLong();
    QVERIFY(id > 8);
    for (const auto &header :
         {QByteArray(""), QByteArray("-1"), QByteArray("+1"), QByteArray("1x"),
          QByteArray("18446744073709551616")}) {
      QCOMPARE(
          control
              .exchange(request("POST", "/command", plistEncode(QVariantMap{}),
                                "X-Apple-StreamID: " + header + "\r\n"))
              .status,
          400);
    }
    const auto route = "X-Apple-StreamID: " + QByteArray::number(id) + "\r\n";
    for (const auto &body :
         {QVariant(QVariantMap{}),
          QVariant(QVariantMap{{"params", QVariantMap{{"data", "wrong"}}}}),
          QVariant(
              QVariantMap{{"params", QVariantMap{{"data", QByteArray{}}}}})})
      QCOMPARE(
          control
              .exchange(request("POST", "/command", plistEncode(body), route))
              .status,
          400);
    QCOMPARE(control.exchange(remoteCommand(id)).status, 200);
  }
  void sharedRemoteStreamsStayWithinSession() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client first(server.port()), second(server.port());
    first.pair();
    first.fairPlay();
    first.setup();
    second.pair();
    second.fairPlay();
    second.setup();
    const auto allocate = [](Client &client) {
      const auto response = client.exchange(streamRequest({remoteStream()}));
      if (response.status != 200)
        throw std::runtime_error("Stream allocation failed");
      return plistDecode(response.body)
          .toMap()
          .value("streams")
          .toList()[0]
          .toMap()
          .value("streamID")
          .toULongLong();
    };
    const auto a = allocate(first), b = allocate(second);
    QVERIFY(a != b);
    QCOMPARE(first.exchange(remoteCommand(b)).status, 454);
    QCOMPARE(second.exchange(remoteCommand(a)).status, 454);
    QCOMPARE(second.exchange(remoteCommand(b)).status, 200);
    first.socket.abort();
    QCOMPARE(second.exchange(remoteCommand(b)).status, 200);
    second.socket.write(second.records->encode(remoteCommand(b)).first(7));
    server.stop();
    QTRY_COMPARE(second.socket.state(), QAbstractSocket::UnconnectedState);
    server.start(QHostAddress::LocalHost, {}, {});
    Client replacement(server.port());
    replacement.pair();
    replacement.fairPlay();
    replacement.setup();
    const auto c = allocate(replacement);
    QVERIFY(c > b);
    QCOMPARE(replacement.exchange(remoteCommand(a)).status, 454);
    QCOMPARE(replacement.exchange(remoteCommand(b)).status, 454);
    QCOMPARE(replacement.exchange(remoteCommand(c)).status, 200);
  }
  void eventAuthenticationIsolationAndStop() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client first(server.port()), second(server.port());
    first.pair();
    second.pair();
    first.fairPlay();
    second.fairPlay();
    const auto firstPort = quint16(first.setup().value("eventPort").toUInt());
    const auto secondPort = quint16(second.setup().value("eventPort").toUInt());
    QVERIFY(firstPort != secondPort);
    Client wrong(firstPort);
    // A different control session cannot authenticate on this event listener.
    const auto command = request("POST", "/command",
                                 plistEncode(QVariantMap{{"type", "observe"}}));
    wrong.socket.write(second.eventRecords->encode(command));
    QTRY_COMPARE(wrong.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(first.socket.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(second.exchange(request("GET", "/info")).status, 200);
    // second's own event counter was consumed above. Pair a new session
    // instead.
    second.socket.abort();
    Client third(server.port());
    third.pair();
    third.fairPlay();
    Client event(quint16(third.setup().value("eventPort").toUInt()));
    event.records = std::move(third.eventRecords);
    QCOMPARE(event.exchange(command).status, 200);
    auto corrupt = event.records->encode(command);
    corrupt.back() ^= 1;
    event.socket.write(corrupt);
    QTRY_COMPARE(third.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
    Client last(server.port());
    last.pair();
    last.fairPlay();
    Client pending(quint16(last.setup().value("eventPort").toUInt()));
    pending.records = std::move(last.eventRecords);
    QCOMPARE(pending.exchange(command).status, 200);
    const auto partial = pending.records->encode(command);
    pending.socket.write(partial.first(7));
    QTest::qWait(2);
    server.stop();
    QTRY_COMPARE(last.socket.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(pending.socket.state(), QAbstractSocket::UnconnectedState);
    server.start(QHostAddress::LocalHost, {}, {});
    Client fresh(server.port());
    fresh.pair();
    fresh.fairPlay();
    Client freshEvent(quint16(fresh.setup().value("eventPort").toUInt()));
    freshEvent.records = std::move(fresh.eventRecords);
    QCOMPARE(freshEvent.exchange(command).status, 200);
    fresh.socket.abort();
    QTRY_COMPARE(freshEvent.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void eventRejectsMalformedAndExtraConnections() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    const auto command =
        request("POST", "/command", plistEncode(QVariantMap{}));
    const QList<QByteArray> invalid{
        QByteArray("POST /command RTSP/1.0\r\nContent-Length: 0\r\n\r\n"),
        QByteArray("POST /command RTSP/1.0\r\nCSeq: x\r\n\r\n"),
        QByteArray("POST /command RTSP/1.0\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n"),
        QByteArray("POST /command RTSP/1.0\r\nCSeq: 1\r\nContent-Length: "
                   "999999999\r\n\r\n")};
    for (const auto &bytes : invalid) {
      Client control(server.port());
      control.pair();
      control.fairPlay();
      Client event(quint16(control.setup().value("eventPort").toUInt()));
      event.records = std::move(control.eventRecords);
      QCOMPARE(event.exchange(command).status, 200);
      event.socket.write(event.records->encode(bytes));
      QTRY_COMPARE(control.socket.state(), QAbstractSocket::UnconnectedState);
      QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
    }
    Client control(server.port());
    control.pair();
    control.fairPlay();
    const auto setup = control.setup();
    const auto port = quint16(setup.value("eventPort").toUInt());
    Client event(port);
    event.records = std::move(control.eventRecords);
    QCOMPARE(event.exchange(command).status, 200);
    Client extra(port);
    QTRY_COMPARE(extra.socket.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(event.exchange(command).status, 200);
    QCOMPARE(control.setup(), setup);
    Client pending(server.port());
    pending.pair();
    pending.fairPlay();
    const auto unusedPort =
        quint16(pending.setup().value("eventPort").toUInt());
    pending.socket.abort();
    QTest::qWait(10);
    Client stale(unusedPort);
    QTRY_COMPARE(stale.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void eventDeadlineAndDisconnect() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client control(server.port());
    control.pair();
    control.fairPlay();
    Client event(quint16(control.setup().value("eventPort").toUInt()));
    event.records = std::move(control.eventRecords);
    QCOMPARE(
        event.exchange(request("POST", "/command", plistEncode(QVariantMap{})))
            .status,
        200);
    // Valid traffic must not extend the existing absolute observation deadline.
    QTRY_COMPARE_WITH_TIMEOUT(control.socket.state(),
                              QAbstractSocket::UnconnectedState, 16000);
    QTRY_COMPARE(event.socket.state(), QAbstractSocket::UnconnectedState);
    Client next(server.port());
    next.pair();
    next.fairPlay();
    Client nextEvent(quint16(next.setup().value("eventPort").toUInt()));
    nextEvent.records = std::move(next.eventRecords);
    QCOMPARE(
        nextEvent
            .exchange(request("POST", "/command", plistEncode(QVariantMap{})))
            .status,
        200);
    nextEvent.socket.abort();
    QTRY_COMPARE(next.socket.state(), QAbstractSocket::UnconnectedState);
  }
  void randomChallengeAndProofValidation() {
    TransientPairing first, second;
    const auto a = tlvDecode(first.challenge()),
               b = tlvDecode(second.challenge());
    QVERIFY(a.value(2) != b.value(2));
    QVERIFY(a.value(3) != b.value(3));
    auto proof = srp(a.value(2), a.value(3));
    const auto result = first.verify(proof.publicKey, proof.proof);
    QCOMPARE(result.key, proof.key);
    QCOMPARE(result.proof, proof.expected);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             first.verify(proof.publicKey, proof.proof));
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        second.verify(QByteArray(384, '\0'), QByteArray(64, '\0')));
  }
  void infoAndEncryptedRequest() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client client(server.port());
    auto response = client.exchange(
        request("GET", "/info",
                plistEncode(QVariantMap{
                    {"qualifier", QVariantList{QString("txtAirPlay")}}})),
        true);
    QCOMPARE(response.status, 200);
    const auto info = plistDecode(response.body).toMap();
    QVERIFY(info.value("txtAirPlay").toByteArray().contains("model=AirPlayQt"));
    QCOMPARE(info.value("name").toString(), "AirPlayQt");
    client.pair();
    client.fairPlay();
    // Multiple authenticated HAP frames and arbitrary TCP fragments.
    const QVariantMap setup{{"isRemoteControlOnly", true},
                            {"timingProtocol", "None"},
                            {"name", QString(1600, 'x')},
                            {"shk", QByteArray("secret-key")}};
    response = client.exchange(
        request("SETUP", "rtsp://airplayqt/session", plistEncode(setup),
                "Content-Type: application/x-apple-binary-plist\r\n"),
        true);
    QCOMPARE(response.status, 200);
  }
  void authenticationFailures() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client client(server.port());
    QCOMPARE(client.exchange(request("SETUP", "/")).status, 470);
    QCOMPARE(client
                 .exchange(request("POST", "/pair-setup",
                                   QByteArray::fromHex("060103"),
                                   "X-Apple-HKP: 4\r\n"))
                 .status,
             400);
    QCOMPARE(client
                 .exchange(request("POST", "/pair-setup",
                                   QByteArray::fromHex("060101000100"),
                                   "X-Apple-HKP: 4\r\n"))
                 .status,
             400);
    auto proof = client.challenge();
    proof.proof[0] ^= 1;
    const auto rejected =
        client.exchange(request("POST", "/pair-setup",
                                tlvEncode({{6, QByteArray(1, char(3))},
                                           {3, proof.publicKey},
                                           {4, proof.proof}}),
                                "X-Apple-HKP: 4\r\n"));
    QCOMPARE(tlvDecode(rejected.body).value(7), QByteArray(1, char(2)));
    QTRY_COMPARE(client.socket.state(), QAbstractSocket::UnconnectedState);
    Client next(server.port());
    next.pair();
    QCOMPARE(next.exchange(request("SETUP", "/")).status, 455);
  }
  void coalescedEncryptionTransition() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client client(server.port());
    const auto proof = client.challenge();
    HapRecords records(
        hkdf(proof.key, "Control-Salt", "Control-Write-Encryption-Key"),
        hkdf(proof.key, "Control-Salt", "Control-Read-Encryption-Key"));
    auto bytes = request("POST", "/pair-setup",
                         tlvEncode({{6, QByteArray(1, char(3))},
                                    {3, proof.publicKey},
                                    {4, proof.proof}}),
                         "X-Apple-HKP: 4\r\n");
    bytes += records.encode(request("SETUP", "/"));
    // exchange consumes only M4; any encrypted response remains in plain.
    const auto m4 = client.exchange(bytes);
    QCOMPARE(tlvDecode(m4.body).value(4), proof.expected);
    QByteArray wire = std::move(client.plain), plain;
    QElapsedTimer deadline;
    deadline.start();
    std::optional<RtspResponse> response;
    do {
      wire += client.socket.readAll();
      plain += records.decodeAvailable(wire);
      response = parseResponse(plain);
      if (!response)
        QTest::qWait(1);
    } while (!response && deadline.elapsed() < 2000);
    QVERIFY(response.has_value());
    QCOMPARE(response->status, 455);
  }
  void corruptRecordAndIndependentConnections() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client first(server.port()), second(server.port());
    first.pair();
    second.pair();
    auto wire = first.records->encode(request("SETUP", "/"));
    wire[wire.size() - 1] ^= 1;
    first.socket.write(wire);
    QTRY_COMPARE(first.socket.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(second.exchange(request("GET", "/info")).status, 200);
    server.stop();
    QTRY_COMPARE(second.socket.state(), QAbstractSocket::UnconnectedState);
    server.start(QHostAddress::LocalHost, {}, {});
    Client third(server.port());
    third.pair();
    QCOMPARE(third.exchange(request("SETUP", "/")).status, 455);
  }
  void malformedAndOversizedRequests() {
    InboundControlServer server;
    server.start(QHostAddress::LocalHost, {}, {});
    Client first(server.port());
    first.socket.write(
        "GET /info RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 999999999\r\n\r\n");
    QTRY_COMPARE(first.socket.state(), QAbstractSocket::UnconnectedState);
    Client second(server.port());
    second.socket.write("GET /info RTSP/1.0\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n");
    QTRY_COMPARE(second.socket.state(), QAbstractSocket::UnconnectedState);
    Client third(server.port());
    QCOMPARE(third.exchange(request("GET", "/info")).status, 200);
  }
};
QTEST_GUILESS_MAIN(Tests)
#include "InboundControlTests.moc"
