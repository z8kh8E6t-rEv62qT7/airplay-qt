#pragma once
#include "ReceiverEndpoint.h"
#include <QObject>
#include <memory>
namespace airplay {
struct DiscoveryApi;
const DiscoveryApi &defaultDiscoveryApi();
class ReceiverDiscovery : public QObject {
  Q_OBJECT
public:
  explicit ReceiverDiscovery(QObject *parent = nullptr,
                             const DiscoveryApi &api = defaultDiscoveryApi());
  ~ReceiverDiscovery() override;
  void refresh();
  void cancel();
  bool busy() const;
signals:
  void cleared();
  // Canonical IPv4:port text keeps plugin-owned types out of Qt's registry.
  void found(QString name, QString endpoint);
  void status(QString text);
  void idle();

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace airplay
