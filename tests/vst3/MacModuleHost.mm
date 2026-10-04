#include "TestComponentHandler.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "vst3/Plugin.h"
#import <Cocoa/Cocoa.h>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMetaMethod>
#include <QPushButton>
#include <QSplitter>
#include <QWidget>
#include <cstdio>
#include <dlfcn.h>

using namespace Steinberg;
using namespace Steinberg::Vst;
#define REQUIRE(value)                                                         \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #value);          \
      return 1;                                                                \
    }                                                                          \
  } while (false)
namespace {
class ResizeFrame final : public IPlugFrame {
public:
  bool reject = false;
  int calls = 0;
  tresult PLUGIN_API queryInterface(const TUID, void **object) override {
    *object = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *size) override {
    ++calls;
    return reject ? kResultFalse : view->onSize(size);
  }
};
QWidget *embedded(NSView *parent) {
  for (NSView *child in [parent subviews]) {
    if (auto *widget = QWidget::find(reinterpret_cast<WId>(child)))
      return widget;
    if (auto *widget = embedded(child))
      return widget;
  }
  return nullptr;
}
void pump() {
  for (int i = 0; i < 5; ++i) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, false);
    if (QCoreApplication::instance())
      QCoreApplication::sendPostedEvents();
  }
}
QObject *objectOfClass(QObject *root, const char *name) {
  if (std::strcmp(root->metaObject()->className(), name) == 0)
    return root;
  for (auto *child : root->children())
    if (auto *result = objectOfClass(child, name))
      return result;
  return nullptr;
}
bool businessTypesUnregistered() {
  for (const char *name :
       {"airplay::ReceiverEndpoint", "app::Timing",
        "QList<airplay::ReceiverEndpoint>", "i18n::Message", "i18n::Language"})
    if (QMetaType::fromName(name).isValid())
      return false;
  return true;
}
int exercisePanel(QWidget *widget) {
  auto *panel = objectOfClass(widget, "ui::StreamingPanel");
  auto *discovery = objectOfClass(widget, "airplay::ReceiverDiscovery");
  REQUIRE(panel && discovery);
  // Inspect signal arguments as a Qt host may do. Only Qt-owned builtins
  // may enter the process-wide registry from these plugin interfaces.
  for (auto *object : {panel, discovery}) {
    const auto *meta = object->metaObject();
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
      const auto method = meta->method(i);
      for (int p = 0; p < method.parameterCount(); ++p)
        REQUIRE(method.parameterMetaType(p).id() < QMetaType::User);
    }
  }
  auto *list = widget->findChild<QListWidget *>("receivers");
  auto *summary = widget->findChild<QLabel *>("receiverSummary");
  REQUIRE(list && summary);
  REQUIRE(
      QMetaObject::invokeMethod(discovery, "cleared", Qt::DirectConnection));
  REQUIRE(list->count() == 0);
  for (int i = 1; i <= 3; ++i) {
    const QString endpoint = QString("192.0.2.%1:7001").arg(i);
    REQUIRE(QMetaObject::invokeMethod(discovery, "found", Qt::DirectConnection,
                                      Q_ARG(QString, QString("Test receiver")),
                                      Q_ARG(QString, endpoint)));
    REQUIRE(list->count() == i);
    REQUIRE(list->item(i - 1)->data(Qt::UserRole).metaType().id() ==
            QMetaType::QString);
    REQUIRE(list->item(i - 1)->data(Qt::UserRole).toString() == endpoint);
  }
  list->item(1)->setCheckState(Qt::Checked);
  list->item(0)->setCheckState(Qt::Checked);
  REQUIRE(summary->text().startsWith("192.0.2.1:7001 + 192.0.2.2:7001"));
  list->item(2)->setCheckState(Qt::Checked);
  REQUIRE(list->item(2)->data(Qt::CheckStateRole).toInt() == Qt::Unchecked);
  // The audio processor remains inactive: exercise the start notification
  // and validation, but never connect to a receiver or send audio.
  REQUIRE(
      QMetaObject::invokeMethod(panel, "startRequested", Qt::DirectConnection));
  const auto spins = widget->findChildren<QDoubleSpinBox *>();
  REQUIRE(!spins.empty());
  spins.first()->stepUp();
  REQUIRE(
      QMetaObject::invokeMethod(panel, "timingChanged", Qt::DirectConnection));
  REQUIRE(businessTypesUnregistered());
  REQUIRE(
      QMetaObject::invokeMethod(discovery, "cleared", Qt::DirectConnection));
  REQUIRE(list->count() == 0);
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  @autoreleasepool {
    REQUIRE(argc >= 3);
    [NSApplication sharedApplication];
    const bool borrowed = std::strcmp(argv[2], "borrowed") == 0;
    const bool incompatible = std::strcmp(argv[2], "incompatible") == 0;
    const bool missingPlatform = std::strcmp(argv[2], "missing-platform") == 0;
    std::unique_ptr<QCoreApplication> application;
    if (borrowed)
      application = std::make_unique<QApplication>(argc, argv);
    if (incompatible)
      application = std::make_unique<QCoreApplication>(argc, argv);
    const auto originalPaths = QCoreApplication::libraryPaths();
    const bool originalPlugin =
        QCoreApplication::testAttribute(Qt::AA_PluginApplication);
    const bool originalMenu =
        QCoreApplication::testAttribute(Qt::AA_DontUseNativeMenuBar);
    id originalDelegate = [NSApp delegate];
    NSMenu *originalMainMenu = [NSApp mainMenu];
    HostApplication host;
    TestComponentHandler handler;
    const auto bundlePath =
        QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    for (int cycle = 0; cycle < 5; ++cycle) {
      CFStringRef bundleString = bundlePath.toCFString();
      CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, bundleString,
                                                   kCFURLPOSIXPathStyle, true);
      CFRelease(bundleString);
      REQUIRE(url);
      CFBundleRef bundle = CFBundleCreate(nullptr, url);
      CFRelease(url);
      REQUIRE(bundle);
      CFURLRef executable = CFBundleCopyExecutableURL(bundle);
      REQUIRE(executable);
      char path[4096];
      REQUIRE(CFURLGetFileSystemRepresentation(
          executable, true, reinterpret_cast<UInt8 *>(path), sizeof(path)));
      CFRelease(executable);
      void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
      if (!module) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 2;
      }
      auto entry =
          reinterpret_cast<bool (*)(CFBundleRef)>(dlsym(module, "bundleEntry"));
      auto exit = reinterpret_cast<bool (*)()>(dlsym(module, "bundleExit"));
      auto factoryFunction = reinterpret_cast<IPluginFactory *(PLUGIN_API *)()>(
          dlsym(module, "GetPluginFactory"));
      REQUIRE(entry && exit && factoryFunction && entry(bundle));
      auto factory = owned(factoryFunction());
      IComponent *rawComponent = nullptr;
      IEditController *rawController = nullptr;
      REQUIRE(factory->createInstance(
                  vst3::processorId, IComponent::iid,
                  reinterpret_cast<void **>(&rawComponent)) == kResultOk);
      REQUIRE(factory->createInstance(
                  vst3::controllerId, IEditController::iid,
                  reinterpret_cast<void **>(&rawController)) == kResultOk);
      auto component = owned(rawComponent);
      auto controller = owned(rawController);
      REQUIRE(component->initialize(&host) == kResultOk);
      REQUIRE(controller->initialize(&host) == kResultOk);
      REQUIRE(controller->setComponentHandler(&handler) == kResultOk);
      FUnknownPtr<IConnectionPoint> pc(component), cc(controller);
      REQUIRE(pc->connect(cc) == kResultOk);
      REQUIRE(cc->connect(pc) == kResultOk);
      NSWindow *window =
          [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1000, 1000)
                                      styleMask:NSWindowStyleMaskTitled
                                        backing:NSBackingStoreBuffered
                                          defer:NO];
      [window setReleasedWhenClosed:NO];
      auto view = owned(controller->createView(ViewType::kEditor));
      REQUIRE(view && QCoreApplication::instance() == application.get());
      ViewRect initial;
      REQUIRE(view->getSize(&initial) == kResultOk);
      REQUIRE(initial.getWidth() == 950 && initial.getHeight() == 880);
      REQUIRE(view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultFalse);
      QList<int> savedParts;
      for (int reopen = 0; reopen < 2; ++reopen) {
        REQUIRE(view->attached([window contentView], kPlatformTypeNSView) ==
                kResultOk);
        QWidget *widget = embedded([window contentView]);
        REQUIRE((incompatible || missingPlatform) ? !widget
                                                  : widget != nullptr);
        if (widget && reopen == 1) {
          pump();
          auto *log = widget->findChild<QWidget *>("sessionLog");
          REQUIRE(log && log->isHidden());
          widget->findChild<QPushButton *>("toggleLog")->click();
          pump();
          REQUIRE(widget->findChild<QSplitter *>("streamingColumns")->sizes() == savedParts);
        }
        ViewRect rect{0, 0, 900, 880};
        REQUIRE(view->onSize(&rect) == kResultOk);
        REQUIRE(view->onFocus(true) == kResultOk);
        FUnknownPtr<IPlugViewContentScaleSupport> scale(view);
        REQUIRE(scale && scale->setContentScaleFactor(2) == kResultTrue);
        ViewRect after;
        REQUIRE(view->getSize(&after) == kResultOk);
        REQUIRE(after.getWidth() == 900 && after.getHeight() == 880);
        if (widget)
          REQUIRE(widget->width() == 900 && widget->height() == 880);
        if (widget) {
          ResizeFrame frame;
          REQUIRE(view->setFrame(&frame) == kResultOk);
          auto *logToggle = widget->findChild<QPushButton *>("toggleLog");
          auto *log = widget->findChild<QWidget *>("sessionLog");
          REQUIRE(logToggle && log);
          pump();
          logToggle->click();
          pump();
          ViewRect compact;
          REQUIRE(view->getSize(&compact) == kResultOk);
          REQUIRE(frame.calls == 1 && log->isHidden());
          REQUIRE(compact.getWidth() < after.getWidth());
          REQUIRE(compact.getHeight() == after.getHeight());
          logToggle->click();
          pump();
          ViewRect restored;
          REQUIRE(view->getSize(&restored) == kResultOk);
          REQUIRE(frame.calls == 2 && !log->isHidden());
          REQUIRE(restored.getWidth() == after.getWidth());
          REQUIRE(restored.getHeight() == after.getHeight());
          frame.reject = true;
          logToggle->click();
          REQUIRE(view->getSize(&restored) == kResultOk);
          REQUIRE(log->isHidden() && restored.getWidth() == after.getWidth());
          logToggle->click();
          REQUIRE(!log->isHidden());
          REQUIRE(view->setFrame(nullptr) == kResultOk);
          REQUIRE(exercisePanel(widget) == 0);
          auto *toggle = widget->findChild<QPushButton *>("languageToggle");
          auto *start = widget->findChild<QPushButton *>("start");
          auto *pause = widget->findChild<QPushButton *>("pauseDisplay");
          REQUIRE(toggle && start && pause && !pause->isChecked());
          REQUIRE(start->text() ==
                  (reopen == 0 ? QString("Start") : QString("开始")));
          const auto dirty = handler.dirtyCalls;
          pause->click();
          REQUIRE(pause->isChecked());
          REQUIRE(handler.dirtyCalls == dirty && handler.parameterCalls == 0);
          toggle->click();
          REQUIRE(handler.dirtyCalls == dirty + 1 &&
                  handler.parameterCalls == 0);
          REQUIRE(start->text() ==
                  (reopen == 0 ? QString("开始") : QString("Start")));
          REQUIRE(pause->text() ==
                  (reopen == 0 ? QString("恢复显示") : QString("Resume Display")));
        }
        pump();
        if (widget && cycle == 0)
          REQUIRE(widget->grab().save(
              QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath() +
              "/VstEmbedded-" + argv[2] +
              (reopen == 0 ? "-zh.png" : "-en.png")));
        scale = nullptr;
        if (widget && reopen == 0) {
          auto *splitter = widget->findChild<QSplitter *>("streamingColumns");
          REQUIRE(splitter);
          auto parts = splitter->sizes();
          splitter->setSizes({parts[0] + 35, parts[1] - 35});
          pump();
          savedParts = splitter->sizes();
          widget->findChild<QPushButton *>("toggleLog")->click();
          pump();
        }
        REQUIRE(view->removed() == kResultOk);
        REQUIRE([[window contentView] subviews].count == 0);
        if (widget && reopen == 0) {
          view = owned(controller->createView(ViewType::kEditor));
          REQUIRE(view);
          ViewRect restored;
          REQUIRE(view->getSize(&restored) == kResultOk);
          REQUIRE(restored.getWidth() == 900 && restored.getHeight() == 880);
        }
      }
      view = nullptr;
      [window close];
      [window release];
      pc->disconnect(cc);
      cc->disconnect(pc);
      pc = nullptr;
      cc = nullptr;
      if (cycle % 2) {
        controller->terminate();
        controller = nullptr;
        component->terminate();
        component = nullptr;
      } else {
        component->terminate();
        component = nullptr;
        controller->terminate();
        controller = nullptr;
      }
      REQUIRE(QCoreApplication::instance() == application.get());
      REQUIRE(QCoreApplication::testAttribute(Qt::AA_PluginApplication) ==
              originalPlugin);
      REQUIRE(QCoreApplication::testAttribute(Qt::AA_DontUseNativeMenuBar) ==
              originalMenu);
      REQUIRE([NSApp delegate] == originalDelegate &&
              [NSApp mainMenu] == originalMainMenu);
      REQUIRE(QCoreApplication::libraryPaths() == originalPaths);
      factory = nullptr;
      REQUIRE(exit());
      REQUIRE(dlclose(module) == 0);
      CFRelease(bundle);
      void *remaining = dlopen(path, RTLD_NOLOAD | RTLD_NOW);
      if (remaining)
        dlclose(remaining);
      REQUIRE(!remaining);
      pump();
      REQUIRE(businessTypesUnregistered());
    }
    std::puts("5 macOS load/reopen/resize/cancel/unload cycles passed");
  }
}
