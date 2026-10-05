#pragma once
#include <QVariantMap>
#include "ControlTypes.h"
namespace airplay {
QByteArray liveDmapMetadata();
QList<QVariantMap> liveNowPlaying(const QString &identity,
                                  const QString &session, const QString &group,
                                  PlaybackState state = PlaybackState::Playing);
QVariantMap nowPlayingInfo(PlaybackState state);
QVariantMap playbackState(PlaybackState state);
QByteArray volumeProperties(double db, bool includeVolume,
                            bool includeCapability);
} // namespace airplay
