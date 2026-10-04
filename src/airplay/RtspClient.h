#pragma once
#include "Crypto.h"
#include "NetworkBinding.h"
#include "app/Message.h"
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>
#include <memory>
#include <optional>
namespace airplay {
QByteArray plistEncode(const QVariant &value);
QVariant plistDecode(const QByteArray &data);
struct RtspResponse {
  int status;
  QMap<QByteArray, QByteArray> headers;
  QByteArray body;
};
struct ControlMessage {
  QByteArray line;
  QMap<QByteArray, QByteArray> headers;
  QByteArray body;
};
std::optional<ControlMessage> parseControlMessage(QByteArray &data);
std::optional<RtspResponse> parseResponse(QByteArray &data);
class RtspClient : public QObject {
  Q_OBJECT
public:
  explicit RtspClient(QObject *parent = nullptr);
  void open(const QHostAddress &host, quint16 port, const QString &identity,
            double timeoutMs, const QHostAddress &local = {},
            const NetworkRoute &route = {}, quint32 activeRemote = 1);
  void request(const QByteArray &method, const QByteArray &path,
               const QByteArray &body, const QByteArray &contentType,
               double timeoutMs, std::optional<quint32> rtpTime = {});
  void encrypt(const QByteArray &shared);
  void abort();
  bool busy() const { return pending_; }
  bool connected() const {
    return socket_.state() == QAbstractSocket::ConnectedState;
  }
  QHostAddress localAddress() const { return socket_.localAddress(); }
signals:
  void opened();
  void response(QByteArray body);
  void failed(QJsonArray error);

private:
  void receive();
  void fail(const i18n::Message &error);
  QTcpSocket socket_;
  QTimer timer_;
  QByteArray wire_, plain_, identity_, session_;
  uint32_t cseq_ = 0;
  quint32 activeRemote_ = 1;
  bool pending_ = false, closed_ = true;
  std::unique_ptr<HapRecords> records_;
};
} // namespace airplay
