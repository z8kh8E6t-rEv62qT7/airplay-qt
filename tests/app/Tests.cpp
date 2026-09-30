#include "airplay/DiscoveryApi.h"
#include "app/Settings.h"
#include "ui/MainWindow.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <aclapi.h>

namespace {
struct LocalMemory {
  void *pointer = nullptr;
  ~LocalMemory() {
    if (pointer)
      LocalFree(pointer);
  }
};
struct RestoreDirectoryAcl {
  std::wstring path;
  PACL original = nullptr;
  bool active = false;
  DWORD restore() {
    if (!active)
      return ERROR_SUCCESS;
    const auto result = SetNamedSecurityInfoW(
        path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
        nullptr, original, nullptr);
    if (result == ERROR_SUCCESS)
      active = false;
    return result;
  }
  ~RestoreDirectoryAcl() { restore(); }
};
} // namespace
class AppTests : public QObject {
  Q_OBJECT
private slots:
  void closingWaitsForDiscovery() {
    DNS_SERVICE_BROWSE_REQUEST request{};
    int cancellations = 0;
    airplay::DiscoveryApi api;
    api.browse = [&](auto *value, auto *) {
      request = *value;
      return DNS_STATUS(DNS_REQUEST_PENDING);
    };
    api.cancelBrowse = [&](auto *) {
      ++cancellations;
      return DNS_STATUS(ERROR_SUCCESS);
    };
    ui::MainWindow window(api);
    window.show();
    auto *discovery = window.findChild<airplay::ReceiverDiscovery *>();
    QVERIFY(discovery);
    window.findChild<QPushButton *>("refreshReceivers")->click();
    QVERIFY(discovery->busy());
    window.close();
    QVERIFY(window.isVisible());
    QCOMPARE(cancellations, 1);
    request.pBrowseCallback(ERROR_CANCELLED, request.pQueryContext, nullptr);
    // Deliver only native callback events; do not initialize a real ASIO
    // driver.
    QCoreApplication::sendPostedEvents(discovery, QEvent::MetaCall);
    QVERIFY(!discovery->busy());
    window.close();
    QVERIFY(!window.isVisible());
  }
  void receiverModesAndSelection() {
    airplay::DiscoveryApi api;
    api.browse = [](auto *, auto *) { return DNS_STATUS(ERROR_NOT_SUPPORTED); };
    ui::MainWindow window(api);
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
    ui::MainWindow fresh(api);
    QVERIFY(fresh.findChild<QLineEdit *>("manualFirst")->text().isEmpty());
    QVERIFY(fresh.findChild<QLineEdit *>("manualSecond")->text().isEmpty());
  }
  void readOnlyDirectoryPreservesConfig() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("AirPlayQt.json");
    app::Settings initial;
    initial.save(path);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto before = file.readAll();
    file.close();
    LocalMemory descriptor;
    RestoreDirectoryAcl permissions{
        QDir::toNativeSeparators(directory.path()).toStdWString()};
    QCOMPARE(GetNamedSecurityInfoW(permissions.path.c_str(), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   &permissions.original, nullptr,
                                   &descriptor.pointer),
             DWORD(ERROR_SUCCESS));
    alignas(SID) BYTE everyone[SECURITY_MAX_SID_SIZE];
    DWORD bytes = sizeof(everyone);
    QVERIFY(CreateWellKnownSid(WinWorldSid, nullptr, everyone, &bytes));
    EXPLICIT_ACCESSW denied{};
    denied.grfAccessPermissions = FILE_ADD_FILE;
    denied.grfAccessMode = DENY_ACCESS;
    denied.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&denied.Trustee, everyone);
    PACL restricted = nullptr;
    QCOMPARE(SetEntriesInAclW(1, &denied, permissions.original, &restricted),
             DWORD(ERROR_SUCCESS));
    LocalMemory restrictedOwner{restricted};
    QCOMPARE(SetNamedSecurityInfoW(permissions.path.data(), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   restricted, nullptr),
             DWORD(ERROR_SUCCESS));
    permissions.active = true;
    auto changed = initial;
    changed.left = 30;
    changed.right = 31;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, changed.save(path));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    QCOMPARE(permissions.restore(), DWORD(ERROR_SUCCESS));
  }
  void guiSmoke() {
    airplay::DiscoveryApi api;
    api.browse = [](auto *, auto *) { return DNS_STATUS(ERROR_NOT_SUPPORTED); };
    ui::MainWindow window(api);
    window.show();
    QVERIFY(window.isVisible());
    QVERIFY(window.findChildren<QComboBox *>().size() >= 3);
    QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() +
                               "/MainWindow.png"));
    window.close();
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
QTEST_MAIN(AppTests)
#include "Tests.moc"
