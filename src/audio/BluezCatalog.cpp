#include "BluezCatalog.h"
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <algorithm>

namespace audio {
BluezCatalog::BluezCatalog(QObject *parent, QDBusConnection bus, QString service)
    : QObject(parent), bus_(std::move(bus)), service_(std::move(service)) {
  qDBusRegisterMetaType<DBusInterfaces>();
  qDBusRegisterMetaType<DBusObjects>();
  refresh_.setSingleShot(true);
  refresh_.setInterval(50);
  connect(&refresh_, &QTimer::timeout, this, &BluezCatalog::refresh);
  bus_.connect(service_, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this, SLOT(schedule()));
  bus_.connect(service_, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", this, SLOT(schedule()));
  bus_.connect(service_, {}, "org.freedesktop.DBus.Properties", "PropertiesChanged", this, SLOT(schedule()));
  auto *watcher = new QDBusServiceWatcher(service_, bus_, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { schedule(); });
  schedule();
}
void BluezCatalog::schedule() {
  if (pending_) dirty_ = true;
  else refresh_.start();
}
void BluezCatalog::refresh() {
  pending_ = true;
  const auto request = QDBusMessage::createMethodCall(service_, "/",
      "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
  auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(request, 3000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](auto *finished) {
    QDBusPendingReply<DBusObjects> reply = *finished;
    finished->deleteLater();
    pending_ = false;
    // An owner/property change during the snapshot makes that snapshot stale.
    if (dirty_) { dirty_ = false; schedule(); return; }
    const auto next = reply.isError() ? QList<BluetoothSource>{} : sourcesFromObjects(reply.value());
    if (next != sources_) { sources_ = next; emit changed(); }
  });
}
QList<BluetoothSource> BluezCatalog::sourcesFromObjects(const DBusObjects &objects) {
  QList<BluetoothSource> result;
  const QString a2dpSource = "0000110a-0000-1000-8000-00805f9b34fb";
  for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
    const auto props = it.value().value("org.bluez.Device1");
    if (!props.value("Paired").toBool()) continue;
    const auto uuids = props.value("UUIDs").toStringList();
    if (!uuids.contains(a2dpSource, Qt::CaseInsensitive)) continue;
    const auto adapter = props.value("Adapter").value<QDBusObjectPath>();
    const auto adapterAddress = objects.value(adapter).value("org.bluez.Adapter1").value("Address").toString();
    const auto address = props.value("Address").toString();
    if (adapterAddress.isEmpty() || address.isEmpty()) continue;
    result.append({"bluez:" + adapterAddress.toUpper() + "/" + address.toUpper(),
        it.key().path(), props.value("Alias", address).toString(), props.value("Connected").toBool()});
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  return result;
}
} // namespace audio
