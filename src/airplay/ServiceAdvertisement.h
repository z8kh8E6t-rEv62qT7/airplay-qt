#pragma once
#include "NetworkBinding.h"
#include "app/Message.h"
#include <QObject>
#include <memory>
#ifdef Q_OS_LINUX
#include "DiscoveryApi.h"
#endif
namespace airplay {
class ServiceAdvertisement : public QObject {
  Q_OBJECT
public:
#ifdef Q_OS_LINUX
  explicit ServiceAdvertisement(QObject *parent = nullptr,
                                const DiscoveryApi &api = DiscoveryApi{});
#else
  explicit ServiceAdvertisement(QObject *parent = nullptr);
#endif
  ~ServiceAdvertisement() override;
  void start(const QString &identity, quint16 port, const NetworkRoute &route);
  void stop();
signals:
  void ready();
  void failed(QJsonArray text);

private:
#ifdef Q_OS_LINUX
  DiscoveryApi api_;
#endif
  struct State;
  std::unique_ptr<State> state_;
  quint64 generation_ = 0;
};
} // namespace airplay
