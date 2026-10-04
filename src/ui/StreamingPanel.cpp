#include "StreamingPanel.h"
#include "app/Message.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace ui {
StreamingPanel::StreamingPanel(app::SessionController &session, QWidget *parent,
                               const airplay::DiscoveryApi &discoveryApi)
    : QWidget(parent), discovery_(this, std::move(discoveryApi)),
      session_(session) {
  session_.setTelemetryEnabled(true);
  auto *languageRow = new QHBoxLayout;
  auto *toggleLog = toggleLog_ = button(i18n::text(i18n::Id::HideLog));
  toggleLog->setObjectName("toggleLog");
  languageRow->addWidget(toggleLog);
  auto *clearLog = button(i18n::text(i18n::Id::ClearLog));
  clearLog->setObjectName("clearLog");
  languageRow->addWidget(clearLog);
  languageRow->addStretch();
  pauseDisplay_ = button(i18n::text(i18n::Id::PauseDisplay));
  pauseDisplay_->setObjectName("pauseDisplay");
  pauseDisplay_->setCheckable(true);
  languageRow->addWidget(pauseDisplay_);
  connect(pauseDisplay_, &QPushButton::toggled, this, [this](bool paused) {
    session_.setTelemetryEnabled(!paused);
    bindText(pauseDisplay_, "text",
             i18n::text(paused ? i18n::Id::ResumeDisplay
                               : i18n::Id::PauseDisplay));
  });
  languageButton_ = new QPushButton;
  languageButton_->setObjectName("languageToggle");
  languageRow->addWidget(languageButton_);
  connect(languageButton_, &QPushButton::clicked, this, [this] {
    setLanguage(language() == i18n::Language::English
                    ? i18n::Language::Chinese
                    : i18n::Language::English);
  });
  auto *outer = new QHBoxLayout(this);
  auto *columns = columns_ = new QSplitter(Qt::Horizontal);
  outer->addWidget(columns);
  columns->setObjectName("streamingColumns");
  columns->setChildrenCollapsible(false);
  columns->setHandleWidth(8);
  auto *controlGroup = new QWidget;
  auto *controlColumns = new QHBoxLayout(controlGroup);
  controlColumns->setContentsMargins(0, 0, 0, 0);
  columns->addWidget(controlGroup);
  auto *controlsPane = new QWidget;
  controlsPane->setObjectName("controlsPane");
  auto *layout = new QVBoxLayout(controlsPane);
  layout->setContentsMargins(0, 0, 0, 0);
  controlColumns->addWidget(controlsPane, 1);
  inputLayout_ = new QVBoxLayout;
  layout->addLayout(inputLayout_);
  auto *networkRow = new QHBoxLayout;
  networkRow->addWidget(label(i18n::text(i18n::Id::SendingInterface)));
  network_ = new QComboBox;
  network_->setObjectName("networkBinding");
  refreshNetwork_ = button(i18n::text(i18n::Id::RefreshInterfaces));
  refreshNetwork_->setObjectName("refreshNetwork");
  networkRow->addWidget(network_, 1);
  networkRow->addWidget(refreshNetwork_);
  layout->addLayout(networkRow);
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
  discoveryStatus_ = label(i18n::text(i18n::Id::WaitingToScan));
  discoveryStatus_->setWordWrap(true);
  refresh_ = button(i18n::text(i18n::Id::Refresh));
  refresh_->setObjectName("refreshReceivers");
  scanRow->addWidget(discoveryStatus_, 1);
  scanRow->addWidget(refresh_);
  discoveredLayout->addLayout(scanRow);
  receiverModes_->addTab(discovered, QString{});
  auto *manual = new QWidget;
  auto *manualForm = new QFormLayout(manual);
  manualFirst_ = new QLineEdit;
  manualFirst_->setObjectName("manualFirst");
  manualSecond_ = new QLineEdit;
  manualSecond_->setObjectName("manualSecond");
  bindText(manualFirst_, "placeholderText",
           i18n::text(i18n::Id::IPvPortDefaultsToPort));
  bindText(manualSecond_, "placeholderText",
           i18n::text(i18n::Id::OptionalTwoReceiversMustBelongToAn));
  manualForm->addRow(label(i18n::text(i18n::Id::ReceiverRequired)),
                     manualFirst_);
  manualForm->addRow(label(i18n::text(i18n::Id::ReceiverOptional)),
                     manualSecond_);
  receiverModes_->addTab(manual, QString{});
  layout->addWidget(receiverModes_);
  targets_ = new QLabel;
  targets_->setWordWrap(true);
  targets_->setObjectName("receiverSummary");
  layout->addWidget(targets_);
  timingTabs_ = new QTabWidget;
  auto *tabs = timingTabs_;
  for (int page = 0; page < 2; ++page) {
    auto *widget = new QWidget;
    auto *fields = new QFormLayout(widget);
    for (size_t i = page ? 4 : 0; i < (page ? app::timingMsFields.size() : 4);
         ++i) {
      if (i == 2) {
        for (size_t j = 0; j < sampleInputs_.size(); ++j) {
          const auto &f = app::timingSamplesFields[j];
          auto *row = new QHBoxLayout;
          auto *input = new QSpinBox;
          input->setObjectName(f.key);
          input->setRange(f.minimum, f.maximum);
          input->setSuffix(" samples");
          input->setSingleStep(f.member == &app::Timing::packetSamples ? 32 : 256);
          input->setMinimumWidth(64);
          input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
          row->addWidget(input, 1);
          sampleInputs_[j] = input;
          samplePreviews_[j] = new QLabel;
          samplePreviews_[j]->setObjectName(QString(f.key) + "Preview");
          samplePreviews_[j]->setWordWrap(false);
          row->addWidget(samplePreviews_[j]);
          fields->addRow(label(f.label), row);
        }
      }
      const auto &f = app::timingMsFields[i];
      auto *spin = new QDoubleSpinBox;
      timings_[i] = spin;
      spin->setObjectName(f.key);
      spin->setDecimals(3);
      spin->setRange(f.minimum, f.maximum);
      spin->setSuffix(" ms");
      spin->setSingleStep(f.powerOfTwo ? 15.625 : 10);
      if (f.powerOfTwo)
        bindText(spin, "toolTip", i18n::text(i18n::Id::PowerOfTwoIntervalHint));
      fields->addRow(label(f.label), spin);
    }
    tabs->addTab(widget, QString{});
  }
  layout->addWidget(tabs);
  defaults_ = button(i18n::text(i18n::Id::ResetTimingDefaults));
  layout->addWidget(defaults_, 0, Qt::AlignRight);
  auto *controls = new QHBoxLayout;
  start_ = button(i18n::text(i18n::Id::Start));
  start_->setObjectName("start");
  stop_ = button(i18n::text(i18n::Id::Stop));
  stop_->setObjectName("stop");
  stop_->setEnabled(false);
  controls->addWidget(start_);
  controls->addWidget(stop_);
  layout->addLayout(controls);
  group_ = label(i18n::text(i18n::Id::ReceiverIdentityHasNotBeenRead));
  group_->setWordWrap(true);
  layout->addWidget(group_);
  auto *volumeRow = new QHBoxLayout;
  volume_ = new QDoubleSpinBox;
  volume_->setRange(-144, 0);
  volume_->setDecimals(2);
  volume_->setSuffix(" dB");
  applyVolume_ = button(i18n::text(i18n::Id::SetAirPlayVolume));
  mute_ = new QCheckBox;
  bindText(mute_, "text", i18n::text(i18n::Id::Mute));
  volumeRow->addWidget(label(i18n::text(i18n::Id::ReceiverVolume)));
  volumeRow->addWidget(volume_);
  volumeRow->addWidget(applyVolume_);
  volumeRow->addWidget(mute_);
  layout->addLayout(volumeRow);
  volume_->setEnabled(false);
  applyVolume_->setEnabled(false);
  mute_->setEnabled(false);
  auto *metersPane = new QWidget;
  metersPane->setObjectName("metersPane");
  auto *meters = new QHBoxLayout(metersPane);
  meters->setContentsMargins(0, 0, 0, 0);
  meters->setSpacing(8);
  for (size_t i = 0; i < levels_.size(); ++i) {
    auto *channel = new QVBoxLayout;
    channel->setSpacing(2);
    auto *name = new QLabel(i == 0 ? "L" : "R");
    name->setFixedWidth(40);
    name->setAlignment(Qt::AlignCenter);
    channel->addWidget(name);
    auto *meter = new QProgressBar;
    levels_[i] = meter;
    meter->setObjectName(i == 0 ? "leftLevel" : "rightLevel");
    meter->setRange(0, 1000);
    meter->setOrientation(Qt::Vertical);
    meter->setInvertedAppearance(false);
    meter->setTextVisible(false);
    // Keep the meter body visibly thick while preserving the vertical scale.
    meter->setFixedWidth(40);
    meter->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    channel->addWidget(meter, 1);
    auto *readout = new QPushButton;
    levelReadouts_[i] = readout;
    readout->setObjectName(i == 0 ? "leftLevelReadout" : "rightLevelReadout");
    readout->setFlat(true);
    readout->setAutoDefault(false);
    readout->setFixedSize(40, readout->fontMetrics().lineSpacing() * 2 + 8);
    bindText(readout, "toolTip", i18n::text(i18n::Id::ClearStereoPeaks));
    bindText(readout, "accessibleName", i18n::text(i18n::Id::ClearStereoPeaks));
    connect(readout, &QPushButton::clicked, this, [this] {
      levelPeaks_.fill(0);
      setLevels(0, 0);
    });
    channel->addWidget(readout);
    meters->addLayout(channel);
  }
  setLevels(0, 0);
  metersPane->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  controlColumns->addWidget(metersPane);
  state_ = label(i18n::text(i18n::Id::Ready));
  stats_ = label(i18n::text(i18n::Id::InitialStatistics));
  state_->setObjectName("sessionStatus");
  stats_->setObjectName("statistics");
  state_->setWordWrap(true);
  stats_->setWordWrap(true);
  layout->addWidget(state_);
  layout->addWidget(stats_);
  layout->addStretch();
  layout->addLayout(languageRow);
  log_ = new QPlainTextEdit;
  log_->setObjectName("sessionLog");
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(1000);
  log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  connect(clearLog, &QPushButton::clicked, log_, &QPlainTextEdit::clear);
  columns->addWidget(log_);
  columns->setStretchFactor(0, 0);
  columns->setStretchFactor(1, 1);
  connect(toggleLog, &QPushButton::clicked, this, [this] {
    const bool show = log_->isHidden();
    if (!show)
      expandedSplitterSizes_ = columns_->sizes();
    const int removedWidth = show ? 0 : log_->width() + columns_->handleWidth();
    setLogVisible(show);
    this->layout()->activate();
    emit logVisibilityChanged(show, removedWidth);
    if (show)
      columns_->setSizes(expandedSplitterSizes_);
    emit layoutChanged();
  });
  connect(columns, &QSplitter::splitterMoved, this,
          [this] { emit layoutChanged(); });
  connect(&discovery_, &airplay::ReceiverDiscovery::cleared, this,
          &StreamingPanel::clearDiscoveredReceivers);
  connect(
      &discovery_, &airplay::ReceiverDiscovery::status, this,
      [this](i18n::Message text) { bindText(discoveryStatus_, "text", text); });
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
      appendLog(i18n::text(i18n::Id::NotAllSavedReceiversMatchedByName));
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
              bindText(discoveryStatus_, "text",
                       i18n::text(i18n::Id::SelectAtMostTwoReceiversBothMust));
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
  refreshNetworks();
  connect(network_, &QComboBox::currentIndexChanged, this, [this] {
    emit networkBindingChanged();
    restartDiscovery();
  });
  connect(refreshNetwork_, &QPushButton::clicked, this, [this] {
    refreshNetworks();
    updateTargets();
    scan();
  });
  updateTargets();
  connect(&session_, &app::SessionController::status, this,
          [this](i18n::Message text) { bindText(state_, "text", text); });
  connect(&session_, &app::SessionController::log, this,
          &StreamingPanel::appendLog);
  connect(&session_, &app::SessionController::group, this,
          [this](i18n::Message text) { bindText(group_, "text", text); });
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
            {
              QSignalBlocker blocker(mute_);
              mute_->setChecked(db <= -144);
            }
            if (db > -144) {
              QSignalBlocker blocker(volume_);
              volume_->setValue(db);
              restoreVolume_ = db;
            }
            volume_->setEnabled(streaming_ && !mute_->isChecked());
            applyVolume_->setEnabled(streaming_ && !mute_->isChecked());
            mute_->setEnabled(streaming_);
          });
  connect(&session_, &app::SessionController::telemetry, this,
          [this](double l, double r, double backlog, quint64 packets,
                 quint64 retransmitted, quint64 expired) {
            if (!session_.telemetryEnabled())
              return;
            setLevels(l, r);
            bindText(stats_, "text",
                     i18n::text(i18n::Id::Statistics)
                         .arg(backlog * 44100, 0, 'f', 0)
                         .arg(backlog * 1000, 0, 'f', 1)
                         .arg(packets)
                         .arg(retransmitted)
                         .arg(expired));
          });
  connect(defaults_, &QPushButton::clicked, this, [this] {
    setTiming(app::Timing{});
    timingEdited();
  });
  connect(start_, &QPushButton::clicked, this, [this] {
    QSignalBlocker block(mute_);
    mute_->setChecked(false);
    volumePending_ = false;
    try {
      airplay::NetworkRoute::resolve(networkBinding());
      endpoints(); // Validate before canceling discovery and notifying
                   // consumers.
      discovery_.cancel();
      emit startRequested();
    } catch (const std::exception &e) {
      appendLog(i18n::fromException(e));
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

  for (auto *spin : timings_)
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            &StreamingPanel::timingEdited);
  for (auto *input : sampleInputs_)
    connect(input, &QSpinBox::valueChanged, this,
            &StreamingPanel::timingEdited);
  retranslate();
  setTiming(app::Timing{});
  bindText(state_, "text", session_.currentStatus());
  bindText(group_, "text", session_.currentGroup());
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
QLabel *StreamingPanel::label(const i18n::Message &text) {
  auto *result = new QLabel;
  bindText(result, "text", text);
  return result;
}
QPushButton *StreamingPanel::button(const i18n::Message &text) {
  auto *result = new QPushButton;
  bindText(result, "text", text);
  return result;
}
void StreamingPanel::bindText(QObject *target, const char *property,
                              const i18n::Message &message) {
  for (auto &binding : textBindings_) {
    if (binding.target == target && binding.property == property) {
      binding.message = message;
      target->setProperty(property, message.render(language()));
      return;
    }
  }
  textBindings_.append({target, property, message});
  target->setProperty(property, message.render(language()));
}
void StreamingPanel::setLanguage(i18n::Language value) {
  if (!i18n::valid(value) || value == language())
    return;
  session_.setLanguage(value);
  retranslate();
  emit languageChanged();
}
void StreamingPanel::retranslate() {
  for (const auto &binding : textBindings_)
    if (binding.target)
      binding.target->setProperty(binding.property.constData(),
                                  binding.message.render(language()));
  languageButton_->setText(language() == i18n::Language::English
                               ? QStringLiteral("中文")
                               : QStringLiteral("English"));
  receiverModes_->setTabText(
      0, i18n::text(i18n::Id::Discovery).render(language()));
  receiverModes_->setTabText(1,
                             i18n::text(i18n::Id::Manual).render(language()));
  timingTabs_->setTabText(0,
                          i18n::text(i18n::Id::BasicTiming).render(language()));
  timingTabs_->setTabText(
      1, i18n::text(i18n::Id::AdvancedTiming).render(language()));
  if (network_->count())
    network_->setItemText(
        0, i18n::text(i18n::Id::AutomaticSystemRouting).render(language()));
  for (int i = 1; i < network_->count(); ++i)
    if (network_->itemData(i, Qt::UserRole + 2).toBool())
      network_->setItemText(
          i, (network_->itemData(i).toString() + " · " +
              network_->itemData(i, Qt::UserRole + 1).toString() +
              i18n::text(i18n::Id::Unavailable))
                 .render(language()));
}
airplay::NetworkBinding StreamingPanel::networkBinding() const {
  return {network_->currentData(Qt::UserRole).toString(),
          network_->currentData(Qt::UserRole + 1).toString()};
}
void StreamingPanel::refreshNetworks() { setNetworkBinding(networkBinding()); }
void StreamingPanel::setNetworkBinding(const airplay::NetworkBinding &value) {
  const auto previous = networkBinding();
  QSignalBlocker block(network_);
  network_->clear();
  network_->addItem(
      i18n::text(i18n::Id::AutomaticSystemRouting).render(language()),
      QString{});
  int selected = 0;
  for (const auto &binding : airplay::NetworkBinding::available()) {
    network_->addItem(binding.interfaceName + " · " + binding.ipv4,
                      binding.interfaceName);
    const int row = network_->count() - 1;
    network_->setItemData(row, binding.ipv4, Qt::UserRole + 1);
    if (binding == value)
      selected = row;
  }
  if (!value.automatic() && !selected) {
    network_->addItem((value.interfaceName + " · " + value.ipv4 +
                       i18n::text(i18n::Id::Unavailable))
                          .render(language()),
                      value.interfaceName);
    selected = network_->count() - 1;
    network_->setItemData(selected, true, Qt::UserRole + 2);
    network_->setItemData(selected, value.ipv4, Qt::UserRole + 1);
  }
  network_->setCurrentIndex(selected);
  updateTargets();
  if (discoveryStarted_ && previous != value)
    restartDiscovery();
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
    if (const auto error = timing().validate(); !error.isEmpty())
      throw i18n::MessageError(error);
    airplay::NetworkRoute::resolve(networkBinding());
    const auto selected = endpoints();
    QStringList addresses;
    for (const auto &endpoint : selected)
      addresses.append(endpoint.text());
    bindText(targets_, "text",
             addresses.join(" + ") + (unavailable_.isEmpty()
                                          ? i18n::Message{}
                                          : "\n" + unavailable_));
    start_->setEnabled(!busy_ && !recoveryPending_ && !closing_ &&
                       unavailable_.isEmpty());
  } catch (const std::exception &e) {
    bindText(targets_, "text",
             i18n::fromException(e) + (unavailable_.isEmpty()
                                           ? i18n::Message{}
                                           : "\n" + unavailable_));
    start_->setEnabled(false);
  }
}
void StreamingPanel::clearDiscoveredReceivers() {
  QSignalBlocker block(receivers_);
  receivers_->clear();
  restoreReceiversAllowed_ = true;
  updateTargets();
}
void StreamingPanel::restartDiscovery() {
  discovery_.cancel();
  clearDiscoveredReceivers();
  scan();
}
void StreamingPanel::scan() {
  if (!busy_ && !recoveryPending_ && !closing_ &&
      receiverModes_->currentIndex() == 0)
    discovery_.refresh(networkBinding());
}
app::Timing StreamingPanel::timing() const {
  app::Timing value;
  for (size_t i = 0; i < timings_.size(); ++i)
    value.*(app::timingMsFields[i].member) = timings_[i]->value();
  for (size_t i = 0; i < sampleInputs_.size(); ++i)
    value.*(app::timingSamplesFields[i].member) = sampleInputs_[i]->value();
  return value;
}
void StreamingPanel::setBusy(bool busy) {
  busy_ = busy;
  const bool engaged = busy || recoveryPending_;
  receiverModes_->setEnabled(!engaged);
  network_->setEnabled(!engaged);
  refreshNetwork_->setEnabled(!engaged);
  defaults_->setEnabled(!engaged);
  for (auto *field : timings_)
    field->setEnabled(!engaged);
  for (auto *input : sampleInputs_)
    input->setEnabled(!engaged);
  stop_->setEnabled(engaged);
  updateTargets();
  if (!busy) {
    volumePending_ = false;
    if (session_.telemetryEnabled())
      setLevels(0, 0);
  }
}
void StreamingPanel::setRecoveryPending(bool pending) {
  const bool changed = recoveryPending_ != pending;
  recoveryPending_ = pending;
  setBusy(busy_);
  if (!pending && changed)
    bindText(state_, "text", session_.currentStatus());
  if (pending)
    bindText(state_, "text",
             i18n::text(i18n::Id::HostAudioProcessingInterruptedWaitingUpTo));
}
void StreamingPanel::appendLog(const i18n::Message &text) {
  log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") +
                        "  " + text.render(language()));
  auto *scroll = log_->verticalScrollBar();
  scroll->setValue(scroll->maximum());
}
app::WindowLayout StreamingPanel::windowLayout(QSize size, int expandedWidth) const {
  const bool visible = !log_->isHidden();
  const auto sizes = visible ? columns_->sizes() : expandedSplitterSizes_;
  return {size.width(), size.height(), sizes.value(0), sizes.value(1),
          visible ? size.width() : std::max(size.width(), expandedWidth), visible};
}
void StreamingPanel::setWindowLayout(const app::WindowLayout &value) {
  if (!value.valid())
    return;
  setLogVisible(value.logVisible);
  if (value.controlWidth > 0 && value.logWidth > 0) {
    expandedSplitterSizes_ = {value.controlWidth, value.logWidth};
    columns_->setSizes(expandedSplitterSizes_);
  }
}
void StreamingPanel::setLogVisible(bool visible) {
  log_->setVisible(visible);
  bindText(toggleLog_, "text",
           i18n::text(visible ? i18n::Id::HideLog : i18n::Id::ShowLog));
}
void StreamingPanel::hideEvent(QHideEvent *event) {
  QWidget::hideEvent(event);
  emit layoutChanged();
}

void StreamingPanel::setTiming(const app::Timing &value) {
  for (size_t i = 0; i < timings_.size(); ++i) {
    QSignalBlocker blocker(timings_[i]);
    timings_[i]->setValue(value.*(app::timingMsFields[i].member));
  }
  for (size_t i = 0; i < sampleInputs_.size(); ++i) {
    QSignalBlocker blocker(sampleInputs_[i]);
    sampleInputs_[i]->setValue(value.*(app::timingSamplesFields[i].member));
  }
  updateSamplePreviews();
  updateTargets();
}
void StreamingPanel::updateSamplePreviews() {
  const auto value = timing();
  for (size_t i = 0; i < samplePreviews_.size(); ++i) {
    const auto &field = app::timingSamplesFields[i];
    const int samples = value.*(field.member);
    bindText(samplePreviews_[i], "text", QString("%1 ms").arg(
                 double(samples) * 1000 / 44100, 0, 'f', 3));
  }
}
void StreamingPanel::setLevels(double left, double right) {
  const std::array values{left, right};
  for (size_t i = 0; i < levels_.size(); ++i) {
    levels_[i]->setValue(int(values[i] * 1000));
    levelPeaks_[i] = std::max(levelPeaks_[i], values[i]);
  }
  updatePeakReadouts();
}
void StreamingPanel::updatePeakReadouts() {
  for (size_t i = 0; i < levelReadouts_.size(); ++i) {
    bindText(levelReadouts_[i], "text",
             (levelPeaks_[i] > 0
                  ? QString::number(20 * std::log10(levelPeaks_[i]), 'f', 1)
                  : QStringLiteral("−∞")) + "\ndBFS");
  }
}
void StreamingPanel::timingEdited() {
  updateSamplePreviews();
  updateTargets();
  emit timingChanged();
}
void StreamingPanel::showError(const i18n::Message &text) {
  bindText(state_, "text", i18n::text(i18n::Id::ErrorPrefix) + text);
  appendLog(i18n::text(i18n::Id::ErrorPrefix) + text);
}
void StreamingPanel::setUnavailable(const i18n::Message &reason) {
  unavailable_ = reason;
  updateTargets();
}
void StreamingPanel::cancelDiscovery() {
  closing_ = true;
  discovery_.cancel();
  updateTargets();
}
void StreamingPanel::beginDiscovery() {
  discoveryStarted_ = true;
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
