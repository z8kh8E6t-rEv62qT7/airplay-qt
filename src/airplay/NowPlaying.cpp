#include "NowPlaying.h"
#include "Crypto.h"
#include "RtspClient.h"
#include <QCoreApplication>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace airplay {
namespace {
QByteArray varint(quint64 value) {
  QByteArray out;
  do {
    auto byte = char(value & 127);
    value >>= 7;
    out += char(byte | (value ? 128 : 0));
  } while (value);
  return out;
}
QByteArray number(unsigned field, quint64 value) {
  return varint(quint64(field) << 3) + varint(value);
}
QByteArray bytes(unsigned field, const QByteArray &value) {
  return varint((quint64(field) << 3) | 2) + varint(value.size()) + value;
}
QVariantMap command(const QString &type, const QVariantMap &params) {
  return {{"type", type}, {"params", params}};
}
double playbackRate(PlaybackState state) {
  return state == PlaybackState::Playing ? 1. : 0.;
}
QByteArray tag(const QByteArray &name, const QByteArray &value) {
  QByteArray out = name;
  appendBe(out, uint64_t(value.size()), 4);
  return out + value;
}
} // namespace
QByteArray liveDmapMetadata() {
  return tag("mlit", tag("minm", "AirPlayQt") +
                         tag("asar", QStringLiteral("实时音频").toUtf8()));
}
QByteArray volumeProperties(double db, bool includeVolume,
                            bool includeCapability) {
  QByteArray status;
  appendBe(status, 200, 4);
  auto body = tag("mstt", status);
  if (includeVolume) {
    QByteArray volume;
    appendBe(volume,
             db <= -144
                 ? 0
                 : quint64(std::clamp(std::lround((db + 30.) / .3), 1L, 100L)),
             4);
    body += tag("cmvo", volume);
  }
  if (includeCapability)
    body += tag("cavc", QByteArray(1, '\1'));
  return tag("cmgt", body);
}
QVariantMap nowPlayingInfo(PlaybackState state) {
  const QVariantMap text{
      {"kMRMediaRemoteNowPlayingInfoTitle", "AirPlayQt"},
      {"kMRMediaRemoteNowPlayingInfoArtist", QStringLiteral("实时音频")},
      {"kMRMediaRemoteNowPlayingInfoIsLiveStream", true},
      {"kMRMediaRemoteNowPlayingInfoPlaybackRate", playbackRate(state)},
      {"kMRMediaRemoteNowPlayingInfoDefaultPlaybackRate", 1.},
      {"kMRMediaRemoteNowPlayingInfoMediaType",
       QStringLiteral("MRMediaRemoteMediaTypeMusic")},
      {"kMRMediaRemoteNowPlayingInfoUniqueIdentifier",
       QVariant::fromValue<qulonglong>(1)}};
  return command("updateMRNowPlayingInfo",
                 {{"type", "npi-text"},
                  {"mergePolicy", "replace"},
                  {"params", text}});
}
QVariantMap playbackState(PlaybackState state) {
  return command("updateMRPlaybackState",
                 {{"mrPlaybackState", int(state)}});
}
QList<QVariantMap> liveNowPlaying(const QString &identity,
                                  const QString &session, const QString &group,
                                  PlaybackState state) {
  // Minimal proto2 DeviceInfo / ProtocolMessage and NowPlayingClient messages.
  // Field numbers are documented in pyatv's protobuf schemas and the
  // music-assistant sender. Identify ourselves, not com.apple.Music.
  const auto bundle = QByteArray("org.airplayqt.app");
  auto info = bytes(1, identity.toUtf8()) + bytes(2, "AirPlayQt") +
              bytes(3, "Computer") + bytes(5, bundle) + number(7, 1) +
              number(8, 15) + bytes(19, identity.toUtf8()) + number(22, 1) +
              bytes(41, session.toUtf8()) + bytes(42, group.toUtf8());
  const auto envelope =
      number(1, 15) + bytes(20, info) +
      bytes(85, QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
  const auto client = number(1, quint64(QCoreApplication::applicationPid())) +
                      bytes(2, bundle) + bytes(7, "AirPlayQt");
  // MRMediaRemoteCommand numbering (0/1/2), not protobuf Command (1/2/3).
  // Each supported command is an archived binary plist data item.
  QVariantList supported;
  for (int code : {0, 1, 2})
    supported.append(plistEncode(QVariantMap{
        {"kCommandInfoCommandKey", code}, {"kCommandInfoEnabledKey", true}}));
  return {
      {{"params", QVariantMap{{"data", varint(envelope.size()) + envelope}}}},
      nowPlayingInfo(state),
      command("updateMRSupportedCommands",
              {{"mrSupportedCommandsFromSender", supported}}),
      playbackState(state),
      command("updateMRNowPlayingClient", {{"mrNowPlayingClient", client}})};
}
} // namespace airplay
