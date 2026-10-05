#pragma once
#include "DacpServer.h"
#include "NowPlaying.h"

namespace airplay {
class RemoteControl : public QObject {
  Q_OBJECT
public:
  explicit RemoteControl(QObject *parent = nullptr);
  void start(const QString &identity, const QHostAddress &local,
             const NetworkRoute &route, const QList<QHostAddress> &peers,
             bool advertise, quint32 activeRemote);
  void stop();
  void setEnabled(bool enabled);
  void setVolume(double value);
  void handleEventCommand(const QVariantMap &command);
  void requestPlayback(PlaybackState state);
  PlaybackState playbackState() const { return playbackState_; }
  quint16 port() const { return dacp_.port(); }

signals:
  void ready();
  void failed(QJsonArray text);
  void log(QJsonArray text);
  void volumeCommand(QString peer, QString action, double value);
  // Built-in Qt types only: no metatype registrations surviving VST unload.
  void playbackChanged();

private:
  void handleDacpCommand(const QString &peer, const QString &action,
                         double value);
  bool handlePlaybackCommand(const QString &action);
  DacpServer dacp_;
  QTimer publicationDeadline_;
  PlaybackState playbackState_ = PlaybackState::Stopped;
  bool enabled_ = false;
};
} // namespace airplay
