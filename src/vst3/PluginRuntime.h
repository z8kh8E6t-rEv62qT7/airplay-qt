#pragma once
#include "NativeRuntime.h"
#include "PluginState.h"
#include "ui/StreamingPanel.h"
#include <QApplication>
#include <map>

namespace vst3 {
// Created lazily by IPlugView::attached on the native UI thread.
class PluginRuntime {
public:
  static PluginRuntime &acquire(void *parent,
                                airplay::SessionEnvironment environment = {});
  static void addComponent() noexcept;
  static void removeComponent() noexcept;
  static void processorRemoved(uint64_t id) noexcept;
  static void shutdown() noexcept;
  static bool exists() noexcept;
  static bool prepareUnload() noexcept;
  ui::StreamingPanel *
  open(const std::shared_ptr<PluginState> &,
       const airplay::DiscoveryApi & = airplay::defaultDiscoveryApi());
  void close(uint64_t id);
  void pump();

private:
  struct Instance;
  explicit PluginRuntime(airplay::SessionEnvironment);
  ~PluginRuntime();
  void retire(uint64_t);
  struct ApplicationMode;
  std::unique_ptr<ApplicationMode> applicationMode_;
  std::unique_ptr<QApplication> application_;
  std::unique_ptr<NativeRuntime> native_;
  bool pumping_ = false, shutdownPending_ = false, shuttingDown_ = false;
  uint64_t sender_ = 0;
  int argc_ = 1;
  char name_[16] = "AirPlayQtVst3";
  QByteArray platformPath_;
  char platformOption_[21] = "-platformpluginpath";
  char *argv_[4] = {name_, nullptr, nullptr, nullptr};
  std::map<uint64_t, std::unique_ptr<Instance>> instances_;
  airplay::SessionEnvironment environment_;
};
} // namespace vst3
