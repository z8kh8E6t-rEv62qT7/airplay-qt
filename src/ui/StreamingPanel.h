#pragma once
#include "airplay/ReceiverDiscovery.h"
#include "app/SessionController.h"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>

namespace ui {
class StreamingPanel : public QWidget {
  Q_OBJECT
public:
  explicit StreamingPanel(
      app::SessionController &, QWidget *parent = nullptr,
      const airplay::DiscoveryApi & = airplay::defaultDiscoveryApi());
  app::Timing timing() const;
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
  void setUnavailable(const QString &reason);
  void showError(const QString &);
  void appendLog(const QString &);
  void setRecoveryPending(bool);
  Q_SLOT void setBusy(bool);
signals:
  // UI-thread notifications; consumers read typed values synchronously.
  void startRequested();
  void stopRequested();
  void discoveryIdle();
  void timingChanged();

private:
  void updateTargets();
  void scan();
  void restoreReceivers();
  airplay::ReceiverDiscovery discovery_;
  app::SessionController &session_;
  QTabWidget *receiverModes_;
  QListWidget *receivers_;
  QLineEdit *manualFirst_, *manualSecond_;
  QPushButton *refresh_, *start_, *stop_, *defaults_, *applyVolume_;
  QLabel *title_, *targets_, *discoveryStatus_, *state_, *group_, *stats_;
  QProgressBar *leftLevel_, *rightLevel_;
  QDoubleSpinBox *volume_;
  QCheckBox *mute_;
  QPlainTextEdit *log_;
  std::array<QDoubleSpinBox *, 13> timings_{};
  bool busy_ = false, closing_ = false, streaming_ = false,
       volumePending_ = false, recoveryPending_ = false;
  double restoreVolume_ = 0;
  QString unavailable_;
  QList<app::ReceiverSelection> rememberedReceivers_;
  bool restoreReceiversAllowed_ = true;
};
} // namespace ui
