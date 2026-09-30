#include "airplay/DiscoveryApi.h"
#include "app/Settings.h"
#include "ui/StreamingPanel.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
class CommonAppTests : public QObject {
  Q_OBJECT
private slots:
  void discoverySortsNumericAddressAndPortWithoutLosingSelection() {
    app::SessionController session;
    ui::StreamingPanel panel(session);
    auto *discovery = panel.findChild<airplay::ReceiverDiscovery *>();
    auto *list = panel.findChild<QListWidget *>("receivers");
    emit discovery->found("first", "192.168.8.10:7000");
    auto *selected = list->item(0);
    selected->setCheckState(Qt::Checked);
    list->setCurrentItem(selected);
    for (const auto *endpoint :
         {"192.168.8.9:10000", "10.0.0.10:2", "192.168.8.9:900",
          "192.168.7.200:7000", "10.0.0.2:1", "127.0.0.1:7000"})
      emit discovery->found("same name", QString::fromLatin1(endpoint));
    emit discovery->found("duplicate", "192.168.8.9:900");
    const QStringList expected{"10.0.0.2:1",       "10.0.0.10:2",
                               "127.0.0.1:7000",   "192.168.7.200:7000",
                               "192.168.8.9:900",  "192.168.8.9:10000",
                               "192.168.8.10:7000"};
    QCOMPARE(list->count(), expected.size());
    for (int i = 0; i < list->count(); ++i)
      QCOMPARE(list->item(i)->data(Qt::UserRole).toString(), expected[i]);
    QCOMPARE(list->currentItem(), selected);
    QCOMPARE(panel.receiverSelection().size(), 1);
    QCOMPARE(panel.receiverSelection()[0].endpoint,
             QString("192.168.8.10:7000"));
    QCOMPARE(panel.endpoints()[0].text(), QString("192.168.8.10:7000"));
  }
  void rememberedTargetsSchemaAndStore() {
    QTemporaryDir dir;
    const auto path = dir.filePath("AirPlayQt.json");
    app::SettingsStore store(path);
    QVERIFY(!store.writable());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.rememberReceivers({}));
    QCOMPARE(store.load().receiverSelection.size(), 0);
    const QList<app::ReceiverSelection> pair{{"L", "192.0.2.1:7000"},
                                             {"R", "192.0.2.2:7001"}};
    store.rememberReceivers(pair);
    QCOMPARE(app::Settings::load(path).receiverSelection, pair);
    app::Settings inputs;
    inputs.driverId = "new-input";
    store.saveInput(
        inputs); // A manual-mode start cannot clear discovery memory.
    QCOMPARE(app::Settings::load(path).receiverSelection, pair);
    QCOMPARE(app::Settings::load(path).driverId, inputs.driverId);
    auto legacy = inputs.json();
    legacy.remove("receiverSelection");
    QVERIFY(app::Settings::fromJson(legacy).receiverSelection.isEmpty());
    for (const auto value :
         {QJsonValue(1), QJsonValue(QJsonArray{true}),
          QJsonValue(QJsonArray{
              QJsonObject{{"name", "X"}, {"endpoint", "192.0.2.1"}}}),
          QJsonValue(QJsonArray{
              QJsonObject{{"name", ""}, {"endpoint", "192.0.2.1:7000"}}})}) {
      auto bad = legacy;
      bad["receiverSelection"] = value;
      QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                               app::Settings::fromJson(bad));
    }
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             store.rememberReceivers({pair[0], pair[0]}));
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        store.rememberReceivers({pair[0], pair[1], pair[0]}));
    QCOMPARE(app::Settings::load(path).receiverSelection, pair);
    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{broken");
    corrupt.close();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.load());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.rememberReceivers(pair));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.saveInput(inputs));
    QVERIFY(corrupt.open(QIODevice::ReadOnly));
    QCOMPARE(corrupt.readAll(), QByteArray("{broken"));
    app::SettingsStore unwritable(dir.filePath("missing/config.json"));
    unwritable.load();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             unwritable.rememberReceivers(pair));
    QVERIFY(unwritable.receivers().isEmpty());
  }
  void restoreCompleteSelectionAndUserOverride() {
    app::SessionController session;
    ui::StreamingPanel panel(session);
    auto *discovery = panel.findChild<airplay::ReceiverDiscovery *>();
    auto *list = panel.findChild<QListWidget *>("receivers");
    auto *start = panel.findChild<QPushButton *>("start");
    const QList<app::ReceiverSelection> pair{{"L", "192.0.2.1:7000"},
                                             {"R", "192.0.2.2:7000"}};
    panel.setRememberedReceivers(pair);
    emit discovery->found("R", "192.0.2.2:7000");
    QVERIFY(panel.receiverSelection().isEmpty());
    QVERIFY(!start->isEnabled());
    emit discovery->found("R", "192.0.2.2:7000");
    QCOMPARE(list->count(), 1);
    emit discovery->found("L", "192.0.2.1:7000");
    QCOMPARE(panel.receiverSelection().size(), 2);
    QVERIFY(start->isEnabled());
    QVERIFY(!session.busy()); // Restoring never starts playback.
    emit discovery->cleared();
    emit discovery->found("renamed L", "192.0.2.1:7000");
    emit discovery->found("R", "192.0.2.2:7001");
    emit discovery->found("L", "192.0.2.3:7000");
    QVERIFY(panel.receiverSelection().isEmpty());
    emit discovery->cleared();
    emit discovery->found("L", "192.0.2.1:7000");
    list->item(0)->setCheckState(Qt::Checked);
    emit discovery->found("R", "192.0.2.2:7000");
    QCOMPARE(panel.receiverSelection().size(),
             1); // Manual edit cancels this scan's restore.
    emit discovery->cleared();
    emit discovery->found("L", "192.0.2.1:7000");
    emit discovery->found("R", "192.0.2.2:7000");
    QCOMPARE(panel.receiverSelection().size(), 2);
    panel.setRememberedReceivers({pair[0]});
    emit discovery->cleared();
    emit discovery->found("L", "192.0.2.1:7000");
    QCOMPARE(panel.receiverSelection().size(), 1);
    QSignalSpy stops(&panel, &ui::StreamingPanel::stopRequested);
    panel.setRecoveryPending(true);
    QVERIFY(!start->isEnabled());
    auto *stop = panel.findChild<QPushButton *>("stop");
    QVERIFY(stop->isEnabled());
    stop->click();
    QCOMPARE(stops.size(), 1);
    panel.setRecoveryPending(false);
    QVERIFY(!stop->isEnabled());
  }
  void receiverModesAndSelection() {
    airplay::DiscoveryApi api;
#ifdef Q_OS_WIN
    api.browse = [](auto *, auto *) { return DNS_STATUS(ERROR_NOT_SUPPORTED); };
#else
    api.browse = [](DNSServiceRef *, DNSServiceFlags, uint32_t, const char *,
                    const char *, DNSServiceBrowseReply,
                    void *) -> DNSServiceErrorType {
      return kDNSServiceErr_Unsupported;
    };
#endif
    app::SessionController session;
    ui::StreamingPanel window(session, nullptr, api);
    auto *modes = window.findChild<QTabWidget *>("receiverModes");
    auto *list = window.findChild<QListWidget *>("receivers");
    auto *first = window.findChild<QLineEdit *>("manualFirst");
    auto *second = window.findChild<QLineEdit *>("manualSecond");
    auto *start = window.findChild<QPushButton *>("start");
    auto *refresh = window.findChild<QPushButton *>("refreshReceivers");
    auto *discovery = window.findChild<airplay::ReceiverDiscovery *>();
    QVERIFY(modes && list && first && second && start && refresh);
    QCOMPARE(modes->currentIndex(), 0);
    QVERIFY(!start->isEnabled());
    QVERIFY(first->text().isEmpty() && second->text().isEmpty());
    for (int i = 1; i <= 3; ++i) {
      emit discovery->found("same name", QString("192.168.8.%1:7000").arg(i));
      QCOMPARE(list->item(i - 1)->checkState(), Qt::Unchecked);
    }
    list->item(0)->setCheckState(Qt::Checked);
    QVERIFY(start->isEnabled());
    list->item(1)->setCheckState(Qt::Checked);
    list->item(2)->setCheckState(Qt::Checked);
    QCOMPARE(list->item(2)->checkState(), Qt::Unchecked);
    modes->setCurrentIndex(1);
    QVERIFY(!start->isEnabled()); // Discovery selection cannot supply manual
                                  // targets.
    first->setText("192.168.8.9");
    QVERIFY(start->isEnabled());
    second->setText("192.168.8.9:7000");
    QVERIFY(!start->isEnabled());
    second->setText("192.168.8.10:7001");
    QVERIFY(start->isEnabled());
    QVERIFY(first->completer() == nullptr);
    QVERIFY(second->completer() == nullptr);
    QVERIFY(QMetaObject::invokeMethod(&window, "setBusy", Q_ARG(bool, true)));
    QVERIFY(!modes->isEnabled());
    QVERIFY(!first->isEnabled() && !second->isEnabled());
    QVERIFY(!refresh->isEnabled() && !start->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "setBusy", Q_ARG(bool, false)));
    QVERIFY(start->isEnabled());
    modes->setCurrentIndex(0);
    QCOMPARE(list->count(),
             0); // Re-entering discovery clears stale selections.
    QVERIFY(!start->isEnabled());
    modes->setCurrentIndex(1);
    QCOMPARE(first->text(), QString("192.168.8.9"));
    QVERIFY(start->isEnabled());
    second->clear();
    first->clear();
    QVERIFY(!start->isEnabled());
    QVERIFY(!app::Settings{}.json().contains("endpoints"));
    ui::StreamingPanel fresh(session, nullptr, api);
    QVERIFY(fresh.findChild<QLineEdit *>("manualFirst")->text().isEmpty());
    QVERIFY(fresh.findChild<QLineEdit *>("manualSecond")->text().isEmpty());
  }
  void defaultsAndBounds() {
    app::Settings settings;
    QVERIFY(settings.validate().isEmpty());
    for (const auto &field : app::timingFields) {
      auto invalid = settings;
      invalid.timing.*(field.member) = field.maximum + 1;
      QVERIFY(!invalid.validate().isEmpty());
      invalid = settings;
      invalid.timing.*(field.member) = std::numeric_limits<double>::quiet_NaN();
      QVERIFY(!invalid.validate().isEmpty());
    }
    settings.timing.ptpSync = .12;
    QVERIFY(!settings.validate().isEmpty());
    settings = {};
    settings.timing.prebuffer = settings.timing.backlog;
    QVERIFY(!settings.validate().isEmpty());
    settings = {};
    settings.left = settings.right;
    QVERIFY(!settings.validate().isEmpty());
  }
  void roundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto path = dir.filePath("AirPlayQt.json");
    app::Settings settings;
    settings.driverId = "{driver-id}";
    settings.left = 30;
    settings.right = 31;
    settings.save(path);
    QCOMPARE(app::Settings::load(path).json(), settings.json());
    QVERIFY(!settings.json().contains("volume"));
    QCOMPARE(app::Settings::load(dir.filePath("missing.json")).json(),
             app::Settings{}.json());
  }
  void malformedPreserved() {
    QTemporaryDir dir;
    const auto path = dir.filePath("AirPlayQt.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{broken");
    file.close();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::load(path));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{broken"));
    auto json = app::Settings{}.json();
    json["left"] = 1.5;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    json = app::Settings{}.json();
    json["version"] = 2;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    json = app::Settings{}.json();
    json["timing"] = QJsonObject{};
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
  }
  void writeFailureAndAtomicity() {
    QTemporaryDir dir;
    auto path = dir.filePath("AirPlayQt.json");
    app::Settings settings;
    settings.save(path);
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto before = original.readAll();
    original.close();
    settings.left = -1;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, settings.save(path));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), before);
    original.close();
    settings = {};
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             settings.save(dir.filePath("missing/child.json")));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, settings.save(dir.path()));
  }
};
QTEST_MAIN(CommonAppTests)
#include "CommonTests.moc"
