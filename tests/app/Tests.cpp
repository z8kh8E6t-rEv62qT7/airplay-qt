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
};
QTEST_MAIN(AppTests)
#include "Tests.moc"
