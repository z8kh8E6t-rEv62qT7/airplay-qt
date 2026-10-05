#pragma once
#include "FairPlaySetup.h"
#include "RtspClient.h"
#include "SharedRemoteStreams.h"
#include "TransientPairing.h"
#include <QTcpServer>
namespace airplay {
class InboundControlServer;
// Owns one authenticated control/event pair and its bounded MRP streams.
class InboundControlConnection : public QObject {
public:
  InboundControlConnection(QTcpSocket *socket, InboundControlServer &owner);
  ~InboundControlConnection() override;
  void controlUpdated(bool publish);

private:
  struct Channel {
    QTcpSocket *socket = nullptr;
    QByteArray wire, plain;
    std::unique_ptr<HapRecords> records;
  };
  struct PendingControl {
    quint64 stream;
    MrpSession::Exchange request;
  };
  struct Outgoing {
    quint64 stream;
    QList<QByteArray> frames;
    bool snapshot = false;
  };
  struct InFlight {
    quint64 sequence, stream;
  };
  void attach(Channel &channel, QTcpSocket *socket);
  void clear(Channel &channel);
  void shutdown();
  void finish(const QString &reason, bool publicationFailure = false);
  void drainAndClose();
  QVariantMap info() const;
  void reply(Channel &channel, const ControlMessage &request, int status,
             const QByteArray &body = {},
             const QByteArray &type = "application/octet-stream");
  void write(Channel &channel, QByteArray out);
  void enqueue(quint64 stream, const QByteArray &data, bool snapshot = false);
  void executeControls();
  void sendNextEvent();
  void eventResponse(Channel &channel, const ControlMessage &message);
  void read(Channel &channel);
  void handle(Channel &channel, const ControlMessage &message);
  void pair(const ControlMessage &message);
  void setup(const ControlMessage &message,
             const std::optional<QVariant> &value);
  void acceptEvent();
  InboundControlServer &owner_;
  QTimer deadline_, eventDeadline_;
  QList<Outgoing> outgoing_;
  QList<PendingControl> pendingControls_, newControls_;
  std::optional<InFlight> inFlight_;
  quint64 nextEventSequence_ = 0;
  bool closing_ = false;
  Channel control_, event_;
  QTcpServer eventServer_;
  quint16 eventPort_ = 0;
  std::unique_ptr<TransientPairing> pairing_;
  FairPlaySetup fairPlay_;
  SharedRemoteStreams streams_;
};
} // namespace airplay
