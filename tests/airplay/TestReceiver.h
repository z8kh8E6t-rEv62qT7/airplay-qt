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
  QByteArray wire, plain;
  std::unique_ptr<HapRecords> records;
  ServerSrp srp;
  Failure failure = Failure::None;
  QString id, stereo = "pair-id", error;
  int connections = 0, volumeRequests = 0, teardowns = 0, options = 0,
      optionsDelay = 0;
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
      ++connections;
      if (socket)
        socket->disconnect(this);
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

} // namespace test
