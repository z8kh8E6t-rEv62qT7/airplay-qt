#include "airplay/AvahiObject.h"
#include "airplay/ReceiverDiscovery.h"
#include "airplay/ServiceAdvertisement.h"
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QDBusVirtualObject>
#include <QTimer>
#include <QtTest>

class Avahi : public QDBusVirtualObject {
public:
  QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-avahi");
  QString service = "org.airplayqt.TestAvahi";
  QHash<QString, QString> objects;
  QStringList freed;
  int serial = 0, delay = 0;
  bool failStart = false;
  QVariantList browserArgs;
  QHash<QString, QVariantList> publication;
  QString reject;
  Avahi() {
    bus.registerService(service);
    bus.registerVirtualObject("/", this, QDBusConnection::SubPath);
  }
  ~Avahi() override { bus.unregisterObject("/", QDBusConnection::UnregisterTree); bus.unregisterService(service); QDBusConnection::disconnectFromBus("fake-avahi"); }
  QString introspect(const QString &) const override { return {}; }
  bool handleMessage(const QDBusMessage &m, const QDBusConnection &connection) override {
    if (m.member() == reject) {
      connection.send(m.createErrorReply("org.freedesktop.Avahi.Error", "test failure"));
      return true;
    }
    if (m.member().endsWith("Prepare") || m.member() == "EntryGroupNew") {
      const auto path = "/object" + QString::number(++serial);
      const QString iface = m.member() == "EntryGroupNew" ? "org.freedesktop.Avahi.EntryGroup" :
          m.member() == "ServiceBrowserPrepare" ? "org.freedesktop.Avahi.ServiceBrowser" :
          "org.freedesktop.Avahi.ServiceResolver";
      objects.insert(path, iface);
      if (m.member() == "ServiceBrowserPrepare") browserArgs = m.arguments();
      QTimer::singleShot(delay, this, [m, connection, path] {
        connection.send(m.createReply(QVariant::fromValue(QDBusObjectPath(path))));
      });
    } else if (m.member() == "Free") {
      freed.append(m.path()); objects.remove(m.path());
      connection.send(m.createReply());
    } else if (m.member() == "Start") {
      if (failStart) {
        connection.send(m.createErrorReply("org.freedesktop.Avahi.Error", "test failure"));
        return true;
      }
      connection.send(m.createReply());
      const auto iface = objects.value(m.path());
      auto signal = QDBusMessage::createSignal(m.path(), iface,
          iface.endsWith("ServiceBrowser") ? "ItemNew" : "Found");
      if (iface.endsWith("ServiceBrowser"))
        signal.setArguments({1, 0, "HomePod", "_airplay._tcp", "local", uint(0)});
      else
        signal.setArguments({1, 0, "HomePod", "_airplay._tcp", "local", "homepod.local", 0,
                             "192.0.2.20", QVariant::fromValue(quint16(7000)),
                             QVariant::fromValue(QList<QByteArray>{}), uint(0)});
      connection.send(signal);
    } else {
      publication.insert(m.member(), m.arguments());
      connection.send(m.createReply());
      if (m.member() == "Commit") {
        auto signal = QDBusMessage::createSignal(m.path(), objects.value(m.path()), "StateChanged");
        signal.setArguments({2, QString()});
        connection.send(signal);
      }
    }
    return true;
  }
};
class Tests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY(QDBusConnection::sessionBus().isConnected());
    qDBusRegisterMetaType<QList<QByteArray>>();
  }
  void resolvesAndReleases() {
    Avahi service;
    airplay::DiscoveryApi api{QDBusConnection::sessionBus(), service.service};
    airplay::ReceiverDiscovery discovery(nullptr, api);
    QSignalSpy found(&discovery, &airplay::ReceiverDiscovery::found);
    discovery.refresh();
    QTRY_COMPARE(found.size(), 1);
    QCOMPARE(found[0][1].toString(), "192.0.2.20:7000");
    QCOMPARE(service.browserArgs[1].toInt(), 0);
    discovery.cancel();
    QVERIFY(!discovery.busy());
    QTRY_VERIFY(service.objects.isEmpty());
  }
  void cancellationDuringFactoryReply() {
    Avahi service;
    service.delay = 40;
    airplay::ReceiverDiscovery discovery(nullptr, {QDBusConnection::sessionBus(), service.service});
    QSignalSpy found(&discovery, &airplay::ReceiverDiscovery::found);
    discovery.refresh();
    QTRY_COMPARE(service.serial, 1);
    discovery.cancel();
    QTRY_COMPARE(service.freed.size(), 1);
    QVERIFY(found.isEmpty());
    QVERIFY(service.objects.isEmpty());
  }
  void failureEndsScan() {
    Avahi service;
    service.failStart = true;
    airplay::ReceiverDiscovery discovery(nullptr, {QDBusConnection::sessionBus(), service.service});
    QSignalSpy idle(&discovery, &airplay::ReceiverDiscovery::idle);
    discovery.refresh();
    QTRY_COMPARE(idle.size(), 1);
    QVERIFY(!discovery.busy());
    QTRY_VERIFY(service.objects.isEmpty());
  }
  void publishesDacpAndReleases() {
    Avahi service;
    airplay::ServiceAdvertisement advertisement(nullptr, {QDBusConnection::sessionBus(), service.service});
    QSignalSpy ready(&advertisement, &airplay::ServiceAdvertisement::ready);
    QSignalSpy failed(&advertisement, &airplay::ServiceAdvertisement::failed);
    airplay::NetworkRoute route;
    route.local = QHostAddress("192.0.2.10");
    advertisement.start("0123ABCD", 3689, route);
    QTRY_COMPARE(ready.size(), 1);
    QVERIFY(failed.isEmpty());
    const auto address = service.publication.value("AddAddress");
    QCOMPARE(address, QVariantList({-1, 0, uint(16), "airplayqt-0123abcd.local", "192.0.2.10"}));
    const auto record = service.publication.value("AddService");
    QCOMPARE(record.size(), 9);
    QCOMPARE(record[3].toString(), "iTunes_Ctrl_0123ABCD");
    QCOMPARE(record[4].toString(), "_dacp._tcp");
    QCOMPARE(record[7].value<quint16>(), quint16(3689));
    const auto txt = qdbus_cast<QList<QByteArray>>(record[8]);
    QVERIFY(txt.contains("DbId=0123ABCD"));
    advertisement.stop();
    QTRY_VERIFY(service.objects.isEmpty());
  }
  void publicationCancellationAndFailure() {
    Avahi service;
    service.delay = 80;
    airplay::ServiceAdvertisement advertisement(nullptr, {QDBusConnection::sessionBus(), service.service});
    QSignalSpy ready(&advertisement, &airplay::ServiceAdvertisement::ready);
    QSignalSpy failed(&advertisement, &airplay::ServiceAdvertisement::failed);
    airplay::NetworkRoute route;
    route.local = QHostAddress("192.0.2.10");
    advertisement.start("0123ABCD", 3689, route);
    QTRY_COMPARE(service.serial, 1);
    advertisement.stop();
    QTRY_COMPARE(service.freed.size(), 1);
    QVERIFY(ready.isEmpty());
    QVERIFY(service.publication.isEmpty());
    service.delay = 0;
    service.reject = "AddService";
    advertisement.start("0123ABCD", 3689, route);
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(ready.isEmpty());
    QVERIFY(!service.publication.contains("Commit"));
    advertisement.stop();
    QTRY_VERIFY(service.objects.isEmpty());
  }
};
QTEST_GUILESS_MAIN(Tests)
#include "DiscoveryLinuxTests.moc"
