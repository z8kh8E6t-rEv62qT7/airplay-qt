#include "RemoteControl.h"

namespace airplay {
RemoteControl::RemoteControl(QObject *parent) : QObject(parent), dacp_(this) {
  publicationDeadline_.setSingleShot(true);
  connect(&publicationDeadline_, &QTimer::timeout, this, [this] {
    stop();
    emit failed(i18n::text(i18n::Id::DACPServicePublicationTimedOut));
  });
  connect(&dacp_, &DacpServer::ready, this, [this] {
    publicationDeadline_.stop();
    emit ready();
  });
  connect(&dacp_, &DacpServer::failed, this, [this](QJsonArray text) {
    stop();
    emit failed(text);
  });
  connect(&dacp_, &DacpServer::log, this, &RemoteControl::log);
  connect(&dacp_, &DacpServer::command, this,
          &RemoteControl::handleDacpCommand);
}
void RemoteControl::start(const QString &identity, const QHostAddress &local,
                          const NetworkRoute &route,
                          const QList<QHostAddress> &peers, bool advertise,
                          quint32 activeRemote) {
  stop();
  playbackState_ = PlaybackState::Playing;
  try {
    dacp_.start(identity, local, route, peers, advertise, activeRemote);
    publicationDeadline_.start(5000);
  } catch (...) {
    stop();
    throw;
  }
}
void RemoteControl::stop() {
  enabled_ = false;
  publicationDeadline_.stop();
  playbackState_ = PlaybackState::Stopped;
  dacp_.stop();
}
void RemoteControl::setEnabled(bool enabled) {
  enabled_ = enabled && dacp_.port() != 0;
  dacp_.setEnabled(enabled_);
}
void RemoteControl::setVolume(double value) { dacp_.setVolume(value); }
void RemoteControl::requestPlayback(PlaybackState state) {
  if (!enabled_ || state == playbackState_)
    return;
  playbackState_ = state;
  // This is protocol state only. It must never gate capture or audio packets.
  emit playbackChanged();
}
bool RemoteControl::handlePlaybackCommand(const QString &action) {
  if (action == "play")
    requestPlayback(PlaybackState::Playing);
  else if (action == "pause")
    requestPlayback(PlaybackState::Paused);
  else if (action == "playpause")
    requestPlayback(playbackState_ == PlaybackState::Playing
                        ? PlaybackState::Paused
                        : PlaybackState::Playing);
  else
    return false;
  return true;
}
void RemoteControl::handleDacpCommand(const QString &peer,
                                      const QString &action, double value) {
  if (!enabled_ || handlePlaybackCommand(action))
    return;
  emit volumeCommand(peer, action, value);
}
void RemoteControl::handleEventCommand(const QVariantMap &root) {
  if (!enabled_)
    return;
  const auto type = root.value("type"), value = root.value("value");
  if (type.typeId() != QMetaType::QString ||
      type.toString() != "sendMediaRemoteCommand" ||
      value.typeId() != QMetaType::QString)
    return;
  const auto command = value.toString();
  if (command == "play")
    handlePlaybackCommand("play");
  else if (command == "paus")
    handlePlaybackCommand("pause");
  else if (command == "plps")
    handlePlaybackCommand("playpause");
}
} // namespace airplay
