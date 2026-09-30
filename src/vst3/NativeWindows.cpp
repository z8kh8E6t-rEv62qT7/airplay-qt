#include "NativeEditor.h"
#include "NativeRuntime.h"
#include "airplay/Crypto.h"
#include "pluginterfaces/gui/iplugview.h"
#include <QWidget>
#include <QWindow>
#include <mmsystem.h>
#include <windows.h>

namespace vst3 {
namespace {
constexpr UINT invokeMessage = WM_APP + 71;
constexpr wchar_t dispatcherClass[] = L"AirPlayQt.VST3.EventDispatcher.0.1";
} // namespace
struct NativeRuntime::State {
  std::function<void()> tick;
  HWND window = nullptr;
  HMODULE module = nullptr;
  DWORD thread = GetCurrentThreadId();
  bool registered = false;
  static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM w,
                                   LPARAM l) {
    auto *self =
        reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<State *>(
          reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    if (self && message == WM_TIMER) {
      auto callback = self->tick;
      callback();
      return 0;
    }
    if (self && message == invokeMessage) {
      const auto callback = *reinterpret_cast<const std::function<void()> *>(l);
      callback();
      return 0;
    }
    return DefWindowProcW(window, message, w, l);
  }
  ~State() {
    if (window) {
      KillTimer(window, 1);
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      DestroyWindow(window);
    }
    if (registered)
      UnregisterClassW(dispatcherClass, module);
  }
};
NativeRuntime::NativeRuntime(std::function<void()> tick)
    : state_(std::make_unique<State>()) {
  state_->tick = std::move(tick);
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&State::dispatch),
                          &state_->module))
    throw airplay::Error("读取插件模块句柄失败。");
  WNDCLASSW type{};
  type.lpfnWndProc = State::dispatch;
  type.hInstance = state_->module;
  type.lpszClassName = dispatcherClass;
  if (!RegisterClassW(&type))
    throw airplay::Error("注册插件事件派发窗口失败。");
  state_->registered = true;
  state_->window =
      CreateWindowExW(0, dispatcherClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, state_->module, state_.get());
  if (!state_->window || !SetTimer(state_->window, 1, 10, nullptr))
    throw airplay::Error("创建插件事件派发窗口或计时器失败。");
}
NativeRuntime::~NativeRuntime() = default;
bool NativeRuntime::validParentThread(void *parent) {
  return parent && GetWindowThreadProcessId(static_cast<HWND>(parent),
                                            nullptr) == GetCurrentThreadId();
}
bool NativeRuntime::onThread() const {
  return state_->thread == GetCurrentThreadId();
}
void NativeRuntime::invoke(const std::function<void()> &callback) {
  if (onThread())
    callback();
  else
    SendMessageW(state_->window, invokeMessage, 0,
                 reinterpret_cast<LPARAM>(&callback));
}
void beginSessionTiming() {
  if (timeBeginPeriod(1) != TIMERR_NOERROR)
    throw airplay::Error("申请 1 ms 计时精度失败。");
}
bool endSessionTiming() { return timeEndPeriod(1) == TIMERR_NOERROR; }
struct NativeEditor::State {
  std::unique_ptr<QWindow> foreign;
  QWidget *widget = nullptr;
  HWND error = nullptr;
  ~State() {
    if (widget && widget->windowHandle())
      widget->windowHandle()->setParent(nullptr);
    if (error)
      DestroyWindow(error);
  }
};
NativeEditor::NativeEditor(void *parent, QWidget *widget, const QString &error)
    : state_(std::make_unique<State>()) {
  if (widget) {
    state_->foreign.reset(QWindow::fromWinId(reinterpret_cast<WId>(parent)));
    if (!state_->foreign)
      throw airplay::Error("Qt 无法嵌入宿主 HWND。");
    state_->widget = widget;
    widget->windowHandle()->setParent(state_->foreign.get());
  } else {
    const auto message = error.toStdWString();
    state_->error = CreateWindowExW(
        0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 12,
        600, 140, static_cast<HWND>(parent), nullptr, GetModuleHandleW(nullptr),
        nullptr);
    if (!state_->error)
      throw airplay::Error("无法创建编辑器错误提示。");
  }
}
NativeEditor::~NativeEditor() = default;
const char *NativeEditor::platformType() {
  return Steinberg::kPlatformTypeHWND;
}
bool NativeEditor::usesLogicalCoordinates() { return false; }
void NativeEditor::resize(int width, int height) {
  if (state_->widget)
    state_->widget->windowHandle()->setPosition(0, 0);
  if (state_->error)
    MoveWindow(state_->error, 12, 12, width - 24, height - 24, TRUE);
}
void NativeEditor::focus() {
  if (state_->widget)
    SetFocus(reinterpret_cast<HWND>(state_->widget->winId()));
}
} // namespace vst3
