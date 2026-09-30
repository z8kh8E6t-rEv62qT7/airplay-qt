#include "NativeEditor.h"
#include "NativeRuntime.h"
#include "airplay/Crypto.h"
#include "pluginterfaces/gui/iplugview.h"
#import <Cocoa/Cocoa.h>
#include <QWidget>
#include <QWindow>
#include <dispatch/dispatch.h>

namespace vst3 {
struct NativeRuntime::State {
  std::function<void()> tick;
  CFRunLoopTimerRef timer = nullptr;
  ~State() {
    if (timer) {
      CFRunLoopTimerInvalidate(timer);
      CFRelease(timer);
    }
  }
};
NativeRuntime::NativeRuntime(std::function<void()> tick)
    : state_(std::make_unique<State>()) {
  state_->tick = std::move(tick);
  CFRunLoopTimerContext context{0, state_.get(), nullptr, nullptr, nullptr};
  state_->timer = CFRunLoopTimerCreate(
      kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + .01, .01, 0, 0,
      [](CFRunLoopTimerRef, void *context) {
        @autoreleasepool {
          // Tick may delete the runtime. Do not access its state after
          // invocation.
          auto callback = static_cast<State *>(context)->tick;
          callback();
        }
      },
      &context);
  if (!state_->timer)
    throw airplay::Error("创建插件事件计时器失败");
  CFRunLoopAddTimer(CFRunLoopGetMain(), state_->timer, kCFRunLoopCommonModes);
}
NativeRuntime::~NativeRuntime() = default;
bool NativeRuntime::validParentThread(void *parent) {
  return parent && [NSThread isMainThread] &&
         [(id)parent isKindOfClass:[NSView class]];
}
bool NativeRuntime::onThread() const { return [NSThread isMainThread]; }
void NativeRuntime::invoke(const std::function<void()> &callback) {
  if ([NSThread isMainThread])
    callback();
  else
    dispatch_sync(dispatch_get_main_queue(), ^{
      callback();
    });
}
void beginSessionTiming() {}
bool endSessionTiming() { return true; }
struct NativeEditor::State {
  NSView *parent = nil;
  NSView *view = nil;
  bool error = false;
  ~State() {
    [view removeFromSuperview];
    if (error)
      [view release];
  }
};
NativeEditor::NativeEditor(void *parent, QWidget *widget, const QString &error)
    : state_(std::make_unique<State>()) {
  if (!NativeRuntime::validParentThread(parent))
    throw airplay::Error("编辑器需要主线程 NSView");
  state_->parent = (NSView *)parent;
  if (widget)
    state_->view = (NSView *)widget->winId();
  else {
    auto *label =
        [[NSTextField alloc] initWithFrame:NSMakeRect(12, 12, 600, 140)];
    [label setStringValue:error.toNSString()];
    [label setEditable:NO];
    [label setSelectable:YES];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    state_->view = label;
    state_->error = true;
  }
  [state_->parent addSubview:state_->view];
  [state_->view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
}
NativeEditor::~NativeEditor() = default;
const char *NativeEditor::platformType() {
  return Steinberg::kPlatformTypeNSView;
}
bool NativeEditor::usesLogicalCoordinates() { return true; }
void NativeEditor::resize(int width, int height) {
  const int margin = state_->error ? 12 : 0;
  [state_->view
      setFrame:NSMakeRect(margin, margin, std::max(0, width - 2 * margin),
                          std::max(0, height - 2 * margin))];
}
void NativeEditor::focus() {
  [[state_->parent window] makeFirstResponder:state_->view];
}
} // namespace vst3
