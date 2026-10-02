#pragma once
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace audio {
using DBusInterfaces = QMap<QString, QVariantMap>;
using DBusObjects = QMap<QDBusObjectPath, DBusInterfaces>;
struct BluetoothSource {
  QString id, path, name;
  bool connected = false;
  bool operator==(const BluetoothSource &) const = default;
};
class BluezCatalog : public QObject {
  Q_OBJECT
public:
  explicit BluezCatalog(QObject *parent = nullptr,
      QDBusConnection bus = QDBusConnection::systemBus(), QString service = "org.bluez");
  const QList<BluetoothSource> &sources() const { return sources_; }
  static QList<BluetoothSource> sourcesFromObjects(const DBusObjects &);
signals:
  void changed();
private slots:
  void schedule();
private:
  void refresh();
  QDBusConnection bus_;
  QString service_;
  QTimer refresh_;
  QList<BluetoothSource> sources_;
  bool pending_ = false, dirty_ = false;
};
} // namespace audio
Q_DECLARE_METATYPE(audio::DBusInterfaces)
Q_DECLARE_METATYPE(audio::DBusObjects)
