#include "MainWindow.h"
#include "app/Message.h"
#include <QCloseEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QTimer>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#ifdef Q_OS_MACOS
#include <QApplication>
#include <QMenuBar>
#endif

namespace ui {
MainWindow::MainWindow(const airplay::DiscoveryApi &api,
                       const audio::InputCaptureApi &captureApi)
    : controller_(nullptr, captureApi) {
#ifdef Q_OS_MACOS
  const QString backend = "Core Audio";
#elif defined(Q_OS_LINUX)
  const QString backend = "PipeWire";
#else
  const QString backend = "ASIO";
#endif
  setWindowTitle("AirPlayQt · " + backend + " → AirPlay");
  resize(950, 980);
  auto *central = new QWidget;
  setCentralWidget(central);
  auto *layout = new QVBoxLayout(central);
  streaming_ = new StreamingPanel(controller_.session(), central, api);
  auto *input = new QGroupBox;
  streaming_->bindText(input, "title",
                       backend +
#ifdef Q_OS_MACOS
                       " " + i18n::text(i18n::Id::AudioCaptureDevice)
#else
                       i18n::text(i18n::Id::InputDeviceGroupSuffix)
#endif
  );
  auto label = [this](const i18n::Message &message) {
    auto *widget = new QLabel;
    streaming_->bindText(widget, "text", message);
    return widget;
  };
  auto *form = new QFormLayout(input);
  driver_ = new QComboBox;
  driver_->setObjectName("captureDevice");
  left_ = new QComboBox;
  left_->setObjectName("leftInputChannel");
  right_ = new QComboBox;
  right_->setObjectName("rightInputChannel");
  refresh_ = new QPushButton;
  refresh_->setObjectName("refreshAudioDevices");
  refresh_->setEnabled(false);
  streaming_->bindText(refresh_, "text", i18n::text(i18n::Id::Refresh));
  panel_ = new QPushButton;
#ifdef Q_OS_LINUX
  panel_->hide();
#endif
  streaming_->bindText(panel_, "text",
#ifdef Q_OS_MACOS
                       i18n::text(i18n::Id::AudioMIDISetup)
#else
                       i18n::text(i18n::Id::DriverControlPanel)
#endif
  );
  auto *row = new QHBoxLayout;
  row->addWidget(driver_, 1);
  row->addWidget(refresh_);
  row->addWidget(panel_);
  form->addRow(label(i18n::text(
#ifdef Q_OS_MACOS
      i18n::Id::AudioCaptureDevice
#else
      i18n::Id::InputDeviceLabel
#endif
      )), row);
  form->addRow(label(i18n::text(i18n::Id::LeftInputChannel)), left_);
  form->addRow(label(i18n::text(i18n::Id::RightInputChannel)), right_);
  streaming_->addInputWidget(input);
  layout->addWidget(streaming_, 1);
  connect(streaming_, &StreamingPanel::logVisibilityChanged, this,
          [this](bool visible, int removedWidth) {
    if (!visible)
      expandedWidth_ = width();
    this->layout()->activate();
    resize(visible ? expandedWidth_ : width() - removedWidth, height());
  });
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
          this, &MainWindow::saveWindowLayout);
#ifdef Q_OS_MACOS
  qApp->setQuitOnLastWindowClosed(false);
  auto *windowMenu = menuBar()->addMenu(QString());
  streaming_->bindText(windowMenu, "title", i18n::text(i18n::Id::WindowMenu));
  auto *closeWindow = windowMenu->addAction(QString());
  closeWindow->setObjectName("closeWindow");
  streaming_->bindText(closeWindow, "text", i18n::text(i18n::Id::CloseWindow));
  // Qt maps CTRL to the Command key on macOS.
  closeWindow->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
  connect(closeWindow, &QAction::triggered, this, &QWidget::close);
  auto *quit = windowMenu->addAction(QString());
  quit->setObjectName("quitApplication");
  quit->setMenuRole(QAction::QuitRole);
  quit->setShortcut(QKeySequence::Quit);
  quit->setShortcutContext(Qt::ApplicationShortcut);
  streaming_->bindText(quit, "text", i18n::text(i18n::Id::QuitAirPlayQt));
  connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [this](Qt::ApplicationState state) {
    if (state == Qt::ApplicationActive && !closing_ &&
        (!isVisible() || isMinimized())) {
      showNormal();
      raise();
      activateWindow();
    }
  });
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
    closing_ = true;
    refresh_->setEnabled(false);
    streaming_->cancelDiscovery();
    controller_.stop();
    controller_.session().shutdown();
  });
#endif
  connect(streaming_, &StreamingPanel::languageChanged, this, [this] {
    if (!loading_)
      controller_.saveLanguage(streaming_->language());
    for (auto *combo : {driver_, left_, right_})
      for (int i = 0; i < combo->count(); ++i)
        combo->setItemText(
            i, i18n::Message(combo->itemData(i, Qt::UserRole + 1).toJsonArray())
                   .render(streaming_->language()));
  });
  connect(&controller_, &app::Controller::channels, this,
          &MainWindow::setChannels);
  connect(&controller_, &app::Controller::devicesChanged, this,
          &MainWindow::setDevices);
  connect(refresh_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
  connect(&controller_, &app::Controller::devicesRefreshed, this,
          [this](const QList<audio::DriverInfo> &devices) {
    const auto selected = driver_->currentData().toString();
    fillDevices(devices, selected);
    if (driver_->currentIndex() < 0)
      saved_.driverId.clear(); // Do not restore a discarded selection on hotplug.
  });
  connect(&controller_, &app::Controller::error, streaming_,
          &StreamingPanel::showError);
  connect(&controller_, &app::Controller::busyChanged, this,
          &MainWindow::setBusy);
  connect(&controller_, &app::Controller::stopped, this, [this] {
    if (closing_)
      close();
  });
  connect(streaming_, &StreamingPanel::discoveryIdle, this, [this] {
    if (closing_)
      QTimer::singleShot(0, this, [this] { close(); });
  });
  connect(driver_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (index >= 0 && !loading_)
      controller_.selectDriver(driver_->currentData().toString(),
                               reinterpret_cast<void *>(winId()));
  });
  connect(panel_, &QPushButton::clicked, &controller_,
          &app::Controller::controlPanel);
  connect(streaming_, &StreamingPanel::stopRequested, &controller_,
          &app::Controller::stop);
  connect(streaming_, &StreamingPanel::startRequested, this, [this] {
    try {
      const auto endpoints = streaming_->endpoints();
      app::Settings settings;
      settings.language = streaming_->language();
      settings.driverId = driver_->currentData().toString();
      settings.left =
          left_->currentIndex() < 0 ? -1 : left_->currentData().toInt();
      settings.right =
          right_->currentIndex() < 0 ? -1 : right_->currentData().toInt();
      settings.timing = streaming_->timing();
      settings.networkBinding = streaming_->networkBinding();
      const auto selection =
          streaming_->discoveryMode()
              ? std::optional(streaming_->receiverSelection())
              : std::nullopt;
      controller_.start(settings, endpoints, true, selection);
      streaming_->setRememberedReceivers(controller_.rememberedReceivers());
    } catch (const std::exception &e) {
      streaming_->showError(i18n::fromException(e));
    }
  });
  QTimer::singleShot(0, this, [this] {
    if (closing_)
      return;
    try {
      saved_ = controller_.initialize();
      streaming_->setLanguage(saved_.language);
      streaming_->setNetworkBinding(saved_.networkBinding);
      fillDevices(controller_.drivers(), saved_.driverId);
      streaming_->setTiming(saved_.timing);
      streaming_->setRememberedReceivers(saved_.receiverSelection);
      const auto &savedLayout = saved_.windowLayout;
      expandedWidth_ = savedLayout.expandedWidth;
      streaming_->setWindowLayout(savedLayout);
      if (savedLayout.width > 0) {
        resize(savedLayout.width, savedLayout.height);
        this->layout()->activate();
        streaming_->setWindowLayout(savedLayout);
      }
      const auto index =
          saved_.driverId.isEmpty() ? -1 : driver_->findData(saved_.driverId);
      driver_->setCurrentIndex(index);
      if (index >= 0)
        controller_.selectDriver(saved_.driverId,
                                 reinterpret_cast<void *>(winId()));
      else if (!saved_.driverId.isEmpty()) {
#ifdef Q_OS_LINUX
        // BlueZ/PipeWire enumerate asynchronously. An empty initial list is
        // not evidence that the saved device disappeared.
        streaming_->appendLog(
            i18n::text(i18n::Id::WaitingForSavedInputDevice));
#else
        streaming_->appendLog(
            i18n::text(i18n::Id::SavedDriverIsUnavailableSelectAnotherDriver));
#endif
      } else
        streaming_->appendLog(
            i18n::text(i18n::Id::SelectAnInputDeviceAndTwoInput) +
            app::Settings::path());
    } catch (const std::exception &e) {
      streaming_->showError(i18n::fromException(e));
    }
    loading_ = false;
    setBusy(controller_.busy());
    streaming_->beginDiscovery();
  });
}
void MainWindow::fillDevices(const QList<audio::DriverInfo> &devices,
                             const QString &selected) {
  QSignalBlocker blocker(driver_);
  driver_->clear();
  for (const auto &device : devices) {
    const auto label = device.displayName();
    driver_->addItem(label.render(streaming_->language()), device.id);
    driver_->setItemData(driver_->count() - 1, QJsonArray(label), Qt::UserRole + 1);
  }
  driver_->setCurrentIndex(driver_->findData(selected));
}
void MainWindow::refreshDevices() {
  if (loading_ || closing_ || refreshing_ || controller_.busy())
    return;
  {
    QScopedValueRollback<bool> refreshing(refreshing_, true);
    refresh_->setEnabled(false);
    controller_.refreshDevices(driver_->currentData().toString(),
                               reinterpret_cast<void *>(winId()));
  }
  setBusy(controller_.busy());
}
void MainWindow::setDevices(const QList<audio::DriverInfo> &devices) {
  if (loading_ || closing_) return;
  const auto previous = driver_->currentData().toString();
  const auto selected = previous.isEmpty() ? saved_.driverId : previous;
  const auto oldLabel = driver_->currentData(Qt::UserRole + 1);
  {
    QSignalBlocker blocker(driver_);
    fillDevices(devices, selected);
    // An active selection survives disappearance; it must never fall back to
    // another input. Reappearance is handled by the capture backend itself.
    if (!previous.isEmpty() && driver_->findData(previous) < 0) {
      const auto label = i18n::Message(oldLabel.toJsonArray());
      driver_->addItem(label.render(streaming_->language()), previous);
      driver_->setItemData(driver_->count() - 1, oldLabel, Qt::UserRole + 1);
    }
    driver_->setCurrentIndex(driver_->findData(selected));
  }
  if (previous.isEmpty() && driver_->currentIndex() >= 0 && !controller_.busy()) {
    // The saved device can arrive after startup's asynchronous enumeration.
    // Restore its saved channel mapping just as during initial loading.
    QScopedValueRollback<bool> loading(loading_, true);
    controller_.selectDriver(selected, reinterpret_cast<void *>(winId()));
  }
}
void MainWindow::setChannels(const QList<audio::ChannelInfo> &channels) {
  const int previousLeft =
      left_->currentIndex() < 0 ? -1 : left_->currentData().toInt();
  const int previousRight =
      right_->currentIndex() < 0 ? -1 : right_->currentData().toInt();
  left_->clear();
  right_->clear();
  for (const auto &channel : channels) {
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    const auto label =
        i18n::Message("%1 · %2").arg(channel.index + 1).arg(channel.name);
#else
    const auto label = i18n::text(i18n::Id::AsioChannelLabel)
                           .arg(channel.index + 1)
                           .arg(channel.name)
                           .arg(channel.type);
#endif
    for (auto *combo : {left_, right_}) {
      combo->addItem(label.render(streaming_->language()), channel.index);
      combo->setItemData(combo->count() - 1, QJsonArray(label),
                         Qt::UserRole + 1);
    }
  }
  const int l = loading_ ? saved_.left : (refreshing_ || previousLeft >= 0 ? previousLeft : 0),
            r = loading_ ? saved_.right
                         : (refreshing_ || previousRight >= 0 ? previousRight : 1);
  left_->setCurrentIndex(left_->findData(l));
  right_->setCurrentIndex(right_->findData(r));
  if (!channels.isEmpty() &&
      (left_->currentIndex() < 0 || right_->currentIndex() < 0))
    streaming_->appendLog(
        i18n::text(i18n::Id::SelectedInputChannelsAreNoLongerValid));
}

void MainWindow::setBusy(bool busy) {
  for (QWidget *widget :
       std::array<QWidget *, 4>{driver_, left_, right_, panel_})
    widget->setEnabled(!busy);
  refresh_->setEnabled(!busy && !loading_ && !closing_ && !refreshing_);
  streaming_->setBusy(busy);
}
void MainWindow::closeEvent(QCloseEvent *event) {
  saveWindowLayout();
#ifdef Q_OS_MACOS
  // Closing only hides the widget; the controller and its session stay alive.
  // Explicit application quit is cleaned up by aboutToQuit above.
  event->accept();
#else
  if (!closing_) {
    closing_ = true;
    refresh_->setEnabled(false);
    streaming_->cancelDiscovery();
    controller_.stop();
  }
  if (controller_.busy() || streaming_->discoveryBusy())
    event->ignore();
  else
    event->accept();
#endif
}
void MainWindow::saveWindowLayout() {
  if (loading_)
    return;
  const auto normal = normalGeometry().size();
  controller_.saveWindowLayout(streaming_->windowLayout(
      normal.isValid() ? normal : size(), expandedWidth_));
}
} // namespace ui
