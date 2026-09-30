#include "PluginRuntime.h"
#include <QThread>
#include <QFileInfo>
#include <QDir>
#ifdef AIRPLAY_PACKAGED_RUNTIME
#include <windows.h>
#endif

namespace vst3 {
namespace {
std::atomic<PluginRuntime *> runtime{nullptr};
std::atomic<unsigned> components{0};
QString faultText(InputFault value) {
  switch (value) {
  case InputFault::Inactive:
    return "宿主已停用处理；恢复后请手动开始。";
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
  const bool plugin = QCoreApplication::testAttribute(Qt::AA_PluginApplication);
  const bool menu = QCoreApplication::testAttribute(Qt::AA_DontUseNativeMenuBar);
  ApplicationMode() { QCoreApplication::setAttribute(Qt::AA_PluginApplication); }
  ~ApplicationMode() {
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
    QObject::connect(&session, &app::SessionController::stopped, &session,
                     [this] { release(); });
  }
  ~Instance() {
    state->input.stop();
    if (panel)
      panel->drainDiscovery();
    session.shutdown();
    release();
  }
  void release() {
    if (timer) {
      const bool restored = endSessionTiming();
      timer = false;
      if (!restored && panel)
        panel->showError("释放 1 ms 计时精度失败。");
    }
    if (runtime.sender_ == state->id)
      runtime.sender_ = 0;
  }
  void start(const app::Timing &timing,
             const QList<airplay::ReceiverEndpoint> &endpoints) {
    if (session.busy())
      return;
    try {
      if (!state->processorAlive.load())
        throw airplay::Error("音频组件已卸载。");
      if (runtime.sender_ && runtime.sender_ != state->id)
        throw airplay::Error(
            "另一个 AirPlayQt 插件实例正在发送，请先停止该实例。");
      if (const auto error = timing.validate(); !error.isEmpty())
        throw airplay::Error(error);
      airplay::validateEndpoints(endpoints);
      const auto stream = state->input.prepare(timing.backlog);
      beginSessionTiming();
      timer = true;
      runtime.sender_ = state->id;
      state->setTiming(timing);
      session.start(timing, stream, endpoints);
    } catch (const std::exception &e) {
      state->input.stop();
      release();
      if (panel)
        panel->showError(QString::fromUtf8(e.what()));
    }
  }
  void poll() {
    const auto stop = state->stopRevision.load();
    const auto timing = state->timingRevision.load();
    if (!state->processorAlive.load() || stop != stopRevision)
      session.stop();
    if (stop != stopRevision && panel)
      panel->clearReceiverSelection();
    stopRevision = stop;
    if (session.busy() && state->input.fault() != InputFault::None)
      session.stop(faultText(state->input.fault()));
    if (panel) {
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
PluginRuntime &PluginRuntime::acquire(void *parent) {
  if (!NativeRuntime::validParentThread(parent))
    throw airplay::Error("Qt 编辑器必须在宿主窗口所属 UI 线程打开。");
  if (auto *value = runtime.load()) {
    if (!value->native_->onThread())
      throw airplay::Error("插件 Qt 运行时属于另一个 UI 线程。");
    return *value;
  }
  auto *value = new PluginRuntime;
  runtime.store(value);
  return *value;
}
PluginRuntime::PluginRuntime() {
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
                           reinterpret_cast<LPCWSTR>(&PluginRuntime::exists), &module) ||
        !GetModuleFileNameW(module, path, 32768))
      throw airplay::Error("无法定位包内 Qt 平台插件。");
    const auto directory = QFileInfo(QString::fromWCharArray(path)).absolutePath();
    if (!QFileInfo::exists(directory + "/platforms/qwindows.dll"))
      throw airplay::Error("包内缺少 platforms/qwindows.dll。");
    platformPath_ = QDir::toNativeSeparators(directory + "/platforms").toLocal8Bit();
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
    if (shuttingDown_) return;
    pump();
    if (shutdownPending_) shutdown();
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
    if (auto *current = runtime.load()) current->retire(id);
  });
}
void PluginRuntime::retire(uint64_t id) {
  const auto it = instances_.find(id);
  if (it == instances_.end())
    return;
  it->second->state->input.stop();
  it->second->session.stop();
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
  QObject::connect(
      panel, &ui::StreamingPanel::startRequested, &instance->session,
      [target = instance.get(), panel] {
        target->start(panel->timing(), panel->endpoints());
      });
  QObject::connect(panel, &ui::StreamingPanel::timingChanged,
                   &instance->session,
                   [state, panel] { state->setTiming(panel->timing()); });
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
