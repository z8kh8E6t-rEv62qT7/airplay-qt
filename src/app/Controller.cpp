#include "Controller.h"
#ifdef Q_OS_MACOS
#include <QCoreApplication>
#include <QPermissions>
#endif

namespace app {
Controller::Controller(QObject *parent)
    : QObject(parent), capture_(audio::createInputCapture()) {
  connect(&session_, &SessionController::status, this, &Controller::status);
  connect(&session_, &SessionController::log, this, &Controller::log);
  connect(&session_, &SessionController::group, this, &Controller::group);
  connect(&session_, &SessionController::error, this, &Controller::error);
  connect(&session_, &SessionController::busyChanged, this,
          &Controller::busyChanged);
  connect(&session_, &SessionController::streamingChanged, this,
          &Controller::streamingChanged);
  connect(&session_, &SessionController::volumeApplied, this,
          &Controller::volumeApplied);
  connect(&session_, &SessionController::telemetry, this,
          &Controller::telemetry);
  connect(&session_, &SessionController::stopped, this, &Controller::stopped);
  connect(&session_, &SessionController::stopCapture, this,
          &Controller::stopCapture);
  connect(&session_, &SessionController::startCapture, this, [this] {
    try {
      capture_->start();
      session_.captureStarted();
    } catch (const std::exception &e) {
      session_.stop(QString::fromUtf8(e.what()));
    }
  });
}
Controller::~Controller() { capture_->stop(); }
Settings Controller::initialize() {
  drivers_ = audio::inputDevices();
  try {
    return settings_.load();
  } catch (const std::exception &e) {
    emit error(QString::fromUtf8(e.what()) +
               "\n原配置不会被覆盖。请修复或移走 AirPlayQt.json 后重启。");
    return {};
  }
}
void Controller::selectDriver(const QString &id, void *window) {
  if (busy())
    return;
  selectedId_.clear();
  window_ = window;
  try {
    const auto list = capture_->open(id, window);
    selectedId_ = id;
    emit channels(list);
    emit session_.status("驱动已加载，等待开始");
  } catch (const std::exception &e) {
    emit channels({});
    emit error(QString::fromUtf8(e.what()));
  }
}
void Controller::controlPanel() {
  if (busy())
    return;
  try {
    capture_->controlPanel();
    const auto id = selectedId_;
    selectDriver(id, window_);
  } catch (const std::exception &e) {
    emit error(QString::fromUtf8(e.what()));
  }
}
void Controller::start(const Settings &settings,
                       const QList<airplay::ReceiverEndpoint> &endpoints,
                       bool saveSettings) {
  if (busy())
    return;
#ifdef Q_OS_MACOS
  const QMicrophonePermission permission;
  const auto permissionStatus = qApp->checkPermission(permission);
  if (permissionStatus == Qt::PermissionStatus::Denied) {
    emit error("音频输入权限被拒绝。");
    return;
  }
  if (permissionStatus == Qt::PermissionStatus::Undetermined) {
    permissionPending_ = true;
    const auto revision = ++permissionRevision_;
    emit busyChanged(true);
    qApp->requestPermission(
        permission, this,
        [this, settings, endpoints, revision,
         saveSettings](const QPermission &result) {
          if (!permissionPending_ || revision != permissionRevision_)
            return;
          permissionPending_ = false;
          emit busyChanged(false);
          if (result.status() == Qt::PermissionStatus::Granted)
            start(settings, endpoints, saveSettings);
          else
            emit error("音频输入权限被拒绝。");
        });
    return;
  }
#endif
  try {
    airplay::validateEndpoints(endpoints);
    if (settings.driverId.isEmpty() || selectedId_ != settings.driverId)
      throw airplay::Error("请选择有效输入设备");
    if (const auto message = settings.validate(); !message.isEmpty())
      throw airplay::Error(message);
    emit session_.status("准备音频输入");
    const auto stream = capture_->prepare(settings.left, settings.right,
                                          settings.timing.backlog);
#ifdef Q_OS_WIN
    emit session_.log("采集计时：已申请 1 ms 精度，并禁止忽略计时精度请求。");
#endif
    if (saveSettings && settings_.writable())
      settings_.saveInput(settings);
    session_.start(settings.timing, stream, endpoints);
  } catch (const std::exception &e) {
    stopCapture();
    emit error(QString::fromUtf8(e.what()));
    emit session_.status("准备失败");
  }
}
void Controller::stopCapture() {
  if (const auto message = capture_->stop(); !message.isEmpty())
    emit error(message);
}
void Controller::stop() {
  if (permissionPending_) {
    permissionPending_ = false;
    ++permissionRevision_;
    emit busyChanged(false);
    emit stopped();
  }
  session_.stop();
}
void Controller::volume(double db) { session_.volume(db); }
} // namespace app
