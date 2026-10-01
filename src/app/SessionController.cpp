#include "SessionController.h"
#include "app/Message.h"
#include <QEventLoop>

namespace app {
struct SessionController::NetworkContext {
  airplay::AirPlaySession *session = nullptr;
};
SessionController::SessionController(QObject *parent)
    : QObject(parent), network_(std::make_shared<NetworkContext>()),
      worker_(new QObject) {
  worker_->moveToThread(&thread_);
  connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(this, &SessionController::status, this,
          [this](i18n::Message text) { status_ = text; });
  connect(this, &SessionController::group, this,
          [this](i18n::Message text) { group_ = text; });
  connect(this, &SessionController::log, this, [this](i18n::Message text) {
    log_.append(text.render(language_));
    if (log_.size() > 1000)
      log_.removeFirst();
  });
  connect(this, &SessionController::volumeApplied, this, [this](double db) {
    volume_ = db;
    if (db > -144)
      restoreVolume_ = db;
  });
  connect(this, &SessionController::streamingChanged, this,
          [this](bool value) { streaming_ = value; });
  thread_.start();
}
SessionController::~SessionController() { shutdown(); }
void SessionController::shutdown() {
  if (!thread_.isRunning())
    return;
  // Run bounded TEARDOWN while the network event loop still exists. Never
  // wait for UI-thread delivery to finish shutdown.
  QMetaObject::invokeMethod(
      worker_,
      [context = network_, this] {
        if (auto *session = context->session) {
          session->disconnect(this);
          QEventLoop drained;
          connect(session, &airplay::AirPlaySession::finished, &drained,
                  &QEventLoop::quit);
          session->stop();
          if (context->session)
            drained.exec();
        }
      },
      Qt::BlockingQueuedConnection);
  thread_.quit();
  thread_.wait();
  busy_ = streaming_ = false;
}
void SessionController::start(const Timing &timing, audio::CaptureStream stream,
                              const QList<airplay::ReceiverEndpoint> &endpoints,
                              airplay::SessionEnvironment environment,
                              airplay::NetworkRoute route) {
  if (busy_)
    return;
  airplay::validateEndpoints(endpoints);
  if (const auto error = timing.validate(); !error.isEmpty())
    throw airplay::Error(error);
  if (!stream.queue || stream.blockFrames <= 0)
    throw airplay::Error(i18n::text(i18n::Id::AudioInputIsNotReady));
  busy_ = true;
  stopping_ = false;
  endReason_ = airplay::SessionEnd::Stopped;
  const auto generation = ++generation_;
  emit busyChanged(true);
  if (timing.lead < .5)
    emit log(i18n::text(i18n::Id::LowPlaybackLeadMayCauseLateArrival));
  QMetaObject::invokeMethod(worker_, [this, context = network_, timing, stream,
                                      endpoints, generation, environment,
                                      route] {
    auto *session = new airplay::AirPlaySession(timing, stream, endpoints,
                                                worker_, environment, route);
    context->session = session;
    // Every delivery is tagged; events from a prior session cannot update a
    // replacement session or reopen its producer gate.
    const auto forward = [this, session, generation](auto source, auto target) {
      connect(session, source, this, [this, generation, target](auto... args) {
        if (generation == generation_)
          (this->*target)(args...);
      });
    };
    forward(&airplay::AirPlaySession::status, &SessionController::status);
    forward(&airplay::AirPlaySession::log, &SessionController::log);
    forward(&airplay::AirPlaySession::group, &SessionController::group);
    forward(&airplay::AirPlaySession::streamingChanged,
            &SessionController::streamingChanged);
    forward(&airplay::AirPlaySession::volumeApplied,
            &SessionController::volumeApplied);
    forward(&airplay::AirPlaySession::telemetry, &SessionController::telemetry);
    forward(&airplay::AirPlaySession::stopCapture,
            &SessionController::stopCapture);
    connect(session, &airplay::AirPlaySession::startCapture, this,
            [this, generation] {
              if (generation == generation_ && busy_ && !stopping_)
                emit startCapture();
            });
    connect(session, &airplay::AirPlaySession::finished, this,
            [this, generation](i18n::Message message, int reason) {
              if (generation != generation_)
                return;
              emit stopCapture();
              endReason_ = airplay::SessionEnd(reason);
              busy_ = false;
              emit busyChanged(false);
              emit streamingChanged(false);
              if (!message.isEmpty())
                emit error(message);
              emit stopped();
            });
    connect(session, &airplay::AirPlaySession::finished, worker_,
            [context, session] {
              context->session = nullptr;
              session->deleteLater();
            });
    session->start();
  });
}
void SessionController::captureStarted() {
  if (!busy_ || stopping_)
    return;
  QMetaObject::invokeMethod(worker_, [context = network_] {
    if (context->session)
      context->session->captureStarted();
  });
}
void SessionController::stop(const i18n::Message &reason,
                             airplay::SessionEnd end) {
  if (!busy_)
    return;
  stopping_ = true;
  emit stopCapture();
  QMetaObject::invokeMethod(worker_, [context = network_, reason, end] {
    if (context->session)
      context->session->stop(reason, end);
  });
}
void SessionController::volume(double db) {
  if (!busy_ || stopping_)
    return;
  QMetaObject::invokeMethod(worker_, [context = network_, db] {
    if (context->session)
      context->session->volume(db);
  });
}
} // namespace app
