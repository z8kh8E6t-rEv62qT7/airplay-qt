#pragma once
#include "NetworkBinding.h"
#include <QObject>
#include <memory>
namespace airplay {
class ServiceAdvertisement : public QObject {
  Q_OBJECT
public:
  explicit ServiceAdvertisement(QObject *parent = nullptr);
  ~ServiceAdvertisement() override;
  void start(const QString &identity, quint16 port, const NetworkRoute &route);
  void stop();
signals:
  void ready();
  void failed(QString text);

private:
  struct State;
  std::unique_ptr<State> state_;
  quint64 generation_ = 0;
};
} // namespace airplay
