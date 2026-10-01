#include "MainWindow.h"
#include "app/Message.h"
#include <QCloseEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>

namespace ui {
MainWindow::MainWindow(const airplay::DiscoveryApi &api) {
#ifdef Q_OS_MACOS
  const QString backend = "Core Audio";
#else
  const QString backend = "ASIO";
#endif
  setWindowTitle("AirPlayQt · " + backend + " → AirPlay");
  resize(900, 980);
  auto *central = new QWidget;
  setCentralWidget(central);
  auto *layout = new QVBoxLayout(central);
  streaming_ = new StreamingPanel(controller_.session(), central, api);
  auto *input = new QGroupBox;
  streaming_->bindText(input, "title",
                       backend + i18n::text(i18n::Id::InputDeviceGroupSuffix));
  auto label = [this](const i18n::Message &message) {
    auto *widget = new QLabel;
    streaming_->bindText(widget, "text", message);
    return widget;
  };
  auto *form = new QFormLayout(input);
  driver_ = new QComboBox;
  left_ = new QComboBox;
  right_ = new QComboBox;
  panel_ = new QPushButton;
  streaming_->bindText(panel_, "text",
#ifdef Q_OS_MACOS
                       i18n::text(i18n::Id::AudioMIDISetup)
#else
                       i18n::text(i18n::Id::DriverControlPanel)
#endif
  );
  auto *row = new QHBoxLayout;
  row->addWidget(driver_, 1);
  row->addWidget(panel_);
  form->addRow(label(i18n::text(i18n::Id::InputDeviceLabel)), row);
  form->addRow(label(i18n::text(i18n::Id::LeftInputChannel)), left_);
  form->addRow(label(i18n::text(i18n::Id::RightInputChannel)), right_);
  streaming_->addInputWidget(input);
  layout->addWidget(streaming_, 1);
  connect(streaming_, &StreamingPanel::languageChanged, this, [this] {
    if (!loading_)
      controller_.saveLanguage(streaming_->language());
    for (auto *combo : {left_, right_})
      for (int i = 0; i < combo->count(); ++i)
        combo->setItemText(
            i, i18n::Message(combo->itemData(i, Qt::UserRole + 1).toJsonArray())
                   .render(streaming_->language()));
  });
  connect(&controller_, &app::Controller::channels, this,
          &MainWindow::setChannels);
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
      for (const auto &driver : controller_.drivers())
        driver_->addItem(driver.name, driver.id);
      streaming_->setTiming(saved_.timing);
      streaming_->setRememberedReceivers(saved_.receiverSelection);
      const auto index =
          saved_.driverId.isEmpty() ? -1 : driver_->findData(saved_.driverId);
      driver_->setCurrentIndex(index);
      if (index >= 0)
        controller_.selectDriver(saved_.driverId,
                                 reinterpret_cast<void *>(winId()));
      else if (!saved_.driverId.isEmpty())
        streaming_->appendLog(
            i18n::text(i18n::Id::SavedDriverIsUnavailableSelectAnotherDriver));
      else
        streaming_->appendLog(
            i18n::text(i18n::Id::SelectAnInputDeviceAndTwoInput) +
            app::Settings::path());
    } catch (const std::exception &e) {
      streaming_->showError(i18n::fromException(e));
    }
    loading_ = false;
    streaming_->beginDiscovery();
  });
}
void MainWindow::setChannels(const QList<audio::ChannelInfo> &channels) {
  const int previousLeft =
      left_->currentIndex() < 0 ? -1 : left_->currentData().toInt();
  const int previousRight =
      right_->currentIndex() < 0 ? -1 : right_->currentData().toInt();
  left_->clear();
  right_->clear();
  for (const auto &channel : channels) {
#ifdef Q_OS_MACOS
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
  const int l = loading_ ? saved_.left : (previousLeft >= 0 ? previousLeft : 0),
            r = loading_ ? saved_.right
                         : (previousRight >= 0 ? previousRight : 1);
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
  streaming_->setBusy(busy);
}
void MainWindow::closeEvent(QCloseEvent *event) {
  if (!closing_) {
    closing_ = true;
    streaming_->cancelDiscovery();
    controller_.stop();
  }
  if (controller_.busy() || streaming_->discoveryBusy())
    event->ignore();
  else
    event->accept();
}
} // namespace ui
