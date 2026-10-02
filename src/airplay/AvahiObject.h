#pragma once
#include "DiscoveryApi.h"
#include <QDBusMessage>
#include <QObject>
#include <functional>

namespace airplay {
// Owns one native Avahi D-Bus object. A late factory reply is freed even if
// cancellation destroyed this owner while the request was in flight.
class AvahiObject : public QObject {
  Q_OBJECT
public:
  AvahiObject(const DiscoveryApi &, QString interface, QObject *parent = nullptr);
  ~AvahiObject() override;
  void create(const QString &method, const QVariantList &arguments);
  void call(const QString &method, const QVariantList &arguments = {},
            std::function<void()> completed = {});
signals:
  void created();
  void event(QString member, QVariantList arguments);
  void failed(QString error);
private slots:
  void receive(const QDBusMessage &);
private:
  DiscoveryApi api_;
  QString interface_, path_;
};
} // namespace airplay
