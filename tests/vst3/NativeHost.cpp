// Deliberately no Qt headers or link dependency: exercises a cold DAW load.
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

#include <psapi.h>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;
static LONG CALLBACK exceptionTrace(EXCEPTION_POINTERS *exception) {
  if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  std::fprintf(stderr, "Access violation at %p\n", exception->ExceptionRecord->ExceptionAddress);
  void *stack[48]{};
  const auto count = CaptureStackBackTrace(0, 48, stack, nullptr);
  for (unsigned i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    wchar_t name[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(stack[i]), &module);
    if (module) GetModuleFileNameW(module, name, 32768);
    std::fwprintf(stderr, L"%p %ls + %llx\n", stack[i], name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stack[i]) -
                                        reinterpret_cast<uintptr_t>(module)));
  }
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "Failed line %d: %s (Win32 %lu)\n", __LINE__, #x,   \
                   GetLastError());                                            \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static BOOL CALLBACK countChild(HWND, LPARAM context) {
  ++*reinterpret_cast<int *>(context);
  return TRUE;
}
int wmain(int argc, wchar_t **argv) {
  AddVectoredExceptionHandler(1, exceptionTrace);
  setvbuf(stdout, nullptr, _IONBF, 0);
  CHECK(argc >= 3);
  const std::filesystem::path path = argv[1];
  const std::wstring mode = argv[2];
  SetEnvironmentVariableW(L"QT_PLUGIN_PATH", nullptr);
  SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", nullptr);
  SetEnvironmentVariableW(L"QT_QPA_PLATFORM", nullptr);
  SetEnvironmentVariableW(L"QML2_IMPORT_PATH", nullptr);
  wchar_t system[32768]{};
  CHECK(GetSystemDirectoryW(system, 32768));
  CHECK(SetEnvironmentVariableW(L"PATH", system));
  CHECK(SetCurrentDirectoryW(system));
  CHECK(!GetModuleHandleW(L"Qt6Core.dll"));
  HMODULE preload = argc > 3 ? LoadLibraryExW(argv[3], nullptr,
                                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                  LOAD_LIBRARY_SEARCH_SYSTEM32)
                             : nullptr;
  CHECK(argc <= 3 || preload);
  HostApplication host;
  std::vector<void *> oldEngineAddresses;
  const auto enginePath =
      path.parent_path() / L"runtime" / L"AirPlayQtEngine.dll";
  for (int cycle = 0; cycle < 5; ++cycle) {
    auto module = LoadLibraryW(path.c_str());
    CHECK(module);
    auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    auto done = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
    auto getFactory = reinterpret_cast<IPluginFactory *(*)()>(
        GetProcAddress(module, "GetPluginFactory"));
    auto error = reinterpret_cast<unsigned (*)(wchar_t *, unsigned)>(
        GetProcAddress(module, "AirPlayQtLoaderError"));
    CHECK(init && done && getFactory && error);
    CHECK(!getFactory());
    const bool initialized = init();
    if (!initialized) {
      wchar_t diagnostic[2048]{};
      error(diagnostic, 2048);
      std::fwprintf(stderr, L"%ls\n", diagnostic);
    }
    if (mode == L"failure") {
      CHECK(!initialized);
      CHECK(!init());
      CHECK(!getFactory());
      CHECK(!GetModuleHandleW(enginePath.c_str()));
      CHECK(FreeLibrary(module));
      break;
    }
    CHECK(initialized);
    CHECK(init());
    CHECK(done());
    auto factory = owned(getFactory());
    CHECK(factory && factory->countClasses() == 2);
    CHECK(GetModuleHandleW(enginePath.c_str()));
    MODULEINFO engineInfo{};
    CHECK(GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(enginePath.c_str()),
                               &engineInfo, sizeof(engineInfo)));
    std::ifstream manifest(path.parent_path() / L"runtime/runtime-sha256.txt");
    std::string entry;
    while (std::getline(manifest, entry)) {
      const auto expected = path.parent_path() / L"runtime" / entry.substr(66);
      auto loaded = GetModuleHandleW(expected.filename().c_str());
      if (!loaded)
        continue; // qwindows is loaded only on opening the editor.
      wchar_t actual[32768]{};
      CHECK(GetModuleFileNameW(loaded, actual, 32768));
      std::fwprintf(stdout, L"MODULE %ls\n", actual);
      const std::wstring actualPath(actual);
      const bool fromSystem = _wcsnicmp(actual, system, wcslen(system)) == 0 &&
                              actualPath.size() > wcslen(system) &&
                              actualPath[wcslen(system)] == L'\\';
      const bool allowedCopy = preload && loaded == preload;
      CHECK(fromSystem || allowedCopy ||
            std::filesystem::equivalent(actual, expected));
    }
    PClassInfo processorInfo{}, controllerInfo{};
    CHECK(factory->getClassInfo(0, &processorInfo) == kResultOk);
    CHECK(factory->getClassInfo(1, &controllerInfo) == kResultOk);
    IComponent *rawComponent = nullptr;
    IEditController *rawController = nullptr;
    CHECK(factory->createInstance(processorInfo.cid, IComponent::iid,
                                  reinterpret_cast<void **>(&rawComponent)) ==
          kResultOk);
    CHECK(factory->createInstance(controllerInfo.cid, IEditController::iid,
                                  reinterpret_cast<void **>(&rawController)) ==
          kResultOk);
    auto component = owned(rawComponent);
    auto controller = owned(rawController);
    CHECK(component->initialize(&host) == kResultOk);
    CHECK(controller->initialize(&host) == kResultOk);
    CHECK(!done()); // A live component must keep engine code mapped.
    FUnknownPtr<IConnectionPoint> pc(component), cc(controller);
    CHECK(pc->connect(cc) == kResultOk);
    CHECK(cc->connect(pc) == kResultOk);
    if (cycle == 0)
      CHECK(!GetModuleHandleW(L"qwindows.dll"));
    for (int reopen = 0; reopen < 2; ++reopen) {
      auto view = owned(controller->createView(ViewType::kEditor));
      CHECK(view);
      auto window = CreateWindowExW(
          0, L"STATIC", L"Native VST3 test", WS_OVERLAPPED, 0, 0, 1000, 900,
          nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      CHECK(window);
      CHECK(view->attached(window, kPlatformTypeHWND) == kResultOk);
      int children = 0;
      EnumChildWindows(window, countChild, reinterpret_cast<LPARAM>(&children));
      CHECK(children > 0);
      auto platform = GetModuleHandleW(L"qwindows.dll");
      CHECK(platform);
      wchar_t loadedPath[32768]{};
      CHECK(GetModuleFileNameW(platform, loadedPath, 32768));
      std::fwprintf(stdout, L"PLATFORM %ls\n", loadedPath);
      CHECK(std::filesystem::equivalent(
          loadedPath, path.parent_path() / L"runtime/platforms/qwindows.dll"));
      CHECK(view->removed() == kResultOk);
      view = nullptr;
      CHECK(DestroyWindow(window));
      MSG message{};
      const auto deadline = GetTickCount64() + 150;
      while (GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        Sleep(1);
      }
    }
    cc->disconnect(pc);
    pc->disconnect(cc);
    cc = nullptr;
    pc = nullptr;
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
    CHECK(!done()); // Factory ownership is also an outstanding SDK reference.
    factory = nullptr;
    CHECK(done());
    CHECK(!done());
    CHECK(!GetModuleHandleW(enginePath.c_str()));
    if (mode == L"relocate") {
      // Force a different image base on the next load: stale callbacks and Qt
      // registrations must not accidentally work because addresses are reused.
      auto reservation = VirtualAlloc(engineInfo.lpBaseOfDll, engineInfo.SizeOfImage,
                                     MEM_RESERVE, PAGE_NOACCESS);
      CHECK(reservation == engineInfo.lpBaseOfDll);
      oldEngineAddresses.push_back(reservation);
    }
    CHECK(FreeLibrary(module));
    CHECK(!GetModuleHandleW(path.c_str()));
  }
  if (preload)
    CHECK(FreeLibrary(preload));
  for (auto address : oldEngineAddresses) CHECK(VirtualFree(address, 0, MEM_RELEASE));
  std::puts("Native cold-load checks passed; no audio activated");
  return 0;
}
