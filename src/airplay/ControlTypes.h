#pragma once
#include <QByteArray>
#include <QHostAddress>
#include <QList>

namespace airplay {
enum class PlaybackState : int { Playing = 1, Paused = 2, Stopped = 3 };
enum class ControlAction { Play, Pause, Toggle, Volume };
struct ControlRequest {
  ControlAction action = ControlAction::Play;
  double volumeDb = 0;
};
struct ControlOutput {
  QByteArray id, name;
  QList<QByteArray> aliases;
  QHostAddress address;
  bool operator==(const ControlOutput &) const = default;
};
// Value snapshots stay on the session's network thread, never in Qt's global
// metatype registry: VST3 runtimes may be unloaded after a session finishes.
struct ControlState {
  bool available = false;
  PlaybackState playback = PlaybackState::Stopped;
  double volumeDb = 0;
  QByteArray group;
  QList<ControlOutput> outputs;
  bool settled = false;
  bool operator==(const ControlState &) const = default;
};
} // namespace airplay
