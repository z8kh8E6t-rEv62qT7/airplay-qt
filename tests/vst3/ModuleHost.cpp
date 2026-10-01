#include "TestComponentHandler.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "vst3/Plugin.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QPushButton>
#include <QWidget>
#include <cstdio>
#include <windows.h>

using namespace Steinberg;
using namespace Steinberg::Vst;
static BOOL CALLBACK findQtChild(HWND window, LPARAM context) {
  if (auto *widget = QWidget::find(reinterpret_cast<WId>(window))) {
    *reinterpret_cast<QWidget **>(context) = widget;
    return FALSE;
  }
  return TRUE;
}
// A hidden native host exercises the actual DLL, independently of the test
// executable's Qt lifetime. Never activates audio or starts an AirPlay session.
int main(int argc, char **argv) {
  if (argc < 2)
    return 1;
  // Resolve the host's output directory before the DLL creates its QApplication
  // with synthetic argv; that application's applicationDirPath may be empty.
  const auto outputDirectory =
      QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
  std::unique_ptr<QCoreApplication> existing;
  const bool incompatible =
      argc > 2 && std::strcmp(argv[2], "incompatible") == 0;
  const bool borrowed = argc > 2 && std::strcmp(argv[2], "borrowed") == 0;
  if (incompatible)
    existing = std::make_unique<QCoreApplication>(argc, argv);
  else if (borrowed)
    existing = std::make_unique<QApplication>(argc, argv);
  HostApplication host;
  TestComponentHandler handler;
  const auto originalLibraryPaths = QCoreApplication::libraryPaths();
  for (int cycle = 0; cycle < 5; ++cycle) {
    const auto path = QDir::toNativeSeparators(QString::fromLocal8Bit(argv[1]))
                          .toStdWString();
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module) {
      std::fprintf(stderr, "LoadLibrary: %lu\n", GetLastError());
      return 2;
    }
    auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    auto exit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
    auto factoryFunction = reinterpret_cast<IPluginFactory *(PLUGIN_API *)()>(
        GetProcAddress(module, "GetPluginFactory"));
    if (!init || !exit || !factoryFunction || !init())
      return 3;
    auto factory = owned(factoryFunction());
    IComponent *rawComponent = nullptr;
    IEditController *rawController = nullptr;
    if (factory->createInstance(vst3::processorId, IComponent::iid,
                                reinterpret_cast<void **>(&rawComponent)) !=
            kResultOk ||
        factory->createInstance(vst3::controllerId, IEditController::iid,
                                reinterpret_cast<void **>(&rawController)) !=
            kResultOk)
      return 4;
    auto component = owned(rawComponent);
    auto controller = owned(rawController);
    if (component->initialize(&host) != kResultOk ||
        controller->initialize(&host) != kResultOk)
      return 5;
    if (controller->setComponentHandler(&handler) != kResultOk)
      return 20;
    FUnknownPtr<IConnectionPoint> processorConnection(component);
    FUnknownPtr<IConnectionPoint> controllerConnection(controller);
    processorConnection->connect(controllerConnection);
    controllerConnection->connect(processorConnection);
    // Scanning and creating an editor object alone must not construct Qt.
    auto view = owned(controller->createView(ViewType::kEditor));
    if (!view || QCoreApplication::instance() != existing.get())
      return 6;
    HWND window = CreateWindowExW(0, L"STATIC", L"AirPlayQt test host",
                                  WS_OVERLAPPED, 0, 0, 1000, 1000, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window || view->attached(window, kPlatformTypeHWND) != kResultOk)
      return 7;
    if (!incompatible &&
        !qobject_cast<QApplication *>(QCoreApplication::instance()))
      return 8;
    QWidget *embedded = nullptr;
    EnumChildWindows(window, findQtChild, reinterpret_cast<LPARAM>(&embedded));
    if (incompatible ? embedded != nullptr : embedded == nullptr)
      return 11;
    if (embedded) {
      auto *toggle = embedded->findChild<QPushButton *>("languageToggle");
      auto *start = embedded->findChild<QPushButton *>("start");
      if (!toggle || !start || start->text() != "Start")
        return 21;
      const auto dirty = handler.dirtyCalls;
      toggle->click();
      if (handler.dirtyCalls != dirty + 1 || handler.parameterCalls ||
          start->text() != "开始")
        return 22;
    }
    ViewRect size{0, 0, 900, 880};
    view->onSize(&size);
    view->onFocus(true);
    FUnknownPtr<IPlugViewContentScaleSupport> scale(view);
    if (!scale || scale->setContentScaleFactor(1.25f) != kResultTrue)
      return 12;
    view->getSize(&size);
    view->onSize(&size);
    if (embedded && cycle == 0) {
      QImageWriter writer(outputDirectory + "/VstEmbedded-" +
                          (borrowed ? "borrowed" : "owned") + ".png");
      if (!writer.write(embedded->grab().toImage())) {
        std::fprintf(stderr, "Screenshot %s: %s\n",
                     qPrintable(writer.fileName()),
                     qPrintable(writer.errorString()));
        return 13;
      }
    }
    scale = nullptr;
    // Process a bounded set of native messages, then cancel discovery during
    // view removal. Component release must drain native callbacks before DLL
    // unload.
    MSG message{};
    for (int n = 0; n < 50 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
         ++n) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    view->removed();
    view = nullptr;
    DestroyWindow(window);
    controllerConnection->disconnect(processorConnection);
    processorConnection->disconnect(controllerConnection);
    controllerConnection = nullptr;
    processorConnection = nullptr;
    if (cycle % 2 == 0) {
      component->terminate();
      component = nullptr;
      controller->terminate();
      controller = nullptr;
    } else {
      controller->terminate();
      controller = nullptr;
      component->terminate();
      component = nullptr;
    }
    if (QCoreApplication::instance() != existing.get())
      return 9;
    if (borrowed && QCoreApplication::libraryPaths() != originalLibraryPaths)
      return 16;
    factory = nullptr;
    if (!exit() || !FreeLibrary(module))
      return 10;
    if (GetModuleHandleW(path.c_str()))
      return 14;
    const auto enginePath =
        QDir::toNativeSeparators(
            QFileInfo(QString::fromStdWString(path)).absolutePath() +
            "/runtime/AirPlayQtEngine.dll")
            .toStdWString();
    if (GetModuleHandleW(enginePath.c_str()))
      return 15;
  }
  std::puts("5 DLL load/editor/cancel/unload cycles passed");
  return 0;
}
