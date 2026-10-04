#include "../airplay/TestReceiver.h"
#include "airplay/DiscoveryApi.h"
#include "app/Settings.h"
#include "ui/StreamingPanel.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSplitter>
#include <QScrollBar>
#include <QtTest>
#include <cmath>
#include <thread>
class CommonAppTests : public QObject {
  Q_OBJECT
private slots:
  void layoutConfigurationIsOptionalAndPreserved() {
    app::Settings settings;
    auto old = settings.json();
    old.remove("windowLayout");
    QCOMPARE(app::Settings::fromJson(old).windowLayout, app::WindowLayout{});
    settings.windowLayout = {900, 880, 600, 350, 1100, false};
    QCOMPARE(app::Settings::fromJson(settings.json()).windowLayout,
             settings.windowLayout);
    for (const QJsonValue value : {QJsonValue(-1), QJsonValue(1.5),
                                  QJsonValue(32769), QJsonValue("900")}) {
      auto json = settings.json();
      auto layout = json["windowLayout"].toObject();
      layout["width"] = value;
      json["windowLayout"] = layout;
      QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    }
    QTemporaryDir directory;
    const auto path = directory.filePath("settings.json");
    settings.save(path);
    app::SettingsStore store(path);
    store.load();
    store.saveLanguage(i18n::Language::Chinese);
    store.saveStart(app::Settings{});
    QCOMPARE(app::Settings::load(path).windowLayout, settings.windowLayout);
    settings.windowLayout = {1000, 900, 600, 350, 1000, true};
    store.saveWindowLayout(settings.windowLayout);
    QCOMPARE(app::Settings::load(path).windowLayout, settings.windowLayout);
  }
  void clearLogPreservesVisibilityAndContinuesRecording() {
    app::SessionController session;
    ui::StreamingPanel panel(session);
    auto *clear = panel.findChild<QPushButton *>("clearLog");
    auto *toggle = panel.findChild<QPushButton *>("toggleLog");
    auto *log = panel.findChild<QPlainTextEdit *>("sessionLog");
    QVERIFY(clear && toggle && log);
    QCOMPARE(clear->text(), QString("Clear Log"));
    panel.setLanguage(i18n::Language::Chinese);
    QCOMPARE(clear->text(), QString("清除日志"));
    panel.resize(1200, 880);
    panel.show();
    QCoreApplication::processEvents();
    QCOMPARE(clear->parentWidget(), toggle->parentWidget());
    QCOMPARE(clear->y(), toggle->y());
    QVERIFY(clear->x() > toggle->geometry().right());
    QSignalSpy visibilityChanges(&panel, &ui::StreamingPanel::logVisibilityChanged);
    for (bool hidden : {false, true}) {
      if (hidden)
        toggle->click();
      panel.setBusy(hidden);
      QVERIFY(clear->isEnabled());
      emit session.log(i18n::Message("old log entry"));
      QVERIFY(log->toPlainText().endsWith("old log entry"));
      const auto size = panel.size();
      const auto changes = visibilityChanges.count();
      clear->click();
      QVERIFY(log->toPlainText().isEmpty());
      clear->click(); // Clearing an empty log is harmless.
      QVERIFY(log->toPlainText().isEmpty());
      QCOMPARE(log->isHidden(), hidden);
      QCOMPARE(panel.size(), size);
      QCOMPARE(visibilityChanges.count(), changes);
      emit session.log(i18n::Message("new log entry"));
      QVERIFY(log->toPlainText().endsWith("new log entry"));
      QVERIFY(!log->toPlainText().contains("old log entry"));
      QCOMPARE(log->document()->blockCount(), 1);
    }
    toggle->click();
    QVERIFY(!log->isHidden());
    QVERIFY(log->toPlainText().endsWith("new log entry"));
  }
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
        panel.resize(950, 880);
        panel.show();
        QCoreApplication::processEvents();
        for (int i = 0; i < 100; ++i)
          emit session.log(message);
        auto *scroll = log->verticalScrollBar();
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(scroll->minimum());
        QVERIFY(scroll->value() < scroll->maximum());
        emit session.log(message);
        QCOMPARE(scroll->value(), scroll->maximum());
        auto *toggle = panel.findChild<QPushButton *>("toggleLog");
        toggle->click();
        scroll->setValue(scroll->minimum());
        emit session.log(message);
        toggle->click();
        QCoreApplication::processEvents();
        QCOMPARE(scroll->value(), scroll->maximum());
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
      timing.settleMs = 0;
      timing.packetSamples = 352;
      timing.prebufferSamples = 512;
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
    QCOMPARE(panel.timing().leadMs, timing.leadMs);
    QVERIFY(starts.isEmpty() && stops.isEmpty() && edits.isEmpty());

    panel.resize(1400, 880);
    panel.show();
    QCoreApplication::processEvents();
    auto *controls = panel.findChild<QWidget *>("controlsPane");
    auto *meters = panel.findChild<QWidget *>("metersPane");
    auto *columns = panel.findChild<QSplitter *>("streamingColumns");
    QVERIFY(controls && meters && columns);
    QCOMPARE(columns->count(), 2);
    QCOMPARE(columns->widget(1), log);
    QVERIFY(columns->handle(1)->isEnabled());
    QVERIFY(meters->x() > controls->geometry().right());
    QVERIFY(log->x() > meters->geometry().right());
    QCOMPARE(left->orientation(), Qt::Vertical);
    QCOMPARE(right->orientation(), Qt::Vertical);
    QVERIFY(!left->invertedAppearance() && !right->invertedAppearance());
    QVERIFY(left->mapTo(&panel, QPoint{}).x() < right->mapTo(&panel, QPoint{}).x());
    QVERIFY(left->height() > left->width());
    QCOMPARE(left->width(), 40);
    QCOMPARE(right->width(), 40);
    QCOMPARE(right->mapTo(&panel, QPoint{}).x() -
                 left->mapTo(&panel, QPoint{}).x() - left->width(),
             8);
    auto *peak = panel.findChild<QPushButton *>("leftLevelReadout");
    QVERIFY(peak);
    QCOMPARE(peak->text(), QString("-6.0\ndBFS"));
    QCOMPARE(peak->width(), left->width());
    QCOMPARE(peak->mapTo(&panel, QPoint{}).x(), left->mapTo(&panel, QPoint{}).x());
    QCOMPARE(log->y(), controls->y());
    QCOMPARE(log->height(), controls->height());
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
      QVERIFY(log->x() > meters->geometry().right());
    }
  }
  void stereoPeakHoldAndReset() {
    app::SessionController session;
    ui::StreamingPanel panel(session);
    auto *left = panel.findChild<QPushButton *>("leftLevelReadout");
    auto *right = panel.findChild<QPushButton *>("rightLevelReadout");
    auto *meter = panel.findChild<QProgressBar *>("leftLevel");
    auto *rightMeter = panel.findChild<QProgressBar *>("rightLevel");
    auto *pause = panel.findChild<QPushButton *>("pauseDisplay");
    QVERIFY(left && right && meter && rightMeter && pause);
    QCOMPARE(left->text(), QString("−∞\ndBFS"));
    emit session.telemetry(.5, .25, 0, 0, 0, 0);
    emit session.telemetry(.1, .125, 0, 0, 0, 0);
    QCOMPARE(meter->value(), 100);
    QCOMPARE(left->text(), QString("-6.0\ndBFS"));
    QCOMPARE(right->text(), QString("-12.0\ndBFS"));
    panel.setBusy(false);
    QCOMPARE(meter->value(), 0);
    QCOMPARE(left->text(), QString("-6.0\ndBFS"));
    panel.setBusy(true);
    panel.setLanguage(i18n::Language::Chinese);
    QCOMPARE(left->text(), QString("-6.0\ndBFS"));
    QVERIFY(left->toolTip().contains("清除左右"));
    for (auto *button : {left, right}) {
      emit session.telemetry(1, .5, 0, 0, 0, 0);
      QCOMPARE(left->text(), QString("0.0\ndBFS"));
      QTest::mouseClick(button, Qt::LeftButton);
      QCOMPARE(left->text(), QString("−∞\ndBFS"));
      QCOMPARE(right->text(), QString("−∞\ndBFS"));
      QCOMPARE(meter->value(), 0);
      QCOMPARE(rightMeter->value(), 0);
      emit session.telemetry(.1, .01, 0, 0, 0, 0);
      QCOMPARE(left->text(), QString("-20.0\ndBFS"));
      QCOMPARE(right->text(), QString("-40.0\ndBFS"));
    }
    pause->click();
    emit session.telemetry(1, 1, 0, 0, 0, 0);
    QCOMPARE(left->text(), QString("-20.0\ndBFS"));
    left->click();
    QCOMPARE(meter->value(), 0);
    QCOMPARE(rightMeter->value(), 0);
    QVERIFY(pause->isChecked());
    emit session.telemetry(.5, .5, 0, 0, 0, 0);
    QCOMPARE(left->text(), QString("−∞\ndBFS"));
    QCOMPARE(right->text(), QString("−∞\ndBFS"));
    QCOMPARE(meter->value(), 0);
    QCOMPARE(rightMeter->value(), 0);
    pause->click();
    emit session.telemetry(.5, .25, 0, 0, 0, 0);
    QCOMPARE(left->text(), QString("-6.0\ndBFS"));
    QCOMPARE(right->text(), QString("-12.0\ndBFS"));
    QCOMPARE(meter->value(), 500);
    QCOMPARE(rightMeter->value(), 250);
  }
  void telemetryGateSurvivesQueuedEventsAndRestart() {
    app::SessionController session;
    session.setTelemetryEnabled(false);
    QSignalSpy telemetry(&session, &app::SessionController::telemetry);
    QSignalSpy errors(&session, &app::SessionController::error);
    for (int run = 0; run < 2; ++run) {
      test::Receiver receiver("single");
      app::Timing timing;
      timing.settleMs = 0;
      timing.packetSamples = 352;
      timing.prebufferSamples = 512;
      timing.backlogSamples = 32768;
      timing.lateMs = 500;
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
    original.timing.leadMs = 250;
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
    for (int version : {1, 2, 3, 5}) {
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
    timing.leadMs = 500;
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
    QCOMPARE(panel.timing().leadMs, 500.);
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
    QCOMPARE(json["version"].toInt(), 4);
    QCOMPARE(app::Settings::fromJson(json).networkBinding,
             settings.networkBinding);
    json["version"] = 1;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, app::Settings::fromJson(json));
    json["version"] = 4;
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
  void sampleConfigurationAndCountUi() {
    app::Settings settings;
    const auto json = settings.json();
    QCOMPARE(json["timing"].toObject().size(), 14);
    QCOMPARE(settings.timing.packetSamples, 64);
    QCOMPARE(settings.timing.prebufferSamples, 2048);
    QCOMPARE(settings.timing.backlogSamples, 8192);
    for (const auto &field : app::timingSamplesFields) {
      for (const QJsonValue invalid :
           {QJsonValue(), QJsonValue(true), QJsonValue("64"), QJsonValue(0),
            QJsonValue(-1), QJsonValue(1.5), QJsonValue(2147483648.)}) {
        auto changed = json;
        auto timing = changed["timing"].toObject();
        timing[field.key] = invalid;
        changed["timing"] = timing;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 app::Settings::fromJson(changed));
      }
    }
    for (int packet : {1, 64, 352}) {
      settings.timing.packetSamples = packet;
      QCOMPARE(app::Settings::fromJson(settings.json()).timing.packetSamples,
               packet);
    }
    settings.timing.packetSamples = 353;
    QVERIFY(!settings.validate().isEmpty());
    settings = {};
    settings.timing.prebufferSamples = 3;
    settings.timing.backlogSamples = 8193;
    QVERIFY(settings.validate().isEmpty());
    QCOMPARE(app::Settings::fromJson(settings.json()).timing, settings.timing);
    settings.timing.prebufferSamples = 1;
    settings.timing.backlogSamples = 1 << 30;
    QVERIFY(settings.validate().isEmpty());
    QCOMPARE(app::Settings::fromJson(settings.json()).timing.backlogSamples,
             1 << 30);
    app::SessionController session;
    ui::StreamingPanel panel(session);
    auto *packet = panel.findChild<QSpinBox *>("packetSamples");
    auto *pre = panel.findChild<QSpinBox *>("prebufferSamples");
    auto *back = panel.findChild<QSpinBox *>("backlogSamples");
    auto *packetPreview = panel.findChild<QLabel *>("packetSamplesPreview");
    auto *prePreview = panel.findChild<QLabel *>("prebufferSamplesPreview");
    auto *backPreview = panel.findChild<QLabel *>("backlogSamplesPreview");
    QVERIFY(packet && pre && back && packetPreview && prePreview && backPreview);
    QCOMPARE(packet->text(), QString("64 samples"));
    QCOMPARE(pre->text(), QString("2048 samples"));
    QCOMPARE(back->text(), QString("8192 samples"));
    QCOMPARE(packetPreview->text(), QString("1.451 ms"));
    QCOMPARE(prePreview->text(), QString("46.440 ms"));
    QCOMPARE(backPreview->text(), QString("185.760 ms"));
    panel.findChild<QTabWidget *>("receiverModes")->setCurrentIndex(1);
    panel.findChild<QLineEdit *>("manualFirst")->setText("127.0.0.1:7000");
    auto *start = panel.findChild<QPushButton *>("start");
    QVERIFY(start->isEnabled());
    QSignalSpy changed(&panel, &ui::StreamingPanel::timingChanged);
    for (const auto &field : app::timingSamplesFields) {
      auto *input = panel.findChild<QSpinBox *>(field.key);
      auto *preview = panel.findChild<QLabel *>(QString(field.key) + "Preview");
      const bool isPacket = field.member == &app::Timing::packetSamples;
      const int step = isPacket ? 32 : 256;
      QCOMPARE(input->singleStep(), step);
      QCOMPARE(input->suffix(), QString(" samples"));
      QCOMPARE(input->buttonSymbols(), QAbstractSpinBox::UpDownArrows);
      input->setValue(field.minimum);
      input->stepUp();
      QCOMPARE(input->value(), field.minimum + step);
      QCOMPARE(panel.timing().*(field.member), input->value());
      QCOMPARE(preview->text(), isPacket ? QString("0.748 ms") : QString("5.828 ms"));
      input->stepDown();
      QCOMPARE(input->value(), field.minimum);
      input->stepDown();
      QCOMPARE(input->value(), field.minimum);
      input->setValue(field.maximum - 1);
      input->stepUp();
      QCOMPARE(input->value(), field.maximum);
      input->stepUp();
      QCOMPARE(input->value(), field.maximum);
      input->setValue(field.maximum + 1);
      QCOMPARE(input->value(), field.maximum);
      input->setValue(0);
      QCOMPARE(input->value(), field.minimum);
      panel.setTiming(app::Timing{});
      QVERIFY(start->isEnabled());
    }
    QVERIFY(changed.count() > 0);
    packet->setValue(64);
    packet->stepUp();
    QCOMPARE(packet->value(), 96);
    packet->stepDown();
    QCOMPARE(packet->value(), 64);
    packet->setValue(320);
    packet->stepUp();
    QCOMPARE(packet->value(), 352);
    pre->setValue(3);
    back->setValue(8193);
    QVERIFY(panel.timing().validate().isEmpty());
    QVERIFY(start->isEnabled());
    QCOMPARE(packetPreview->text(), QString("7.982 ms"));
    QCOMPARE(prePreview->text(), QString("0.068 ms"));
    QCOMPARE(backPreview->text(), QString("185.782 ms"));
    back->setValue(3);
    QVERIFY(!start->isEnabled());
    back->setValue(2);
    QVERIFY(!start->isEnabled());
    pre->setValue(1);
    back->setValue(1073741824);
    QCOMPARE(panel.timing().prebufferSamples, 1);
    QCOMPARE(panel.timing().backlogSamples, 1 << 30);
    QCOMPARE(prePreview->text(), QString("0.023 ms"));
    QVERIFY(start->isEnabled());
    panel.setLanguage(i18n::Language::Chinese);
    QCOMPARE(pre->text(), QString("1 samples"));
    QCOMPARE(prePreview->text(), QString("0.023 ms"));
    panel.setBusy(true);
    QVERIFY(!pre->isEnabled() && !back->isEnabled() && !packet->isEnabled());
    panel.setBusy(false);
    QVERIFY(pre->isEnabled() && back->isEnabled() && packet->isEnabled());
    panel.setTiming(settings.timing);
    QCOMPARE(panel.timing(), settings.timing);
    panel.setTiming(app::Timing{});
    QCOMPARE(packet->text(), QString("64 samples"));
    QCOMPARE(pre->text(), QString("2048 samples"));
    QCOMPARE(back->text(), QString("8192 samples"));
    QTemporaryDir directory;
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        app::Settings::load(directory.filePath("missing.json"), true));
  }
  void captureQueueReblocksCallbacks() {
    for (const int packet : {1, 64, 352}) {
      audio::CaptureQueue queue(packet, size_t(packet) * 2, size_t(packet) * 4,
                                audio::CaptureQueue::capacityFor(packet, 8193));
      std::array<int16_t, 1200> left{};
      std::array<int32_t, 1200> right{};
      for (size_t i = 0; i < left.size(); ++i) {
        left[i] = int16_t(i);
        right[i] = -int32_t(i);
      }
      size_t offset = 0, consumed = 0;
      for (size_t count : {1u, 31u, 127u, 513u, 528u}) {
        uint64_t copied = 0;
        QVERIFY(queue.append(left.data() + offset, right.data() + offset, count,
                             &copied));
        QCOMPARE(copied,
                 audio::captureChecksum(
                     std::as_bytes(std::span(left).subspan(offset, count)),
                     std::as_bytes(std::span(right).subspan(offset, count))));
        offset += count;
        std::span<const std::byte> l, r;
        while (queue.peek(l, r)) {
          QCOMPARE(l.size(), size_t(packet) * 2);
          QCOMPARE(r.size(), size_t(packet) * 4);
          QCOMPARE(std::memcmp(l.data(), left.data() + consumed, l.size()), 0);
          QCOMPARE(std::memcmp(r.data(), right.data() + consumed, r.size()), 0);
          consumed += packet;
          queue.pop();
        }
      }
      QCOMPARE(consumed, size_t(1200 / packet * packet));
      QCOMPARE(queue.queuedFrames(), uint64_t(0));
    }
    audio::CaptureQueue small(64, 128, 128, 2);
    std::array<int16_t, 129> samples{};
    QVERIFY(!small.append(samples.data(), samples.data(), 129));
    QCOMPARE(small.capturedFrames(), uint64_t(0));
    QVERIFY(small.append(samples.data(), samples.data(), 128));
    QVERIFY(!small.append(samples.data(), samples.data(), 1));
    QVERIFY_THROWS_EXCEPTION(
        std::invalid_argument,
        audio::CaptureQueue(1, 8, 8, std::numeric_limits<size_t>::max()));
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                             audio::CaptureQueue::capacityFor(353, 8192));
    for (const int invalid : {0, -1, (1 << 30) + 1})
      QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                               audio::CaptureQueue::capacityFor(64, invalid));
  }
  void captureQueueConcurrentReblocking() {
    constexpr int packet = 7, callbacks = 10000, callbackSamples = 64;
    audio::CaptureQueue queue(packet, packet * sizeof(int32_t),
                              packet * sizeof(int32_t), 128);
    std::atomic<bool> done{false};
    std::thread producer([&] {
      std::array<int32_t, callbackSamples> left{}, right{};
      for (int callback = 0; callback < callbacks; ++callback) {
        for (int i = 0; i < callbackSamples; ++i) {
          left[i] = callback * callbackSamples + i;
          right[i] = -left[i];
        }
        while (!queue.append(left.data(), right.data(), callbackSamples))
          std::this_thread::yield();
      }
      done.store(true);
    });
    size_t consumed = 0;
    bool exact = true;
    for (;;) {
      std::span<const std::byte> left, right;
      if (!queue.peek(left, right)) {
        if (done.load() && !queue.queuedFrames())
          break;
        std::this_thread::yield();
        continue;
      }
      for (int i = 0; i < packet; ++i, ++consumed) {
        int32_t l, r;
        std::memcpy(&l, left.data() + i * 4, 4);
        std::memcpy(&r, right.data() + i * 4, 4);
        exact &= l == int32_t(consumed) && r == -int32_t(consumed);
      }
      queue.pop();
    }
    producer.join();
    QVERIFY(exact);
    QCOMPARE(consumed, size_t(callbacks * callbackSamples / packet * packet));
  }
  void defaultsAndBounds() {
    app::Settings settings;
    QVERIFY(settings.validate().isEmpty());
    for (const auto &field : app::timingMsFields) {
      auto invalid = settings;
      invalid.timing.*(field.member) = field.maximum + 1;
      QVERIFY(!invalid.validate().isEmpty());
      invalid = settings;
      invalid.timing.*(field.member) = std::numeric_limits<double>::quiet_NaN();
      QVERIFY(!invalid.validate().isEmpty());
    }
    settings.timing.ptpSyncMs = 120;
    QVERIFY(!settings.validate().isEmpty());
    settings = {};
    settings.timing.prebufferSamples = settings.timing.backlogSamples;
    QVERIFY(!settings.validate().isEmpty());
    settings = {};
    settings.left = settings.right;
    QVERIFY(settings.validate().isEmpty());
    settings.left = -1;
    QVERIFY(!settings.validate().isEmpty());
    settings.left = 0;
    settings.right = -1;
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
    settings.right = settings.left;
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
