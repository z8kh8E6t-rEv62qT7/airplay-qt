#pragma once
#include "ControlTypes.h"
#include <QByteArray>
#include <QList>
#include <optional>

namespace airplay {
// Socket-independent server state, owned by exactly one authenticated stream.
class MrpSession {
public:
  struct Exchange {
    quint32 type;
    QByteArray response; // One or more length-prefixed ProtocolMessages.
    int errorCode = 0;
    QByteArray identifier;
    std::optional<ControlRequest> request;
    QByteArray deviceUID; // DeviceInfo update, never an authorization identity.
  };
  // Accepts split/coalesced frames. Throws on malformed or excessive input;
  // failure leaves this session unchanged and emits no partial replies.
  QList<Exchange> receive(const QByteArray &data, const QByteArray &identity,
                          const ControlState &state = {});
  QByteArray stateUpdate(const QByteArray &identity, const ControlState &state,
                         bool initial = false) const;
  static QByteArray complete(const Exchange &request, bool success);
  // Validate and split an outgoing batch without changing any protobuf bytes.
  static QList<QByteArray> outgoingFrames(const QByteArray &data);

private:
  QByteArray pending_;
  bool introduced_ = false, subscribed_ = false;
  bool nowPlayingUpdates_ = false, volumeUpdates_ = false,
       outputUpdates_ = false;
};
} // namespace airplay
