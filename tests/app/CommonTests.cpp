#include "airplay/DiscoveryApi.h"
#include "app/Settings.h"
#include "ui/StreamingPanel.h"
#include "../airplay/TestReceiver.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
class CommonAppTests : public QObject {
  Q_OBJECT
private slots:
  void logLifetimeMatchesPanel() {
    app::SessionController session;
    const auto message = i18n::text(i18n::Id::WaitingForPTPSynchronization);
    emit session.log(message);
    for (int cycle = 0; cycle < 3; ++cycle) {
      {
        ui::StreamingPanel panel(session);
        auto *log = panel.findChild<QPlainTextEdit *>("sessionLog");
        QVERIFY(log);
        QVERIFY(log->toPlainText().isEmpty());
        emit session.log(message);
        const auto text = log->toPlainText();
        QVERIFY(text.endsWith("  Waiting for PTP synchronization"));
        QVERIFY(QTime::fromString(text.left(12), "HH:mm:ss.zzz").isValid());
        QCOMPARE(log->document()->blockCount(), 1);
        panel.showError(i18n::text(i18n::Id::SelectOneOrTwoReceivers));
        QCOMPARE(log->document()->blockCount(), 2);
      }
      emit session.log(message);
    }
  }
  void inputVolumeCrossesNetworkThreadAndUpdatesDisplay() {
    app::SessionController session;
    ui::StreamingPanel panel(session);
    QSignalSpy errors(&session, &app::SessionController::error);
    connect(&session, &app::SessionController::startCapture, &session,
            &app::SessionController::captureStarted);
    for (int run = 0; run < 2; ++run) {
      test::Receiver left("left"), right("right");
      app::Timing timing;
      timing.settle = 0;
      timing.prebuffer = .008;
      audio::CaptureStream stream{
          std::make_shared<audio::CaptureQueue>(352, 704, 704, 128),
          audio::format(16), audio::format(16), 352};
      stream.gapPolicy = audio::GapPolicy::Silence;
      session.inputVolume(-5);
      session.inputVolumeStep(1);
      session.start(timing, stream,
                    {{QHostAddress::LocalHost, left.port()}, {QHostAddress::LocalHost, right.port()}},
                    {[] {}, [] {}, false});
      session.inputVolume(-5); // Connecting is not a valid volume-control state.
      QTRY_VERIFY(session.streaming());
      QCOMPARE(session.currentVolume(), -30.);
      session.inputVolume(-25);
      for (int i = 0; i < 5; ++i) session.inputVolumeStep(1);
      QTRY_COMPARE(session.currentVolume(), -20.);
      QCOMPARE(left.volumes.last(), -20.);
      QCOMPARE(right.volumes.last(), -20.);
      bool displayed = false;
      for (auto *spin : panel.findChildren<QDoubleSpinBox *>())
        if (spin->suffix() == " dB") { QCOMPARE(spin->value(), -20.); displayed = true; }
      QVERIFY(displayed);
      const auto count = left.volumeRequests;
      session.stop();
      session.inputVolume(-10);
      session.inputVolumeStep(-1);
      QTRY_VERIFY(!session.busy());
      QCOMPARE(left.volumeRequests, count);
      QVERIFY(errors.isEmpty());
    }
  }
  void pauseDisplayFreezesOnlyTelemetry() {
    app::SessionController session, otherSession;
    ui::StreamingPanel panel(session), other(otherSession);
    auto *pause = panel.findChild<QPushButton *>("pauseDisplay");
    auto *left = panel.findChild<QProgressBar *>("leftLevel");
    auto *right = panel.findChild<QProgressBar *>("rightLevel");
    auto *stats = panel.findChild<QLabel *>("statistics");
    auto *status = panel.findChild<QLabel *>("sessionStatus");
    auto *log = panel.findChild<QPlainTextEdit *>("sessionLog");
    auto *first = panel.findChild<QLineEdit *>("manualFirst");
    QVERIFY(pause && left && right && stats && status && log && first);
    first->setText("192.0.2.1:7000");
    const auto timing = panel.timing();
    QSignalSpy starts(&panel, &ui::StreamingPanel::startRequested);
    QSignalSpy stops(&panel, &ui::StreamingPanel::stopRequested);
    QSignalSpy edits(&panel, &ui::StreamingPanel::timingChanged);
    emit session.telemetry(.5, .25, .02, 123, 4, 5);
    const auto frozen = stats->text();
    pause->click();
    QVERIFY(pause->isChecked());
    QVERIFY(!session.telemetryEnabled());
    QCOMPARE(pause->text(), QString("Resume Display"));
    QVERIFY(otherSession.telemetryEnabled());
    emit session.telemetry(0, 0, .03, 456, 7, 8);
    panel.setBusy(false);
    QCOMPARE(left->value(), 500);
    QCOMPARE(right->value(), 250);
    QCOMPARE(stats->text(), frozen);
    emit session.status(i18n::text(i18n::Id::Stopped));
    QCOMPARE(status->text(), QString("Stopped"));
    emit session.log(i18n::text(i18n::Id::WaitingForPTPSynchronization));
    const auto history = log->toPlainText();
    QVERIFY(history.endsWith("Waiting for PTP synchronization"));
    panel.showError(i18n::text(i18n::Id::SelectOneOrTwoReceivers));
    QVERIFY(log->toPlainText().contains("Error: Select one or two receivers"));
    panel.setLanguage(i18n::Language::Chinese);
    QCOMPARE(pause->text(), QString("恢复显示"));
    QVERIFY(stats->text().contains("每台包数 123"));
    QCOMPARE(left->value(), 500);
    QVERIFY(log->toPlainText().startsWith(history));
    pause->click();
    QVERIFY(session.telemetryEnabled());
    QCOMPARE(left->value(), 500);
    emit session.telemetry(.1, .2, .03, 456, 7, 8);
    QCOMPARE(left->value(), 100);
    QVERIFY(stats->text().contains("每台包数 456"));
    QCOMPARE(first->text(), QString("192.0.2.1:7000"));
    QCOMPARE(panel.timing().lead, timing.lead);
    QVERIFY(starts.isEmpty() && stops.isEmpty() && edits.isEmpty());

    panel.resize(1400, 880);
    panel.show();
    QCoreApplication::processEvents();
    auto *controls = panel.findChild<QWidget *>("controlsPane");
    QVERIFY(controls);
    QVERIFY(log->x() >= controls->geometry().right());
    QCOMPARE(log->y(), controls->y());
    QCOMPARE(log->height(), controls->height());
    QVERIFY(std::abs(log->width() - controls->width()) <= 1);
    QCOMPARE(log->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    QCOMPARE(log->maximumBlockCount(), 1000);
    panel.appendLog(QStringLiteral("Receiver 192.0.2.1:7000: diagnostic detail; ")
                        .repeated(20));
    pause->click();
    for (const auto language :
         {i18n::Language::English, i18n::Language::Chinese}) {
      panel.setLanguage(language);
      QCoreApplication::processEvents();
      QVERIFY(pause->width() >= pause->sizeHint().width());
      const auto suffix = language == i18n::Language::English ? "en" : "zh";
      QVERIFY(panel.grab().save(QCoreApplication::applicationDirPath() +
                               "/PausedDisplay-" + suffix + ".png"));
      QVERIFY(std::abs(log->width() - controls->width()) <= 1);
    }
  }
  void telemetryGateSurvivesQueuedEventsAndRestart() {
    app::SessionController session;
    session.setTelemetryEnabled(false);
    QSignalSpy telemetry(&session, &app::SessionController::telemetry);
    QSignalSpy errors(&session, &app::SessionController::error);
    for (int run = 0; run < 2; ++run) {
      test::Receiver receiver("single");
      app::Timing timing;
      timing.settle = 0;
      timing.prebuffer = .008;
      timing.backlog = .5;
      timing.late = .5;
      audio::CaptureStream stream{
          std::make_shared<audio::CaptureQueue>(352, 704, 704, 128),
          audio::format(16), audio::format(16), 352};
      QTimer producer;
      QElapsedTimer clock;
      std::array<int16_t, 352> samples;
      samples.fill(4096);
      connect(&producer, &QTimer::timeout, &producer, [&] {
        const auto target = quint64(clock.nsecsElapsed()) * 44100 / 1000000000;
        while (stream.queue->capturedFrames() + 352 <= target)
          if (!stream.queue->push(samples.data(), samples.data()))
            break;
      });
      connect(&session, &app::SessionController::startCapture, &producer, [&] {
        clock.start();
        producer.start(4);
        session.captureStarted();
      });
      connect(&session, &app::SessionController::stopCapture, &producer,
              &QTimer::stop);
      telemetry.clear();
      session.start(timing, stream,
                    {{QHostAddress::LocalHost, receiver.port()}},
                    {[] {}, [] {}, false});
      QTRY_VERIFY(receiver.packets.size() > 20);
      QVERIFY(telemetry.isEmpty());
      producer.stop();
      QTRY_COMPARE(stream.queue->queuedFrames(), uint64_t(0));
      QTRY_COMPARE(receiver.packets.size(),
                   qsizetype(stream.queue->capturedFrames() / 352));
      session.setTelemetryEnabled(true);
      QTRY_VERIFY(!telemetry.isEmpty());
      // Each new session starts at zero, even if it sent packets while paused.
      QCOMPARE(telemetry.first()[3].toULongLong(), quint64(0));
      QCOMPARE(telemetry.first()[4].toULongLong(), quint64(0));
      QCOMPARE(telemetry.first()[5].toULongLong(), quint64(0));
      producer.start(4);
      QTRY_VERIFY(telemetry.last()[3].toULongLong() > 0);
      telemetry.clear();
      // Leave old telemetry queued while the network thread keeps polling.
      QTest::qSleep(150);
      session.setTelemetryEnabled(false);
      session.setTelemetryEnabled(true);
      QCoreApplication::sendPostedEvents(&session, QEvent::MetaCall);
      QVERIFY(telemetry.isEmpty());
      QTRY_VERIFY(!telemetry.isEmpty());
      session.setTelemetryEnabled(false);
      telemetry.clear();
      const auto received = receiver.packets.size();
      QTRY_VERIFY(receiver.packets.size() > received + 20);
      QVERIFY(telemetry.isEmpty());
      session.stop();
      QTRY_VERIFY(!session.busy());
      QVERIFY(errors.isEmpty());
    }
  }
  void messagesPreserveLiteralArgumentsAndCatalogueCoverage() {
    using namespace i18n;
    const auto message =
        text(Id::SingleReceiverIdentity).arg("设备 %2").arg("192.0.2.1");
    QCOMPARE(message.render(), QString("Single receiver: 设备 %2\n192.0.2.1"));
    QCOMPARE(message.render(Language::Chinese),
             QString("单台接收端：设备 %2\n192.0.2.1"));
    const auto nested = text(Id::ErrorPrefix) + message;
    QCOMPARE(Message(QJsonArray(nested)).render(), nested.render());
    try {
      throw MessageError(nested);
    } catch (const std::exception &error) {
      QCOMPARE(fromException(error).render(Language::Chinese),
               nested.render(Language::Chinese));
    }
    const std::runtime_error native("native %1 error");
    QCOMPARE(fromException(native).render(Language::Chinese),
             QString("native %1 error"));
    for (int i = 0; i < int(Id::Count); ++i) {
      auto value = text(Id(i));
      const auto english = value.render();
      const auto chinese = value.render(Language::Chinese);
      QVERIFY(!english.isEmpty());
      QVERIFY(!chinese.isEmpty());
      QVERIFY(english != chinese);
      for (const auto c : english)
        QVERIFY(c.unicode() < 0x4e00 || c.unicode() > 0x9fff);
      // Both languages must consume the same numbered arguments.
      for (int n = 1; n <= 13; ++n) {
        const auto marker = QString("%%1").arg(n);
        QCOMPARE(english.contains(marker), chinese.contains(marker));
        value = value.arg(QString("argument-%1").arg(n));
      }
      QVERIFY(!value.render().contains('%'));
      QVERIFY(!value.render(Language::Chinese).contains('%'));
    }
  }
  void languageConfigurationIsStrictAndPreservesOtherSettings() {
    using i18n::Language;
    QTemporaryDir dir;
    const auto path = dir.filePath("config.json");
    app::SettingsStore store(path);
    QCOMPARE(store.load().language, Language::English);
    app::Settings original;
    original.driverId = "device";
    original.left = 4;
    original.right = 5;
    original.networkBinding = {"en-test", "192.0.2.10"};
    original.timing.lead = .25;
    original.receiverSelection = {{"设备", "192.0.2.1:7000"}};
    store.saveStart(original, original.receiverSelection);
    store.saveLanguage(Language::Chinese);
    auto expected = original.json();
    expected["language"] = "zh-CN";
    QCOMPARE(app::Settings::load(path).json(), expected);
    app::SettingsStore reopened(path);
    QCOMPARE(reopened.load().language, Language::Chinese);
    reopened.saveLanguage(Language::English);
    QCOMPARE(app::Settings::load(path).json(), original.json());
    auto json = original.json();
    for (const auto invalid :
         {QJsonValue(), QJsonValue(true), QJsonValue(0), QJsonValue(""),
          QJsonValue("fr"), QJsonValue("EN")}) {
      json["language"] = invalid;
      QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                               app::Settings::fromJson(json));
    }
    json.remove("language");
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    for (int version : {1, 2, 4}) {
      json = original.json();
      json["version"] = version;
      QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                               app::Settings::fromJson(json));
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{broken");
    file.close();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, reopened.load());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             reopened.saveLanguage(Language::Chinese));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{broken"));
    app::SettingsStore unwritable(dir.filePath("missing/config.json"));
    unwritable.load();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             unwritable.saveLanguage(Language::Chinese));
  }
  void languageSwitchPreservesUiAndHistory() {
    using namespace i18n;
    app::SessionController session, otherSession;
    ui::StreamingPanel panel(session), other(otherSession);
    auto *toggle = panel.findChild<QPushButton *>("languageToggle");
    auto *start = panel.findChild<QPushButton *>("start");
    auto *modes = panel.findChild<QTabWidget *>("receiverModes");
    auto *first = panel.findChild<QLineEdit *>("manualFirst");
    auto *log = panel.findChild<QPlainTextEdit *>();
    auto *list = panel.findChild<QListWidget *>("receivers");
    auto *discovery = panel.findChild<airplay::ReceiverDiscovery *>();
    QVERIFY(toggle && start && modes && first && log && list && discovery);
    QCOMPARE(panel.language(), Language::English);
    QCOMPARE(toggle->text(), QString("中文"));
    QCOMPARE(start->text(), QString("Start"));
    emit discovery->found("设备 %1", "192.0.2.1:7000");
    list->item(0)->setCheckState(Qt::Checked);
    auto *selected = list->item(0);
    list->setCurrentItem(selected);
    first->setText("192.0.2.2:7001");
    auto timing = panel.timing();
    timing.lead = .5;
    panel.setTiming(timing);
    emit session.log(text(Id::WaitingForPTPSynchronization));
    const auto history = log->toPlainText();
    emit session.status(text(Id::StreamingKHzBitStereo));
    emit session.telemetry(.5, 0, .02, 123, 4, 5);
    emit session.streamingChanged(true);
    emit session.volumeApplied(-30);
    panel.setBusy(true);
    QSignalSpy starts(&panel, &ui::StreamingPanel::startRequested);
    QSignalSpy stops(&panel, &ui::StreamingPanel::stopRequested);
    QSignalSpy networks(&panel, &ui::StreamingPanel::networkBindingChanged);
    toggle->click();
    QCOMPARE(panel.language(), Language::Chinese);
    QCOMPARE(toggle->text(), QString("English"));
    QCOMPARE(start->text(), QString("开始"));
    QCOMPARE(other.findChild<QPushButton *>("start")->text(), QString("Start"));
    QCOMPARE(list->item(0), selected);
    QCOMPARE(list->currentItem(), selected);
    QCOMPARE(selected->checkState(), Qt::Checked);
    QCOMPARE(selected->text(), QString("设备 %1 · 192.0.2.1:7000"));
    QCOMPARE(first->text(), QString("192.0.2.2:7001"));
    QCOMPARE(panel.timing().lead, .5);
    QVERIFY(!start->isEnabled());
    QCOMPARE(log->toPlainText(), history);
    QVERIFY(session.streaming());
    QCOMPARE(session.currentVolume(), -30.);
    QVERIFY(starts.isEmpty() && stops.isEmpty() && networks.isEmpty());
    emit session.log(text(Id::WaitingForPTPSynchronization));
    QVERIFY(log->toPlainText().startsWith(history));
    QVERIFY(log->toPlainText().endsWith("等待 PTP 同步"));
    panel.showError(text(Id::SelectOneOrTwoReceivers));
    toggle->click();
    bool translatedError = false, translatedStats = false;
    for (auto *label : panel.findChildren<QLabel *>()) {
      translatedError |= label->text() == "Error: Select one or two receivers";
      translatedStats |= label->text().contains("Packets/receiver 123");
    }
    QVERIFY(translatedError && translatedStats);
    const auto currentLog = log->toPlainText();
    ui::StreamingPanel reopened(session);
    QVERIFY(reopened.findChild<QPlainTextEdit *>()->toPlainText().isEmpty());
    QCOMPARE(log->toPlainText(), currentLog);
  }
  void networkConfigurationAndUi() {
    app::Settings settings;
    settings.networkBinding = {"en-test", "192.0.2.10"};
    auto json = settings.json();
    QCOMPARE(json["version"].toInt(), 3);
    QCOMPARE(app::Settings::fromJson(json).networkBinding,
             settings.networkBinding);
    json["version"] = 1;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    json["version"] = 3;
    json.remove("networkBinding");
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    QTemporaryDir dir;
    app::SettingsStore store(dir.filePath("config.json"));
    store.load();
    QList<app::ReceiverSelection> selection{{"L", "192.0.2.1:7000"}};
    store.saveStart(settings, selection);
    QCOMPARE(app::Settings::load(dir.filePath("config.json")).networkBinding,
             settings.networkBinding);
    auto invalid = settings;
    invalid.networkBinding.ipv4 = "invalid";
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        store.saveStart(invalid, QList<app::ReceiverSelection>{}));
    QCOMPARE(app::Settings::load(dir.filePath("config.json")).receiverSelection,
             selection);
    app::SessionController session;
    ui::StreamingPanel panel(session);
    panel.setNetworkBinding(settings.networkBinding);
    QCOMPARE(panel.networkBinding(), settings.networkBinding);
    auto *combo = panel.findChild<QComboBox *>("networkBinding");
    QVERIFY(combo->currentText().contains("unavailable"));
    auto *refresh = panel.findChild<QPushButton *>("refreshNetwork");
    refresh->click();
    QCOMPARE(panel.networkBinding(), settings.networkBinding);
    panel.setRecoveryPending(true);
    QVERIFY(!combo->isEnabled() && !refresh->isEnabled());
    panel.setRecoveryPending(false);
    QVERIFY(combo->isEnabled() && refresh->isEnabled());
    QSignalSpy changes(&panel, &ui::StreamingPanel::networkBindingChanged);
    combo->setCurrentIndex(0);
    QCOMPARE(changes.size(), 1);
    QVERIFY(panel.networkBinding().automatic());
    panel.cancelDiscovery();
  }
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
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        store.saveStart({}, QList<app::ReceiverSelection>{}));
    QCOMPARE(store.load().receiverSelection.size(), 0);
    const QList<app::ReceiverSelection> pair{{"L", "192.0.2.1:7000"},
                                             {"R", "192.0.2.2:7001"}};
    store.saveStart({}, pair);
    QCOMPARE(app::Settings::load(path).receiverSelection, pair);
    app::Settings inputs;
    inputs.driverId = "new-input";
    store.saveStart(
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
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        store.saveStart({}, QList<app::ReceiverSelection>{pair[0], pair[0]}));
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        store.saveStart(
            {}, QList<app::ReceiverSelection>{pair[0], pair[1], pair[0]}));
    QCOMPARE(app::Settings::load(path).receiverSelection, pair);
    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{broken");
    corrupt.close();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.load());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.saveStart({}, pair));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.saveStart(inputs));
    QVERIFY(corrupt.open(QIODevice::ReadOnly));
    QCOMPARE(corrupt.readAll(), QByteArray("{broken"));
    app::SettingsStore unwritable(dir.filePath("missing/config.json"));
    unwritable.load();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             unwritable.saveStart({}, pair));
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
#elif defined(Q_OS_LINUX)
    api.service = "org.airplayqt.UnavailableAvahi";
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
    json["version"] = 1;
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
