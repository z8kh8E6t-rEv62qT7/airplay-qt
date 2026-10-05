#pragma once
#include "airplay/AirPlaySession.h"
#include <QNetworkDatagram>
#include <QTcpServer>
#include <openssl/bn.h>
namespace test {
using namespace airplay;
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
  QTcpSocket *eventSocket = nullptr;
  QByteArray eventWire, eventPlain, activeRemote;
  std::unique_ptr<HapRecords> eventRecords;
  QList<RtspResponse> eventResponses;
  bool rejectMetadata = false;
  QString rejectCommandType, holdCommandType;
  int commandDelay = 0, protocolState = 0;
  QByteArray wire, plain;
  std::unique_ptr<HapRecords> records;
  ServerSrp srp;
  Failure failure = Failure::None;
  QString id, stereo = "pair-id", error;
  int connections = 0, volumeRequests = 0, teardowns = 0, options = 0,
      optionsDelay = 0;
  QList<double> volumes;
  QVariantMap sessionSetup;
  QList<QVariantMap> commands;
  QByteArray metadata;
  QByteArray metadataRtpInfo;
  QList<QByteArray> packets, retransmits;
  QHostAddress eventSource, dataSource, controlSource;
  quint16 clientControl = 0;
  explicit Receiver(QString identity) : id(std::move(identity)) {
    if (!server.listen(QHostAddress::LocalHost, 0) ||
        !eventServer.listen(QHostAddress::LocalHost, 0) ||
        !data.bind(QHostAddress::LocalHost, 0) ||
        !control.bind(QHostAddress::LocalHost, 0))
      throw Error("Mock sockets failed");
    connect(&server, &QTcpServer::newConnection, this, [this] {
      ++connections;
      if (socket)
        socket->disconnect(this);
      socket = server.nextPendingConnection();
      socket->setParent(this);
      wire.clear();
      plain.clear();
      records.reset();
      initialCommand_ = 0;
      pendingRate_ = -1;
      protocolState = 0;
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
      eventSocket = client;
      eventWire.clear();
      eventPlain.clear();
      eventRecords = std::make_unique<HapRecords>(
          hkdf(srp.key, "Events-Salt", "Events-Write-Encryption-Key"),
          hkdf(srp.key, "Events-Salt", "Events-Read-Encryption-Key"));
      eventSource = client->peerAddress();
      client->setParent(this);
      connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
      connect(client, &QTcpSocket::disconnected, this, [this, client] {
        if (eventSocket == client)
          eventSocket = nullptr;
      });
      connect(client, &QTcpSocket::readyRead, this, [this, client] {
        try {
          eventWire += client->readAll();
          eventPlain += eventRecords->decodeAvailable(eventWire);
          while (auto response = parseResponse(eventPlain))
            eventResponses.append(*response);
        } catch (const std::exception &e) {
          error = QString::fromUtf8(e.what());
          client->abort();
        }
      });
    });
    connect(&data, &QUdpSocket::readyRead, this, [this] {
      while (data.hasPendingDatagrams()) {
        const auto datagram = data.receiveDatagram();
        dataSource = datagram.senderAddress();
        packets.append(datagram.data());
      }
    });
    connect(&control, &QUdpSocket::readyRead, this, [this] {
      while (control.hasPendingDatagrams()) {
        const auto datagram = control.receiveDatagram();
        controlSource = datagram.senderAddress();
        const auto packet = datagram.data();
        if (packet.size() > 1 && uint8_t(packet[1]) == 0xd6)
          retransmits.append(packet);
      }
    });
  }
  quint16 port() const { return server.serverPort(); }
  void sendEvent(const QVariantMap &command, int cseq = 1) {
    const auto body = plistEncode(command);
    const auto request = "POST /command RTSP/1.0\r\nCSeq: " +
                         QByteArray::number(cseq) + "\r\nContent-Length: " +
                         QByteArray::number(body.size()) + "\r\n\r\n" + body;
    eventSocket->write(eventRecords->encode(request));
  }
  // Independent wire contract, deliberately not built with NowPlaying helpers.
  static bool validNowPlaying(const QVariantMap &command) {
    const auto params = command.value("params").toMap();
    if (params.value("type") != "npi-text" ||
        params.value("mergePolicy") != "replace")
      return false;
    const auto info = params.value("params").toMap();
    const QString prefix = "kMRMediaRemoteNowPlayingInfo";
    const QStringList fields{"Title", "Artist", "IsLiveStream", "PlaybackRate",
                             "DefaultPlaybackRate", "MediaType", "UniqueIdentifier"};
    if (info.size() != fields.size())
      return false;
    for (const auto &field : fields)
      if (!info.contains(prefix + field))
        return false;
    const auto rate = info.value(prefix + "PlaybackRate");
    const auto defaultRate = info.value(prefix + "DefaultPlaybackRate");
    const auto uid = info.value(prefix + "UniqueIdentifier");
    return info.value(prefix + "Title") == "AirPlayQt" &&
           info.value(prefix + "Artist") == QStringLiteral("实时音频") &&
           info.value(prefix + "IsLiveStream").typeId() == QMetaType::Bool &&
           info.value(prefix + "IsLiveStream").toBool() &&
           rate.typeId() == QMetaType::Double &&
           (rate.toDouble() == 0. || rate.toDouble() == 1.) &&
           defaultRate.typeId() == QMetaType::Double && defaultRate.toDouble() == 1. &&
           info.value(prefix + "MediaType").typeId() == QMetaType::QString &&
           info.value(prefix + "MediaType") == "MRMediaRemoteMediaTypeMusic" &&
           (uid.typeId() == QMetaType::ULongLong || uid.typeId() == QMetaType::LongLong) &&
           uid.toULongLong() == 1;
  }
  ~Receiver() override {
    for (auto *socket : findChildren<QTcpSocket *>())
      socket->disconnect(this);
  }
  void retransmit(uint16_t first, uint16_t count) {
    QByteArray request = QByteArray::fromHex("80551234");
    appendBe(request, first, 2);
    appendBe(request, count, 2);
    control.writeDatagram(request, QHostAddress::LocalHost, clientControl);
  }

private:
  int initialCommand_ = 0;
  double pendingRate_ = -1;
  void validateCommand(const QVariantMap &command) {
    const auto type = command.value("type").toString();
    const auto params = command.value("params").toMap();
    const QStringList initial{"", "updateMRNowPlayingInfo",
                              "updateMRSupportedCommands", "updateMRPlaybackState",
                              "updateMRNowPlayingClient"};
    const bool stopping = type == "updateMRPlaybackState" &&
                          params.value("mrPlaybackState").toInt() == 3;
    if (!stopping && initialCommand_ < initial.size()) {
      if (type != initial[initialCommand_++])
        throw Error("Mock initial metadata order mismatch");
    } else if (!stopping && type != "updateMRNowPlayingInfo" &&
               type != "updateMRPlaybackState")
      throw Error("Mock unexpected runtime command");
    if (type == "updateMRNowPlayingInfo") {
      if (!validNowPlaying(command) || pendingRate_ != -1)
        throw Error("Mock invalid now-playing fields or order");
      pendingRate_ = params.value("params").toMap()
                         .value("kMRMediaRemoteNowPlayingInfoPlaybackRate").toDouble();
    } else if (type == "updateMRSupportedCommands") {
      const auto list = params.value("mrSupportedCommandsFromSender");
      if (list.typeId() != QMetaType::QVariantList || list.toList().size() != 3)
        throw Error("Mock invalid supported commands");
      int expected = 0;
      for (const auto &item : list.toList()) {
        if (item.typeId() != QMetaType::QByteArray || !item.toByteArray().startsWith("bplist00"))
          throw Error("Mock command capability must be archived plist data");
        const auto capability = plistDecode(item.toByteArray()).toMap();
        const auto code = capability.value("kCommandInfoCommandKey");
        if (capability.size() != 2 ||
            (code.typeId() != QMetaType::LongLong && code.typeId() != QMetaType::ULongLong) ||
            code.toInt() != expected++ ||
            capability.value("kCommandInfoEnabledKey").typeId() != QMetaType::Bool ||
            !capability.value("kCommandInfoEnabledKey").toBool())
          throw Error("Mock invalid command capability fields");
      }
    } else if (type == "updateMRPlaybackState") {
      const auto state = params.value("mrPlaybackState");
      if ((state.typeId() != QMetaType::LongLong && state.typeId() != QMetaType::ULongLong) ||
          state.toInt() < 1 || state.toInt() > 3 ||
          (!stopping && pendingRate_ != (state.toInt() == 1 ? 1. : 0.)))
        throw Error("Mock playback state does not match preceding metadata");
      protocolState = state.toInt();
      pendingRate_ = -1;
    } else if (type == "updateMRNowPlayingClient") {
      if (params.value("mrNowPlayingClient").typeId() != QMetaType::QByteArray ||
          !params.value("mrNowPlayingClient").toByteArray().contains("org.airplayqt.app"))
        throw Error("Mock invalid now-playing client");
    } else if (type.isEmpty()) {
      if (params.value("data").typeId() != QMetaType::QByteArray ||
          !params.value("data").toByteArray().contains("org.airplayqt.app"))
        throw Error("Mock invalid device info");
    }
  }
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
      activeRemote = headers["active-remote"];
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
        if (headers["content-type"] == "text/parameters") {
          ++volumeRequests;
          volumes.append(body.mid(8).trimmed().toDouble());
        } else {
          metadata = body;
          metadataRtpInfo = headers["rtp-info"];
        }
        respond(cseq, {},
                failure == Failure::Volume && body.startsWith("volume:") &&
                volumeRequests > 1 ? 500 : 200);
      } else if (path == "/command") {
        const auto command = plistDecode(body).toMap();
        validateCommand(command);
        commands.append(command);
        const auto type = command.value("type").toString();
        if (!holdCommandType.isEmpty() && type == holdCommandType)
          continue;
        const int code = rejectMetadata ||
                         (!rejectCommandType.isEmpty() && type == rejectCommandType) ? 400 : 200;
        if (commandDelay) {
          auto *client = socket;
          QTimer::singleShot(commandDelay, client, [this, client, cseq, code] {
            if (socket == client && client->state() == QAbstractSocket::ConnectedState)
              respond(cseq, {}, code);
          });
        } else
          respond(cseq, {}, code);
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

} // namespace test
