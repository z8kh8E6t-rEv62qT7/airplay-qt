#include "AvahiObject.h"
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPointer>

namespace airplay {
namespace {
void release(const DiscoveryApi &api, const QString &path, const QString &iface) {
  if (!path.isEmpty())
    api.bus.asyncCall(QDBusMessage::createMethodCall(api.service, path, iface, "Free"), 3000);
}
}
AvahiObject::AvahiObject(const DiscoveryApi &api, QString interface, QObject *parent)
    : QObject(parent), api_(api), interface_(std::move(interface)) {}
AvahiObject::~AvahiObject() {
  if (!path_.isEmpty())
    api_.bus.disconnect(api_.service, path_, interface_, {}, this,
                        SLOT(receive(QDBusMessage)));
  release(api_, path_, interface_);
}
void AvahiObject::create(const QString &method, const QVariantList &arguments) {
  auto message = QDBusMessage::createMethodCall(api_.service, "/",
      "org.freedesktop.Avahi.Server2", method);
  message.setArguments(arguments);
  auto *watcher = new QDBusPendingCallWatcher(api_.bus.asyncCall(message, 3000));
  connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
          [self = QPointer(this), api = api_, iface = interface_](auto *finished) {
    QDBusPendingReply<QDBusObjectPath> reply = *finished;
    finished->deleteLater();
    if (reply.isError()) {
      if (self) emit self->failed(reply.error().message());
      return;
    }
    const auto path = reply.value().path();
    if (!self) {
      release(api, path, iface);
      return;
    }
    self->path_ = path;
    if (!self->api_.bus.connect(api.service, path, iface, {}, self,
                                SLOT(receive(QDBusMessage)))) {
      emit self->failed(self->api_.bus.lastError().message());
      return;
    }
    emit self->created();
  });
}
void AvahiObject::call(const QString &method, const QVariantList &arguments,
                       std::function<void()> completed) {
  auto message = QDBusMessage::createMethodCall(api_.service, path_, interface_, method);
  message.setArguments(arguments);
  auto *watcher = new QDBusPendingCallWatcher(api_.bus.asyncCall(message, 3000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, completed = std::move(completed)](auto *finished) {
    QDBusPendingReply<> reply = *finished;
    finished->deleteLater();
    if (reply.isError()) emit failed(reply.error().message());
    else if (completed) completed();
  });
}
void AvahiObject::receive(const QDBusMessage &message) {
  emit event(message.member(), message.arguments());
}
} // namespace airplay
