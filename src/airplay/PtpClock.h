#pragma once
#include "Crypto.h"
#include "NetworkBinding.h"
#include "app/Settings.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>
#include <array>
namespace airplay {
int64_t wallNs();
QByteArray ptpHeader(int kind, int length, uint64_t clock, uint16_t sequence,
                     int flags = 0x408, int interval = -3);
QByteArray ptpTime(uint64_t nanoseconds);
QByteArray syncPacket(uint64_t clock, uint32_t firstRtp, int64_t audibleNs,
                      int64_t nowNs, bool first);
class PtpClock : public QObject {
  Q_OBJECT
public:
  explicit PtpClock(QObject *parent = nullptr);
  void start(const QHostAddress &local, const QList<QHostAddress> &hosts,
             uint64_t identity, const app::Timing &timing,
             const NetworkRoute &route = {});
  void stop();
signals:
  void failed(QString error);

private:
  void tick();
  void receive(QUdpSocket &socket);
  void send(QUdpSocket &socket, quint16 port, const QByteArray &packet,
            const QHostAddress &host = {});
  QUdpSocket event_, general_;
  QTimer timer_;
  QElapsedTimer elapsed_;
  QList<QHostAddress> hosts_;
  app::Timing timing_;
  NetworkRoute route_;
  uint64_t identity_ = 0;
  uint16_t sequence_ = 0;
  qint64 nextSync_ = 0, nextAnnounce_ = 0;
  bool active_ = false;
};
} // namespace airplay
