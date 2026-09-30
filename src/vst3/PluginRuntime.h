#pragma once
#include "PluginState.h"
#include "ui/StreamingPanel.h"
#include <QApplication>
#include <map>
#include <windows.h>

namespace vst3 {
// Created lazily by IPlugView::attached on the HWND's owning thread.
class PluginRuntime {
public:
  static PluginRuntime &acquire(HWND parent);
  static void addComponent() noexcept;
  static void removeComponent() noexcept;
  static void processorRemoved(uint64_t id) noexcept;
  static void shutdown() noexcept;
  static bool exists() noexcept;
  static bool prepareUnload() noexcept;
  ui::StreamingPanel *open(const std::shared_ptr<PluginState> &,
                           airplay::DiscoveryApi = {});
  void close(uint64_t id);
  void pump();

private:
  struct Instance;
  PluginRuntime();
  ~PluginRuntime();
  void retire(uint64_t);
  static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
  std::unique_ptr<QApplication> application_;
  HWND dispatcher_ = nullptr;
  HMODULE module_ = nullptr;
  DWORD threadId_ = 0;
  bool pumping_ = false, shutdownPending_ = false, shuttingDown_ = false;
  uint64_t sender_ = 0;
  int argc_ = 1;
  char name_[16] = "AirPlayQtVst3";
  QByteArray platformPath_;
  char platformOption_[21] = "-platformpluginpath";
  char *argv_[4] = {name_, nullptr, nullptr, nullptr};
  std::map<uint64_t, std::unique_ptr<Instance>> instances_;
};
} // namespace vst3
