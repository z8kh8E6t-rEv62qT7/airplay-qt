#pragma once
#include "ControlTypes.h"
#include "NetworkBinding.h"
#include <QSet>
#include <QTcpServer>
#include <QVariantMap>
#include <functional>

namespace airplay {
class InboundControlConnection;
class InboundControlServer : public QObject {
  Q_OBJECT
public:
  // Called on the network thread. A true result accepts the command; the
  // owner applies it and calls updateState on a later event-loop turn, never
  // synchronously stopping or destroying the service from this callback.
  using CommandHandler = std::function<bool(const ControlRequest &)>;
  explicit InboundControlServer(QObject *parent = nullptr);
  ~InboundControlServer() override;
  void start(const QHostAddress &local, const NetworkRoute &route,
             CommandHandler handler);
  void updateState(const ControlState &state);
  // Reject new clients/commands; existing clients can receive the final state.
  void beginStop();
  void stop();
  quint16 port() const { return server_.serverPort(); }

signals:
  void failed(QJsonArray error);
  void error(QJsonArray error);

private:
  friend class InboundControlConnection;
  void accept();
  void observeDevice(const QHostAddress &peer, const QByteArray &uid);
  void publicationFailed();
  QTcpServer server_;
  QHostAddress address_;
  NetworkRoute route_;
  QSet<InboundControlConnection *> connections_;
  QVariantMap info_;
  QByteArray identity_;
  ControlState state_;
  CommandHandler handler_;
  quint64 nextStream_ = 0, generation_ = 0;
  bool stopping_ = false, failurePending_ = false;
};
} // namespace airplay
