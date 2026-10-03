#pragma once
#include "airplay/AirPlaySession.h"
#include "app/Message.h"
#include <QThread>

namespace app {
// UI-thread owner. All protocol objects are confined to the network thread.
class SessionController : public QObject {
  Q_OBJECT
public:
  explicit SessionController(QObject *parent = nullptr);
  ~SessionController() override;
  void shutdown();
  void setLanguage(i18n::Language value) { language_ = value; }
  i18n::Language language() const { return language_; }
  void start(const Timing &, audio::CaptureStream,
             const QList<airplay::ReceiverEndpoint> &,
             airplay::SessionEnvironment environment = {},
             airplay::NetworkRoute route = {});
  void captureStarted();
  void stop(const i18n::Message &reason = {},
            airplay::SessionEnd end = airplay::SessionEnd::Stopped);
  void volume(double db);
  void inputVolume(double db);
  void inputVolumeStep(int direction);
  void setTelemetryEnabled(bool enabled);
  bool telemetryEnabled() const { return telemetryEnabled_; }
  bool busy() const { return busy_; }
  bool streaming() const { return streaming_; }
  i18n::Message currentStatus() const { return status_; }
  i18n::Message currentGroup() const { return group_; }
  double currentVolume() const { return volume_; }
  double restoreVolume() const { return restoreVolume_; }
  airplay::SessionEnd endReason() const { return endReason_; }
signals:
  void startCapture();
  void stopCapture();
  void status(QJsonArray text);
  void log(QJsonArray text);
  void group(QJsonArray text);
  void error(QJsonArray text);
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
  quint64 telemetryRevision_ = 0;
  bool telemetryEnabled_ = true;
  bool busy_ = false, stopping_ = false, streaming_ = false;
  i18n::Message status_ = i18n::text(i18n::Id::Ready), group_;
  i18n::Language language_ = i18n::Language::English;
  double volume_ = 0, restoreVolume_ = 0;
  airplay::SessionEnd endReason_ = airplay::SessionEnd::Stopped;
};
} // namespace app
