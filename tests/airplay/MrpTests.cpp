#include "airplay/MrpSession.h"
#include <QtTest>

namespace {
// Synthetic fixtures using the published proto2 field numbers, not captured
// identities. DeviceInfo: type=15, identifier="req", extension 20
// {name="test"}.
const auto Hello = QByteArray::fromHex("10080f1203726571a20106120474657374");
const auto Subscribe = QByteArray::fromHex("0e08101203726571aa010408011001");
const auto Connected = QByteArray::fromHex("070826d202020802");
const auto Keyboard = QByteArray::fromHex("0708181203726571");
const auto Unsupported = QByteArray::fromHex("0708631203726571");
quint64 takeVarint(QByteArray &data) {
  quint64 result = 0;
  for (int shift = 0; shift < 70; shift += 7) {
    if (data.isEmpty())
      throw std::runtime_error("truncated test response");
    const auto c = quint8(data.front());
    data.remove(0, 1);
    result |= quint64(c & 127) << shift;
    if (!(c & 128))
      return result;
  }
  throw std::runtime_error("invalid test response");
}
QByteArray envelope(QByteArray data) {
  const auto length = takeVarint(data);
  if (length != quint64(data.size()))
    throw std::runtime_error("wrong response length");
  return data;
}
QByteArray v(quint64 x) {
  QByteArray b;
  do {
    auto c = quint8(x & 127);
    x >>= 7;
    b += char(c | (x ? 128 : 0));
  } while (x);
  return b;
}
QByteArray b(int tag, QByteArray value) {
  return v((quint64(tag) << 3) | 2) + v(value.size()) + value;
}
QByteArray n(int tag, quint64 value) { return v(quint64(tag) << 3) + v(value); }
QByteArray message(int type, QByteArray inner, int extension,
                   QByteArray id = "req") {
  const auto body = n(1, type) + b(2, id) + b(extension, inner);
  return v(body.size()) + body;
}
QMap<int, QList<QVariant>> decode(QByteArray data) {
  QMap<int, QList<QVariant>> result;
  while (!data.isEmpty()) {
    const auto key = takeVarint(data);
    const auto tag = int(key >> 3), wire = int(key & 7);
    if (wire == 0)
      result[tag].append(QVariant::fromValue(takeVarint(data)));
    else {
      const auto size = wire == 2   ? takeVarint(data)
                        : wire == 5 ? 4
                        : wire == 1 ? 8
                                    : 0;
      if (!size || size > quint64(data.size()))
        throw std::runtime_error("test invalid field");
      result[tag].append(data.first(qsizetype(size)));
      data.remove(0, qsizetype(size));
    }
  }
  return result;
}
QMap<int, QByteArray> frames(QByteArray data) {
  QMap<int, QByteArray> result;
  while (!data.isEmpty()) {
    const auto length = takeVarint(data);
    const auto body = data.first(qsizetype(length));
    data.remove(0, qsizetype(length));
    result.insert(decode(body)[1].first().toInt(), body);
  }
  return result;
}
} // namespace
class MrpTests : public QObject {
  Q_OBJECT
private slots:
  void publishedFieldsAndConfirmedSnapshot() {
    airplay::ControlState state{
        true,
        airplay::PlaybackState::Paused,
        -15.,
        "actual-group",
        {{"actual-left", "Left", {}}, {"actual-right", "Right", {}}}};
    airplay::MrpSession session;
    session.receive(Hello, "own", state);
    const auto result = session.receive(Subscribe, "own", state);
    const auto published = frames(result.first().response);
    for (int type : {0, 4, 17, 37, 46, 47, 52, 64, 65, 72})
      QVERIFY(published.contains(type));
    const auto s = decode(decode(published[4])[9].first().toByteArray());
    QCOMPARE(s[6].first().toInt(), 2);
    const auto info = decode(s[1].first().toByteArray());
    QCOMPARE(info[5].first().toByteArray(), QByteArray::fromHex("00000000"));
    QCOMPARE(info[17].first().toInt(), 1);
    QVERIFY(!info.contains(3)); // No invented duration.
    const auto commands = decode(s[2].first().toByteArray())[1];
    QCOMPARE(commands.size(), 3);
    for (int i = 0; i < 3; ++i) {
      const auto command = decode(commands[i].toByteArray());
      QCOMPARE(command[1].first().toInt(), i + 1);
      QCOMPARE(command[2].first().toInt(), 1);
    }
    const auto path = decode(s[9].first().toByteArray());
    const auto client = decode(path[2].first().toByteArray());
    QCOMPARE(client[1].first().toLongLong(),
             QCoreApplication::applicationPid());
    QCOMPARE(client[2].first().toByteArray(), "org.airplayqt.app");
    const auto capabilities =
        decode(decode(published[17])[22].first().toByteArray());
    QCOMPARE(capabilities[2].first().toInt(),
             2); // Absolute, not unsupported relative controls.
    const auto volume = decode(decode(published[52])[56].first().toByteArray());
    QCOMPARE(volume[1].first().toByteArray(), QByteArray::fromHex("0000003f"));
    const auto outputs =
        decode(decode(published[65])[69].first().toByteArray())[1];
    QCOMPARE(outputs.size(), 2);
    QCOMPARE(decode(outputs[0].toByteArray())[2].first().toByteArray(),
             "actual-left");
    state.available = false;
    const auto stopped = frames(session.stateUpdate("own", state, true));
    QCOMPARE(
        decode(decode(stopped[4])[9].first().toByteArray())[6].first().toInt(),
        3);
  }
  void controlsAndOutputValidation() {
    airplay::ControlState state{true,
                                airplay::PlaybackState::Playing,
                                -22.5,
                                "group",
                                {{"device", "Name", {"mrp-device-uid"}}}};
    airplay::MrpSession session;
    session.receive(Hello, "own", state);
    for (int code : {1, 2, 3}) {
      const auto request =
          session.receive(message(1, n(1, code), 6), "own", state).first();
      QCOMPARE(request.request->action, code == 1 ? airplay::ControlAction::Play
                                        : code == 2
                                            ? airplay::ControlAction::Pause
                                            : airplay::ControlAction::Toggle);
      QVERIFY(
          request.response
              .isEmpty()); // No success before the actual session applies it.
      const auto result =
          decode(envelope(airplay::MrpSession::complete(request, true)));
      QCOMPARE(result[1].first().toInt(), 2);
      QCOMPARE(decode(result[7].first().toByteArray())[1].first().toInt(), 0);
    }
    const auto setVolume = QByteArray::fromHex("0d0000003f");
    for (const auto uid :
         {QByteArray{}, QByteArray("own"), QByteArray("group"),
          QByteArray("device"), QByteArray("mrp-device-uid")}) {
      const auto request =
          session.receive(message(51, setVolume + b(2, uid), 55), "own", state)
              .first();
      QCOMPARE(request.request->action, airplay::ControlAction::Volume);
      QCOMPARE(request.request->volumeDb, -15.);
    }
    QCOMPARE(session
                 .receive(message(51, setVolume + b(2, "stranger"), 55), "own",
                          state)
                 .first()
                 .errorCode,
             39);
    for (const auto bits : {"0000807f", "0000c07f", "000000bf", "0000c03f"})
      QCOMPARE(session
                   .receive(message(51,
                                    QByteArray::fromHex("0d") +
                                        QByteArray::fromHex(bits),
                                    55),
                            "own", state)
                   .first()
                   .errorCode,
               18);
    QCOMPARE(
        session
            .receive(message(1, n(1, 1) + b(3, b(2, b(2, "foreign.app"))), 6),
                     "own", state)
            .first()
            .errorCode,
        35);
    QCOMPARE(
        session.receive(message(1, n(1, 4), 6), "own", state).first().errorCode,
        6);
    QCOMPARE(
        session
            .receive(
                message(1, n(1, 1) + b(2, b(2, "controller") + n(4, 1)), 6),
                "own", state)
            .first()
            .request->action,
        airplay::ControlAction::Play);
    QVERIFY(
        session.receive(message(1, n(1, 1) + b(2, n(12, 123)), 6), "own", state)
            .first()
            .request == std::nullopt);
    const auto get =
        frames(session.receive(message(49, b(1, "device"), 53), "own", state)
                   .first()
                   .response);
    QCOMPARE(decode(decode(get[50])[54].first().toByteArray())[1]
                 .first()
                 .toByteArray(),
             QByteArray::fromHex("0000803e"));
    state.available = false;
    QVERIFY(
        session.receive(message(1, n(1, 1), 6), "own", state).first().request ==
        std::nullopt);
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        session.receive(message(1, n(1, 1), 6) + QByteArray::fromHex("00"),
                        "own", state));
  }
  void deviceInfoUpdateDoesNotRestartHandshake() {
    airplay::MrpSession session;
    session.receive(Hello, "own");
    const auto result =
        session
            .receive(message(37, b(2, "Receiver") + b(19, "uid"), 20, {}),
                     "own")
            .first();
    QCOMPARE(result.errorCode, 0);
    QCOMPARE(result.deviceUID, "uid");
    QVERIFY(result.response.isEmpty());
  }
  void outgoingFramesSplitAndReject() {
    QCOMPARE(airplay::MrpSession::outgoingFrames(Hello + Subscribe),
             (QList<QByteArray>{Hello, Subscribe}));
    QCOMPARE(airplay::MrpSession::outgoingFrames(Hello.repeated(32)).size(),
             32);
    QVERIFY(airplay::MrpSession::outgoingFrames({}).isEmpty());
    for (const auto &bad :
         {QByteArray::fromHex("04080f"), QByteArray::fromHex("80"),
          QByteArray::fromHex("00"),
          QByteArray::fromHex("ffffffffffffffffff02"), QByteArray(65537, 'x'),
          Hello.repeated(33)})
      QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                               airplay::MrpSession::outgoingFrames(bad));
    airplay::MrpSession session;
    airplay::ControlState state{true,
                                airplay::PlaybackState::Playing,
                                -15.,
                                "group",
                                {{"left", "Left", {}}, {"right", "Right", {}}}};
    session.receive(Hello, "own", state);
    auto batch = session.receive(Subscribe, "own", state).first().response;
    QByteArray rejoined;
    QList<int> types;
    for (const auto &frame : airplay::MrpSession::outgoingFrames(batch)) {
      rejoined += frame;
      types.append(decode(envelope(frame))[1].first().toInt());
    }
    QCOMPARE(
        rejoined,
        batch); // Exact preservation of all protobuf fields and identifiers.
    QCOMPARE(types, (QList<int>{37, 65, 46, 47, 4, 72, 17, 64, 52, 64, 52, 64,
                                52, 64, 52, 0}));
  }
  void deviceInfoAndInitialization() {
    airplay::MrpSession session;
    const auto result = session.receive(Hello, "own-identity");
    QCOMPARE(result.size(), 1);
    QCOMPARE(result[0].type, 15U);
    QCOMPARE(result[0].errorCode, 0);
    auto response = envelope(result[0].response);
    QVERIFY(response.startsWith(QByteArray::fromHex("080f12037265712000")));
    response.remove(0, 9);
    QCOMPARE(takeVarint(response), 162ULL); // field 20, length-delimited
    const auto infoSize = takeVarint(response);
    const auto info = response.first(qsizetype(infoSize));
    QVERIFY(info.contains(QByteArray::fromHex("0a0c") + "own-identity"));
    QVERIFY(info.contains("AirPlayQt"));
    QVERIFY(info.contains("org.airplayqt.app"));
    QVERIFY(!info.contains("com.apple"));
    QVERIFY(info.contains(QByteArray::fromHex(
        "4800500068007000"))); // No pairing/ACL/shared queue claims.
    response.remove(0, qsizetype(infoSize));
    QCOMPARE(takeVarint(response), 682ULL); // uniqueIdentifier, field 85
    QCOMPARE(takeVarint(response), 36ULL);
    QCOMPARE(response.size(), 36);
    const auto followup = session.receive(
        Connected + Subscribe + Keyboard + Unsupported, "own-identity");
    QCOMPARE(followup.size(), 4);
    QVERIFY(followup[0].response.isEmpty());
    QVERIFY(envelope(followup[1].response)
                .startsWith(QByteArray::fromHex("080012037265712000")));
    QVERIFY(envelope(followup[2].response)
                .startsWith(QByteArray::fromHex("081712037265712000")));
    QCOMPARE(followup[3].errorCode, 6);
    QVERIFY(envelope(followup[3].response)
                .startsWith(QByteArray::fromHex("080012037265712006")));
  }
  void splitAndCoalescedFrames() {
    for (qsizetype split = 1; split < Hello.size(); ++split) {
      airplay::MrpSession session;
      QVERIFY(session.receive(Hello.first(split), "own").isEmpty());
      QCOMPARE(session.receive(Hello.mid(split) + Subscribe, "own").size(), 2);
    }
    airplay::MrpSession session;
    QCOMPARE(session.receive(Hello + Hello, "own").size(),
             2); // Each valid request gets its own response.
    QVERIFY(session.receive(QByteArray::fromHex("020800"), "own")
                .first()
                .response.isEmpty());
  }
  void initializationAndIsolation() {
    airplay::MrpSession a, b;
    QCOMPARE(a.receive(Hello, "own").size(), 1);
    QCOMPARE(b.receive(Subscribe, "own").first().errorCode, 2);
    QCOMPARE(a.receive(Subscribe, "own").first().errorCode, 0);
    QVERIFY(a.receive(QByteArray::fromHex("020863"), "own")
                .first()
                .response.isEmpty());
    QCOMPARE(a.receive(QByteArray::fromHex("020863"), "own").first().errorCode,
             6);
  }
  void rejectsMalformed_data() {
    QTest::addColumn<QByteArray>("data");
    QTest::newRow("zero-length") << QByteArray::fromHex("00");
    QTest::newRow("excessive-length") << QByteArray::fromHex("818004");
    QTest::newRow("varint-overflow")
        << QByteArray::fromHex("ffffffffffffffffff02");
    QTest::newRow("zero-tag") << QByteArray::fromHex("020000");
    QTest::newRow("group-wire") << QByteArray::fromHex("010b");
    QTest::newRow("truncated-inner-varint") << QByteArray::fromHex("020880");
    QTest::newRow("truncated-bytes") << QByteArray::fromHex("04080f1201");
    QTest::newRow("missing-type") << QByteArray::fromHex("03120178");
    QTest::newRow("duplicate-type") << QByteArray::fromHex("04080f080f");
    QTest::newRow("wrong-type-wire") << QByteArray::fromHex("020a00");
    QTest::newRow("missing-device-info")
        << QByteArray::fromHex("07080f1203726571");
    QTest::newRow("missing-name")
        << QByteArray::fromHex("0a080f1203726571a20100");
    QTest::newRow("invalid-name-utf8")
        << QByteArray::fromHex("0d080f1203726571a201031201ff");
    QTest::newRow("identifier-null")
        << QByteArray::fromHex("0e080f120100a20106120474657374");
    QTest::newRow("too-many-frames") << Hello.repeated(33);
    QTest::newRow("input-limit") << QByteArray(65547, 'x');
  }
  void rejectsMalformed() {
    QFETCH(QByteArray, data);
    airplay::MrpSession session;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.receive(data, "own"));
    QCOMPARE(session.receive(Hello, "own").size(), 1);
  }
  void atomicBatchAndUnknownFields() {
    airplay::MrpSession session;
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        session.receive(Hello + QByteArray::fromHex("00"), "own"));
    QCOMPARE(session.receive(Subscribe, "own").first().errorCode, 2);
    // Unknown fixed32 and fixed64 fields are skipped, as are length-delimited
    // fields.
    auto body = Hello.mid(1) +
                QByteArray::fromHex("a50601020304a9060102030405060708b2060178");
    auto framed = QByteArray(1, char(body.size())) + body;
    QCOMPARE(session.receive(framed, "own").size(), 1);
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        session.receive(QByteArray::fromHex("080810aa0103080180"), "own"));
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        session.receive(QByteArray::fromHex("070826d202020804"), "own"));
    QCOMPARE(session.receive(Keyboard, "own").size(), 1);
  }
  void multibyteLengthAndFrameLimit() {
    // A synthetic unknown bytes field makes exactly a 64 KiB message. Verify
    // split length prefixes and the maximum complete frame independently.
    const auto body = Hello.mid(1) + QByteArray::fromHex("b206ebff03") +
                      QByteArray(65515, 'x');
    QCOMPARE(body.size(), 65536);
    const auto framed = QByteArray::fromHex("808004") + body;
    airplay::MrpSession session;
    QVERIFY(session.receive(framed.first(1), "own").isEmpty());
    QVERIFY(session.receive(framed.mid(1, 1), "own").isEmpty());
    QCOMPARE(session.receive(framed.mid(2), "own").size(), 1);
    // Oversized declared frames fail before their body has arrived.
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        session.receive(QByteArray::fromHex("818004"), "own"));
    QCOMPARE(session.receive(Hello, "own").size(), 1);
  }
};
QTEST_GUILESS_MAIN(MrpTests)
#include "MrpTests.moc"
