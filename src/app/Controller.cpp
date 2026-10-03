#include "Controller.h"
#include "app/Message.h"
#include <algorithm>
#ifdef Q_OS_MACOS
#include <QCoreApplication>
#include <QPermissions>
#endif

namespace app {
Controller::Controller(QObject *parent, const audio::InputCaptureApi &api)
    : QObject(parent), captureApi_(api),
      capture_(captureApi_.create(audio::CaptureKind::Input)) {
  connectCapture();
  connect(&session_, &SessionController::status, this, &Controller::status);
  connect(&session_, &SessionController::log, this, &Controller::log);
  connect(&session_, &SessionController::group, this, &Controller::group);
  connect(&session_, &SessionController::error, this, &Controller::error);
  connect(&session_, &SessionController::busyChanged, this,
          &Controller::busyChanged);
  connect(&session_, &SessionController::streamingChanged, this,
          &Controller::streamingChanged);
  connect(&session_, &SessionController::streamingChanged, this, [this](bool streaming) {
    capture_->setVolumeControlEnabled(streaming);
  });
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
      session_.stop(i18n::fromException(e));
    }
  });
}
Controller::~Controller() { capture_->stop(); }
void Controller::connectCapture() {
  connect(capture_.get(), &audio::InputCapture::volumeRequested, &session_, &SessionController::inputVolume);
  connect(capture_.get(), &audio::InputCapture::volumeStepRequested, &session_, &SessionController::inputVolumeStep);
  connect(capture_.get(), &audio::InputCapture::log, &session_, &SessionController::log);
  connect(capture_.get(), &audio::InputCapture::devicesChanged, this, [this] {
    drivers_ = captureApi_.devices();
    emit devicesChanged(drivers_);
  }, Qt::QueuedConnection);
}
Settings Controller::initialize() {
  drivers_ = captureApi_.devices();
  try {
    return settings_.load();
  } catch (const std::exception &e) {
    emit error(
        i18n::fromException(e) +
        i18n::text(i18n::Id::ConfigPreservedPrefix) +
        Settings::path() + i18n::text(i18n::Id::AndRestart));
    return {};
  }
}
void Controller::saveLanguage(i18n::Language language) {
  try {
    settings_.saveLanguage(language);
  } catch (const std::exception &e) {
    emit error(
        i18n::text(i18n::Id::LanguageSaveFailed).arg(i18n::fromException(e)));
  }
}
void Controller::selectDriver(const QString &id, void *window) {
  if (busy())
    return;
  selectedId_.clear();
  window_ = window;
  try {
    const auto drivers = captureApi_.devices();
    const auto selected = std::find_if(
        drivers.begin(), drivers.end(),
        [&](const auto &driver) { return driver.id == id; });
    if (selected == drivers.end())
      throw i18n::MessageError(i18n::text(i18n::Id::SelectAValidInputDevice));
    openDriver(*selected, window);
  } catch (const std::exception &e) {
    emit channels({});
    emit error(i18n::fromException(e));
  }
}
void Controller::openDriver(const audio::DriverInfo &driver, void *window) {
  if (const auto error = capture_->close(); !error.isEmpty())
    throw i18n::MessageError(error);
  capture_ = captureApi_.create(driver.kind);
  connectCapture();
  const auto list = capture_->open(driver.id, window);
  selectedKind_ = driver.kind;
  selectedId_ = driver.id;
  window_ = window;
  emit channels(list);
  emit session_.status(i18n::text(i18n::Id::DriverLoadedReadyToStart));
}
void Controller::refreshDevices(const QString &id, void *window) {
  if (busy())
    return;
  // Enumeration failure must leave both the list and current input intact.
  try {
    drivers_ = captureApi_.devices();
  } catch (const std::exception &e) {
    emit error(i18n::fromException(e));
    return;
  }
  emit devicesRefreshed(drivers_);
  selectedId_.clear();
  try {
    const auto selected = std::find_if(
        drivers_.begin(), drivers_.end(),
        [&](const auto &driver) { return driver.id == id; });
    if (selected != drivers_.end()) {
      openDriver(*selected, window);
    } else {
      if (const auto error = capture_->close(); !error.isEmpty())
        throw i18n::MessageError(error);
      emit channels({});
      if (!id.isEmpty())
        emit error(i18n::text(i18n::Id::InputDeviceUnavailable) + "; " +
                   i18n::text(i18n::Id::SelectAValidInputDevice));
    }
  } catch (const std::exception &e) {
    emit channels({});
    emit error(i18n::fromException(e));
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
    emit error(i18n::fromException(e));
  }
}
void Controller::start(
    const Settings &settings, const QList<airplay::ReceiverEndpoint> &endpoints,
    bool saveSettings,
    const std::optional<QList<ReceiverSelection>> &selection) {
  if (busy())
    return;
  try {
    if (settings.driverId.isEmpty() || selectedId_ != settings.driverId)
      throw airplay::Error(i18n::text(i18n::Id::SelectAValidInputDevice));
    airplay::validateEndpoints(endpoints);
    airplay::NetworkRoute::resolve(settings.networkBinding);
    if (const auto error = settings.validate(); !error.isEmpty())
      throw airplay::Error(error);
    if (saveSettings)
      settings_.saveStart(settings, selection);
  } catch (const std::exception &e) {
    emit error(i18n::fromException(e));
    return;
  }
#ifdef Q_OS_MACOS
  if (selectedKind_ == audio::CaptureKind::Input) {
    const QMicrophonePermission permission;
    const auto permissionStatus = qApp->checkPermission(permission);
    if (permissionStatus == Qt::PermissionStatus::Denied) {
      emit error(i18n::text(i18n::Id::AudioInputPermissionDenied));
      return;
    }
    if (permissionStatus == Qt::PermissionStatus::Undetermined) {
      permissionPending_ = true;
      const auto revision = ++permissionRevision_;
      emit busyChanged(true);
      qApp->requestPermission(
          permission, this,
          [this, settings, endpoints, revision](const QPermission &result) {
            if (!permissionPending_ || revision != permissionRevision_)
              return;
            permissionPending_ = false;
            emit busyChanged(false);
            if (result.status() == Qt::PermissionStatus::Granted)
              start(settings, endpoints, false);
            else
              emit error(i18n::text(i18n::Id::AudioInputPermissionDenied));
          });
      return;
    }
  }
#endif
  try {
    airplay::validateEndpoints(endpoints);
    if (const auto message = settings.validate(); !message.isEmpty())
      throw airplay::Error(message);
    emit session_.status(i18n::text(i18n::Id::PreparingAudioInput));
    const auto stream = capture_->prepare(settings.left, settings.right,
                                          settings.timing.backlog);
#ifdef Q_OS_WIN
    emit session_.log(
        i18n::text(i18n::Id::CaptureTimingRequestedMsResolutionAndDisabled));
#endif
    session_.start(settings.timing, stream, endpoints, {},
                   airplay::NetworkRoute::resolve(settings.networkBinding));
  } catch (const std::exception &e) {
    stopCapture();
    emit error(i18n::fromException(e));
    emit session_.status(i18n::text(i18n::Id::PreparationFailed));
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
  const bool sessionWasBusy = session_.busy();
  session_.stop();
  if (!sessionWasBusy)
    stopCapture();
}
void Controller::volume(double db) { session_.volume(db); }
} // namespace app
