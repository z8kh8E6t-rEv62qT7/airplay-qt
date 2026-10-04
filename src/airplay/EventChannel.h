#pragma once
#include "RtspClient.h"
#include "app/Message.h"

namespace airplay {
// The receiver initiates requests on this connection. Its HAP directions are
// reversed relative to the RTSP control channel, with independent counters.
class EventChannel : public QObject {
  Q_OBJECT
public:
  explicit EventChannel(QObject *parent = nullptr);
  ~EventChannel() override;
  void open(const QByteArray &shared, const QHostAddress &host, quint16 port,
            const QHostAddress &local, const NetworkRoute &route,
            double timeoutMs);
  void close();
signals:
  void connected();
  void failed(QJsonArray error);
  void log(QJsonArray text);
  void command(QByteArray plist);

private:
  void receive();
  void fail(const i18n::Message &);
  QTcpSocket socket_;
  QTimer timeout_;
  std::unique_ptr<HapRecords> records_;
  QByteArray wire_, plain_;
  bool closed_ = true;
};
} // namespace airplay
