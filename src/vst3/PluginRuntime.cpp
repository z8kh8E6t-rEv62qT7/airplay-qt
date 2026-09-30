#include "PluginRuntime.h"
#include "HostRecovery.h"
#include <QDir>
#include <QFileInfo>
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
QString faultText(InputFault value) {
  switch (value) {
  case InputFault::SampleRate:
    return "宿主采样率或样本格式改变，已停止 AirPlay。";
  case InputFault::Bypass:
    return "插件已旁路，已停止 AirPlay。";
  case InputFault::NonRealtime:
    return "进入预处理／离线导出，已停止 AirPlay。";
  case InputFault::Overflow:
    return "DAW 输入队列溢出，已停止 AirPlay。";
  case InputFault::InvalidBlock:
    return "宿主音频块长度或格式无效，已停止 AirPlay。";
  case InputFault::NonFinite:
    return "DAW 输入含 NaN/Inf，已停止 AirPlay。";
  case InputFault::StateLoad:
    return "工程状态已载入，请重新选择并手动开始。";
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
      throw airplay::Error("无法定位插件模块。");
    const auto contents = QFileInfo(QString::fromUtf8(module.dli_fname)).dir();
    const auto plugins = QDir::cleanPath(contents.filePath("../PlugIns"));
    // Development builds have no private runtime. A deployed bundle must have
    // its Cocoa backend; never silently fall back to the developer's Qt.
    if (QFileInfo::exists(contents.filePath("../Frameworks"))) {
      if (!QFileInfo::exists(plugins + "/platforms/libqcocoa.dylib"))
        throw airplay::Error("包内缺少 platforms/libqcocoa.dylib。");
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
  QList<airplay::ReceiverEndpoint> requestTargets;
  uint64_t stopRevision = 0, timingRevision = 0;
  explicit Instance(PluginRuntime &owner, std::shared_ptr<PluginState> value)
      : runtime(owner), state(std::move(value)),
        stopRevision(state->stopRevision.load()),
        timingRevision(state->timingRevision.load()) {
    QObject::connect(&session, &app::SessionController::startCapture, &session,
                     [this] {
                       if (state->processorAlive.load() && state->input.start())
                         session.captureStarted();
                       else
                         session.stop("宿主音频条件已变化，不能开始 AirPlay。");
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
        panel->showError("释放 1 ms 计时精度失败。");
    }
    if (runtime.sender_ == state->id && !recovery.active() && !session.busy())
      runtime.sender_ = 0;
  }
  void cancel(const QString &reason = {}) {
    recovery.cancel();
    state->input.stop();
    if (panel)
      panel->setRecoveryPending(false);
    if (session.busy())
      session.stop(reason);
    else {
      emit session.status(reason.isEmpty() ? "已停止" : "错误：" + reason);
      if (!reason.isEmpty())
        emit session.log("错误：" + reason);
    }
    release();
  }
  void start(const app::Timing &timing,
             const QList<airplay::ReceiverEndpoint> &endpoints) {
    if (session.busy() || recovery.active())
      return;
    requestTiming = timing;
    requestTargets = endpoints;
    recovery.start();
    begin(false);
  }
  void begin(bool reconnect) {
    try {
      if (!state->processorAlive.load())
        throw airplay::Error("音频组件已卸载。");
      if (runtime.sender_ && runtime.sender_ != state->id)
        throw airplay::Error(
            "另一个 AirPlayQt 插件实例正在发送，请先停止该实例。");
      if (const auto error = requestTiming.validate(); !error.isEmpty())
        throw airplay::Error(error);
      airplay::validateEndpoints(requestTargets);
      if (reconnect && (state->input.fault() != InputFault::None ||
                        !state->input.unavailable().isEmpty()))
        throw airplay::Error("宿主音频条件已变化，请手动开始。");
      const auto stream =
          state->input.prepare(requestTiming.backlog, reconnect);
      beginSessionTiming();
      timer = true;
      runtime.sender_ = state->id;
      state->setTiming(requestTiming);
      if (panel)
        panel->setRecoveryPending(false);
      if (reconnect)
        emit session.log("宿主已恢复，自动重建 AirPlay 会话（一次）");
      session.start(requestTiming, stream, requestTargets,
                    runtime.environment_);
    } catch (const std::exception &e) {
      cancel(QString::fromUtf8(e.what()));
    }
  }
  void poll() {
    const auto stop = state->stopRevision.load();
    const auto timing = state->timingRevision.load();
    if (!state->processorAlive.load() || stop != stopRevision)
      cancel();
    if (stop != stopRevision && panel)
      panel->clearReceiverSelection();
    stopRevision = stop;
    const auto fault = state->input.fault();
    switch (recovery.update(
        state->input.interruption(), state->input.unavailable().isEmpty(),
        !session.busy(), fault != InputFault::None, monotonicNs())) {
    case HostRecovery::Action::Interrupt:
      emit session.log("宿主暂停音频处理，等待恢复（最多 5 秒）");
      session.stop({}, airplay::SessionEnd::HostInterrupted);
      break;
    case HostRecovery::Action::Reconnect:
      begin(true);
      break;
    case HostRecovery::Action::Cancel:
      cancel(
          fault != InputFault::None
              ? faultText(fault)
              : "宿主未满足 5 秒自动恢复条件，或重连中再次中断；请手动开始。");
      break;
    case HostRecovery::Action::None:
      break;
    }
    if (panel) {
      panel->setRecoveryPending(recovery.waiting());
      if (timing != timingRevision)
        panel->setTiming(state->timing());
      panel->setUnavailable(state->processorAlive.load()
                                ? state->input.unavailable()
                                : "音频组件已卸载。");
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
    throw airplay::Error("Qt 编辑器必须在宿主窗口所属 UI 线程打开。");
  if (auto *value = runtime.load()) {
    if (!value->native_->onThread())
      throw airplay::Error("插件 Qt 运行时属于另一个 UI 线程。");
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
          "宿主已有不兼容的 Qt 应用或 Qt UI 线程；音频仍原样透传。");
  } else {
#ifdef AIRPLAY_PACKAGED_RUNTIME
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PluginRuntime::exists),
                            &module) ||
        !GetModuleFileNameW(module, path, 32768))
      throw airplay::Error("无法定位包内 Qt 平台插件。");
    const auto directory =
        QFileInfo(QString::fromWCharArray(path)).absolutePath();
    if (!QFileInfo::exists(directory + "/platforms/qwindows.dll"))
      throw airplay::Error("包内缺少 platforms/qwindows.dll。");
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
    throw airplay::Error("尚未关联有效音频组件。");
  auto &instance = instances_[state->id];
  if (!instance)
    instance = std::make_unique<Instance>(*this, state);
  if (instance->panel)
    throw airplay::Error("此实例已有编辑器或正在等待发现取消，请稍后重试。");
  instance->panel = std::make_unique<ui::StreamingPanel>(
      instance->session, nullptr, std::move(api));
  instance->attached = true;
  auto *panel = instance->panel.get();
  panel->setTiming(state->timing());
  panel->setUnavailable(state->input.unavailable());
  panel->setRecoveryPending(instance->recovery.waiting());
  QObject::connect(panel, &ui::StreamingPanel::startRequested,
                   &instance->session, [target = instance.get(), panel] {
                     target->start(panel->timing(), panel->endpoints());
                   });
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
  instance.panel->hide();
  instance.panel->cancelDiscovery();
  if (!instance.panel->discoveryBusy())
    instance.panel.reset();
}
} // namespace vst3
