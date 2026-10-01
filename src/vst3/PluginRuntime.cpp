#include "PluginRuntime.h"
#include "HostRecovery.h"
#include "app/Message.h"
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QThread>
#ifdef Q_OS_MACOS
#include <dlfcn.h>
#endif
#ifdef AIRPLAY_PACKAGED_RUNTIME
#include <windows.h>
#endif

namespace vst3 {
namespace {
std::atomic<PluginRuntime *> runtime{nullptr};
std::atomic<unsigned> components{0};
i18n::Message faultText(InputFault value) {
  switch (value) {
  case InputFault::SampleRate:
    return i18n::text(i18n::Id::HostSampleRateOrSampleFormatChanged);
  case InputFault::Bypass:
    return i18n::text(i18n::Id::PluginBypassedAirPlayStopped);
  case InputFault::NonRealtime:
    return i18n::text(
        i18n::Id::PreprocessingOrOfflineExportStartedAirPlayStopped);
  case InputFault::Overflow:
    return i18n::text(i18n::Id::DAWInputQueueOverflowAirPlayStopped);
  case InputFault::InvalidBlock:
    return i18n::text(i18n::Id::InvalidHostAudioBlockLengthOrFormat);
  case InputFault::NonFinite:
    return i18n::text(i18n::Id::DAWInputContainsNaNInfAirPlayStopped);
  case InputFault::StateLoad:
    return i18n::text(i18n::Id::ProjectStateLoadedSelectReceiversAndStart);
  default:
    return {};
  }
}
} // namespace
struct PluginRuntime::ApplicationMode {
#ifdef Q_OS_MACOS
  const QStringList paths = QCoreApplication::libraryPaths();
  bool changedPaths = false;
  const bool plugin = QCoreApplication::testAttribute(Qt::AA_PluginApplication);
  const bool menu =
      QCoreApplication::testAttribute(Qt::AA_DontUseNativeMenuBar);
  ApplicationMode() {
    Dl_info module{};
    if (!dladdr(reinterpret_cast<const void *>(&PluginRuntime::exists),
                &module) ||
        !module.dli_fname)
      throw airplay::Error(i18n::text(i18n::Id::CannotLocateThePluginModule));
    const auto contents = QFileInfo(QString::fromUtf8(module.dli_fname)).dir();
    const auto plugins = QDir::cleanPath(contents.filePath("../PlugIns"));
    // Development builds have no private runtime. A deployed bundle must have
    // its Cocoa backend; never silently fall back to the developer's Qt.
    if (QFileInfo::exists(contents.filePath("../Frameworks"))) {
      if (!QFileInfo::exists(plugins + "/platforms/libqcocoa.dylib"))
        throw airplay::Error(
            i18n::text(i18n::Id::PackageIsMissingPlatformsLibqcocoaDylib));
      QCoreApplication::setLibraryPaths({plugins});
      changedPaths = true;
    }
    QCoreApplication::setAttribute(Qt::AA_PluginApplication);
  }
  ~ApplicationMode() {
    // application_ is destroyed first, including Qt's default-path reset.
    if (changedPaths)
      QCoreApplication::setLibraryPaths(paths);
    QCoreApplication::setAttribute(Qt::AA_PluginApplication, plugin);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, menu);
  }
#endif
};
struct PluginRuntime::Instance {
  PluginRuntime &runtime;
  std::shared_ptr<PluginState> state;
  app::SessionController session;
  std::unique_ptr<ui::StreamingPanel> panel;
  bool attached = false, timer = false;
  HostRecovery recovery;
  app::Timing requestTiming;
  airplay::NetworkBinding requestBinding;
  airplay::NetworkRoute requestRoute;
  QList<airplay::ReceiverEndpoint> requestTargets;
  uint64_t stopRevision = 0, timingRevision = 0, languageRevision = 0;
  explicit Instance(PluginRuntime &owner, std::shared_ptr<PluginState> value)
      : runtime(owner), state(std::move(value)),
        stopRevision(state->stopRevision.load()),
        timingRevision(state->timingRevision.load()),
        languageRevision(state->languageRevision.load()) {
    session.setLanguage(state->language());
    QObject::connect(
        &session, &app::SessionController::startCapture, &session, [this] {
          if (state->processorAlive.load() && state->input.start())
            session.captureStarted();
          else
            session.stop(i18n::text(
                i18n::Id::HostAudioConditionsChangedAirPlayCannotStart));
        });
    QObject::connect(&session, &app::SessionController::stopCapture, &session,
                     [this] { state->input.stop(); });
    QObject::connect(
        &session, &app::SessionController::stopped, &session, [this] {
          if (session.endReason() != airplay::SessionEnd::HostInterrupted)
            recovery.cancel();
          release();
          poll();
        });
    QObject::connect(&session, &app::SessionController::streamingChanged,
                     &session, [this](bool active) {
                       if (active)
                         recovery.streaming();
                     });
  }
  ~Instance() {
    recovery.cancel();
    state->input.stop();
    if (panel)
      panel->drainDiscovery();
    session.shutdown();
    release();
  }
  void release() {
    if (timer && !session.busy()) {
      const bool restored = endSessionTiming();
      timer = false;
      if (!restored && panel)
        panel->showError(
            i18n::text(i18n::Id::FailedToReleaseMsTimerResolution));
    }
    if (runtime.sender_ == state->id && !recovery.active() && !session.busy())
      runtime.sender_ = 0;
  }
  void cancel(const i18n::Message &reason = {}) {
    recovery.cancel();
    state->input.stop();
    if (panel)
      panel->setRecoveryPending(false);
    if (session.busy())
      session.stop(reason);
    else {
      emit session.status(reason.isEmpty()
                              ? i18n::text(i18n::Id::Stopped)
                              : i18n::text(i18n::Id::ErrorPrefix) + reason);
      if (!reason.isEmpty())
        emit session.log(i18n::text(i18n::Id::ErrorPrefix) + reason);
    }
    release();
  }
  void start(const app::Timing &timing,
             const QList<airplay::ReceiverEndpoint> &endpoints,
             const airplay::NetworkBinding &binding) {
    if (session.busy() || recovery.active())
      return;
    requestBinding = binding;
    requestTiming = timing;
    requestTargets = endpoints;
    recovery.start();
    begin(false);
  }
  void begin(bool reconnect) {
    try {
      if (const auto error = state->configurationError(); !error.isEmpty())
        throw airplay::Error(error);
      if (reconnect)
        requestRoute.validate();
      else
        requestRoute = airplay::NetworkRoute::resolve(requestBinding);
      if (!state->processorAlive.load())
        throw airplay::Error(
            i18n::text(i18n::Id::AudioComponentHasBeenUnloaded));
      if (runtime.sender_ && runtime.sender_ != state->id)
        throw airplay::Error(
            i18n::text(i18n::Id::AnotherAirPlayQtInstanceIsStreamingStopThat));
      if (const auto error = requestTiming.validate(); !error.isEmpty())
        throw airplay::Error(error);
      airplay::validateEndpoints(requestTargets);
      if (reconnect && (state->input.fault() != InputFault::None ||
                        !state->input.unavailable().isEmpty()))
        throw airplay::Error(
            i18n::text(i18n::Id::HostAudioConditionsChangedStartManually));
      const auto stream =
          state->input.prepare(requestTiming.backlog, reconnect);
      beginSessionTiming();
      timer = true;
      runtime.sender_ = state->id;
      state->setTiming(requestTiming);
      if (panel)
        panel->setRecoveryPending(false);
      if (reconnect)
        emit session.log(i18n::text(
            i18n::Id::HostRecoveredRebuildingTheAirPlaySessionAutomatically));
      session.start(requestTiming, stream, requestTargets, runtime.environment_,
                    requestRoute);
    } catch (const std::exception &e) {
      cancel(i18n::fromException(e));
    }
  }
  void poll() {
    const auto language = state->languageRevision.load();
    if (language != languageRevision) {
      // Restoring host state is not a user edit and must not dirty the project.
      if (panel) {
        const QSignalBlocker blocker(panel.get());
        panel->setLanguage(state->language());
      } else {
        session.setLanguage(state->language());
      }
      languageRevision = language;
    }
    const auto stop = state->stopRevision.load();
    const auto timing = state->timingRevision.load();
    if (!state->processorAlive.load() || stop != stopRevision)
      cancel(state->configurationError());
    if (recovery.waiting() && !requestBinding.automatic()) {
      try {
        requestRoute.validate();
      } catch (const std::exception &e) {
        cancel(i18n::fromException(e));
      }
    }
    if (stop != stopRevision && panel)
      panel->clearReceiverSelection();
    stopRevision = stop;
    const auto fault = state->input.fault();
    switch (recovery.update(
        state->input.interruption(), state->input.unavailable().isEmpty(),
        !session.busy(), fault != InputFault::None, monotonicNs())) {
    case HostRecovery::Action::Interrupt:
      emit session.log(
          i18n::text(i18n::Id::HostAudioProcessingPausedWaitingUpTo));
      session.stop({}, airplay::SessionEnd::HostInterrupted);
      break;
    case HostRecovery::Action::Reconnect:
      begin(true);
      break;
    case HostRecovery::Action::Cancel:
      cancel(fault != InputFault::None
                 ? faultText(fault)
                 : i18n::text(i18n::Id::HostDidNotRecoverWithinSecondsOr));
      break;
    case HostRecovery::Action::None:
      break;
    }
    if (panel) {
      panel->setRecoveryPending(recovery.waiting());
      if (timing != timingRevision) {
        panel->setTiming(state->timing());
        panel->setNetworkBinding(state->networkBinding());
      }
      panel->setUnavailable(
          state->processorAlive.load()
              ? (state->configurationError().isEmpty()
                     ? state->input.unavailable()
                     : state->configurationError())
              : i18n::text(i18n::Id::AudioComponentHasBeenUnloaded));
      if (!attached && !panel->discoveryBusy())
        panel.reset();
    }
    timingRevision = timing;
  }
};
void PluginRuntime::addComponent() noexcept { ++components; }
void PluginRuntime::removeComponent() noexcept {
  if (components.fetch_sub(1) == 1)
    shutdown();
}
bool PluginRuntime::exists() noexcept { return runtime.load() != nullptr; }
bool PluginRuntime::prepareUnload() noexcept {
  if (components.load() != 0)
    return false;
  shutdown();
  return !exists();
}
PluginRuntime &PluginRuntime::acquire(void *parent,
                                      airplay::SessionEnvironment environment) {
  if (!NativeRuntime::validParentThread(parent))
    throw airplay::Error(i18n::text(i18n::Id::QtEditorMustOpenOnTheHost));
  if (auto *value = runtime.load()) {
    if (!value->native_->onThread())
      throw airplay::Error(
          i18n::text(i18n::Id::PluginQtRuntimeBelongsToADifferent));
    return *value;
  }
  auto *value = new PluginRuntime(std::move(environment));
  runtime.store(value);
  return *value;
}
PluginRuntime::PluginRuntime(airplay::SessionEnvironment environment)
    : environment_(std::move(environment)) {
  if (auto *existing = QCoreApplication::instance()) {
    if (!qobject_cast<QApplication *>(existing) ||
        existing->thread() != QThread::currentThread())
      throw airplay::Error(
          i18n::text(i18n::Id::HostHasAnIncompatibleQtApplicationOr));
  } else {
#ifdef AIRPLAY_PACKAGED_RUNTIME
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PluginRuntime::exists),
                            &module) ||
        !GetModuleFileNameW(module, path, 32768))
      throw airplay::Error(
          i18n::text(i18n::Id::CannotLocateThePackagedQtPlatformPlugin));
    const auto directory =
        QFileInfo(QString::fromWCharArray(path)).absolutePath();
    if (!QFileInfo::exists(directory + "/platforms/qwindows.dll"))
      throw airplay::Error(
          i18n::text(i18n::Id::PackageIsMissingPlatformsQwindowsDll));
    platformPath_ =
        QDir::toNativeSeparators(directory + "/platforms").toLocal8Bit();
    // The Qt build's compiled-in development path must not win over this
    // package. This branch owns the application; borrowed applications retain
    // their existing library paths untouched.
    QCoreApplication::setLibraryPaths({directory});
    argc_ = 3;
    argv_[1] = platformOption_;
    argv_[2] = platformPath_.data();
#endif
    applicationMode_ = std::make_unique<ApplicationMode>();
    application_ = std::make_unique<QApplication>(argc_, argv_);
    QApplication::setStyle("Fusion");
    application_->setQuitOnLastWindowClosed(false);
  }
  native_ = std::make_unique<NativeRuntime>([this] {
    if (shuttingDown_)
      return;
    pump();
    if (shutdownPending_)
      shutdown();
  });
}
PluginRuntime::~PluginRuntime() {
  shuttingDown_ = true;
  native_.reset();
  instances_.clear();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void PluginRuntime::shutdown() noexcept {
  auto *value = runtime.load();
  if (!value)
    return;
  if (!value->native_->onThread()) {
    value->native_->invoke([] { shutdown(); });
    return;
  }
  if (value->pumping_) {
    value->shutdownPending_ = true;
    return;
  }
  runtime.store(nullptr);
  delete value;
}
void PluginRuntime::processorRemoved(uint64_t id) noexcept {
  auto *value = runtime.load();
  if (!value)
    return;
  value->native_->invoke([id] {
    if (auto *current = runtime.load())
      current->retire(id);
  });
}
void PluginRuntime::retire(uint64_t id) {
  const auto it = instances_.find(id);
  if (it == instances_.end())
    return;
  it->second->cancel();
}
void PluginRuntime::pump() {
  if (pumping_)
    return;
  pumping_ = true;
  // Native dispatch belongs to the host. Only drain Qt posted events here;
  // entering another native loop could re-enter host unload from this stack.
  QCoreApplication::sendPostedEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  for (auto it = instances_.begin(); it != instances_.end();) {
    it->second->poll();
    if (!it->second->state->processorAlive.load() && !it->second->attached &&
        !it->second->session.busy() && !it->second->panel)
      it = instances_.erase(it);
    else
      ++it;
  }
  pumping_ = false;
}
ui::StreamingPanel *
PluginRuntime::open(const std::shared_ptr<PluginState> &state,
                    const airplay::DiscoveryApi &api) {
  if (!state || !state->processorAlive.load())
    throw airplay::Error(
        i18n::text(i18n::Id::NoValidAudioComponentIsAssociatedYet));
  auto &instance = instances_[state->id];
  if (!instance)
    instance = std::make_unique<Instance>(*this, state);
  if (instance->panel)
    throw airplay::Error(
        i18n::text(i18n::Id::ThisInstanceAlreadyHasAnEditorOr));
  instance->session.setLanguage(state->language());
  instance->panel = std::make_unique<ui::StreamingPanel>(
      instance->session, nullptr, std::move(api));
  instance->attached = true;
  auto *panel = instance->panel.get();
  QObject::connect(panel, &ui::StreamingPanel::languageChanged,
                   &instance->session,
                   [state, panel] { state->setLanguage(panel->language()); });
  panel->setTiming(state->timing());
  panel->setNetworkBinding(state->networkBinding());
  panel->setUnavailable(state->configurationError().isEmpty()
                            ? state->input.unavailable()
                            : state->configurationError());
  panel->setRecoveryPending(instance->recovery.waiting());
  QObject::connect(panel, &ui::StreamingPanel::startRequested,
                   &instance->session, [target = instance.get(), panel] {
                     target->start(panel->timing(), panel->endpoints(),
                                   panel->networkBinding());
                   });
  QObject::connect(
      panel, &ui::StreamingPanel::networkBindingChanged, &instance->session,
      [state, panel] { state->setNetworkBinding(panel->networkBinding()); });
  QObject::connect(panel, &ui::StreamingPanel::timingChanged,
                   &instance->session,
                   [state, panel] { state->setTiming(panel->timing()); });
  QObject::connect(panel, &ui::StreamingPanel::stopRequested,
                   &instance->session,
                   [target = instance.get()] { target->cancel(); });
  panel->beginDiscovery();
  return panel;
}
void PluginRuntime::close(uint64_t id) {
  const auto it = instances_.find(id);
  if (it == instances_.end() || !it->second->panel)
    return;
  auto &instance = *it->second;
  instance.attached = false;
  instance.session.setTelemetryEnabled(false);
  instance.panel->hide();
  instance.panel->cancelDiscovery();
  if (!instance.panel->discoveryBusy())
    instance.panel.reset();
}
} // namespace vst3
