#pragma once
#include "airplay/ReceiverDiscovery.h"
#include "app/SessionController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ui {
class StreamingPanel : public QWidget {
  Q_OBJECT
public:
  explicit StreamingPanel(
      app::SessionController &, QWidget *parent = nullptr,
      const airplay::DiscoveryApi & = airplay::defaultDiscoveryApi());
  i18n::Language language() const { return session_.language(); }
  void setLanguage(i18n::Language);
  // Bind display-only properties; updating a translation never recreates
  // controls.
  void bindText(QObject *, const char *property, const i18n::Message &);
  void addInputWidget(QWidget *widget) { inputLayout_->addWidget(widget); }
  app::Timing timing() const;
  airplay::NetworkBinding networkBinding() const;
  void setNetworkBinding(const airplay::NetworkBinding &);
  QList<airplay::ReceiverEndpoint> endpoints() const;
  bool discoveryMode() const;
  QList<app::ReceiverSelection> receiverSelection() const;
  void setRememberedReceivers(const QList<app::ReceiverSelection> &);
  void setTiming(const app::Timing &);
  void beginDiscovery();
  void clearReceiverSelection();
  void cancelDiscovery();
  bool discoveryBusy() const { return discovery_.busy(); }
  void drainDiscovery();
  void setUnavailable(const i18n::Message &reason);
  void showError(const i18n::Message &);
  void appendLog(const i18n::Message &);
  void setRecoveryPending(bool);
  app::WindowLayout windowLayout(QSize windowSize, int expandedWidth) const;
  void setWindowLayout(const app::WindowLayout &);
  Q_SLOT void setBusy(bool);
protected:
  void hideEvent(QHideEvent *) override;
signals:
  // UI-thread notifications; consumers read typed values synchronously.
  void languageChanged();
  void startRequested();
  void stopRequested();
  void discoveryIdle();
  void timingChanged();
  void networkBindingChanged();
  // The owner resizes its window (or asks the VST3 host) after visibility changes.
  void logVisibilityChanged(bool visible, int removedWidth);
  void layoutChanged();

private:
  struct TextBinding {
    QPointer<QObject> target;
    QByteArray property;
    i18n::Message message;
  };
  QList<TextBinding> textBindings_;
  QLabel *label(const i18n::Message &);
  QPushButton *button(const i18n::Message &);
  void retranslate();
  QPushButton *languageButton_, *pauseDisplay_;
  QPushButton *toggleLog_ = nullptr;
  QSplitter *columns_ = nullptr;
  QList<int> expandedSplitterSizes_;
  void setLogVisible(bool);
  QTabWidget *timingTabs_;
  QVBoxLayout *inputLayout_;
  void updateTargets();
  void refreshNetworks();
  void clearDiscoveredReceivers();
  void restartDiscovery();
  QComboBox *network_;
  QPushButton *refreshNetwork_;
  void scan();
  void restoreReceivers();
  airplay::ReceiverDiscovery discovery_;
  app::SessionController &session_;
  QTabWidget *receiverModes_;
  QListWidget *receivers_;
  QLineEdit *manualFirst_, *manualSecond_;
  QPushButton *refresh_, *start_, *stop_, *defaults_, *applyVolume_;
  QLabel *targets_, *discoveryStatus_, *state_, *group_, *stats_;
  std::array<QProgressBar *, 2> levels_{};
  std::array<QPushButton *, 2> levelReadouts_{};
  std::array<double, 2> levelPeaks_{};
  void setLevels(double left, double right);
  void updatePeakReadouts();
  QDoubleSpinBox *volume_;
  QCheckBox *mute_;
  QPlainTextEdit *log_;
  std::array<QDoubleSpinBox *, 11> timings_{};
  std::array<QSpinBox *, 3> sampleInputs_{};
  std::array<QLabel *, 3> samplePreviews_{};
  void updateSamplePreviews();
  void timingEdited();
  bool discoveryStarted_ = false;
  bool busy_ = false, closing_ = false, streaming_ = false,
       volumePending_ = false, recoveryPending_ = false;
  double restoreVolume_ = 0;
  i18n::Message unavailable_;
  QList<app::ReceiverSelection> rememberedReceivers_;
  bool restoreReceiversAllowed_ = true;
};
} // namespace ui
