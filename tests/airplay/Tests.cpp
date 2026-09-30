#include "airplay/AirPlaySession.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QTcpServer>
#include <QtTest>
#include <openssl/bn.h>

using namespace airplay;
namespace {
// Independent server half of HAP SRP, used by the in-process receiver.
class ServerSrp {
  using Big = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
  using Context = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;
  static Big number(const QByteArray &data) {
    return Big(
        BN_bin2bn(reinterpret_cast<const unsigned char *>(data.constData()),
                  int(data.size()), nullptr),
        BN_clear_free);
  }
  static QByteArray encoded(const BIGNUM *value, int width = 0) {
    QByteArray out(width ? width : BN_num_bytes(value), Qt::Uninitialized);
    if (BN_bn2binpad(value, reinterpret_cast<unsigned char *>(out.data()),
                     int(out.size())) != out.size())
      throw Error("Mock BN encode");
    return out;
  }
  static void check(int result) {
    if (result != 1)
      throw Error("Mock BN operation");
  }
  Context context_{BN_CTX_new(), BN_CTX_free};
  Big n_{BN_get_rfc3526_prime_3072(nullptr), BN_clear_free};
  Big g_ = number(QByteArray::fromHex("05"));
  Big b_ = number(QByteArray::fromHex("123456789abcdef0123456789abcdef0"));
  Big verifier_{BN_new(), BN_clear_free};
  Big public_{BN_new(), BN_clear_free};

public:
  QByteArray salt = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),
             key;
  ServerSrp() {
    auto x = number(sha512(salt + sha512("Pair-Setup:3939")));
    auto k = number(sha512(encoded(n_.get(), 384) + encoded(g_.get(), 384)));
    Big gb(BN_new(), BN_clear_free);
    check(BN_mod_exp(verifier_.get(), g_.get(), x.get(), n_.get(),
                     context_.get()));
    check(BN_mod_exp(gb.get(), g_.get(), b_.get(), n_.get(), context_.get()));
    check(BN_mod_mul(public_.get(), k.get(), verifier_.get(), n_.get(),
                     context_.get()));
    check(BN_mod_add(public_.get(), public_.get(), gb.get(), n_.get(),
                     context_.get()));
  }
  QByteArray publicKey() const { return encoded(public_.get()); }
  QByteArray prove(const QByteArray &client, const QByteArray &proof) {
    auto a = number(client);
    auto u =
        number(sha512(encoded(a.get(), 384) + encoded(public_.get(), 384)));
    Big base(BN_new(), BN_clear_free), shared(BN_new(), BN_clear_free);
    check(BN_mod_exp(base.get(), verifier_.get(), u.get(), n_.get(),
                     context_.get()));
    check(
        BN_mod_mul(base.get(), base.get(), a.get(), n_.get(), context_.get()));
    check(BN_mod_exp(shared.get(), base.get(), b_.get(), n_.get(),
                     context_.get()));
    key = sha512(encoded(shared.get()));
    auto mixed = sha512(encoded(n_.get())),
         gh = sha512(QByteArray::fromHex("05"));
    for (int i = 0; i < 64; ++i)
      mixed[i] = char(uint8_t(mixed[i]) ^ uint8_t(gh[i]));
    const auto expected = sha512(mixed + sha512("Pair-Setup") + salt + client +
                                 publicKey() + key);
    if (proof != expected)
      throw Error("Mock rejected client proof");
    return sha512(client + proof + key);
  }
};
class Receiver : public QObject {
public:
  enum class Failure {
    None,
    InfoTimeout,
    SessionSetup,
    Volume,
    BadProof,
    KeepAlive
  };
  QTcpServer server, eventServer;
  QUdpSocket data, control;
  QTcpSocket *socket = nullptr;
  QByteArray wire, plain;
  std::unique_ptr<HapRecords> records;
  ServerSrp srp;
  Failure failure = Failure::None;
  QString id, stereo = "pair-id", error;
  int volumeRequests = 0, teardowns = 0, options = 0, optionsDelay = 0;
  QList<double> volumes;
  QVariantMap sessionSetup;
  QList<QByteArray> packets, retransmits;
  quint16 clientControl = 0;
  explicit Receiver(QString identity) : id(std::move(identity)) {
    if (!server.listen(QHostAddress::LocalHost, 0) ||
        !eventServer.listen(QHostAddress::LocalHost, 0) ||
        !data.bind(QHostAddress::LocalHost, 0) ||
        !control.bind(QHostAddress::LocalHost, 0))
      throw Error("Mock sockets failed");
    connect(&server, &QTcpServer::newConnection, this, [this] {
      socket = server.nextPendingConnection();
      socket->setParent(this);
      wire.clear();
      plain.clear();
      records.reset();
      connect(socket, &QTcpSocket::readyRead, this, [this] {
        try {
          read();
        } catch (const std::exception &e) {
          error = QString::fromUtf8(e.what());
          socket->abort();
        }
      });
    });
    connect(&eventServer, &QTcpServer::newConnection, this, [this] {
      auto *client = eventServer.nextPendingConnection();
      client->setParent(this);
      connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
    });
    connect(&data, &QUdpSocket::readyRead, this, [this] {
      while (data.hasPendingDatagrams())
        packets.append(data.receiveDatagram().data());
    });
    connect(&control, &QUdpSocket::readyRead, this, [this] {
      while (control.hasPendingDatagrams()) {
        const auto packet = control.receiveDatagram().data();
        if (packet.size() > 1 && uint8_t(packet[1]) == 0xd6)
          retransmits.append(packet);
      }
    });
  }
  quint16 port() const { return server.serverPort(); }
  void retransmit(uint16_t first, uint16_t count) {
    QByteArray request = QByteArray::fromHex("80551234");
    appendBe(request, first, 2);
    appendBe(request, count, 2);
    control.writeDatagram(request, QHostAddress::LocalHost, clientControl);
  }

private:
  void respond(const QByteArray &cseq, const QByteArray &body = {},
               int code = 200) {
    auto response = "RTSP/1.0 " + QByteArray::number(code) +
                    " Result\r\nCSeq: " + cseq +
                    "\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\n\r\n" + body;
    if (records)
      response = records->encode(response);
    // Fragment headers/records across TCP writes to exercise incremental
    // parsing.
    socket->write(response.left(1));
    socket->write(response.mid(1));
  }
  void read() {
    wire += socket->readAll();
    if (records) {
      while (wire.size() >= 2) {
        int length = uint8_t(wire[0]) + (uint8_t(wire[1]) << 8);
        if (wire.size() < length + 18)
          break;
        plain += records->decode(wire.left(2), wire.mid(2, length + 16));
        wire.remove(0, length + 18);
      }
    } else {
      plain += wire;
      wire.clear();
    }
    for (;;) {
      const auto end = plain.indexOf("\r\n\r\n");
      if (end < 0)
        return;
      const auto lines = plain.left(end).split('\n');
      const auto requestLine = lines[0].trimmed().split(' ');
      QMap<QByteArray, QByteArray> headers;
      for (qsizetype i = 1; i < lines.size(); ++i) {
        const auto line = lines[i].trimmed();
        const auto colon = line.indexOf(':');
        headers.insert(line.left(colon).toLower(),
                       line.mid(colon + 1).trimmed());
      }
      const int size = headers["content-length"].toInt();
      if (plain.size() < end + 4 + size)
        return;
      const auto body = plain.mid(end + 4, size);
      plain.remove(0, end + 4 + size);
      const auto method = requestLine[0], path = requestLine[1],
                 cseq = headers["cseq"];
      if (path == "/info") {
        if (failure == Failure::InfoTimeout)
          continue;
        QByteArray txt;
        for (const auto &item : {QByteArray("tsid=") + stereo.toUtf8(),
                                 QByteArray("tsm=") + id.toUtf8()}) {
          txt.append(char(item.size()));
          txt += item;
        }
        respond(cseq, plistEncode(QVariantMap{
                          {"name", id},
                          {"deviceID", id},
                          {"txtAirPlay", txt},
                          {"initialVolume", id == "left" ? -20. : -30.}}));
      } else if (path == "/pair-setup") {
        const auto fields = tlvDecode(body);
        if (fields.value(6) == QByteArray::fromHex("01"))
          respond(cseq, tlvEncode({{6, QByteArray::fromHex("02")},
                                   {2, srp.salt},
                                   {3, srp.publicKey()}}));
        else {
          auto proof = srp.prove(fields.value(3), fields.value(4));
          if (failure == Failure::BadProof)
            proof[0] = char(uint8_t(proof[0]) ^ 1);
          respond(cseq,
                  tlvEncode({{6, QByteArray::fromHex("04")}, {4, proof}}));
          records = std::make_unique<HapRecords>(
              hkdf(srp.key, "Control-Salt", "Control-Read-Encryption-Key"),
              hkdf(srp.key, "Control-Salt", "Control-Write-Encryption-Key"));
        }
      } else if (method == "SETUP") {
        const auto value = plistDecode(body).toMap();
        if (!value.contains("streams"))
          sessionSetup = value;
        if (value.contains("streams")) {
          auto stream = value["streams"].toList()[0].toMap();
          clientControl = quint16(stream["controlPort"].toUInt());
          if (stream["sr"].toInt() != 44100 || stream["spf"].toInt() != 352 ||
              stream["shk"].toByteArray() != srp.key.left(32))
            throw Error("Mock stream mismatch");
          const QVariantMap ports{{"dataPort", int(data.localPort())},
                                  {"controlPort", int(control.localPort())}};
          respond(cseq,
                  plistEncode(QVariantMap{{"streams", QVariantList{ports}}}));
        } else if (failure == Failure::SessionSetup)
          respond(cseq, {}, 500);
        else
          respond(cseq, plistEncode(QVariantMap{
                            {"eventPort", int(eventServer.serverPort())}}));
      } else if (method == "SET_PARAMETER") {
        ++volumeRequests;
        volumes.append(body.mid(8).trimmed().toDouble());
        respond(cseq, {},
                failure == Failure::Volume && volumeRequests > 1 ? 500 : 200);
      } else if (method == "OPTIONS") {
        ++options;
        const int code = failure == Failure::KeepAlive ? 500 : 200;
        QTimer::singleShot(optionsDelay, this,
                           [this, cseq, code] { respond(cseq, {}, code); });
      } else if (method == "TEARDOWN") {
        ++teardowns;
        respond(cseq);
      } else
        respond(cseq);
    }
  }
};
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
    QCOMPARE(done[0][0].toString(), QString{});
    QCOMPARE(f.left.teardowns, 1);
    QVERIFY(!f.input.isActive());
    for (const auto &entry : logs)
      QVERIFY(!entry[0].toString().startsWith("[VST速率"));
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
        if (entry[0].toString().startsWith("[VST速率] 窗口"))
          return entry[0].toString();
      return QString{};
    };
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(!periodic().isEmpty(), 4000);
    const auto match = QRegularExpression("输入 ([0-9.]+) 帧/s；发送 ([0-9.]+) 帧/s")
                           .match(periodic());
    QVERIFY(match.hasMatch());
    QVERIFY(match.captured(1).toDouble() > 42000);
    QVERIFY(match.captured(1).toDouble() < 46200);
    QVERIFY(match.captured(2).toDouble() > 38000);
    QVERIFY(periodic().contains("poll 最大间隔"));
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QVERIFY(done[0][0].toString().isEmpty());
    QVERIFY(std::any_of(logs.begin(), logs.end(), [](const auto &entry) {
      return entry[0].toString().startsWith("[VST速率·停止]");
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
    QVERIFY(done[0][0].toString().contains("积压"));
    QString final;
    for (const auto &entry : logs)
      if (entry[0].toString().startsWith("[VST速率·停止]"))
        final = entry[0].toString();
    QVERIFY(!final.isEmpty());
    const auto match = QRegularExpression("输入 ([0-9.]+) 帧/s；发送 ([0-9.]+) 帧/s")
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
    QVERIFY(!done[0][0].toString().isEmpty());
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
    QCOMPARE(done[0][0].toString(), QString{});
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
    QVERIFY(!done[0][0].toString().isEmpty());
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
    QCOMPARE(done[0][0].toString(), QString{});
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
    QVERIFY(!done[0][0].toString().isEmpty());
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
    QVERIFY(!done[0][0].toString().isEmpty());
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
      QCOMPARE(done[0][0].toString(), QString{});
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
    QVERIFY(!done[0][0].toString().isEmpty());
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
    QVERIFY(done[0][0].toString().contains("断流"));
  }
};
QTEST_GUILESS_MAIN(ProtocolTests)
#include "Tests.moc"
