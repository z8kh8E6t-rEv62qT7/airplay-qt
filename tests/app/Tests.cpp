#include "airplay/DiscoveryApi.h"
#include "app/Settings.h"
#include "ui/MainWindow.h"
#include <QFile>
#include <QGroupBox>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
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
#endif
class AppTests : public QObject {
  Q_OBJECT
private slots:
#ifdef Q_OS_WIN
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
#endif
  void guiSmoke() {
    airplay::DiscoveryApi api;
#ifdef Q_OS_WIN
    api.browse = [](auto *, auto *) { return DNS_STATUS(ERROR_NOT_SUPPORTED); };
#elif defined(Q_OS_LINUX)
    api.service = "org.airplayqt.UnavailableAvahi";
#else
    api.browse = [](DNSServiceRef *, DNSServiceFlags, uint32_t, const char *,
                    const char *, DNSServiceBrowseReply,
                    void *) -> DNSServiceErrorType {
      return kDNSServiceErr_Unsupported;
    };
#endif
    ui::MainWindow window(api);
    window.show();
    QVERIFY(window.isVisible());
    QVERIFY(window.findChildren<QComboBox *>().size() >= 3);
    auto *panel = window.findChild<ui::StreamingPanel *>();
    auto *toggle = window.findChild<QPushButton *>("languageToggle");
    auto *input = window.findChild<QGroupBox *>();
    QVERIFY(panel && toggle && input);
    const auto widgetCount = window.findChildren<QWidget *>().size();
    const auto originalTitle = input->title();
    auto *devices = window.findChild<QComboBox *>("captureDevice");
    QVERIFY(devices);
    const audio::DriverInfo loopback{"loopback:test", "MacBook Speakers", audio::CaptureKind::Loopback};
    const auto name = loopback.displayName();
    devices->addItem(name.render(), loopback.id);
    devices->setItemData(0, QJsonArray(name), Qt::UserRole + 1);
    devices->setCurrentIndex(0);
    QSignalSpy selectionChanged(devices, &QComboBox::currentIndexChanged);
#ifdef Q_OS_MACOS
    QVERIFY(originalTitle.endsWith("Audio Capture Device"));
#else
    QVERIFY(originalTitle.endsWith("Input Device"));
#endif
    QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() +
                               "/MainWindow-en.png"));
    // The initialization timer has not run: language changes cannot save settings.
    // Keep signals enabled to exercise MainWindow's combo translations too.
    toggle->click();
#ifdef Q_OS_MACOS
    QVERIFY(input->title().endsWith("音频采集设备"));
#else
    QVERIFY(input->title().endsWith("输入设备"));
#endif
    QCOMPARE(toggle->text(), QString("English"));
    QCOMPARE(devices->currentText(), QString("MacBook Speakers(自动环回)"));
    QCOMPARE(devices->currentData().toString(), loopback.id);
    QCOMPARE(selectionChanged.count(), 0);
    QCOMPARE(window.findChildren<QWidget *>().size(), widgetCount);
    QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() +
                               "/MainWindow-zh.png"));
    toggle->click();
    QCOMPARE(input->title(), originalTitle);
    QCOMPARE(devices->currentText(), QString("MacBook Speakers(auto loopback)"));
    QCOMPARE(selectionChanged.count(), 0);
    window.close();
  }
};
QTEST_MAIN(AppTests)
#include "Tests.moc"
