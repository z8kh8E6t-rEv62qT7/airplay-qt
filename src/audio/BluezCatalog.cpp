#include "BluezCatalog.h"
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <algorithm>

namespace audio {
namespace {
QList<BluetoothSource> devicesFromObjects(const DBusObjects &, bool audioOnly);
}
BluezCatalog::BluezCatalog(QObject *parent, QDBusConnection bus, QString service)
    : QObject(parent), bus_(std::move(bus)), service_(std::move(service)) {
  qDBusRegisterMetaType<DBusInterfaces>();
  qDBusRegisterMetaType<DBusObjects>();
  refresh_.setSingleShot(true);
  refresh_.setInterval(50);
  connect(&refresh_, &QTimer::timeout, this, &BluezCatalog::refresh);
  bus_.connect(service_, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this,
               SLOT(interfacesAdded(QDBusObjectPath,audio::DBusInterfaces)));
  bus_.connect(service_, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", this,
               SLOT(interfacesRemoved(QDBusObjectPath,QStringList)));
  bus_.connect(service_, {}, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
               SLOT(propertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
  auto *watcher = new QDBusServiceWatcher(service_, bus_, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
    objects_.clear();
    updateSources();
    schedule();
  });
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
    const bool first = !initialQueryCompleted_;
    initialQueryCompleted_ = true;
    const auto error = reply.isError()
        ? QString("BlueZ device query failed: %1").arg(reply.error().message())
        : QString{};
    // An owner/property change during the snapshot makes that snapshot stale.
    if (dirty_) {
      dirty_ = false;
      schedule();
      if (first)
        emit initialQueryFinished(error.isEmpty()
            ? QString("BlueZ devices changed during the initial query; restart the CLI")
            : error);
      return;
    }
    objects_ = reply.isError() ? DBusObjects{} : reply.value();
    updateSources();
    if (first) emit initialQueryFinished(error);
  });
}
void BluezCatalog::updateSources() {
  const auto next = sourcesFromObjects(objects_);
  const auto inputs = devicesFromObjects(objects_, false);
  for (const auto &source : sources_)
    if (source.connected && std::none_of(next.begin(), next.end(), [&](const auto &s) {
          return s.id == source.id && s.path == source.path && s.connected;
        }))
      emit volumeReset(source.id);
  if (next != sources_ || inputs != inputSources_) {
    sources_ = next;
    inputSources_ = inputs;
    emit changed();
  }
}
QString BluezCatalog::transportSource(const QVariantMap &transport) const {
  const auto uuid = transport.value("UUID").toString().toLower();
  if (uuid != "0000110a-0000-1000-8000-00805f9b34fb" &&
      uuid != "0000110b-0000-1000-8000-00805f9b34fb") return {};
  const auto path = transport.value("Device").value<QDBusObjectPath>().path();
  for (const auto &source : sources_)
    if (source.path == path && source.connected) return source.id;
  return {};
}
void BluezCatalog::propertiesChanged(const QString &interface, const QVariantMap &values,
                                     const QStringList &invalidated, const QDBusMessage &message) {
  const QDBusObjectPath path(message.path());
  // Unknown objects need a baseline before their properties can be routed.
  if (!objects_.contains(path) || !objects_[path].contains(interface)) { schedule(); return; }
  auto &props = objects_[path][interface];
  const auto previous = props.value("Volume");
  const auto source = transportSource(props);
  props.insert(values);
  for (const auto &key : invalidated) props.remove(key);
  if (interface == "org.bluez.MediaTransport1") {
    if (invalidated.contains("Volume") || invalidated.contains("Device"))
      emit volumeReset(source);
    const auto value = values.value("Volume");
    if (!source.isEmpty() && source == transportSource(props) &&
        !invalidated.contains("Volume") && value.metaType().id() == QMetaType::UShort &&
        value.toUInt() <= 127 && (value.metaType() != previous.metaType() || value != previous))
      emit volumeRequested(source, value.toUInt() == 0 ? -144. : -30. + 30. * value.toUInt() / 127.);
  }
  if (interface == "org.bluez.Device1" || interface == "org.bluez.Adapter1") updateSources();
  if (pending_ || !invalidated.isEmpty()) schedule();
}
void BluezCatalog::interfacesAdded(const QDBusObjectPath &path, const DBusInterfaces &interfaces) {
  objects_[path].insert(interfaces);
  updateSources(); // Added transports establish a baseline, never a volume request.
  if (pending_) schedule();
}
void BluezCatalog::interfacesRemoved(const QDBusObjectPath &path, const QStringList &interfaces) {
  if (interfaces.contains("org.bluez.MediaTransport1"))
    emit volumeReset(transportSource(objects_.value(path).value("org.bluez.MediaTransport1")));
  for (const auto &interface : interfaces) objects_[path].remove(interface);
  if (objects_[path].isEmpty()) objects_.remove(path);
  updateSources();
  if (pending_) schedule();
}
QList<BluetoothSource> BluezCatalog::sourcesFromObjects(const DBusObjects &objects) {
  return devicesFromObjects(objects, true);
}
namespace {
QList<BluetoothSource> devicesFromObjects(const DBusObjects &objects, bool audioOnly) {
  QList<BluetoothSource> result;
  const QString a2dpSource = "0000110a-0000-1000-8000-00805f9b34fb";
  for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
    const auto props = it.value().value("org.bluez.Device1");
    if (audioOnly && !props.value("Paired").toBool()) continue;
    const auto uuids = props.value("UUIDs").toStringList();
    if (audioOnly && !uuids.contains(a2dpSource, Qt::CaseInsensitive)) continue;
    const auto adapter = props.value("Adapter").value<QDBusObjectPath>();
    const auto adapterAddress = objects.value(adapter).value("org.bluez.Adapter1").value("Address").toString();
    const auto address = props.value("Address").toString();
    if (adapterAddress.isEmpty() || address.isEmpty()) continue;
    result.append({"bluez:" + adapterAddress.toUpper() + "/" + address.toUpper(),
        it.key().path(), props.value("Alias", address).toString(), props.value("Connected").toBool(),
        props.value("Name", address).toString()});
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  return result;
}
} // namespace
} // namespace audio
