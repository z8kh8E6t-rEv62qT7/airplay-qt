#pragma once
#include "RtspClient.h"

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
            double timeout);
  void close();
signals:
  void connected();
  void failed(QString error);
  void log(QString text);
  void command(QByteArray plist);

private:
  void receive();
  void fail(const QString &);
  QTcpSocket socket_;
  QTimer timeout_;
  std::unique_ptr<HapRecords> records_;
  QByteArray wire_, plain_;
  bool closed_ = true;
};
} // namespace airplay
