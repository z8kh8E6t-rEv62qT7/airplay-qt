#pragma once
#include "airplay/AirPlaySession.h"
#include <QThread>

namespace app {
// UI-thread owner. All protocol objects are confined to the network thread.
class SessionController : public QObject {
  Q_OBJECT
public:
  explicit SessionController(QObject *parent = nullptr);
  ~SessionController() override;
  void shutdown();
  void start(const Timing &, audio::CaptureStream,
             const QList<airplay::ReceiverEndpoint> &,
             airplay::SessionEnvironment environment = {},
             airplay::NetworkRoute route = {});
  void captureStarted();
  void stop(const QString &reason = {},
            airplay::SessionEnd end = airplay::SessionEnd::Stopped);
  void volume(double db);
  bool busy() const { return busy_; }
  bool streaming() const { return streaming_; }
  QString currentStatus() const { return status_; }
  QString currentGroup() const { return group_; }
  QStringList recentLog() const { return log_; }
  double currentVolume() const { return volume_; }
  double restoreVolume() const { return restoreVolume_; }
  airplay::SessionEnd endReason() const { return endReason_; }
signals:
  void startCapture();
  void stopCapture();
  void status(QString text);
  void log(QString text);
  void group(QString text);
  void error(QString text);
  void busyChanged(bool busy);
  void streamingChanged(bool streaming);
  void volumeApplied(double db);
  void telemetry(double left, double right, double backlog, quint64 packets,
                 quint64 retransmitted, quint64 expired);
  void stopped();

private:
  struct NetworkContext;
  std::shared_ptr<NetworkContext> network_;
  QThread thread_;
  QObject *worker_;
  quint64 generation_ = 0;
  bool busy_ = false, stopping_ = false, streaming_ = false;
  QString status_ = "就绪", group_;
  QStringList log_;
  double volume_ = 0, restoreVolume_ = 0;
  airplay::SessionEnd endReason_ = airplay::SessionEnd::Stopped;
};
} // namespace app
