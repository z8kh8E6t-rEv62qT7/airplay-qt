#include "airplay/DiscoveryApi.h"
#include "app/Settings.h"
#include "ui/MainWindow.h"
#include <QFile>
#include <QGroupBox>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSplitter>
#include <QtTest>
#ifdef Q_OS_MACOS
#include "../airplay/TestReceiver.h"
#include <QAction>
#include <QProcess>
#endif
namespace app {
class ControllerTestAccess {
public:
  static void settingsPath(Controller &controller, const QString &path) {
    controller.settings_ = SettingsStore(path);
  }
  static void permissionPending(Controller &controller, bool pending) {
    controller.permissionPending_ = pending;
    emit controller.busyChanged(pending);
  }
};
} // namespace app
namespace ui {
class MainWindowTestAccess {
public:
  static app::Controller &controller(MainWindow &window) { return window.controller_; }
};
} // namespace ui
namespace {
airplay::DiscoveryApi unavailableDiscovery() {
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
  return api;
}
struct CaptureState {
  QList<audio::DriverInfo> devices{{"a", "Same name"}, {"b", "Same name"}};
  QList<audio::ChannelInfo> channels{{0, "First", 0}, {1, "Second", 0}, {2, "Third", 0}};
  int enumerations = 0, creations = 0, opens = 0, closes = 0, starts = 0, stops = 0;
  bool failEnumeration = false, failOpen = false, failClose = false;
  QString openedId;
  QPointer<audio::InputCapture> capture;
};
class FakeCapture final : public audio::InputCapture {
public:
  explicit FakeCapture(CaptureState &state) : state_(state) { state.capture = this; }
  QList<audio::ChannelInfo> open(const QString &id, void *) override {
    ++state_.opens;
    if (state_.failOpen) throw std::runtime_error("open failed");
    state_.openedId = id;
    return state_.channels;
  }
  i18n::Message close() noexcept override {
    ++state_.closes;
    if (state_.failClose) return i18n::Message("close failed");
    state_.openedId.clear();
    return {};
  }
  i18n::Message stop() noexcept override { ++state_.stops; return {}; }
  void start() override { ++state_.starts; }
  void controlPanel() override {}
  audio::CaptureStream prepare(int, int, int, int) override { return {}; }

private:
  CaptureState &state_;
};
struct RefreshWindow {
  QTemporaryDir directory;
  CaptureState state;
  std::unique_ptr<ui::MainWindow> window;
  explicit RefreshWindow(QString savedId = "b") {
    app::Settings saved;
    saved.driverId = savedId;
    saved.left = saved.right = 2;
    saved.save(path());
    audio::InputCaptureApi api;
    api.devices = [this] {
      ++state.enumerations;
      if (state.failEnumeration) throw std::runtime_error("enumeration failed");
      return state.devices;
    };
    api.create = [this](audio::CaptureKind) {
      ++state.creations;
      return std::make_unique<FakeCapture>(state);
    };
    window = std::make_unique<ui::MainWindow>(unavailableDiscovery(), api);
    app::ControllerTestAccess::settingsPath(controller(), path());
  }
  QString path() const { return directory.filePath("settings.json"); }
  QByteArray config() const {
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("read config failed");
    return file.readAll();
  }
  app::Controller &controller() { return ui::MainWindowTestAccess::controller(*window); }
  QPushButton *refresh() const { return window->findChild<QPushButton *>("refreshAudioDevices"); }
  QComboBox *devices() const { return window->findChild<QComboBox *>("captureDevice"); }
  QComboBox *left() const { return window->findChild<QComboBox *>("leftInputChannel"); }
  QComboBox *right() const { return window->findChild<QComboBox *>("rightInputChannel"); }
};
} // namespace
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
  void windowLayoutSurvivesCloseAndRecreate() {
    RefreshWindow fixture;
    auto &window = *fixture.window;
    window.show();
    QCoreApplication::processEvents();
    window.resize(1300, 1000);
    auto *columns = window.findChild<QSplitter *>("streamingColumns");
    QVERIFY(columns);
    columns->setSizes({700, 500});
    QCoreApplication::processEvents();
    auto *toggle = window.findChild<QPushButton *>("toggleLog");
    QVERIFY(toggle);
    const auto expandedSize = window.size();
    const auto expandedParts = columns->sizes();
    toggle->click();
    QCoreApplication::processEvents();
    const auto compactSize = window.size();
#ifdef Q_OS_MACOS
    window.findChild<QAction *>("closeWindow")->trigger();
#else
    window.close();
#endif
    const auto saved = app::Settings::load(fixture.path());
    QCOMPARE(saved.windowLayout.width, compactSize.width());
    QCOMPARE(saved.windowLayout.height, compactSize.height());
    QCOMPARE(saved.windowLayout.expandedWidth, expandedSize.width());
    QVERIFY(!saved.windowLayout.logVisible);
    QCOMPARE(saved.windowLayout.controlWidth, expandedParts[0]);
    QCOMPARE(saved.windowLayout.logWidth, expandedParts[1]);
    audio::InputCaptureApi api;
    api.devices = [] { return QList<audio::DriverInfo>{}; };
    ui::MainWindow restored(unavailableDiscovery(), api);
    app::ControllerTestAccess::settingsPath(
        ui::MainWindowTestAccess::controller(restored), fixture.path());
    restored.show();
    QCoreApplication::processEvents();
    QCOMPARE(restored.size(), compactSize);
    QVERIFY(restored.findChild<QPlainTextEdit *>("sessionLog")->isHidden());
    restored.findChild<QPushButton *>("toggleLog")->click();
    QCoreApplication::processEvents();
    QCOMPARE(restored.size(), expandedSize);
    QCOMPARE(restored.findChild<QSplitter *>("streamingColumns")->sizes(), expandedParts);
    restored.close();
  }
  void logToggleResizesWindowWithoutLosingHistory() {
    RefreshWindow fixture;
    auto &window = *fixture.window;
    auto *panel = window.findChild<ui::StreamingPanel *>();
    auto *toggle = window.findChild<QPushButton *>("toggleLog");
    auto *log = window.findChild<QPlainTextEdit *>("sessionLog");
    QVERIFY(panel && toggle && log);
    window.resize(1400, 1000);
    window.show();
    QCoreApplication::processEvents();
    const auto expanded = window.size();
    auto *columns = window.findChild<QSplitter *>("streamingColumns");
    auto *controls = window.findChild<QWidget *>("controlsPane");
    auto *meters = window.findChild<QWidget *>("metersPane");
    QVERIFY(columns && controls && meters);
    const int controlsWidth = controls->width();
    const int metersWidth = meters->width();
    auto sizes = columns->sizes();
    columns->setSizes({sizes[0] + 120, sizes[1] - 120});
    QCoreApplication::processEvents();
    QVERIFY(controls->width() > controlsWidth);
    QCOMPARE(meters->width(), metersWidth);
    QCOMPARE(window.size(), expanded);
    const auto adjustedSizes = columns->sizes();
    panel->appendLog("before hiding");
    const auto history = log->toPlainText();
    for (int i = 0; i < 3; ++i) {
      toggle->click();
      QCoreApplication::processEvents();
      QVERIFY(log->isHidden());
      QVERIFY(window.width() < expanded.width());
      QCOMPARE(window.height(), expanded.height());
      panel->setLanguage(i18n::Language::Chinese);
      QCOMPARE(toggle->text(), QString("显示日志"));
      panel->appendLog("while hidden");
      QVERIFY(log->toPlainText().startsWith(history));
      QVERIFY(log->toPlainText().endsWith("while hidden"));
      if (i == 0)
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() +
                                  "/MainWindow-log-hidden.png"));
      toggle->click();
      QCoreApplication::processEvents();
      QVERIFY(!log->isHidden());
      QCOMPARE(window.size(), expanded);
      QCOMPARE(columns->sizes(), adjustedSizes);
      QCOMPARE(toggle->text(), QString("隐藏日志"));
    }
  }
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
  void refreshPreservesIdentityChannelsAndConfig() {
    RefreshWindow test;
    QVERIFY(!test.refresh()->isEnabled());
    QTRY_VERIFY(test.refresh()->isEnabled());
    const auto config = test.config();
    QCOMPARE(test.state.openedId, QString("b"));
    QCOMPARE(test.left()->currentData().toInt(), 2);
    QCOMPARE(test.right()->currentData().toInt(), 2);
    test.state.devices = {{"b", "Renamed"}, {"c", "New device"}, {"a", "Same name"}};
    QSignalSpy selection(test.devices(), &QComboBox::currentIndexChanged);
    for (int i = 0; i < 3; ++i) {
      const auto opens = test.state.opens, queries = test.state.enumerations;
      test.refresh()->click();
      QCOMPARE(test.state.opens, opens + 1);
      QCOMPARE(test.state.enumerations, queries + 1);
      QCOMPARE(test.devices()->count(), 3);
      QCOMPARE(test.devices()->currentData().toString(), QString("b"));
      QCOMPARE(test.devices()->currentText(), QString("Renamed"));
      QCOMPARE(test.left()->currentData().toInt(), 2);
      QCOMPARE(test.right()->currentData().toInt(), 2);
    }
    QCOMPARE(selection.count(), 0);
    test.left()->setCurrentIndex(0);
    test.state.channels.removeLast();
    test.refresh()->click();
    QCOMPARE(test.left()->currentData().toInt(), 0);
    QCOMPARE(test.right()->currentIndex(), -1);
    test.refresh()->click();
    QCOMPARE(test.right()->currentIndex(), -1); // Never replace an invalid selection.
    QCOMPARE(test.config(), config);
  }
  void refreshMissingDevice_data() {
    QTest::addColumn<bool>("empty");
    QTest::newRow("other-device-remains") << false;
    QTest::newRow("no-devices") << true;
  }
  void refreshMissingDevice() {
    QFETCH(bool, empty);
    RefreshWindow test;
    QTRY_VERIFY(test.refresh()->isEnabled());
    const auto config = test.config();
    test.state.devices = empty ? QList<audio::DriverInfo>{}
                               : QList<audio::DriverInfo>{{"a", "Same name"}};
    // Automatic notifications retain the selected device and its channels.
    emit test.state.capture->devicesChanged();
    QTRY_COMPARE(test.devices()->count(), empty ? 1 : 2);
    QCOMPARE(test.devices()->currentData().toString(), QString("b"));
    QCOMPARE(test.right()->currentData().toInt(), 2);
    QSignalSpy errors(&test.controller(), &app::Controller::error);
    const auto opens = test.state.opens;
    test.refresh()->click();
    QCOMPARE(test.devices()->count(), empty ? 0 : 1);
    QCOMPARE(test.devices()->currentIndex(), -1);
    QCOMPARE(test.left()->count(), 0);
    QCOMPARE(test.right()->count(), 0);
    QVERIFY(test.state.openedId.isEmpty());
    QCOMPARE(errors.count(), 1);
    test.state.devices = {{"a", "Same name"}, {"b", "Same name"}};
    emit test.state.capture->devicesChanged();
    QTRY_COMPARE(test.devices()->count(), 2);
    QCOMPARE(test.devices()->currentIndex(), -1);
    test.refresh()->click();
    QCOMPARE(test.devices()->currentIndex(), -1);
    QCOMPARE(test.state.opens, opens);
    QCOMPARE(test.config(), config);
  }
  void refreshFailuresAndRetry() {
    RefreshWindow test;
    QTRY_VERIFY(test.refresh()->isEnabled());
    QSignalSpy errors(&test.controller(), &app::Controller::error);
    const auto config = test.config();
    const auto opens = test.state.opens, closes = test.state.closes;
    test.state.devices.clear();
    test.state.failEnumeration = true;
    test.refresh()->click();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(test.devices()->count(), 2);
    QCOMPARE(test.devices()->currentData().toString(), QString("b"));
    QCOMPARE(test.right()->currentData().toInt(), 2);
    QCOMPARE(test.state.opens, opens);
    QCOMPARE(test.state.closes, closes);
    test.state.failEnumeration = false;
    test.state.devices = {{"b", "Device"}};
    test.state.failOpen = true;
    test.refresh()->click();
    QCOMPARE(errors.count(), 2);
    QCOMPARE(test.devices()->currentData().toString(), QString("b"));
    QCOMPARE(test.left()->count(), 0);
    QCOMPARE(test.right()->count(), 0);
    test.state.failOpen = false;
    test.refresh()->click();
    QCOMPARE(test.state.openedId, QString("b"));
    QCOMPARE(test.left()->count(), 3);
    QCOMPARE(test.left()->currentIndex(), -1);
    QCOMPARE(test.right()->currentIndex(), -1);
    QCOMPARE(test.config(), config);
  }
  void refreshCleanupFailure_data() {
    QTest::addColumn<bool>("missing");
    QTest::newRow("reopen") << false;
    QTest::newRow("removed") << true;
  }
  void refreshCleanupFailure() {
    QFETCH(bool, missing);
    RefreshWindow test;
    QTRY_VERIFY(test.refresh()->isEnabled());
    const auto capture = test.state.capture;
    const auto creations = test.state.creations;
    test.state.failClose = true;
    if (missing) test.state.devices.clear();
    QSignalSpy errors(&test.controller(), &app::Controller::error);
    test.refresh()->click();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(test.state.creations, creations);
    QCOMPARE(test.state.capture, capture);
    QVERIFY(capture);
    QCOMPARE(test.left()->count(), 0);
    QCOMPARE(test.right()->count(), 0);
    test.state.failClose = false;
    test.refresh()->click();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(test.state.creations, creations + (missing ? 0 : 1));
    QCOMPARE(test.state.openedId, missing ? QString() : QString("b"));
  }
  void refreshWithoutSelectionAndBusyGuard() {
    RefreshWindow test("");
    QTRY_VERIFY(test.refresh()->isEnabled());
    test.refresh()->click();
    QCOMPARE(test.devices()->currentIndex(), -1);
    QCOMPARE(test.state.opens, 0);
    const auto queries = test.state.enumerations;
    app::ControllerTestAccess::permissionPending(test.controller(), true);
    QVERIFY(!test.refresh()->isEnabled());
    test.refresh()->click();
    test.controller().refreshDevices("a", nullptr);
    QCOMPARE(test.state.enumerations, queries);
    app::ControllerTestAccess::permissionPending(test.controller(), false);
    QVERIFY(test.refresh()->isEnabled());
    test.window->close();
#ifdef Q_OS_MACOS
    // The reusable window is hidden, so its controls remain ready on reopen.
    QVERIFY(test.refresh()->isEnabled());
#else
    QVERIFY(!test.refresh()->isEnabled());
    test.refresh()->click();
    QCOMPARE(test.state.enumerations, queries);
#endif
  }
#ifdef Q_OS_MACOS
  void macCloseKeepsStreamingAndDockRestores_data() {
    QTest::addColumn<int>("method");
    QTest::newRow("window-close") << 0;
    QTest::newRow("command-w") << 1;
  }
  void macCloseKeepsStreamingAndDockRestores() {
    QFETCH(int, method);
    RefreshWindow fixture;
    QTRY_VERIFY(fixture.refresh()->isEnabled());
    QVERIFY(!qApp->quitOnLastWindowClosed());
    fixture.window->show();
    fixture.window->activateWindow();
    QTRY_VERIFY(fixture.window->isActiveWindow());
    test::Receiver receiver("background");
    app::Timing timing;
    timing.settleMs = 0;
    timing.packetSamples = 352;
    timing.prebufferSamples = 512;
    audio::CaptureStream stream{
        std::make_shared<audio::CaptureQueue>(352, 704, 704, 128),
        audio::format(16), audio::format(16), 352};
    stream.gapPolicy = audio::GapPolicy::Silence;
    auto &session = fixture.controller().session();
    session.start(timing, stream, {{QHostAddress::LocalHost, receiver.port()}},
                  {[] {}, [] {}, false});
    QTRY_VERIFY(session.streaming());
    QTRY_VERIFY(!receiver.packets.isEmpty());
    const auto stops = fixture.state.stops;
    const auto opens = fixture.state.opens;
    const auto config = fixture.config();
    auto *log = fixture.window->findChild<QPlainTextEdit *>("sessionLog");
    QVERIFY(log);
    emit session.log(i18n::Message("background-log-marker"));
    if (method == 0)
      fixture.window->close();
    else
      QTest::keyClick(fixture.window.get(), Qt::Key_W, Qt::ControlModifier);
    QVERIFY(!fixture.window->isVisible());
    const auto packets = receiver.packets.size();
    QTRY_VERIFY(receiver.packets.size() > packets + 2);
    QCOMPARE(fixture.state.stops, stops);
    QVERIFY(session.streaming());
    // Qt's Cocoa delegate emits this on every Dock reopen, even when active.
    qApp->applicationStateChanged(Qt::ApplicationActive);
    QVERIFY(fixture.window->isVisible());
    QVERIFY(session.streaming());
    QVERIFY(log->toPlainText().contains("background-log-marker"));
    QCOMPARE(fixture.state.opens, opens);
    QCOMPARE(fixture.devices()->currentData().toString(), QString("b"));
    QCOMPARE(fixture.left()->currentData().toInt(), 2);
    auto before = QJsonDocument::fromJson(config).object();
    auto after = QJsonDocument::fromJson(fixture.config()).object();
    const auto savedLayout = app::Settings::load(fixture.path()).windowLayout;
    QCOMPARE(savedLayout.width, fixture.window->width());
    QCOMPARE(savedLayout.height, fixture.window->height());
    before.remove("windowLayout");
    after.remove("windowLayout");
    QCOMPARE(after, before);
    fixture.controller().stop();
    QTRY_VERIFY(!session.busy());
    QVERIFY(fixture.state.stops > stops);
  }
  void macCloseDuringInitializationAndPermission() {
    RefreshWindow fixture;
    fixture.window->show();
    fixture.window->close();
    QTRY_VERIFY(fixture.refresh()->isEnabled());
    QVERIFY(!fixture.window->isVisible());
    app::ControllerTestAccess::permissionPending(fixture.controller(), true);
    const auto stops = fixture.state.stops;
    fixture.window->show();
    fixture.window->close();
    QVERIFY(fixture.controller().busy());
    QCOMPARE(fixture.state.stops, stops);
    qApp->applicationStateChanged(Qt::ApplicationActive);
    QVERIFY(fixture.window->isVisible());
    QVERIFY(!fixture.refresh()->isEnabled());
    app::ControllerTestAccess::permissionPending(fixture.controller(), false);
  }
  void macQuit_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("visible") << QString("visible");
    QTest::newRow("hidden") << QString("hidden");
  }
  void macQuit() {
    QFETCH(QString, mode);
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--quit-child", mode});
    QVERIFY(child.waitForFinished(10000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QVERIFY2(child.exitCode() == 0, child.readAllStandardError().constData());
  }
#endif
  void guiSmoke() {
    const auto api = unavailableDiscovery();
    ui::MainWindow window(api);
    window.show();
    QVERIFY(window.isVisible());
    QVERIFY(window.findChildren<QComboBox *>().size() >= 3);
    auto *panel = window.findChild<ui::StreamingPanel *>();
    auto *toggle = window.findChild<QPushButton *>("languageToggle");
    auto *input = window.findChild<QGroupBox *>();
    auto *refresh = window.findChild<QPushButton *>("refreshAudioDevices");
    QVERIFY(panel && toggle && input && refresh);
    QCOMPARE(refresh->text(), QString("Refresh"));
    QVERIFY(!refresh->isEnabled());
    const auto widgetCount = window.centralWidget()->findChildren<QWidget *>().size();
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
    QCOMPARE(refresh->text(), QString("刷新"));
    QCOMPARE(devices->currentText(), QString("MacBook Speakers(自动环回)"));
    QCOMPARE(devices->currentData().toString(), loopback.id);
    QCOMPARE(selectionChanged.count(), 0);
    QCOMPARE(window.centralWidget()->findChildren<QWidget *>().size(), widgetCount);
    QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() +
                               "/MainWindow-zh.png"));
    toggle->click();
    QCOMPARE(input->title(), originalTitle);
    QCOMPARE(refresh->text(), QString("Refresh"));
    QCOMPARE(devices->currentText(), QString("MacBook Speakers(auto loopback)"));
    QCOMPARE(selectionChanged.count(), 0);
    window.close();
  }
};
#ifdef Q_OS_MACOS
int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (application.arguments().contains("--quit-child")) {
    RefreshWindow fixture;
    fixture.window->show();
    test::Receiver receiver("quit");
    QTimer quitWhenStreaming;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &application,
                     [] { QCoreApplication::exit(2); });
    watchdog.start(5000);
    bool requested = false, cleaned = false;
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &application, [&] {
      cleaned = !fixture.controller().busy() && fixture.state.stops > 0;
      const auto layout = app::Settings::load(fixture.path()).windowLayout;
      cleaned = cleaned && layout.width == fixture.window->width() &&
                layout.height == fixture.window->height() && layout.controlWidth > 0;
      // Activation during termination must not bring the window back.
      application.applicationStateChanged(Qt::ApplicationActive);
      cleaned = cleaned && !fixture.window->isVisible();
    });
    QTimer::singleShot(0, &application, [&] {
      app::Timing timing;
      timing.settleMs = 0;
      timing.packetSamples = 352;
      timing.prebufferSamples = 512;
      audio::CaptureStream stream{
          std::make_shared<audio::CaptureQueue>(352, 704, 704, 128),
          audio::format(16), audio::format(16), 352};
      stream.gapPolicy = audio::GapPolicy::Silence;
      fixture.controller().session().start(
          timing, stream, {{QHostAddress::LocalHost, receiver.port()}},
          {[] {}, [] {}, false});
      quitWhenStreaming.start(10);
    });
    QObject::connect(&quitWhenStreaming, &QTimer::timeout, &application, [&] {
      if (!fixture.controller().session().streaming() || receiver.packets.isEmpty()) return;
      quitWhenStreaming.stop();
      fixture.window->resize(1230, 1010);
      if (application.arguments().contains("hidden")) fixture.window->close();
      auto *quit = fixture.window->findChild<QAction *>("quitApplication");
      if (!quit || quit->menuRole() != QAction::QuitRole) {
        application.exit(3);
        return;
      }
      requested = true;
      quit->trigger();
    });
    const auto result = application.exec();
    return result ? result : (requested && cleaned ? 0 : 4);
  }
  AppTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#else
QTEST_MAIN(AppTests)
#endif
#include "Tests.moc"
