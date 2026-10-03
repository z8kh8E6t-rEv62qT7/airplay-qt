#include "audio/BluetoothVolume.h"
#include <QDBusMetaType>
#include <QDBusVirtualObject>
#include <QtTest>
#include <linux/input.h>

using namespace audio;
namespace {
const QString deviceInterface = "org.bluez.Device1";
const QString transportInterface = "org.bluez.MediaTransport1";
const QDBusObjectPath adapter("/org/bluez/hci0"), phone("/org/bluez/hci0/dev_A"),
    other("/org/bluez/hci0/dev_B"), transport("/org/bluez/hci0/dev_A/fd0");

class Bluez : public QDBusVirtualObject {
public:
  QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "test-bluez");
  const QString service = "org.airplayqt.TestBluez";
  DBusObjects objects;
  int snapshots = 0, delay = 0;
  Bluez() {
    qDBusRegisterMetaType<DBusInterfaces>();
    qDBusRegisterMetaType<DBusObjects>();
    objects[adapter]["org.bluez.Adapter1"] = {{"Address", "00:11:22:33:44:55"}};
    for (const auto &path : {phone, other}) {
      const bool first = path == phone;
      objects[path][deviceInterface] = {
          {"Address", first ? "AA:BB:CC:DD:EE:01" : "AA:BB:CC:DD:EE:02"},
          {"Name", first ? "Phone A" : "Phone B"}, {"Alias", "Custom alias"},
          {"Paired", true}, {"Connected", true}, {"Adapter", QVariant::fromValue(adapter)},
          {"UUIDs", QStringList{"0000110a-0000-1000-8000-00805f9b34fb"}}};
    }
    objects[transport][transportInterface] = {
        {"Device", QVariant::fromValue(phone)},
        {"UUID", "0000110b-0000-1000-8000-00805f9b34fb"},
        {"Volume", QVariant::fromValue(quint16(60))}};
    bus.registerService(service);
    bus.registerVirtualObject("/", this, QDBusConnection::SubPath);
  }
  ~Bluez() override {
    bus.unregisterObject("/", QDBusConnection::UnregisterTree);
    bus.unregisterService(service);
    QDBusConnection::disconnectFromBus("test-bluez");
  }
  QString introspect(const QString &) const override { return {}; }
  bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
    if (message.member() != "GetManagedObjects") return false;
    ++snapshots;
    const auto snapshot = objects;
    QTimer::singleShot(delay, this, [message, connection, snapshot] {
      connection.send(message.createReply(QVariant::fromValue(snapshot)));
    });
    return true;
  }
  void change(const QDBusObjectPath &path, const QString &interface,
              const QVariantMap &values, const QStringList &invalidated = {}) {
    objects[path][interface].insert(values);
    for (const auto &key : invalidated) objects[path][interface].remove(key);
    auto signal = QDBusMessage::createSignal(path.path(), "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal.setArguments({interface, values, invalidated});
    bus.send(signal);
  }
  void volume(quint16 value) {
    change(transport, transportInterface, {{"Volume", QVariant::fromValue(value)}});
  }
  void removeTransport() {
    objects.remove(transport);
    auto signal = QDBusMessage::createSignal("/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved");
    signal.setArguments({QVariant::fromValue(transport), QStringList{transportInterface}});
    bus.send(signal);
  }
  void addTransport(const DBusInterfaces &interfaces) {
    objects[transport] = interfaces;
    auto signal = QDBusMessage::createSignal("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
    signal.setArguments({QVariant::fromValue(transport), QVariant::fromValue(interfaces)});
    bus.send(signal);
  }
};
}

class BluetoothTests : public QObject {
  Q_OBJECT
private slots:
  void absoluteVolumeAndLifecycle() {
    Bluez service;
    BluezCatalog catalog(nullptr, QDBusConnection::sessionBus(), service.service);
    QSignalSpy requests(&catalog, &BluezCatalog::volumeRequested);
    QSignalSpy resets(&catalog, &BluezCatalog::volumeReset);
    QTRY_COMPARE(catalog.sources().size(), 2);
    const auto id = catalog.sources().first().id;
    QVERIFY(requests.isEmpty()); // Snapshot must not change the receiver volume.
    service.volume(127);
    QTRY_COMPARE(requests.size(), 1);
    QCOMPARE(requests.last()[0].toString(), id);
    QCOMPARE(requests.last()[1].toDouble(), 0.);
    service.volume(127);
    service.volume(128);
    service.change(transport, transportInterface, {{"Volume", true}});
    QTest::qWait(80);
    QCOMPARE(requests.size(), 1);
    service.volume(0);
    QTRY_COMPARE(requests.size(), 2);
    QCOMPARE(requests.last()[1].toDouble(), -144.);
    service.volume(1);
    QTRY_COMPARE(requests.size(), 3);
    QCOMPARE(requests.last()[1].toDouble(), -30. + 30. / 127.);
    service.change(phone, deviceInterface, {{"Connected", false}});
    QTRY_VERIFY(!catalog.sources().first().connected);
    service.volume(80);
    QTest::qWait(80);
    QCOMPARE(requests.size(), 3);
    QVERIFY(!resets.isEmpty());
    service.change(phone, deviceInterface, {{"Connected", true}});
    QTRY_VERIFY(catalog.sources().first().connected);
    service.volume(90);
    QTRY_COMPARE(requests.size(), 4);
    const auto saved = service.objects.value(transport);
    const auto resetCount = resets.size();
    service.removeTransport();
    QTRY_VERIFY(resets.size() > resetCount);
    service.addTransport(saved);
    QTest::qWait(80);
    QCOMPARE(requests.size(), 4);
    service.volume(100);
    QTRY_COMPARE(requests.size(), 5);
    service.bus.unregisterService(service.service);
    QTRY_VERIFY(catalog.sources().isEmpty());
    service.bus.registerService(service.service);
    QTRY_COMPARE(catalog.sources().size(), 2);
    QCOMPARE(requests.size(), 5);
    service.volume(110);
    QTRY_COMPARE(requests.size(), 6);
  }
  void staleSnapshotCannotRestoreDisconnectedSource() {
    Bluez service;
    service.delay = 150;
    BluezCatalog catalog(nullptr, QDBusConnection::sessionBus(), service.service);
    QTRY_COMPARE(service.snapshots, 1);
    service.change(phone, deviceInterface, {{"Connected", false}});
    QTRY_COMPARE(catalog.sources().size(), 2);
    QVERIFY(!catalog.sources().first().connected);
    QVERIFY(service.snapshots >= 2);
  }
  void selectedSourceAndKeyPriority() {
    Bluez service;
    BluezCatalog catalog(nullptr, QDBusConnection::sessionBus(), service.service);
    BluetoothVolume volume(catalog, nullptr, {}); // No real evdev devices in this test.
    QSignalSpy steps(&volume, &BluetoothVolume::stepRequested);
    QSignalSpy absolute(&volume, &BluetoothVolume::absoluteRequested);
    QTRY_COMPARE(catalog.sources().size(), 2);
    const auto id = catalog.sources().first().id;
    const auto otherId = catalog.sources().last().id;
    volume.setSource(id);
    volume.keyEvent(id, KEY_VOLUMEUP, 1);
    volume.keyEvent(id, KEY_VOLUMEUP, 2);
    volume.keyEvent(id, KEY_VOLUMEUP, 0);
    volume.keyEvent(id, KEY_VOLUMEDOWN, 1);
    volume.keyEvent(otherId, KEY_VOLUMEUP, 1);
    volume.keyEvent(id, KEY_PLAY, 1);
    QTRY_COMPARE(steps.size(), 3);
    QCOMPARE(steps[0][0].toInt(), 1);
    QCOMPARE(steps[1][0].toInt(), 1);
    QCOMPARE(steps[2][0].toInt(), -1);
    steps.clear();
    volume.keyEvent(id, KEY_VOLUMEUP, 1);
    service.volume(90);
    QTRY_COMPARE(absolute.size(), 1);
    volume.keyEvent(id, KEY_VOLUMEUP, 1); // Reverse signal order is suppressed too.
    QTest::qWait(150);
    QVERIFY(steps.isEmpty());
    volume.keyEvent(id, KEY_VOLUMEDOWN, 1);
    QTRY_COMPARE(steps.size(), 1);
    steps.clear();
    volume.keyEvent(id, KEY_VOLUMEUP, 1);
    volume.setSource(otherId);
    service.volume(100);
    QTest::qWait(150);
    QVERIFY(steps.isEmpty());
    QCOMPARE(absolute.size(), 1);
    volume.setSource(id);
    volume.keyEvent(id, KEY_VOLUMEUP, 1);
    service.change(phone, deviceInterface, {{"Connected", false}});
    QTRY_VERIFY(!catalog.sources().first().connected);
    QTest::qWait(150);
    QVERIFY(steps.isEmpty());
    service.change(phone, deviceInterface, {{"Connected", true}});
    QTRY_VERIFY(catalog.sources().first().connected);
    volume.keyEvent(id, KEY_VOLUMEUP, 1);
    const auto saved = service.objects.value(transport);
    service.removeTransport();
    QTest::qWait(150);
    QVERIFY(steps.isEmpty());
    service.addTransport(saved);
    QTest::qWait(30);
    volume.keyEvent(id, KEY_VOLUMEUP, 1);
    volume.setSource({});
    QTest::qWait(150);
    QVERIFY(steps.isEmpty());
    service.volume(110);
    QTest::qWait(50);
    QCOMPARE(absolute.size(), 1);
  }
  void inputIdentityMustBeUnambiguous() {
    Bluez service;
    auto sources = BluezCatalog::sourcesFromObjects(service.objects);
    const auto id = sources.first().id;
    QCOMPARE(bluetoothInputSource("Phone A (AVRCP)", "00:11:22:33:44:55", {}, sources), id);
    QVERIFY(bluetoothInputSource("Custom alias (AVRCP)", "00:11:22:33:44:55", {}, sources).isEmpty());
    QVERIFY(bluetoothInputSource("Phone A (AVRCP)", "00:11:22:33:44:56", {}, sources).isEmpty());
    QVERIFY(bluetoothInputSource("Keyboard", "00:11:22:33:44:55", {}, sources).isEmpty());
    sources.last().inputName = sources.first().inputName;
    QVERIFY(bluetoothInputSource("Phone A (AVRCP)", "00:11:22:33:44:55", {}, sources).isEmpty());
    QCOMPARE(bluetoothInputSource("Phone A (AVRCP)", "00:11:22:33:44:55", "aa:bb:cc:dd:ee:01", sources), id);
    sources.first().connected = false;
    QVERIFY(bluetoothInputSource("Phone A (AVRCP)", "00:11:22:33:44:55", "aa:bb:cc:dd:ee:01", sources).isEmpty());
  }
  void nonAudioDevicesAlsoPreventAmbiguousMatching() {
    Bluez service;
    service.objects[other][deviceInterface]["UUIDs"] = QStringList{};
    service.objects[other][deviceInterface]["Name"] = "Phone A";
    service.objects[other][deviceInterface]["Paired"] = false;
    BluezCatalog catalog(nullptr, QDBusConnection::sessionBus(), service.service);
    QTRY_COMPARE(catalog.sources().size(), 1);
    QCOMPARE(catalog.inputSources().size(), 2);
    QVERIFY(bluetoothInputSource("Phone A (AVRCP)", "00:11:22:33:44:55", {}, catalog.inputSources()).isEmpty());
  }
};
QTEST_GUILESS_MAIN(BluetoothTests)
#include "BluetoothVolumeTests.moc"
