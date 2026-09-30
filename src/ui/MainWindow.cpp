#include "MainWindow.h"
#include <QCloseEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>

namespace ui {
MainWindow::MainWindow(airplay::DiscoveryApi api) {
  setWindowTitle("AirPlayQt · ASIO → HomePod");
  resize(900, 980);
  auto *central = new QWidget;
  setCentralWidget(central);
  auto *layout = new QVBoxLayout(central);
  auto *input = new QGroupBox("ASIO 输入设备");
  auto *form = new QFormLayout(input);
  driver_ = new QComboBox;
  left_ = new QComboBox;
  right_ = new QComboBox;
  panel_ = new QPushButton("驱动控制面板");
  auto *row = new QHBoxLayout;
  row->addWidget(driver_, 1);
  row->addWidget(panel_);
  form->addRow("ASIO 驱动", row);
  form->addRow("左声道输入", left_);
  form->addRow("右声道输入", right_);
  layout->addWidget(input);
  streaming_ =
      new StreamingPanel(controller_.session(), central, std::move(api));
  layout->addWidget(streaming_, 1);
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
  connect(streaming_, &StreamingPanel::startRequested, this,
          [this](app::Timing timing,
                 const QList<airplay::ReceiverEndpoint> &endpoints) {
            app::Settings settings;
            settings.driverId = driver_->currentData().toString();
            settings.left =
                left_->currentIndex() < 0 ? -1 : left_->currentData().toInt();
            settings.right =
                right_->currentIndex() < 0 ? -1 : right_->currentData().toInt();
            settings.timing = timing;
            controller_.start(settings, endpoints);
          });
  QTimer::singleShot(0, this, [this] {
    if (closing_)
      return;
    try {
      saved_ = controller_.initialize();
      for (const auto &driver : controller_.drivers())
        driver_->addItem(driver.name, driver.id);
      streaming_->setTiming(saved_.timing);
      const auto index =
          saved_.driverId.isEmpty() ? -1 : driver_->findData(saved_.driverId);
      driver_->setCurrentIndex(index);
      if (index >= 0)
        controller_.selectDriver(saved_.driverId,
                                 reinterpret_cast<void *>(winId()));
      else if (!saved_.driverId.isEmpty())
        streaming_->appendLog("已保存的驱动不可用，请重新选择。");
      else
        streaming_->appendLog("请选择 ASIO 驱动及两个输入通道。配置：" +
                              app::Settings::path());
    } catch (const std::exception &e) {
      streaming_->showError(QString::fromUtf8(e.what()));
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
    const auto label = QString("%1 · %2 [ASIO 类型 %3]")
                           .arg(channel.index + 1)
                           .arg(channel.name)
                           .arg(channel.type);
    left_->addItem(label, channel.index);
    right_->addItem(label, channel.index);
  }
  const int l = loading_ ? saved_.left : (previousLeft >= 0 ? previousLeft : 0),
            r = loading_ ? saved_.right
                         : (previousRight >= 0 ? previousRight : 1);
  left_->setCurrentIndex(left_->findData(l));
  right_->setCurrentIndex(right_->findData(r));
  if (!channels.isEmpty() &&
      (left_->currentIndex() < 0 || right_->currentIndex() < 0))
    streaming_->appendLog("已选择的输入通道失效，请重新选择。");
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
