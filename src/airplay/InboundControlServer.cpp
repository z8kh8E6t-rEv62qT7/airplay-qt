#include "InboundControlServer.h"
#include "Crypto.h"
#include "InboundControlConnection.h"
#include <QNetworkProxy>
#include <QTimer>
#include <QUuid>

namespace airplay {
InboundControlServer::InboundControlServer(QObject *parent) : QObject(parent) {
  server_.setProxy(QNetworkProxy::NoProxy);
  server_.setMaxPendingConnections(8);
  connect(&server_, &QTcpServer::newConnection, this,
          &InboundControlServer::accept);
}
InboundControlServer::~InboundControlServer() { stop(); }

void InboundControlServer::start(const QHostAddress &local,
                                 const NetworkRoute &route,
                                 CommandHandler handler) {
  stop();
  route.validate();
  if (local.protocol() != QAbstractSocket::IPv4Protocol ||
      local == QHostAddress::AnyIPv4 || local.isMulticast() ||
      local == QHostAddress::Broadcast ||
      (!route.binding.automatic() && local != route.local))
    throw std::runtime_error("Invalid inbound control source address");
  address_ = local;
  route_ = route;
  handler_ = std::move(handler);
  identity_ =
      QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper().toUtf8();
  auto mac = randomBytes(6);
  mac[0] = char((quint8(mac[0]) | 2) & 0xfe);
  const auto device = QString::fromLatin1(mac.toHex(':').toUpper());
  QByteArray txt;
  for (const auto &entry : QList<QByteArray>{
           "deviceid=" + device.toLatin1(), "features=0x4000,0x10000",
           "flags=0x4", "model=AirPlayQt", "pi=" + identity_,
           "psi=" + identity_, "protovers=1.1", "srcvers=550.10", "vv=1"}) {
    txt.append(char(entry.size()));
    txt += entry;
  }
  info_ = {{"name", "AirPlayQt"},
           {"deviceID", device},
           {"macAddress", device},
           {"model", "AirPlayQt"},
           {"sourceVersion", "550.10"},
           {"protocolVersion", "1.1"},
           {"pi", QString::fromUtf8(identity_)},
           {"psi", QString::fromUtf8(identity_)},
           {"features",
            QVariant::fromValue(qulonglong((1ULL << 14) | (1ULL << 48)))},
           {"statusFlags", 4},
           {"vv", 1},
           {"txtAirPlay", txt}};
  try {
    if (!server_.listen(address_, 0))
      throw std::runtime_error(server_.errorString().toStdString());
    route_.bindInterface(server_.socketDescriptor());
  } catch (...) {
    stop();
    throw;
  }
}

void InboundControlServer::updateState(const ControlState &state) {
  // Never publish a target that the receivers have not confirmed yet.
  if (state.available && !state.settled)
    return;
  auto next = state;
  for (auto &output : next.outputs)
    for (const auto &old : state_.outputs)
      if (old.id == output.id && old.address == output.address)
        output.aliases = old.aliases;
  const bool changed = next != state_;
  state_ = std::move(next);
  const auto active = connections_;
  for (auto *connection : active)
    connection->controlUpdated(changed);
}

void InboundControlServer::observeDevice(const QHostAddress &peer,
                                         const QByteArray &uid) {
  if (stopping_ || uid.isEmpty())
    return;
  // An authenticated phone does not get to assign identities to audio outputs.
  bool changed = false;
  for (auto &output : state_.outputs)
    if (output.address == peer && output.aliases != QList<QByteArray>{uid}) {
      output.aliases = {uid};
      changed = true;
    }
  if (!changed)
    return;
  const auto generation = generation_;
  QTimer::singleShot(0, this, [this, generation] {
    if (generation != generation_ || stopping_)
      return;
    const auto active = connections_;
    for (auto *connection : active)
      connection->controlUpdated(true);
  });
}

void InboundControlServer::publicationFailed() {
  if (failurePending_ || stopping_)
    return;
  failurePending_ = true;
  const auto generation = generation_;
  // Stop the audio owner only after the connection's callback has unwound.
  QTimer::singleShot(0, this, [this, generation] {
    if (generation == generation_ && !stopping_)
      emit failed(i18n::Message("MRP state publication failed"));
  });
}

void InboundControlServer::beginStop() {
  if (stopping_)
    return;
  stopping_ = true;
  server_.close();
  handler_ = {};
  auto finalState = state_;
  finalState.available = finalState.settled = false;
  finalState.playback = PlaybackState::Stopped;
  updateState(finalState);
  const auto generation = generation_;
  QTimer::singleShot(250, this, [this, generation] {
    if (generation == generation_)
      stop();
  });
}

void InboundControlServer::stop() {
  ++generation_;
  server_.close();
  const auto active = connections_;
  connections_.clear();
  for (auto *connection : active)
    delete connection;
  state_ = {};
  handler_ = {};
  info_.clear();
  identity_.clear();
  stopping_ = failurePending_ = false;
}

void InboundControlServer::accept() {
  while (auto *socket = server_.nextPendingConnection()) {
    if (stopping_ || connections_.size() >= 8) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    connections_.insert(new InboundControlConnection(socket, *this));
  }
}
} // namespace airplay
