#include "StreamingPanel.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace ui {
StreamingPanel::StreamingPanel(app::SessionController &session, QWidget *parent,
                               const airplay::DiscoveryApi &discoveryApi)
    : QWidget(parent), discovery_(this, std::move(discoveryApi)),
      session_(session) {
  auto *layout = new QVBoxLayout(this);
  title_ = new QLabel("音频输入 → AirPlay 接收端");
  title_->setObjectName("streamingTitle");
  title_->setStyleSheet("font-size: 22px; font-weight: 600; padding: 8px 0;");
  layout->addWidget(title_);
  layout->addWidget(new QLabel("44.1 kHz / 16-bit / 双声道 · 不重采样"));
  receiverModes_ = new QTabWidget;
  receiverModes_->setObjectName("receiverModes");
  auto *discovered = new QWidget;
  auto *discoveredLayout = new QVBoxLayout(discovered);
  receivers_ = new QListWidget;
  receivers_->setObjectName("receivers");
  receivers_->setMinimumHeight(85);
  receivers_->setMaximumHeight(130);
  discoveredLayout->addWidget(receivers_);
  auto *scanRow = new QHBoxLayout;
  discoveryStatus_ = new QLabel("等待扫描");
  discoveryStatus_->setWordWrap(true);
  refresh_ = new QPushButton("刷新");
  refresh_->setObjectName("refreshReceivers");
  scanRow->addWidget(discoveryStatus_, 1);
  scanRow->addWidget(refresh_);
  discoveredLayout->addLayout(scanRow);
  receiverModes_->addTab(discovered, "发现模式");
  auto *manual = new QWidget;
  auto *manualForm = new QFormLayout(manual);
  manualFirst_ = new QLineEdit;
  manualFirst_->setObjectName("manualFirst");
  manualSecond_ = new QLineEdit;
  manualSecond_->setObjectName("manualSecond");
  manualFirst_->setPlaceholderText("IPv4[:端口]，省略端口使用 7000");
  manualSecond_->setPlaceholderText("可留空；双台必须属于同一个已有立体声组");
  manualForm->addRow("接收端 1（必填）", manualFirst_);
  manualForm->addRow("接收端 2（可选）", manualSecond_);
  receiverModes_->addTab(manual, "手动模式");
  layout->addWidget(receiverModes_);
  targets_ = new QLabel;
  targets_->setWordWrap(true);
  targets_->setObjectName("receiverSummary");
  layout->addWidget(targets_);
  auto *tabs = new QTabWidget;
  for (int page = 0; page < 2; ++page) {
    auto *widget = new QWidget;
    auto *fields = new QFormLayout(widget);
    for (size_t i = page ? 6 : 0; i < (page ? app::timingFields.size() : 6);
         ++i) {
      const auto &f = app::timingFields[i];
      auto *spin = new QDoubleSpinBox;
      timings_[i] = spin;
      spin->setDecimals(3);
      spin->setRange(f.minimum * 1000, f.maximum * 1000);
      spin->setSuffix(" ms");
      spin->setSingleStep(f.powerOfTwo ? 15.625 : 10);
      spin->setValue(app::Timing{}.*(f.member) * 1000);
      connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
        if (std::all_of(timings_.begin(), timings_.end(),
                        [](auto *p) { return p != nullptr; })) {
          const auto value = timing();
          if (value.validate().isEmpty())
            emit timingChanged();
        }
      });
      if (f.powerOfTwo)
        spin->setToolTip("周期须为 1000 × 2ⁿ ms（n 为整数），例如 "
                         "15.625、31.25、62.5、125、250、500、1000；"
                         "具体取值受当前范围限制。");
      fields->addRow(QString::fromUtf8(f.label), spin);
    }
    tabs->addTab(widget, page ? "高级时间设置" : "常用时间设置");
  }
  layout->addWidget(tabs);
  defaults_ = new QPushButton("恢复时间默认值");
  layout->addWidget(defaults_, 0, Qt::AlignRight);
  auto *controls = new QHBoxLayout;
  start_ = new QPushButton("开始");
  start_->setObjectName("start");
  stop_ = new QPushButton("停止");
  stop_->setObjectName("stop");
  stop_->setEnabled(false);
  controls->addWidget(start_);
  controls->addWidget(stop_);
  layout->addLayout(controls);
  group_ = new QLabel("尚未读取接收端身份");
  group_->setWordWrap(true);
  layout->addWidget(group_);
  auto *volumeRow = new QHBoxLayout;
  volume_ = new QDoubleSpinBox;
  volume_->setRange(-144, 0);
  volume_->setDecimals(2);
  volume_->setSuffix(" dB");
  applyVolume_ = new QPushButton("设置 HomePod 音量");
  mute_ = new QCheckBox("静音");
  volumeRow->addWidget(new QLabel("接收端音量"));
  volumeRow->addWidget(volume_);
  volumeRow->addWidget(applyVolume_);
  volumeRow->addWidget(mute_);
  layout->addLayout(volumeRow);
  volume_->setEnabled(false);
  applyVolume_->setEnabled(false);
  mute_->setEnabled(false);
  auto *meters = new QHBoxLayout;
  leftLevel_ = new QProgressBar;
  rightLevel_ = new QProgressBar;
  for (auto *meter : {leftLevel_, rightLevel_}) {
    meter->setRange(0, 1000);
    meter->setValue(0);
    meter->setFormat("静音");
  }
  meters->addWidget(new QLabel("L"));
  meters->addWidget(leftLevel_);
  meters->addWidget(new QLabel("R"));
  meters->addWidget(rightLevel_);
  layout->addLayout(meters);
  state_ = new QLabel("就绪");
  stats_ = new QLabel("积压 0 ms · 每台包数 0 · 重传 0 · 过期 0");
  layout->addWidget(state_);
  layout->addWidget(stats_);
  log_ = new QPlainTextEdit;
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(1000);
  layout->addWidget(log_, 1);
  connect(&discovery_, &airplay::ReceiverDiscovery::cleared, this, [this] {
    QSignalBlocker block(receivers_);
    receivers_->clear();
    restoreReceiversAllowed_ = true;
    updateTargets();
  });
  connect(&discovery_, &airplay::ReceiverDiscovery::status, discoveryStatus_,
          &QLabel::setText);
  connect(&discovery_, &airplay::ReceiverDiscovery::found, this,
          [this](const QString &name, const QString &endpoint) {
            const auto incoming = airplay::parseReceiverEndpoint(endpoint);
            const auto address = incoming.host.toIPv4Address();
            int row = 0;
            for (; row < receivers_->count(); ++row) {
              const auto existing = airplay::parseReceiverEndpoint(
                  receivers_->item(row)->data(Qt::UserRole).toString());
              if (incoming == existing)
                return;
              const auto otherAddress = existing.host.toIPv4Address();
              if (address < otherAddress ||
                  (address == otherAddress && incoming.port < existing.port))
                break;
            }
            QSignalBlocker block(receivers_);
            auto *item = new QListWidgetItem(name + " · " + endpoint);
            item->setData(Qt::UserRole, endpoint);
            item->setData(Qt::UserRole + 1, name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
            receivers_->insertItem(row, item);
            restoreReceivers();
          });
  connect(&discovery_, &airplay::ReceiverDiscovery::idle, this, [this] {
    if (restoreReceiversAllowed_ && !rememberedReceivers_.isEmpty() &&
        !closing_)
      appendLog("上次勾选的接收端尚未全部匹配（名称、地址及端口），请刷新或手动"
                "选择。");
    if (closing_)
      emit discoveryIdle();
  });
  connect(receivers_, &QListWidget::itemChanged, this,
          [this](QListWidgetItem *changed) {
            restoreReceiversAllowed_ = false;
            int checked = 0;
            // Qt 6.11 checkState() registers an inline enum metatype in
            // this module. Read the stored integer to keep unload safe.
            for (int i = 0; i < receivers_->count(); ++i)
              checked +=
                  receivers_->item(i)->data(Qt::CheckStateRole).toInt() ==
                  Qt::Checked;
            if (checked > 2) {
              QSignalBlocker blocker(receivers_);
              changed->setCheckState(Qt::Unchecked);
              discoveryStatus_->setText(
                  "最多选择两台接收端；双台必须属于同一个已有立体声组。");
            }
            updateTargets();
          });
  connect(refresh_, &QPushButton::clicked, this, &StreamingPanel::scan);
  connect(receiverModes_, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == 0)
      scan();
    else
      discovery_.cancel();
    updateTargets();
  });
  for (auto *field : {manualFirst_, manualSecond_})
    connect(field, &QLineEdit::textChanged, this,
            &StreamingPanel::updateTargets);
  updateTargets();
  connect(&session_, &app::SessionController::status, state_, &QLabel::setText);
  connect(&session_, &app::SessionController::log, this,
          &StreamingPanel::appendLog);
  connect(&session_, &app::SessionController::group, group_, &QLabel::setText);
  connect(&session_, &app::SessionController::error, this,
          &StreamingPanel::showError);
  connect(&session_, &app::SessionController::busyChanged, this,
          &StreamingPanel::setBusy);
  connect(&session_, &app::SessionController::streamingChanged, this,
          [this](bool active) {
            streaming_ = active;
            volume_->setEnabled(active && !mute_->isChecked());
            applyVolume_->setEnabled(active && !volumePending_ &&
                                     !mute_->isChecked());
            mute_->setEnabled(active && !volumePending_);
          });
  connect(&session_, &app::SessionController::volumeApplied, this,
          [this](double db) {
            volumePending_ = false;
            if (!mute_->isChecked()) {
              QSignalBlocker blocker(volume_);
              volume_->setValue(db);
              restoreVolume_ = db;
            }
            applyVolume_->setEnabled(streaming_ && !mute_->isChecked());
            mute_->setEnabled(streaming_);
          });
  connect(
      &session_, &app::SessionController::telemetry, this,
      [this](double l, double r, double backlog, quint64 packets,
             quint64 retransmitted, quint64 expired) {
        for (auto [meter, value] :
             {std::pair{leftLevel_, l}, std::pair{rightLevel_, r}}) {
          meter->setValue(int(value * 1000));
          meter->setFormat(
              value > 0
                  ? QString::number(20 * std::log10(value), 'f', 1) + " dBFS"
                  : "静音");
        }
        stats_->setText(QString("积压 %1 ms · 每台包数 %2 · 重传 %3 · 过期 %4")
                            .arg(backlog * 1000, 0, 'f', 1)
                            .arg(packets)
                            .arg(retransmitted)
                            .arg(expired));
      });
  connect(defaults_, &QPushButton::clicked, this, [this] {
    app::Timing defaults;
    for (size_t i = 0; i < timings_.size(); ++i)
      timings_[i]->setValue(defaults.*(app::timingFields[i].member) * 1000);
  });
  connect(start_, &QPushButton::clicked, this, [this] {
    QSignalBlocker block(mute_);
    mute_->setChecked(false);
    volumePending_ = false;
    try {
      endpoints(); // Validate before canceling discovery and notifying
                   // consumers.
      discovery_.cancel();
      emit startRequested();
    } catch (const std::exception &e) {
      appendLog(QString::fromUtf8(e.what()));
      updateTargets();
    }
  });
  connect(stop_, &QPushButton::clicked, this, &StreamingPanel::stopRequested);
  connect(applyVolume_, &QPushButton::clicked, this, [this] {
    volumePending_ = true;
    applyVolume_->setEnabled(false);
    mute_->setEnabled(false);
    session_.volume(volume_->value());
  });
  connect(mute_, &QCheckBox::toggled, this, [this](bool muted) {
    volumePending_ = true;
    applyVolume_->setEnabled(false);
    mute_->setEnabled(false);
    volume_->setEnabled(!muted);
    session_.volume(muted ? -144 : restoreVolume_);
  });

  setTiming(app::Timing{});
  for (const auto &text : session_.recentLog())
    appendLog(text);
  state_->setText(session_.currentStatus());
  group_->setText(session_.currentGroup());
  volume_->setValue(session_.currentVolume());
  restoreVolume_ = session_.restoreVolume();
  {
    QSignalBlocker blocker(mute_);
    mute_->setChecked(session_.currentVolume() <= -144);
  }
  setBusy(session_.busy());
  streaming_ = session_.streaming();
  volume_->setEnabled(streaming_ && !mute_->isChecked());
  applyVolume_->setEnabled(streaming_ && !mute_->isChecked());
  mute_->setEnabled(streaming_);
}
QList<airplay::ReceiverEndpoint> StreamingPanel::endpoints() const {
  QList<airplay::ReceiverEndpoint> result;
  if (receiverModes_->currentIndex() == 0) {
    for (int i = 0; i < receivers_->count(); ++i)
      if (const auto *item = receivers_->item(i);
          item->data(Qt::CheckStateRole).toInt() == Qt::Checked)
        result.append(airplay::parseReceiverEndpoint(
            item->data(Qt::UserRole).toString()));
  } else {
    result.append(airplay::parseReceiverEndpoint(manualFirst_->text()));
    if (!manualSecond_->text().trimmed().isEmpty())
      result.append(airplay::parseReceiverEndpoint(manualSecond_->text()));
  }
  airplay::validateEndpoints(result);
  return result;
}
bool StreamingPanel::discoveryMode() const {
  return receiverModes_->currentIndex() == 0;
}
QList<app::ReceiverSelection> StreamingPanel::receiverSelection() const {
  QList<app::ReceiverSelection> result;
  for (int i = 0; i < receivers_->count(); ++i) {
    const auto *item = receivers_->item(i);
    if (item->data(Qt::CheckStateRole).toInt() == Qt::Checked)
      result.append({item->data(Qt::UserRole + 1).toString(),
                     item->data(Qt::UserRole).toString()});
  }
  return result;
}
void StreamingPanel::setRememberedReceivers(
    const QList<app::ReceiverSelection> &selection) {
  rememberedReceivers_ = selection;
  restoreReceiversAllowed_ = true;
  restoreReceivers();
}
void StreamingPanel::restoreReceivers() {
  if (!restoreReceiversAllowed_ || rememberedReceivers_.isEmpty() ||
      !discoveryMode())
    return;
  QList<QListWidgetItem *> matches;
  for (const auto &saved : rememberedReceivers_) {
    QListWidgetItem *match = nullptr;
    for (int i = 0; i < receivers_->count(); ++i) {
      auto *item = receivers_->item(i);
      if (item->data(Qt::UserRole).toString() == saved.endpoint &&
          item->data(Qt::UserRole + 1).toString() == saved.name)
        match = item;
    }
    if (!match)
      return; // Restore the complete set, never silently use one peer.
    matches.append(match);
  }
  QSignalBlocker block(receivers_);
  for (int i = 0; i < receivers_->count(); ++i)
    receivers_->item(i)->setCheckState(Qt::Unchecked);
  for (auto *item : matches)
    item->setCheckState(Qt::Checked);
  restoreReceiversAllowed_ = false;
  updateTargets();
}
void StreamingPanel::updateTargets() {
  try {
    const auto selected = endpoints();
    QStringList addresses;
    for (const auto &endpoint : selected)
      addresses.append(endpoint.text());
    title_->setText(selected.size() == 1 ? "音频输入 → AirPlay 单台接收端"
                                         : "音频输入 → HomePod 立体声组");
    targets_->setText(addresses.join(" + ") + (unavailable_.isEmpty()
                                                   ? QString{}
                                                   : "\n" + unavailable_));
    start_->setEnabled(!busy_ && !recoveryPending_ && !closing_ &&
                       unavailable_.isEmpty());
  } catch (const std::exception &e) {
    title_->setText("音频输入 → AirPlay 接收端");
    targets_->setText(
        QString::fromUtf8(e.what()) +
        (unavailable_.isEmpty() ? QString{} : "\n" + unavailable_));
    start_->setEnabled(false);
  }
}
void StreamingPanel::scan() {
  if (!busy_ && !recoveryPending_ && !closing_ &&
      receiverModes_->currentIndex() == 0)
    discovery_.refresh();
}
app::Timing StreamingPanel::timing() const {
  app::Timing value;
  for (size_t i = 0; i < timings_.size(); ++i)
    value.*(app::timingFields[i].member) = timings_[i]->value() / 1000;
  return value;
}
void StreamingPanel::setBusy(bool busy) {
  busy_ = busy;
  const bool engaged = busy || recoveryPending_;
  receiverModes_->setEnabled(!engaged);
  defaults_->setEnabled(!engaged);
  for (auto *field : timings_)
    field->setEnabled(!engaged);
  stop_->setEnabled(engaged);
  updateTargets();
  if (!busy) {
    volumePending_ = false;
    leftLevel_->setValue(0);
    rightLevel_->setValue(0);
    leftLevel_->setFormat("静音");
    rightLevel_->setFormat("静音");
  }
}
void StreamingPanel::setRecoveryPending(bool pending) {
  recoveryPending_ = pending;
  setBusy(busy_);
  if (pending)
    state_->setText("宿主音频处理中断，等待恢复（最多 5 秒）；可点击停止取消");
}
void StreamingPanel::appendLog(const QString &text) {
  log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") +
                        "  " + text);
}

void StreamingPanel::setTiming(const app::Timing &value) {
  for (size_t i = 0; i < timings_.size(); ++i) {
    QSignalBlocker blocker(timings_[i]);
    timings_[i]->setValue(value.*(app::timingFields[i].member) * 1000);
  }
}
void StreamingPanel::showError(const QString &text) {
  state_->setText("错误：" + text);
  appendLog("错误：" + text);
}
void StreamingPanel::setUnavailable(const QString &reason) {
  unavailable_ = reason;
  updateTargets();
}
void StreamingPanel::cancelDiscovery() {
  closing_ = true;
  discovery_.cancel();
  updateTargets();
}
void StreamingPanel::beginDiscovery() {
  closing_ = false;
  scan();
}
void StreamingPanel::clearReceiverSelection() {
  manualFirst_->clear();
  manualSecond_->clear();
  for (int i = 0; i < receivers_->count(); ++i)
    receivers_->item(i)->setCheckState(Qt::Unchecked);
  updateTargets();
}
void StreamingPanel::drainDiscovery() {
  cancelDiscovery();
  if (!discovery_.busy())
    return;
  QEventLoop loop;
  connect(&discovery_, &airplay::ReceiverDiscovery::idle, &loop,
          &QEventLoop::quit);
  // Cancellation completion is delivered asynchronously. Keep the object and
  // DLL callback code alive until native requests have acknowledged it.
  QTimer retry;
  retry.setInterval(100);
  connect(&retry, &QTimer::timeout, &discovery_,
          &airplay::ReceiverDiscovery::cancel);
  retry.start();
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}
} // namespace ui
