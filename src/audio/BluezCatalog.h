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
  QString inputName;
  bool operator==(const BluetoothSource &) const = default;
};
class BluezCatalog : public QObject {
  Q_OBJECT
public:
  explicit BluezCatalog(QObject *parent = nullptr,
      QDBusConnection bus = QDBusConnection::systemBus(), QString service = "org.bluez");
  const QList<BluetoothSource> &sources() const { return sources_; }
  const QList<BluetoothSource> &inputSources() const { return inputSources_; }
  static QList<BluetoothSource> sourcesFromObjects(const DBusObjects &);
signals:
  // First GetManagedObjects result only; an empty error means success.
  void initialQueryFinished(QString error);
  void changed();
  void volumeRequested(QString sourceId, double db);
  void volumeReset(QString sourceId);
private slots:
  void schedule();
  void propertiesChanged(const QString &, const QVariantMap &, const QStringList &,
                         const QDBusMessage &);
  void interfacesAdded(const QDBusObjectPath &, const audio::DBusInterfaces &);
  void interfacesRemoved(const QDBusObjectPath &, const QStringList &);
private:
  void refresh();
  void updateSources();
  QString transportSource(const QVariantMap &) const;
  QDBusConnection bus_;
  QString service_;
  QTimer refresh_;
  QList<BluetoothSource> sources_;
  QList<BluetoothSource> inputSources_;
  DBusObjects objects_;
  bool pending_ = false, dirty_ = false;
  bool initialQueryCompleted_ = false;
};
} // namespace audio
Q_DECLARE_METATYPE(audio::DBusInterfaces)
Q_DECLARE_METATYPE(audio::DBusObjects)
