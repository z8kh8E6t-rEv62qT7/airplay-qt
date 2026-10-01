#pragma once
#include <QVariantMap>
namespace airplay {
QByteArray liveDmapMetadata();
QList<QVariantMap> liveNowPlaying(const QString &identity,
                                  const QString &session, const QString &group);
QVariantMap playbackState(bool playing);
QByteArray volumeProperties(double db, bool includeVolume,
                            bool includeCapability);
} // namespace airplay
