#pragma once
#include "RtspClient.h"
#include "ServiceAdvertisement.h"
#include "app/Message.h"
#include <QSet>
#include <QTcpServer>
namespace airplay {
class DacpServer : public QObject {
  Q_OBJECT
public:
  explicit DacpServer(QObject *parent = nullptr);
  ~DacpServer() override;
  void start(const QString &identity, const QHostAddress &local,
             const NetworkRoute &route, const QList<QHostAddress> &peers,
             bool advertise = true, quint32 activeRemote = 1);
  void stop();
  quint16 port() const { return server_.serverPort(); }
  void setEnabled(bool value) { enabled_ = value; }
  void setVolume(double value) { volume_ = value; }
signals:
  void ready();
  void failed(QJsonArray text);
  // Built-in Qt types only: no metatype registrations surviving VST unload.
  void command(QString peer, QString action, double value);
  void log(QJsonArray text);

private:
  void accept();
  int handle(QTcpSocket &, const ControlMessage &, QByteArray &body);
  QTcpServer server_;
  ServiceAdvertisement advertisement_;
  QSet<QTcpSocket *> clients_;
  QList<QHostAddress> peers_;
  bool enabled_ = false;
  QByteArray activeRemote_;
  NetworkRoute route_;
  double volume_ = -144;
};
} // namespace airplay
