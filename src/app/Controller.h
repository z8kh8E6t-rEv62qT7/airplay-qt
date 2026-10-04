#pragma once
#include "SessionController.h"
#include "Settings.h"
#include "airplay/ReceiverEndpoint.h"
#include "app/Message.h"
#include "audio/InputCapture.h"
#include <QObject>
#include <QThread>
#include <memory>
namespace app {
class Controller : public QObject {
  Q_OBJECT
public:
  explicit Controller(QObject *parent = nullptr,
                      const audio::InputCaptureApi &api = {});
  ~Controller() override;
  Settings initialize();
  void saveLanguage(i18n::Language);
  void saveWindowLayout(const WindowLayout &);
  const QList<ReceiverSelection> &rememberedReceivers() const {
    return settings_.receivers();
  }
  QList<audio::DriverInfo> drivers() const { return drivers_; }
  void selectDriver(const QString &id, void *window);
  void controlPanel();
  void refreshDevices(const QString &id, void *window);
  void start(
      const Settings &settings,
      const QList<airplay::ReceiverEndpoint> &endpoints,
      bool saveSettings = true,
      const std::optional<QList<ReceiverSelection>> &selection = std::nullopt);
  void stop();
  void volume(double db);
  bool busy() const { return permissionPending_ || session_.busy(); }
  SessionController &session() { return session_; }
signals:
  void devicesChanged(QList<audio::DriverInfo> devices);
  void devicesRefreshed(QList<audio::DriverInfo> devices);
  void channels(QList<audio::ChannelInfo> channels);
  void status(QJsonArray text);
  void log(QJsonArray text);
  void group(QJsonArray text);
  void error(QJsonArray text);
  void busyChanged(bool busy);
  void streamingChanged(bool streaming);
  void volumeApplied(double db);
  void telemetry(double left, double right, double backlog, quint64 packets,
                 quint64 retransmitted, quint64 expired);
  void stopped();

private:
  friend class ControllerTestAccess;
  void connectCapture();
  void openDriver(const audio::DriverInfo &, void *window);
  void stopCapture();
  SessionController session_;
  audio::InputCaptureApi captureApi_;
  std::unique_ptr<audio::InputCapture> capture_;
  QList<audio::DriverInfo> drivers_;
  QString selectedId_;
  audio::CaptureKind selectedKind_ = audio::CaptureKind::Input;
  void *window_ = nullptr;
  SettingsStore settings_;
  bool permissionPending_ = false;
  quint64 permissionRevision_ = 0;
};
} // namespace app
